/*
 * Nintendo Pokemon GBA FVMV decoder using the cartridge's ARM image
 * Copyright (c) 2026 the FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <unicorn/unicorn.h>
#include <unicorn/arm.h>

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define ROM_BASE    0x08000000U
#define IWRAM_BASE  0x03000000U
#define EWRAM_BASE  0x02000000U
#define IWRAM_SIZE  0x8000
#define FRAME_SIZE  (240 * 160 * 2)
#define FRAME0      (EWRAM_BASE + 8)
#define FRAME1      (FRAME0 + FRAME_SIZE)
#define WORK        (EWRAM_BASE + 0x26000)
#define PCM_WORK    (EWRAM_BASE + 0x28000)
#define STACK       (EWRAM_BASE + 0x3f000)
#define RETURN_PC   (IWRAM_BASE + 0x7ff0)

typedef struct FVMVDecodeContext {
    uc_engine *uc;
    uint32_t src, dst;
} FVMVDecodeContext;

static int uc_error(AVCodecContext *avctx, uc_err err, const char *what)
{
    av_log(avctx, AV_LOG_ERROR, "%s: %s\n", what, uc_strerror(err));
    return AVERROR_EXTERNAL;
}

static int fvmv_call(AVCodecContext *avctx, uint32_t pc,
                     uint32_t r0, uint32_t r1, uint32_t r2)
{
    FVMVDecodeContext *s = avctx->priv_data;
    uint32_t cpsr = 0x1f, sp = STACK, lr = RETURN_PC, out_pc;
    uc_err err;

#define REG(id, value) do {                                            \
    if ((err = uc_reg_write(s->uc, id, &(value))) != UC_ERR_OK)        \
        return uc_error(avctx, err, "writing ARM register");           \
} while (0)
    REG(UC_ARM_REG_CPSR, cpsr);
    REG(UC_ARM_REG_R0, r0);
    REG(UC_ARM_REG_R1, r1);
    REG(UC_ARM_REG_R2, r2);
    REG(UC_ARM_REG_SP, sp);
    REG(UC_ARM_REG_LR, lr);
#undef REG
    err = uc_emu_start(s->uc, pc, RETURN_PC, 0, 20000000);
    if (err != UC_ERR_OK)
        return uc_error(avctx, err, "executing FVMV ARM decoder");
    if ((err = uc_reg_read(s->uc, UC_ARM_REG_PC, &out_pc)) != UC_ERR_OK)
        return uc_error(avctx, err, "reading ARM PC");
    return out_pc == RETURN_PC ? 0 : AVERROR_INVALIDDATA;
}

static av_cold int fvmv_common_init(AVCodecContext *avctx)
{
    FVMVDecodeContext *s = avctx->priv_data;
    uc_err err;

    if (avctx->extradata_size != IWRAM_SIZE)
        return AVERROR_INVALIDDATA;
    if ((err = uc_open(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN,
                       &s->uc)) != UC_ERR_OK)
        return uc_error(avctx, err, "opening Unicorn");
    if ((err = uc_mem_map(s->uc, EWRAM_BASE, 0x40000, UC_PROT_ALL)) != UC_ERR_OK ||
        (err = uc_mem_map(s->uc, IWRAM_BASE, IWRAM_SIZE, UC_PROT_ALL)) != UC_ERR_OK ||
        (err = uc_mem_map(s->uc, ROM_BASE, 0x2000000, UC_PROT_ALL)) != UC_ERR_OK ||
        (err = uc_mem_write(s->uc, IWRAM_BASE, avctx->extradata,
                            IWRAM_SIZE)) != UC_ERR_OK)
        return uc_error(avctx, err, "mapping FVMV ARM memory");
    s->src = FRAME0;
    s->dst = FRAME1;
    return 0;
}

static av_cold int fvmv_video_init(AVCodecContext *avctx)
{
    int ret = fvmv_common_init(avctx);
    if (ret < 0)
        return ret;
    if (avctx->width != 240 || avctx->height != 160)
        return AVERROR_INVALIDDATA;
    avctx->pix_fmt = AV_PIX_FMT_BGR555LE;
    return 0;
}

static int fvmv_video_decode(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *pkt)
{
    FVMVDecodeContext *s = avctx->priv_data;
    uint32_t input;
    uint32_t obj[16];
    static const uint8_t padding[AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t row[240 * 2];
    uc_err err;
    int ret;

    if (pkt->size < 8 || pkt->size > 0x100000 || pkt->pos < 0 ||
        pkt->pos + pkt->size + sizeof(padding) > 0x2000000)
        return AVERROR_INVALIDDATA;
    input = ROM_BASE + pkt->pos;
    obj[0] = s->src;
    obj[1] = s->dst;
    obj[2] = input;
    obj[3] = 240;
    obj[4] = 160;
    obj[5] = 0;
    memset(obj + 6, 0, sizeof(obj) - 6 * sizeof(*obj));
    if ((err = uc_mem_write(s->uc, input, pkt->data, pkt->size)) != UC_ERR_OK ||
        (err = uc_mem_write(s->uc, input + pkt->size, padding,
                            sizeof(padding))) != UC_ERR_OK ||
        (err = uc_mem_write(s->uc, WORK, obj, sizeof(obj))) != UC_ERR_OK)
        return uc_error(avctx, err, "loading FVMV video packet");
    if ((ret = fvmv_call(avctx, IWRAM_BASE + 0x5fe8, WORK, 0, 0)) < 0)
        return ret;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int y = 0; y < 160; y++) {
        if ((err = uc_mem_read(s->uc, s->dst + y * sizeof(row),
                               row, sizeof(row))) != UC_ERR_OK)
            return uc_error(avctx, err, "reading FVMV frame");
        memcpy(frame->data[0] + y * frame->linesize[0], row, sizeof(row));
    }
    FFSWAP(uint32_t, s->src, s->dst);
    frame->pict_type = pkt->pts ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    if (!pkt->pts)
        frame->flags |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;
    return pkt->size;
}

static av_cold int fvmv_audio_init(AVCodecContext *avctx)
{
    int ret = fvmv_common_init(avctx);
    if (ret < 0)
        return ret;
    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    avctx->sample_fmt = AV_SAMPLE_FMT_U8;
    avctx->sample_rate = 65536;
    return 0;
}

static int fvmv_audio_decode(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *pkt)
{
    FVMVDecodeContext *s = avctx->priv_data;
    uint8_t pcm[1024];
    uc_err err;
    int blocks, ret;

    if (!pkt->size || pkt->size % 40 || pkt->size > 0x100000 ||
        pkt->pos < 0 || pkt->pos + pkt->size > 0x2000000)
        return AVERROR_INVALIDDATA;
    blocks = pkt->size / 40;
    frame->nb_samples = blocks * 1024;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    if ((err = uc_mem_write(s->uc, ROM_BASE + pkt->pos,
                            pkt->data, pkt->size)) != UC_ERR_OK)
        return uc_error(avctx, err, "loading FVMV audio packet");
    for (int block = 0; block < blocks; block++) {
        if ((ret = fvmv_call(avctx, IWRAM_BASE + 0x1b88,
                             ROM_BASE + pkt->pos + block * 40, WORK, 0)) < 0 ||
            (ret = fvmv_call(avctx, IWRAM_BASE + 0x1d18,
                             WORK, PCM_WORK, 1024)) < 0)
            return ret;
        if ((err = uc_mem_read(s->uc, PCM_WORK, pcm, sizeof(pcm))) != UC_ERR_OK)
            return uc_error(avctx, err, "reading FVMV audio");
        for (int i = 0; i < 1024; i++)
            frame->data[0][block * 1024 + i] = pcm[i] ^ 0x80;
    }
    *got_frame = 1;
    return pkt->size;
}

static av_cold int fvmv_close(AVCodecContext *avctx)
{
    FVMVDecodeContext *s = avctx->priv_data;
    if (s->uc)
        uc_close(s->uc);
    s->uc = NULL;
    return 0;
}

const FFCodec ff_fvmv_decoder = {
    .p.name         = "fvmv",
    CODEC_LONG_NAME("Nintendo Pokemon GBA FVMV Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FVMV,
    .priv_data_size = sizeof(FVMVDecodeContext),
    .init           = fvmv_video_init,
    .close          = fvmv_close,
    FF_CODEC_DECODE_CB(fvmv_video_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    CODEC_PIXFMTS(AV_PIX_FMT_BGR555LE),
};

const FFCodec ff_fvmv_audio_decoder = {
    .p.name         = "fvmv_audio",
    CODEC_LONG_NAME("Nintendo Pokemon GBA FVMV audio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_FVMV_AUDIO,
    .priv_data_size = sizeof(FVMVDecodeContext),
    .init           = fvmv_audio_init,
    .close          = fvmv_close,
    FF_CODEC_DECODE_CB(fvmv_audio_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_U8),
};
