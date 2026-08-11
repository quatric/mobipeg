/*
 * Caimans 2.2 GBA Video ROM demuxer
 * Copyright (c) 2026 the FFmpeg developers
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define VIDEO_OFFSET 0x2710
#define AUDIO_OFFSET 0x707720
#define VIDEO_RATE   16
#define AUDIO_RATE   10512
#define AUDIO_BLOCK  16
#define SAMPLES_FRAME (AUDIO_RATE / VIDEO_RATE)

typedef struct Caimans22DemuxContext {
    int video_idx, audio_idx;
    int frames, video_frame;
    int64_t video_pos, audio_pos, audio_pts, audio_samples;
} Caimans22DemuxContext;

static int caimans22_probe(const AVProbeData *p)
{
    if (p->buf_size < VIDEO_OFFSET + 12 || p->buf[0xb2] != 0x96)
        return 0;
    if (AV_RL32(p->buf + 0x213c) != 0x08707720 ||
        AV_RL32(p->buf + 0x2154) != 0x08002710)
        return 0;
    if (p->buf[VIDEO_OFFSET + 11] != 0x10 ||
        AV_RB24(p->buf + VIDEO_OFFSET + 1) < 22)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int caimans22_read_header(AVFormatContext *avctx)
{
    Caimans22DemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    AVStream *st;
    int64_t size = avio_size(pb), pos = VIDEO_OFFSET;
    uint8_t h[22];

    if (size <= AUDIO_OFFSET || avio_seek(pb, 0x213c, SEEK_SET) < 0 ||
        avio_rl32(pb) != 0x08707720 || avio_seek(pb, 0x2154, SEEK_SET) < 0 ||
        avio_rl32(pb) != 0x08002710)
        return AVERROR_INVALIDDATA;

    while (pos + sizeof(h) <= AUDIO_OFFSET) {
        int len;
        if (avio_seek(pb, pos, SEEK_SET) < 0 ||
            avio_read(pb, h, sizeof(h)) != sizeof(h) ||
            (h[11] != 0x10 && h[11] != 0x11))
            break;
        len = AV_RB24(h + 1);
        if (len < 22 || pos + len > AUDIO_OFFSET)
            return AVERROR_INVALIDDATA;
        s->frames++;
        pos += len + 8;
    }
    if (!s->frames || pos != AUDIO_OFFSET)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(avctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_CAIMANS22;
    st->codecpar->width      = 240;
    st->codecpar->height     = 160;
    avpriv_set_pts_info(st, 64, 1, VIDEO_RATE);
    st->avg_frame_rate = (AVRational){ VIDEO_RATE, 1 };
    st->duration = s->frames;
    s->video_idx = st->index;

    st = avformat_new_stream(avctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_CAIMANS22_AUDIO;
    st->codecpar->sample_rate = AUDIO_RATE;
    st->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    avpriv_set_pts_info(st, 64, 1, AUDIO_RATE);
    st->duration = (int64_t)s->frames * SAMPLES_FRAME;
    s->audio_idx = st->index;

    s->video_pos    = VIDEO_OFFSET;
    s->audio_pos    = AUDIO_OFFSET;
    s->audio_samples = (int64_t)s->frames * SAMPLES_FRAME;
    avctx->duration = av_rescale_q(s->frames, (AVRational){ 1, VIDEO_RATE },
                                   AV_TIME_BASE_Q);
    return 0;
}

static int read_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    Caimans22DemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    uint8_t h[12];
    int len, ret;

    if (avio_seek(pb, s->video_pos, SEEK_SET) < 0 ||
        avio_read(pb, h, sizeof(h)) != sizeof(h))
        return AVERROR_INVALIDDATA;
    len = AV_RB24(h + 1);
    if (len < 22 || s->video_pos + len > AUDIO_OFFSET)
        return AVERROR_INVALIDDATA;
    ret = av_get_packet(pb, pkt, len - sizeof(h));
    if (ret < 0)
        return ret;
    if (ret != len - sizeof(h))
        return AVERROR_EOF;
    if (av_grow_packet(pkt, sizeof(h)) < 0)
        return AVERROR(ENOMEM);
    memmove(pkt->data + sizeof(h), pkt->data, len - sizeof(h));
    memcpy(pkt->data, h, sizeof(h));
    pkt->stream_index = s->video_idx;
    pkt->pts = pkt->dts = s->video_frame;
    pkt->duration = 1;
    pkt->pos = s->video_pos;
    if (h[11] == 0x10)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->video_pos += len + 8;
    s->video_frame++;
    return 0;
}

static int read_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    Caimans22DemuxContext *s = avctx->priv_data;
    int64_t left = s->audio_samples - s->audio_pts;
    int samples = FFMIN(left, 32);
    int size = (samples + 1) / 2;
    int ret;

    if (avio_seek(avctx->pb, s->audio_pos, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    ret = av_get_packet(avctx->pb, pkt, size);
    if (ret != size)
        return ret < 0 ? ret : AVERROR_EOF;
    pkt->stream_index = s->audio_idx;
    pkt->pts = pkt->dts = s->audio_pts;
    pkt->duration = samples;
    pkt->pos = s->audio_pos;
    if (!s->audio_pts)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->audio_pos += size;
    s->audio_pts += samples;
    return 0;
}

static int caimans22_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    Caimans22DemuxContext *s = avctx->priv_data;

    if (s->video_frame >= s->frames && s->audio_pts >= s->audio_samples)
        return AVERROR_EOF;
    if (s->video_frame < s->frames &&
        (s->audio_pts >= s->audio_samples ||
         av_compare_ts(s->video_frame, (AVRational){ 1, VIDEO_RATE },
                       s->audio_pts, (AVRational){ 1, AUDIO_RATE }) <= 0))
        return read_video_packet(avctx, pkt);
    return read_audio_packet(avctx, pkt);
}

const FFInputFormat ff_caimans22_demuxer = {
    .p.name         = "caimans22",
    .p.long_name    = "Caimans 2.2 GBA Video ROM",
    .p.extensions   = "gba",
    .priv_data_size = sizeof(Caimans22DemuxContext),
    .read_probe     = caimans22_probe,
    .read_header    = caimans22_read_header,
    .read_packet    = caimans22_read_packet,
};
