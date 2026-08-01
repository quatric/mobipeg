/*
 * FastVideoDS (.fv) muxer
 * Copyright (c) 2026 mobipeg / quatric
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "mux.h"

typedef struct FVMuxContext {
    AVPacket *video;
    uint8_t *audio;
    unsigned audio_alloc;
    int audio_size;
    int video_stream;
    int audio_stream;
    int audio_block_size;
    uint32_t frame_count;
} FVMuxContext;

static int fv_write_header(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;
    AVStream *vst = NULL, *ast = NULL;
    AVRational rate;

    c->video_stream = c->audio_stream = -1;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (vst || st->codecpar->codec_id != AV_CODEC_ID_FASTVIDEO)
                return AVERROR(EINVAL);
            vst = st;
            c->video_stream = i;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (ast || st->codecpar->codec_id != AV_CODEC_ID_ADPCM_IMA_MOFLEX)
                return AVERROR(EINVAL);
            ast = st;
            c->audio_stream = i;
        } else {
            return AVERROR(EINVAL);
        }
    }
    if (!vst || vst->codecpar->width <= 0 || vst->codecpar->width > UINT16_MAX ||
        vst->codecpar->height <= 0 || vst->codecpar->height > UINT16_MAX)
        return AVERROR(EINVAL);
    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        av_log(s, AV_LOG_ERROR, "FV output must be seekable\n");
        return AVERROR(EINVAL);
    }

    rate = av_guess_frame_rate(s, vst, NULL);
    if (rate.num <= 0 || rate.den <= 0 ||
        rate.num > UINT32_MAX || rate.den > UINT32_MAX)
        rate = av_inv_q(vst->time_base);
    if (rate.num <= 0 || rate.den <= 0 ||
        rate.num > UINT32_MAX || rate.den > UINT32_MAX)
        return AVERROR(EINVAL);

    if (ast) {
        const int channels = ast->codecpar->ch_layout.nb_channels;
        if (channels <= 0 || channels > UINT16_MAX ||
            ast->codecpar->sample_rate <= 0 ||
            ast->codecpar->sample_rate > UINT16_MAX)
            return AVERROR(EINVAL);
        c->audio_block_size = channels * 132;
    }

    c->video = av_packet_alloc();
    if (!c->video)
        return AVERROR(ENOMEM);

    avio_wl32(s->pb, MKTAG('F', 'V', 'D', 'S'));
    avio_wl16(s->pb, vst->codecpar->width);
    avio_wl16(s->pb, vst->codecpar->height);
    avio_wl32(s->pb, rate.num);
    avio_wl32(s->pb, rate.den);
    avio_wl16(s->pb, ast ? ast->codecpar->sample_rate : 0);
    avio_wl16(s->pb, ast ? ast->codecpar->ch_layout.nb_channels : 0);
    avio_wl32(s->pb, 0); /* patched frame count */
    avio_wl32(s->pb, 0); /* no optional seek index */
    return 0;
}

static int fv_flush_record(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;
    const int video_size = c->video->size;
    int aligned_size;
    const int audio_blocks = c->audio_block_size ?
                             c->audio_size / c->audio_block_size : 0;

    if (!video_size)
        return c->audio_size ? AVERROR_INVALIDDATA : 0;
    if (video_size > 0x1FFFF - 3 || audio_blocks > 0x7FFF ||
        (c->audio_block_size && c->audio_size % c->audio_block_size))
        return AVERROR(EINVAL);
    if (c->frame_count == UINT32_MAX)
        return AVERROR(EINVAL);
    aligned_size = FFALIGN(video_size, 4);

    avio_wl32(s->pb, aligned_size | ((uint32_t)audio_blocks << 17));
    avio_write(s->pb, c->video->data, video_size);
    for (int i = video_size; i < aligned_size; i++)
        avio_w8(s->pb, 0);
    if (c->audio_size)
        avio_write(s->pb, c->audio, c->audio_size);
    av_packet_unref(c->video);
    c->audio_size = 0;
    c->frame_count++;
    return 0;
}

static int fv_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    FVMuxContext *c = s->priv_data;
    int ret;

    if (pkt->stream_index == c->video_stream) {
        if (c->video->size && (ret = fv_flush_record(s)) < 0)
            return ret;
        return av_packet_ref(c->video, pkt);
    }
    if (pkt->stream_index == c->audio_stream) {
        uint8_t *audio;
        if (pkt->size <= 0 || pkt->size % c->audio_block_size ||
            pkt->size > INT_MAX - c->audio_size)
            return AVERROR(EINVAL);
        audio = av_fast_realloc(c->audio, &c->audio_alloc,
                                c->audio_size + pkt->size);
        if (!audio)
            return AVERROR(ENOMEM);
        c->audio = audio;
        memcpy(c->audio + c->audio_size, pkt->data, pkt->size);
        c->audio_size += pkt->size;
        return 0;
    }
    return AVERROR(EINVAL);
}

static int fv_write_trailer(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;
    int64_t end;
    int ret = fv_flush_record(s);

    if (ret < 0)
        return ret;
    end = avio_tell(s->pb);
    avio_seek(s->pb, 0x14, SEEK_SET);
    avio_wl32(s->pb, c->frame_count);
    avio_seek(s->pb, end, SEEK_SET);
    return 0;
}

static void fv_deinit(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;

    av_packet_free(&c->video);
    av_freep(&c->audio);
    c->audio_alloc = c->audio_size = 0;
}

const FFOutputFormat ff_fv_muxer = {
    .p.name         = "fv",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FastVideoDS (.fv)"),
    .p.extensions   = "fv",
    .p.audio_codec  = AV_CODEC_ID_ADPCM_IMA_MOFLEX,
    .p.video_codec  = AV_CODEC_ID_FASTVIDEO,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .priv_data_size = sizeof(FVMuxContext),
    .flags_internal = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                      FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
    .write_header   = fv_write_header,
    .write_packet   = fv_write_packet,
    .write_trailer  = fv_write_trailer,
    .deinit         = fv_deinit,
};
