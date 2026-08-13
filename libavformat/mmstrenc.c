/*
 * ADS-era (Majesco) GBA Video .mmstr muxer
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
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

/**
 * @file
 * Writes the resource container that gbavideo.c reads: a uint32 total size, a
 * uint32 resource count, then one resource per stream, each a single uint32 of
 * size_in_words:28 | type:4 followed by its payload.
 *
 * Only the video resource (type 1) is written. Audio would need an encoder for
 * the cart's ADPCM, which does not exist yet; the demuxer is happy with a
 * video-only table.
 *
 * A video payload is three header words - chapter count and frame count, an
 * unused magic, and the chunk count - then the chunks back to back and a
 * uint32 terminator. Writing zero chapters means no title or chapter names
 * follow, which puts the first chunk at a fixed offset.
 *
 * Sizes are in words here, the unit the ADS-era carts use and the only one the
 * standalone .mmstr demuxer accepts. Nothing in the container records a frame
 * rate: the retail player recovers one by dividing the frame count by the
 * soundtrack's running time, so a video-only file has nothing to divide and
 * reads back at the demuxer's default. Pass -frame_rate to the demuxer to say
 * otherwise.
 *
 * The frame count, chunk count and resource size are only known once the last
 * packet is in, so the output has to be seekable.
 */

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#define T_VIDEO 1

typedef struct MMSTRMuxContext {
    int64_t res_pos;            /* the resource's size|type word */
    int64_t payload_pos;        /* first byte of the payload */
    int64_t nb_frames;
    int64_t nb_chunks;
} MMSTRMuxContext;

static int mmstr_init(AVFormatContext *s)
{
    if (s->nb_streams != 1 ||
        s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "exactly one video stream is required\n");
        return AVERROR(EINVAL);
    }
    if (s->streams[0]->codecpar->codec_id != AV_CODEC_ID_ADS_GBA) {
        av_log(s, AV_LOG_ERROR,
               "this container only carries the ads_gba codec\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int mmstr_write_header(AVFormatContext *s)
{
    MMSTRMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;

    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        av_log(s, AV_LOG_ERROR,
               "a seekable output is required: the frame and chunk counts are "
               "only known once the last packet has been written\n");
        return AVERROR(EINVAL);
    }

    avio_wl32(pb, 0);                   /* total size, patched in the trailer */
    avio_wl32(pb, 1);                   /* one resource */

    m->res_pos = avio_tell(pb);
    avio_wl32(pb, 0);                   /* size | type, patched */

    m->payload_pos = avio_tell(pb);
    avio_wl32(pb, 0);                   /* chapters and frame count, patched */
    avio_wl32(pb, 0);                   /* magic; the player never reads it */
    avio_wl32(pb, 0);                   /* chunk count, patched */

    return 0;
}

static int mmstr_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MMSTRMuxContext *m = s->priv_data;

    if (pkt->size < 8 || (pkt->size & 3)) {
        av_log(s, AV_LOG_ERROR,
               "chunk of %d bytes is not a whole number of words\n", pkt->size);
        return AVERROR(EINVAL);
    }

    /* The chunk states its own frame count, which is what the resource header
     * has to add up to. */
    m->nb_frames += AV_RL32(pkt->data) >> 16;
    m->nb_chunks++;

    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

static int mmstr_write_trailer(AVFormatContext *s)
{
    MMSTRMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t payload_size;

    avio_wl32(pb, 0);                   /* the payload's terminator word */

    payload_size = avio_tell(pb) - m->payload_pos;

    if (m->nb_frames > 0xFFFF) {
        av_log(s, AV_LOG_ERROR,
               "%"PRId64" frames overflows the resource header's 16-bit count\n",
               m->nb_frames);
        return AVERROR(EINVAL);
    }
    if (payload_size / 4 > 0x0FFFFFFF) {
        av_log(s, AV_LOG_ERROR, "resource is too large for its size field\n");
        return AVERROR(EINVAL);
    }

    avio_seek(pb, m->res_pos, SEEK_SET);
    avio_wl32(pb, (uint32_t)(payload_size / 4) | ((uint32_t)T_VIDEO << 28));

    /* Zero chapters in the low byte, so no title or chapter names follow. */
    avio_wl32(pb, (uint32_t)(m->nb_frames & 0xFFFF) << 8);
    avio_skip(pb, 4);                   /* the magic stays as written */
    avio_wl32(pb, (uint32_t)m->nb_chunks);

    /* The probe walks the table and insists it lands on total + 4. */
    avio_seek(pb, 0, SEEK_SET);
    avio_wl32(pb, (uint32_t)(payload_size + 8));

    return 0;
}

const FFOutputFormat ff_mmstr_muxer = {
    .p.name         = "mmstr",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ADS-era GBA Video (Majesco)"),
    .p.extensions   = "mmstr",
    .p.video_codec  = AV_CODEC_ID_ADS_GBA,
    .p.audio_codec  = AV_CODEC_ID_NONE,
    .p.flags        = AVFMT_NOTIMESTAMPS,
    .priv_data_size = sizeof(MMSTRMuxContext),
    .init           = mmstr_init,
    .write_header   = mmstr_write_header,
    .write_packet   = mmstr_write_packet,
    .write_trailer  = mmstr_write_trailer,
};
