/* Copyright (c) 2026 quatric - quatricsoftware@gmail.com */
#include <limits.h>
#include <string.h>

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

/* Layout of the "VXDS" (ActImagine .vx) audio extradata block, matching
 * AudioExtraData in libavcodec/vx_audio.c: 3*64*8 int16 codebooks +
 * 8 uint16 scale modifiers + 8 int32 lpc base + 1 uint32 initial scale. */
#define VXDS_AUDIO_EXTRADATA_SIZE (3*64*8*2 + 8*2 + 8*4 + 4)

typedef struct MobiClipDSContext {
    int nb_frames;
    int video_width;
    int video_height;
    uint32_t frame_rate_fixed; /* 16.16 fixed point */
    uint32_t quantizer;
    int sample_rate;
    int channels;
    int current_frame;

    int vstream_index;
    int astream_index;

    int64_t audio_sample_pos;
    uint8_t *pending_audio_data;
    int pending_audio_size;
} MobiClipDSContext;

static int vx_probe(const AVProbeData *p)
{
    /* NB: "MODSN3\n\0" (DS .mods) is handled by mods.c, not here -- avoid
     * claiming the same magic from two demuxers. */
    if (!memcmp(p->buf, "VXDS", 4))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int read_header_vxds(AVFormatContext *s)
{
    MobiClipDSContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *vst, *ast;
    uint32_t audio_extradata_offset;
    int ret;

    avio_skip(pb, 4); /* "VXDS" signature already matched by probe */
    ctx->nb_frames        = avio_rl32(pb);
    ctx->video_width      = avio_rl32(pb);
    ctx->video_height     = avio_rl32(pb);
    ctx->frame_rate_fixed = avio_rl32(pb);
    ctx->quantizer        = avio_rl32(pb);
    ctx->sample_rate      = avio_rl32(pb);
    ctx->channels          = avio_rl32(pb) > 0 ? 1 : 0; /* mono only, see vx_audio.c */
    avio_skip(pb, 4); /* frame_data_size_max */
    audio_extradata_offset = avio_rl32(pb);
    avio_skip(pb, 8); /* seek_table_offset, seek_table_entries_qty (unused: sequential demux only) */

    if (ctx->video_width <= 0 || ctx->video_height <= 0 ||
        (ctx->video_width % 16) || (ctx->video_height % 16))
        return AVERROR_INVALIDDATA;

    vst = avformat_new_stream(s, NULL);
    if (!vst) return AVERROR(ENOMEM);
    ctx->vstream_index = vst->index;
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_VX;
    vst->codecpar->width      = ctx->video_width;
    vst->codecpar->height     = ctx->video_height;
    vst->nb_frames            = ctx->nb_frames;

    ret = ff_alloc_extradata(vst->codecpar, 4);
    if (ret < 0) return ret;
    AV_WL32(vst->codecpar->extradata, ctx->quantizer);

    {
        int num = ctx->frame_rate_fixed, den = 0x10000;
        if (num > 0) {
            av_reduce(&num, &den, num, den, INT_MAX);
            avpriv_set_pts_info(vst, 32, den, num);
            vst->avg_frame_rate = (AVRational){ num, den };
        } else {
            avpriv_set_pts_info(vst, 32, 1, 30);
            vst->avg_frame_rate = (AVRational){ 30, 1 };
        }
    }

    ctx->astream_index = -1;
    if (ctx->sample_rate > 0 && ctx->channels > 0) {
        ast = avformat_new_stream(s, NULL);
        if (!ast) return AVERROR(ENOMEM);
        ctx->astream_index = ast->index;
        ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id   = AV_CODEC_ID_VX_AUDIO;
        ast->codecpar->sample_rate = ctx->sample_rate;
        av_channel_layout_default(&ast->codecpar->ch_layout, ctx->channels);
        avpriv_set_pts_info(ast, 64, 1, ctx->sample_rate);

        ret = ff_alloc_extradata(ast->codecpar, 12 + VXDS_AUDIO_EXTRADATA_SIZE);
        if (ret < 0) return ret;
        AV_WL32(ast->codecpar->extradata,     ctx->quantizer);
        AV_WL32(ast->codecpar->extradata + 4, ctx->video_width);
        AV_WL32(ast->codecpar->extradata + 8, ctx->video_height);

        {
            int64_t here = avio_tell(pb);
            avio_seek(pb, audio_extradata_offset, SEEK_SET);
            ret = avio_read(pb, ast->codecpar->extradata + 12, VXDS_AUDIO_EXTRADATA_SIZE);
            if (ret != VXDS_AUDIO_EXTRADATA_SIZE)
                return AVERROR_INVALIDDATA;
            avio_seek(pb, here, SEEK_SET);
        }
    }

    return 0;
}

static int vx_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    uint8_t magic[4];

    avio_read(pb, magic, 4);
    avio_seek(pb, 0, SEEK_SET);

    if (!memcmp(magic, "VXDS", 4))
        return read_header_vxds(s);

    return AVERROR_INVALIDDATA;
}

static int vx_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MobiClipDSContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    int frame_data_size, aframes_qty, payload_len, ret;

    if (ctx->pending_audio_data) {
        ret = av_new_packet(pkt, ctx->pending_audio_size);
        if (ret < 0) return ret;
        memcpy(pkt->data, ctx->pending_audio_data, ctx->pending_audio_size);
        av_freep(&ctx->pending_audio_data);
        pkt->stream_index = ctx->astream_index;
        return 0;
    }

    if (ctx->current_frame >= ctx->nb_frames || avio_feof(pb))
        return AVERROR_EOF;

    frame_data_size = avio_rl16(pb);
    aframes_qty     = avio_rl16(pb);
    if (frame_data_size < 2)
        return AVERROR_INVALIDDATA;
    payload_len = frame_data_size - 2;

    ret = av_get_packet(pb, pkt, payload_len);
    if (ret < 0) return ret;
    pkt->stream_index = ctx->vstream_index;
    pkt->pts = ctx->current_frame;
    pkt->flags |= AV_PKT_FLAG_KEY;

    if (aframes_qty > 0 && ctx->astream_index >= 0) {
        ctx->pending_audio_size = 4 + ret;
        ctx->pending_audio_data = av_malloc(ctx->pending_audio_size);
        if (!ctx->pending_audio_data)
            return AVERROR(ENOMEM);
        AV_WL32(ctx->pending_audio_data, aframes_qty);
        memcpy(ctx->pending_audio_data + 4, pkt->data, ret);
        ctx->audio_sample_pos += aframes_qty * 128;
    }

    ctx->current_frame++;
    return 0;
}

static int vx_close(AVFormatContext *s)
{
    MobiClipDSContext *ctx = s->priv_data;
    av_freep(&ctx->pending_audio_data);
    return 0;
}

const FFInputFormat ff_vx_demuxer = {
    .p.name           = "vx",
    .p.long_name      = NULL_IF_CONFIG_SMALL("ActImagine / MobiClip Nintendo DS (VXDS)"),
    .priv_data_size = sizeof(MobiClipDSContext),
    .read_probe     = vx_probe,
    .read_header    = vx_read_header,
    .read_packet    = vx_read_packet,
    .read_close     = vx_close,
    .p.extensions     = "vx",
    .p.flags          = AVFMT_GENERIC_INDEX,
};
