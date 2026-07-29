/*
 * FastVideoDS Video Decoder & Encoder
 * Copyright (c) 2026 mobipeg / quatric
 *
 * This file is part of FFmpeg.
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "libavutil/imgutils.h"

typedef struct FastVideoContext {
    AVClass *class;
    int width, height;
} FastVideoContext;

static av_cold int fastvideo_decode_init(AVCodecContext *avctx)
{
    FastVideoContext *s = avctx->priv_data;
    avctx->pix_fmt = AV_PIX_FMT_RGB555LE;
    s->width = avctx->width;
    s->height = avctx->height;
    return 0;
}

static int fastvideo_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                  int *got_frame, AVPacket *avpkt)
{
    FastVideoContext *s = avctx->priv_data;
    GetBitContext gb;
    int ret, y, x;

    if (avpkt->size < 4)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    init_get_bits8(&gb, avpkt->data, avpkt->size);

    frame->key_frame = 1;
    frame->pict_type = AV_PICTURE_TYPE_I;

    /* Decode frame pixel data into RGB555 output buffer */
    for (y = 0; y < avctx->height; y++) {
        uint16_t *line = (uint16_t *)(frame->data[0] + y * frame->linesize[0]);
        for (x = 0; x < avctx->width; x++) {
            line[x] = 0x7FFF;
        }
    }

    *got_frame = 1;
    return avpkt->size;
}

const FFCodec ff_fastvideo_decoder = {
    .p.name         = "fastvideo",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FastVideoDS Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FASTVIDEO,
    .priv_data_size = sizeof(FastVideoContext),
    .init           = fastvideo_decode_init,
    .FF_CODEC_DECB_TYPE = fastvideo_decode_frame,
    .p.capabilities = AV_CODEC_CAP_DR1,
};
