/*
 * RocketVideo (.rvid) demuxer
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
    int      nframes;      /* stored frames: two per picture when dual screen */
    int      version, dual;
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
    if (AV_RL32(p->buf + 4) < 1 || AV_RL32(p->buf + 4) > 5)   /* version */
        return 0;
    return AVPROBE_SCORE_MAX;
}

/* The header went through three layouts.  Version 1 and 2 files are raw
 * 16 bit frames one after another from 0x200 (RocketVideoPlayer refuses the
 * compressed version 2 files) with one 16 bit sound stream; versions 3 to 5
 * share a layout with a frame offset table at 0x200, version 3 lacking the
 * right sound stream. */
static int rvid_read_header(AVFormatContext *s)
{
    RVIDDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;
    uint8_t hdr[0x20];
    int fps_base, i, pictures, sound_bytes;
    unsigned comp_off = 0;
    AVRational fps;

    if (avio_read(pb, hdr, sizeof(hdr)) != sizeof(hdr))
        return AVERROR_INVALIDDATA;
    if (AV_RL32(hdr) != RVID_MAGIC)
        return AVERROR_INVALIDDATA;
    c->version = AV_RL32(hdr + 4);
    if (c->version < 1 || c->version > 5)
        return AVERROR_PATCHWELCOME;

    pictures      = AV_RL32(hdr + 8);
    fps_base      = hdr[0x0C];
    c->vres       = hdr[0x0D];
    c->file_size  = avio_size(pb);
    c->width      = 256;
    if (c->version == 1) {
        c->bmp_mode    = 1;
        c->audio_16bit = 1;
        c->sample_rate = hdr[0x0E] ? AV_RL16(hdr + 0x10) : 0;
        c->snd_left    = 0x200 + 0x200LL * c->vres * pictures;
    } else if (c->version == 2) {
        c->bmp_mode    = 1;
        c->audio_16bit = 1;
        c->sample_rate = hdr[0x0F] ? AV_RL16(hdr + 0x10) : 0;
        if (AV_RL16(hdr + 0x12)) {
            avpriv_request_sample(s, "Compressed version 2 file");
            return AVERROR_PATCHWELCOME;
        }
        c->snd_left    = AV_RL32(hdr + 0x18);
    } else {
        c->interlaced  = hdr[0x0E];
        c->dual        = hdr[0x0F];
        c->sample_rate = AV_RL16(hdr + 0x10);
        c->audio_16bit = hdr[0x12];
        c->bmp_mode    = hdr[0x13];
        comp_off       = AV_RL32(hdr + 0x14);
        c->snd_left    = AV_RL32(hdr + 0x18);
        c->snd_right   = c->version == 3 ? 0 : AV_RL32(hdr + 0x1C);
        if (c->dual > 2)
            return AVERROR_INVALIDDATA;
        if (c->dual == 2 && c->version < 5)   /* only version 5 knows GBA videos */
            c->dual = 0;
        if (c->dual == 2)
            c->width = 240;
    }
    if (!c->sample_rate || !c->snd_left)
        c->snd_left = c->snd_right = 0;
    c->compressed  = comp_off != 0;

    if (c->bmp_mode > 2 || (c->version >= 3 && fps_base == 0x80))
        return AVERROR_INVALIDDATA;
    if (pictures <= 0 || pictures > INT_MAX / 2 || c->vres <= 0 || c->vres > 255)
        return AVERROR_INVALIDDATA;
    c->nframes = c->dual == 1 ? pictures * 2 : pictures;

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

        if (c->snd_left < 0x200 ||
            c->snd_left > c->file_size || c->snd_right > c->file_size)
            return AVERROR_INVALIDDATA;
        left_size = (c->snd_right ? c->snd_right : c->file_size) - c->snd_left;
        c->audio_bytes = c->snd_right
            ? FFMIN(left_size, c->file_size - c->snd_right) : left_size;
        c->audio_bytes -= c->audio_bytes % bps;
    }

    c->ftab = av_malloc_array(c->nframes, sizeof(*c->ftab));
    if (!c->ftab)
        return AVERROR(ENOMEM);
    if (c->version < 3) {
        for (i = 0; i < c->nframes; i++)
            c->ftab[i] = 0x200 + 0x200LL * c->vres * i;
    } else {
        /* frame offset table at 0x200 */
        if (avio_seek(pb, 0x200, SEEK_SET) < 0)
            return AVERROR_INVALIDDATA;
        for (i = 0; i < c->nframes; i++) {
            c->ftab[i] = avio_rl32(pb);
            if (avio_feof(pb))
                return AVERROR_INVALIDDATA;
            /* From version 4 on the low two bits name the part of a file
             * split at 4 GiB; only the first part is read here. */
            if (c->version >= 4 && (c->ftab[i] & 3)) {
                avpriv_request_sample(s, "Split RVID file");
                return AVERROR_PATCHWELCOME;
            }
        }
    }

    if (c->compressed) {
        c->comp = av_malloc_array(c->nframes, sizeof(*c->comp));
        if (!c->comp)
            return AVERROR(ENOMEM);
        if (avio_seek(pb, comp_off, SEEK_SET) < 0)
            return AVERROR_INVALIDDATA;
        for (i = 0; i < c->nframes; i++) {
            c->comp[i] = (c->bmp_mode == 0) ? avio_rl16(pb) : avio_rl32(pb);
            if (avio_feof(pb) || c->comp[i] < 4 || c->comp[i] > INT_MAX / 4)
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
    vst->codecpar->height     = c->vres * (c->interlaced ? 2 : 1) * (c->dual == 1 ? 2 : 1);
    vst->nb_frames = vst->duration = pictures;
    c->vstream = vst->index;

    if (ff_alloc_extradata(vst->codecpar, 4) < 0)
        return AVERROR(ENOMEM);
    vst->codecpar->extradata[0] = c->bmp_mode;
    vst->codecpar->extradata[1] = c->interlaced ? 1 : 0;
    vst->codecpar->extradata[2] = c->compressed ? 1 : 0;
    vst->codecpar->extradata[3] = c->dual == 1;

    /* The byte is a whole rate; bit 7 says the real rate is 1000/1001 of it
     * and 0 stands for the DS's own refresh rate.  Older versions know
     * neither. */
    if (c->version >= 3 && fps_base == 0)
        fps = av_make_q(598261, 10000);
    else if (c->version >= 3 && (fps_base & 0x80))
        fps = av_make_q((fps_base & 0x7F) * 1000, 1001);
    else
        fps = av_make_q(FFMAX(fps_base, 1), 1);
    avpriv_set_pts_info(vst, 64, fps.den, fps.num);
    vst->avg_frame_rate = vst->r_frame_rate = fps;

    /* audio stream (signed PCM), stored as separate L then R blobs */
    sound_bytes = c->audio_16bit ? 2 : 1;
    if (c->snd_left && c->audio_bytes >= sound_bytes) {
        c->channels = c->snd_right ? 2 : 1;
        ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = c->audio_16bit ? AV_CODEC_ID_PCM_S16LE
                                                     : AV_CODEC_ID_PCM_S8;
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
        const int screens = c->dual == 1 ? 2 : 1;
        const int i = c->frame;
        int size = 0, pos = 0;

        for (int k = 0; k < screens; k++)
            size += rvid_frame_size(c, i + k);
        /* A dual screen packet is the top screen's size, then both screens. */
        if ((ret = av_new_packet(pkt, size + (screens == 2 ? 4 : 0))) < 0)
            return ret;
        if (screens == 2) {
            AV_WL32(pkt->data, rvid_frame_size(c, i));
            pos = 4;
        }
        for (int k = 0; k < screens; k++) {
            const int part = rvid_frame_size(c, i + k);

            if (avio_seek(pb, c->ftab[i + k], SEEK_SET) < 0 ||
                avio_read(pb, pkt->data + pos, part) != part) {
                av_packet_unref(pkt);
                return AVERROR_INVALIDDATA;
            }
            pos += part;
        }
        pkt->stream_index = c->vstream;
        pkt->pts = pkt->dts = i / screens;
        pkt->duration = 1;
        pkt->flags |= AV_PKT_FLAG_KEY;
        c->frame += screens;
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
