/*
 * FVDS (Metroid Prime Hunters: First Hunt) demuxer
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

/**
 * @file
 * The First Hunt demo stores its movies as "FVDS" files with a different
 * layout from the one in fvdec.c:
 *
 *   0x00  "FVDS"
 *   0x04  LE32 frame count
 *   0x08  LE32 width           (256)
 *   0x0c  LE32 height          (192)
 *   0x10  LE32 frame rate      (16.16 fixed point)
 *   0x14  LE32 sample rate     (32768)
 *   0x18  LE32 total data size (file size - 36 - 16)
 *   0x1c  LE32 largest frame data size
 *   0x20  LE32 unused
 *
 * followed by the frames, each with a 16 byte header (data size, offsets to
 * the previous and next frame header, frame index) and, inside the data size,
 *
 *   LE32 video size, video data, LE32 audio size, audio data
 *
 * Audio is FastAudio: 40 bytes decode to 256 samples.  A frame header with a
 * data size of 0 ends the file.
 */

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define FVFH_MAGIC       MKTAG('F', 'V', 'D', 'S')
#define FVFH_HEADER_SIZE 36
#define FVFH_AUDIO_BLOCK 40
#define FVFH_AUDIO_SAMPLES 256

typedef struct FVFHDemuxContext {
    int      nb_frames;
    int      frame;
    int      sample_rate;
    int64_t  audio_pts;
    AVPacket *audio;    /* audio of the frame whose video was just returned */
} FVFHDemuxContext;

/* The older FVDS layout (fvdec.c) keeps two 16 bit values where this one has
 * the width and the height as 32 bit values. */
static int fvfh_probe_layout(const uint8_t *buf, int size)
{
    uint32_t w, h, sr;

    if (size < FVFH_HEADER_SIZE || AV_RL32(buf) != FVFH_MAGIC)
        return 0;
    w  = AV_RL32(buf + 0x08);
    h  = AV_RL32(buf + 0x0c);
    sr = AV_RL32(buf + 0x14);
    return w && h && !(w & 7) && !(h & 7) && w <= 4096 && h <= 4096 &&
           sr >= 4000 && sr <= 96000;
}

static int fvfh_probe(const AVProbeData *p)
{
    return fvfh_probe_layout(p->buf, p->buf_size) ? AVPROBE_SCORE_MAX : 0;
}

static int fvfh_read_header(AVFormatContext *s)
{
    FVFHDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;
    uint32_t rate;

    if (avio_rl32(pb) != FVFH_MAGIC)
        return AVERROR_INVALIDDATA;

    c->nb_frames = avio_rl32(pb);
    vst = avformat_new_stream(s, NULL);
    ast = avformat_new_stream(s, NULL);
    if (!vst || !ast)
        return AVERROR(ENOMEM);

    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_FVFH;
    vst->codecpar->width      = avio_rl32(pb);
    vst->codecpar->height     = avio_rl32(pb);
    rate                      = avio_rl32(pb);
    c->sample_rate            = avio_rl32(pb);
    avio_skip(pb, 3 * 4);

    if (!rate || (vst->codecpar->width | vst->codecpar->height) & 7 ||
        vst->codecpar->width <= 0 || vst->codecpar->height <= 0 ||
        !c->sample_rate || c->nb_frames < 0)
        return AVERROR_INVALIDDATA;

    avpriv_set_pts_info(vst, 64, 65536, rate);
    vst->avg_frame_rate = av_make_q(rate, 65536);
    vst->nb_frames = vst->duration = c->nb_frames;

    ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_id    = AV_CODEC_ID_FASTAUDIO;
    ast->codecpar->sample_rate = c->sample_rate;
    ast->codecpar->codec_tag   = FVFH_MAGIC;   /* selects the DS FastAudio variant */
    ast->codecpar->block_align = FVFH_AUDIO_BLOCK;
    av_channel_layout_default(&ast->codecpar->ch_layout, 1);
    avpriv_set_pts_info(ast, 64, 1, c->sample_rate);

    return 0;
}

static int fvfh_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    FVFHDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t data_size, video_size, audio_size;
    int64_t end;
    int ret;

    if (c->audio) {
        av_packet_move_ref(pkt, c->audio);
        av_packet_free(&c->audio);
        return 0;
    }

    data_size = avio_rl32(pb);
    if (avio_feof(pb) || !data_size)
        return AVERROR_EOF;
    avio_skip(pb, 3 * 4);   /* seek offsets and frame index */
    end = avio_tell(pb) + data_size;

    video_size = avio_rl32(pb);
    if (data_size < 8 || video_size > data_size - 8)
        return AVERROR_INVALIDDATA;
    ret = av_get_packet(pb, pkt, video_size);
    if (ret < 0)
        return ret;
    pkt->stream_index = 0;
    pkt->pts = pkt->dts = c->frame;
    pkt->duration = 1;
    pkt->flags |= c->frame ? 0 : AV_PKT_FLAG_KEY;
    c->frame++;

    audio_size = avio_rl32(pb);
    if (audio_size && audio_size % FVFH_AUDIO_BLOCK == 0 &&
        audio_size <= end - avio_tell(pb)) {
        AVPacket *apkt = av_packet_alloc();

        if (!apkt)
            return AVERROR(ENOMEM);
        ret = av_get_packet(pb, apkt, audio_size);
        if (ret < 0) {
            av_packet_free(&apkt);
            return ret;
        }
        apkt->stream_index = 1;
        apkt->pts          = apkt->dts = c->audio_pts;
        apkt->duration     = audio_size / FVFH_AUDIO_BLOCK * FVFH_AUDIO_SAMPLES;
        c->audio_pts      += apkt->duration;
        c->audio           = apkt;
    }

    if (avio_seek(pb, end, SEEK_SET) < 0)
        return AVERROR(EIO);
    return 0;
}

static int fvfh_read_seek(AVFormatContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    FVFHDemuxContext *c = s->priv_data;

    /* every frame is predicted from the one before it */
    if (timestamp > 0)
        return AVERROR(ENOSYS);
    av_packet_free(&c->audio);
    c->frame     = 0;
    c->audio_pts = 0;
    return avio_seek(s->pb, FVFH_HEADER_SIZE, SEEK_SET) < 0 ? AVERROR(EIO) : 0;
}

static int fvfh_read_close(AVFormatContext *s)
{
    FVFHDemuxContext *c = s->priv_data;

    av_packet_free(&c->audio);
    return 0;
}

const FFInputFormat ff_fvfh_demuxer = {
    .p.name         = "fvfh",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FVDS (Metroid Prime Hunters: First Hunt)"),
    .p.extensions   = "fv",
    .p.flags        = AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK,
    .priv_data_size = sizeof(FVFHDemuxContext),
    .read_probe     = fvfh_probe,
    .read_header    = fvfh_read_header,
    .read_packet    = fvfh_read_packet,
    .read_seek      = fvfh_read_seek,
    .read_close     = fvfh_read_close,
};
