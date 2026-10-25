/*
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
 * MobiClip "mus1" audio-only .mo demuxer (Fisher-Price Pixter Multimedia).
 *
 * Layout, taken from the Pixter Multimedia console firmware's mus1 reader:
 *
 *   0   magic     "MOC2" or "MOC3"
 *   4   hdr_size  LE32
 *   8   tag       "mus1"
 *   12  LE32
 *   ... the stream parameters, at an offset inside the header region:
 *       LE16 sample rate     (8000, 16000 or 32000)
 *       LE16 channels        (must be 1)
 *       LE16 N               (title length in UTF-16 units)
 *       title                (((2 * N + 5) & ~3) - 2 bytes; the firmware adds
 *                             the terminating NUL itself)
 *       LE32 data size
 *       data                 (FastAudio blocks, 40 bytes -> 256 samples)
 *
 * The firmware reaches the parameters through its own stream position
 * bookkeeping, which is not visible in the header, so the demuxer locates
 * them by validating each candidate offset instead of assuming one.
 */

#include "libavutil/intreadwrite.h"
#include "libavcodec/codec_id.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define MUS1_BLOCK_BYTES   40
#define MUS1_BLOCK_SAMPLES 256
#define MUS1_PKT_BLOCKS    64

typedef struct Mus1Context {
    int64_t data_end;
    int64_t pts;
} Mus1Context;

/* Look for a valid parameter block in [buf, buf + size).  Returns the offset of
 * the data bytes, or a negative value. */
static int mus1_find_params(const uint8_t *buf, int size, int hdr_size,
                            int *rate, uint32_t *data_size)
{
    int last = FFMIN(16 + hdr_size + 32, size - 12);

    for (int p = 16; p <= last; p++) {
        int r = AV_RL16(buf + p);
        int n, str, need;

        if (r != 8000 && r != 16000 && r != 32000)
            continue;
        if (AV_RL16(buf + p + 2) != 1)
            continue;
        n = AV_RL16(buf + p + 4);
        if (n > 256)
            continue;
        str  = ((2 * n + 5) & ~3) - 2;
        need = p + 6 + str + 4;
        if (need > size)
            continue;
        *rate      = r;
        *data_size = AV_RL32(buf + p + 6 + str);
        if (!*data_size)
            continue;
        return need;
    }
    return -1;
}

static int mus1_probe(const AVProbeData *p)
{
    uint32_t magic, hdr_size;
    int rate;
    uint32_t data_size;

    if (p->buf_size < 32)
        return 0;
    magic = AV_RL32(p->buf);
    if (magic != MKTAG('M', 'O', 'C', '2') && magic != MKTAG('M', 'O', 'C', '3'))
        return 0;
    if (AV_RL32(p->buf + 8) != MKTAG('m', 'u', 's', '1'))
        return 0;
    hdr_size = AV_RL32(p->buf + 4);
    if (hdr_size < 16 || hdr_size > (1 << 20))
        return 0;
    if (mus1_find_params(p->buf, p->buf_size, hdr_size, &rate, &data_size) < 0)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int mus1_read_header(AVFormatContext *s)
{
    Mus1Context *m = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t buf[16 + (1 << 10) + 1024];
    uint32_t hdr_size, data_size;
    int64_t fsize;
    int rate, data_off, got;
    AVStream *st;

    got = avio_read(pb, buf, sizeof(buf));
    if (got < 32)
        return got < 0 ? got : AVERROR_INVALIDDATA;
    hdr_size = AV_RL32(buf + 4);
    if (hdr_size < 16 || hdr_size > (1 << 20))
        return AVERROR_INVALIDDATA;

    data_off = mus1_find_params(buf, got, hdr_size, &rate, &data_size);
    if (data_off < 0)
        return AVERROR_INVALIDDATA;

    fsize = avio_size(pb);
    if (fsize > 0 && data_size > fsize - data_off)
        data_size = fsize - data_off;
    m->data_end = data_off + data_size;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_FASTAUDIO;
    st->codecpar->sample_rate = rate;
    st->codecpar->block_align = MUS1_BLOCK_BYTES;
    av_channel_layout_default(&st->codecpar->ch_layout, 1);
    avpriv_set_pts_info(st, 64, 1, rate);
    st->duration = (int64_t)(data_size / MUS1_BLOCK_BYTES) * MUS1_BLOCK_SAMPLES;

    return avio_seek(pb, data_off, SEEK_SET) < 0 ? AVERROR(EIO) : 0;
}

static int mus1_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    Mus1Context *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t left = m->data_end - avio_tell(pb);
    int size, ret;

    if (left < MUS1_BLOCK_BYTES)
        return AVERROR_EOF;
    size = FFMIN(left, MUS1_PKT_BLOCKS * MUS1_BLOCK_BYTES);
    size -= size % MUS1_BLOCK_BYTES;

    ret = av_get_packet(pb, pkt, size);
    if (ret < 0)
        return ret;
    pkt->stream_index = 0;
    pkt->pts          = m->pts;
    pkt->duration     = (int64_t)(pkt->size / MUS1_BLOCK_BYTES) * MUS1_BLOCK_SAMPLES;
    m->pts           += pkt->duration;
    return 0;
}

const FFInputFormat ff_mobimus_demuxer = {
    .p.name         = "mobimus",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MobiClip mus1 audio (Pixter Multimedia)"),
    .p.extensions   = "mo",
    .priv_data_size = sizeof(Mus1Context),
    .read_probe     = mus1_probe,
    .read_header    = mus1_read_header,
    .read_packet    = mus1_read_packet,
};
