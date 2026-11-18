/*
 * MIVF muxer
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
 * MIVF, the container of the MIVF 3DS homebrew movie player.
 *
 * A 64-byte header and one descriptor per stream, then one page per video
 * frame: "MP" and a 32-byte header (frame number, time in 1/30000 s,
 * payload size, packet count, CRC-32 of the payload), then the frame's
 * M2Y1 packet and, when there is sound, one audio packet holding exactly
 * rate / fps samples. That fixed ratio is why the audio is packed here:
 * the muxer takes 16-bit PCM and cuts it into those packets itself, as
 * IA4M ADPCM (mono) or PC16 PCM (mono or stereo). A seek-index footer
 * listing the keyframe pages ends the file.
 */

#include "libavutil/crc.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/rational.h"

#include "libavcodec/adpcm_data.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#define MIVF_HEADER_SIZE      64
#define MIVF_DESC_SIZE        48
#define MIVF_PAGE_HEADER_SIZE 32
#define MIVF_PKT_HEADER_SIZE  16
#define MIVF_TICKS            30000
#define MIVF_MAX_SEEK_POINTS  4096

#define IDX_MAGIC        0x314A4458
#define IDX_VERSION      2
#define IDX_FOOTER_MAGIC 0x5844494D

typedef struct MIVFMuxContext {
    const AVClass *class;
    int audio_codec;            ///< 0 IA4M, 1 PC16
    int write_index;

    int video_index, audio_index;
    int fps_num, fps_den;
    int rate, channels, spf;
    int header_written;

    AVPacket **vq;              ///< video packets waiting for their sound
    int nb_vq, vq_size;
    AVFifo *pcm;                ///< interleaved s16le samples not yet paged
    uint8_t *pcm_frame;         ///< one frame's worth of them
    unsigned pcm_frame_size;
    int64_t audio_next;         ///< sample the next audio packet should start at
    int audio_started;

    uint32_t frames;
    uint8_t *page;
    unsigned page_size;
    uint32_t *seek_frame;
    int64_t *seek_pos;
    int nb_seek;
} MIVFMuxContext;

static int mivf_init(AVFormatContext *s)
{
    MIVFMuxContext *m = s->priv_data;
    AVRational fps;

    m->video_index = m->audio_index = -1;
    for (int i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO && m->video_index < 0 &&
            par->codec_id == AV_CODEC_ID_M2Y1)
            m->video_index = i;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && m->audio_index < 0 &&
                 par->codec_id == AV_CODEC_ID_PCM_S16LE)
            m->audio_index = i;
        else {
            av_log(s, AV_LOG_ERROR, "MIVF takes one m2y1 video stream and "
                   "one pcm_s16le audio stream; stream %d is neither\n", i);
            return AVERROR(EINVAL);
        }
    }
    if (m->video_index < 0) {
        av_log(s, AV_LOG_ERROR, "MIVF needs an m2y1 video stream\n");
        return AVERROR(EINVAL);
    }

    {
        AVStream *st = s->streams[m->video_index];
        fps = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        if (!fps.num || !fps.den)
            fps = av_inv_q(st->time_base);
    }
    av_reduce(&m->fps_num, &m->fps_den, fps.num, fps.den, 0xFFFF);
    if (av_cmp_q(av_make_q(m->fps_num, m->fps_den), fps)) {
        av_log(s, AV_LOG_ERROR, "frame rate %d/%d does not fit MIVF's 16-bit "
               "fields exactly\n", fps.num, fps.den);
        return AVERROR(EINVAL);
    }

    if (m->audio_index >= 0) {
        AVCodecParameters *par = s->streams[m->audio_index]->codecpar;
        int64_t num = (int64_t)par->sample_rate * m->fps_den;

        m->rate     = par->sample_rate;
        m->channels = par->ch_layout.nb_channels;
        if (num % m->fps_num || num / m->fps_num > 0xFFFF || m->rate > 0xFFFF) {
            av_log(s, AV_LOG_ERROR, "%d Hz audio does not split into whole "
                   "%d/%d fps frames; 48000 Hz suits every film rate\n",
                   m->rate, m->fps_num, m->fps_den);
            return AVERROR(EINVAL);
        }
        m->spf = num / m->fps_num;
        if (m->channels < 1 || m->channels > 2 ||
            (m->audio_codec == 0 && m->channels != 1)) {
            av_log(s, AV_LOG_ERROR, "IA4M audio is mono and PC16 mono or "
                   "stereo; got %d channels\n", m->channels);
            return AVERROR(EINVAL);
        }
        m->pcm = av_fifo_alloc2(m->spf * m->channels * 4, 1, AV_FIFO_FLAG_AUTO_GROW);
        if (!m->pcm)
            return AVERROR(ENOMEM);
        avpriv_set_pts_info(s->streams[m->audio_index], 64, 1, m->rate);
    }
    avpriv_set_pts_info(s->streams[m->video_index], 64, m->fps_den, m->fps_num);

    m->seek_frame = av_malloc_array(MIVF_MAX_SEEK_POINTS, sizeof(*m->seek_frame));
    m->seek_pos   = av_malloc_array(MIVF_MAX_SEEK_POINTS, sizeof(*m->seek_pos));
    return m->seek_frame && m->seek_pos ? 0 : AVERROR(ENOMEM);
}

static int64_t frame_ticks(MIVFMuxContext *m, int64_t frame)
{
    return frame * MIVF_TICKS * m->fps_den / m->fps_num;
}

static void write_file_header(AVFormatContext *s, const uint8_t *codec)
{
    MIVFMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    AVCodecParameters *par = s->streams[m->video_index]->codecpar;
    const int streams = 1 + (m->audio_index >= 0);

    ffio_wfourcc(pb, "MIVF");
    avio_wl16(pb, 0);
    avio_wl16(pb, 12);
    avio_wl32(pb, MIVF_HEADER_SIZE);
    avio_wl32(pb, streams);
    avio_wl32(pb, MIVF_TICKS);
    avio_wl64(pb, 0);                       /* duration, patched at the end */
    avio_wl32(pb, streams);
    avio_wl32(pb, 4096);
    avio_wl64(pb, MIVF_HEADER_SIZE + streams * MIVF_DESC_SIZE);
    ffio_fill(pb, 0, MIVF_HEADER_SIZE - 44);

    /* Video: id 0, type 1; the frame rate is num/den at 0x14. */
    avio_w8(pb, 0);
    avio_w8(pb, 1);
    avio_wl16(pb, MIVF_DESC_SIZE);
    avio_write(pb, codec, 4);
    avio_wl32(pb, 1);
    avio_wl32(pb, (m->fps_num + m->fps_den / 2) / m->fps_den);
    avio_wl16(pb, par->width);
    avio_wl16(pb, par->height);
    avio_wl16(pb, m->fps_num);
    avio_wl16(pb, m->fps_den);
    avio_wl32(pb, 0);
    avio_wl16(pb, 0);
    avio_wl16(pb, 16);
    avio_write(pb, codec, 4);
    avio_wl16(pb, par->width);
    avio_wl16(pb, par->height);
    avio_wl32(pb, par->width * par->height);
    avio_wl32(pb, (par->width / 2) * (par->height / 2));

    if (m->audio_index >= 0) {
        /* Audio: id 1, type 2; rate, channels, samples per frame. */
        const char *tag = m->audio_codec ? "PC16" : "IA4M";

        avio_w8(pb, 1);
        avio_w8(pb, 2);
        avio_wl16(pb, MIVF_DESC_SIZE);
        ffio_wfourcc(pb, tag);
        avio_wl32(pb, 1);
        avio_wl32(pb, m->spf);
        avio_wl16(pb, m->rate);
        avio_wl16(pb, m->channels);
        avio_wl16(pb, m->spf);
        avio_wl16(pb, 1);
        avio_wl32(pb, 0);
        avio_wl16(pb, 0);
        avio_wl16(pb, 16);
        ffio_wfourcc(pb, tag);
        avio_wl32(pb, m->rate);
        avio_wl16(pb, m->channels);
        avio_wl16(pb, m->spf);
        avio_wl32(pb, 0);
    }
}

/** One frame's IA4M packet body; its step index starts at 0 every time. */
static int ia4m_pack(uint8_t *dst, const int16_t *pcm, int ns, uint32_t frame)
{
    int predictor = pcm[0], index = 0;
    const int nbytes = ns / 2;

    AV_WL32(dst, MKTAG('I', 'A', '4', 'M'));
    AV_WL32(dst + 4, frame);
    AV_WL16(dst + 8, ns);
    dst[10] = 1;
    dst[11] = 0;
    AV_WL16(dst + 12, predictor);
    dst[14] = 0;
    dst[15] = 0;
    AV_WL32(dst + 16, nbytes);
    memset(dst + 20, 0, nbytes);
    for (int i = 1; i < ns; i++) {
        const int step = ff_adpcm_step_table[index];
        int diff = pcm[i] - predictor, nibble = 0, delta = step >> 3;

        if (diff < 0) {
            nibble = 8;
            diff = -diff;
        }
        if (diff >= step) {
            nibble |= 4;
            diff  -= step;
            delta += step;
        }
        if (diff >= step >> 1) {
            nibble |= 2;
            diff  -= step >> 1;
            delta += step >> 1;
        }
        if (diff >= step >> 2) {
            nibble |= 1;
            delta += step >> 2;
        }
        predictor = av_clip_int16(nibble & 8 ? predictor - delta : predictor + delta);
        index     = av_clip(index + ff_adpcm_index_table[nibble], 0, 88);
        dst[20 + (i - 1) / 2] |= nibble << ((i - 1) & 1 ? 4 : 0);
    }
    return 20 + nbytes;
}

static void put_packet_header(uint8_t *p, int sid, int flags, uint32_t size,
                              uint32_t frame)
{
    p[0] = sid;
    p[1] = flags;
    AV_WL16(p + 2, MIVF_PKT_HEADER_SIZE);
    AV_WL32(p + 4, 0);
    AV_WL32(p + 8, size);
    AV_WL32(p + 12, frame);
}

/** Write the page of the oldest queued video frame, with its sound. */
static int write_page(AVFormatContext *s)
{
    MIVFMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    AVPacket *vpkt = m->vq[0];
    const int pcm_bytes = m->spf * m->channels * 2;
    const unsigned audio_max = m->audio_index >= 0 ?
        MIVF_PKT_HEADER_SIZE + FFMAX(pcm_bytes, 20 + m->spf / 2) : 0;
    const unsigned need = MIVF_PKT_HEADER_SIZE + vpkt->size + audio_max;
    const int64_t pos = avio_tell(pb);
    unsigned payload;
    uint8_t *p;

    if (vpkt->size < 28 || memcmp(vpkt->data, "M2Y", 3))
        return AVERROR_INVALIDDATA;
    if (!m->header_written) {
        write_file_header(s, vpkt->data);
        m->header_written = 1;
    }

    av_fast_malloc(&m->page, &m->page_size, need);
    if (!m->page)
        return AVERROR(ENOMEM);
    p = m->page;

    /* The encoder numbered its frames from its own start; a joined stream
     * would repeat them, so they take their place in the file instead. */
    put_packet_header(p, 0, 7, vpkt->size, m->frames);
    memcpy(p + MIVF_PKT_HEADER_SIZE, vpkt->data, vpkt->size);
    AV_WL32(p + MIVF_PKT_HEADER_SIZE + 8, m->frames);
    payload = MIVF_PKT_HEADER_SIZE + vpkt->size;

    if (m->audio_index >= 0) {
        uint8_t *a = p + payload + MIVF_PKT_HEADER_SIZE;
        const size_t have = FFMIN(av_fifo_can_read(m->pcm), pcm_bytes);
        int asize;

        /* Missing sound at the end is silence. */
        av_fast_malloc(&m->pcm_frame, &m->pcm_frame_size, pcm_bytes);
        if (!m->pcm_frame)
            return AVERROR(ENOMEM);
        av_fifo_read(m->pcm, m->pcm_frame, have);
        memset(m->pcm_frame + have, 0, pcm_bytes - have);
        if (m->audio_codec) {
            memcpy(a, m->pcm_frame, pcm_bytes);
            asize = pcm_bytes;
        } else {
            int16_t *pcm = (int16_t *)m->pcm_frame;
            for (int i = 0; i < m->spf; i++)
                pcm[i] = AV_RL16(m->pcm_frame + 2 * i);
            asize = ia4m_pack(a, pcm, m->spf, m->frames);
        }
        put_packet_header(p + payload, 1, 0, asize, m->frames);
        payload += MIVF_PKT_HEADER_SIZE + asize;
    }

    if ((vpkt->flags & AV_PKT_FLAG_KEY) && m->nb_seek < MIVF_MAX_SEEK_POINTS) {
        m->seek_frame[m->nb_seek] = m->frames;
        m->seek_pos[m->nb_seek++] = pos;
    }

    avio_write(pb, "MP", 2);
    avio_w8(pb, MIVF_PAGE_HEADER_SIZE);
    avio_w8(pb, 3);                         /* CRC present, keyframe flag */
    avio_wl32(pb, m->frames);
    avio_wl64(pb, frame_ticks(m, m->frames));
    avio_wl32(pb, payload);
    avio_wl16(pb, 1 + (m->audio_index >= 0));
    avio_wl16(pb, 0);
    avio_wl32(pb, av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), UINT32_MAX,
                         p, payload) ^ UINT32_MAX);
    avio_wl32(pb, 0);
    avio_write(pb, p, payload);

    av_packet_free(&m->vq[0]);
    memmove(m->vq, m->vq + 1, --m->nb_vq * sizeof(*m->vq));
    m->frames++;
    return 0;
}

/** Write every queued frame whose sound has arrived. Audio is interleaved
 *  within a packet or so of the video, so a frame still short of sound two
 *  seconds of video later has none coming, and is written with silence. */
static int flush_pages(AVFormatContext *s, int all)
{
    MIVFMuxContext *m = s->priv_data;
    const int lag = FFMAX(8, 2 * m->fps_num / m->fps_den);
    int ret;

    while (m->nb_vq) {
        if (m->audio_index >= 0 && !all && m->nb_vq <= lag &&
            av_fifo_can_read(m->pcm) < (size_t)m->spf * m->channels * 2)
            break;
        if ((ret = write_page(s)) < 0)
            return ret;
    }
    return 0;
}

static int mivf_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MIVFMuxContext *m = s->priv_data;

    if (pkt->stream_index == m->video_index) {
        AVPacket *copy;

        if (m->nb_vq >= m->vq_size) {
            int n = FFMAX(16, m->vq_size * 2);
            AVPacket **q = av_realloc_array(m->vq, n, sizeof(*q));
            if (!q)
                return AVERROR(ENOMEM);
            m->vq = q;
            m->vq_size = n;
        }
        if (!(copy = av_packet_clone(pkt)))
            return AVERROR(ENOMEM);
        m->vq[m->nb_vq++] = copy;
    } else {
        const int fs = m->channels * 2;
        const uint8_t *data = pkt->data;
        int size = pkt->size - pkt->size % fs;
        int ret;

        /* Line the sound up with the pictures: silence before a late
         * start, and nothing of what comes before the first frame. */
        if (pkt->pts != AV_NOPTS_VALUE) {
            int64_t start = av_rescale_q(pkt->pts, s->streams[pkt->stream_index]->time_base,
                                         av_make_q(1, m->rate));
            if (!m->audio_started)
                m->audio_next = 0;
            if (start > m->audio_next && start - m->audio_next < (int64_t)m->rate * 60) {
                static const uint8_t zero[256];
                for (int64_t n = (start - m->audio_next) * fs; n > 0; n -= sizeof(zero))
                    if ((ret = av_fifo_write(m->pcm, zero, FFMIN(n, sizeof(zero)))) < 0)
                        return ret;
                m->audio_next = start;
            } else if (start < m->audio_next) {
                int64_t drop = FFMIN((m->audio_next - start) * fs, size);
                data += drop;
                size -= drop;
            }
        }
        m->audio_started = 1;
        if (size > 0) {
            if ((ret = av_fifo_write(m->pcm, data, size)) < 0)
                return ret;
            m->audio_next += size / fs;
        }
    }
    return flush_pages(s, 0);
}

static int mivf_write_trailer(AVFormatContext *s)
{
    MIVFMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    if ((ret = flush_pages(s, 1)) < 0)
        return ret;
    if (!m->header_written)
        return AVERROR(EINVAL);

    if (m->write_index && m->nb_seek) {
        /* Seek index, in the layout the player's embedded-index reader
         * takes: the final file size, the first page, the frame count and
         * (frame, page offset) pairs; then a footer pointing at it. */
        const int64_t index_offset = avio_tell(pb);
        const int index_size = 28 + 16 * m->nb_seek;

        avio_wl32(pb, IDX_MAGIC);
        avio_wl32(pb, IDX_VERSION);
        avio_wl64(pb, index_offset + index_size + 32);
        avio_wl32(pb, MIVF_HEADER_SIZE + (1 + (m->audio_index >= 0)) * MIVF_DESC_SIZE);
        avio_wl32(pb, m->frames);
        avio_wl32(pb, m->nb_seek);
        for (int i = 0; i < m->nb_seek; i++) {
            avio_wl32(pb, m->seek_frame[i]);
            avio_wl32(pb, 0);
            avio_wl64(pb, m->seek_pos[i]);
        }
        avio_wl32(pb, IDX_FOOTER_MAGIC);
        avio_wl32(pb, 1);
        avio_wl64(pb, index_offset);
        avio_wl32(pb, index_size);
        avio_wl32(pb, IDX_MAGIC);
        avio_wl32(pb, IDX_VERSION);
        avio_wl32(pb, 0);
    }

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        int64_t end = avio_tell(pb);
        avio_seek(pb, 20, SEEK_SET);
        avio_wl64(pb, frame_ticks(m, m->frames));
        avio_seek(pb, end, SEEK_SET);
    }
    return 0;
}

static void mivf_deinit(AVFormatContext *s)
{
    MIVFMuxContext *m = s->priv_data;

    for (int i = 0; i < m->nb_vq; i++)
        av_packet_free(&m->vq[i]);
    av_freep(&m->vq);
    av_fifo_freep2(&m->pcm);
    av_freep(&m->page);
    av_freep(&m->pcm_frame);
    av_freep(&m->seek_frame);
    av_freep(&m->seek_pos);
}

#define OFFSET(x) offsetof(MIVFMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "mivf_audio", "how to store the sound", OFFSET(audio_codec), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, E, .unit = "audio" },
        { "ia4m", "IMA ADPCM, mono",           0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "audio" },
        { "pc16", "16-bit PCM, mono or stereo", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, E, .unit = "audio" },
    { "mivf_index", "end the file with a seek index", OFFSET(write_index), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, E },
    { NULL },
};

static const AVClass mivf_muxer_class = {
    .class_name = "mivf muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_mivf_muxer = {
    .p.name           = "mivf",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MIVF (3DS homebrew movie)"),
    .p.extensions     = "mivf",
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_M2Y1,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.priv_class     = &mivf_muxer_class,
    .priv_data_size   = sizeof(MIVFMuxContext),
    .init             = mivf_init,
    .write_packet     = mivf_write_packet,
    .write_trailer    = mivf_write_trailer,
    .deinit           = mivf_deinit,
};
