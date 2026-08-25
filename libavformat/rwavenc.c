/*
 * Nintendo RWAV / FWAV / CWAV muxer
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

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

enum RWAVVariant {
    RWAV_VARIANT_RWAV,
    RWAV_VARIANT_FWAV,
    RWAV_VARIANT_CWAV,
};

typedef struct RWAVMuxContext {
    const AVClass *class;

    int      variant;
    int      little_endian;  /* -1: the variant's default */
    int      loop;
    int64_t  loop_start;     /* samples */

    AVIOContext *ch_buf[FF_DSP_ADPCM_MAX_CHANNELS];
    int16_t      coefs[FF_DSP_ADPCM_MAX_CHANNELS][16];
    int          have_coefs;
    int64_t      nb_samples;
    int          is_adpcm;
    int          bytes_per_sample;   /* PCM only */
} RWAVMuxContext;

static void wr16(AVFormatContext *s, unsigned v)
{
    RWAVMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl16(s->pb, v);
    else
        avio_wb16(s->pb, v);
}

static void wr32(AVFormatContext *s, unsigned v)
{
    RWAVMuxContext *c = s->priv_data;
    if (c->little_endian)
        avio_wl32(s->pb, v);
    else
        avio_wb32(s->pb, v);
}

static void wr_tag(AVFormatContext *s, const char *tag)
{
    avio_write(s->pb, tag, 4);
}

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

static int64_t bytes_to_samples(const RWAVMuxContext *c, int64_t bytes)
{
    return c->is_adpcm ? bytes / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME
                       : bytes / c->bytes_per_sample;
}

static int rwav_common_init(AVFormatContext *s)
{
    RWAVMuxContext *c = s->priv_data;
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
        break;
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_S8_PLANAR:
        c->bytes_per_sample = 1;
        break;
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S16BE_PLANAR:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16LE_PLANAR:
        c->bytes_per_sample = 2;
        break;
    default:
        av_log(s, AV_LOG_ERROR,
               "unsupported codec; use adpcm_thp, pcm_s16be_planar/pcm_s16le_planar, "
               "or pcm_s8_planar\n");
        return AVERROR(EINVAL);
    }

    if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "1 to %d channels are supported\n",
               FF_DSP_ADPCM_MAX_CHANNELS);
        return AVERROR(EINVAL);
    }

    if (c->little_endian < 0)
        c->little_endian = (c->variant == RWAV_VARIANT_CWAV);

    for (int ch = 0; ch < channels; ch++) {
        int ret = avio_open_dyn_buf(&c->ch_buf[ch]);
        if (ret < 0)
            return ret;
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, par->sample_rate);
    return 0;
}

static void rwav_deinit(AVFormatContext *s)
{
    RWAVMuxContext *c = s->priv_data;

    for (int ch = 0; ch < FF_DSP_ADPCM_MAX_CHANNELS; ch++)
        ffio_free_dyn_buf(&c->ch_buf[ch]);
}

static int rwav_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    RWAVMuxContext *c = s->priv_data;
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
                if (c->little_endian)
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
        c->nb_samples += bytes_to_samples(c, per_ch);

    return 0;
}

static int rwav_write_trailer(AVFormatContext *s)
{
    RWAVMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    int sample_rate = s->streams[0]->codecpar->sample_rate;
    uint8_t *ch_data[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int ch_bytes[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    int64_t n_samples;
    int encoding, ret;

    for (int ch = 0; ch < channels; ch++) {
        ch_bytes[ch] = avio_close_dyn_buf(c->ch_buf[ch], &ch_data[ch]);
        c->ch_buf[ch] = NULL;
    }

    if (c->nb_samples > 0)
        n_samples = c->nb_samples;
    else
        n_samples = bytes_to_samples(c, ch_bytes[0]);

    encoding = c->is_adpcm ? 2 : (c->bytes_per_sample == 2 ? 1 : 0);

    if (c->variant == RWAV_VARIANT_RWAV) {
        uint32_t ch_table_off = 0x1C;
        uint32_t ci_size = 0x1C;
        uint32_t ai_size = c->is_adpcm ? 0x30 : 0;
        uint32_t ci_base = ch_table_off + 4 * channels;
        uint32_t ai_base = ci_base + ci_size * channels;
        uint32_t info_body_size = ai_base + ai_size * channels;
        uint32_t info_chunk_size = ((8 + info_body_size) + 3) & ~3u;
        uint32_t data_payload_size = 0;
        uint32_t data_chunk_size, file_size;
        int64_t nibbles = c->is_adpcm ? ff_dsp_adpcm_nibble_count(n_samples) : 0;
        int64_t loop_nibbles = (c->is_adpcm && c->loop) ? ff_dsp_adpcm_nibble_address(c->loop_start) : c->loop_start;

        for (int ch = 0; ch < channels; ch++)
            data_payload_size += ch_bytes[ch];
        data_chunk_size = ((8 + data_payload_size) + 31) & ~31u;
        file_size = 0x20 + info_chunk_size + data_chunk_size;

        /* Header (0x20) */
        wr_tag(s, "RWAV");
        avio_wb16(s->pb, 0xFEFF); /* BOM */
        avio_wb16(s->pb, 0x0100); /* version 1.0 */
        avio_wb32(s->pb, file_size);
        avio_wb16(s->pb, 0x20);   /* header size */
        avio_wb16(s->pb, 2);      /* chunk count */
        avio_wb32(s->pb, 0x20);   /* info offset */
        avio_wb32(s->pb, info_chunk_size);
        avio_wb32(s->pb, 0x20 + info_chunk_size); /* data offset */
        avio_wb32(s->pb, data_chunk_size);

        /* INFO chunk */
        int64_t info_pos = avio_tell(s->pb);
        wr_tag(s, "INFO");
        avio_wb32(s->pb, info_chunk_size);
        avio_w8(s->pb, encoding);
        avio_w8(s->pb, c->loop ? 1 : 0);
        avio_w8(s->pb, channels);
        avio_w8(s->pb, 0);
        avio_wb16(s->pb, sample_rate);
        avio_w8(s->pb, 0); avio_w8(s->pb, 0);
        avio_wb32(s->pb, (uint32_t)loop_nibbles);
        avio_wb32(s->pb, c->is_adpcm ? (uint32_t)nibbles : (uint32_t)n_samples);
        avio_wb32(s->pb, ch_table_off);
        avio_wb32(s->pb, 0); avio_wb32(s->pb, 0);

        for (int ch = 0; ch < channels; ch++)
            avio_wb32(s->pb, ci_base + ci_size * ch);

        for (int ch = 0; ch < channels; ch++) {
            avio_wb32(s->pb, ch * ch_bytes[0]);
            avio_wb32(s->pb, c->is_adpcm ? (ai_base + ai_size * ch) : 0);
            avio_wb32(s->pb, 1); avio_wb32(s->pb, 1);
            avio_wb32(s->pb, 1); avio_wb32(s->pb, 1);
            avio_wb32(s->pb, 0);
        }

        if (c->is_adpcm) {
            for (int ch = 0; ch < channels; ch++) {
                for (int i = 0; i < 16; i++)
                    avio_wb16(s->pb, (uint16_t)c->coefs[ch][i]);
                avio_wb16(s->pb, 0); /* gain */
                avio_wb16(s->pb, ch_bytes[ch] ? ch_data[ch][0] : 0); /* ps */
                avio_wb16(s->pb, 0); avio_wb16(s->pb, 0); /* yn1, yn2 */
                avio_wb16(s->pb, 0); avio_wb16(s->pb, 0); avio_wb16(s->pb, 0); /* loop ps, yn1, yn2 */
                avio_wb16(s->pb, 0); /* pad */
            }
        }

        if ((ret = pad_to(s, info_pos + info_chunk_size)) < 0)
            goto fail;

        /* DATA chunk */
        int64_t data_pos = avio_tell(s->pb);
        wr_tag(s, "DATA");
        avio_wb32(s->pb, data_chunk_size);
        for (int ch = 0; ch < channels; ch++)
            avio_write(s->pb, ch_data[ch], ch_bytes[ch]);
        if ((ret = pad_to(s, data_pos + data_chunk_size)) < 0)
            goto fail;

    } else {
        /* FWAV (Wii U) / CWAV (3DS) */
        uint32_t info_off = 0x40;
        uint32_t wi_size = 0x18;
        uint32_t entry_size = 8;
        uint32_t ci_size = 0x14;
        uint32_t ai_size = 0x2E;
        uint32_t stream_pad = 24;

        uint32_t entries_off = wi_size;
        uint32_t ci_base = entries_off + entry_size * channels;
        uint32_t ai_base = ci_base + ci_size * channels;
        uint32_t info_body_size = ai_base + (c->is_adpcm ? ai_size * channels : 0);
        uint32_t info_chunk_size = (8 + info_body_size + 31) & ~31u;
        uint32_t data_off = info_off + info_chunk_size;
        uint64_t data_content = (uint64_t)stream_pad * channels + (uint64_t)ch_bytes[0] * channels;
        uint64_t data_chunk_size = 8 + data_content;
        uint64_t file_size = (uint64_t)data_off + data_chunk_size;

        /* Header (0x40) */
        wr_tag(s, c->variant == RWAV_VARIANT_CWAV ? "CWAV" : "FWAV");
        wr16(s, 0xFEFF);       /* BOM */
        wr16(s, 0x0040);       /* version 0.64 */
        wr32(s, 0x00010100);   /* marker */
        wr32(s, file_size);
        wr16(s, 2);            /* block count */
        wr16(s, 0);
        wr16(s, 0x7000);       /* INFO SizedReference type id */
        wr16(s, 0);            /* padding */
        wr32(s, info_off);
        wr32(s, info_chunk_size);
        wr16(s, 0x7001);       /* DATA SizedReference type id */
        wr16(s, 0);            /* padding */
        wr32(s, data_off);
        wr32(s, data_chunk_size);
        if ((ret = pad_to(s, info_off)) < 0)
            goto fail;

        /* INFO block */
        int64_t info_pos = avio_tell(s->pb);
        wr_tag(s, "INFO");
        wr32(s, info_chunk_size);
        avio_w8(s->pb, encoding);
        avio_w8(s->pb, c->loop ? 1 : 0);
        avio_w8(s->pb, 0); avio_w8(s->pb, 0);
        wr32(s, sample_rate);
        wr32(s, c->loop ? (uint32_t)c->loop_start : 0);
        wr32(s, n_samples);
        wr32(s, 0);
        wr32(s, channels); /* channel table anchor */

        for (int ch = 0; ch < channels; ch++) {
            wr32(s, 0x71000000);
            wr32(s, 4 + entry_size * channels + ci_size * ch);
        }

        for (int ch = 0; ch < channels; ch++) {
            wr16(s, 0x1F00);
            wr16(s, 0);
            wr32(s, stream_pad + ch * (stream_pad + ch_bytes[0]));
            wr32(s, 0x03000000);
            wr32(s, c->is_adpcm ? (ai_base + ai_size * ch - (ci_base + ci_size * ch)) : 0xFFFFFFFF);
            wr32(s, 0);
        }

        if (c->is_adpcm) {
            for (int ch = 0; ch < channels; ch++) {
                for (int i = 0; i < 16; i++)
                    wr16(s, (uint16_t)c->coefs[ch][i]);
                wr16(s, 0); /* gain */
                wr16(s, 0); /* ps */
                wr16(s, 0); wr16(s, 0); /* yn1, yn2 */
                wr16(s, 0); /* loop ps */
                wr16(s, 0); wr16(s, 0); /* loop yn1, yn2 */
            }
        }

        if ((ret = pad_to(s, info_pos + info_chunk_size)) < 0)
            goto fail;

        /* DATA block */
        int64_t data_pos = avio_tell(s->pb);
        wr_tag(s, "DATA");
        wr32(s, data_chunk_size);
        for (int ch = 0; ch < channels; ch++) {
            ffio_fill(s->pb, 0, stream_pad);
            avio_write(s->pb, ch_data[ch], ch_bytes[ch]);
        }
        if ((ret = pad_to(s, data_pos + data_chunk_size)) < 0)
            goto fail;
    }

fail:
    for (int ch = 0; ch < channels; ch++)
        av_freep(&ch_data[ch]);

    return ret;
}

#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption rwav_options[] = {
    { "endian", "byte order of the header fields", offsetof(RWAVMuxContext, little_endian),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, E, .unit = "endian" },
    { "default", "the variant's usual byte order", 0, AV_OPT_TYPE_CONST,
      { .i64 = -1 }, 0, 0, E, .unit = "endian" },
    { "be", "big-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "endian" },
    { "le", "little-endian", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, E, .unit = "endian" },
    { "loop", "mark the stream as looping", offsetof(RWAVMuxContext, loop), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, E },
    { "loop_start", "loop start, in samples", offsetof(RWAVMuxContext, loop_start),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, E },
    { NULL },
};

#define RWAV_MUXER(name_, long_name_, ext_, variant_)                       \
static const AVClass name_ ## _muxer_class = {                              \
    .class_name = #name_ " muxer",                                          \
    .item_name  = av_default_item_name,                                     \
    .option     = rwav_options,                                             \
    .version    = LIBAVUTIL_VERSION_INT,                                    \
};                                                                          \
static int name_ ## _init(AVFormatContext *s)                               \
{                                                                           \
    ((RWAVMuxContext *)s->priv_data)->variant = variant_;                   \
    return rwav_common_init(s);                                             \
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
    .priv_data_size   = sizeof(RWAVMuxContext),                             \
    .init             = name_ ## _init,                                     \
    .deinit           = rwav_deinit,                                        \
    .write_packet     = rwav_write_packet,                                  \
    .write_trailer    = rwav_write_trailer,                                 \
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH,                       \
}

#if CONFIG_RWAV_MUXER
RWAV_MUXER(rwav, "RWAV (Binary Revolution Wave)", "rwav,brwav", RWAV_VARIANT_RWAV);
#endif
#if CONFIG_FWAV_MUXER
RWAV_MUXER(fwav, "FWAV (Binary Cafe Wave)", "fwav,bfwav", RWAV_VARIANT_FWAV);
#endif
#if CONFIG_CWAV_MUXER
RWAV_MUXER(cwav, "CWAV (Binary CTR Wave)", "cwav,bcwav", RWAV_VARIANT_CWAV);
#endif
