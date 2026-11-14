/*
 * FastVideoDS (.fv) muxer
 * Copyright (c) 2026 quatric
 *
 * This file is part of FFmpeg.
 */

/**
 * @file
 * Writes what Gericom's FastVideoDSEncoder writes and FastVideoDSPlayer
 * reads: a 28 byte header, the table of key frames the player starts from,
 * then for each video frame a size word, the frame padded to four bytes and
 * the audio blocks that have fallen due by the end of that frame.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "libavcodec/adpcm_fvds.h"

#include "avformat.h"
#include "avio_internal.h"
#include "mux.h"

#define FV_HEADER_SIZE 0x1C
#define FV_AUDIO_FRAME (2 * FVDS_ADPCM_BLOCK_SIZE)

typedef struct FVMuxContext {
    AVPacket *video;            /* the frame waiting for its audio */
    uint8_t *audio;             /* audio frames not yet written */
    unsigned audio_alloc;
    int audio_size;
    int video_stream;
    int audio_stream;
    AVRational rate;
    uint32_t frame_count;
    int64_t audio_frames;       /* audio frames written */
    FVDSADPCMState state[2];    /* where the coder of each channel stands */
    uint32_t *keys;             /* frame number and offset of each key frame */
    unsigned keys_alloc;
    unsigned nb_keys;
} FVMuxContext;

static int fv_write_header(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;
    AVStream *vst = NULL, *ast = NULL;

    c->video_stream = c->audio_stream = -1;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (vst || st->codecpar->codec_id != AV_CODEC_ID_FASTVIDEO)
                return AVERROR(EINVAL);
            vst = st;
            c->video_stream = i;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (ast || st->codecpar->codec_id != AV_CODEC_ID_ADPCM_IMA_FVDS)
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
    /* the player never looks at the two audio fields of the header */
    if (ast && (ast->codecpar->ch_layout.nb_channels != 2 ||
                ast->codecpar->sample_rate != FVDS_ADPCM_SAMPLE_RATE)) {
        av_log(s, AV_LOG_ERROR, "FV audio is stereo at %d Hz\n", FVDS_ADPCM_SAMPLE_RATE);
        return AVERROR(EINVAL);
    }

    c->rate = av_guess_frame_rate(s, vst, NULL);
    if (c->rate.num <= 0 || c->rate.den <= 0)
        c->rate = av_inv_q(vst->time_base);
    if (c->rate.num <= 0 || c->rate.den <= 0)
        return AVERROR(EINVAL);

    c->video = av_packet_alloc();
    if (!c->video)
        return AVERROR(ENOMEM);

    avio_wl32(s->pb, MKTAG('F', 'V', 'D', 'S'));
    avio_wl16(s->pb, vst->codecpar->width);
    avio_wl16(s->pb, vst->codecpar->height);
    avio_wl32(s->pb, c->rate.num);
    avio_wl32(s->pb, c->rate.den);
    avio_wl16(s->pb, FVDS_ADPCM_SAMPLE_RATE);
    avio_wl16(s->pb, 2);
    avio_wl32(s->pb, 0); /* frames, patched */
    avio_wl32(s->pb, 0); /* key frames, patched; their table goes in here */
    return 0;
}

static int fv_write_record(AVFormatContext *s)
{
    static const int16_t silence[FVDS_ADPCM_SAMPLES];
    FVMuxContext *c = s->priv_data;
    const int video_size = c->video->size;
    const int aligned_size = FFALIGN(video_size, 4);
    /* whole audio frames up to the end of this video frame, less those written */
    const int64_t due = av_rescale_rnd((int64_t)FVDS_ADPCM_SAMPLE_RATE * (c->frame_count + 1LL),
                                       c->rate.den, c->rate.num, AV_ROUND_DOWN) /
                        FVDS_ADPCM_SAMPLES - c->audio_frames;
    int used = 0;

    if (video_size <= 0 || aligned_size > 0x1FFFF || due < 0 || due > 0x7FFF ||
        c->frame_count == UINT32_MAX)
        return AVERROR(EINVAL);

    if (!(AV_RL16(c->video->data) & 0x8000)) {
        uint32_t *keys = av_fast_realloc(c->keys, &c->keys_alloc,
                                         (c->nb_keys + 1) * 2 * sizeof(*keys));
        if (!keys)
            return AVERROR(ENOMEM);
        c->keys = keys;
        c->keys[2 * c->nb_keys]     = c->frame_count;
        c->keys[2 * c->nb_keys + 1] = avio_tell(s->pb) - FV_HEADER_SIZE;
        c->nb_keys++;
    }

    avio_wl32(s->pb, aligned_size | ((uint32_t)due << 17));
    avio_write(s->pb, c->video->data, video_size);
    ffio_fill(s->pb, 0, aligned_size - video_size);
    for (int i = 0; i < due; i++) {
        uint8_t block[FV_AUDIO_FRAME];
        const uint8_t *frame = block;

        if (c->audio_size - used >= FV_AUDIO_FRAME) {
            frame = c->audio + used;
            used += FV_AUDIO_FRAME;
            for (int ch = 0; ch < 2; ch++)
                fvds_adpcm_decode_block(frame + ch * FVDS_ADPCM_BLOCK_SIZE, NULL,
                                        &c->state[ch]);
        } else {
            /* past the end of the audio, or without any, the reference
             * encoder goes on coding silence */
            for (int ch = 0; ch < 2; ch++)
                fvds_adpcm_encode_block(silence, block + ch * FVDS_ADPCM_BLOCK_SIZE,
                                        &c->state[ch]);
        }
        avio_write(s->pb, frame, FV_AUDIO_FRAME);
    }
    c->audio_size -= used;
    memmove(c->audio, c->audio + used, c->audio_size);
    c->audio_frames += due;
    c->frame_count++;
    av_packet_unref(c->video);
    return 0;
}

static int fv_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    FVMuxContext *c = s->priv_data;
    int ret;

    if (pkt->stream_index == c->video_stream) {
        /* a frame is written when the next one comes, by which time the
         * audio that goes with it has */
        if (c->video->size && (ret = fv_write_record(s)) < 0)
            return ret;
        return av_packet_ref(c->video, pkt);
    }
    if (pkt->stream_index == c->audio_stream) {
        uint8_t *audio;
        if (pkt->size <= 0 || pkt->size % FV_AUDIO_FRAME ||
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
    int table_size, ret;

    if (c->video->size && (ret = fv_write_record(s)) < 0)
        return ret;
    if (c->nb_keys > (INT_MAX - FV_HEADER_SIZE) / 8)
        return AVERROR(EINVAL);
    table_size = c->nb_keys * 8;
    if (!c->nb_keys)
        av_log(s, AV_LOG_WARNING, "No key frame: FastVideoDSPlayer cannot open this file\n");

    /* only now is the size of the key frame table known */
    end = avio_tell(s->pb);
    if (table_size) {
        if ((ret = ff_format_shift_data(s, FV_HEADER_SIZE, table_size)) < 0)
            return ret;
        avio_seek(s->pb, FV_HEADER_SIZE, SEEK_SET);
        for (unsigned i = 0; i < c->nb_keys; i++) {
            avio_wl32(s->pb, c->keys[2 * i]);
            avio_wl32(s->pb, c->keys[2 * i + 1] + FV_HEADER_SIZE + table_size);
        }
    }
    avio_seek(s->pb, 0x14, SEEK_SET);
    avio_wl32(s->pb, c->frame_count);
    avio_wl32(s->pb, c->nb_keys);
    avio_seek(s->pb, end + table_size, SEEK_SET);
    return 0;
}

static void fv_deinit(AVFormatContext *s)
{
    FVMuxContext *c = s->priv_data;

    av_packet_free(&c->video);
    av_freep(&c->audio);
    av_freep(&c->keys);
    c->audio_alloc = c->audio_size = 0;
    c->keys_alloc = c->nb_keys = 0;
}

const FFOutputFormat ff_fv_muxer = {
    .p.name         = "fv",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FastVideoDS (.fv)"),
    .p.extensions   = "fv",
    .p.audio_codec  = AV_CODEC_ID_ADPCM_IMA_FVDS,
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
