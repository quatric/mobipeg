/*
 * MIVF demuxer
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
 * MIVF demuxer: see mivfenc.c for the layout.
 */

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define MIVF_HEADER_SIZE      64
#define MIVF_PAGE_HEADER_SIZE 32
#define MIVF_PKT_HEADER_SIZE  16
#define MIVF_TICKS            30000

#define IDX_MAGIC        0x314A4458
#define IDX_FOOTER_MAGIC 0x5844494D

typedef struct MIVFDemuxContext {
    int video_sid, audio_sid;
    int video_index, audio_index;
    int spf;
    int64_t first;
    uint8_t *page;
    unsigned page_size, page_len, page_off;
    int64_t page_pos;
    uint32_t page_frame;
    int64_t page_pts;
} MIVFDemuxContext;

static int mivf_probe(const AVProbeData *p)
{
    if (p->buf_size < MIVF_HEADER_SIZE || AV_RL32(p->buf) != MKTAG('M', 'I', 'V', 'F') ||
        AV_RL32(p->buf + 8) != MIVF_HEADER_SIZE)
        return 0;
    return AVPROBE_SCORE_MAX;
}

/** Fill the index from the seek-index footer, if the file ends with one. */
static void read_index(AVFormatContext *s, AVStream *st)
{
    AVIOContext *pb = s->pb;
    int64_t size = avio_size(pb), offset, here = avio_tell(pb);
    uint32_t index_size, count, first;
    uint8_t f[32];

    if (size < 32 + 28 || !(pb->seekable & AVIO_SEEKABLE_NORMAL))
        return;
    if (avio_seek(pb, size - 32, SEEK_SET) < 0 || avio_read(pb, f, 32) != 32 ||
        AV_RL32(f) != IDX_FOOTER_MAGIC || AV_RL32(f + 20) != IDX_MAGIC)
        goto done;
    offset     = AV_RL64(f + 8);
    index_size = AV_RL32(f + 16);
    if (offset < MIVF_HEADER_SIZE || offset + index_size > size - 32 || index_size < 28 ||
        avio_seek(pb, offset, SEEK_SET) < 0 || avio_rl32(pb) != IDX_MAGIC)
        goto done;
    avio_skip(pb, 4 + 8);
    first = avio_rl32(pb);
    avio_skip(pb, 4);                       /* frame count */
    count = avio_rl32(pb);
    if (first < MIVF_HEADER_SIZE || count > (index_size - 28) / 16)
        goto done;
    for (uint32_t i = 0; i < count && !avio_feof(pb); i++) {
        uint32_t frame = avio_rl32(pb);
        int64_t pos;
        avio_skip(pb, 4);
        pos = avio_rl64(pb);
        if (pos >= first && pos < offset)
            av_add_index_entry(st, pos, av_rescale_q(frame, av_inv_q(st->avg_frame_rate),
                                                     st->time_base),
                               0, 0, AVINDEX_KEYFRAME);
    }
done:
    avio_seek(pb, here, SEEK_SET);
}

static int mivf_read_header(AVFormatContext *s)
{
    MIVFDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t h[MIVF_HEADER_SIZE];
    uint32_t streams;
    int64_t pos;

    if (avio_read(pb, h, MIVF_HEADER_SIZE) != MIVF_HEADER_SIZE)
        return AVERROR_INVALIDDATA;
    streams  = AV_RL32(h + 12);
    m->first = AV_RL64(h + 36);
    if (!streams || streams > 8 || m->first < MIVF_HEADER_SIZE || m->first > 4096)
        return AVERROR_INVALIDDATA;

    m->video_sid = m->audio_sid = -1;
    m->video_index = m->audio_index = -1;
    pos = MIVF_HEADER_SIZE;
    for (uint32_t i = 0; i < streams && pos + 32 <= m->first; i++) {
        uint8_t d[32];
        int dsize;
        AVStream *st;

        avio_seek(pb, pos, SEEK_SET);
        if (avio_read(pb, d, 32) != 32)
            return AVERROR_INVALIDDATA;
        dsize = AV_RL16(d + 2);
        if (dsize < 32)
            return AVERROR_INVALIDDATA;
        pos += dsize;

        if (d[1] == 1 && m->video_index < 0 &&
            (!memcmp(d + 4, "M2Y1", 4) || !memcmp(d + 4, "M2Y2", 4))) {
            int num = AV_RL16(d + 20), den = AV_RL16(d + 22);

            if (!num || !den) {
                num = AV_RL32(d + 12);
                den = 1;
            }
            if (!num)
                return AVERROR_INVALIDDATA;
            if (!(st = avformat_new_stream(s, NULL)))
                return AVERROR(ENOMEM);
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id   = AV_CODEC_ID_M2Y1;
            st->codecpar->codec_tag  = AV_RL32(d + 4);
            st->codecpar->width      = AV_RL16(d + 16);
            st->codecpar->height     = AV_RL16(d + 18);
            st->avg_frame_rate = st->r_frame_rate = av_make_q(num, den);
            avpriv_set_pts_info(st, 64, 1, MIVF_TICKS);
            m->video_sid   = d[0];
            m->video_index = st->index;
        } else if (d[1] == 2 && m->audio_index < 0 &&
                   (!memcmp(d + 4, "IA4M", 4) || !memcmp(d + 4, "PC16", 4))) {
            int rate = AV_RL16(d + 16), channels = AV_RL16(d + 18);

            m->spf = AV_RL16(d + 20);
            if (rate < 1000 || channels < 1 || channels > 2 || !m->spf)
                continue;
            if (!(st = avformat_new_stream(s, NULL)))
                return AVERROR(ENOMEM);
            st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id    = d[4] == 'I' ? AV_CODEC_ID_ADPCM_IMA_MIVF
                                                    : AV_CODEC_ID_PCM_S16LE;
            st->codecpar->sample_rate = rate;
            av_channel_layout_default(&st->codecpar->ch_layout, channels);
            st->codecpar->frame_size  = m->spf;
            avpriv_set_pts_info(st, 64, 1, rate);
            m->audio_sid   = d[0];
            m->audio_index = st->index;
        }
    }
    if (m->video_index < 0)
        return AVERROR_INVALIDDATA;

    {
        AVStream *st = s->streams[m->video_index];
        uint64_t duration = AV_RL64(h + 20);
        if (duration)
            st->duration = duration;
        read_index(s, st);
    }
    return avio_seek(pb, m->first, SEEK_SET) < 0 ? AVERROR(EIO) : 0;
}

static int mivf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MIVFDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    for (;;) {
        if (m->page_off + MIVF_PKT_HEADER_SIZE <= m->page_len) {
            const uint8_t *ph = m->page + m->page_off;
            const unsigned hsize = AV_RL16(ph + 2), psize = AV_RL32(ph + 8);
            const uint8_t *body = ph + hsize;

            if (hsize < MIVF_PKT_HEADER_SIZE || psize > m->page_len - m->page_off - hsize) {
                m->page_off = m->page_len;
                continue;
            }
            m->page_off += hsize + psize;

            if (ph[0] == m->video_sid && m->video_index >= 0 && psize >= 28) {
                if ((ret = av_new_packet(pkt, psize)) < 0)
                    return ret;
                memcpy(pkt->data, body, psize);
                pkt->stream_index = m->video_index;
                pkt->pts = pkt->dts = m->page_pts;
                pkt->pos = m->page_pos;
                if (body[12] == 1)
                    pkt->flags |= AV_PKT_FLAG_KEY;
                return 0;
            }
            if (ph[0] == m->audio_sid && m->audio_index >= 0 && psize) {
                if ((ret = av_new_packet(pkt, psize)) < 0)
                    return ret;
                memcpy(pkt->data, body, psize);
                pkt->stream_index = m->audio_index;
                pkt->pts = pkt->dts = (int64_t)m->page_frame * m->spf;
                pkt->duration = m->spf;
                pkt->pos = m->page_pos;
                pkt->flags |= AV_PKT_FLAG_KEY;
                return 0;
            }
            continue;
        }

        /* Next page; anything else (the seek index) ends the stream. */
        {
            uint8_t h[MIVF_PAGE_HEADER_SIZE];
            unsigned payload;

            m->page_pos = avio_tell(pb);
            if (avio_read(pb, h, MIVF_PAGE_HEADER_SIZE) != MIVF_PAGE_HEADER_SIZE ||
                h[0] != 'M' || h[1] != 'P')
                return AVERROR_EOF;
            if (h[2] > MIVF_PAGE_HEADER_SIZE)
                avio_skip(pb, h[2] - MIVF_PAGE_HEADER_SIZE);
            payload = AV_RL32(h + 16);
            if (payload > 64 << 20)
                return AVERROR_INVALIDDATA;
            av_fast_malloc(&m->page, &m->page_size, payload);
            if (!m->page)
                return AVERROR(ENOMEM);
            if ((ret = avio_read(pb, m->page, payload)) != payload)
                return ret < 0 ? ret : AVERROR_EOF;
            m->page_len   = payload;
            m->page_off   = 0;
            m->page_frame = AV_RL32(h + 4);
            m->page_pts   = AV_RL64(h + 8);
        }
    }
}

static int mivf_read_seek(AVFormatContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    MIVFDemuxContext *m = s->priv_data;
    AVStream *st = s->streams[m->video_index];
    int idx;

    if (stream_index != m->video_index)
        timestamp = av_rescale_q(timestamp, s->streams[stream_index]->time_base,
                                 st->time_base);
    idx = av_index_search_timestamp(st, timestamp, flags | AVSEEK_FLAG_BACKWARD);
    if (idx < 0)
        return -1;
    if (avio_seek(s->pb, ffstream(st)->index_entries[idx].pos, SEEK_SET) < 0)
        return -1;
    m->page_len = m->page_off = 0;
    return 0;
}

static int mivf_read_close(AVFormatContext *s)
{
    MIVFDemuxContext *m = s->priv_data;
    av_freep(&m->page);
    return 0;
}

const FFInputFormat ff_mivf_demuxer = {
    .p.name         = "mivf",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MIVF (3DS homebrew movie)"),
    .p.extensions   = "mivf",
    .priv_data_size = sizeof(MIVFDemuxContext),
    .read_probe     = mivf_probe,
    .read_header    = mivf_read_header,
    .read_packet    = mivf_read_packet,
    .read_seek      = mivf_read_seek,
    .read_close     = mivf_read_close,
};
