/*
 * Bink 1 (BIKi) video encoder
 * Copyright (C) 2026 Justin Marshall
 *
 * Ported from the binkenc encoder of LibBink.
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
 * Bink 1 revision 'i' video encoder.
 *
 * Every 8x8 block is coded as skip, fill, two-colour pattern, motion copy,
 * intra DCT, inter DCT or raw. Huffman trees are identity trees, so every
 * symbol is a plain 4-bit value. The output decodes with FFmpeg's Bink
 * decoder and with LibBink.
 */

#include <math.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "binkdata.h"

enum {
    BLOCK_SKIP    = 0,
    BLOCK_MOTION  = 2,
    BLOCK_INTRA   = 5,
    BLOCK_FILL    = 6,
    BLOCK_INTER   = 7,
    BLOCK_PATTERN = 8,
    BLOCK_RAW     = 9,
};

enum {
    SRC_BLOCK_TYPES,
    SRC_SUB_BLOCK_TYPES,
    SRC_COLORS,
    SRC_PATTERN,
    SRC_X_OFF,
    SRC_Y_OFF,
    SRC_INTRA_DC,
    SRC_INTER_DC,
    SRC_RUN,
    NB_SRC
};

enum BundleKind {
    KIND_NIBBLE,
    KIND_COLOR,
    KIND_PATTERN,
    KIND_MOTION,
    KIND_DC_UNSIGNED,
    KIND_DC_SIGNED,
};

typedef struct BitBuf {
    uint8_t *buf;
    unsigned alloc;
    uint64_t nbits;
    int count_only;
    int err;
} BitBuf;

typedef struct VPlane {
    uint8_t *pix;
    int w, h;              ///< padded to whole 8x8 blocks; stride == w
} VPlane;

typedef struct DctBlock {
    int16_t coef[64];      ///< quantised, in scan order; coef[0] is DC
    int quant;
} DctBlock;

typedef struct Bundle {
    enum BundleKind kind;
    int len_bits;
    const uint8_t *bytes;
    const int16_t *dcs;
    int count;
    int loaded;
    int consumed;
    int ended;
} Bundle;

typedef struct BinkVideoEncContext {
    const AVClass *class;
    int quality;
    int quant_opt;
    int motion_range;

    int quant;
    double lambda;
    double lambda_mul;
    double rnd_intra, rnd_inter;
    VPlane src[3];
    VPlane rec[2][3];
    int cur;
    int have_prev;
    int64_t frame_num;

    /* per-plane scratch, sized for the luma plane */
    uint8_t  *types;
    DctBlock *dct;
    uint8_t  *has_dct;
    uint8_t  *colors;
    uint8_t  *patterns;
    uint8_t  *xoff, *yoff;
    int16_t  *intra_dc, *inter_dc;
    int      *cons[NB_SRC];

    BitBuf plane_bits[3];
} BinkVideoEncContext;

static const uint8_t quality_to_quant[11] = {
    15, 14, 12, 11, 10, 9, 7, 5, 3, 1, 0
};

/* ---- bit writer (LSB first, as Bink reads) ---- */

static void bb_reset(BitBuf *b)
{
    if (b->buf && b->nbits)
        memset(b->buf, 0, (b->nbits + 7) >> 3);
    b->nbits = 0;
    b->err   = 0;
}

static void bb_put(BitBuf *b, uint32_t v, int n)
{
    if (b->count_only) {
        b->nbits += n;
        return;
    }
    if (n < 32)
        v &= (1U << n) - 1;
    if (((b->nbits + n + 7) >> 3) > b->alloc) {
        unsigned need = ((b->nbits + n + 7) >> 3) * 2 + 1024;
        uint8_t *nb = av_realloc(b->buf, need);
        if (!nb) {
            b->err = AVERROR(ENOMEM);
            b->count_only = 1;
            b->nbits += n;
            return;
        }
        memset(nb + b->alloc, 0, need - b->alloc);
        b->buf   = nb;
        b->alloc = need;
    }
    while (n > 0) {
        int off = b->nbits & 7;
        int k   = FFMIN(8 - off, n);
        b->buf[b->nbits >> 3] |= (v & ((1U << k) - 1)) << off;
        v >>= k;
        n -= k;
        b->nbits += k;
    }
}

static void bb_align32(BitBuf *b)
{
    int rem = b->nbits & 31;
    if (rem)
        bb_put(b, 0, 32 - rem);
}

static int highest_bit(int v)
{
    return v ? av_log2(v) : -1;
}

static int bits_required(int v)
{
    return v ? av_log2(v) + 1 : 0;
}

static int integer_log2(unsigned v)
{
    return v ? av_log2(v) : 0;
}

/* ---- DCT ---- */

static double dct_basis[8][8];

static av_cold void init_basis(void)
{
    for (int f = 0; f < 8; f++) {
        double alpha = f ? 1.0 : 1.0 / sqrt(2.0);
        for (int s = 0; s < 8; s++)
            dct_basis[f][s] = 0.5 * alpha * cos(M_PI * (2 * s + 1) * f / 16.0);
    }
}

static void forward_dct(const uint8_t *src, const uint8_t *ref, int stride,
                        double out[64])
{
    double tmp[64];
    int sample[64];

    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            sample[r * 8 + c] = src[r * stride + c] - (ref ? ref[r * stride + c] : 0);

    for (int v = 0; v < 8; v++)
        for (int c = 0; c < 8; c++) {
            double sum = 0;
            for (int r = 0; r < 8; r++)
                sum += dct_basis[v][r] * sample[r * 8 + c];
            tmp[v * 8 + c] = sum;
        }
    for (int v = 0; v < 8; v++)
        for (int h = 0; h < 8; h++) {
            double sum = 0;
            for (int c = 0; c < 8; c++)
                sum += tmp[v * 8 + c] * dct_basis[h][c];
            out[v * 8 + h] = sum;
        }
}

/* The Bink inverse DCT, exactly as libavcodec/binkdsp.c computes it. */
#define A1  2896
#define A2  2217
#define A3  3784
#define A4 -5352
#define MUL(X, Y) ((int)((unsigned)(X) * (Y)) >> 11)

static void idct_1d(const int *src, int sstep, int *dst, int dstep, int row)
{
    const int a0 = src[0 * sstep] + src[4 * sstep];
    const int a1 = src[0 * sstep] - src[4 * sstep];
    const int a2 = src[2 * sstep] + src[6 * sstep];
    const int a3 = MUL(A1, src[2 * sstep] - src[6 * sstep]);
    const int a4 = src[5 * sstep] + src[3 * sstep];
    const int a5 = src[5 * sstep] - src[3 * sstep];
    const int a6 = src[1 * sstep] + src[7 * sstep];
    const int a7 = src[1 * sstep] - src[7 * sstep];
    const int b0 = a4 + a6;
    const int b1 = MUL(A3, a5 + a7);
    const int b2 = MUL(A4, a5) - b0 + b1;
    const int b3 = MUL(A1, a6 - a4) - b2;
    const int b4 = MUL(A2, a7) + b3 - b1;
    int v[8] = {
        a0 + a2 + b0, a1 + a3 - a2 + b2, a1 - a3 + a2 + b3, a0 - a2 - b4,
        a0 - a2 + b4, a1 - a3 + a2 - b3, a1 + a3 - a2 - b2, a0 + a2 - b0,
    };
    for (int i = 0; i < 8; i++)
        dst[i * dstep] = row ? (v[i] + 0x7F) >> 8 : v[i];
}

static void bink_idct(int block[64])
{
    int temp[64];
    for (int c = 0; c < 8; c++) {
        const int *s = block + c;
        if (!(s[8] | s[16] | s[24] | s[32] | s[40] | s[48] | s[56])) {
            for (int r = 0; r < 8; r++)
                temp[r * 8 + c] = s[0];
        } else {
            idct_1d(s, 8, temp + c, 8, 0);
        }
    }
    for (int r = 0; r < 8; r++)
        idct_1d(temp + r * 8, 1, block + r * 8, 1, 1);
}

/* Dequantise and inverse transform; fails if a product leaves 32 bits,
 * where decoders that use wider arithmetic would disagree. */
static int dct_reconstruct(const DctBlock *b, int inter, int out[64])
{
    const int32_t *q = inter ? bink_inter_quant[b->quant] : bink_intra_quant[b->quant];
    memset(out, 0, 64 * sizeof(*out));
    for (int i = 0; i < 64; i++) {
        int64_t p;
        if (!b->coef[i])
            continue;
        p = (int64_t)b->coef[i] * q[i];
        if (p < INT32_MIN || p > INT32_MAX)
            return 0;
        out[bink_scan[i]] = (int)p >> 11;
    }
    bink_idct(out);
    return 1;
}

/* Quantise one block (rounding magnitudes up from rnd), check the decoders
 * reproduce it within range, and return its reconstruction in rec. */
static int dct_try(DctBlock *b, const uint8_t *src, int sstride,
                   const uint8_t *ref, int rstride, int quant, double rnd,
                   uint8_t rec[64])
{
    const int32_t *q = ref ? bink_inter_quant[quant] : bink_intra_quant[quant];
    double t[64];
    int maxmag = 0, out[64];
    uint8_t diff_src[64];

    if (ref) {
        /* forward_dct() takes one stride; gather both blocks densely */
        uint8_t dref[64];
        for (int r = 0; r < 8; r++) {
            memcpy(diff_src + r * 8, src + r * sstride, 8);
            memcpy(dref + r * 8, ref + r * rstride, 8);
        }
        forward_dct(diff_src, dref, 8, t);
    } else {
        forward_dct(src, NULL, sstride, t);
    }
    for (int i = 0; i < 64; i++) {
        double scaled = t[bink_scan[i]] * bink_intra_quant[0][i] / q[i];
        long v = !i ? lround(scaled)
                    : (long)(floor(fabs(scaled) + rnd)) * (scaled < 0 ? -1 : 1);
        if (!i) {
            if (ref ? (v < -1023 || v > 1023) : (v < 0 || v > 2047))
                return 0;
        } else {
            if (FFABS(v) > 32767)
                return 0;
            maxmag = FFMAX(maxmag, FFABS((int)v));
        }
        b->coef[i] = v;
    }
    if (bits_required(maxmag) > 15)
        return 0;
    b->quant = quant;

    if (!dct_reconstruct(b, !!ref, out))
        return 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int v = out[r * 8 + c] + (ref ? ref[r * rstride + c] : 0);
            if (v < 0 || v > 255)
                return 0;
            rec[r * 8 + c] = v;
        }
    return 1;
}

static int64_t ssd8(const uint8_t *a, int as, const uint8_t *b, int bs)
{
    int64_t d = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int e = a[r * as + c] - b[r * bs + c];
            d += e * e;
        }
    return d;
}

static int sad8(const uint8_t *a, const uint8_t *b, int stride)
{
    int d = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            d += FFABS(a[r * stride + c] - b[r * stride + c]);
    return d;
}

static void gather8(uint8_t dst[64], const uint8_t *src, int stride)
{
    for (int r = 0; r < 8; r++)
        memcpy(dst + r * 8, src + r * stride, 8);
}

static void write_coefficient(BitBuf *bb, int v, int mag_bits)
{
    if (!mag_bits) {
        bb_put(bb, v < 0, 1);
    } else {
        bb_put(bb, FFABS(v), mag_bits);
        bb_put(bb, v < 0, 1);
    }
}

static int subtree_live(const int *rem, int coef, int mode, int mag_bits)
{
    int n = mode == 0 ? 20 : mode == 1 ? 16 : mode == 2 ? 4 : 1;
    for (int i = coef; i < coef + n; i++)
        if (rem[i] && highest_bit(FFABS(rem[i])) >= mag_bits)
            return 1;
    return 0;
}

/* Mirror of the decoder's read_dct_coeffs() list walk. */
static void dct_write(const DctBlock *b, BitBuf *bb)
{
    int rem[64] = { 0 };
    int coef_list[128], mode_list[128];
    int list_start = 64, list_end = 64;
    int maxmag = 0, top;

    for (int i = 1; i < 64; i++) {
        rem[i] = b->coef[i];
        maxmag = FFMAX(maxmag, FFABS(rem[i]));
    }
    if (!maxmag) {
        bb_put(bb, 0, 4);
        bb_put(bb, b->quant, 4);
        return;
    }
    top = highest_bit(maxmag);
    bb_put(bb, top + 1, 4);

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    for (int bits = top; bits >= 0; bits--) {
        int pos = list_start;
        while (pos < list_end) {
            int coef = coef_list[pos], mode = mode_list[pos], act;
            if (!(mode | coef)) {
                pos++;
                continue;
            }
            act = mode == 3 ? rem[coef] && highest_bit(FFABS(rem[coef])) == bits
                            : subtree_live(rem, coef, mode, bits);
            bb_put(bb, act, 1);
            if (!act) {
                pos++;
                continue;
            }
            if (mode == 0 || mode == 2) {
                if (mode == 0) {
                    coef_list[pos] = coef + 4;
                    mode_list[pos] = 1;
                } else {
                    coef_list[pos] = 0;
                    mode_list[pos++] = 0;
                }
                for (int g = 0; g < 4; g++, coef++) {
                    int v = rem[coef];
                    if (v && highest_bit(FFABS(v)) == bits) {
                        bb_put(bb, 0, 1);
                        write_coefficient(bb, v, bits);
                        rem[coef] = 0;
                    } else {
                        bb_put(bb, 1, 1);
                        coef_list[--list_start] = coef;
                        mode_list[list_start]   = 3;
                    }
                }
            } else if (mode == 1) {
                mode_list[pos] = 2;
                for (int g = 0; g < 3; g++) {
                    coef += 4;
                    coef_list[list_end]   = coef;
                    mode_list[list_end++] = 2;
                }
            } else {
                write_coefficient(bb, rem[coef], bits);
                rem[coef] = 0;
                coef_list[pos] = 0;
                mode_list[pos++] = 0;
            }
        }
    }
    bb_put(bb, b->quant, 4);
}

static int dct_cost(const DctBlock *b)
{
    BitBuf c = { .count_only = 1 };
    dct_write(b, &c);
    return c.nbits;
}

static int dct_is_zero(const DctBlock *b)
{
    for (int i = 0; i < 64; i++)
        if (b->coef[i])
            return 0;
    return 1;
}

/* ---- bundles ---- */

static int all_equal(const uint8_t *v, int n)
{
    for (int i = 1; i < n; i++)
        if (v[i] != v[0])
            return 0;
    return 1;
}

static void write_dcs(BitBuf *bb, const int16_t *v, int n, int is_signed)
{
    int val = v[0];
    bb_put(bb, FFABS(val), is_signed ? 10 : 11);
    if (is_signed && val)
        bb_put(bb, val < 0, 1);
    for (int off = 1; off < n; off += 8) {
        int cnt = FFMIN(8, n - off), maxd = 0, prev = val, size;
        for (int i = 0; i < cnt; i++) {
            maxd = FFMAX(maxd, FFABS(v[off + i] - prev));
            prev = v[off + i];
        }
        size = bits_required(maxd);
        bb_put(bb, size, 4);
        for (int i = 0; i < cnt; i++) {
            int d = v[off + i] - val;
            if (size) {
                bb_put(bb, FFABS(d), size);
                if (d)
                    bb_put(bb, d < 0, 1);
            }
            val = v[off + i];
        }
    }
}

static void bundle_write_if_needed(Bundle *b, BitBuf *bb, int requested)
{
    int count;
    if (b->ended || b->loaded > b->consumed)
        return;
    count = FFMIN(b->count - b->loaded, requested);
    bb_put(bb, count, b->len_bits);
    if (!count) {
        b->ended = 1;
        return;
    }
    if (b->kind == KIND_DC_UNSIGNED || b->kind == KIND_DC_SIGNED) {
        write_dcs(bb, b->dcs + b->loaded, count, b->kind == KIND_DC_SIGNED);
    } else {
        const uint8_t *v = b->bytes + b->loaded;
        switch (b->kind) {
        case KIND_NIBBLE:
            if (all_equal(v, count)) {
                bb_put(bb, 1, 1);
                bb_put(bb, v[0], 4);
            } else {
                bb_put(bb, 0, 1);
                for (int i = 0; i < count; i++)
                    bb_put(bb, v[i], 4);
            }
            break;
        case KIND_MOTION: {
            int c = all_equal(v, count);
            bb_put(bb, c, 1);
            for (int i = 0; i < (c ? 1 : count); i++) {
                int s = (int8_t)v[i];
                bb_put(bb, FFABS(s), 4);
                if (s)
                    bb_put(bb, s < 0, 1);
            }
            break;
        }
        case KIND_COLOR: {
            int c = all_equal(v, count);
            bb_put(bb, c, 1);
            for (int i = 0; i < (c ? 1 : count); i++) {
                bb_put(bb, v[i] >> 4, 4);
                bb_put(bb, v[i] & 15, 4);
            }
            break;
        }
        case KIND_PATTERN:
            for (int i = 0; i < count; i++) {
                bb_put(bb, v[i] & 15, 4);
                bb_put(bb, v[i] >> 4, 4);
            }
            break;
        default:
            break;
        }
    }
    b->loaded += count;
}

static int next_nonzero(const int *cons, int row, int rows)
{
    for (int i = row; i < rows; i++)
        if (cons[i])
            return cons[i];
    return 0;
}

/* ---- block helpers ---- */

static int blocks_equal(const uint8_t *a, const uint8_t *b, int stride)
{
    for (int r = 0; r < 8; r++)
        if (memcmp(a + r * stride, b + r * stride, 8))
            return 0;
    return 1;
}

static void copy_block(uint8_t *dst, const uint8_t *src, int stride)
{
    for (int r = 0; r < 8; r++)
        memcpy(dst + r * stride, src + r * stride, 8);
}

/* Rate-distortion choice: rec holds the candidate's reconstruction. */
#define CANDIDATE(t, dist, bits) do {                       \
        int64_t d_ = (dist);                                \
        double c_ = d_ + s->lambda * (bits);                \
        if (c_ < best_cost) {                               \
            best_cost = c_;                                 \
            best_d    = d_;                                 \
            type      = (t);                                \
            memcpy(best_rec, rec, 64);                      \
        }                                                   \
    } while (0)

static int encode_plane(BinkVideoEncContext *s, int pi, int key, int logical_width,
                        BitBuf *bb)
{
    const VPlane *pl   = &s->src[pi];
    const VPlane *prev = s->have_prev && !key ? &s->rec[s->cur ^ 1][pi] : NULL;
    VPlane *out_plane  = &s->rec[s->cur][pi];
    const int stride = pl->w;
    const int bw = pl->w >> 3, bh = pl->h >> 3;
    const int range = s->motion_range;
    int n_types = 0, n_col = 0, n_pat = 0, n_mv = 0, n_idc = 0, n_pdc = 0;
    int len_bits[NB_SRC], aligned;
    Bundle bun[NB_SRC] = { 0 };

    for (int by = 0; by < bh; by++) {
        int col0 = n_col, pat0 = n_pat, mv0 = n_mv, idc0 = n_idc, pdc0 = n_pdc;
        for (int bx = 0; bx < bw; bx++) {
            const int off = by * 8 * stride + bx * 8;
            const uint8_t *src = pl->pix + off;
            uint8_t *dst = out_plane->pix + off;
            int bi = n_types;
            uint8_t rec[64], best_rec[64], pat[8], best_pat[8];
            int best_col[2] = { 0 }, fillv, sum = 0, type = BLOCK_RAW;
            int mvx = 0, mvy = 0, best_mvx = 0, best_mvy = 0, mv_bits = 0;
            const uint8_t *mvref = NULL;
            double best_cost = INFINITY;
            int64_t best_d = INT64_MAX;
            DctBlock dblk, best_dct;

            s->has_dct[bi] = 0;

            if (prev && blocks_equal(src, prev->pix + off, stride)) {
                s->types[n_types++] = BLOCK_SKIP;
                copy_block(dst, prev->pix + off, stride);
                continue;
            }

            /* fill: exact when the block is flat */
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    sum += src[r * stride + c];
            fillv = (sum + 32) >> 6;
            for (int i = 0; i < 64; i++)
                rec[i] = fillv;
            CANDIDATE(BLOCK_FILL, ssd8(src, stride, rec, 8), 12);
            if (best_d == 0)
                goto decided;

            if (prev) {
                const uint8_t *ref = prev->pix + off;
                int best_sad = INT_MAX;
                gather8(rec, ref, stride);
                CANDIDATE(BLOCK_SKIP, ssd8(src, stride, rec, 8), 4);

                for (int dy = -range; dy <= range; dy++)
                    for (int dx = -range; dx <= range; dx++) {
                        int sx = bx * 8 + dx, sy = by * 8 + dy, sad;
                        if (sx < 0 || sy < 0 || sx + 8 > prev->w || sy + 8 > prev->h)
                            continue;
                        sad = sad8(src, prev->pix + sy * stride + sx, stride);
                        if (sad < best_sad || (sad == best_sad && !dx && !dy)) {
                            best_sad = sad;
                            mvx = dx;
                            mvy = dy;
                        }
                    }
                mvref = prev->pix + (by * 8 + mvy) * stride + bx * 8 + mvx;
                mv_bits = 8 + !!mvx + !!mvy;
                if (mvx || mvy) {
                    gather8(rec, mvref, stride);
                    CANDIDATE(BLOCK_MOTION, ssd8(src, stride, rec, 8), 4 + mv_bits);
                    if (type == BLOCK_MOTION) {
                        best_mvx = mvx;
                        best_mvy = mvy;
                    }
                }
            }

            /* two-colour pattern, split at the mean */
            {
                int lo = 0, hi = 0, nlo = 0, nhi = 0;
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++) {
                        int v = src[r * stride + c];
                        if (v * 64 > sum) { hi += v; nhi++; } else { lo += v; nlo++; }
                    }
                if (nlo && nhi) {
                    int c0 = (lo + nlo / 2) / nlo, c1 = (hi + nhi / 2) / nhi;
                    for (int r = 0; r < 8; r++) {
                        int p = 0;
                        for (int c = 0; c < 8; c++) {
                            int one = src[r * stride + c] * 64 > sum;
                            p |= one << c;
                            rec[r * 8 + c] = one ? c1 : c0;
                        }
                        pat[r] = p;
                    }
                    CANDIDATE(BLOCK_PATTERN, ssd8(src, stride, rec, 8), 84);
                    if (type == BLOCK_PATTERN) {
                        best_col[0] = c0;
                        best_col[1] = c1;
                        memcpy(best_pat, pat, 8);
                    }
                }
            }

            if (dct_try(&dblk, src, stride, NULL, 0, s->quant, s->rnd_intra, rec)) {
                CANDIDATE(BLOCK_INTRA, ssd8(src, stride, rec, 8), dct_cost(&dblk) + 10);
                if (type == BLOCK_INTRA)
                    best_dct = dblk;
            }
            if (prev && dct_try(&dblk, src, stride, mvref, stride, s->quant, s->rnd_inter, rec) &&
                !dct_is_zero(&dblk)) {
                CANDIDATE(BLOCK_INTER, ssd8(src, stride, rec, 8), dct_cost(&dblk) + 8 + mv_bits);
                if (type == BLOCK_INTER) {
                    best_dct = dblk;
                    best_mvx = mvx;
                    best_mvy = mvy;
                }
            }
            gather8(rec, src, stride);
            CANDIDATE(BLOCK_RAW, 0, 516);

decided:
            s->types[n_types++] = type;
            switch (type) {
            case BLOCK_FILL:
                s->colors[n_col++] = fillv;
                break;
            case BLOCK_MOTION:
                s->xoff[n_mv]   = (uint8_t)(int8_t)best_mvx;
                s->yoff[n_mv++] = (uint8_t)(int8_t)best_mvy;
                break;
            case BLOCK_PATTERN:
                s->colors[n_col++] = best_col[0];
                s->colors[n_col++] = best_col[1];
                memcpy(s->patterns + n_pat, best_pat, 8);
                n_pat += 8;
                break;
            case BLOCK_INTRA:
                s->dct[bi] = best_dct;
                s->has_dct[bi] = 1;
                s->intra_dc[n_idc++] = best_dct.coef[0];
                break;
            case BLOCK_INTER:
                s->dct[bi] = best_dct;
                s->has_dct[bi] = 1;
                s->xoff[n_mv]   = (uint8_t)(int8_t)best_mvx;
                s->yoff[n_mv++] = (uint8_t)(int8_t)best_mvy;
                s->inter_dc[n_pdc++] = best_dct.coef[0];
                break;
            case BLOCK_RAW:
                for (int i = 0; i < 64; i++)
                    s->colors[n_col++] = best_rec[i];
                break;
            }
            for (int r = 0; r < 8; r++)
                memcpy(dst + r * stride, best_rec + r * 8, 8);
        }
        s->cons[SRC_COLORS][by]   = n_col - col0;
        s->cons[SRC_PATTERN][by]  = n_pat - pat0;
        s->cons[SRC_X_OFF][by]    = n_mv - mv0;
        s->cons[SRC_Y_OFF][by]    = n_mv - mv0;
        s->cons[SRC_INTRA_DC][by] = n_idc - idc0;
        s->cons[SRC_INTER_DC][by] = n_pdc - pdc0;
    }

    aligned = (FFMAX(logical_width, 8) + 7) & ~7;
    len_bits[SRC_BLOCK_TYPES]     = integer_log2((aligned >> 3) + 511) + 1;
    len_bits[SRC_SUB_BLOCK_TYPES] = integer_log2((aligned >> 4) + 511) + 1;
    len_bits[SRC_COLORS]          = integer_log2(bw * 64 + 511) + 1;
    len_bits[SRC_X_OFF]           =
    len_bits[SRC_Y_OFF]           =
    len_bits[SRC_INTRA_DC]        =
    len_bits[SRC_INTER_DC]        = integer_log2((aligned >> 3) + 511) + 1;
    len_bits[SRC_PATTERN]         = integer_log2((bw << 3) + 511) + 1;
    len_bits[SRC_RUN]             = integer_log2(bw * 48 + 511) + 1;

    /* identity Huffman trees: codebook 0 everywhere */
    for (int src = 0; src < NB_SRC; src++) {
        if (src == SRC_COLORS)
            for (int t = 0; t < 16; t++)
                bb_put(bb, 0, 4);
        if (src != SRC_INTRA_DC && src != SRC_INTER_DC)
            bb_put(bb, 0, 4);
    }

    for (int i = 0; i < NB_SRC; i++)
        bun[i].len_bits = len_bits[i];
    bun[SRC_BLOCK_TYPES]     = (Bundle){ KIND_NIBBLE,  len_bits[SRC_BLOCK_TYPES], s->types, NULL, n_types };
    bun[SRC_SUB_BLOCK_TYPES] = (Bundle){ KIND_NIBBLE,  len_bits[SRC_SUB_BLOCK_TYPES], NULL, NULL, 0 };
    bun[SRC_COLORS]          = (Bundle){ KIND_COLOR,   len_bits[SRC_COLORS], s->colors, NULL, n_col };
    bun[SRC_PATTERN]         = (Bundle){ KIND_PATTERN, len_bits[SRC_PATTERN], s->patterns, NULL, n_pat };
    bun[SRC_X_OFF]           = (Bundle){ KIND_MOTION,  len_bits[SRC_X_OFF], s->xoff, NULL, n_mv };
    bun[SRC_Y_OFF]           = (Bundle){ KIND_MOTION,  len_bits[SRC_Y_OFF], s->yoff, NULL, n_mv };
    bun[SRC_INTRA_DC]        = (Bundle){ KIND_DC_UNSIGNED, len_bits[SRC_INTRA_DC], NULL, s->intra_dc, n_idc };
    bun[SRC_INTER_DC]        = (Bundle){ KIND_DC_SIGNED,   len_bits[SRC_INTER_DC], NULL, s->inter_dc, n_pdc };
    bun[SRC_RUN]             = (Bundle){ KIND_NIBBLE,  len_bits[SRC_RUN], NULL, NULL, 0 };

    for (int by = 0; by < bh; by++) {
        bundle_write_if_needed(&bun[SRC_BLOCK_TYPES], bb, bw);
        bundle_write_if_needed(&bun[SRC_SUB_BLOCK_TYPES], bb, 0);
        for (int src = SRC_COLORS; src <= SRC_INTER_DC; src++)
            bundle_write_if_needed(&bun[src], bb, next_nonzero(s->cons[src], by, bh));
        bundle_write_if_needed(&bun[SRC_RUN], bb, 0);

        for (int bx = 0; bx < bw; bx++)
            if (s->has_dct[by * bw + bx])
                dct_write(&s->dct[by * bw + bx], bb);

        bun[SRC_BLOCK_TYPES].consumed += bw;
        for (int src = SRC_COLORS; src <= SRC_INTER_DC; src++)
            bun[src].consumed += s->cons[src][by];
    }
    for (int i = 0; i < NB_SRC; i++)
        if (bun[i].consumed != bun[i].count || bun[i].loaded != bun[i].count) {
            av_log(NULL, AV_LOG_ERROR, "Bink bundle %d accounting error\n", i);
            return AVERROR_BUG;
        }
    bb_align32(bb);
    return bb->err;
}

static void pad_plane(VPlane *dst, const uint8_t *src, int linesize, int w, int h)
{
    for (int r = 0; r < h; r++) {
        uint8_t *d = dst->pix + r * dst->w;
        memcpy(d, src + r * linesize, w);
        memset(d + w, d[w - 1], dst->w - w);
    }
    for (int r = h; r < dst->h; r++)
        memcpy(dst->pix + r * dst->w, dst->pix + (h - 1) * dst->w, dst->w);
}

static av_cold int bink_encode_init(AVCodecContext *avctx)
{
    BinkVideoEncContext *s = avctx->priv_data;
    int w = avctx->width, h = avctx->height;
    int sizes[3][2] = {
        { (w + 7) & ~7, (h + 7) & ~7 },
        { ((w + 15) >> 4) * 8, ((h + 15) >> 4) * 8 },
        { ((w + 15) >> 4) * 8, ((h + 15) >> 4) * 8 },
    };
    int blocks = (sizes[0][0] >> 3) * (sizes[0][1] >> 3);

    if (w < 1 || h < 1 || w > 7680 || h > 4800) {
        av_log(avctx, AV_LOG_ERROR, "Bink supports sizes up to 7680x4800\n");
        return AVERROR(EINVAL);
    }
    init_basis();
    s->quant = s->quant_opt >= 0 ? s->quant_opt : quality_to_quant[s->quality];
    {
        /* quantiser step for an orthonormal DCT coefficient */
        double step = bink_intra_quant[s->quant][0] / 65536.0;
        s->lambda = s->lambda_mul * step * step;
    }

    for (int p = 0; p < 3; p++) {
        int sz = sizes[p][0] * sizes[p][1];
        s->src[p] = (VPlane){ av_mallocz(sz), sizes[p][0], sizes[p][1] };
        for (int f = 0; f < 2; f++)
            s->rec[f][p] = (VPlane){ av_mallocz(sz), sizes[p][0], sizes[p][1] };
        if (!s->src[p].pix || !s->rec[0][p].pix || !s->rec[1][p].pix)
            return AVERROR(ENOMEM);
    }
    s->types    = av_malloc(blocks);
    s->dct      = av_malloc_array(blocks, sizeof(*s->dct));
    s->has_dct  = av_malloc(blocks);
    s->colors   = av_malloc_array(blocks, 64);
    s->patterns = av_malloc_array(blocks, 8);
    s->xoff     = av_malloc(blocks);
    s->yoff     = av_malloc(blocks);
    s->intra_dc = av_malloc_array(blocks, sizeof(int16_t));
    s->inter_dc = av_malloc_array(blocks, sizeof(int16_t));
    if (!s->types || !s->dct || !s->has_dct || !s->colors || !s->patterns ||
        !s->xoff || !s->yoff || !s->intra_dc || !s->inter_dc)
        return AVERROR(ENOMEM);
    for (int i = 0; i < NB_SRC; i++) {
        s->cons[i] = av_calloc(sizes[0][1] >> 3, sizeof(int));
        if (!s->cons[i])
            return AVERROR(ENOMEM);
    }
    if (avctx->gop_size <= 0)
        avctx->gop_size = 0;
    return 0;
}

static int bink_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    BinkVideoEncContext *s = avctx->priv_data;
    int key = !s->frame_num ||
              (avctx->gop_size > 0 && s->frame_num % avctx->gop_size == 0) ||
              frame->pict_type == AV_PICTURE_TYPE_I;
    int cw = (avctx->width + 1) >> 1, ch = (avctx->height + 1) >> 1;
    /* bitstream order is Y, V, U */
    static const int order[3] = { 0, 2, 1 };
    int64_t size;
    uint8_t *p;
    int ret;

    pad_plane(&s->src[0], frame->data[0], frame->linesize[0], avctx->width, avctx->height);
    pad_plane(&s->src[1], frame->data[1], frame->linesize[1], cw, ch);
    pad_plane(&s->src[2], frame->data[2], frame->linesize[2], cw, ch);

    for (int i = 0; i < 3; i++) {
        int pi = order[i];
        bb_reset(&s->plane_bits[i]);
        ret = encode_plane(s, pi, key, pi ? avctx->width >> 1 : avctx->width,
                           &s->plane_bits[i]);
        if (ret < 0)
            return ret;
    }

    size = 4;
    for (int i = 0; i < 3; i++)
        size += s->plane_bits[i].nbits >> 3;
    if ((ret = ff_get_encode_buffer(avctx, pkt, size, 0)) < 0)
        return ret;
    p = pkt->data;
    AV_WL32(p, 4 + (s->plane_bits[0].nbits >> 3));
    p += 4;
    for (int i = 0; i < 3; i++) {
        size_t n = s->plane_bits[i].nbits >> 3;
        memcpy(p, s->plane_bits[i].buf, n);
        p += n;
    }

    if (key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->have_prev = 1;
    s->cur ^= 1;
    s->frame_num++;
    *got_packet = 1;
    return 0;
}

static av_cold int bink_encode_close(AVCodecContext *avctx)
{
    BinkVideoEncContext *s = avctx->priv_data;
    for (int p = 0; p < 3; p++) {
        av_freep(&s->src[p].pix);
        av_freep(&s->rec[0][p].pix);
        av_freep(&s->rec[1][p].pix);
        av_freep(&s->plane_bits[p].buf);
    }
    av_freep(&s->types);
    av_freep(&s->dct);
    av_freep(&s->has_dct);
    av_freep(&s->colors);
    av_freep(&s->patterns);
    av_freep(&s->xoff);
    av_freep(&s->yoff);
    av_freep(&s->intra_dc);
    av_freep(&s->inter_dc);
    for (int i = 0; i < NB_SRC; i++)
        av_freep(&s->cons[i]);
    return 0;
}

#define OFFSET(x) offsetof(BinkVideoEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "quality", "video quality, 10 is highest", OFFSET(quality), AV_OPT_TYPE_INT, { .i64 = 3 }, 0, 10, VE },
    { "quant", "DCT quantiser index, 0 is finest (-1 = from quality)", OFFSET(quant_opt), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 15, VE },
    { "lambda", "rate-distortion weight; higher is smaller and lossier", OFFSET(lambda_mul), AV_OPT_TYPE_DOUBLE, { .dbl = 0.2 }, 0, 100, VE },
    { "rnd_intra", "intra DCT rounding offset", OFFSET(rnd_intra), AV_OPT_TYPE_DOUBLE, { .dbl = 0.4 }, 0, 0.5, VE },
    { "rnd_inter", "inter DCT rounding offset", OFFSET(rnd_inter), AV_OPT_TYPE_DOUBLE, { .dbl = 0.3 }, 0, 0.5, VE },
    { "motion_range", "motion search range in pixels (0 = zero vectors only)", OFFSET(motion_range), AV_OPT_TYPE_INT, { .i64 = 15 }, 0, 15, VE },
    { NULL },
};

static const AVClass bink_encoder_class = {
    .class_name = "binkvideo encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault bink_defaults[] = {
    { "g", "120" },
    { NULL },
};

const FFCodec ff_bink_encoder = {
    .p.name         = "binkvideo",
    CODEC_LONG_NAME("Bink video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_BINKVIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(BinkVideoEncContext),
    .init           = bink_encode_init,
    FF_CODEC_ENCODE_CB(bink_encode_frame),
    .close          = bink_encode_close,
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P),
    .p.priv_class   = &bink_encoder_class,
    .defaults       = bink_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
