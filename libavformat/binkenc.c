/*
 * Bink 1 (BIKi) muxer
 * Copyright (C) 2026 Justin Marshall
 *
 * Container layout follows the binkenc encoder of LibBink.
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
 * Bink 1 muxer.
 *
 * A frame is the audio packet of the (single) audio track, prefixed by its
 * size, followed by the video packet. Audio packets each start with their
 * decoded size in bytes; every packet that ends by the end of a video frame
 * is merged into that frame, under one summed size word. The frame index
 * goes in front of the frames, so the frames are written first and moved
 * along when the file is finished.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/rational.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#define BINK_AUD_16BITS 0x4000
#define BINK_AUD_STEREO 0x2000
#define BINK_AUD_USEDCT 0x1000

typedef struct BinkFrame {
    uint32_t size;
    uint8_t key;
} BinkFrame;

typedef struct BinkMuxContext {
    int video_idx, audio_idx;
    AVRational fps;
    int sample_rate, channels;

    BinkFrame *frames;
    unsigned nb_frames, frames_alloc;
    uint32_t largest_frame;
    uint32_t max_audio_bytes;
    int64_t payload;

    AVPacket **vq;              ///< video packets waiting for their audio
    unsigned nb_vq, vq_alloc;
    AVPacket **aq;              ///< audio packets not yet written
    unsigned nb_aq, aq_alloc;
    int64_t audio_end;          ///< end of the latest audio packet, in samples
    int64_t audio_written;      ///< samples reported so far
} BinkMuxContext;

static int push(AVPacket ***q, unsigned *n, unsigned *alloc, const AVPacket *pkt)
{
    AVPacket *c;
    if (*n >= *alloc) {
        unsigned na = *alloc ? *alloc * 2 : 64;
        AVPacket **nq = av_realloc_array(*q, na, sizeof(**q));
        if (!nq)
            return AVERROR(ENOMEM);
        *q = nq;
        *alloc = na;
    }
    if (!(c = av_packet_clone(pkt)))
        return AVERROR(ENOMEM);
    (*q)[(*n)++] = c;
    return 0;
}

static int bink_init(AVFormatContext *s)
{
    BinkMuxContext *bink = s->priv_data;

    bink->video_idx = bink->audio_idx = -1;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && bink->video_idx < 0) {
            if (par->codec_id != AV_CODEC_ID_BINKVIDEO ||
                (par->codec_tag && par->codec_tag != MKTAG('B', 'I', 'K', 'i'))) {
                av_log(s, AV_LOG_ERROR, "Bink files need BIKi Bink video\n");
                return AVERROR(EINVAL);
            }
            bink->video_idx = i;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && bink->audio_idx < 0) {
            if (par->codec_id != AV_CODEC_ID_BINKAUDIO_DCT) {
                av_log(s, AV_LOG_ERROR, "Bink files need Bink DCT audio\n");
                return AVERROR(EINVAL);
            }
            bink->audio_idx = i;
        } else {
            av_log(s, AV_LOG_ERROR, "Bink files hold one video and one audio stream\n");
            return AVERROR(EINVAL);
        }
    }
    if (bink->video_idx < 0) {
        av_log(s, AV_LOG_ERROR, "Bink files need a video stream\n");
        return AVERROR(EINVAL);
    }
    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        av_log(s, AV_LOG_ERROR, "Bink output must be seekable\n");
        return AVERROR(EINVAL);
    }

    {
        AVStream *st = s->streams[bink->video_idx];
        AVRational fps = st->avg_frame_rate;
        if (fps.num <= 0 || fps.den <= 0)
            fps = av_inv_q(st->time_base);
        av_reduce(&fps.num, &fps.den, fps.num, fps.den, INT_MAX);
        bink->fps = fps;
        avpriv_set_pts_info(st, 64, fps.den, fps.num);
    }
    if (bink->audio_idx >= 0) {
        AVStream *st = s->streams[bink->audio_idx];
        bink->sample_rate = st->codecpar->sample_rate;
        bink->channels    = st->codecpar->ch_layout.nb_channels;
        if (bink->sample_rate < 1 || bink->sample_rate > 65535 ||
            bink->channels < 1 || bink->channels > 2) {
            av_log(s, AV_LOG_ERROR, "Bink audio must be mono or stereo at 1-65535 Hz\n");
            return AVERROR(EINVAL);
        }
        avpriv_set_pts_info(st, 64, 1, bink->sample_rate);
    }
    return 0;
}

/* Audio samples due by the end of video frame n. */
static int64_t samples_at_frame(const BinkMuxContext *bink, int64_t n)
{
    return av_rescale_rnd(n * bink->sample_rate, bink->fps.den, bink->fps.num,
                          AV_ROUND_NEAR_INF);
}

static int64_t audio_pkt_end(const AVPacket *pkt)
{
    return pkt->pts + pkt->duration;
}

static int write_frame(AVFormatContext *s, const AVPacket *vpkt, int last)
{
    BinkMuxContext *bink = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t start = avio_tell(pb);
    uint32_t size;

    if (bink->nb_frames >= bink->frames_alloc) {
        unsigned na = bink->frames_alloc ? bink->frames_alloc * 2 : 1024;
        BinkFrame *nf;
        if (na > 1000000)
            na = 1000000;
        if (bink->nb_frames >= na) {
            av_log(s, AV_LOG_ERROR, "Bink files hold at most 1000000 frames\n");
            return AVERROR(EINVAL);
        }
        nf = av_realloc_array(bink->frames, na, sizeof(*nf));
        if (!nf)
            return AVERROR(ENOMEM);
        bink->frames = nf;
        bink->frames_alloc = na;
    }

    if (bink->audio_idx >= 0) {
        int64_t due = samples_at_frame(bink, bink->nb_frames + 1);
        unsigned n = 0;
        uint32_t body = 0, reported = 0;

        while (n < bink->nb_aq &&
               (last ? bink->aq[n]->pts < due : audio_pkt_end(bink->aq[n]) <= due)) {
            const AVPacket *a = bink->aq[n];
            if (a->size < 4 || (a->size & 3)) {
                av_log(s, AV_LOG_ERROR, "invalid Bink audio packet\n");
                return AVERROR_INVALIDDATA;
            }
            body     += a->size - 4;
            reported += AV_RL32(a->data);
            n++;
        }
        if (last) {
            int64_t cap = (due - bink->audio_written) * 2 * bink->channels;
            if (cap < 0)
                cap = 0;
            if (reported > cap)
                reported = cap;
        }
        if (!n || !reported) {
            avio_wl32(pb, 0);
        } else {
            avio_wl32(pb, 4 + body);
            avio_wl32(pb, reported);
            for (unsigned i = 0; i < n; i++)
                avio_write(pb, bink->aq[i]->data + 4, bink->aq[i]->size - 4);
            bink->audio_written += reported / (2 * bink->channels);
            bink->max_audio_bytes = FFMAX(bink->max_audio_bytes, reported);
        }
        for (unsigned i = 0; i < n; i++)
            av_packet_free(&bink->aq[i]);
        memmove(bink->aq, bink->aq + n, (bink->nb_aq - n) * sizeof(*bink->aq));
        bink->nb_aq -= n;
    }

    if (vpkt->size & 3) {
        av_log(s, AV_LOG_ERROR, "Bink video packets must be a multiple of 4 bytes\n");
        return AVERROR_INVALIDDATA;
    }
    avio_write(pb, vpkt->data, vpkt->size);

    size = avio_tell(pb) - start;
    bink->frames[bink->nb_frames].size = size;
    bink->frames[bink->nb_frames].key  = !bink->nb_frames || (vpkt->flags & AV_PKT_FLAG_KEY);
    bink->nb_frames++;
    bink->largest_frame = FFMAX(bink->largest_frame, size);
    bink->payload += size;
    return 0;
}

static int flush_video(AVFormatContext *s, int eof)
{
    BinkMuxContext *bink = s->priv_data;
    unsigned done = 0;
    int ret = 0;

    while (done < bink->nb_vq) {
        int last = eof && done == bink->nb_vq - 1;
        if (bink->audio_idx >= 0 && !eof &&
            bink->audio_end < samples_at_frame(bink, bink->nb_frames + 1))
            break;
        if ((ret = write_frame(s, bink->vq[done], last)) < 0)
            break;
        av_packet_free(&bink->vq[done]);
        done++;
    }
    memmove(bink->vq, bink->vq + done, (bink->nb_vq - done) * sizeof(*bink->vq));
    bink->nb_vq -= done;
    return ret;
}

static int bink_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    BinkMuxContext *bink = s->priv_data;
    int ret;

    if (pkt->stream_index == bink->audio_idx) {
        if (pkt->pts == AV_NOPTS_VALUE)
            pkt->pts = bink->audio_end;
        if (pkt->duration <= 0 && pkt->size >= 4)
            pkt->duration = AV_RL32(pkt->data) / (2 * bink->channels);
        if ((ret = push(&bink->aq, &bink->nb_aq, &bink->aq_alloc, pkt)) < 0)
            return ret;
        bink->audio_end = FFMAX(bink->audio_end, audio_pkt_end(pkt));
    } else if (pkt->stream_index == bink->video_idx) {
        if ((ret = push(&bink->vq, &bink->nb_vq, &bink->vq_alloc, pkt)) < 0)
            return ret;
    }
    return flush_video(s, 0);
}

static int bink_write_trailer(AVFormatContext *s)
{
    BinkMuxContext *bink = s->priv_data;
    AVIOContext *pb = s->pb;
    int tracks = bink->audio_idx >= 0;
    int64_t header, total, offset;
    int ret;

    if ((ret = flush_video(s, 1)) < 0)
        return ret;
    if (!bink->nb_frames) {
        av_log(s, AV_LOG_ERROR, "no video frames\n");
        return AVERROR(EINVAL);
    }

    header = 44 + 12 * tracks + 4LL * (bink->nb_frames + 1);
    total  = header + bink->payload;
    if (total > UINT32_MAX) {
        av_log(s, AV_LOG_ERROR, "Bink files are limited to 4 GiB\n");
        return AVERROR(EINVAL);
    }

    avio_flush(pb);
    if ((ret = ff_format_shift_data(s, 0, header)) < 0)
        return ret;
    avio_seek(pb, 0, SEEK_SET);

    ffio_wfourcc(pb, "BIKi");
    avio_wl32(pb, total - 8);
    avio_wl32(pb, bink->nb_frames);
    avio_wl32(pb, bink->largest_frame);
    avio_wl32(pb, bink->nb_frames);
    avio_wl32(pb, s->streams[bink->video_idx]->codecpar->width);
    avio_wl32(pb, s->streams[bink->video_idx]->codecpar->height);
    avio_wl32(pb, bink->fps.num);
    avio_wl32(pb, bink->fps.den);
    avio_wl32(pb, 0);               /* video flags */
    avio_wl32(pb, tracks);
    if (tracks) {
        avio_wl32(pb, FFMAX(2, bink->max_audio_bytes));
        avio_wl16(pb, bink->sample_rate);
        avio_wl16(pb, BINK_AUD_USEDCT | BINK_AUD_16BITS |
                      (bink->channels == 2 ? BINK_AUD_STEREO : 0));
        avio_wl32(pb, 0);           /* track id */
    }
    offset = header;
    for (unsigned i = 0; i < bink->nb_frames; i++) {
        avio_wl32(pb, offset | bink->frames[i].key);
        offset += bink->frames[i].size;
    }
    avio_wl32(pb, offset);
    avio_seek(pb, total, SEEK_SET);
    return 0;
}

static void bink_deinit(AVFormatContext *s)
{
    BinkMuxContext *bink = s->priv_data;
    for (unsigned i = 0; i < bink->nb_vq; i++)
        av_packet_free(&bink->vq[i]);
    for (unsigned i = 0; i < bink->nb_aq; i++)
        av_packet_free(&bink->aq[i]);
    av_freep(&bink->vq);
    av_freep(&bink->aq);
    av_freep(&bink->frames);
}

const FFOutputFormat ff_bink_muxer = {
    .p.name           = "bink",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Bink"),
    .p.extensions     = "bik",
    .p.audio_codec    = AV_CODEC_ID_BINKAUDIO_DCT,
    .p.video_codec    = AV_CODEC_ID_BINKVIDEO,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .priv_data_size   = sizeof(BinkMuxContext),
    .init             = bink_init,
    .write_packet     = bink_write_packet,
    .write_trailer    = bink_write_trailer,
    .deinit           = bink_deinit,
};
