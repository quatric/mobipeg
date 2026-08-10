/*
 * Nintendo DS DPG (nDs-mPeG) demuxer
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

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "demux.h"
#include "dpg.h"
#include "avio_internal.h"
#include "internal.h"

#define DPG_CHUNK 4096

typedef struct DPGDemuxContext {
    int64_t audio_pos, audio_end;
    int64_t video_pos, video_end;
    int audio_index, video_index;
} DPGDemuxContext;

static int dpg_probe(const AVProbeData *p)
{
    if (p->buf_size < DPG_HEADER_SIZE_V0)
        return 0;
    if (memcmp(p->buf, "DPG", 3) || p->buf[3] < '0' || p->buf[3] > '4')
        return 0;
    /* The four offsets and sizes must at least be inside a plausible file. */
    if (AV_RL32(p->buf + 20) < DPG_HEADER_SIZE_V0 ||
        AV_RL32(p->buf + 28) < DPG_HEADER_SIZE_V0)
        return 0;

    return AVPROBE_SCORE_MAX;
}

static int dpg_read_header(AVFormatContext *s)
{
    DPGDemuxContext *c = s->priv_data;
    uint8_t hdr[DPG_HEADER_SIZE_V4];
    AVStream *vst, *ast = NULL;
    uint32_t frames, fps_raw, rate, channels;
    uint32_t audio_off, audio_size, video_off, video_size;
    AVRational fps;
    int version, ret;

    if ((ret = ffio_read_size(s->pb, hdr, DPG_HEADER_SIZE_V0)) < 0)
        return ret;
    if (memcmp(hdr, "DPG", 3) || hdr[3] < '0' || hdr[3] > '4')
        return AVERROR_INVALIDDATA;
    version = hdr[3] - '0';

    frames     = AV_RL32(hdr +  4);
    fps_raw    = AV_RL32(hdr +  8);
    rate       = AV_RL32(hdr + 12);
    channels   = AV_RL32(hdr + 16);
    audio_off  = AV_RL32(hdr + 20);
    audio_size = AV_RL32(hdr + 24);
    video_off  = AV_RL32(hdr + 28);
    video_size = AV_RL32(hdr + 32);

    if (!video_size || video_off < DPG_HEADER_SIZE_V0)
        return AVERROR_INVALIDDATA;

    /* DPG2 onwards store 8.8 fixed point here so fractional rates fit; the
     * two older versions store whole frames per second. */
    if (version >= 2)
        fps = fps_raw ? (AVRational){ fps_raw, 256 } : (AVRational){ 15, 1 };
    else
        fps = (AVRational){ fps_raw ? fps_raw : 15, 1 };

    vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_MPEG1VIDEO;
    ffstream(vst)->need_parsing = AVSTREAM_PARSE_FULL;
    vst->avg_frame_rate       = fps;
    vst->start_time           = 0;
    vst->nb_frames            = frames;
    avpriv_set_pts_info(vst, 64, fps.den, fps.num);
    c->video_index = vst->index;

    if (audio_size && rate) {
        ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = AV_CODEC_ID_MP2;
        ast->codecpar->sample_rate = rate;
        ast->codecpar->ch_layout.nb_channels = channels ? channels : 2;
        ffstream(ast)->need_parsing = AVSTREAM_PARSE_FULL;
        ast->start_time            = 0;
        avpriv_set_pts_info(ast, 64, 1, rate);
        c->audio_index = ast->index;
    } else {
        c->audio_index = -1;
    }

    c->audio_pos = audio_off;
    c->audio_end = audio_off + (int64_t)audio_size;
    c->video_pos = video_off;
    c->video_end = video_off + (int64_t)video_size;

    av_log(s, AV_LOG_DEBUG, "DPG%d: %u frames, %d/%d fps\n",
           version, frames, fps.num, fps.den);

    return 0;
}

/* Audio and video are stored one after the other rather than multiplexed, so
 * reading means jumping between two cursors. Feeding them out in alternating
 * chunks keeps a player's demuxer queues balanced; the parsers handle framing,
 * since neither elementary stream carries packet boundaries. */
static int dpg_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DPGDemuxContext *c = s->priv_data;
    int audio_left = c->audio_index >= 0 && c->audio_pos < c->audio_end;
    int video_left = c->video_pos < c->video_end;
    int64_t *pos, end;
    int index, size, ret;

    if (audio_left && (!video_left ||
                       c->audio_pos - c->audio_end < c->video_pos - c->video_end)) {
        pos   = &c->audio_pos;
        end   = c->audio_end;
        index = c->audio_index;
    } else if (video_left) {
        pos   = &c->video_pos;
        end   = c->video_end;
        index = c->video_index;
    } else {
        return AVERROR_EOF;
    }

    if (avio_seek(s->pb, *pos, SEEK_SET) < 0)
        return AVERROR(EIO);

    size = FFMIN(DPG_CHUNK, end - *pos);
    if ((ret = av_get_packet(s->pb, pkt, size)) < 0)
        return ret;

    pkt->stream_index = index;
    *pos += ret;

    return 0;
}

const FFInputFormat ff_dpg_demuxer = {
    .p.name         = "dpg",
    .p.long_name    = NULL_IF_CONFIG_SMALL("DPG (Nintendo DS nDs-mPeG)"),
    .p.extensions   = "dpg",
    .priv_data_size = sizeof(DPGDemuxContext),
    .read_probe     = dpg_probe,
    .read_header    = dpg_read_header,
    .read_packet    = dpg_read_packet,
};
