/*
 * HVQM4 Demuxer
 * Copyright (c) 2019 Tillmann Karras
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

#include "avformat.h"
#include "avio.h"
#include "demux.h"
#include "internal.h"
#include "libavutil/channel_layout.h"
#include "libavutil/internal.h"

#define HVQM4_MAGIC_SIZE 16

static int hvqm4_read_probe(const AVProbeData *p)
{
    if (p->buf_size < HVQM4_MAGIC_SIZE)
        return 0;
    if (memcmp(p->buf, "HVQM4 1.3", 10) == 0)
        return AVPROBE_SCORE_MAX;
    if (memcmp(p->buf, "HVQM4 1.5", 10) == 0)
        return AVPROBE_SCORE_MAX;
    return 0;
}

typedef struct
{
    struct FileHeader
    {
        uint32_t nb_gops;
    } file;
    struct GopHeader
    {
        uint32_t prev_size;
        uint32_t next_size;
        uint32_t nb_video_frames;
        uint32_t nb_audio_frames;
    } gop;

    int video_stream_index;
    int audio_stream_index;

    int64_t gop_start;
    uint32_t gop_index;
    uint32_t gop_video_index;
    uint32_t gop_audio_index;
    uint32_t video_dts;
    uint32_t audio_dts;
    uint32_t gop_beginning_video_pts;
} Hvqm4DemuxContext;

static int hvqm4_read_header(AVFormatContext *ctx)
{
    Hvqm4DemuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;
    int ret;

    memset(h4m, 0, sizeof(*h4m));

    avio_skip(pb, HVQM4_MAGIC_SIZE);
    uint32_t header_size = avio_rb32(pb);
    if (header_size != 0x44)
        return AVERROR_INVALIDDATA;
    avio_rb32(pb); // body_size
    h4m->file.nb_gops = avio_rb32(pb);
    uint32_t video_frames = avio_rb32(pb);
    uint32_t audio_frames = avio_rb32(pb);
    uint32_t frame_usec = avio_rb32(pb);
    avio_rb32(pb); // max_frame_size
    avio_skip(pb, 4); // unknown
    avio_rb32(pb); // audio_frame_size
    uint16_t width = avio_rb16(pb);
    uint16_t height = avio_rb16(pb);
    uint8_t hsamp = avio_r8(pb);
    uint8_t vsamp = avio_r8(pb);
    avio_r8(pb); // video_mode
    avio_skip(pb, 1); // unknown
    uint8_t audio_channels = avio_r8(pb);
    avio_r8(pb); // audio_bitdepth
    avio_skip(pb, 2); // unknown
    uint32_t audio_sample_rate = avio_rb32(pb);

    if (!frame_usec)
        frame_usec = 33367; // ~29.97 fps fallback

    if (video_frames) {
        AVStream *vid = avformat_new_stream(ctx, NULL);
        if (!vid)
            return AVERROR(ENOMEM);
        /* Express the timebase as the frame duration derived from the frame
         * rate (e.g. 33367 us -> 30000/1001). Using the raw us/1000000 gives a
         * 1000000 denominator that downstream encoders (mpeg4) reject; a
         * reduced fps keeps pts in whole-frame units with a small denominator. */
        AVRational fps = av_d2q(1000000.0 / frame_usec, 100000);
        avpriv_set_pts_info(vid, 64, fps.den, fps.num);
        vid->avg_frame_rate = fps;
        vid->r_frame_rate   = fps;
        vid->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vid->codecpar->codec_id   = AV_CODEC_ID_HVQM4;
        vid->codecpar->codec_tag  = 0; // no FOURCC
        if ((ret = ff_alloc_extradata(vid->codecpar, 2)) < 0)
            return ret;
        vid->codecpar->extradata[0] = hsamp;
        vid->codecpar->extradata[1] = vsamp;
        vid->codecpar->width  = width;
        vid->codecpar->height = height;
        vid->nb_frames = video_frames;
        vid->duration  = video_frames;
        h4m->video_stream_index = vid->index;
    }

    if (audio_frames) {
        AVStream *aud = avformat_new_stream(ctx, NULL);
        if (!aud)
            return AVERROR(ENOMEM);
        avpriv_set_pts_info(aud, 64, 1, 1000000);
        aud->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        aud->codecpar->codec_id    = AV_CODEC_ID_NONE;
        av_channel_layout_default(&aud->codecpar->ch_layout, audio_channels);
        aud->codecpar->sample_rate = audio_sample_rate;
        h4m->audio_stream_index = aud->index;
    }

    h4m->gop_beginning_video_pts = 1; // one frame headroom

    return 0;
}

static int hvqm4_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret;
    Hvqm4DemuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    // are we expecting a new GOP?
    if (h4m->gop_video_index == h4m->gop.nb_video_frames &&
        h4m->gop_audio_index == h4m->gop.nb_audio_frames) {

        if (h4m->gop_index < h4m->file.nb_gops) {
            ++h4m->gop_index;

            h4m->gop_beginning_video_pts += h4m->gop.nb_video_frames;

            h4m->gop_start = avio_tell(pb);
            h4m->gop.prev_size = avio_rb32(pb);
            h4m->gop.next_size = avio_rb32(pb);
            h4m->gop.nb_video_frames = avio_rb32(pb);
            h4m->gop.nb_audio_frames = avio_rb32(pb);
            uint32_t unknown = avio_rb32(pb);
            if (unknown != 0x01000000)
                av_log(ctx, AV_LOG_WARNING, "unexpected value in GOP header\n");

            h4m->gop_video_index = 0;
            h4m->gop_audio_index = 0;
        } else {
            return AVERROR_EOF;
        }
    }

    if (h4m->gop_video_index < h4m->gop.nb_video_frames ||
        h4m->gop_audio_index < h4m->gop.nb_audio_frames) {

        // frame record: [media_type:2][frame_type:2][frame_size:4][disp_id:4][payload]
        uint16_t media_type = avio_rb16(pb);
        avio_rb16(pb); // frame_type (I/P/B) — captured into the packet below
        // rewind so the 2-byte frame type becomes the packet's first bytes
        avio_seek(pb, -2, SEEK_CUR);
        if ((ret = av_get_packet(pb, pkt, 2)) < 0)
            return ret;
        if (ret < 2) {
            av_packet_unref(pkt);
            return AVERROR(EIO);
        }
        uint32_t frame_size = avio_rb32(pb);
        uint32_t disp_id = avio_rb32(pb);
        // rewind so disp_id is included at the head of the payload (the decoder
        // reads frame_type at +0, disp_id/pts at +2, frame data at +6)
        avio_seek(pb, -4, SEEK_CUR);
        ret = av_append_packet(pb, pkt, frame_size);
        if (ret < 0 || (uint32_t)ret < frame_size) {
            av_packet_unref(pkt);
            return AVERROR(EIO);
        }

        if (media_type == 0) {
            pkt->dts = h4m->audio_dts++;
            ++h4m->gop_audio_index;
            pkt->stream_index = h4m->audio_stream_index;
        } else if (media_type == 1) {
            pkt->dts = h4m->video_dts++;
            ++h4m->gop_video_index;
            pkt->stream_index = h4m->video_stream_index;
        } else {
            av_packet_unref(pkt);
            av_log(ctx, AV_LOG_ERROR, "unknown media type\n");
            return AVERROR_INVALIDDATA;
        }
        pkt->pts = h4m->gop_beginning_video_pts + disp_id;
    }
    return 0;
}

static int hvqm4_read_seek(AVFormatContext *ctx, int stream_index,
                           int64_t timestamp, int flags)
{
    Hvqm4DemuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL))
        return -1;

    if (avio_seek(pb, h4m->gop_start, SEEK_SET) < 0)
        return -1;

    h4m->gop_index -= 1;
    h4m->gop_video_index = 0;
    h4m->gop_audio_index = 0;
    h4m->gop.nb_video_frames = 0;
    h4m->gop.nb_audio_frames = 0;

    return 0;
}

const FFInputFormat ff_hvqm4_demuxer = {
    .p.name         = "hvqm4",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Hudson HVQM4"),
    .p.extensions   = "h4m",
    .priv_data_size = sizeof(Hvqm4DemuxContext),
    .read_probe     = hvqm4_read_probe,
    .read_header    = hvqm4_read_header,
    .read_packet    = hvqm4_read_packet,
    .read_seek      = hvqm4_read_seek,
};
