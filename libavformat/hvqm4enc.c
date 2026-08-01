/*
 * HVQM4 muxer
 * Copyright (c) 2026 quatric
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

/* Container layout mirrors libavformat/hvqm4.c (the demuxer) exactly:
 * a 16-byte magic, a fixed 0x44-byte file header, then one or more GOPs,
 * each holding a run of [media_type:2][frame_type:2][frame_size:4]
 * [disp_id:4][payload] records. Video-only, single GOP; nb_gops and the
 * per-GOP/file frame counts are patched in at write_trailer() once the
 * true count is known. */

#include "avformat.h"
#include "avio.h"
#include "mux.h"
#include "internal.h"

typedef struct Hvqm4MuxContext {
    int64_t file_header_pos;
    int64_t gop_header_pos;
    uint32_t nb_video_frames;
    uint32_t max_frame_size;
    uint32_t frame_usec;
} Hvqm4MuxContext;

static int hvqm4_write_header(AVFormatContext *ctx)
{
    Hvqm4MuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;
    AVStream *st;
    int i;

    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        av_log(ctx, AV_LOG_ERROR, "hvqm4: output must be seekable (frame counts are patched in at the end)\n");
        return AVERROR(EINVAL);
    }

    if (ctx->nb_streams != 1 || ctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
        ctx->streams[0]->codecpar->codec_id != AV_CODEC_ID_HVQM4) {
        av_log(ctx, AV_LOG_ERROR, "hvqm4: exactly one HVQM4 video stream is required (no audio yet)\n");
        return AVERROR(EINVAL);
    }
    st = ctx->streams[0];
    if (st->codecpar->extradata_size < 2) {
        av_log(ctx, AV_LOG_ERROR, "hvqm4: missing h_samp/v_samp extradata\n");
        return AVERROR_INVALIDDATA;
    }

    /* derive the container's frame_usec from whatever time_base the
     * encoding pipeline already gave this stream; don't override it here
     * (the container carries no explicit per-frame timestamp beyond
     * disp_id/frame order, so this only affects metadata consumers) */
    if (st->time_base.num > 0 && st->time_base.den > 0)
        h4m->frame_usec = (uint32_t)av_rescale_q(1, st->time_base, (AVRational){ 1, 1000000 });
    if (!h4m->frame_usec)
        h4m->frame_usec = 33367;

    avio_write(pb, "HVQM4 1.5", 9);
    for (i = 9; i < 16; i++)
        avio_w8(pb, 0);

    h4m->file_header_pos = avio_tell(pb);
    avio_wb32(pb, 0x44);       /* header_size */
    avio_wb32(pb, 0);          /* body_size, patched later */
    avio_wb32(pb, 1);          /* nb_gops (single GOP) */
    avio_wb32(pb, 0);          /* video_frames, patched later */
    avio_wb32(pb, 0);          /* audio_frames */
    avio_wb32(pb, h4m->frame_usec);
    avio_wb32(pb, 0);          /* max_frame_size, patched later */
    avio_wb32(pb, 0);          /* unknown */
    avio_wb32(pb, 0);          /* audio_frame_size */
    avio_wb16(pb, st->codecpar->width);
    avio_wb16(pb, st->codecpar->height);
    avio_w8(pb, st->codecpar->extradata[0]); /* h_samp */
    avio_w8(pb, st->codecpar->extradata[1]); /* v_samp */
    avio_w8(pb, 0);            /* video_mode */
    avio_w8(pb, 0);            /* unknown */
    avio_w8(pb, 0);            /* audio_channels */
    avio_w8(pb, 0);            /* audio_bitdepth */
    avio_wb16(pb, 0);          /* unknown */
    avio_wb32(pb, 0);          /* audio_sample_rate */

    h4m->gop_header_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* prev_size */
    avio_wb32(pb, 0);          /* next_size */
    avio_wb32(pb, 0);          /* nb_video_frames, patched later */
    avio_wb32(pb, 0);          /* nb_audio_frames */
    avio_wb32(pb, 0x01000000); /* unknown, matches the demuxer's sanity check */

    return 0;
}

static int hvqm4_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    Hvqm4MuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    avio_wb16(pb, 1);    /* media_type: video */
    avio_wb16(pb, pkt->flags & AV_PKT_FLAG_KEY ? 0x10 : 0x20);
    avio_wb32(pb, (uint32_t)pkt->size); /* disp_id + payload */
    avio_write(pb, pkt->data, pkt->size);

    h4m->nb_video_frames++;
    if ((uint32_t)pkt->size > h4m->max_frame_size)
        h4m->max_frame_size = (uint32_t)pkt->size;

    return 0;
}

static int hvqm4_write_trailer(AVFormatContext *ctx)
{
    Hvqm4MuxContext *h4m = ctx->priv_data;
    AVIOContext *pb = ctx->pb;
    int64_t end_pos = avio_tell(pb);

    avio_seek(pb, h4m->file_header_pos + 12, SEEK_SET);
    avio_wb32(pb, h4m->nb_video_frames);
    avio_seek(pb, h4m->file_header_pos + 24, SEEK_SET);
    avio_wb32(pb, h4m->max_frame_size);

    avio_seek(pb, h4m->gop_header_pos + 8, SEEK_SET);
    avio_wb32(pb, h4m->nb_video_frames);

    avio_seek(pb, end_pos, SEEK_SET);
    return 0;
}

const FFOutputFormat ff_hvqm4_muxer = {
    .p.name         = "hvqm4",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Hudson HVQM4"),
    .p.extensions   = "h4m",
    .priv_data_size = sizeof(Hvqm4MuxContext),
    .p.video_codec  = AV_CODEC_ID_HVQM4,
    .p.flags        = AVFMT_GLOBALHEADER,
    .write_header   = hvqm4_write_header,
    .write_packet   = hvqm4_write_packet,
    .write_trailer  = hvqm4_write_trailer,
};
