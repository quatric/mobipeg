/*
 * RocketVideo (.rvid) muxer
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

/*
 * Buffers every stored frame, then lays out the v5 container at trailer time:
 *   0x200 header, u32 frame-offset table, compressed-size table (u32/frame for
 *   16bpp, u16/frame for 8bpp), the unique frame blobs (exact duplicates share
 *   an offset), then the left and right PCM streams.
 *
 * What RocketVideoPlayer expects of the file, beyond the layout:
 *   - a frame offset is a multiple of four (its low two bits name the part of
 *     a split file), so every stored frame is padded to four bytes;
 *   - a dual screen video has two table entries per frame, top screen first;
 *   - the frame rate byte is a whole rate, with bit 7 set when the real rate
 *     is that times 1000/1001, or 0 for the DS's own 59.8261 Hz;
 *   - sound is signed PCM, 8 or 16 bit, at 32000 Hz at most.
 */
#include <math.h>

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
    int bmp_mode, interlaced, compressed, dual;
    int vres, width, have_video, video_stream;

    RVIDFrame *frames;
    int nframes, cap;

    /* dedup hash table (indices into frames[], -1 empty) */
    int *htab;
    int  hsize;

    /* audio */
    int audio_16bit, audio_flip, channels, sample_rate, have_audio;
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
            if (m->have_video) {
                av_log(s, AV_LOG_ERROR, "rvid: only one video stream is supported\n");
                return AVERROR(EINVAL);
            }
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
            m->dual       = par->extradata_size > 3 && par->extradata[3] == 1;
            m->width      = par->width;
            m->vres       = par->height / (m->dual ? 2 : 1) / (m->interlaced ? 2 : 1);
            if ((m->width != 240 && m->width != 256) ||
                m->vres < 1 || m->vres > 255 || m->bmp_mode > 2 ||
                par->height != m->vres * (m->dual ? 2 : 1) * (m->interlaced ? 2 : 1) ||
                (m->dual && m->width != 256))
                return AVERROR(EINVAL);
            m->video_stream = i;
            m->have_video = 1;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (m->have_audio ||
                (par->codec_id != AV_CODEC_ID_PCM_S16LE && par->codec_id != AV_CODEC_ID_PCM_S8 &&
                 par->codec_id != AV_CODEC_ID_PCM_U8) ||
                par->ch_layout.nb_channels < 1 || par->ch_layout.nb_channels > 2 ||
                par->sample_rate < 1 || par->sample_rate > 32000) {
                av_log(s, AV_LOG_ERROR,
                       "rvid: audio must be one mono/stereo PCM S16LE or S8 stream at "
                       "no more than 32000 Hz (the player refuses higher rates)\n");
                return AVERROR(EINVAL);
            }
            m->channels    = par->ch_layout.nb_channels;
            m->sample_rate = par->sample_rate;
            m->audio_16bit = (par->codec_id == AV_CODEC_ID_PCM_S16LE);
            /* the DS plays 8 bit sound as signed samples */
            m->audio_flip  = (par->codec_id == AV_CODEC_ID_PCM_U8);
            m->have_audio  = 1;
        } else {
            return AVERROR(EINVAL);
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

static int rvid_add_frame(RVIDMuxContext *m, const uint8_t *data, int size)
{
    const int padded = FFALIGN(size, 4);
    RVIDFrame *f;

    if (m->nframes >= m->cap) {
        int nc = FFMAX(m->cap * 2, 1024);
        RVIDFrame *nf = av_realloc_array(m->frames, nc, sizeof(*nf));
        if (!nf)
            return AVERROR(ENOMEM);
        m->frames = nf; m->cap = nc;
    }
    f = &m->frames[m->nframes];
    f->size = padded;
    f->data = av_mallocz(padded);
    if (!f->data)
        return AVERROR(ENOMEM);
    memcpy(f->data, data, size);
    f->hash = fnv1a(f->data, f->size);
    f->dup_of = -1;
    m->nframes++;
    return 0;
}

static int rvid_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    RVIDMuxContext *m = s->priv_data;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        int ret;

        if (m->dual) {
            /* the top screen's size, then both screens */
            unsigned first;

            if (pkt->size < 4 || (first = AV_RL32(pkt->data)) > pkt->size - 4)
                return AVERROR_INVALIDDATA;
            if ((ret = rvid_add_frame(m, pkt->data + 4, first)) < 0)
                return ret;
            return rvid_add_frame(m, pkt->data + 4 + first, pkt->size - 4 - first);
        }
        return rvid_add_frame(m, pkt->data, pkt->size);
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        const int bps = m->audio_16bit ? 2 : 1;
        const int n = pkt->size / (bps * m->channels);
        int ret;

        for (int k = 0; k < n; k++) {
            for (int ch = 0; ch < m->channels; ch++) {
                uint8_t sample[2];

                memcpy(sample, pkt->data + (m->channels * k + ch) * bps, bps);
                if (m->audio_flip)
                    sample[0] ^= 0x80;
                ret = ch ? append(&m->right, &m->right_size, &m->right_cap, sample, bps)
                         : append(&m->left,  &m->left_size,  &m->left_cap,  sample, bps);
                if (ret < 0)
                    return ret;
            }
        }
    }
    return 0;
}

/* The player counts frames against a multiple of the frame rate and reads one
 * second of sound per that many frames, so the byte holds a whole rate; bit 7
 * marks a rate 1000/1001 of it (23.976, 29.97, ...), which the player matches
 * by reading 1.001 seconds of sound instead. */
static int fps_byte(AVFormatContext *s)
{
    RVIDMuxContext *m = s->priv_data;
    AVStream *st = s->streams[m->video_stream];
    AVRational fr = st->avg_frame_rate;
    double fps;
    int base, reduce;
    if (fr.num <= 0 || fr.den <= 0)
        fr = st->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = av_inv_q(st->time_base);
    fps = av_q2d(fr);
    if (fps <= 0)
        return 30;
    if (fps > 59.80 && fps < 59.85)
        return 0;
    base = (int)(fps + 0.5);
    if (base < 1) base = 1;
    if (base > 127) base = 127;
    reduce = fabs(fps - base * 1000.0 / 1001.0) < fabs(fps - base);
    if (fabs(fps - (reduce ? base * 1000.0 / 1001.0 : base)) > 0.005)
        av_log(s, AV_LOG_WARNING,
               "rvid: %.3f fps is stored as %s%d fps; the video will play at that rate\n",
               fps, reduce ? "1000/1001 of " : "", base);
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
    if (m->dual && (n & 1))
        return AVERROR_INVALIDDATA;

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
    if (size_no_audio + m->left_size + m->right_size > UINT32_MAX) {
        av_log(s, AV_LOG_ERROR, "rvid: output exceeds the 32-bit offset range\n");
        return AVERROR(EINVAL);
    }
    if (m->compressed) {
        for (i = 0; i < n; i++) {
            int payload = m->frames[i].size - pal;
            if (payload < 0 || (m->bmp_mode == 0 && payload > UINT16_MAX)) {
                av_log(s, AV_LOG_ERROR, "rvid: frame %d size does not fit the size table\n", i);
                return AVERROR_INVALIDDATA;
            }
        }
    }

    snd_left_off  = m->left_size  ? size_no_audio                     : 0;
    snd_right_off = m->right_size ? size_no_audio + m->left_size      : 0;
    comp_off      = m->compressed ? 0x200 + frame_table_size          : 0;

    /* header */
    avio_wl32(pb, MKTAG('R', 'V', 'I', 'D'));
    avio_wl32(pb, 5);
    avio_wl32(pb, m->dual ? n / 2 : n);
    avio_w8(pb, fps_byte(s));
    avio_w8(pb, m->vres);
    avio_w8(pb, m->interlaced ? 1 : 0);
    avio_w8(pb, m->width == 240 ? 2 : m->dual); /* GBA / DS dual screen / DS */
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
    .p.audio_codec  = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec  = AV_CODEC_ID_RVID,
    .init           = rvid_init,
    .write_header   = rvid_write_header,
    .write_packet   = rvid_write_packet,
    .write_trailer  = rvid_write_trailer,
    .deinit         = rvid_deinit,
    .p.flags        = AVFMT_NOTIMESTAMPS,
};
