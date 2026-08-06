/*
 * ADS-era (Majesco) GBA Video decoder
 * Copyright (c) 2026 the FFmpeg developers
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

/**
 * @file
 * Video half of the ADS-era GBA Video stack, the in-house codec Majesco used
 * before the GCC-era carts moved to ActImagine VX.
 *
 * A packet is one chunk: an 8-byte header, an LZMA-compressed codebook and an
 * LZMA-compressed plane of block indices covering several frames.
 *
 * A codebook always holds 256 entries and is planar *across* entries, not
 * within one: all 256 entries' luma first, then all their Cb, then all their
 * Cr. Chroma is subsampled 4:1 horizontally, with either one sample per block
 * row or a single sample for the whole block depending on the stream's mode
 * (see setup_geometry).
 *
 * Both the luma region and the chroma region are delta-coded: each is a
 * running sum modulo 256, which the ROM undoes in StreamBase::UnPredictLuminance
 * and StreamBase::UnPredictChrominance. The two chroma components are summed as
 * one contiguous run, Cb flowing into Cr.
 *
 * Frames are then intra: a frame is a plain lookup of absolute pixels out of
 * that codebook, with no temporal prediction at all.
 *
 * The colour transform is exact, taken from the IWRAM converter at 0x030004f8:
 *     R = clip(Y + 2*Cr)   G = clip(Y - Cr - Cb/2)   B = clip(Y + 2*Cb)
 * with signed chroma.
 */

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "adslzma.h"

#define GRID_W 60               /* every known stream is 60 blocks wide */

#define NB_ENTRIES 256

typedef struct ADSVideoContext {
    int blk_w, blk_h;
    int grid_h;
    int chroma_rows;            /* chroma samples per block, per component */
    int luma_region;            /* bytes of luma in the codebook */

    uint8_t *luma;              /* expanded planes, one byte per sample */
    int8_t  *cb, *cr;

    uint8_t *codebook;
    int codebook_size;

    uint8_t *index;             /* block indices for the whole chunk */
    int index_size;

    int nb_frames;              /* frames carried by the current chunk */
    int next_frame;             /* how many of them we have emitted */
    int64_t chunk_pts;          /* timestamp of the chunk's first frame */
} ADSVideoContext;

static av_cold int ads_video_init(AVCodecContext *avctx)
{
    avctx->pix_fmt = AV_PIX_FMT_RGB24;
    return 0;
}

static av_cold int ads_video_close(AVCodecContext *avctx)
{
    ADSVideoContext *s = avctx->priv_data;

    av_freep(&s->luma);
    av_freep(&s->cb);
    av_freep(&s->cr);
    av_freep(&s->codebook);
    av_freep(&s->index);
    return 0;
}

/* ROM 0xea70 in the Hydrogen build, and the same switch in every IWRAM
 * routine that walks pixels: the mode nibble picks the block size. */
static const uint8_t block_dims[8][2] = {
    { 4, 4 }, { 4, 4 }, { 4, 3 }, { 4, 3 },
    { 3, 3 }, { 3, 3 }, { 4, 2 }, { 4, 2 },
};

/**
 * Block geometry comes from the mode nibble in the chunk header; the grid is
 * always 60 blocks wide, so the height follows from the index plane's size.
 */
static int setup_geometry(AVCodecContext *avctx, int per_frame, int mode)
{
    ADSVideoContext *s = avctx->priv_data;
    int grid_h, blk_w = block_dims[mode][0], blk_h = block_dims[mode][1];

    if (per_frame <= 0 || per_frame % GRID_W)
        return AVERROR_INVALIDDATA;

    grid_h = per_frame / GRID_W;

    if (s->grid_h == grid_h && s->blk_h == blk_h && s->luma)
        return 0;

    s->blk_w  = blk_w;
    s->blk_h  = blk_h;
    s->grid_h = grid_h;

    av_freep(&s->luma);
    av_freep(&s->cb);
    av_freep(&s->cr);

    s->luma = av_calloc(GRID_W * blk_w, grid_h * blk_h);
    s->cb   = av_calloc(GRID_W, grid_h * blk_h);
    s->cr   = av_calloc(GRID_W, grid_h * blk_h);
    if (!s->luma || !s->cb || !s->cr)
        return AVERROR(ENOMEM);

    return ff_set_dimensions(avctx, GRID_W * blk_w, grid_h * blk_h);
}

/** Running sum modulo 256, the inverse of the encoder's byte-wise delta. */
static void undelta(uint8_t *p, int len)
{
    unsigned acc = 0;

    for (int i = 0; i < len; i++)
        p[i] = acc = (acc + p[i]) & 0xff;
}

static int decode_chunk(AVCodecContext *avctx, const AVPacket *avpkt)
{
    ADSVideoContext *s = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int size_a, size_b, ret, per_frame;

    if (avpkt->size < 8)
        return AVERROR_INVALIDDATA;

    s->nb_frames = AV_RL32(buf) >> 16;
    size_a       = (AV_RL32(buf + 4) & 0x1FFF) * 4;
    size_b       = (AV_RL32(buf + 4) >> 13)    * 4;

    if (!s->nb_frames || !size_a || !size_b ||
        8 + (int64_t)size_a + size_b > avpkt->size)
        return AVERROR_INVALIDDATA;

    av_freep(&s->codebook);
    av_freep(&s->index);

    ret = ff_ads_lzma_decode_blob(buf + 8, size_a,
                                  &s->codebook, &s->codebook_size);
    if (ret < 0)
        return ret;

    ret = ff_ads_lzma_decode_blob(buf + 8 + size_a, size_b,
                                  &s->index, &s->index_size);
    if (ret < 0)
        return ret;

    per_frame = s->index_size / s->nb_frames;
    ret = setup_geometry(avctx, per_frame, AV_RL32(buf) & 7);
    if (ret < 0)
        return ret;

    /* The mode's low bit picks the chroma resolution: odd modes store one
     * Cb/Cr per block row, even modes a single pair for the whole block. */
    s->luma_region = NB_ENTRIES * s->blk_w * s->blk_h;
    s->chroma_rows = (AV_RL32(buf) & 1) ? s->blk_h : 1;

    if (s->codebook_size < s->luma_region + 2 * NB_ENTRIES * s->chroma_rows) {
        av_log(avctx, AV_LOG_ERROR,
               "codebook holds %d bytes, which is not a %dx%d codebook\n",
               s->codebook_size, s->blk_w, s->blk_h);
        return AVERROR_INVALIDDATA;
    }

    /* Undo the delta coding: luma is one run, Cb and Cr together are another
     * (StreamBase::UnPredictLuminance / UnPredictChrominance). */
    undelta(s->codebook, s->luma_region);
    undelta(s->codebook + s->luma_region, 2 * NB_ENTRIES * s->chroma_rows);

    s->next_frame = 0;
    s->chunk_pts  = avpkt->pts;

    return 0;
}

/**
 * Expand one frame out of the codebook. Frames are intra: once the codebook
 * has been un-delta'd its entries are absolute pixels, so a frame is a plain
 * lookup with no dependency on the frame before it.
 */
static void expand_frame(ADSVideoContext *s, const uint8_t *idx)
{
    const int bw = s->blk_w, bh = s->blk_h, cr = s->chroma_rows;
    const int width = GRID_W * bw;
    const uint8_t *cb_base = s->codebook + s->luma_region;
    const uint8_t *cr_base = cb_base + NB_ENTRIES * cr;

    for (int by = 0; by < s->grid_h; by++) {
        for (int bx = 0; bx < GRID_W; bx++) {
            unsigned e = idx[by * GRID_W + bx];
            const uint8_t *l = s->codebook + e * bw * bh;

            for (int j = 0; j < bh; j++) {
                int row = (by * bh + j) * width + bx * bw;
                int c   = (by * bh + j) * GRID_W + bx;
                int k   = e * cr + (cr == 1 ? 0 : j);

                for (int i = 0; i < bw; i++)
                    s->luma[row + i] = l[j * bw + i];

                s->cb[c] = (int8_t)cb_base[k];
                s->cr[c] = (int8_t)cr_base[k];
            }
        }
    }
}

static int emit_frame(AVCodecContext *avctx, AVFrame *frame)
{
    ADSVideoContext *s = avctx->priv_data;
    int ret;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    for (int y = 0; y < avctx->height; y++) {
        uint8_t *dst = frame->data[0] + y * frame->linesize[0];

        for (int x = 0; x < avctx->width; x++) {
            int luma = s->luma[y * avctx->width + x];
            int cb   = s->cb[y * GRID_W + x / s->blk_w];
            int cr   = s->cr[y * GRID_W + x / s->blk_w];

            *dst++ = av_clip_uint8(luma + 2 * cr);
            *dst++ = av_clip_uint8(luma - cr - (cb >> 1));
            *dst++ = av_clip_uint8(luma + 2 * cb);
        }
    }

    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags    |= AV_FRAME_FLAG_KEY;
    return 0;
}

static int ads_video_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    ADSVideoContext *s = avctx->priv_data;
    int ret, per_frame;

    while (s->next_frame >= s->nb_frames) {
        AVPacket *avpkt = av_packet_alloc();

        if (!avpkt)
            return AVERROR(ENOMEM);
        ret = ff_decode_get_packet(avctx, avpkt);
        if (ret >= 0)
            ret = decode_chunk(avctx, avpkt);
        av_packet_free(&avpkt);
        if (ret < 0)
            return ret;
    }

    per_frame = s->index_size / s->nb_frames;
    expand_frame(s, s->index + s->next_frame * per_frame);

    ret = emit_frame(avctx, frame);
    if (ret < 0)
        return ret;

    /* A chunk carries several frames but only one timestamp, so space them
     * out here; otherwise every frame in a chunk shares a pts. */
    if (s->chunk_pts != AV_NOPTS_VALUE)
        frame->pts = s->chunk_pts + s->next_frame;
    frame->duration = 1;
    s->next_frame++;

    return 0;
}

static void ads_video_flush(AVCodecContext *avctx)
{
    ADSVideoContext *s = avctx->priv_data;

    s->nb_frames = s->next_frame = 0;
}

const FFCodec ff_ads_gba_decoder = {
    .p.name         = "ads_gba",
    CODEC_LONG_NAME("ADS-era GBA Video (Majesco)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ADS_GBA,
    .priv_data_size = sizeof(ADSVideoContext),
    .init           = ads_video_init,
    .close          = ads_video_close,
    FF_CODEC_RECEIVE_FRAME_CB(ads_video_receive_frame),
    .flush          = ads_video_flush,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
