/*
 * RocketVideo (.rvid) muxer
 * Copyright (c) 2026 mobipeg
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

/*
 * Buffers every stored frame, then lays out the v5 container at trailer time:
 *   0x200 header, u32 frame-offset table, compressed-size table (u32/frame for
 *   16bpp, u16/frame for 8bpp), the unique frame blobs (exact duplicates share
 *   an offset), then the left and right PCM streams.
 */
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "mux.h"
#include "internal.h"

#define RVID_PAL_BYTES 0x200

typedef struct RVIDFrame {
    uint8_t *data;
    int      size;
    uint32_t hash;
    int64_t  offset;   /* rebased at trailer */
    int      dup_of;   /* index of first identical frame, or -1 */
} RVIDFrame;

typedef struct RVIDMuxContext {
    int bmp_mode, interlaced, compressed;
    int vres, width, have_video;

    RVIDFrame *frames;
    int nframes, cap;

    /* dedup hash table (indices into frames[], -1 empty) */
    int *htab;
    int  hsize;

    /* audio */
    int audio_16bit, channels, sample_rate, have_audio;
    uint8_t *left, *right;
    size_t   left_size, right_size, left_cap, right_cap;
} RVIDMuxContext;

static uint32_t fnv1a(const uint8_t *d, int n)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++)
        h = (h ^ d[i]) * 16777619u;
    return h;
}

static int rvid_init(AVFormatContext *s)
{
    RVIDMuxContext *m = s->priv_data;
    int i;
    m->htab = NULL;

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (par->codec_id != AV_CODEC_ID_RVID) {
                av_log(s, AV_LOG_ERROR, "rvid: video must be the rvid codec\n");
                return AVERROR(EINVAL);
            }
            if (par->extradata_size < 3) {
                av_log(s, AV_LOG_ERROR, "rvid: encoder extradata missing\n");
                return AVERROR(EINVAL);
            }
            m->bmp_mode   = par->extradata[0];
            m->interlaced = par->extradata[1];
            m->compressed = par->extradata[2];
            m->width      = par->width;
            m->vres       = m->interlaced ? par->height / 2 : par->height;
            m->have_video = 1;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            m->channels    = par->ch_layout.nb_channels;
            m->sample_rate = par->sample_rate;
            m->audio_16bit = (par->codec_id == AV_CODEC_ID_PCM_S16LE);
            m->have_audio  = 1;
        }
    }
    if (!m->have_video) {
        av_log(s, AV_LOG_ERROR, "rvid: no video stream\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int rvid_write_header(AVFormatContext *s)
{
    return 0;   /* everything is written at trailer time */
}

static int append(uint8_t **buf, size_t *size, size_t *cap,
                  const uint8_t *src, int n)
{
    if (*size + n > *cap) {
        size_t nc = FFMAX(*cap * 2, *size + n + 4096);
        uint8_t *nb = av_realloc(*buf, nc);
        if (!nb)
            return AVERROR(ENOMEM);
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *size, src, n);
    *size += n;
    return 0;
}

static int rvid_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    RVIDMuxContext *m = s->priv_data;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        RVIDFrame *f;
        if (m->nframes >= m->cap) {
            int nc = FFMAX(m->cap * 2, 1024);
            RVIDFrame *nf = av_realloc_array(m->frames, nc, sizeof(*nf));
            if (!nf)
                return AVERROR(ENOMEM);
            m->frames = nf; m->cap = nc;
        }
        f = &m->frames[m->nframes];
        f->size = pkt->size;
        f->data = av_malloc(pkt->size);
        if (!f->data)
            return AVERROR(ENOMEM);
        memcpy(f->data, pkt->data, pkt->size);
        f->hash = fnv1a(f->data, f->size);
        f->dup_of = -1;
        m->nframes++;
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        int bps = m->audio_16bit ? 2 : 1;
        if (m->channels <= 1) {
            return append(&m->left, &m->left_size, &m->left_cap,
                          pkt->data, pkt->size);
        } else {
            int n = pkt->size / (bps * 2), k, ret;
            for (k = 0; k < n; k++) {
                if ((ret = append(&m->left, &m->left_size, &m->left_cap,
                                  pkt->data + (2 * k) * bps, bps)) < 0)
                    return ret;
                if ((ret = append(&m->right, &m->right_size, &m->right_cap,
                                  pkt->data + (2 * k + 1) * bps, bps)) < 0)
                    return ret;
            }
        }
    }
    return 0;
}

static int fps_byte(AVFormatContext *s)
{
    AVRational fr = s->streams[0]->avg_frame_rate;
    double fps;
    int base, reduce;
    if (fr.num <= 0 || fr.den <= 0)
        fr = av_inv_q(s->streams[0]->time_base);
    fps = av_q2d(fr);
    if (fps <= 0)
        return 30;
    base = (int)(fps + 0.5);
    if (base < 1) base = 1;
    if (base > 127) base = 127;
    reduce = (base - fps) > 0.02;
    return base | (reduce ? 0x80 : 0);
}

static int rvid_write_trailer(AVFormatContext *s)
{
    RVIDMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int n = m->nframes, i, comp_entry, pal;
    int64_t frame_table_size, comp_table_size, frames_base, cursor;
    int64_t size_no_audio, snd_left_off, snd_right_off, comp_off;

    if (n == 0)
        return AVERROR(EINVAL);

    /* dedup: hash table, linear probing */
    m->hsize = 1;
    while (m->hsize < n * 2) m->hsize <<= 1;
    m->htab = av_malloc_array(m->hsize, sizeof(int));
    if (!m->htab)
        return AVERROR(ENOMEM);
    for (i = 0; i < m->hsize; i++) m->htab[i] = -1;

    pal = (m->bmp_mode == 0) ? RVID_PAL_BYTES : 0;
    comp_entry = (m->bmp_mode == 0) ? 2 : 4;
    frame_table_size = 4LL * n;
    if (m->compressed) {
        comp_table_size = (int64_t)comp_entry * n;
        comp_table_size += (-comp_table_size) & 3;   /* align to 4 */
    } else {
        comp_table_size = 0;
    }
    frames_base = 0x200 + frame_table_size + comp_table_size;

    cursor = 0;
    for (i = 0; i < n; i++) {
        RVIDFrame *f = &m->frames[i];
        unsigned h = f->hash & (m->hsize - 1);
        int found = -1;
        while (m->htab[h] != -1) {
            int j = m->htab[h];
            if (m->frames[j].hash == f->hash &&
                m->frames[j].size == f->size &&
                !memcmp(m->frames[j].data, f->data, f->size)) {
                found = j;
                break;
            }
            h = (h + 1) & (m->hsize - 1);
        }
        if (found >= 0) {
            f->dup_of = found;
            f->offset = m->frames[found].offset;
        } else {
            f->dup_of = -1;
            f->offset = cursor;
            cursor += f->size;
            m->htab[h] = i;
        }
    }
    size_no_audio = frames_base + cursor;

    snd_left_off  = m->left_size  ? size_no_audio                     : 0;
    snd_right_off = m->right_size ? size_no_audio + m->left_size      : 0;
    comp_off      = m->compressed ? 0x200 + frame_table_size          : 0;

    /* header */
    avio_wl32(pb, MKTAG('R', 'V', 'I', 'D'));
    avio_wl32(pb, 5);
    avio_wl32(pb, n);
    avio_w8(pb, fps_byte(s));
    avio_w8(pb, m->vres);
    avio_w8(pb, m->interlaced ? 1 : 0);
    avio_w8(pb, 0);                            /* dualScreen */
    avio_wl16(pb, m->have_audio ? m->sample_rate : 0);
    avio_w8(pb, m->audio_16bit ? 1 : 0);
    avio_w8(pb, m->bmp_mode);
    avio_wl32(pb, comp_off);
    avio_wl32(pb, snd_left_off);
    avio_wl32(pb, snd_right_off);
    ffio_fill(pb, 0, 0x200 - 0x20);            /* pad header to 0x200 */

    /* frame offset table */
    for (i = 0; i < n; i++)
        avio_wl32(pb, frames_base + m->frames[i].offset);

    /* compressed-size table (payload size, excludes 8bpp palette) */
    if (m->compressed) {
        int64_t written = 0;
        for (i = 0; i < n; i++) {
            int payload = m->frames[i].size - pal;
            if (m->bmp_mode == 0) avio_wl16(pb, payload);
            else                  avio_wl32(pb, payload);
            written += comp_entry;
        }
        ffio_fill(pb, 0, comp_table_size - written);
    }

    /* unique frame blobs, in first-seen order */
    for (i = 0; i < n; i++)
        if (m->frames[i].dup_of < 0)
            avio_write(pb, m->frames[i].data, m->frames[i].size);

    /* audio: left then right */
    if (m->left_size)
        avio_write(pb, m->left, m->left_size);
    if (m->right_size)
        avio_write(pb, m->right, m->right_size);

    return 0;
}

static void rvid_deinit(AVFormatContext *s)
{
    RVIDMuxContext *m = s->priv_data;
    int i;
    for (i = 0; i < m->nframes; i++)
        av_freep(&m->frames[i].data);
    av_freep(&m->frames);
    av_freep(&m->htab);
    av_freep(&m->left);
    av_freep(&m->right);
}

const FFOutputFormat ff_rvid_muxer = {
    .p.name         = "rvid",
    .p.long_name    = NULL_IF_CONFIG_SMALL("RocketVideo (RVID)"),
    .p.extensions   = "rvid",
    .priv_data_size = sizeof(RVIDMuxContext),
    .p.audio_codec  = AV_CODEC_ID_PCM_U8,
    .p.video_codec  = AV_CODEC_ID_RVID,
    .init           = rvid_init,
    .write_header   = rvid_write_header,
    .write_packet   = rvid_write_packet,
    .write_trailer  = rvid_write_trailer,
    .deinit         = rvid_deinit,
    .p.flags        = AVFMT_NOTIMESTAMPS,
};
