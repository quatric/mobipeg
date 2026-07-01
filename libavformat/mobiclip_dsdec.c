/* Copyright (c) 2026 quatric - quatricsoftware@gmail.com */
#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"

typedef struct MobiClipDSContext {
    int nb_frames;
    int video_width;
    int video_height;
    int fps;
    int sample_rate;
    int channels;
    uint32_t index_offset;
    int current_frame;
} MobiClipDSContext;

static int mobiclip_ds_probe(const AVProbeData *p)
{
    if (!memcmp(p->buf, "VXDS", 4))
        return AVPROBE_SCORE_MAX;
    if (!memcmp(p->buf, "MODSN3\n\0", 8))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int mobiclip_ds_read_header(AVFormatContext *s)
{
    MobiClipDSContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t magic[8];
    AVStream *vst = NULL, *ast = NULL;

    avio_read(pb, magic, 8);

    if (!memcmp(magic, "VXDS", 4)) {
        avio_seek(pb, 4, SEEK_SET);
        ctx->nb_frames = avio_rl32(pb);
        ctx->video_width = avio_rl32(pb);
        ctx->video_height = avio_rl32(pb);
        avio_skip(pb, 4); // uncompressed frame size
        ctx->fps = avio_rl32(pb);
        ctx->sample_rate = avio_rl32(pb);
        ctx->channels = avio_rl32(pb);
        avio_skip(pb, 4); // audio blocks or unknown
        ctx->index_offset = avio_rl32(pb);
    } else if (!memcmp(magic, "MODSN3\n\0", 8)) {
        ctx->nb_frames = avio_rl32(pb);
        ctx->video_width = avio_rl32(pb);
        ctx->video_height = avio_rl32(pb);
        avio_skip(pb, 2); // unknown
        ctx->fps = avio_rl16(pb);
        avio_skip(pb, 2); // unknown
        ctx->channels = avio_rl16(pb);
        ctx->sample_rate = avio_rl32(pb);
        avio_skip(pb, 4); // unknown
        ctx->index_offset = avio_rl32(pb);
    } else {
        return AVERROR_INVALIDDATA;
    }

    vst = avformat_new_stream(s, NULL);
    if (!vst) return AVERROR(ENOMEM);

    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_MOBICLIP; // Assuming generic codec id or needs to be added
    vst->codecpar->width = ctx->video_width;
    vst->codecpar->height = ctx->video_height;
    
    // FPS could be 0, provide fallback
    if (ctx->fps > 0) {
        avpriv_set_pts_info(vst, 32, 1, ctx->fps);
        vst->avg_frame_rate = (AVRational){ctx->fps, 1};
    } else {
        avpriv_set_pts_info(vst, 32, 1, 30); // fallback
        vst->avg_frame_rate = (AVRational){30, 1};
    }
    
    vst->nb_frames = ctx->nb_frames;

    if (ctx->sample_rate > 0 && ctx->channels > 0) {
        ast = avformat_new_stream(s, NULL);
        if (!ast) return AVERROR(ENOMEM);
        ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_DK3; // Placeholder for ADPCM used in some DS MobiClip
        ast->codecpar->sample_rate = ctx->sample_rate;
        ast->codecpar->ch_layout.nb_channels = ctx->channels;
        avpriv_set_pts_info(ast, 32, 1, ctx->sample_rate);
    }

    ctx->current_frame = 0;

    return 0;
}

static int mobiclip_ds_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MobiClipDSContext *ctx = s->priv_data;
    // Preliminary demuxer: we don't know the exact packet size or layout yet.
    // The index is at ctx->index_offset which will have to be parsed.
    // For now, return EOF since index reading and packet extraction is not fully known.
    if (ctx->current_frame >= ctx->nb_frames)
        return AVERROR_EOF;
        
    return AVERROR_EOF;
}

const FFInputFormat ff_mobiclip_ds_demuxer = {
    .p.name           = "mobiclip_ds",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MobiClip Nintendo DS (VXDS/MODSN3)"),
    .priv_data_size = sizeof(MobiClipDSContext),
    .read_probe     = mobiclip_ds_probe,
    .read_header    = mobiclip_ds_read_header,
    .read_packet    = mobiclip_ds_read_packet,
    .p.extensions     = "vx,mods",
    .p.flags          = AVFMT_GENERIC_INDEX,
};
