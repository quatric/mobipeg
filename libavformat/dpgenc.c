/*
 * Nintendo DS DPG (nDs-mPeG) muxer
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

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "dpg.h"
#include "internal.h"
#include "mux.h"

typedef struct DPGMuxContext {
    const AVClass *class;

    int   version;
    int   pixel_format;
    char *thumbnail;

    AVIOContext *video;
    AVIOContext *audio;
    int video_index;
    int audio_index;
    int64_t frames;
} DPGMuxContext;

static int dpg_init(AVFormatContext *s)
{
    DPGMuxContext *c = s->priv_data;
    int ret;

    c->video_index = c->audio_index = -1;

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO && c->video_index < 0) {
            if (par->codec_id != AV_CODEC_ID_MPEG1VIDEO) {
                av_log(s, AV_LOG_ERROR,
                       "DPG video is MPEG-1; use -c:v mpeg1video\n");
                return AVERROR(EINVAL);
            }
            c->video_index = i;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && c->audio_index < 0) {
            if (par->codec_id != AV_CODEC_ID_MP2) {
                av_log(s, AV_LOG_ERROR, "DPG audio is MP2; use -c:a mp2\n");
                return AVERROR(EINVAL);
            }
            c->audio_index = i;
        } else {
            av_log(s, AV_LOG_ERROR,
                   "DPG holds one video stream and one audio stream\n");
            return AVERROR(EINVAL);
        }
    }
    if (c->video_index < 0) {
        av_log(s, AV_LOG_ERROR, "DPG needs a video stream\n");
        return AVERROR(EINVAL);
    }

    if ((ret = avio_open_dyn_buf(&c->video)) < 0)
        return ret;
    if ((ret = avio_open_dyn_buf(&c->audio)) < 0)
        return ret;

    return 0;
}

static void dpg_deinit(AVFormatContext *s)
{
    DPGMuxContext *c = s->priv_data;

    ffio_free_dyn_buf(&c->video);
    ffio_free_dyn_buf(&c->audio);
}

static int dpg_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    DPGMuxContext *c = s->priv_data;

    /* The two streams sit in separate regions of the file rather than being
     * multiplexed, so each one just accumulates. */
    if (pkt->stream_index == c->video_index) {
        avio_write(c->video, pkt->data, pkt->size);
        c->frames++;
    } else {
        avio_write(c->audio, pkt->data, pkt->size);
    }

    return 0;
}

/*
 * Byte offsets of every GOP header in the video stream, relative to its start.
 *
 * MoonShell seeks by jumping to the nearest entry, so only positions a decoder
 * can start from belong here -- which is what a GOP start code marks. dpg4x
 * builds the index the same way, by scanning for the start code; the entry
 * width is not documented anywhere, and 32-bit little-endian offsets are what
 * every other field in this header uses.
 */
static int dpg_write_gop_index(AVFormatContext *s, const uint8_t *video,
                               int size)
{
    uint32_t state = 0xFFFFFFFF;
    int count = 0;

    for (int i = 0; i < size; i++) {
        state = (state << 8) | video[i];
        if (state == 0x000001B8) {
            avio_wl32(s->pb, i - 3);
            count++;
        }
    }

    return count;
}

static int dpg_write_thumbnail(AVFormatContext *s)
{
    DPGMuxContext *c = s->priv_data;
    uint8_t *buf;
    AVIOContext *in = NULL;
    int ret;

    if (!c->thumbnail) {
        /* A black thumbnail is what MoonShell shows when there is nothing to
         * show; leaving the region out entirely would move every offset. */
        ffio_fill(s->pb, 0, DPG_THUMB_SIZE);
        return 0;
    }

    if ((ret = avio_open(&in, c->thumbnail, AVIO_FLAG_READ)) < 0) {
        av_log(s, AV_LOG_ERROR, "could not open thumbnail %s\n", c->thumbnail);
        return ret;
    }

    buf = av_malloc(DPG_THUMB_SIZE);
    if (!buf) {
        avio_closep(&in);
        return AVERROR(ENOMEM);
    }
    ret = avio_read(in, buf, DPG_THUMB_SIZE);
    avio_closep(&in);
    if (ret != DPG_THUMB_SIZE) {
        av_log(s, AV_LOG_ERROR,
               "thumbnail must be exactly %d bytes: %dx%d raw 16-bit pixels "
               "(ffmpeg -i in -vf scale=%d:%d -pix_fmt rgb555le -frames:v 1 "
               "-f rawvideo thumb.raw)\n",
               DPG_THUMB_SIZE, DPG_THUMB_WIDTH, DPG_THUMB_HEIGHT,
               DPG_THUMB_WIDTH, DPG_THUMB_HEIGHT);
        av_free(buf);
        return AVERROR_INVALIDDATA;
    }

    avio_write(s->pb, buf, DPG_THUMB_SIZE);
    av_free(buf);
    return 0;
}

static int dpg_write_trailer(AVFormatContext *s)
{
    DPGMuxContext *c = s->priv_data;
    AVStream *vst = s->streams[c->video_index];
    AVCodecParameters *apar = c->audio_index >= 0
                            ? s->streams[c->audio_index]->codecpar : NULL;
    uint8_t *video = NULL, *audio = NULL;
    int video_size, audio_size;
    int header = ff_dpg_header_size(c->version);
    int64_t audio_off, video_off, gop_off, gop_pos;
    AVRational fps = vst->avg_frame_rate.num ? vst->avg_frame_rate
                                             : vst->r_frame_rate;
    int ret = 0, gop_count;

    video_size = avio_close_dyn_buf(c->video, &video);
    audio_size = avio_close_dyn_buf(c->audio, &audio);
    c->video = c->audio = NULL;
    if (video_size < 0 || audio_size < 0) {
        ret = FFMIN(video_size, audio_size);
        goto end;
    }

    audio_off = header + (c->version >= 4 ? DPG_THUMB_SIZE : 0);
    video_off = audio_off + audio_size;
    gop_off   = video_off + video_size;

    avio_write(s->pb, "DPG", 3);
    avio_w8(s->pb, '0' + c->version);
    avio_wl32(s->pb, c->frames);
    /* DPG0 and DPG1 store a whole number of frames per second. DPG2 raised
     * the frame rate ceiling, and the field became 8.8 fixed point so rates
     * like 23.976 can be expressed; that scaling is not written down
     * anywhere, but it is what the encoders in circulation produce. */
    if (c->version >= 2)
        avio_wl32(s->pb, av_rescale(256, fps.num, FFMAX(fps.den, 1)));
    else
        avio_wl32(s->pb, fps.den ? fps.num / fps.den : 0);
    avio_wl32(s->pb, apar ? apar->sample_rate : 0);
    avio_wl32(s->pb, apar ? apar->ch_layout.nb_channels : 0);
    avio_wl32(s->pb, audio_off);
    avio_wl32(s->pb, audio_size);
    avio_wl32(s->pb, video_off);
    avio_wl32(s->pb, video_size);

    if (c->version >= 2) {
        avio_wl32(s->pb, gop_off);
        /* Backfilled once the index has been written and counted. */
        gop_pos = avio_tell(s->pb);
        avio_wl32(s->pb, 0);
        avio_wl32(s->pb, c->pixel_format);
    } else {
        gop_pos = -1;
    }
    if (c->version >= 4)
        avio_write(s->pb, "THM0", 4);

    if (c->version >= 4 && (ret = dpg_write_thumbnail(s)) < 0)
        goto end;

    avio_write(s->pb, audio, audio_size);
    avio_write(s->pb, video, video_size);

    if (c->version >= 2) {
        gop_count = dpg_write_gop_index(s, video, video_size);
        if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
            av_log(s, AV_LOG_WARNING,
                   "output is not seekable, so the GOP index size stays 0; "
                   "the file plays but MoonShell cannot seek in it\n");
        } else {
            int64_t end = avio_tell(s->pb);
            avio_seek(s->pb, gop_pos, SEEK_SET);
            avio_wl32(s->pb, gop_count * 4);
            avio_seek(s->pb, end, SEEK_SET);
        }
    }

end:
    av_free(video);
    av_free(audio);
    return ret;
}

#define OFFSET(x) offsetof(DPGMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption dpg_options[] = {
    /* Not "version": ffmpeg's own global -version would swallow it. */
    { "dpg_version", "DPG version to write (0-4)", OFFSET(version),
      AV_OPT_TYPE_INT, { .i64 = 4 }, 0, 4, E },
    { "pixel_format", "MoonShell pixel format field (DPG2 and later)",
      OFFSET(pixel_format), AV_OPT_TYPE_INT,
      { .i64 = DPG_PIXEL_FORMAT_RGB24 }, 0, 3, E },
    { "thumbnail", "raw 256x192 16-bit image for the DPG4 thumbnail slot",
      OFFSET(thumbnail), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { NULL },
};

static const AVClass dpg_muxer_class = {
    .class_name = "dpg muxer",
    .item_name  = av_default_item_name,
    .option     = dpg_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_dpg_muxer = {
    .p.name           = "dpg",
    .p.long_name      = NULL_IF_CONFIG_SMALL("DPG (Nintendo DS nDs-mPeG)"),
    .p.extensions     = "dpg",
    .p.audio_codec    = AV_CODEC_ID_MP2,
    .p.video_codec    = AV_CODEC_ID_MPEG1VIDEO,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &dpg_muxer_class,
    .priv_data_size   = sizeof(DPGMuxContext),
    .init             = dpg_init,
    .deinit           = dpg_deinit,
    .write_packet     = dpg_write_packet,
    .write_trailer    = dpg_write_trailer,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                        FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
