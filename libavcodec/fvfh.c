/*
 * FastVideo DS (Metroid Prime Hunters: First Hunt) video decoder
 * Copyright (c) 2026 quatric
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
 * Decoder for the "FVDS" movies of the First Hunt demo.  Each frame is a
 * 15-bit BGR picture built from 8x8 blocks.  A block is a node of a binary
 * partition tree (8x8 down to 2x1 / 1x2) and is either copied from the
 * previous picture, motion compensated, motion compensated with a colour
 * offset, filled with one colour (with the edges blended into the
 * neighbouring pixels), or painted from a four colour palette.
 *
 * Three streams are read in parallel: motion vector indices (bytes), colours
 * (16 bit words) and control bits (32 bit words, MSB first).
 *
 * Several operations work on two pixels at a time with the same 32 bit
 * arithmetic as the original decoder.  That leaks one bit into bit 15 of
 * some pixels; it is kept in the reference picture, because later blocks
 * (the pair adds of the colour offset blocks) can carry it into a
 * neighbour, and masked off on output.
 */

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define MASK_HI32 0x7BDF7BDFu
#define MASK_LO32 0x04200420u
#define MASK_HI   0x7BDFu
#define MASK_LO   0x0420u
#define PAD_ROWS  16

typedef struct FVFHContext {
    uint16_t *buf[2];   /* padded reference pictures */
    int       cur;
    int       stride;
    int       width, height;
    int       frame_num;

    /* per frame stream state */
    const uint8_t *idx, *idx_end;
    const uint8_t *col, *col_end;
    const uint8_t *ctl, *ctl_end;
    uint32_t bits;
    int      nbits;
    int      err;
} FVFHContext;

static inline int get_bit(FVFHContext *s)
{
    int b;

    if (!s->nbits) {
        if (s->ctl_end - s->ctl < 4) {
            s->err = 1;
            return 0;
        }
        s->bits  = AV_RL32(s->ctl);
        s->ctl  += 4;
        s->nbits = 32;
    }
    b        = s->bits >> 31;
    s->bits <<= 1;
    s->nbits--;
    return b;
}

static inline unsigned get_col(FVFHContext *s)
{
    unsigned v;

    if (s->col_end - s->col < 2) {
        s->err = 1;
        return 0;
    }
    v = AV_RL16(s->col);
    s->col += 2;
    return v;
}

static inline int get_mv(FVFHContext *s)
{
    int i;

    if (s->idx >= s->idx_end) {
        s->err = 1;
        return 0;
    }
    i = *s->idx++;
    return ((i >> 4) - 8) * s->stride + (i & 15) - 8;
}

/* Copy a block from the previous picture, displaced by mv; with dc >= 0 the
 * colour is added to every pixel pair as a packed 32 bit add. */
static void copy_mc(FVFHContext *s, uint16_t *cur, const uint16_t *prev,
                    int p, int w, int h, int mv, int dc)
{
    const int stride = s->stride;

    for (int y = 0; y < h; y++) {
        uint16_t *dst       = cur  + p + y * stride;
        const uint16_t *src = prev + p + mv + y * stride;

        if (w == 1) {
            dst[0] = dc < 0 ? src[0] : (uint16_t)(src[0] + dc);
            continue;
        }
        for (int x = 0; x < w; x += 2) {
            uint32_t v = src[x] | (src[x + 1] << 16);

            if (dc >= 0)
                v += (uint32_t)dc * 0x10001u;
            dst[x]     = v & 0xFFFF;
            dst[x + 1] = v >> 16;
        }
    }
}

/* One step of the edge blend: a is the pixel across the edge. */
static void blend(uint32_t c_hi, uint32_t c_lo, unsigned a, int even,
                  uint16_t *n0, uint16_t *n_across, uint16_t *n_inside)
{
    uint32_t a_lo = a & MASK_LO32;
    uint32_t a_hi = a & MASK_HI32;
    uint32_t lo_center = even ? a_lo : c_lo;
    uint32_t lo_other  = even ? c_lo : a_lo;
    uint32_t r6, r7;

    r6  = lo_center | ((c_hi + a_hi) >> 1);
    *n0 = r6 & 0xFFFF;
    r6 &= MASK_HI32;
    r7  = lo_other | ((r6 + a_hi) >> 1);
    *n_across = r7 & 0xFFFF;
    r7  = lo_other | ((r6 + c_hi) >> 1);
    *n_inside = r7 & 0xFFFF;
}

static void fill_flat(FVFHContext *s, uint16_t *cur, int p, int w, int h,
                      unsigned c)
{
    const int stride = s->stride;
    uint32_t pair, c_hi, c_lo;

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            cur[p + y * stride + x] = c;
    if (w < 2 || h < 2)
        return;

    pair = c | (c << 16);
    c_hi = pair & MASK_HI32;
    c_lo = pair & MASK_LO32;

    for (int x = 0; x < w; x++) {
        uint16_t n0, na, ni;

        blend(c_hi, c_lo, cur[p - stride + x], !(x & 1), &n0, &na, &ni);
        cur[p + x]          = n0;
        cur[p - stride + x] = na;
        cur[p + stride + x] = ni;
    }
    for (int y = 0; y < h; y++) {
        uint16_t n0, na, ni;

        blend(c_hi, c_lo, cur[p + y * stride - 1], !(y & 1), &n0, &na, &ni);
        cur[p + y * stride]     = n0;
        cur[p + y * stride - 1] = na;
        cur[p + y * stride + 1] = ni;
    }
}

/* palette entry 2 and 3 are the thirds between the two stored colours */
static void make_palette(unsigned c0, unsigned c1, uint16_t pal[4])
{
    unsigned c2 = 0, c3 = 0;

    for (int sh = 10; sh >= 0; sh -= 5) {
        unsigned a = (c0 >> sh) & 31, b = (c1 >> sh) & 31;

        c2 = (c2 << 5) + (b + 2 * a + 1) / 3;
        c3 = (c3 << 5) + (a + 2 * b + 1) / 3;
    }
    pal[0] = c0;
    pal[1] = c1;
    pal[2] = c2;
    pal[3] = c3;
}

static void read_palette(FVFHContext *s, uint16_t pal[4], uint32_t *indices)
{
    unsigned c0 = get_col(s);
    unsigned c1 = get_col(s);
    unsigned i0 = get_col(s);
    unsigned i1 = get_col(s);

    make_palette(c0, c1, pal);
    *indices = i0 | (i1 << 16);
}

static void pal4(FVFHContext *s, uint16_t *cur, int p)
{
    uint16_t pal[4];
    uint32_t ip;

    read_palette(s, pal, &ip);
    for (int k = 0; k < 16; k++)
        cur[p + (k >> 2) * s->stride + (k & 3)] = pal[(ip >> (2 * k)) & 3];
}

/* Rows 1, 3, 5, 7 carry four indices each and are stretched horizontally;
 * the even rows are the average of their neighbours. */
static void pal8(FVFHContext *s, uint16_t *cur, int p)
{
    const int stride = s->stride;
    uint16_t pal[4];
    uint32_t ip;

    read_palette(s, pal, &ip);

    for (int g = 0; g < 4; g++) {
        uint16_t *row = cur + p + (1 + 2 * g) * stride;
        unsigned left = row[-1];
        unsigned c    = pal[(ip >> (8 * g)) & 3];

        row[1] = c;
        row[0] = (((left & MASK_HI) + (c & MASK_HI)) >> 1) | (c & MASK_LO);
        for (int j = 1; j < 4; j++) {
            unsigned n = pal[(ip >> (8 * g + 2 * j)) & 3];

            row[2 * j + 1] = n;
            row[2 * j]     = (((c & MASK_HI) + (n & MASK_HI)) >> 1) | (n & MASK_LO);
            c = n;
        }
    }

    for (int x = 0; x < 8; x += 2) {
        uint16_t *col = cur + p + x;
#define W32(y)   (col[(y) * stride] | (col[(y) * stride + 1] << 16))
#define S32(y, v) do { uint32_t t_ = (v); col[(y) * stride] = t_ & 0xFFFF; \
                       col[(y) * stride + 1] = t_ >> 16; } while (0)
        uint32_t r0 = W32(7), r6 = r0 & MASK_LO32, r1, r7;

        r0 &= MASK_HI32;
        r1  = W32(5); r7 = r1 & MASK_LO32; r1 &= MASK_HI32;
        S32(6, ((r0 + r1) >> 1) | r6);
        r0  = W32(3); r6 = r0 & MASK_LO32; r0 &= MASK_HI32;
        S32(4, ((r1 + r0) >> 1) | r7);
        r1  = W32(1); r7 = r1 & MASK_LO32; r1 &= MASK_HI32;
        S32(2, ((r0 + r1) >> 1) | r6);
        r0  = W32(-1) & MASK_HI32;
        S32(0, ((r1 + r0) >> 1) | r7);
#undef W32
#undef S32
    }
}

static void decode_node(FVFHContext *s, uint16_t *cur, const uint16_t *prev,
                        int p, int w, int h)
{
    const int stride = s->stride;
    int min = FFMIN(w, h), max = FFMAX(w, h);

    if (s->err)
        return;

    if (!get_bit(s)) {
        int mv = get_bit(s) ? get_mv(s) : 0;

        copy_mc(s, cur, prev, p, w, h, mv, -1);
        return;
    }

    if (min >= 2) {
        /* shapes with a choice of two splits: 8x8 8x4 4x8 4x4 / 8x2 2x8 4x2 2x4 2x2 */
        if (!get_bit(s)) {
            if (get_bit(s)) {   /* halve the width */
                decode_node(s, cur, prev, p, w / 2, h);
                decode_node(s, cur, prev, p + w / 2, w / 2, h);
            } else {            /* halve the height */
                decode_node(s, cur, prev, p, w, h / 2);
                decode_node(s, cur, prev, p + (h / 2) * stride, w, h / 2);
            }
            return;
        }
        if (min >= 4) {
            if (get_bit(s)) {
                int mv = get_mv(s);

                copy_mc(s, cur, prev, p, w, h, mv, get_col(s));
            } else if (!get_bit(s)) {
                fill_flat(s, cur, p, w, h, get_col(s));
            } else if (w == 8 && h == 8) {
                if (get_bit(s)) {
                    pal8(s, cur, p);
                } else {
                    pal4(s, cur, p);
                    pal4(s, cur, p + 4);
                    pal4(s, cur, p + 4 * stride);
                    pal4(s, cur, p + 4 * stride + 4);
                }
            } else if (w == 8) {
                pal4(s, cur, p);
                pal4(s, cur, p + 4);
            } else if (h == 8) {
                pal4(s, cur, p);
                pal4(s, cur, p + 4 * stride);
            } else {
                pal4(s, cur, p);
            }
        } else {
            if (get_bit(s)) {
                fill_flat(s, cur, p, w, h, get_col(s));
            } else {
                int mv = get_mv(s);

                copy_mc(s, cur, prev, p, w, h, mv, get_col(s));
            }
        }
        return;
    }

    if (max >= 4) {
        /* 8x1 1x8 4x1 1x4 */
        if (!get_bit(s)) {
            if (w > 1) {
                decode_node(s, cur, prev, p, w / 2, h);
                decode_node(s, cur, prev, p + w / 2, w / 2, h);
            } else {
                decode_node(s, cur, prev, p, w, h / 2);
                decode_node(s, cur, prev, p + (h / 2) * stride, w, h / 2);
            }
            return;
        }
        if (get_bit(s)) {
            fill_flat(s, cur, p, w, h, get_col(s));
        } else {
            int mv = get_mv(s);

            copy_mc(s, cur, prev, p, w, h, mv, get_col(s));
        }
        return;
    }

    /* 2x1 and 1x2 */
    if (!get_bit(s)) {
        int mv = get_mv(s);

        copy_mc(s, cur, prev, p, w, h, mv, get_col(s));
    } else if (!get_bit(s)) {
        fill_flat(s, cur, p, w, h, get_col(s));
    } else {
        unsigned a = get_col(s), b = get_col(s);

        cur[p] = a;
        cur[p + (w == 2 ? 1 : stride)] = b;
    }
}

static int fvfh_decode(AVCodecContext *avctx, AVFrame *frame,
                       int *got_frame, AVPacket *pkt)
{
    FVFHContext *s = avctx->priv_data;
    const uint8_t *data = pkt->data;
    uint32_t col_off, ctl_off;
    uint16_t *cur, *prev;
    int ret;

    if (pkt->size < 8)
        return AVERROR_INVALIDDATA;

    col_off = AV_RL32(data);
    ctl_off = AV_RL32(data + 4);
    if (col_off > pkt->size - 8 || ctl_off > pkt->size - 8 - col_off ||
        (ctl_off & 1))
        return AVERROR_INVALIDDATA;

    s->idx     = data + 8;
    s->idx_end = s->col = data + 8 + col_off;
    s->col_end = s->ctl = s->col + ctl_off;
    s->ctl_end = data + pkt->size;
    s->nbits   = 0;
    s->err     = 0;

    cur  = s->buf[s->cur]  + PAD_ROWS * s->stride;
    prev = s->buf[!s->cur] + PAD_ROWS * s->stride;

    get_bit(s);     /* the first control bit is skipped */
    for (int by = 0; by < s->height; by += 8)
        for (int bx = 0; bx < s->width; bx += 8)
            decode_node(s, cur, prev, by * s->stride + bx, 8, 8);
    if (s->err)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    for (int y = 0; y < s->height; y++) {
        uint16_t *dst = (uint16_t *)(frame->data[0] + y * frame->linesize[0]);
        const uint16_t *src = cur + y * s->stride;

        for (int x = 0; x < s->width; x++)
            dst[x] = src[x] & 0x7FFF;
    }

    if (!s->frame_num)
        frame->flags |= AV_FRAME_FLAG_KEY;
    frame->pict_type = s->frame_num ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    s->frame_num++;
    s->cur ^= 1;
    *got_frame = 1;

    return pkt->size;
}

static av_cold void fvfh_flush(AVCodecContext *avctx)
{
    FVFHContext *s = avctx->priv_data;
    size_t size = (size_t)(s->height + 2 * PAD_ROWS) * s->stride * sizeof(uint16_t);

    for (int i = 0; i < 2; i++)
        if (s->buf[i])
            memset(s->buf[i], 0, size);
    s->cur       = 0;
    s->frame_num = 0;
}

static av_cold int fvfh_close(AVCodecContext *avctx)
{
    FVFHContext *s = avctx->priv_data;

    av_freep(&s->buf[0]);
    av_freep(&s->buf[1]);
    return 0;
}

static av_cold int fvfh_init(AVCodecContext *avctx)
{
    FVFHContext *s = avctx->priv_data;
    size_t size;

    if (avctx->width < 8 || avctx->height < 8 ||
        (avctx->width | avctx->height) & 7)
        return AVERROR_INVALIDDATA;

    s->width  = avctx->width;
    s->height = avctx->height;
    s->stride = avctx->width;
    size = (size_t)(s->height + 2 * PAD_ROWS) * s->stride * sizeof(uint16_t);

    for (int i = 0; i < 2; i++) {
        s->buf[i] = av_mallocz(size);
        if (!s->buf[i])
            return AVERROR(ENOMEM);
    }
    avctx->pix_fmt = AV_PIX_FMT_BGR555LE;

    return 0;
}

const FFCodec ff_fvfh_decoder = {
    .p.name         = "fvfh",
    CODEC_LONG_NAME("FastVideo DS (Metroid Prime Hunters: First Hunt)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FVFH,
    .priv_data_size = sizeof(FVFHContext),
    .init           = fvfh_init,
    FF_CODEC_DECODE_CB(fvfh_decode),
    .flush          = fvfh_flush,
    .close          = fvfh_close,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
