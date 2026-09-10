/*
 * Nintendo BWAV muxer (Switch Binary Wave, AAL library)
 *
 * Writes the streamed-wave container of Nintendo's AAL audio library:
 * a 0x10 file header, one 0x4C channel info per channel, then each
 * channel's sample data as a separate block padded to 0x40. The header
 * hash is the standard CRC32 over the concatenated channel data.
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

#include "config_components.h"

#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

#define BWAV_CHANNEL_INFO_SIZE 0x4C
#define BWAV_DATA_ALIGN        0x40

typedef struct BWAVMuxContext {
    const AVClass *class;

    int      little_endian;  /* -1: default (little) */
    int      loop;
    int64_t  loop_start;     /* samples */

    AVIOContext *ch_buf[FF_DSP_ADPCM_MAX_CHANNELS];
    int16_t      coefs[FF_DSP_ADPCM_MAX_CHANNELS][16];
    int          have_coefs;
    int64_t      nb_samples;
    int          is_adpcm;
    int          extradata_le;       /* DSP coefficients in extradata are LE */
    int          bytes_per_sample;   /* PCM only */
} BWAVMuxContext;

static void bwav_wr16(AVFormatContext *s, unsigned v)
{
    BWAVMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl16(s->pb, v);
    else
        avio_wb16(s->pb, v);
}

static void bwav_wr32(AVFormatContext *s, unsigned v)
{
    BWAVMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl32(s->pb, v);
    else
        avio_wb32(s->pb, v);
}

static void bwav_wr_tag(AVFormatContext *s, const char *tag)
{
    avio_write(s->pb, tag, 4);
}

static int bwav_pad_to(AVFormatContext *s, int64_t target)
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

static int64_t bwav_bytes_to_samples(const BWAVMuxContext *c, int64_t bytes)
{
    return c->is_adpcm ? bytes / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME
                       : bytes / c->bytes_per_sample;
}

static int bwav_init(AVFormatContext *s)
{
    BWAVMuxContext *c = s->priv_data;
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
    case AV_CODEC_ID_ADPCM_THP_LE:
        c->is_adpcm = 1;
        /* The byte order of the coefficient table in extradata belongs to
         * the codec that produced it, not to the container it is going
         * into: adpcm_thp writes it big-endian and adpcm_thp_le little-
         * endian, and either may be muxed into either byte order. */
        c->extradata_le = (par->codec_id == AV_CODEC_ID_ADPCM_THP_LE);
        break;
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S16BE_PLANAR:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16LE_PLANAR:
        c->bytes_per_sample = 2;
        break;
    default:
        av_log(s, AV_LOG_ERROR,
               "unsupported codec; use adpcm_thp or pcm_s16be_planar/pcm_s16le_planar\n");
        return AVERROR(EINVAL);
    }

    if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "1 to %d channels are supported\n",
               FF_DSP_ADPCM_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    if (c->little_endian < 0)
        c->little_endian = 1;

    /* PCM16 sample data is written through verbatim, so the encoder's byte
     * order has to be the container's. */
    if (c->bytes_per_sample == 2) {
        int pcm_le = (par->codec_id == AV_CODEC_ID_PCM_S16LE ||
                      par->codec_id == AV_CODEC_ID_PCM_S16LE_PLANAR);
        if (pcm_le != !!c->little_endian) {
            av_log(s, AV_LOG_ERROR,
                   "this file is %s-endian, so PCM16 must be encoded as %s; "
                   "use -c:a %s\n",
                   c->little_endian ? "little" : "big",
                   c->little_endian ? "pcm_s16le_planar" : "pcm_s16be_planar",
                   c->little_endian ? "pcm_s16le_planar" : "pcm_s16be_planar");
            return AVERROR(EINVAL);
        }
    }

    for (int ch = 0; ch < channels; ch++) {
        int ret = avio_open_dyn_buf(&c->ch_buf[ch]);
        if (ret < 0)
            return ret;
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, par->sample_rate);
    return 0;
}

static void bwav_deinit(AVFormatContext *s)
{
    BWAVMuxContext *c = s->priv_data;

    for (int ch = 0; ch < FF_DSP_ADPCM_MAX_CHANNELS; ch++)
        ffio_free_dyn_buf(&c->ch_buf[ch]);
}

static int bwav_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    BWAVMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    size_t side_size;
    const uint8_t *side;
    int per_ch;

    side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    if (!side) {
        side      = s->streams[0]->codecpar->extradata;
        side_size = s->streams[0]->codecpar->extradata_size;
    }
    if (side && side_size >= 32 * (size_t)channels) {
        for (int ch = 0; ch < channels; ch++) {
            for (int i = 0; i < 16; i++) {
                if (c->extradata_le)
                    c->coefs[ch][i] = (int16_t)AV_RL16(side + ch * 32 + i * 2);
                else
                    c->coefs[ch][i] = (int16_t)AV_RB16(side + ch * 32 + i * 2);
            }
        }
        c->have_coefs = 1;
    }

    if (pkt->size % channels) {
        av_log(s, AV_LOG_ERROR, "packet of %d bytes is not divisible by %d channels\n",
               pkt->size, channels);
        return AVERROR_INVALIDDATA;
    }
    per_ch = pkt->size / channels;

    for (int ch = 0; ch < channels; ch++)
        avio_write(c->ch_buf[ch], pkt->data + (size_t)ch * per_ch, per_ch);

    if (pkt->duration > 0)
        c->nb_samples += pkt->duration;
    else
        c->nb_samples += bwav_bytes_to_samples(c, per_ch);

    return 0;
}

static int bwav_write_trailer(AVFormatContext *s)
{
    BWAVMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    int sample_rate = s->streams[0]->codecpar->sample_rate;
    uint8_t *ch_data[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int ch_bytes[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    uint32_t offsets[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int64_t n_samples, info_end;
    int codec, ret = 0;
    const AVCRC *crc_table;
    uint32_t hash;

    for (int ch = 0; ch < channels; ch++) {
        ch_bytes[ch] = avio_close_dyn_buf(c->ch_buf[ch], &ch_data[ch]);
        c->ch_buf[ch] = NULL;
        if (ch_bytes[ch] < 0) {
            ret = ch_bytes[ch];
            ch_bytes[ch] = 0;
            goto fail;
        }
    }

    if (c->nb_samples > 0)
        n_samples = c->nb_samples;
    else
        n_samples = bwav_bytes_to_samples(c, ch_bytes[0]);

    if (n_samples <= 0 || n_samples > UINT32_MAX) {
        av_log(s, AV_LOG_ERROR, "invalid sample count %"PRId64"\n", n_samples);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (c->loop && (c->loop_start < 0 || c->loop_start >= n_samples)) {
        av_log(s, AV_LOG_ERROR, "loop_start out of range\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    codec = c->is_adpcm ? 1 : 0;

    /* Channel data offsets, each padded to 0x40 except the end of file. */
    info_end = 0x10 + (int64_t)channels * BWAV_CHANNEL_INFO_SIZE;
    offsets[0] = (uint32_t)((info_end + BWAV_DATA_ALIGN - 1) &
                            ~(int64_t)(BWAV_DATA_ALIGN - 1));
    for (int ch = 1; ch < channels; ch++)
        offsets[ch] = (offsets[ch - 1] + ch_bytes[ch - 1] +
                       BWAV_DATA_ALIGN - 1) &
                      ~(uint32_t)(BWAV_DATA_ALIGN - 1);

    /* Standard CRC32 over the concatenated channel data, no padding. */
    crc_table = av_crc_get_table(AV_CRC_32_IEEE_LE);
    if (!crc_table) {
        ret = AVERROR_BUG;
        goto fail;
    }
    hash = ~0U;
    for (int ch = 0; ch < channels; ch++)
        hash = av_crc(crc_table, hash, ch_data[ch], ch_bytes[ch]);
    hash = ~hash;

    /* File header (0x10) */
    bwav_wr_tag(s, "BWAV");
    bwav_wr16(s, 0xFEFF); /* BOM */
    bwav_wr16(s, 1);      /* version */
    bwav_wr32(s, hash);
    bwav_wr16(s, 0);      /* prefetch flag */
    bwav_wr16(s, channels);

    /* Channel infos */
    for (int ch = 0; ch < channels; ch++) {
        /* Real files use 0/1/2 for left/right/middle; mono is middle. */
        int pan = channels == 1 ? 2 : ch < 2 ? ch : 2;

        bwav_wr16(s, codec);
        bwav_wr16(s, pan);
        bwav_wr32(s, sample_rate);
        bwav_wr32(s, n_samples);
        bwav_wr32(s, n_samples);
        if (c->is_adpcm) {
            for (int i = 0; i < 16; i++)
                bwav_wr16(s, (uint16_t)c->coefs[ch][i]);
        } else {
            for (int i = 0; i < 16; i++)
                bwav_wr16(s, 0);
        }
        bwav_wr32(s, offsets[ch]);
        bwav_wr32(s, offsets[ch]);
        /* The loop flag reads 1 even in files that do not loop; the
         * loop_end value is what decides. */
        bwav_wr32(s, 1);
        bwav_wr32(s, c->loop ? (uint32_t)n_samples : 0xFFFFFFFF);
        bwav_wr32(s, c->loop ? (uint32_t)c->loop_start : 0);
        /* Start context (predictor scale + histories): the demuxers start
         * from silence, so zeros are an honest description of what the
         * bundled encoder wrote. */
        bwav_wr16(s, 0);
        bwav_wr16(s, 0);
        bwav_wr16(s, 0);
        bwav_wr16(s, 0);
    }

    /* Channel data */
    for (int ch = 0; ch < channels; ch++) {
        if ((ret = bwav_pad_to(s, offsets[ch])) < 0)
            goto fail;
        avio_write(s->pb, ch_data[ch], ch_bytes[ch]);
    }

fail:
    for (int ch = 0; ch < channels; ch++)
        av_freep(&ch_data[ch]);

    return ret;
}

#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption bwav_options[] = {
    { "endian", "byte order of the header fields", offsetof(BWAVMuxContext, little_endian),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, E, .unit = "endian" },
    { "default", "little-endian", 0, AV_OPT_TYPE_CONST,
      { .i64 = -1 }, 0, 0, E, .unit = "endian" },
    { "be", "big-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "endian" },
    { "le", "little-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, E, .unit = "endian" },
    { "loop", "mark the stream as looping", offsetof(BWAVMuxContext, loop), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, E },
    { "loop_start", "loop start, in samples", offsetof(BWAVMuxContext, loop_start),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, E },
    { NULL },
};

static const AVClass bwav_muxer_class = {
    .class_name = "bwav muxer",
    .item_name  = av_default_item_name,
    .option     = bwav_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_bwav_muxer = {
    .p.name           = "bwav",
    .p.long_name      = NULL_IF_CONFIG_SMALL("BWAV (Binary Wave)"),
    .p.extensions     = "bwav",
    .p.audio_codec    = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec    = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &bwav_muxer_class,
    .priv_data_size   = sizeof(BWAVMuxContext),
    .init             = bwav_init,
    .deinit           = bwav_deinit,
    .write_packet     = bwav_write_packet,
    .write_trailer    = bwav_write_trailer,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH,
};
