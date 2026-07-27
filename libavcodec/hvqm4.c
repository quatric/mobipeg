/*
 * HVQM4 Video Decoder
 * Copyright (c) 2019 Tillmann Karras
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

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

/* Pull in hcs/Tillmann Karras' bit-accurate HVQM4 1.3/1.5 decoder as a single
 * translation unit. NATIVE selects the built-in (non-ABI-shim) decode path and
 * skips the standalone symbols.inc / yoloader.c bits; HVQM4_FFMPEG drops its
 * main(). */
#define NATIVE
#define HVQM4_FFMPEG
#include "h4m_audio_decode.c"

/* Frame type tags as they appear in the .h4m stream (see the core decoder). */
#define HVQM4_I_FRAME 0x10
#define HVQM4_P_FRAME 0x20
#define HVQM4_B_FRAME 0x30

typedef struct
{
    Player player;
    VideoState *state;
} Hvqm4DecodeContext;

static av_cold int hvqm4_init(AVCodecContext *ctx)
{
    Hvqm4DecodeContext *h4m = ctx->priv_data;
    Player *player = &h4m->player;
    SeqObj *seqobj = &player->seqobj;

    HVQM4InitDecoder();
    seqobj->width = ctx->width;
    seqobj->height = ctx->height;
    if (ctx->extradata_size < 2)
        return AVERROR_INVALIDDATA;
    seqobj->h_samp = ctx->extradata[0];
    seqobj->v_samp = ctx->extradata[1];
    h4m->state = av_malloc(HVQM4BuffSize(seqobj));
    if (!h4m->state)
        return AVERROR(ENOMEM);
    HVQM4SetBuffer(seqobj, h4m->state);
    decv_init(player);

    if (seqobj->h_samp == 2 && seqobj->v_samp == 2)
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    else {
        av_log(ctx, AV_LOG_ERROR, "pixel format not implemented: h_samp:%u v_samp:%u\n", seqobj->h_samp, seqobj->v_samp);
        return AVERROR_PATCHWELCOME;
    }
    ctx->color_range = AVCOL_RANGE_JPEG; // just a guess

    return 0;
}

static av_cold int hvqm4_close(AVCodecContext *ctx)
{
    Hvqm4DecodeContext *h4m = ctx->priv_data;
    /* player->past/present/future are allocated inside decv_init(). */
    free(h4m->player.past);
    free(h4m->player.present);
    free(h4m->player.future);
    av_freep(&h4m->state);
    return 0;
}

static int hvqm4_decode(AVCodecContext *ctx, AVFrame *frame,
                        int *got_frame, AVPacket *pkt)
{
    Hvqm4DecodeContext *h4m = ctx->priv_data;
    Player *player = &h4m->player;
    SeqObj *seqobj = &player->seqobj;
    int ret;

    if (pkt->size < 6)
        return AVERROR_INVALIDDATA;

    uint16_t frame_type = AV_RB16(pkt->data);
    frame->pts = pkt->pts = AV_RB32(pkt->data + 2);

    if ((ret = ff_reget_buffer(ctx, frame, 0)) < 0)
        return ret;
    frame->pts = pkt->pts;

    if (frame_type != HVQM4_B_FRAME)
        FFSWAP(void *, player->past, player->future);

    switch (frame_type)
    {
        case HVQM4_I_FRAME:
            HVQM4DecodeIpic(seqobj, pkt->data + 6, player->present);
            frame->pict_type = AV_PICTURE_TYPE_I;
            break;
        case HVQM4_P_FRAME:
            HVQM4DecodePpic(seqobj, pkt->data + 6, player->present, player->past);
            frame->pict_type = AV_PICTURE_TYPE_P;
            break;
        case HVQM4_B_FRAME:
            HVQM4DecodeBpic(seqobj, pkt->data + 6, player->present, player->past, player->future);
            frame->pict_type = AV_PICTURE_TYPE_B;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "unknown frame type\n");
            return AVERROR_INVALIDDATA;
    }
    if (frame->pict_type == AV_PICTURE_TYPE_I)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;

    if (frame_type != HVQM4_B_FRAME)
        FFSWAP(void *, player->present, player->future);

    if (pkt->dts > 0)
    {
        /* The core decoder writes contiguous planar YUV into its own buffer;
         * ffmpeg planes may be strided, so copy row-block by row-block. */
        uint8_t *ptr;
        if (frame_type == HVQM4_B_FRAME)
            ptr = player->present;
        else
            ptr = player->past;

        size_t y_plane_size = frame->width * frame->height;
        size_t uv_plane_size = y_plane_size / 4;
        int i;
        for (i = 0; i < frame->height; i++)
            memcpy(frame->data[0] + i * frame->linesize[0], ptr + i * frame->width, frame->width);
        ptr += y_plane_size;
        for (i = 0; i < frame->height / 2; i++)
            memcpy(frame->data[1] + i * frame->linesize[1], ptr + i * (frame->width / 2), frame->width / 2);
        ptr += uv_plane_size;
        for (i = 0; i < frame->height / 2; i++)
            memcpy(frame->data[2] + i * frame->linesize[2], ptr + i * (frame->width / 2), frame->width / 2);

        *got_frame = 1;
    }
    else
    {
        *got_frame = 0;
    }

    return pkt->size;
}

const FFCodec ff_hvqm4_decoder = {
    .p.name         = "hvqm4",
    CODEC_LONG_NAME("Hudson HVQM4 video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HVQM4,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(Hvqm4DecodeContext),
    .init           = hvqm4_init,
    .close          = hvqm4_close,
    FF_CODEC_DECODE_CB(hvqm4_decode),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
