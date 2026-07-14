/*
 * MobiClip / ActImagine DS (.vx) muxer
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
 *
 * Writes ActImagine ".vx" (VXDS) files: one vx video stream, plus an
 * optional vx_audio stream. Each container frame record is
 * [frame_data_size u16][aframes_qty u16][ video bits ++ N AFrame bits ], all in
 * one word-aligned little-endian bitstream. Because both the video encoder and
 * the audio encoder already emit whole 16-bit words (byte-swapped for the
 * decoder), the muxer just concatenates their bytes and records how many
 * AFrames landed in each frame. Audio is packed one 128-sample AFrame per
 * packet, distributed across the video frames to track sample_rate/fps.
 *
 * All packets are buffered and the file body is written in the trailer so the
 * two streams can be paired regardless of arrival interleaving. A seekable
 * output is required (header totals/offsets are patched at the end).
 */

#include "avformat.h"
#include "mux.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#define VXDS_AUDIO_EXTRADATA_SIZE (3*64*8*2 + 8*2 + 8*4 + 4)
#define AFRAME_SAMPLES 128

typedef struct VXMuxContext {
    int video_idx, audio_idx;
    uint32_t quantizer;
    uint32_t frame_rate_fixed;
    AVRational fps;
    int sample_rate;

    AVPacket **vq; int nvq, cvq;   /* video packets */
    AVPacket **aq; int naq, caq;   /* audio packets (one AFrame each) */

    /* trained audio codebook delivered by the encoder via packet side data
     * (it trains on the whole stream, so it only exists at drain time) */
    uint8_t *audio_cbk;
    int      audio_cbk_size;
} VXMuxContext;

static int queue_push(AVPacket ***q, int *n, int *cap, const AVPacket *pkt)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 256;
        AVPacket **nq = av_realloc_array(*q, nc, sizeof(**q));
        if (!nq)
            return AVERROR(ENOMEM);
        *q = nq; *cap = nc;
    }
    if (!((*q)[*n] = av_packet_clone(pkt)))
        return AVERROR(ENOMEM);
    (*n)++;
    return 0;
}

static int vx_write_header(AVFormatContext *s)
{
    VXMuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst;
    AVRational rate;

    c->video_idx = c->audio_idx = -1;
    for (int i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *p = s->streams[i]->codecpar;
        if (p->codec_id == AV_CODEC_ID_VX && c->video_idx < 0)
            c->video_idx = i;
        else if (p->codec_id == AV_CODEC_ID_VX_AUDIO && c->audio_idx < 0)
            c->audio_idx = i;
        else {
            av_log(s, AV_LOG_ERROR, "vx muxer accepts one vx video "
                   "stream and at most one vx_audio stream\n");
            return AVERROR(EINVAL);
        }
    }
    if (c->video_idx < 0) {
        av_log(s, AV_LOG_ERROR, "vx muxer needs a vx video stream\n");
        return AVERROR(EINVAL);
    }
    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        av_log(s, AV_LOG_ERROR, "vx muxer requires a seekable output\n");
        return AVERROR(EINVAL);
    }

    vst = s->streams[c->video_idx];
    if (vst->codecpar->extradata_size >= 4)
        c->quantizer = AV_RL32(vst->codecpar->extradata);
    else
        c->quantizer = 40;

    rate = vst->avg_frame_rate.num ? vst->avg_frame_rate : (AVRational){ 30, 1 };
    c->fps = rate;
    c->frame_rate_fixed = (uint32_t)av_rescale(65536, rate.num, rate.den);

    if (c->audio_idx >= 0) {
        AVCodecParameters *ap = s->streams[c->audio_idx]->codecpar;
        c->sample_rate = ap->sample_rate;
        if (ap->extradata_size != VXDS_AUDIO_EXTRADATA_SIZE) {
            av_log(s, AV_LOG_ERROR, "audio extradata is %d bytes, expected %d\n",
                   ap->extradata_size, VXDS_AUDIO_EXTRADATA_SIZE);
            return AVERROR(EINVAL);
        }
    }

    /* header with placeholders; totals/offsets patched in the trailer */
    avio_write(pb, "VXDS", 4);
    avio_wl32(pb, 0);                              /* frames_qty */
    avio_wl32(pb, vst->codecpar->width);
    avio_wl32(pb, vst->codecpar->height);
    avio_wl32(pb, c->frame_rate_fixed);
    avio_wl32(pb, c->quantizer);
    avio_wl32(pb, c->sample_rate);                 /* audio_sample_rate */
    avio_wl32(pb, c->audio_idx >= 0 ? 1 : 0);      /* audio_streams_qty */
    avio_wl32(pb, 0);                              /* frame_data_size_max */
    avio_wl32(pb, 0);                              /* audio_extradata_offset */
    avio_wl32(pb, 0);                              /* seek_table_offset */
    avio_wl32(pb, 0);                              /* seek_table_entries_qty */

    return 0;
}

static int vx_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    VXMuxContext *c = s->priv_data;
    size_t sd_size;
    const uint8_t *sd;

    if (pkt->stream_index == c->video_idx) {
        /* rate control reports the final global quantizer via side data */
        sd = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &sd_size);
        if (sd && sd_size >= 4)
            c->quantizer = AV_RL32(sd);
        return queue_push(&c->vq, &c->nvq, &c->cvq, pkt);
    }
    if (pkt->stream_index == c->audio_idx) {
        sd = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &sd_size);
        if (sd && sd_size >= VXDS_AUDIO_EXTRADATA_SIZE && !c->audio_cbk) {
            c->audio_cbk = av_memdup(sd, sd_size);
            if (!c->audio_cbk)
                return AVERROR(ENOMEM);
            c->audio_cbk_size = (int)sd_size;
        }
        return queue_push(&c->aq, &c->naq, &c->caq, pkt);
    }
    return 0;
}

static int vx_write_trailer(AVFormatContext *s)
{
    VXMuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t audio_extradata_offset, seek_table_offset;
    uint32_t frame_data_size_max = 0;
    int acursor = 0;

    for (int k = 0; k < c->nvq; k++) {
        AVPacket *vp = c->vq[k];
        int aq_end = acursor, afr_qty, last = (k == c->nvq - 1);
        int64_t payload;

        /* how many AFrames belong up to the end of video frame k */
        if (c->audio_idx >= 0 && c->naq > 0) {
            if (last) {
                aq_end = c->naq;                  /* flush the remainder here */
            } else {
                int64_t target = av_rescale((int64_t)(k + 1) * c->sample_rate,
                                            c->fps.den,
                                            (int64_t)c->fps.num * AFRAME_SAMPLES);
                aq_end = (int)FFMIN(target, (int64_t)c->naq);
                if (aq_end < acursor)
                    aq_end = acursor;
            }
        }
        afr_qty = aq_end - acursor;

        payload = vp->size + 2;                    /* + the aframes_qty word */
        for (int a = acursor; a < aq_end; a++)
            payload += c->aq[a]->size;
        /* The DS player streams each record with an async FS/card read of
         * (frame_data_size + 2) bytes, which silently drops the unaligned tail
         * when that length is not a multiple of 4 — corrupting the next
         * record's aframes_qty word (video still plays, audio turns to white
         * noise). Every retail .vx keeps (size + 2) % 4 == 0 by padding the
         * record end; do the same. The pad bytes sit after the last AFrame and
         * are never read back (audio position comes from the video decoder's
         * consumed-bytes return, AFrames are self-sized). */
        if (((payload + 2) & 3) != 0)
            payload += 2;
        if (payload > 0xFFFF) {
            av_log(s, AV_LOG_ERROR, "frame %d too large for vx container (%"PRId64" bytes)\n",
                   k, payload);
            return AVERROR(EINVAL);
        }

        avio_wl16(pb, (uint16_t)payload);
        avio_wl16(pb, (uint16_t)afr_qty);
        avio_write(pb, vp->data, vp->size);
        {
            int64_t written = vp->size + 2;
            for (int a = acursor; a < aq_end; a++) {
                avio_write(pb, c->aq[a]->data, c->aq[a]->size);
                written += c->aq[a]->size;
            }
            while (written < payload) {            /* alignment pad */
                avio_w8(pb, 0);
                written++;
            }
        }

        if (payload + 2 > frame_data_size_max)
            frame_data_size_max = payload + 2;
        acursor = aq_end;
    }

    /* Audio codebook block. Retail video-only .vx files (e.g. PeterPan ds*.vx)
     * carry NO audio-extradata block and set audio_extradata_offset = 0, so
     * mirror that when there is no audio stream — writing a zeroed block here
     * left a stray offset that doesn't match any retail file. */
    if (c->audio_idx < 0) {
        audio_extradata_offset = 0;
    } else {
        audio_extradata_offset = avio_tell(pb);
        if (c->audio_cbk)
            avio_write(pb, c->audio_cbk, VXDS_AUDIO_EXTRADATA_SIZE);
        else
            avio_write(pb, s->streams[c->audio_idx]->codecpar->extradata,
                       VXDS_AUDIO_EXTRADATA_SIZE);
    }

    seek_table_offset = avio_tell(pb);            /* zero entries follow */

    avio_seek(pb, 4, SEEK_SET);
    avio_wl32(pb, c->nvq);                         /* frames_qty */
    avio_seek(pb, 20, SEEK_SET);
    avio_wl32(pb, c->quantizer);
    avio_seek(pb, 32, SEEK_SET);
    avio_wl32(pb, frame_data_size_max);
    avio_wl32(pb, audio_extradata_offset);
    avio_wl32(pb, seek_table_offset);
    avio_wl32(pb, 0);                              /* seek_table_entries_qty */

    return 0;
}

static void vx_deinit(AVFormatContext *s)
{
    VXMuxContext *c = s->priv_data;
    for (int i = 0; i < c->nvq; i++) av_packet_free(&c->vq[i]);
    for (int i = 0; i < c->naq; i++) av_packet_free(&c->aq[i]);
    av_freep(&c->vq);
    av_freep(&c->aq);
    av_freep(&c->audio_cbk);
}

const FFOutputFormat ff_vx_muxer = {
    .p.name         = "vx",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ActImagine / MobiClip Nintendo DS (VXDS)"),
    .p.extensions   = "vx",
    .p.audio_codec  = AV_CODEC_ID_VX_AUDIO,
    .p.video_codec  = AV_CODEC_ID_VX,
    .priv_data_size = sizeof(VXMuxContext),
    .write_header   = vx_write_header,
    .write_packet   = vx_write_packet,
    .write_trailer  = vx_write_trailer,
    .deinit         = vx_deinit,
    .p.flags        = AVFMT_NOTIMESTAMPS,
};
