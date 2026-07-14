/*
 * RocketVideo (.rvid) video codec (encoder + decoder)
 * Copyright (c) 2026 mobipeg
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

/*
 * RocketVideo frames as used by RocketRobz's Vid2RVID / RocketVideoPlayer.
 * A frame is a DS bitmap: bmpMode 1 = 16bpp RGB555, bmpMode 2 = 16bpp RGB565
 * (packed the DS way, 6-bit green split across bit15 and bits5..9), bmpMode 0 =
 * 8bpp with a per-frame 256-entry 565-packed palette.  Frames may be Nintendo
 * LZ10 compressed and/or interlaced (each stored frame is one field; the player
 * keeps a persistent image and updates alternate scanlines).
 *
 * Container framing lives in the (de)muxer; here a packet is exactly one stored
 * frame.  Mode/interlace/compression flags travel in a 4-byte extradata blob
 * shared with the (de)muxer: [bmpMode, interlaced, compressed, 0].
 */
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/lfg.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "elbg.h"

#define RVID_BMP_8BPP  0
#define RVID_BMP_555   1
#define RVID_BMP_565   2
#define RVID_PAL_BYTES 0x200      /* 256 * u16 palette that precedes 8bpp data */

/* --------------------------------------------------------------------------
 * Nintendo LZ10 (0x10)
 * ------------------------------------------------------------------------ */
static int rvid_lz10_decompress(const uint8_t *src, int src_size,
                                uint8_t *dst, int dst_cap)
{
    int p = 0, o = 0, out_size;
    if (src_size < 4 || src[0] != 0x10)
        return AVERROR_INVALIDDATA;
    out_size = src[1] | (src[2] << 8) | (src[3] << 16);
    if (out_size > dst_cap)
        return AVERROR_INVALIDDATA;
    p = 4;
    while (o < out_size) {
        int flags, i;
        if (p >= src_size)
            return AVERROR_INVALIDDATA;
        flags = src[p++];
        for (i = 0; i < 8 && o < out_size; i++) {
            if (flags & 0x80) {
                int b0, b1, len, back, k;
                if (p + 2 > src_size)
                    return AVERROR_INVALIDDATA;
                b0 = src[p++]; b1 = src[p++];
                len  = (b0 >> 4) + 3;
                back = (((b0 & 0xF) << 8) | b1) + 1;
                if (back > o || o + len > out_size)
                    return AVERROR_INVALIDDATA;
                for (k = 0; k < len; k++, o++)
                    dst[o] = dst[o - back];
            } else {
                if (p >= src_size)
                    return AVERROR_INVALIDDATA;
                dst[o++] = src[p++];
            }
            flags = (flags << 1) & 0xFF;
        }
    }
    return out_size;
}

/* Greedy LZ10 compressor (port of rvid_lz.c / Vid2RVID lz77.cpp). Returns the
 * padded compressed length or a negative error if it would exceed out_cap. */
static int rvid_lz10_compress(const uint8_t *data, int data_size,
                              uint8_t *out, int out_cap)
{
    int dstoffs = 4, length = data_size, offs = 0;
    if (out_cap < 4)
        return AVERROR(ENOMEM);
    out[0] = 0x10;
    out[1] = data_size & 0xFF;
    out[2] = (data_size >> 8) & 0xFF;
    out[3] = (data_size >> 16) & 0xFF;

    while (1) {
        int headeroffs = dstoffs++, i;
        uint8_t header = 0;
        for (i = 0; i < 8; i++) {
            int comp = 0, back = 1, nr = 2;
            int maxnum = FFMIN(18, length - offs);
            int maxback = FFMIN(0x1000, offs);
            const uint8_t *dp = data + offs;
            const uint8_t *ptr = dp - 1;
            const uint8_t *minptr = dp - maxback;
            while (minptr <= ptr) {
                if (ptr[0] == dp[0] && ptr[1] == dp[1] && ptr[2] == dp[2]) {
                    int tmpnr = 3;
                    while (tmpnr < maxnum && ptr[tmpnr] == dp[tmpnr])
                        tmpnr++;
                    if (tmpnr > nr) {
                        if (offs + tmpnr > length) {
                            nr = length - offs;
                            back = (int)(dp - ptr);
                            break;
                        }
                        nr = tmpnr;
                        back = (int)(dp - ptr);
                        if (nr == maxnum)
                            break;
                    }
                }
                --ptr;
            }
            if (nr > 2) {
                if (dstoffs + 2 > out_cap)
                    return AVERROR(ENOMEM);
                offs += nr;
                out[dstoffs++] = (((back - 1) >> 8) & 0xF) | (((nr - 3) & 0xF) << 4);
                out[dstoffs++] = (back - 1) & 0xFF;
                comp = 1;
            } else {
                if (dstoffs + 1 > out_cap)
                    return AVERROR(ENOMEM);
                out[dstoffs++] = data[offs++];
            }
            header = (header << 1) | (comp & 1);
            if (offs >= length) {
                header <<= (7 - i);
                break;
            }
        }
        out[headeroffs] = header;
        if (offs >= length)
            break;
    }
    while (dstoffs & 3) {
        if (dstoffs >= out_cap)
            return AVERROR(ENOMEM);
        out[dstoffs++] = 0;
    }
    return dstoffs;
}

/* --------------------------------------------------------------------------
 * Pixel (un)packing
 * ------------------------------------------------------------------------ */
static av_always_inline void unpack_word(unsigned w, int bmp_mode, uint8_t *rgb)
{
    unsigned r5 = w & 0x1F, b5 = (w >> 10) & 0x1F, g;
    if (bmp_mode == RVID_BMP_555) {
        g = (w >> 5) & 0x1F;
        rgb[0] = (r5 << 3) | (r5 >> 2);
        rgb[1] = (g  << 3) | (g  >> 2);
        rgb[2] = (b5 << 3) | (b5 >> 2);
    } else {  /* 565-in-1555 (bmpMode 0 palette entries and bmpMode 2) */
        unsigned g6 = ((w >> 15) & 1) | (((w >> 5) & 0x1F) << 1);
        rgb[0] = (r5 << 3) | (r5 >> 2);
        rgb[1] = (g6 << 2) | (g6 >> 4);
        rgb[2] = (b5 << 3) | (b5 >> 2);
    }
}

static av_always_inline unsigned pack_word(int R, int G, int B, int bmp_mode)
{
    if (bmp_mode == RVID_BMP_555)
        return (R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | 0x8000;
    else {
        unsigned g6 = (G >> 2) & 0x3F;
        return (R >> 3) | ((B >> 3) << 10) |
               ((g6 & 1) << 15) | (((g6 >> 1) & 0x1F) << 5);
    }
}

/* --------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------ */
typedef struct RVIDDecCtx {
    int bmp_mode, interlaced, compressed;
    int vres, width, disp_h;
    uint8_t *persist;         /* RGB24 full display buffer for interlacing */
    uint8_t *scratch;         /* LZ10 decompress target */
    int frame_idx;
} RVIDDecCtx;

static av_cold int rvid_decode_init(AVCodecContext *avctx)
{
    RVIDDecCtx *c = avctx->priv_data;
    if (avctx->extradata_size < 3) {
        av_log(avctx, AV_LOG_ERROR, "rvid: missing extradata\n");
        return AVERROR_INVALIDDATA;
    }
    c->bmp_mode   = avctx->extradata[0];
    c->interlaced = avctx->extradata[1];
    c->compressed = avctx->extradata[2];
    c->width  = avctx->width;
    c->disp_h = avctx->height;
    c->vres   = c->interlaced ? c->disp_h / 2 : c->disp_h;
    if (c->width <= 0 || c->vres <= 0 || c->vres > 255)
        return AVERROR_INVALIDDATA;
    avctx->pix_fmt = AV_PIX_FMT_RGB24;

    c->scratch = av_malloc((size_t)c->width * c->vres * 2);
    if (!c->scratch)
        return AVERROR(ENOMEM);
    if (c->interlaced) {
        c->persist = av_mallocz((size_t)c->width * c->disp_h * 3);
        if (!c->persist)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static int rvid_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    RVIDDecCtx *c = avctx->priv_data;
    const int W = c->width, VR = c->vres;
    const uint8_t *px;         /* palette-index bytes or 16bpp words */
    const uint8_t *pal = NULL; /* 8bpp palette (raw, never compressed) */
    int ret, x, y, parity, field_stride = W * 3;
    uint8_t fieldrow[256 * 3];

    if (c->bmp_mode == RVID_BMP_8BPP) {
        if (avpkt->size < RVID_PAL_BYTES)
            return AVERROR_INVALIDDATA;
        pal = avpkt->data;
        if (c->compressed) {
            ret = rvid_lz10_decompress(avpkt->data + RVID_PAL_BYTES,
                                       avpkt->size - RVID_PAL_BYTES,
                                       c->scratch, W * VR);
            if (ret < 0)
                return ret;
            px = c->scratch;
        } else {
            if (avpkt->size < RVID_PAL_BYTES + W * VR)
                return AVERROR_INVALIDDATA;
            px = avpkt->data + RVID_PAL_BYTES;
        }
    } else {
        if (c->compressed) {
            ret = rvid_lz10_decompress(avpkt->data, avpkt->size,
                                       c->scratch, W * VR * 2);
            if (ret < 0)
                return ret;
            px = c->scratch;
        } else {
            if (avpkt->size < W * VR * 2)
                return AVERROR_INVALIDDATA;
            px = avpkt->data;
        }
    }

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    parity = c->interlaced ? (c->frame_idx & 1) : 0;

    for (y = 0; y < VR; y++) {
        uint8_t *dst;
        if (c->bmp_mode == RVID_BMP_8BPP) {
            const uint8_t *row = px + (size_t)y * W;
            for (x = 0; x < W; x++)
                unpack_word(AV_RL16(pal + row[x] * 2), RVID_BMP_565,
                            fieldrow + x * 3);
        } else {
            const uint8_t *row = px + (size_t)y * W * 2;
            for (x = 0; x < W; x++)
                unpack_word(AV_RL16(row + x * 2), c->bmp_mode, fieldrow + x * 3);
        }
        if (c->interlaced) {
            memcpy(c->persist + (size_t)(parity + 2 * y) * field_stride,
                   fieldrow, field_stride);
        } else {
            dst = frame->data[0] + (size_t)y * frame->linesize[0];
            memcpy(dst, fieldrow, field_stride);
        }
    }

    if (c->interlaced) {
        for (y = 0; y < c->disp_h; y++)
            memcpy(frame->data[0] + (size_t)y * frame->linesize[0],
                   c->persist + (size_t)y * field_stride, field_stride);
    }

    c->frame_idx++;
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;
    return avpkt->size;
}

static av_cold int rvid_decode_close(AVCodecContext *avctx)
{
    RVIDDecCtx *c = avctx->priv_data;
    av_freep(&c->persist);
    av_freep(&c->scratch);
    return 0;
}

const FFCodec ff_rvid_decoder = {
    .p.name         = "rvid",
    CODEC_LONG_NAME("RocketVideo (RVID)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_RVID,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(RVIDDecCtx),
    .init           = rvid_decode_init,
    .close          = rvid_decode_close,
    FF_CODEC_DECODE_CB(rvid_decode_frame),
};

/* --------------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------------ */
typedef struct RVIDEncCtx {
    const AVClass *class;
    int mode;         /* AVOption: 0=256(8bpp), 1=555, 2=565 */
    int interlaced;   /* AVOption */
    int compress;     /* AVOption */
    int dither;       /* AVOption */
    int vres, width, disp_h;
    int frame_idx;
    struct ELBGContext *elbg;
    AVLFG lfg;
    uint8_t *payload;   /* packed field payload (indices or 16bpp words)   */
    uint8_t *lzbuf;     /* LZ10 scratch                                    */
    int     *points;    /* ELBG input (npix*3)                             */
    int     *closest;   /* ELBG output index per pixel                     */
    int      codebook[256 * 3];
} RVIDEncCtx;

static av_cold int rvid_encode_init(AVCodecContext *avctx)
{
    RVIDEncCtx *c = avctx->priv_data;
    int bmp_mode = c->mode;
    size_t npix;

    if (avctx->width != 256 && avctx->width != 240) {
        av_log(avctx, AV_LOG_ERROR,
               "rvid: width must be 256 (DS) or 240 (GBA); scale first.\n");
        return AVERROR(EINVAL);
    }
    if (c->interlaced && (avctx->height & 1)) {
        av_log(avctx, AV_LOG_ERROR, "rvid: interlaced needs even height\n");
        return AVERROR(EINVAL);
    }
    c->width  = avctx->width;
    c->disp_h = avctx->height;
    c->vres   = c->interlaced ? avctx->height / 2 : avctx->height;
    if (c->vres < 1 || c->vres > 255) {
        av_log(avctx, AV_LOG_ERROR, "rvid: vRes %d out of range 1..255\n", c->vres);
        return AVERROR(EINVAL);
    }
    npix = (size_t)c->width * c->vres;

    c->payload = av_malloc(npix * 2);
    c->lzbuf   = av_malloc(npix * 2 + npix / 4 + 64);
    if (!c->payload || !c->lzbuf)
        return AVERROR(ENOMEM);
    if (bmp_mode == RVID_BMP_8BPP) {
        c->points  = av_malloc(npix * 3 * sizeof(int));
        c->closest = av_malloc(npix * sizeof(int));
        if (!c->points || !c->closest)
            return AVERROR(ENOMEM);
        av_lfg_init(&c->lfg, 1);
    }

    avctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = 4;
    avctx->extradata[0] = bmp_mode;
    avctx->extradata[1] = c->interlaced ? 1 : 0;
    avctx->extradata[2] = c->compress ? 1 : 0;
    avctx->extradata[3] = 0;
    return 0;
}

/* Checkerboard ordered dither (Vid2RVID): nudge R/B by +4, G by +gd on cells
 * where (x+y+parity)&1, clamped so it can't wrap. */
static av_always_inline int dither_ch(int v, int active, int delta)
{
    if (active && v >= delta && v < 0x100 - delta)
        v += delta;
    return v;
}

static int rvid_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    RVIDEncCtx *c = avctx->priv_data;
    const int W = c->width, VR = c->vres, bmp = c->mode;
    const int parity = c->interlaced ? 0 : (c->frame_idx & 1);
    const int gd = (bmp == RVID_BMP_565) ? 2 : 4;
    const int field0 = c->interlaced ? (c->frame_idx & 1) : 0;
    const int step   = c->interlaced ? 2 : 1;
    int raw_payload = W * VR * (bmp == RVID_BMP_8BPP ? 1 : 2);
    int x, y, ret, stored_size, pal_bytes = 0;
    uint8_t pal[RVID_PAL_BYTES];
    const uint8_t *stored;

    if (!frame)                 /* no delay: nothing to flush */
        return 0;

    if (bmp == RVID_BMP_8BPP) {
        int i, k, npix = W * VR;
        /* Gather dithered, 565-snapped pixels as ELBG points. */
        for (y = 0; y < VR; y++) {
            const uint8_t *src = frame->data[0] + (size_t)(field0 + step * y) * frame->linesize[0];
            for (x = 0; x < W; x++) {
                int R = src[x * 3 + 0], G = src[x * 3 + 1], B = src[x * 3 + 2];
                int a = (x + y + parity) & 1;
                if (c->dither) { R = dither_ch(R, a, 4); G = dither_ch(G, a, gd); B = dither_ch(B, a, 4); }
                R &= 0xF8; R |= R >> 5;
                G &= 0xFC; G |= G >> 6;
                B &= 0xF8; B |= B >> 5;
                i = (y * W + x) * 3;
                c->points[i] = R; c->points[i + 1] = G; c->points[i + 2] = B;
            }
        }
        memset(c->codebook, 0, sizeof(c->codebook));
        ret = avpriv_elbg_do(&c->elbg, c->points, 3, npix, c->codebook,
                             256, 1, c->closest, &c->lfg, 0);
        if (ret < 0)
            return ret;
        for (k = 0; k < 256; k++) {
            int R = av_clip_uint8(c->codebook[k * 3 + 0]);
            int G = av_clip_uint8(c->codebook[k * 3 + 1]);
            int B = av_clip_uint8(c->codebook[k * 3 + 2]);
            AV_WL16(pal + k * 2, pack_word(R, G, B, RVID_BMP_565));
        }
        for (i = 0; i < npix; i++)
            c->payload[i] = c->closest[i];
        pal_bytes = RVID_PAL_BYTES;
    } else {
        for (y = 0; y < VR; y++) {
            const uint8_t *src = frame->data[0] + (size_t)(field0 + step * y) * frame->linesize[0];
            uint8_t *dst = c->payload + (size_t)y * W * 2;
            for (x = 0; x < W; x++) {
                int R = src[x * 3 + 0], G = src[x * 3 + 1], B = src[x * 3 + 2];
                int a = (x + y + parity) & 1;
                if (c->dither) { R = dither_ch(R, a, 4); G = dither_ch(G, a, gd); B = dither_ch(B, a, 4); }
                AV_WL16(dst + x * 2, pack_word(R, G, B, bmp));
            }
        }
    }

    stored = c->payload;
    stored_size = raw_payload;
    if (c->compress) {
        int lz = rvid_lz10_compress(c->payload, raw_payload, c->lzbuf,
                                    raw_payload * 2 + raw_payload / 4 + 64);
        if (lz > 0 && lz < raw_payload) {
            stored = c->lzbuf;
            stored_size = lz;
        }
    }

    if ((ret = ff_get_encode_buffer(avctx, pkt, pal_bytes + stored_size, 0)) < 0)
        return ret;
    if (pal_bytes)
        memcpy(pkt->data, pal, pal_bytes);
    memcpy(pkt->data + pal_bytes, stored, stored_size);
    pkt->flags |= AV_PKT_FLAG_KEY;

    c->frame_idx++;
    *got_packet = 1;
    return 0;
}

static av_cold int rvid_encode_close(AVCodecContext *avctx)
{
    RVIDEncCtx *c = avctx->priv_data;
    avpriv_elbg_free(&c->elbg);
    av_freep(&c->payload);
    av_freep(&c->lzbuf);
    av_freep(&c->points);
    av_freep(&c->closest);
    return 0;
}

#define OFF(x) offsetof(RVIDEncCtx, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption rvid_options[] = {
    { "mode", "pixel mode", OFF(mode), AV_OPT_TYPE_INT, {.i64 = RVID_BMP_555}, 0, 2, VE, .unit = "mode" },
    { "rgb555", "16bpp RGB555 (unlimited color)", 0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_555}, 0, 0, VE, .unit = "mode" },
    { "rgb565", "16bpp RGB565 (max color)",       0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_565}, 0, 0, VE, .unit = "mode" },
    { "256",    "8bpp 256-color palette",         0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_8BPP}, 0, 0, VE, .unit = "mode" },
    { "interlaced", "store one field per frame",  OFF(interlaced), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VE },
    { "compress",   "Nintendo LZ10 compression",  OFF(compress),   AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, VE },
    { "dither",     "checkerboard ordered dither", OFF(dither),    AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, VE },
    { NULL }
};

static const AVClass rvid_encoder_class = {
    .class_name = "rvid encoder",
    .item_name  = av_default_item_name,
    .option     = rvid_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_rvid_encoder = {
    .p.name         = "rvid",
    CODEC_LONG_NAME("RocketVideo (RVID)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_RVID,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(RVIDEncCtx),
    .p.priv_class   = &rvid_encoder_class,
    .init           = rvid_encode_init,
    .close          = rvid_encode_close,
    FF_CODEC_ENCODE_CB(rvid_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_RGB24),
};
