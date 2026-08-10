/*
 * Nintendo DSP-ADPCM (.dsp) muxer
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
 * The bare container Nintendo's DSPADPCM tool emits: a 0x60-byte header
 * describing one channel, then that channel's ADPCM frames. It is the
 * reference format for GameCube/Wii/3DS DSP-ADPCM -- everything else
 * (BRSTM, BNS, AST) is this data with a different wrapper -- which makes it
 * the natural thing to check the encoder against.
 *
 * A multi-channel file is the same thing repeated: one header per channel,
 * all of them up front, then the channels' data interleaved in fixed-size
 * blocks. That layout is what tools call MDSP; a mono file is bit-identical
 * to a classic .dsp either way.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

typedef struct DSPMuxContext {
    const AVClass *class;

    int      interleave;    /* frames per channel per interleave block */
    int      loop;
    int64_t  loop_start;    /* samples */
    int64_t  loop_end;      /* samples, <0 = end of stream */

    /* One buffer per channel. adpcm_thp emits a single channel-major packet
     * at drain, but nothing in the packet contract promises that, so split
     * each packet by channel as it arrives and reassemble at the end. */
    AVIOContext *ch_buf[FF_DSP_ADPCM_MAX_CHANNELS];
    int16_t      coefs[FF_DSP_ADPCM_MAX_CHANNELS][16];
    int          have_coefs;
    int64_t      nb_samples;
} DSPMuxContext;

static int dsp_init(AVFormatContext *s)
{
    DSPMuxContext *c = s->priv_data;
    AVCodecParameters *par;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "DSP files carry exactly one stream\n");
        return AVERROR(EINVAL);
    }
    par = s->streams[0]->codecpar;

    if (par->codec_id != AV_CODEC_ID_ADPCM_THP) {
        av_log(s, AV_LOG_ERROR,
               "DSP carries Nintendo DSP-ADPCM only; use -c:a adpcm_thp\n");
        return AVERROR(EINVAL);
    }
    if (par->ch_layout.nb_channels > FF_DSP_ADPCM_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "At most %d channels are supported\n",
               FF_DSP_ADPCM_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    for (int ch = 0; ch < par->ch_layout.nb_channels; ch++) {
        int ret = avio_open_dyn_buf(&c->ch_buf[ch]);
        if (ret < 0)
            return ret;
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, par->sample_rate);
    return 0;
}

static void dsp_deinit(AVFormatContext *s)
{
    DSPMuxContext *c = s->priv_data;

    for (int ch = 0; ch < FF_DSP_ADPCM_MAX_CHANNELS; ch++)
        ffio_free_dyn_buf(&c->ch_buf[ch]);
}

/* The adpcm_thp encoder derives its predictor coefficients over the whole
 * channel and hands them over as replacement extradata; a stream copy from a
 * container that already knows them brings them in par->extradata instead. */
static int dsp_take_coefs(AVFormatContext *s, const AVPacket *pkt)
{
    DSPMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    const uint8_t *src = s->streams[0]->codecpar->extradata;
    size_t size = s->streams[0]->codecpar->extradata_size;
    size_t side_size;
    const uint8_t *side;

    side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    if (side) {
        src  = side;
        size = side_size;
    }
    if (!src || size < 32 * (size_t)channels)
        return 0;

    for (int ch = 0; ch < channels; ch++)
        for (int i = 0; i < 16; i++)
            c->coefs[ch][i] = (int16_t)AV_RB16(src + ch * 32 + i * 2);
    c->have_coefs = 1;
    return 1;
}

static int dsp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    DSPMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    int per_ch;

    dsp_take_coefs(s, pkt);

    if (pkt->size % (channels * FF_DSP_ADPCM_BYTES_PER_FRAME)) {
        av_log(s, AV_LOG_ERROR,
               "packet of %d bytes is not a whole number of %d-byte frames "
               "for %d channels\n", pkt->size, FF_DSP_ADPCM_BYTES_PER_FRAME,
               channels);
        return AVERROR_INVALIDDATA;
    }
    per_ch = pkt->size / channels;

    for (int ch = 0; ch < channels; ch++)
        avio_write(c->ch_buf[ch], pkt->data + (size_t)ch * per_ch, per_ch);

    /* The encoder reports the real sample count, which is what the header
     * must carry: the frame count alone rounds up to a multiple of 14 and
     * would make a player emit up to 13 samples of padding. */
    if (pkt->duration > 0)
        c->nb_samples += pkt->duration;
    else
        c->nb_samples += (int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME;

    return 0;
}

static void dsp_write_header_block(AVFormatContext *s, int ch,
                                   const uint8_t *data, int64_t data_size)
{
    DSPMuxContext *c = s->priv_data;
    AVCodecParameters *par = s->streams[0]->codecpar;
    int64_t loop_start = c->loop ? c->loop_start : 0;
    int64_t loop_end   = c->loop && c->loop_end >= 0 ? c->loop_end
                                                     : c->nb_samples - 1;
    int16_t loop_h1 = 0, loop_h2 = 0;
    int loop_ps = 0;

    if (c->loop && loop_start > 0) {
        /* The header caches the decoder state a player needs to jump to the
         * loop point without decoding the whole file first. Only whole frames
         * are replayed: a loop point inside a frame decodes from that frame's
         * start, which is what loop_ps names. */
        int64_t frames = loop_start / FF_DSP_ADPCM_SAMPLES_PER_FRAME;
        int64_t off    = frames * FF_DSP_ADPCM_BYTES_PER_FRAME;

        if (off < data_size) {
            ff_dsp_adpcm_advance(data, frames, c->coefs[ch], &loop_h1, &loop_h2);
            loop_ps = data[off];
        }
    }

    avio_wb32(s->pb, c->nb_samples);
    avio_wb32(s->pb, ff_dsp_adpcm_nibble_count(c->nb_samples));
    avio_wb32(s->pb, par->sample_rate);
    avio_wb16(s->pb, c->loop);
    avio_wb16(s->pb, 0);                                        /* format: ADPCM */
    avio_wb32(s->pb, ff_dsp_adpcm_nibble_address(loop_start));
    avio_wb32(s->pb, ff_dsp_adpcm_nibble_address(FFMAX(loop_end, 0)));
    avio_wb32(s->pb, 2);                                        /* current address */
    for (int i = 0; i < 16; i++)
        avio_wb16(s->pb, c->coefs[ch][i]);
    avio_wb16(s->pb, 0);                                        /* gain: unused by ADPCM */
    avio_wb16(s->pb, data_size ? data[0] : 0);                  /* initial predictor/scale */
    avio_wb16(s->pb, 0);                                        /* initial hist1 */
    avio_wb16(s->pb, 0);                                        /* initial hist2 */
    avio_wb16(s->pb, loop_ps);
    avio_wb16(s->pb, loop_h1);
    avio_wb16(s->pb, loop_h2);
    avio_wb16(s->pb, par->ch_layout.nb_channels > 1 ?
                     par->ch_layout.nb_channels : 0);
    avio_wb16(s->pb, par->ch_layout.nb_channels > 1 ? c->interleave : 0);
    ffio_fill(s->pb, 0, FF_DSP_ADPCM_HEADER_SIZE - 0x4E);
}

static int dsp_write_trailer(AVFormatContext *s)
{
    DSPMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    uint8_t *data[FF_DSP_ADPCM_MAX_CHANNELS] = { NULL };
    int size[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int block = c->interleave * FF_DSP_ADPCM_BYTES_PER_FRAME;
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
    if (!c->have_coefs) {
        av_log(s, AV_LOG_ERROR, "no DSP-ADPCM coefficient table available; "
               "the input has to come from the adpcm_thp encoder or from a "
               "container that carries the table\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    for (int ch = 0; ch < channels; ch++)
        dsp_write_header_block(s, ch, data[ch], size[ch]);

    if (channels == 1) {
        avio_write(s->pb, data[0], size[0]);
    } else {
        /* Interleave in fixed blocks, zero-padding the tail so every block is
         * the same size -- the sample count in the header is what bounds
         * playback, so the padding is never decoded. */
        for (int off = 0; off < size[0]; off += block) {
            for (int ch = 0; ch < channels; ch++) {
                int n = FFMAX(FFMIN(block, size[ch] - off), 0);
                if (n)
                    avio_write(s->pb, data[ch] + off, n);
                if (n < block)
                    ffio_fill(s->pb, 0, block - n);
            }
        }
    }

end:
    for (int ch = 0; ch < channels; ch++)
        av_free(data[ch]);
    return ret;
}

#define OFFSET(x) offsetof(DSPMuxContext, x)
static const AVOption dsp_options[] = {
    { "interleave", "frames per channel in each interleave block (multi-channel only)",
      OFFSET(interleave), AV_OPT_TYPE_INT, { .i64 = 1024 }, 1, 1 << 20, AV_OPT_FLAG_ENCODING_PARAM },
    { "loop", "mark the stream as looping", OFFSET(loop), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "loop_start", "loop start, in samples", OFFSET(loop_start), AV_OPT_TYPE_INT64,
      { .i64 = 0 }, 0, INT64_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "loop_end", "loop end, in samples (-1 = end of stream)", OFFSET(loop_end),
      AV_OPT_TYPE_INT64, { .i64 = -1 }, -1, INT64_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass dsp_muxer_class = {
    .class_name = "dsp muxer",
    .item_name  = av_default_item_name,
    .option     = dsp_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_dsp_muxer = {
    .p.name            = "dsp",
    .p.long_name       = NULL_IF_CONFIG_SMALL("Nintendo DSP-ADPCM"),
    .p.extensions      = "dsp",
    .p.audio_codec     = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec     = AV_CODEC_ID_NONE,
    .p.subtitle_codec  = AV_CODEC_ID_NONE,
    .p.flags           = AVFMT_NOTIMESTAMPS,
    .p.priv_class      = &dsp_muxer_class,
    .priv_data_size    = sizeof(DSPMuxContext),
    .init              = dsp_init,
    .deinit            = dsp_deinit,
    .write_packet      = dsp_write_packet,
    .write_trailer     = dsp_write_trailer,
    .flags_internal    = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                         FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
