/*
 * BRSTM / BFSTM / BCSTM muxer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Nintendo's streamed-audio containers, one per console generation and all
 * the same idea: a header describing the stream, a seek table caching the
 * decoder state at every block boundary, and the audio cut into fixed-size
 * blocks that interleave the channels.
 *
 *   BRSTM ("RSTM")  Wii        chunked, HEAD/ADPC/DATA
 *   BFSTM ("FSTM")  Wii U      section table, INFO/SEEK/DATA
 *   BCSTM ("CSTM")  3DS        as BFSTM, little-endian by default
 *
 * The differences that matter are the section layout and the byte order; the
 * ADPCM payload itself is endian-neutral, so the same encoder output serves
 * all three. What the block structure costs us is that the file cannot be
 * written until the stream is over: the block count, the last block's odd
 * size and the whole seek table are only known at the end. Everything is
 * therefore buffered per channel and emitted from write_trailer(), which has
 * the side benefit of not needing seekable output.
 */

#include "config_components.h"

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

enum BRSTMVariant {
    BRSTM_RSTM,
    BRSTM_FSTM,
    BRSTM_CSTM,
};

/* Sections are padded out to this; Nintendo's own files all are, and a
 * player that memcpy()s a section into aligned scratch depends on it. */
#define BRSTM_ALIGN 0x20

typedef struct BRSTMMuxContext {
    const AVClass *class;

    int      variant;
    int      block_size;     /* bytes per channel per block */
    int      little_endian;  /* -1: the variant's default */
    int      loop;
    int64_t  loop_start;     /* samples */

    AVIOContext *ch_buf[FF_DSP_ADPCM_MAX_CHANNELS];
    int16_t      coefs[FF_DSP_ADPCM_MAX_CHANNELS][16];
    int          have_coefs;
    int64_t      nb_samples;
    int          is_adpcm;
    int          bytes_per_sample;   /* PCM only */
} BRSTMMuxContext;

static void wr16(AVFormatContext *s, unsigned v)
{
    BRSTMMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl16(s->pb, v);
    else
        avio_wb16(s->pb, v);
}

static void wr32(AVFormatContext *s, unsigned v)
{
    BRSTMMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl32(s->pb, v);
    else
        avio_wb32(s->pb, v);
}

/* Section identifiers ("HEAD", "SEEK", ...) are byte strings, not integers,
 * so they are written in file order regardless of the stream's byte order. */
static void wr_tag(AVFormatContext *s, const char *tag)
{
    avio_write(s->pb, tag, 4);
}

/* Pad with zeroes up to an absolute file offset. Every offset in these
 * formats is computed up front and then has to be hit exactly, so writing
 * the padding from the position we are actually at -- rather than from a
 * hand-counted field size -- is what keeps the two in agreement. */
static int pad_to(AVFormatContext *s, int64_t target)
{
    int64_t pos = avio_tell(s->pb);

    if (pos > target) {
        av_log(s, AV_LOG_ERROR, "internal error: wrote past offset %"PRId64
               " (at %"PRId64")\n", target, pos);
        return AVERROR_BUG;
    }
    ffio_fill(s->pb, 0, target - pos);
    return 0;
}

static int64_t bytes_to_samples(const BRSTMMuxContext *c, int64_t bytes)
{
    return c->is_adpcm ? bytes / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME
                       : bytes / c->bytes_per_sample;
}

static int brstm_common_init(AVFormatContext *s)
{
    BRSTMMuxContext *c = s->priv_data;
    AVCodecParameters *par;
    int channels;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "this format carries exactly one stream\n");
        return AVERROR(EINVAL);
    }
    par = s->streams[0]->codecpar;
    channels = par->ch_layout.nb_channels;

    switch (par->codec_id) {
    case AV_CODEC_ID_ADPCM_THP:
        c->is_adpcm = 1;
        break;
    case AV_CODEC_ID_PCM_S8_PLANAR:
        c->bytes_per_sample = 1;
        break;
    case AV_CODEC_ID_PCM_S16BE_PLANAR:
    case AV_CODEC_ID_PCM_S16LE_PLANAR:
        c->bytes_per_sample = 2;
        break;
    default:
        av_log(s, AV_LOG_ERROR,
               "unsupported codec; use adpcm_thp (what these formats are for), "
               "or pcm_s16be_planar / pcm_s8_planar for an uncompressed stream\n");
        return AVERROR(EINVAL);
    }

    if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "1 to %d channels are supported\n",
               FF_DSP_ADPCM_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    if (c->little_endian < 0)
        c->little_endian = c->variant == BRSTM_CSTM;

    /* A block holds a whole number of ADPCM frames, or of PCM samples. */
    if (c->is_adpcm && c->block_size % FF_DSP_ADPCM_BYTES_PER_FRAME) {
        av_log(s, AV_LOG_ERROR, "block_size must be a multiple of %d for ADPCM\n",
               FF_DSP_ADPCM_BYTES_PER_FRAME);
        return AVERROR(EINVAL);
    }
    if (!c->is_adpcm && c->block_size % c->bytes_per_sample) {
        av_log(s, AV_LOG_ERROR, "block_size must be a multiple of %d for this PCM format\n",
               c->bytes_per_sample);
        return AVERROR(EINVAL);
    }

    for (int ch = 0; ch < channels; ch++) {
        int ret = avio_open_dyn_buf(&c->ch_buf[ch]);
        if (ret < 0)
            return ret;
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, par->sample_rate);
    return 0;
}

static void brstm_deinit(AVFormatContext *s)
{
    BRSTMMuxContext *c = s->priv_data;

    for (int ch = 0; ch < FF_DSP_ADPCM_MAX_CHANNELS; ch++)
        ffio_free_dyn_buf(&c->ch_buf[ch]);
}

static int brstm_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    BRSTMMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    size_t side_size;
    const uint8_t *side;
    int per_ch;

    /* adpcm_thp derives its predictors over the whole stream and hands them
     * over as replacement extradata once, at drain. */
    side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    if (!side) {
        side      = s->streams[0]->codecpar->extradata;
        side_size = s->streams[0]->codecpar->extradata_size;
    }
    if (side && side_size >= 32 * (size_t)channels) {
        for (int ch = 0; ch < channels; ch++)
            for (int i = 0; i < 16; i++)
                c->coefs[ch][i] = (int16_t)AV_RB16(side + ch * 32 + i * 2);
        c->have_coefs = 1;
    }

    if (pkt->size % channels) {
        av_log(s, AV_LOG_ERROR, "packet of %d bytes does not split evenly "
               "across %d channels\n", pkt->size, channels);
        return AVERROR_INVALIDDATA;
    }
    per_ch = pkt->size / channels;

    /* Both adpcm_thp and the planar PCM encoders lay a packet out channel by
     * channel, so the split is a straight slice. */
    for (int ch = 0; ch < channels; ch++)
        avio_write(c->ch_buf[ch], pkt->data + (size_t)ch * per_ch, per_ch);

    if (pkt->duration > 0)
        c->nb_samples += pkt->duration;
    else
        c->nb_samples += bytes_to_samples(c, per_ch);

    return 0;
}

/* Per-block decoder state, in the order the demuxers expect: for every block,
 * every channel's two previous samples as of that block's first sample.
 * Block 0's entry is therefore zero. The encoder cannot supply this, because
 * where the block boundaries fall is decided here. */
static int brstm_build_seek_table(AVFormatContext *s, uint8_t *const *data,
                                  const int *size, int64_t block_count,
                                  int16_t *table)
{
    BRSTMMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    int64_t frames_per_block = c->block_size / FF_DSP_ADPCM_BYTES_PER_FRAME;

    for (int ch = 0; ch < channels; ch++) {
        int16_t h1 = 0, h2 = 0;

        for (int64_t b = 0; b < block_count; b++) {
            int64_t off    = b * c->block_size;
            int64_t frames = FFMIN(frames_per_block,
                                   (size[ch] - off) / FF_DSP_ADPCM_BYTES_PER_FRAME);

            table[(b * channels + ch) * 2 + 0] = h1;
            table[(b * channels + ch) * 2 + 1] = h2;

            if (frames > 0)
                ff_dsp_adpcm_advance(data[ch] + off, frames, c->coefs[ch],
                                     &h1, &h2);
        }
    }
    return 0;
}

/* Every channel's block, back to back, then the next block. */
static void brstm_write_audio(AVFormatContext *s, uint8_t *const *data,
                              const int *size, int64_t block_count,
                              int last_block_size)
{
    BRSTMMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;

    for (int64_t b = 0; b < block_count; b++) {
        int64_t off  = b * c->block_size;
        int     span = b == block_count - 1 ? last_block_size : c->block_size;

        for (int ch = 0; ch < channels; ch++) {
            int n = FFMAX(FFMIN((int64_t)c->block_size, size[ch] - off), 0);

            if (n)
                avio_write(s->pb, data[ch] + off, n);
            if (span > n)
                ffio_fill(s->pb, 0, span - n);
        }
    }
}

static void brstm_write_coefs(AVFormatContext *s, int ch)
{
    BRSTMMuxContext *c = s->priv_data;

    for (int i = 0; i < 16; i++)
        wr16(s, c->coefs[ch][i]);
}

static int brstm_write_rstm(AVFormatContext *s, uint8_t *const *data,
                            const int *size, int64_t block_count,
                            int last_block_used, int last_block_size,
                            int64_t last_block_samples, const int16_t *table)
{
    BRSTMMuxContext *c = s->priv_data;
    AVCodecParameters *par = s->streams[0]->codecpar;
    int channels = par->ch_layout.nb_channels;
    int samples_per_block = bytes_to_samples(c, c->block_size);
    /* HEAD body offsets, all relative to the first byte after the chunk's
     * 8-byte tag+size -- that is the base the format uses throughout. */
    int h1rel  = 0x18;
    int h2rel  = h1rel + 0x34;
    int h3rel  = h2rel + 0x20;
    int ci0    = h3rel + 4 + 8 * channels;
    int head_body = ci0 + 0x38 * channels;
    int head_size = FFALIGN(8 + head_body, BRSTM_ALIGN);
    int adpc_data = c->is_adpcm ? block_count * channels * 4 : 0;
    int adpc_size = c->is_adpcm ? FFALIGN(8 + adpc_data, BRSTM_ALIGN) : 0;
    int64_t audio  = (block_count - 1) * (int64_t)c->block_size * channels +
                     (int64_t)last_block_size * channels;
    int64_t data_off = 0x40 + head_size + adpc_size;
    int64_t data_size = FFALIGN(0x20 + audio, BRSTM_ALIGN);
    int64_t head_body_base = 0x40 + 8;
    int ret;

    wr_tag(s, "RSTM");
    wr16(s, 0xFEFF);
    avio_w8(s->pb, 1);                          /* major */
    avio_w8(s->pb, 0);                          /* minor */
    wr32(s, data_off + data_size);              /* file size */
    wr16(s, 0x40);                              /* header size */
    wr16(s, c->is_adpcm ? 3 : 2);               /* chunk count */
    wr32(s, 0x40);
    wr32(s, head_size);
    wr32(s, c->is_adpcm ? 0x40 + head_size : 0);
    wr32(s, adpc_size);
    wr32(s, data_off);
    wr32(s, data_size);
    if ((ret = pad_to(s, 0x40)) < 0)
        return ret;

    /* HEAD */
    wr_tag(s, "HEAD");
    wr32(s, head_size);
    wr32(s, 0x01000000); wr32(s, h1rel);
    wr32(s, 0x01000000); wr32(s, h2rel);
    wr32(s, 0x01000000); wr32(s, h3rel);

    /* HEAD1: the stream description. */
    if ((ret = pad_to(s, head_body_base + h1rel)) < 0)
        return ret;
    avio_w8(s->pb, c->is_adpcm ? 2 : c->bytes_per_sample == 2 ? 1 : 0);
    avio_w8(s->pb, c->loop);
    avio_w8(s->pb, channels);
    avio_w8(s->pb, 0);
    wr16(s, par->sample_rate);
    wr16(s, 0);
    wr32(s, c->loop ? c->loop_start : 0);
    wr32(s, c->nb_samples);
    wr32(s, data_off + 0x20);
    wr32(s, block_count);
    wr32(s, c->block_size);
    wr32(s, samples_per_block);
    wr32(s, last_block_used);
    wr32(s, last_block_samples);
    wr32(s, last_block_size);
    wr32(s, samples_per_block);                 /* samples per seek entry */
    wr32(s, 4);                                 /* bytes per seek entry */

    /* HEAD2: one track covering the whole stream. Nothing in ffmpeg reads
     * this, but players expect a track to exist. */
    if ((ret = pad_to(s, head_body_base + h2rel)) < 0)
        return ret;
    avio_w8(s->pb, 1);                          /* track count */
    avio_w8(s->pb, 1);                          /* track descriptor type */
    wr16(s, 0);
    wr32(s, 0x01010000); wr32(s, h2rel + 12);
    avio_w8(s->pb, 0x7F);                       /* volume */
    avio_w8(s->pb, 0x40);                       /* pan */
    wr16(s, 0);
    wr32(s, 0);
    avio_w8(s->pb, channels);
    avio_w8(s->pb, 0);
    avio_w8(s->pb, channels > 1 ? 1 : 0);
    avio_w8(s->pb, 0);

    /* HEAD3: per-channel coefficients, reached through two levels of offset. */
    if ((ret = pad_to(s, head_body_base + h3rel)) < 0)
        return ret;
    avio_w8(s->pb, channels);
    avio_w8(s->pb, 0);
    wr16(s, 0);
    for (int ch = 0; ch < channels; ch++) {
        wr32(s, 0x01000000);
        wr32(s, ci0 + 0x38 * ch);
    }
    for (int ch = 0; ch < channels; ch++) {
        int ps = size[ch] ? data[ch][0] : 0;

        if ((ret = pad_to(s, head_body_base + ci0 + 0x38 * ch)) < 0)
            return ret;
        wr32(s, 0x01000000);
        wr32(s, ci0 + 0x38 * ch + 8);
        brstm_write_coefs(s, ch);
        wr16(s, 0);                             /* gain */
        wr16(s, ps);                            /* initial predictor/scale */
        wr16(s, 0);                             /* initial hist1 */
        wr16(s, 0);                             /* initial hist2 */
        wr16(s, 0);                             /* loop predictor/scale */
        wr16(s, 0);                             /* loop hist1 */
        wr16(s, 0);                             /* loop hist2 */
        wr16(s, 0);
    }
    if ((ret = pad_to(s, 0x40 + head_size)) < 0)
        return ret;

    /* ADPC: the seek table, big-endian like the rest of a BRSTM. */
    if (c->is_adpcm) {
        wr_tag(s, "ADPC");
        wr32(s, adpc_size);
        for (int i = 0; i < adpc_data / 2; i++)
            avio_wb16(s->pb, table[i]);
        if ((ret = pad_to(s, data_off)) < 0)
            return ret;
    }

    wr_tag(s, "DATA");
    wr32(s, data_size);
    wr32(s, 0x18);                              /* audio starts 0x18 further on */
    if ((ret = pad_to(s, data_off + 0x20)) < 0)
        return ret;
    brstm_write_audio(s, data, size, block_count, last_block_size);
    return pad_to(s, data_off + data_size);
}

static int brstm_write_fstm(AVFormatContext *s, uint8_t *const *data,
                            const int *size, int64_t block_count,
                            int last_block_used, int last_block_size,
                            int64_t last_block_samples, const int16_t *table)
{
    BRSTMMuxContext *c = s->priv_data;
    AVCodecParameters *par = s->streams[0]->codecpar;
    int channels = par->ch_layout.nb_channels;
    int samples_per_block = bytes_to_samples(c, c->block_size);
    /* INFO body offsets, relative to the first byte after the tag+size. */
    int h1rel  = 0x18;
    int h3rel  = h1rel + 0x40;
    int ci0    = 4 + 8 * channels;              /* relative to the section base */
    int ai0    = 4 + 16 * channels;
    int info_body = h3rel + ai0 + 46 * channels;
    int info_size = FFALIGN(8 + info_body, BRSTM_ALIGN);
    int seek_data = c->is_adpcm ? block_count * channels * 4 : 0;
    int seek_size = c->is_adpcm ? FFALIGN(8 + seek_data, BRSTM_ALIGN) : 0;
    int64_t audio = (block_count - 1) * (int64_t)c->block_size * channels +
                    (int64_t)last_block_size * channels;
    int64_t info_off = 0x40;
    int64_t seek_off = info_off + info_size;
    int64_t data_off = seek_off + seek_size;
    int64_t data_size = FFALIGN(0x20 + audio, BRSTM_ALIGN);
    int sections = c->is_adpcm ? 3 : 2;
    int64_t info_body_base = info_off + 8;
    int64_t ci_base = info_body_base + h3rel;
    int ret;

    wr_tag(s, c->variant == BRSTM_CSTM ? "CSTM" : "FSTM");
    wr16(s, 0xFEFF);
    wr16(s, 0x40);                              /* header size */
    /* Version. Our own demuxer ignores it; these are the values seen on the
     * consoles each variant belongs to. */
    wr32(s, c->variant == BRSTM_CSTM ? 0x00000200 : 0x00030000);
    wr32(s, data_off + data_size);              /* file size */
    wr16(s, sections);
    wr16(s, 0);
    wr16(s, 0x4000); wr16(s, 0); wr32(s, info_off); wr32(s, info_size);
    if (c->is_adpcm) {
        wr16(s, 0x4001); wr16(s, 0); wr32(s, seek_off); wr32(s, seek_size);
    }
    wr16(s, 0x4002); wr16(s, 0); wr32(s, data_off); wr32(s, data_size);
    if ((ret = pad_to(s, info_off)) < 0)
        return ret;

    /* INFO */
    wr_tag(s, "INFO");
    wr32(s, info_size);
    wr32(s, 0x41000000); wr32(s, h1rel);
    /* No track table: -1 is how these formats say a reference is absent. */
    wr32(s, 0x01010000); wr32(s, 0xFFFFFFFF);
    wr32(s, 0x01010000); wr32(s, h3rel);

    if ((ret = pad_to(s, info_body_base + h1rel)) < 0)
        return ret;
    avio_w8(s->pb, c->is_adpcm ? 2 : c->bytes_per_sample == 2 ? 1 : 0);
    avio_w8(s->pb, c->loop);
    avio_w8(s->pb, channels);
    avio_w8(s->pb, 0);                          /* region count */
    wr32(s, par->sample_rate);
    wr32(s, c->loop ? c->loop_start : 0);
    wr32(s, c->nb_samples);
    wr32(s, block_count);
    wr32(s, c->block_size);
    wr32(s, samples_per_block);
    wr32(s, last_block_used);
    wr32(s, last_block_samples);
    wr32(s, last_block_size);
    wr32(s, samples_per_block);                 /* samples per seek entry */
    wr32(s, 4);                                 /* bytes per seek entry */
    wr32(s, 0x1F000000);                        /* sample data reference */
    wr32(s, 0x18);

    /* Channel info: a table of references to references to the coefficients. */
    if ((ret = pad_to(s, ci_base)) < 0)
        return ret;
    wr32(s, channels);
    for (int ch = 0; ch < channels; ch++) {
        wr32(s, 0x41020000);
        wr32(s, ci0 + 8 * ch);
    }
    for (int ch = 0; ch < channels; ch++) {
        wr32(s, c->is_adpcm ? 0x03000000 : 0xFFFFFFFF);
        wr32(s, c->is_adpcm ? (unsigned)(ai0 + 46 * ch) : 0xFFFFFFFF);
    }
    for (int ch = 0; ch < channels; ch++) {
        int ps = size[ch] ? data[ch][0] : 0;

        if ((ret = pad_to(s, ci_base + ai0 + 46 * ch)) < 0)
            return ret;
        brstm_write_coefs(s, ch);
        wr16(s, ps);                            /* initial predictor/scale */
        wr16(s, 0);                             /* initial hist1 */
        wr16(s, 0);                             /* initial hist2 */
        wr16(s, 0);                             /* loop predictor/scale */
        wr16(s, 0);                             /* loop hist1 */
        wr16(s, 0);                             /* loop hist2 */
        wr16(s, 0);
    }
    if ((ret = pad_to(s, info_off + info_size)) < 0)
        return ret;

    /* SEEK. Its entries are little-endian even in a big-endian file -- an
     * inconsistency in the format itself, which the demuxer also special
     * cases (see the byte-swapping read in brstm.c). */
    if (c->is_adpcm) {
        wr_tag(s, "SEEK");
        wr32(s, seek_size);
        for (int i = 0; i < seek_data / 2; i++)
            avio_wl16(s->pb, table[i]);
        if ((ret = pad_to(s, data_off)) < 0)
            return ret;
    }

    wr_tag(s, "DATA");
    wr32(s, data_size);
    if ((ret = pad_to(s, data_off + 0x20)) < 0)
        return ret;
    brstm_write_audio(s, data, size, block_count, last_block_size);
    return pad_to(s, data_off + data_size);
}

static int brstm_write_trailer(AVFormatContext *s)
{
    BRSTMMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    uint8_t *data[FF_DSP_ADPCM_MAX_CHANNELS] = { NULL };
    int size[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int16_t *table = NULL;
    int64_t block_count, last_block_samples;
    int last_block_used, last_block_size;
    int ret = 0;

    for (int ch = 0; ch < channels; ch++)
        size[ch] = avio_close_dyn_buf(c->ch_buf[ch], &data[ch]);
    memset(c->ch_buf, 0, sizeof(c->ch_buf));

    for (int ch = 0; ch < channels; ch++) {
        if (size[ch] < 0) {
            ret = size[ch];
            goto end;
        }
    }
    if (!size[0] || !c->nb_samples) {
        av_log(s, AV_LOG_ERROR, "no audio was written\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (c->is_adpcm && !c->have_coefs) {
        av_log(s, AV_LOG_ERROR, "no DSP-ADPCM coefficient table available; "
               "the input has to come from the adpcm_thp encoder or from a "
               "container that carries the table\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    block_count        = (size[0] + c->block_size - 1) / c->block_size;
    last_block_used    = size[0] - (block_count - 1) * c->block_size;
    last_block_samples = c->nb_samples - (block_count - 1) *
                         bytes_to_samples(c, c->block_size);
    /* The final block is padded so every block in the file is aligned; the
     * "used bytes" field is what bounds the audio. */
    last_block_size    = FFALIGN(last_block_used, BRSTM_ALIGN);

    if (c->is_adpcm) {
        table = av_calloc(block_count * channels * 2, sizeof(*table));
        if (!table) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        brstm_build_seek_table(s, data, size, block_count, table);
    }

    if (c->variant == BRSTM_RSTM)
        ret = brstm_write_rstm(s, data, size, block_count, last_block_used,
                               last_block_size, last_block_samples, table);
    else
        ret = brstm_write_fstm(s, data, size, block_count, last_block_used,
                               last_block_size, last_block_samples, table);

end:
    av_freep(&table);
    for (int ch = 0; ch < channels; ch++)
        av_free(data[ch]);
    return ret;
}

#define OFFSET(x) offsetof(BRSTMMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption brstm_options[] = {
    { "block_size", "bytes of one channel's audio per block", OFFSET(block_size),
      AV_OPT_TYPE_INT, { .i64 = 0x2000 }, 8, 1 << 20, E },
    { "endian", "byte order of the header fields", OFFSET(little_endian),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, E, .unit = "endian" },
    { "default", "the variant's usual byte order", 0, AV_OPT_TYPE_CONST,
      { .i64 = -1 }, 0, 0, E, .unit = "endian" },
    { "be", "big-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "endian" },
    { "le", "little-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, E, .unit = "endian" },
    { "loop", "mark the stream as looping", OFFSET(loop), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, E },
    { "loop_start", "loop start, in samples", OFFSET(loop_start),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, E },
    { NULL },
};

#define BRSTM_MUXER(name_, long_name_, ext_, variant_)                 \
static const AVClass name_ ## _muxer_class = {                              \
    .class_name = #name_ " muxer",                                          \
    .item_name  = av_default_item_name,                                     \
    .option     = brstm_options,                                            \
    .version    = LIBAVUTIL_VERSION_INT,                                    \
};                                                                          \
static int name_ ## _init(AVFormatContext *s)                               \
{                                                                           \
    ((BRSTMMuxContext *)s->priv_data)->variant = variant_;                  \
    return brstm_common_init(s);                                                   \
}                                                                           \
const FFOutputFormat ff_ ## name_ ## _muxer = {                             \
    .p.name           = #name_,                                             \
    .p.long_name      = NULL_IF_CONFIG_SMALL(long_name_),                   \
    .p.extensions     = ext_,                                               \
    .p.audio_codec    = AV_CODEC_ID_ADPCM_THP,                              \
    .p.video_codec    = AV_CODEC_ID_NONE,                                   \
    .p.subtitle_codec = AV_CODEC_ID_NONE,                                   \
    .p.flags          = AVFMT_NOTIMESTAMPS,                                 \
    .p.priv_class     = &name_ ## _muxer_class,                             \
    .priv_data_size   = sizeof(BRSTMMuxContext),                            \
    .init             = name_ ## _init,                                     \
    .deinit           = brstm_deinit,                                       \
    .write_packet     = brstm_write_packet,                                 \
    .write_trailer    = brstm_write_trailer,                                \
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH,                       \
}

#if CONFIG_BRSTM_MUXER
BRSTM_MUXER(brstm, "BRSTM (Binary Revolution Stream)", "brstm", BRSTM_RSTM);
#endif
#if CONFIG_BFSTM_MUXER
BRSTM_MUXER(bfstm, "BFSTM (Binary Cafe Stream)", "bfstm", BRSTM_FSTM);
#endif
#if CONFIG_BCSTM_MUXER
BRSTM_MUXER(bcstm, "BCSTM (Binary CTR Stream)", "bcstm", BRSTM_CSTM);
#endif
