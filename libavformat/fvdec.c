/*
 * FastVideoDS (.fv) demuxer
 * Copyright (c) 2026 mobipeg / quatric
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

    if (c->width <= 0 || c->height <= 0)
        c->width = 256;

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

    if (avio_feof(pb))
        return AVERROR_EOF;

    size_field = avio_rl32(pb);
    if (avio_feof(pb))
        return AVERROR_EOF;

    vlen = size_field & 0x1FFFF;
    audio_blocks = size_field >> 17;

    if (vlen > 0) {
        int pad = ((vlen + 3) & ~3) - vlen;
        if ((ret = av_get_packet(pb, pkt, vlen)) < 0)
            return ret;
        if (pad > 0)
            avio_skip(pb, pad);
        pkt->stream_index = c->vstream;
        pkt->pts = pkt->dts = c->current_frame++;
        pkt->duration = 1;
        return 0;
    }

    if (audio_blocks > 0 && c->astream >= 0) {
        int abytes = audio_blocks * c->channels * 132;
        if ((ret = av_get_packet(pb, pkt, abytes)) < 0)
            return ret;
        pkt->stream_index = c->astream;
        return 0;
    }

    return AVERROR_EOF;
}

const FFInputFormat ff_fv_demuxer = {
    .p.name         = "fv",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FastVideoDS (.fv)"),
    .p.extensions   = "fv",
    .priv_data_size = sizeof(FVDemuxContext),
    .read_probe     = fv_probe,
    .read_header    = fv_read_header,
    .read_packet    = fv_read_packet,
};
