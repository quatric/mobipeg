/*
 * Wii U boot sound (.btsnd) muxer
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

#include "libavutil/opt.h"

#include "avformat.h"
#include "btsnd.h"
#include "internal.h"
#include "mux.h"

typedef struct BTSNDMuxContext {
    const AVClass *class;
    int64_t loop_start;   /* samples; 0 means no loop */
} BTSNDMuxContext;

static int btsnd_init(AVFormatContext *s)
{
    AVCodecParameters *par;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "btsnd carries exactly one stream\n");
        return AVERROR(EINVAL);
    }
    par = s->streams[0]->codecpar;

    /* Nothing downstream reads these back out of the file, so a mismatch
     * would not fail -- it would just play at the wrong speed on hardware. */
    if (par->sample_rate != BTSND_SAMPLE_RATE ||
        par->ch_layout.nb_channels != BTSND_CHANNELS) {
        av_log(s, AV_LOG_ERROR,
               "btsnd is always %d Hz stereo; add -ar %d -ac %d\n",
               BTSND_SAMPLE_RATE, BTSND_SAMPLE_RATE, BTSND_CHANNELS);
        return AVERROR(EINVAL);
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, BTSND_SAMPLE_RATE);
    return 0;
}

static int btsnd_write_header(AVFormatContext *s)
{
    BTSNDMuxContext *c = s->priv_data;

    avio_wb32(s->pb, c->loop_start ? 1 : 0);
    avio_wb32(s->pb, c->loop_start);

    return 0;
}

static int btsnd_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

#define OFFSET(x) offsetof(BTSNDMuxContext, x)
static const AVOption btsnd_options[] = {
    { "loop_start", "loop point, in samples (0 = play once)", OFFSET(loop_start),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, UINT32_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass btsnd_muxer_class = {
    .class_name = "btsnd muxer",
    .item_name  = av_default_item_name,
    .option     = btsnd_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_btsnd_muxer = {
    .p.name           = "btsnd",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Wii U boot sound"),
    .p.extensions     = "btsnd",
    .p.audio_codec    = AV_CODEC_ID_PCM_S16BE,
    .p.video_codec    = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &btsnd_muxer_class,
    .priv_data_size   = sizeof(BTSNDMuxContext),
    .init             = btsnd_init,
    .write_header     = btsnd_write_header,
    .write_packet     = btsnd_write_packet,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                        FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
