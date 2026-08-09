/*
 * Nintendo Pokemon GBA FVMV demuxer
 * Copyright (c) 2026 the FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define FVMV_HEADER_SIZE 0x20
#define FVMV_IWRAM_SIZE  0x8000
#define FVMV_FPS         8
#define FVMV_AUDIO_RATE  65536
#define FVMV_AUDIO_IN    40
#define FVMV_AUDIO_OUT   1024

static const uint8_t decoder_sig[] = {
    0xf0, 0x5f, 0x2d, 0xe9, 0x02, 0x81, 0xa0, 0xe3,
    0x41, 0x9e, 0x4f, 0xe2,
};

typedef struct FVMVStream {
    int64_t off, first, end;
    uint32_t frames, width, height, clock, size;
} FVMVStream;

typedef struct FVMVContext {
    const AVClass *class;
    const char *resource;
    FVMVStream stream;
    int video_idx, audio_idx;
    int frame, pending_audio;
    int64_t packet_pos, audio_pts;
    AVPacket *audio_pkt;
} FVMVContext;

static int parse_stream(FVMVStream *st, const uint8_t *rom, int64_t size,
                        int64_t off)
{
    if (off < 0 || off + FVMV_HEADER_SIZE > size ||
        memcmp(rom + off, "FVMV", 4))
        return 0;
    st->off    = off;
    st->frames = AV_RL32(rom + off + 4);
    st->width  = AV_RL32(rom + off + 8);
    st->height = AV_RL32(rom + off + 12);
    st->clock  = AV_RL32(rom + off + 16);
    st->size   = AV_RL32(rom + off + 20);
    st->first  = off + FVMV_HEADER_SIZE;
    st->end    = st->first + st->size;
    return st->frames > 0 && st->frames < 100000 &&
           st->width == 240 && st->height == 160 &&
           st->size >= 16 && st->end <= size;
}

static int fvmv_probe(const AVProbeData *p)
{
    if (p->buf_size < 0xc0 || p->buf[0xb2] != 0x96)
        return 0;
    for (int i = 0; i + FVMV_HEADER_SIZE <= p->buf_size; i += 4) {
        FVMVStream st;
        if (parse_stream(&st, p->buf, p->buf_size, i))
            return AVPROBE_SCORE_MAX;
    }
    return 0;
}

static int fvmv_read_header(AVFormatContext *avctx)
{
    FVMVContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t size = avio_size(pb), decoder = -1;
    FVMVStream best = { 0 };
    uint8_t *rom;
    int want = s->resource ? atoi(s->resource) : -1;
    int found = 0, ret = AVERROR_INVALIDDATA;

    if (size <= 0 || size > (1 << 27))
        return AVERROR_INVALIDDATA;
    rom = av_malloc(size);
    if (!rom)
        return AVERROR(ENOMEM);
    if (avio_seek(pb, 0, SEEK_SET) < 0 || avio_read(pb, rom, size) != size)
        goto out;

    for (int64_t off = 0; off + FVMV_HEADER_SIZE <= size; off += 4) {
        FVMVStream st;
        if (!parse_stream(&st, rom, size, off))
            continue;
        av_log(avctx, AV_LOG_VERBOSE,
               "[%d] FVMV at 0x%"PRIx64", %u frames, %ux%u\n",
               found, off, st.frames, st.width, st.height);
        if ((want >= 0 && found == want) ||
            (want < 0 && st.frames > best.frames))
            best = st;
        found++;
        off = st.end - 4;
    }
    if (!best.frames)
        goto out;

    for (int64_t off = 0; off + sizeof(decoder_sig) <= size; off++) {
        if (!memcmp(rom + off, decoder_sig, sizeof(decoder_sig))) {
            decoder = off - 0x5fe8;
            break;
        }
    }
    if (decoder < 0 || decoder + FVMV_IWRAM_SIZE > size)
        goto out;

    for (int audio = 0; audio < 2; audio++) {
        AVStream *st = avformat_new_stream(avctx, NULL);
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        ret = ff_alloc_extradata(st->codecpar, FVMV_IWRAM_SIZE);
        if (ret < 0)
            goto out;
        memcpy(st->codecpar->extradata, rom + decoder, FVMV_IWRAM_SIZE);
        if (!audio) {
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id   = AV_CODEC_ID_FVMV;
            st->codecpar->width      = best.width;
            st->codecpar->height     = best.height;
            avpriv_set_pts_info(st, 64, 1, FVMV_FPS);
            st->avg_frame_rate = (AVRational){ FVMV_FPS, 1 };
            st->duration = best.frames;
            s->video_idx = st->index;
        } else {
            st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id    = AV_CODEC_ID_FVMV_AUDIO;
            st->codecpar->sample_rate = FVMV_AUDIO_RATE;
            st->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
            avpriv_set_pts_info(st, 64, 1, FVMV_AUDIO_RATE);
            s->audio_idx = st->index;
        }
    }
    s->stream = best;
    s->packet_pos = best.first;
    s->audio_pkt = av_packet_alloc();
    if (!s->audio_pkt) {
        ret = AVERROR(ENOMEM);
        goto out;
    }
    ret = 0;
out:
    av_free(rom);
    return ret;
}

static int fvmv_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    FVMVContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    uint8_t h[20], ah[8];
    uint32_t total, video_region, audio_size;
    int64_t audio_header, next;
    int ret;

    if (s->pending_audio) {
        av_packet_move_ref(pkt, s->audio_pkt);
        s->pending_audio = 0;
        return 0;
    }
    if (s->frame >= s->stream.frames)
        return AVERROR_EOF;
    if (s->packet_pos + sizeof(h) > s->stream.end ||
        avio_seek(pb, s->packet_pos, SEEK_SET) < 0 ||
        avio_read(pb, h, sizeof(h)) != sizeof(h))
        return AVERROR_INVALIDDATA;
    total        = AV_RL32(h);
    video_region = AV_RL32(h + 16);
    next         = s->packet_pos + 16 + total;
    audio_header = s->packet_pos + 16 + video_region;
    if (video_region < 12 || next > s->stream.end ||
        audio_header + 8 > next || avio_seek(pb, audio_header, SEEK_SET) < 0 ||
        avio_read(pb, ah, sizeof(ah)) != sizeof(ah))
        return AVERROR_INVALIDDATA;
    audio_size = AV_RL32(ah + 4);
    if (audio_size % FVMV_AUDIO_IN || audio_header + 8 + audio_size > next)
        return AVERROR_INVALIDDATA;

    /* The ARM bitreader can consume up to four bytes past the nominal
     * compressed video region, so retain the first audio-header word. */
    ret = av_new_packet(pkt, video_region);
    if (ret < 0)
        return ret;
    if (avio_seek(pb, s->packet_pos + 20, SEEK_SET) < 0 ||
        avio_read(pb, pkt->data, pkt->size) != pkt->size)
        return AVERROR_INVALIDDATA;
    pkt->stream_index = s->video_idx;
    pkt->pos = s->packet_pos + 20;
    pkt->pts = pkt->dts = s->frame;
    pkt->duration = 1;
    if (!s->frame)
        pkt->flags |= AV_PKT_FLAG_KEY;

    ret = av_new_packet(s->audio_pkt, audio_size);
    if (ret < 0)
        return ret;
    if (avio_seek(pb, audio_header + 8, SEEK_SET) < 0 ||
        avio_read(pb, s->audio_pkt->data, audio_size) != audio_size)
        return AVERROR_INVALIDDATA;
    s->audio_pkt->stream_index = s->audio_idx;
    s->audio_pkt->pos = audio_header + 8;
    s->audio_pkt->pts = s->audio_pts;
    s->audio_pkt->duration = (int64_t)audio_size / FVMV_AUDIO_IN * FVMV_AUDIO_OUT;
    if (!s->frame)
        s->audio_pkt->flags |= AV_PKT_FLAG_KEY;
    s->audio_pts += s->audio_pkt->duration;
    s->pending_audio = 1;
    s->packet_pos = next;
    s->frame++;
    return 0;
}

static int fvmv_read_close(AVFormatContext *avctx)
{
    FVMVContext *s = avctx->priv_data;
    av_packet_free(&s->audio_pkt);
    return 0;
}

#define OFFSET(x) offsetof(FVMVContext, x)
static const AVOption fvmv_options[] = {
    { "resource", "index of the episode to demux, default the longest",
      OFFSET(resource), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0,
      AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass fvmv_class = {
    .class_name = "FVMV demuxer",
    .item_name  = av_default_item_name,
    .option     = fvmv_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_fvmv_demuxer = {
    .p.name         = "fvmv",
    .p.long_name    = "Nintendo Pokemon GBA FVMV ROM",
    .p.extensions   = "gba",
    .p.priv_class   = &fvmv_class,
    .priv_data_size = sizeof(FVMVContext),
    .read_probe     = fvmv_probe,
    .read_header    = fvmv_read_header,
    .read_packet    = fvmv_read_packet,
    .read_close     = fvmv_read_close,
};
