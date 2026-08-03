/*
 * FastVideoDS (.fv) demuxer
 * Copyright (c) 2026 quatric
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

#define FV_MAGIC MKTAG('F', 'V', 'D', 'S')

typedef struct FVDemuxContext {
    int      width, height;
    int      fps_num, fps_den;
    int      sample_rate, channels;
    int      nb_frames, nb_keyframes;
    int      current_frame;
    int      vstream, astream;
    uint8_t *audio_data;
    int      audio_blocks;
    int      audio_block;
    int64_t  audio_pts;
} FVDemuxContext;

static int fv_probe(const AVProbeData *p)
{
    if (p->buf_size < 4)
        return 0;
    if (AV_RL32(p->buf) == FV_MAGIC)
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int fv_read_header(AVFormatContext *s)
{
    FVDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;
    uint8_t hdr[0x1C];

    if (avio_read(pb, hdr, sizeof(hdr)) != sizeof(hdr))
        return AVERROR_INVALIDDATA;
    if (AV_RL32(hdr) != FV_MAGIC)
        return AVERROR_INVALIDDATA;

    c->width        = AV_RL16(hdr + 0x04);
    c->height       = AV_RL16(hdr + 0x06);
    c->fps_num      = AV_RL32(hdr + 0x08);
    c->fps_den      = AV_RL32(hdr + 0x0C);
    c->sample_rate  = AV_RL16(hdr + 0x10);
    c->channels     = AV_RL16(hdr + 0x12);
    c->nb_frames    = AV_RL32(hdr + 0x14);
    c->nb_keyframes = AV_RL32(hdr + 0x18);

    if (c->width <= 0 || c->height <= 0 ||
        c->fps_num <= 0 || c->fps_den <= 0 ||
        c->channels < 0 || c->channels > 8 || c->nb_frames < 0 ||
        c->nb_keyframes < 0 || c->nb_keyframes > INT_MAX / 8)
        return AVERROR_INVALIDDATA;

    /* skip past keyframe index table */
    if (c->nb_keyframes > 0)
        avio_skip(pb, c->nb_keyframes * 8);

    /* Video Stream */
    vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_FASTVIDEO;
    vst->codecpar->width      = c->width;
    vst->codecpar->height     = c->height;
    vst->nb_frames = vst->duration = c->nb_frames;
    c->vstream = vst->index;

    if (c->fps_num > 0 && c->fps_den > 0)
        avpriv_set_pts_info(vst, 64, c->fps_den, c->fps_num);

    /* Audio Stream */
    if (c->channels > 0 && c->sample_rate > 0) {
        ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = AV_CODEC_ID_ADPCM_IMA_MOFLEX;
        ast->codecpar->ch_layout.nb_channels = c->channels;
        ast->codecpar->sample_rate = c->sample_rate;
        c->astream = ast->index;
        avpriv_set_pts_info(ast, 64, 1, c->sample_rate);
    } else {
        c->astream = -1;
    }

    return 0;
}

static int fv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    FVDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t size_field;
    int vlen, audio_blocks, ret;

    if (c->audio_block < c->audio_blocks) {
        const int block_size = c->channels * 132;

        if ((ret = av_new_packet(pkt, block_size)) < 0)
            return ret;
        memcpy(pkt->data, c->audio_data + c->audio_block * block_size,
               block_size);
        pkt->stream_index = c->astream;
        pkt->pts = pkt->dts = c->audio_pts;
        pkt->duration = 256;
        c->audio_pts += 256;
        if (++c->audio_block == c->audio_blocks) {
            av_freep(&c->audio_data);
            c->audio_block = c->audio_blocks = 0;
        }
        return 0;
    }

    if (avio_feof(pb) || c->current_frame >= c->nb_frames)
        return AVERROR_EOF;

    size_field = avio_rl32(pb);
    if (avio_feof(pb))
        return AVERROR_EOF;

    vlen = size_field & 0x1FFFF;
    audio_blocks = size_field >> 17;

    if (!vlen || (vlen & 3) || audio_blocks > INT_MAX / FFMAX(c->channels, 1) / 132)
        return AVERROR_INVALIDDATA;

    if ((ret = av_get_packet(pb, pkt, vlen)) < 0)
        return ret;
    if (ret != vlen) {
        av_packet_unref(pkt);
        return AVERROR_INVALIDDATA;
    }

    if (audio_blocks > 0) {
        const int abytes = audio_blocks * c->channels * 132;

        if (c->astream < 0) {
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        }
        c->audio_data = av_malloc(abytes);
        if (!c->audio_data) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        if (avio_read(pb, c->audio_data, abytes) != abytes) {
            av_freep(&c->audio_data);
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        }
        c->audio_blocks = audio_blocks;
        c->audio_block  = 0;
    }

    pkt->stream_index = c->vstream;
    pkt->pts = pkt->dts = c->current_frame++;
    pkt->duration = 1;
    if (vlen >= 2 && !(AV_RL16(pkt->data) & 0x8000))
        pkt->flags |= AV_PKT_FLAG_KEY;
    return 0;
}

static int fv_read_close(AVFormatContext *s)
{
    FVDemuxContext *c = s->priv_data;

    av_freep(&c->audio_data);
    return 0;
}

const FFInputFormat ff_fv_demuxer = {
    .p.name         = "fv",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FastVideoDS (.fv)"),
    .p.extensions   = "fv",
    .priv_data_size = sizeof(FVDemuxContext),
    .read_probe     = fv_probe,
    .read_header    = fv_read_header,
    .read_packet    = fv_read_packet,
    .read_close     = fv_read_close,
};
