/*
 * RocketVideo (.rvid) demuxer
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

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

#define RVID_MAGIC MKTAG('R', 'V', 'I', 'D')
#define RVID_PAL_BYTES 0x200
#define RVID_AUDIO_PACKET_SAMPLES 4096

typedef struct RVIDDemuxContext {
    int      nframes;
    int      bmp_mode, interlaced, compressed;
    int      width, vres;
    uint32_t *ftab;        /* absolute frame offsets */
    uint32_t *comp;        /* per-frame payload size (compressed builds) */
    int      frame;        /* next video frame to emit */
    int      vstream, astream;
    int64_t  audio_bytes, audio_pos; /* per channel */
    int64_t  snd_left, snd_right, file_size;
    int      audio_16bit, channels, sample_rate;
} RVIDDemuxContext;

static int rvid_probe(const AVProbeData *p)
{
    if (p->buf_size < 8)
        return 0;
    if (AV_RL32(p->buf) != RVID_MAGIC)
        return 0;
    if (AV_RL32(p->buf + 4) != 5)          /* version */
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int rvid_read_header(AVFormatContext *s)
{
    RVIDDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;
    uint8_t hdr[0x20];
    int fps_base, i, dual;
    unsigned comp_off;
    AVRational fps;

    if (avio_read(pb, hdr, sizeof(hdr)) != sizeof(hdr))
        return AVERROR_INVALIDDATA;
    if (AV_RL32(hdr) != RVID_MAGIC || AV_RL32(hdr + 4) != 5)
        return AVERROR_INVALIDDATA;

    c->nframes    = AV_RL32(hdr + 8);
    fps_base      = hdr[0x0C];
    c->vres       = hdr[0x0D];
    c->interlaced = hdr[0x0E];
    dual          = hdr[0x0F];
    c->sample_rate = AV_RL16(hdr + 0x10);
    c->audio_16bit = hdr[0x12];
    c->bmp_mode    = hdr[0x13];
    comp_off       = AV_RL32(hdr + 0x14);
    c->snd_left    = AV_RL32(hdr + 0x18);
    c->snd_right   = AV_RL32(hdr + 0x1C);
    c->compressed  = comp_off != 0;

    if (dual > 2 || c->bmp_mode > 2 || fps_base == 0x80)
        return AVERROR_INVALIDDATA;
    if (c->nframes <= 0 || c->vres <= 0 || c->vres > 255)
        return AVERROR_INVALIDDATA;
    c->width = dual == 2 ? 240 : 256;
    c->file_size = avio_size(pb);

    if (c->file_size > 0 &&
        (0x200 + 4LL * c->nframes > c->file_size ||
         (c->compressed && (int64_t)comp_off +
          (c->bmp_mode == 0 ? 2LL : 4LL) * c->nframes > c->file_size)))
        return AVERROR_INVALIDDATA;

    if (c->snd_right && (!c->snd_left || c->snd_right <= c->snd_left))
        return AVERROR_INVALIDDATA;
    if (c->snd_left) {
        int bps = c->audio_16bit ? 2 : 1;
        int64_t left_size;

        if (!c->sample_rate || c->snd_left < 0x200 ||
            c->snd_left > c->file_size || c->snd_right > c->file_size)
            return AVERROR_INVALIDDATA;
        left_size = (c->snd_right ? c->snd_right : c->file_size) - c->snd_left;
        c->audio_bytes = c->snd_right
            ? FFMIN(left_size, c->file_size - c->snd_right) : left_size;
        if (left_size % bps || (c->snd_right && (c->file_size - c->snd_right) % bps))
            return AVERROR_INVALIDDATA;
    }

    /* frame offset table at 0x200 */
    c->ftab = av_malloc_array(c->nframes, sizeof(*c->ftab));
    if (!c->ftab)
        return AVERROR(ENOMEM);
    if (avio_seek(pb, 0x200, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    for (i = 0; i < c->nframes; i++) {
        c->ftab[i] = avio_rl32(pb);
        if (avio_feof(pb))
            return AVERROR_INVALIDDATA;
    }

    if (c->compressed) {
        c->comp = av_malloc_array(c->nframes, sizeof(*c->comp));
        if (!c->comp)
            return AVERROR(ENOMEM);
        if (avio_seek(pb, comp_off, SEEK_SET) < 0)
            return AVERROR_INVALIDDATA;
        for (i = 0; i < c->nframes; i++) {
            c->comp[i] = (c->bmp_mode == 0) ? avio_rl16(pb) : avio_rl32(pb);
            if (avio_feof(pb) || c->comp[i] < 4 || c->comp[i] > INT_MAX - RVID_PAL_BYTES)
                return AVERROR_INVALIDDATA;
        }
    }

    for (i = 0; i < c->nframes; i++) {
        int pal = c->bmp_mode == 0 ? RVID_PAL_BYTES : 0;
        int64_t size = pal + (c->compressed ? c->comp[i]
            : c->width * c->vres * (c->bmp_mode == 0 ? 1 : 2));
        if (c->ftab[i] < 0x200 ||
            (c->file_size > 0 && c->ftab[i] + size > c->file_size))
            return AVERROR_INVALIDDATA;
    }

    /* video stream */
    vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_RVID;
    vst->codecpar->width      = c->width;
    vst->codecpar->height     = c->interlaced ? c->vres * 2 : c->vres;
    vst->nb_frames = vst->duration = c->nframes;
    c->vstream = vst->index;

    if (ff_alloc_extradata(vst->codecpar, 4) < 0)
        return AVERROR(ENOMEM);
    vst->codecpar->extradata[0] = c->bmp_mode;
    vst->codecpar->extradata[1] = c->interlaced ? 1 : 0;
    vst->codecpar->extradata[2] = c->compressed ? 1 : 0;
    vst->codecpar->extradata[3] = 0;

    /* fps: base value, minus 0.1 if the high bit is set (0 == 59.8261) */
    if (fps_base == 0)
        fps = av_d2q(59.8261, INT_MAX);
    else if (fps_base & 0x80)
        fps = av_make_q((fps_base & 0x7F) * 10 - 1, 10);
    else
        fps = av_make_q(fps_base & 0x7F, 1);
    avpriv_set_pts_info(vst, 64, fps.den, fps.num);

    /* audio stream (PCM), stored as separate L then R blobs */
    if (c->snd_left) {
        c->channels = c->snd_right ? 2 : 1;
        ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = c->audio_16bit ? AV_CODEC_ID_PCM_S16LE
                                                     : AV_CODEC_ID_PCM_U8;
        ast->codecpar->ch_layout.nb_channels = c->channels;
        ast->codecpar->sample_rate = c->sample_rate;
        c->astream = ast->index;
        avpriv_set_pts_info(ast, 64, 1, c->sample_rate);
    } else {
        c->astream = -1;
    }

    return 0;
}

static int rvid_frame_size(RVIDDemuxContext *c, int i)
{
    int pal = (c->bmp_mode == 0) ? RVID_PAL_BYTES : 0;
    if (c->compressed)
        return pal + c->comp[i];
    return pal + (c->bmp_mode == 0 ? c->width * c->vres
                                   : c->width * c->vres * 2);
}

static int rvid_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    RVIDDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    if (c->frame < c->nframes) {
        int i = c->frame, size = rvid_frame_size(c, i);
        if (avio_seek(pb, c->ftab[i], SEEK_SET) < 0)
            return AVERROR_EOF;
        if ((ret = av_get_packet(pb, pkt, size)) < 0)
            return ret;
        if (ret != size) {
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        }
        pkt->stream_index = c->vstream;
        pkt->pts = pkt->dts = i;
        pkt->duration = 1;
        pkt->flags |= AV_PKT_FLAG_KEY;
        c->frame++;
        return 0;
    }

    /* The channels are separate file regions. Read bounded chunks rather
     * than allocating the complete soundtrack in one packet. */
    if (c->astream >= 0 && c->audio_pos < c->audio_bytes) {
        uint8_t channel[RVID_AUDIO_PACKET_SAMPLES * 2];
        int bps = c->audio_16bit ? 2 : 1;
        int per_ch = FFMIN(c->audio_bytes - c->audio_pos,
                           RVID_AUDIO_PACKET_SAMPLES * bps);

        if ((ret = av_new_packet(pkt, per_ch * c->channels)) < 0)
            return ret;
        for (int ch = 0; ch < c->channels; ch++) {
            int64_t offset = (ch ? c->snd_right : c->snd_left) + c->audio_pos;
            int64_t seek_ret = avio_seek(pb, offset, SEEK_SET);
            if (seek_ret < 0) {
                av_packet_unref(pkt);
                return seek_ret;
            }
            ret = avio_read(pb, channel, per_ch);
            if (ret != per_ch) {
                av_packet_unref(pkt);
                return ret < 0 && ret != AVERROR_EOF ? ret : AVERROR_INVALIDDATA;
            }
            for (int k = 0; k < per_ch / bps; k++)
                memcpy(pkt->data + (c->channels * k + ch) * bps, channel + k * bps, bps);
        }
        pkt->stream_index = c->astream;
        pkt->pts = pkt->dts = c->audio_pos / bps;
        pkt->duration = per_ch / bps;
        c->audio_pos += per_ch;
        return 0;
    }

    return AVERROR_EOF;
}

static int rvid_read_close(AVFormatContext *s)
{
    RVIDDemuxContext *c = s->priv_data;
    av_freep(&c->ftab);
    av_freep(&c->comp);
    return 0;
}

const FFInputFormat ff_rvid_demuxer = {
    .p.name         = "rvid",
    .p.long_name    = NULL_IF_CONFIG_SMALL("RocketVideo (RVID)"),
    .p.extensions   = "rvid",
    .priv_data_size = sizeof(RVIDDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = rvid_probe,
    .read_header    = rvid_read_header,
    .read_packet    = rvid_read_packet,
    .read_close     = rvid_read_close,
};
