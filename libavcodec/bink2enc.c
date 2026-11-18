/*
 * Bink 2 (KB2j) video encoder
 * Copyright (C) 2026 quatric <quatricsoftware@gmail.com>
 *
 * Ported from bink2enc, Copyright (C) 2026 Simon Berda and contributors
 * (https://github.com/SBerda/bink2enc). The bitstream layout, tables,
 * integer IDCT, predictors and motion compensation filters follow the
 * Bink 2 decoder of NihAV by Kostya Shishkov.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 * Bink 2 KB2j video encoder: 32x32 macroblocks coded as intra, skip,
 * motion, or motion plus residual, with one or four half-pel vectors, an
 * 8x8 integer DCT and fixed variable-length codes. The reconstruction is
 * the decoder's integer arithmetic exactly. Slices are independent and are
 * encoded in parallel.
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
#include "bink2data.h"
#include "bink2dsp.h"

/* The analysis must round exactly as the reference does: no fused
 * multiply-adds. */
#pragma STDC FP_CONTRACT OFF

#define FRAME_FLAGS_INTRA 0x76018
#define FRAME_FLAGS_INTER 0x6e018
#define MAX_LEVEL   1025
#define MAX_DC_CODE 513
#define SYM_ESC_RUN 11
#define SYM_EOB     12
#define MAX_SLICES  8

/* ---------------------------------------------------------------- bits */

typedef struct BW {
    uint8_t *buf;
    size_t alloc;
    size_t nbits;
    int count;
    int err;
} BW;

static void bw_write(BW *b, uint32_t val, int n)
{
    if (b->count) {
        b->nbits += n;
        return;
    }
    if (n < 32)
        val &= (1U << n) - 1;
    if (((b->nbits + n + 7) >> 3) > b->alloc) {
        size_t need = ((b->nbits + n + 7) >> 3) * 2 + 4096;
        uint8_t *nb = av_realloc(b->buf, need);
        if (!nb) {
            b->err = AVERROR(ENOMEM);
            b->count = 1;
            b->nbits += n;
            return;
        }
        memset(nb + b->alloc, 0, need - b->alloc);
        b->buf = nb;
        b->alloc = need;
    }
    while (n > 0) {
        int off = b->nbits & 7, k = FFMIN(8 - off, n);
        b->buf[b->nbits >> 3] |= (val & ((1U << k) - 1)) << off;
        val >>= k;
        n -= k;
        b->nbits += k;
    }
}

static void bw_bit(BW *b, int v)
{
    bw_write(b, !!v, 1);
}

static void bw_reset(BW *b)
{
    if (b->buf)
        memset(b->buf, 0, FFMIN(b->alloc, (b->nbits + 7) >> 3));
    b->nbits = 0;
    b->count = 0;
    b->err = 0;
}

static void unary_lim(BW *b, uint32_t val, uint32_t len, int term)
{
    for (uint32_t i = 0; i < val; i++)
        bw_bit(b, !term);
    if (val < len)
        bw_bit(b, term);
}

/* ---------------------------------------------------------------- transform */

static inline int mul_a(int v) { return v + (v >> 2); }
static inline int mul_b(int v) { return v >> 1; }
static inline int mul_c(int v) { return v - (v >> 2) - (v >> 4); }
static inline int mul_d(int v) { return v + (v >> 2) - (v >> 4); }
static inline int mul_e(int v) { return v >> 2; }

static void idct1d(const int *src, int sstep, int *dst, int dstep, int bias, int shift)
{
    int tmp00 = src[3 * sstep] + src[5 * sstep];
    int tmp01 = src[3 * sstep] - src[5 * sstep];
    int tmp02 = mul_a(src[2 * sstep]) + mul_b(src[6 * sstep]);
    int tmp03 = mul_b(src[2 * sstep]) - mul_a(src[6 * sstep]);
    int tmp0 = (src[0] + src[4 * sstep]) + tmp02;
    int tmp1 = (src[0] + src[4 * sstep]) - tmp02;
    int tmp2 = src[0] - src[4 * sstep];
    int tmp3 = src[sstep] + tmp00;
    int tmp4 = src[sstep] - tmp00;
    int tmp5 = tmp01 + src[7 * sstep];
    int tmp6 = tmp01 - src[7 * sstep];
    int tmp7 = tmp4 + mul_c(tmp6);
    int tmp8 = mul_c(tmp4) - tmp6;
    int tmp9 = mul_d(tmp3) + mul_e(tmp5);
    int tmp10 = mul_e(tmp3) - mul_d(tmp5);
    int tmp11 = tmp2 + tmp03;
    int tmp12 = tmp2 - tmp03;
    dst[0]         = (tmp0 + tmp9 + bias) >> shift;
    dst[dstep]     = (tmp11 + tmp7 + bias) >> shift;
    dst[2 * dstep] = (tmp12 + tmp8 + bias) >> shift;
    dst[3 * dstep] = (tmp1 + tmp10 + bias) >> shift;
    dst[4 * dstep] = (tmp1 - tmp10 + bias) >> shift;
    dst[5 * dstep] = (tmp12 - tmp8 + bias) >> shift;
    dst[6 * dstep] = (tmp11 - tmp7 + bias) >> shift;
    dst[7 * dstep] = (tmp0 - tmp9 + bias) >> shift;
}

/* Reconstruction uses the reference decoder's exact transform. */
static void idct_int(int c[64])
{
    bink2_idct(c);
}

/* Forward transform: the numerical inverse of the decoder's 1-D IDCT. */
static void init_forward_dct(double f[8][8])
{
    const int s = 1 << 14;
    double a[64], inv[64];

    for (int j = 0; j < 8; j++) {
        int p[8] = { 0 }, n[8] = { 0 }, op[8], on[8];
        p[j] = s;
        n[j] = -s;
        idct1d(p, 1, op, 1, 0, 0);
        idct1d(n, 1, on, 1, 0, 0);
        for (int i = 0; i < 8; i++)
            a[i * 8 + j] = ((double)op[i] - (double)on[i]) / (2.0 * s);
    }
    for (int i = 0; i < 64; i++)
        inv[i] = (i % 9) == 0;
    for (int col = 0; col < 8; col++) {
        int piv = col;
        double d;
        for (int r = col + 1; r < 8; r++)
            if (fabs(a[r * 8 + col]) > fabs(a[piv * 8 + col]))
                piv = r;
        if (piv != col)
            for (int k = 0; k < 8; k++) {
                FFSWAP(double, a[col * 8 + k], a[piv * 8 + k]);
                FFSWAP(double, inv[col * 8 + k], inv[piv * 8 + k]);
            }
        d = a[col * 8 + col];
        for (int k = 0; k < 8; k++) {
            a[col * 8 + k] /= d;
            inv[col * 8 + k] /= d;
        }
        for (int r = 0; r < 8; r++) {
            double fr;
            if (r == col)
                continue;
            fr = a[r * 8 + col];
            if (fr == 0.0)
                continue;
            for (int k = 0; k < 8; k++) {
                a[r * 8 + k] -= fr * a[col * 8 + k];
                inv[r * 8 + k] -= fr * inv[col * 8 + k];
            }
        }
    }
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            f[i][j] = inv[i * 8 + j];
}

static void forward_dct(const double f[8][8], const double px[64], double out[64])
{
    double t[8][8];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            double acc = 0.0;
            for (int m = 0; m < 8; m++)
                acc += f[i][m] * px[j * 8 + m];
            t[i][j] = acc;
        }
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++) {
            double acc = 0.0;
            for (int j = 0; j < 8; j++)
                acc += t[i][j] * f[k][j];
            out[i * 8 + k] = 64.0 * acc;
        }
}

/* ---------------------------------------------------------------- planes */

typedef struct B2Plane {
    int w, h, stride, rows;
    uint8_t *data;
} B2Plane;

typedef struct B2Frame {
    B2Plane p[3];      ///< Y, U, V
} B2Frame;

static int plane_alloc(B2Plane *p, int w, int h, int mb)
{
    p->w = w;
    p->h = h;
    p->stride = (w + mb - 1) / mb * mb;
    p->rows   = (h + mb - 1) / mb * mb;
    p->data = av_mallocz((size_t)p->stride * p->rows);
    return p->data ? 0 : AVERROR(ENOMEM);
}

static int frame_alloc(B2Frame *f, int w, int h)
{
    int ret;
    if ((ret = plane_alloc(&f->p[0], w, h, 32)) < 0 ||
        (ret = plane_alloc(&f->p[1], (w + 1) / 2, (h + 1) / 2, 16)) < 0 ||
        (ret = plane_alloc(&f->p[2], (w + 1) / 2, (h + 1) / 2, 16)) < 0)
        return ret;
    return 0;
}

static void frame_free(B2Frame *f)
{
    for (int i = 0; i < 3; i++)
        av_freep(&f->p[i].data);
}

static void frame_copy(B2Frame *dst, const B2Frame *src)
{
    for (int i = 0; i < 3; i++)
        memcpy(dst->p[i].data, src->p[i].data, (size_t)src->p[i].stride * src->p[i].rows);
}

static void plane_fill(B2Plane *p, const uint8_t *src, ptrdiff_t linesize)
{
    for (int y = 0; y < p->rows; y++) {
        const uint8_t *row = src + FFMIN(y, p->h - 1) * linesize;
        uint8_t *dst = p->data + (size_t)y * p->stride;
        memcpy(dst, row, p->w);
        memset(dst + p->w, row[p->w - 1], p->stride - p->w);
    }
}

#define AT(pl, x, y) ((pl)->data[(size_t)(y) * (pl)->stride + (x)])

/* ---------------------------------------------------------------- predictors */

static inline int dc_pred2(int a, int b)
{
    return FFMIN(FFMAX(a, b), FFMAX(FFMIN(a, b), a * 2 - b));
}

static inline int dc_pred(int a, int b, int c)
{
    return FFMIN(FFMAX(FFMAX(a, b), c), FFMAX(FFMIN(FFMIN(a, b), c), b - a + c));
}

static inline int b2_mid_pred(int a, int b, int c)
{
    if (a < b) {
        if (b < c) return b;
        return a < c ? c : a;
    }
    if (b < c)
        return a < c ? a : c;
    return b;
}

static int luma_pred(int k, const int *a, const int *l, const int *p, int top, int left)
{
    switch (k) {
    case 0:
        if (top && left) return 1 << 10;
        if (left)        return dc_pred2(p[4], p[5]);
        if (top)         return dc_pred2(l[5], l[7]);
        return dc_pred(p[3], p[4], l[5]);
    case 1:  return top ? a[0] : dc_pred(p[4], p[5], a[0]);
    case 2:  return left ? dc_pred2(a[0], a[1]) : dc_pred(l[5], l[7], a[0]);
    case 3:  return dc_pred(a[0], a[1], a[2]);
    case 4:  return top ? dc_pred2(a[1], a[3]) : dc_pred(p[5], p[6], a[1]);
    case 5:  return top ? a[4] : dc_pred(p[6], p[7], a[4]);
    case 6:  return dc_pred(a[1], a[3], a[4]);
    case 7:  return dc_pred(a[4], a[5], a[6]);
    case 8:  return left ? dc_pred2(a[2], a[3]) : dc_pred(l[7], l[13], a[2]);
    case 9:  return dc_pred(a[2], a[3], a[8]);
    case 10: return left ? dc_pred2(a[8], a[9]) : dc_pred(l[13], l[15], a[8]);
    case 11: return dc_pred(a[8], a[9], a[10]);
    case 12: return dc_pred(a[3], a[6], a[9]);
    case 13: return dc_pred(a[6], a[7], a[12]);
    case 14: return dc_pred(a[9], a[11], a[12]);
    default: return dc_pred(a[12], a[13], a[14]);
    }
}

static int chroma_pred(int k, const int *a, const int *l, const int *p, int top, int left)
{
    switch (k) {
    case 0:
        if (top && left) return 1 << 10;
        if (left)        return dc_pred2(p[2], p[3]);
        if (top)         return dc_pred2(l[1], l[3]);
        return dc_pred(p[1], p[2], l[1]);
    case 1:  return top ? a[0] : dc_pred(p[2], p[3], a[0]);
    case 2:  return left ? dc_pred2(a[0], a[1]) : dc_pred(l[1], l[3], a[0]);
    default: return dc_pred(a[0], a[1], a[2]);
    }
}

static const int zeros16[16];

static int luma_pred_inter(int k, const int *a)
{
    return k ? luma_pred(k, a, zeros16, zeros16, 1, 1) : 0;
}

static int chroma_pred_inter(int k, const int *a)
{
    return k ? chroma_pred(k, a, zeros16, zeros16, 1, 1) : 0;
}

static int calc_dc(const B2Plane *pl, int x0, int y0)
{
#define AVG(a, b) (((a) + (b) + 1) >> 1)
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        unsigned s[8];
        for (int j = 0; j < 8; j++)
            s[j] = AT(pl, x0 + i, y0 + j);
        sum += AVG(AVG(AVG(s[0], s[1]), AVG(s[2], s[3])), AVG(AVG(s[4], s[5]), AVG(s[6], s[7])));
    }
    return sum;
#undef AVG
}

typedef struct Mv {
    int16_t x, y;
} Mv;

static const Mv zero_mv;

static inline Mv mv_pred(Mv a, Mv b, Mv c)
{
    return (Mv){ b2_mid_pred(a.x, b.x, c.x), b2_mid_pred(a.y, b.y, c.y) };
}

static inline Mv mv_sub(Mv a, Mv o)
{
    return (Mv){ (int16_t)(a.x - o.x), (int16_t)(a.y - o.y) };
}

static inline int mv_eq(Mv a, Mv b)
{
    return a.x == b.x && a.y == b.y;
}

static int mv4_eq(const Mv *a, const Mv *b)
{
    for (int i = 0; i < 4; i++)
        if (!mv_eq(a[i], b[i]))
            return 0;
    return 1;
}

/* ---------------------------------------------------------------- codes */

static void write_code_zero(BW *s, uint32_t v)
{
    uint32_t pfx = 4;
    if (v < 4) {
        unary_lim(s, v, 12, 0);
        return;
    }
    while ((1U << (pfx - 2)) + 2 <= v)
        pfx++;
    unary_lim(s, pfx, 12, 0);
    bw_write(s, v - 2 - (1U << (pfx - 3)), pfx - 3);
}

static void write_level(BW *s, int level)
{
    uint32_t v = FFABS(level);
    if (v <= 3) {
        unary_lim(s, v - 1, 12, 0);
    } else {
        uint32_t pfx = 4;
        while ((1U << (pfx - 2)) + 2 <= v)
            pfx++;
        unary_lim(s, pfx - 1, 12, 0);
        bw_write(s, v - 2 - (1U << (pfx - 3)), pfx - 3);
    }
    bw_bit(s, level < 0);
}

static void write_ac_sym(BW *s, int sym, int cb1)
{
    if (cb1)
        bw_write(s, ac_codes1[sym], ac_bits1[sym]);
    else
        bw_write(s, ac_codes2[sym], ac_bits2[sym]);
}

static void write_block_acs(BW *s, const int *levels, int cb1)
{
    int idx = 1;
    while (idx < 64) {
        int nz = idx, run;
        while (nz < 64 && !levels[zigzag[nz]])
            nz++;
        if (nz >= 64) {
            write_ac_sym(s, SYM_EOB, cb1);
            return;
        }
        run = nz - idx;
        if (run <= 10) {
            write_ac_sym(s, run, cb1);
        } else {
            write_ac_sym(s, SYM_ESC_RUN, cb1);
            bw_write(s, run, 6);
        }
        write_level(s, levels[zigzag[nz]]);
        idx = nz + 1;
    }
}

static size_t block_cost(const int *levels, int cb1)
{
    BW c = { .count = 1 };
    write_block_acs(&c, levels, cb1);
    return c.nbits;
}

static void write_mv_component(BW *s, int v)
{
    if (v >= -7 && v <= 7) {
        int idx = v >= 0 ? v : v + 15;
        bw_write(s, mv_codes[idx], mv_bits[idx]);
    } else {
        uint32_t uval = v >= 0 ? 2U * v : (uint32_t)(-2 * v - 1);
        uint32_t len = 0, nlen;
        bw_write(s, mv_codes[15], mv_bits[15]);
        while (2 * ((1U << (len + 4)) - 1) < uval)
            len++;
        nlen = len + 4;
        unary_lim(s, len, 12, 1);
        bw_write(s, uval - ((1U << nlen) - 1), nlen);
    }
}

static void write_mv1(BW *s, Mv raw)
{
    bw_bit(s, 1);
    write_mv_component(s, raw.x);
    write_mv_component(s, raw.y);
}

static void write_mv4(BW *s, const Mv *raw)
{
    bw_bit(s, 0);
    for (int i = 0; i < 4; i++)
        write_mv_component(s, raw[i].x);
    for (int i = 0; i < 4; i++)
        write_mv_component(s, raw[i].y);
}

static int btype_code(const uint8_t *lru, int btype)
{
    return lru[0] == btype ? 0 : lru[1] == btype ? 1 : lru[3] == btype ? 2 : 3;
}

static void write_btype(BW *s, uint8_t *lru, int btype)
{
    int code = btype_code(lru, btype);
    unary_lim(s, code, 3, 1);
    switch (code) {
    case 0: break;
    case 1: FFSWAP(uint8_t, lru[0], lru[1]); break;
    case 2: lru[3] = lru[2]; lru[2] = btype; break;
    default: lru[2] = lru[1]; lru[1] = btype; break;
    }
}

static size_t btype_cost(const uint8_t *lru, int btype)
{
    int code = btype_code(lru, btype);
    return FFMIN(code + (code < 3), 3);
}

static void write_quant_delta(BW *s, int diff)
{
    uint32_t d = FFABS(diff);
    if (!diff) {
        unary_lim(s, 0, 4, 1);
        return;
    }
    if (d <= 2) {
        unary_lim(s, d, 4, 1);
    } else if (d <= 4) {
        unary_lim(s, 3, 4, 1);
        bw_write(s, d - 3, 1);
    } else {
        unary_lim(s, 4, 4, 1);
        bw_write(s, d - 5, 5);
    }
    bw_bit(s, diff < 0);
}

static void write_cbp_luma(BW *s, uint32_t cbp, uint32_t prev_cbp, int cb1)
{
    int cnt = av_popcount(prev_cbp & 0xFFFF);
    int min_count = cnt < 8 ? cnt : 16 - cnt;
    uint32_t mask = cnt < 8 ? 0 : 0xFFFF;
    uint32_t raw = (cbp & 0xFFFF) ^ mask;
    if (!raw) {
        bw_bit(s, 1);
    } else {
        bw_bit(s, 0);
        if (min_count >= 4) {
            bw_write(s, raw, 16);
        } else {
            for (int i = 0; i < 4; i++) {
                uint32_t nib = raw >> (i * 4) & 0xF;
                if (!nib) {
                    bw_bit(s, 1);
                } else {
                    bw_bit(s, 0);
                    bw_write(s, nib, 4);
                }
            }
        }
    }
    if (cbp & 0xFFFF)
        bw_bit(s, cb1);
}

static void write_cbp_chroma(BW *s, uint32_t full_cbp, uint32_t last_cbp)
{
    uint32_t predicted = (last_cbp & 0xF0000) | chroma_cbps[last_cbp & 0xF];
    if (full_cbp == predicted) {
        bw_bit(s, 1);
    } else {
        bw_bit(s, 0);
        bw_write(s, full_cbp & 0xF, 4);
        bw_bit(s, (full_cbp >> 16) != 0);
    }
}

typedef struct DcCode {
    uint32_t v;
    int neg;
} DcCode;

static void write_dcs(BW *s, const DcCode *codes, int n)
{
    int any = 0;
    for (int i = 0; i < n; i++)
        any |= codes[i].v != 0;
    if (!any) {
        bw_bit(s, 0);
        return;
    }
    bw_bit(s, 1);
    for (int i = 0; i < n; i++) {
        write_code_zero(s, codes[i].v);
        if (codes[i].v)
            bw_bit(s, codes[i].neg);
    }
}

/* ---------------------------------------------------------------- quantisation */

static void quant_dc_delta(int delta, int dcq, uint32_t *code, int *rec)
{
    int64_t v0;
    uint32_t bv = 0;
    int br = 0, berr = delta;
    if (delta <= 0) {
        *code = 0;
        *rec = 0;
        return;
    }
    v0 = ((int64_t)delta * 1024 + dcq / 2) / dcq;
    for (int64_t v = v0 - 1; v <= v0 + 1; v++) {
        int r, err;
        if (v < 0 || v > MAX_DC_CODE)
            continue;
        r = (int)((v * dcq + 0x1FF) >> 10);
        err = FFABS(r - delta);
        if (err < berr) {
            bv = v;
            br = r;
            berr = err;
        }
    }
    *code = bv;
    *rec = br;
}

static inline int q_dc_index(int q)
{
    return FFMAX(q, 8);
}

typedef struct QBlock {
    double dc_target;
    int levels[64];
    int deq[64];
    int has_ac;
} QBlock;

static void analyse_block(const double f[8][8], const double px[64],
                          const int32_t qmat[4][64], int q, double deadzone, QBlock *b)
{
    double coef[64];
    const int32_t *qm = qmat[q & 3];
    int shift = q >> 2;

    forward_dct(f, px, coef);
    memset(b->levels, 0, sizeof(b->levels));
    memset(b->deq, 0, sizeof(b->deq));
    b->has_ac = 0;
    for (int pos = 1; pos < 64; pos++) {
        double step = (double)((int64_t)qm[pos] << shift) / 128.0;
        double r = coef[pos] / step;
        int l = (int)floor(fabs(r) + deadzone);
        if (l > MAX_LEVEL)
            l = MAX_LEVEL;
        if (r < 0.0)
            l = -l;
        if (l) {
            b->levels[pos] = l;
            b->has_ac = 1;
            b->deq[pos] = (int)(((((int64_t)l * qm[pos]) << shift) + 0x40) >> 7);
        }
    }
    b->dc_target = coef[0] / 8.0;
}

static inline void luma_block_pos(int bx, int by, int k, int *x, int *y)
{
    int quad = k / 4, blk = k % 4;
    *x = bx * 32 + (quad & 1) * 16 + (blk & 1) * 8;
    *y = by * 32 + (quad >> 1) * 16 + (blk >> 1) * 8;
}

static inline void chroma_block_pos(int bx, int by, int k, int *x, int *y)
{
    *x = bx * 16 + (k & 1) * 8;
    *y = by * 16 + (k >> 1) * 8;
}

/* ---------------------------------------------------------------- motion compensation */

static inline int luma_filter(const uint8_t *s, ptrdiff_t step)
{
    int t0 = s[0] + s[step];
    int t1 = s[-step] + s[2 * step];
    int t2 = s[-2 * step] + s[3 * step];
    return (((t0 * 19) >> 1) - t1 * 2 + (t2 >> 1) + 8) >> 4;
}

static inline int luma_filter_i(const int *s, int step)
{
    int t0 = s[0] + s[step];
    int t1 = s[-step] + s[2 * step];
    int t2 = s[-2 * step] + s[3 * step];
    return (((t0 * 19) >> 1) - t1 * 2 + (t2 >> 1) + 8) >> 4;
}

static void mc_luma16(const B2Plane *r, int xoff, int yoff, Mv mv, uint8_t out[256])
{
    ptrdiff_t st = r->stride;
    const uint8_t *base = r->data + (ptrdiff_t)(yoff + (mv.y >> 1)) * st + xoff + (mv.x >> 1);
    int mode = (mv.x & 1) + (mv.y & 1) * 2;

    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++)
            memcpy(out + y * 16, base + y * st, 16);
        break;
    case 1:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                out[y * 16 + x] = av_clip_uint8(luma_filter(base + y * st + x, 1));
        break;
    case 2:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                out[y * 16 + x] = av_clip_uint8(luma_filter(base + y * st + x, st));
        break;
    default: {
        int tmp[21 * 16];
        for (int row = 0; row < 21; row++)
            for (int x = 0; x < 16; x++)
                tmp[row * 16 + x] = (int16_t)luma_filter(base + (row - 2) * st + x, 1);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                out[y * 16 + x] = av_clip_uint8(luma_filter_i(tmp + (y + 2) * 16 + x, 16));
        break;
    }
    }
}

static void mc_chroma8(const B2Plane *r, int xoff, int yoff, Mv mv, uint8_t out[64])
{
    ptrdiff_t st = r->stride;
    const uint8_t *base = r->data + (ptrdiff_t)(yoff + (mv.y >> 2)) * st + xoff + (mv.x >> 2);
    int mx = mv.x & 3, my = mv.y & 3;

    if (!mx && !my) {
        for (int y = 0; y < 8; y++)
            memcpy(out + y * 8, base + y * st, 8);
    } else if (!my) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const uint8_t *o = base + y * st + x;
                out[y * 8 + x] = bink2_chroma_interp(o[0], o[1], mx, 2);
            }
    } else if (!mx) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const uint8_t *o = base + y * st + x;
                out[y * 8 + x] = bink2_chroma_interp(o[0], o[st], my, 2);
            }
    } else {
        int tmp[9 * 8];
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 8; x++) {
                const uint8_t *o = base + y * st + x;
                tmp[y * 8 + x] = bink2_chroma_interp(o[0], o[1], mx, 0);
            }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                out[y * 8 + x] = bink2_chroma_interp(tmp[y * 8 + x], tmp[(y + 1) * 8 + x], my, 4);
    }
}

/* ---------------------------------------------------------------- slice state */

typedef struct ResidualPlan {
    QBlock y[16], v[4], u[4];
    int chroma_on;
    uint8_t pred_y[4][256], pred_u[4][64], pred_v[4][64];
} ResidualPlan;

typedef struct SliceCtx {
    BW bw;
    int sb, se;
    int stats[4];
    uint8_t *quants;     ///< bw * 4
    int qpos, pqpos;
    int ydc_cur[16], *ydc_prev, *ydc_new;
    int udc_cur[4], *udc_prev, *udc_new;
    int vdc_cur[4], *vdc_prev, *vdc_new;
    Mv *mvs;             ///< bw * 8
    int mcur, mprev;
    int *ydc_bufs[2], *udc_bufs[2], *vdc_bufs[2];
    Mv *tried;
    int nb_tried, tried_alloc;
    ResidualPlan plans[2];
} SliceCtx;

typedef struct Bink2EncContext {
    const AVClass *class;
    int q_opt, qmin, qmax;
    double rate;
    double lambda_opt;
    double deadzone;
    int search;
    int mv4;
    int row_flag;
    int container_flags;

    int w, h, bw, bh;
    int nb_slices;
    int slice_start[MAX_SLICES], slice_end[MAX_SLICES];
    double fdct[8][8];
    B2Frame src, recon, ref;
    int have_ref;
    Mv *prev_mvs, *prev_mvs_next;
    int scene_cuts, since_key;
    int stats[4];
    int intra;              ///< type of the frame being coded
    int q;
    double lambda;
    SliceCtx slices[MAX_SLICES];

    uint8_t *last_input;    ///< previous input frame, packed
    int have_last;
    int64_t frame_num;
    int cur_q;
    double avg;
} Bink2EncContext;

static inline Mv mvc(const SliceCtx *sc, int bx, int i) { return sc->mvs[sc->mcur + bx * 4 + i]; }
static inline Mv mvp(const SliceCtx *sc, int bx, int i) { return sc->mvs[sc->mprev + bx * 4 + i]; }

static uint8_t quant_pred(const SliceCtx *sc, int nbw, int pos, int top)
{
    int left = pos < 2;
    if (top)
        return left ? 16 : sc->quants[sc->qpos + pos - 2];
    if (left)
        return sc->quants[sc->pqpos + pos];
    return b2_mid_pred(sc->quants[sc->pqpos + pos - 2], sc->quants[sc->pqpos + pos],
                       sc->quants[sc->qpos + pos - 2]);
}

static void raw4(const SliceCtx *sc, int bx, int top, int left, const Mv *m, Mv *r)
{
    if (top) {
        if (left) {
            r[0] = m[0];
            r[1] = mv_sub(m[1], m[0]);
            r[2] = mv_sub(m[2], m[0]);
            r[3] = mv_sub(m[3], mv_pred(m[0], m[1], m[2]));
        } else {
            r[0] = mv_sub(m[0], mv_pred(mvc(sc, bx, -4), mvc(sc, bx, -3), mvc(sc, bx, -1)));
            r[2] = mv_sub(m[2], mv_pred(mvc(sc, bx, -3), mvc(sc, bx, -1), m[0]));
            r[1] = mv_sub(m[1], mv_pred(mvc(sc, bx, -3), m[0], m[2]));
            r[3] = mv_sub(m[3], mv_pred(m[0], m[1], m[2]));
        }
    } else if (left) {
        r[0] = mv_sub(m[0], mv_pred(mvp(sc, bx, 0), mvp(sc, bx, 2), mvp(sc, bx, 3)));
        r[1] = mv_sub(m[1], mv_pred(mvp(sc, bx, 2), mvp(sc, bx, 3), m[0]));
        r[2] = mv_sub(m[2], mv_pred(mvp(sc, bx, 2), m[0], m[1]));
        r[3] = mv_sub(m[3], mv_pred(m[0], m[1], m[2]));
    } else {
        r[0] = mv_sub(m[0], mv_pred(mvp(sc, bx, 2), mvp(sc, bx, -1), mvc(sc, bx, -3)));
        r[1] = mv_sub(m[1], mv_pred(mvp(sc, bx, 2), mvp(sc, bx, 3), m[0]));
        r[2] = mv_sub(m[2], mv_pred(mvc(sc, bx, -3), mvc(sc, bx, -1), m[0]));
        r[3] = mv_sub(m[3], mv_pred(m[0], m[1], m[2]));
    }
}

static int neighbours(const SliceCtx *sc, int bx, int top, int left, Mv *v)
{
    int n = 0;
    if (!left)
        for (int i = -4; i < 0; i++)
            v[n++] = mvc(sc, bx, i);
    if (!top) {
        for (int i = 0; i < 4; i++)
            v[n++] = mvp(sc, bx, i);
        if (!left)
            v[n++] = mvp(sc, bx, -1);
    }
    return n;
}

static void set4(SliceCtx *sc, int bx, const Mv *v)
{
    for (int i = 0; i < 4; i++)
        sc->mvs[sc->mcur + bx * 4 + i] = v[i];
}

static Mv pred1(const SliceCtx *sc, int bx, int top, int left)
{
    if (top)
        return left ? zero_mv : mv_pred(mvc(sc, bx, -4), mvc(sc, bx, -3), mvc(sc, bx, -1));
    if (left)
        return mv_pred(mvp(sc, bx, 0), mvp(sc, bx, 2), mvp(sc, bx, 3));
    return mv_pred(mvp(sc, bx, -1), mvp(sc, bx, 2), mvc(sc, bx, -3));
}

static void predict_intra_mvs(SliceCtx *sc, int bx, int top, int left)
{
    Mv v[4];
    if (top) {
        if (left) {
            v[0] = v[1] = v[2] = v[3] = zero_mv;
        } else {
            v[0] = mv_pred(mvc(sc, bx, -4), mvc(sc, bx, -3), mvc(sc, bx, -1));
            v[2] = mv_pred(mvc(sc, bx, -3), mvc(sc, bx, -1), v[0]);
            v[1] = mv_pred(mvc(sc, bx, -1), v[0], v[2]);
            v[3] = mv_pred(v[0], v[1], v[2]);
        }
    } else if (left) {
        v[0] = mv_pred(mvp(sc, bx, 0), mvp(sc, bx, 2), mvp(sc, bx, 3));
        v[1] = mv_pred(v[0], mvp(sc, bx, 2), mvp(sc, bx, 3));
        v[2] = mv_pred(mvp(sc, bx, 2), v[1], v[0]);
        v[3] = mv_pred(v[0], v[1], v[2]);
    } else {
        v[0] = mv_pred(mvc(sc, bx, -3), mvp(sc, bx, -1), mvp(sc, bx, 2));
        v[1] = mv_pred(v[0], mvp(sc, bx, 2), mvp(sc, bx, 3));
        v[2] = mv_pred(mvc(sc, bx, -3), mvc(sc, bx, -1), v[0]);
        v[3] = mv_pred(v[0], v[1], v[2]);
    }
    set4(sc, bx, v);
}

/* ---------------------------------------------------------------- macroblocks */

static void src_block(const B2Plane *pl, int x0, int y0, double px[64])
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            px[y * 8 + x] = AT(pl, x0 + x, y0 + y);
}

static void reconstruct_intra(B2Plane *dst, int x0, int y0, int dc, const int *deq)
{
    int c[64];
    memcpy(c, deq, sizeof(c));
    c[0] = dc * 8 + 32;
    idct_int(c);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            AT(dst, x0 + x, y0 + y) = av_clip_uint8(c[y * 8 + x]);
}

static void sum_costs(const QBlock *b, int n, uint32_t cbp, size_t *c1, size_t *c2)
{
    *c1 = *c2 = 0;
    for (int k = 0; k < n; k++)
        if (cbp >> k & 1) {
            *c1 += block_cost(b[k].levels, 1);
            *c2 += block_cost(b[k].levels, 0);
        }
}

static uint32_t encode_luma_intra(Bink2EncContext *s, SliceCtx *sc, BW *bw, int bx, int by,
                                  int top, int left, uint32_t prev_cbp)
{
    QBlock blocks[16];
    uint32_t cbp = 0, full;
    size_t c1, c2;
    int cb1, dcq = quant_dc[q_dc_index(s->q)];
    int lefts[16], prev_row[8], a[16] = { 0 };
    DcCode codes[16];

    for (int k = 0; k < 16; k++) {
        int x0, y0;
        double px[64];
        luma_block_pos(bx, by, k, &x0, &y0);
        src_block(&s->src.p[0], x0, y0, px);
        analyse_block(s->fdct, px, quant_intra_luma, s->q, s->deadzone, &blocks[k]);
        if (blocks[k].has_ac)
            cbp |= 1U << k;
    }
    sum_costs(blocks, 16, cbp, &c1, &c2);
    cb1 = cbp && c1 < c2;
    write_cbp_luma(bw, cbp, prev_cbp, cb1);
    full = cb1 ? cbp | cbp << 16 : cbp;
    memcpy(lefts, sc->ydc_cur, sizeof(lefts));
    for (int i = 0; i < 8; i++)
        prev_row[i] = sc->ydc_prev[bx * 4 + i];
    for (int k = 0; k < 16; k++) {
        int pred = luma_pred(k, a, lefts, prev_row, top, left);
        int delta = (int)round(blocks[k].dc_target) - pred, r;
        quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
        codes[k].neg = delta < 0;
        a[k] = av_clip(delta < 0 ? pred - r : pred + r, 0, 0x7FF);
    }
    write_dcs(bw, codes, 16);
    memcpy(sc->ydc_cur, a, sizeof(a));
    sc->ydc_new[bx * 4 + 4] = a[10];
    sc->ydc_new[bx * 4 + 5] = a[11];
    sc->ydc_new[bx * 4 + 6] = a[14];
    sc->ydc_new[bx * 4 + 7] = a[15];
    for (int k = 0; k < 16; k++)
        if (cbp >> k & 1)
            write_block_acs(bw, blocks[k].levels, cb1);
    for (int k = 0; k < 16; k++) {
        static const int zero64[64];
        int x0, y0;
        luma_block_pos(bx, by, k, &x0, &y0);
        reconstruct_intra(&s->recon.p[0], x0, y0, a[k], cbp >> k & 1 ? blocks[k].deq : zero64);
    }
    return full;
}

static uint32_t encode_chroma_intra(Bink2EncContext *s, SliceCtx *sc, BW *bw, int plane_id,
                                    int bx, int by, int top, int left, uint32_t prev_cbp)
{
    const B2Plane *plane = &s->src.p[plane_id];
    int *cur  = plane_id == 1 ? sc->udc_cur : sc->vdc_cur;
    int *prev = plane_id == 1 ? sc->udc_prev : sc->vdc_prev;
    int *nw   = plane_id == 1 ? sc->udc_new : sc->vdc_new;
    QBlock blocks[4];
    uint32_t cbp = 0, full;
    size_t c1, c2;
    int cb1, dcq = quant_dc[q_dc_index(s->q)];
    int lefts[4], prev_row[4], a[4] = { 0 };
    DcCode codes[4];

    for (int k = 0; k < 4; k++) {
        int x0, y0;
        double px[64];
        chroma_block_pos(bx, by, k, &x0, &y0);
        src_block(plane, x0, y0, px);
        analyse_block(s->fdct, px, quant_intra_chroma, s->q, s->deadzone, &blocks[k]);
        if (blocks[k].has_ac)
            cbp |= 1U << k;
    }
    sum_costs(blocks, 4, cbp, &c1, &c2);
    cb1 = cbp && c1 < c2;
    full = cb1 ? cbp | cbp << 16 : cbp;
    write_cbp_chroma(bw, full, prev_cbp);
    memcpy(lefts, cur, sizeof(lefts));
    for (int i = 0; i < 4; i++)
        prev_row[i] = prev[bx * 2 + i];
    for (int k = 0; k < 4; k++) {
        int pred = chroma_pred(k, a, lefts, prev_row, top, left);
        int delta = (int)round(blocks[k].dc_target) - pred, r;
        quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
        codes[k].neg = delta < 0;
        a[k] = av_clip(delta < 0 ? pred - r : pred + r, 0, 0x7FF);
    }
    write_dcs(bw, codes, 4);
    memcpy(cur, a, sizeof(a));
    nw[bx * 2 + 2] = a[2];
    nw[bx * 2 + 3] = a[3];
    for (int k = 0; k < 4; k++)
        if (cbp >> k & 1)
            write_block_acs(bw, blocks[k].levels, cb1);
    for (int k = 0; k < 4; k++) {
        static const int zero64[64];
        int x0, y0;
        chroma_block_pos(bx, by, k, &x0, &y0);
        reconstruct_intra(&s->recon.p[plane_id], x0, y0, a[k], cbp >> k & 1 ? blocks[k].deq : zero64);
    }
    return full;
}

/* Estimated intra bits and distortion, without writing or changing state. */
static void intra_cost(Bink2EncContext *s, SliceCtx *sc, int bx, int by, int top, int left,
                       uint32_t cbp_y, uint32_t cbp_v, uint32_t cbp_u, size_t *obits, double *osse)
{
    BW bits = { .count = 1 };
    double sse = 0;
    int dcq = quant_dc[q_dc_index(s->q)];
    {
        QBlock blocks[16];
        uint32_t cbp = 0;
        size_t c1, c2;
        int prev_row[8], a[16] = { 0 };
        DcCode codes[16];
        for (int k = 0; k < 16; k++) {
            int x0, y0;
            double px[64];
            luma_block_pos(bx, by, k, &x0, &y0);
            src_block(&s->src.p[0], x0, y0, px);
            analyse_block(s->fdct, px, quant_intra_luma, s->q, s->deadzone, &blocks[k]);
            if (blocks[k].has_ac)
                cbp |= 1U << k;
        }
        sum_costs(blocks, 16, cbp, &c1, &c2);
        write_cbp_luma(&bits, cbp, cbp_y, cbp && c1 < c2);
        bits.nbits += FFMIN(c1, c2);
        for (int i = 0; i < 8; i++)
            prev_row[i] = sc->ydc_prev[bx * 4 + i];
        for (int k = 0; k < 16; k++) {
            int pred = luma_pred(k, a, sc->ydc_cur, prev_row, top, left);
            int delta = (int)round(blocks[k].dc_target) - pred, r;
            quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
            codes[k].neg = delta < 0;
            a[k] = av_clip(delta < 0 ? pred - r : pred + r, 0, 0x7FF);
        }
        write_dcs(&bits, codes, 16);
        for (int k = 0; k < 16; k++) {
            int x0, y0, c[64];
            luma_block_pos(bx, by, k, &x0, &y0);
            if (cbp >> k & 1)
                memcpy(c, blocks[k].deq, sizeof(c));
            else
                memset(c, 0, sizeof(c));
            c[0] = a[k] * 8 + 32;
            idct_int(c);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int d = av_clip(c[y * 8 + x], 0, 255) - AT(&s->src.p[0], x0 + x, y0 + y);
                    sse += (double)(d * d);
                }
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        int plane_id = pass ? 1 : 2;
        const int *cur  = plane_id == 1 ? sc->udc_cur : sc->vdc_cur;
        const int *prev = plane_id == 1 ? sc->udc_prev : sc->vdc_prev;
        uint32_t prev_cbp = plane_id == 1 ? cbp_u : cbp_v;
        const B2Plane *plane = &s->src.p[plane_id];
        QBlock blocks[4];
        uint32_t cbp = 0;
        size_t c1, c2;
        int cb1, prev_row[4], a[4] = { 0 };
        DcCode codes[4];
        for (int k = 0; k < 4; k++) {
            int x0, y0;
            double px[64];
            chroma_block_pos(bx, by, k, &x0, &y0);
            src_block(plane, x0, y0, px);
            analyse_block(s->fdct, px, quant_intra_chroma, s->q, s->deadzone, &blocks[k]);
            if (blocks[k].has_ac)
                cbp |= 1U << k;
        }
        sum_costs(blocks, 4, cbp, &c1, &c2);
        cb1 = cbp && c1 < c2;
        write_cbp_chroma(&bits, cb1 ? cbp | cbp << 16 : cbp, prev_cbp);
        bits.nbits += FFMIN(c1, c2);
        for (int i = 0; i < 4; i++)
            prev_row[i] = prev[bx * 2 + i];
        for (int k = 0; k < 4; k++) {
            int pred = chroma_pred(k, a, cur, prev_row, top, left);
            int delta = (int)round(blocks[k].dc_target) - pred, r;
            quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
            codes[k].neg = delta < 0;
            a[k] = av_clip(delta < 0 ? pred - r : pred + r, 0, 0x7FF);
        }
        write_dcs(&bits, codes, 4);
        for (int k = 0; k < 4; k++) {
            int x0, y0, c[64];
            chroma_block_pos(bx, by, k, &x0, &y0);
            if (cbp >> k & 1)
                memcpy(c, blocks[k].deq, sizeof(c));
            else
                memset(c, 0, sizeof(c));
            c[0] = a[k] * 8 + 32;
            idct_int(c);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int d = av_clip(c[y * 8 + x], 0, 255) - AT(plane, x0 + x, y0 + y);
                    sse += (double)(d * d);
                }
        }
    }
    *obits = bits.nbits;
    *osse = sse;
}

/* The reference decoder keeps only the picture rounded up to 16 luma
 * pixels, so prediction may not reach past that. */
static int mv_valid_blk(const Bink2EncContext *s, int bx, int by, int blk, Mv mv)
{
    int aw = FFALIGN(s->w, 16), ah = FFALIGN(s->h, 16);
    int dx = mv.x & 1 ? 2 : 0, ax = mv.x & 1 ? 5 : 0;
    int dy = mv.y & 1 ? 2 : 0, ay = mv.y & 1 ? 5 : 0;
    int xoff = bx * 32 + (blk & 1) * 16, yoff = by * 32 + (blk & 2) * 8;
    int sx = xoff + (mv.x >> 1), sy = yoff + (mv.y >> 1);
    int cw = aw >> 1, chh = ah >> 1;
    int cx, cy, addx, addy;

    if (!(sx - dx >= 0 && sx - dx + ax + 16 <= aw && sy - dy >= 0 && sy - dy + ay + 16 <= ah))
        return 0;
    cx = xoff / 2 + (mv.x >> 2);
    cy = yoff / 2 + (mv.y >> 2);
    addx = (mv.x & 3) != 0;
    addy = (mv.y & 3) != 0;
    return cx >= 0 && cx + addx + 8 <= cw && cy >= 0 && cy + addy + 8 <= chh;
}

static void predict_mb(const Bink2EncContext *s, int bx, int by, const Mv *mvs,
                       uint8_t py[4][256], uint8_t pu[4][64], uint8_t pv[4][64])
{
    for (int blk = 0; blk < 4; blk++) {
        int xoff = bx * 32 + (blk & 1) * 16, yoff = by * 32 + (blk & 2) * 8;
        mc_luma16(&s->ref.p[0], xoff, yoff, mvs[blk], py[blk]);
        mc_chroma8(&s->ref.p[1], xoff >> 1, yoff >> 1, mvs[blk], pu[blk]);
        mc_chroma8(&s->ref.p[2], xoff >> 1, yoff >> 1, mvs[blk], pv[blk]);
    }
}

static int sad_blk(const Bink2EncContext *s, int bx, int by, int blk, Mv mv, int limit)
{
    int xoff = bx * 32 + (blk & 1) * 16, yoff = by * 32 + (blk & 2) * 8;
    uint8_t buf[256];
    int sad = 0;
    mc_luma16(&s->ref.p[0], xoff, yoff, mv, buf);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++)
            sad += FFABS(buf[y * 16 + x] - AT(&s->src.p[0], xoff + x, yoff + y));
        if (sad > limit)
            return sad;
    }
    return sad;
}

static int tried_add(SliceCtx *sc, Mv mv)
{
    for (int i = 0; i < sc->nb_tried; i++)
        if (mv_eq(sc->tried[i], mv))
            return 0;
    if (sc->nb_tried >= sc->tried_alloc) {
        int na = sc->tried_alloc * 2 + 128;
        Mv *nt = av_realloc_array(sc->tried, na, sizeof(*nt));
        if (!nt)
            return 0;
        sc->tried = nt;
        sc->tried_alloc = na;
    }
    sc->tried[sc->nb_tried++] = mv;
    return 1;
}

static void try_mv(const Bink2EncContext *s, SliceCtx *sc, int bx, int by,
                   const int *blks, int nblks, Mv mv, Mv *best, int *best_sad)
{
    int sum = 0;
    for (int i = 0; i < nblks; i++)
        if (!mv_valid_blk(s, bx, by, blks[i], mv))
            return;
    /* the reference marks a vector tried only once it is valid */
    if (!tried_add(sc, mv))
        return;
    for (int i = 0; i < nblks; i++) {
        sum += sad_blk(s, bx, by, blks[i], mv, *best_sad - sum);
        if (sum >= *best_sad)
            return;
    }
    if (sum < *best_sad) {
        *best_sad = sum;
        *best = mv;
    }
}

static Mv search(const Bink2EncContext *s, SliceCtx *sc, int bx, int by,
                 const int *blks, int nblks, const Mv *cands, int ncands, int *osad)
{
    static const int dirs[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 },
    };
    Mv best = zero_mv, c;
    int best_sad = INT_MAX, lim = s->search * 2, step = 8;

    sc->nb_tried = 0;
    try_mv(s, sc, bx, by, blks, nblks, zero_mv, &best, &best_sad);
    for (int i = 0; i < ncands; i++) {
        Mv e = { cands[i].x & ~1, cands[i].y & ~1 };
        try_mv(s, sc, bx, by, blks, nblks, cands[i], &best, &best_sad);
        try_mv(s, sc, bx, by, blks, nblks, e, &best, &best_sad);
    }
    while (step >= 1) {
        c = best;
        for (int d = 0; d < 8; d++) {
            Mv mv = { (int16_t)(c.x + dirs[d][0] * step * 2), (int16_t)(c.y + dirs[d][1] * step * 2) };
            if (FFABS(mv.x) > lim || FFABS(mv.y) > lim)
                continue;
            try_mv(s, sc, bx, by, blks, nblks, mv, &best, &best_sad);
        }
        if (mv_eq(best, c))
            step /= 2;
    }
    c = best;
    for (int d = 0; d < 8; d++) {
        Mv mv = { (int16_t)(c.x + dirs[d][0]), (int16_t)(c.y + dirs[d][1]) };
        try_mv(s, sc, bx, by, blks, nblks, mv, &best, &best_sad);
    }
    *osad = best_sad;
    return best;
}

static void plan_residual(Bink2EncContext *s, int bx, int by, const Mv *mvs,
                          uint32_t cbp_y_p, uint32_t cbp_v_p, uint32_t cbp_u_p,
                          ResidualPlan *plan, size_t *obits, double *osse)
{
    BW bits = { .count = 1 }, chroma_bits = { .count = 1 };
    double sse = 0, chroma_sse = 0, chroma_sse_none = 0, with, without;
    uint32_t cbp = 0;
    size_t c1, c2;
    int dcq = quant_dc[q_dc_index(s->q)];
    int a[16] = { 0 };
    DcCode codes[16];

    predict_mb(s, bx, by, mvs, plan->pred_y, plan->pred_u, plan->pred_v);
    for (int k = 0; k < 16; k++) {
        int quad = k / 4, blk = k % 4, x0, y0;
        double px[64];
        luma_block_pos(bx, by, k, &x0, &y0);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                double pxv = plan->pred_y[quad][((blk >> 1) * 8 + y) * 16 + (blk & 1) * 8 + x];
                px[y * 8 + x] = (double)AT(&s->src.p[0], x0 + x, y0 + y) - pxv;
            }
        analyse_block(s->fdct, px, quant_inter, s->q, s->deadzone, &plan->y[k]);
        if (plan->y[k].has_ac)
            cbp |= 1U << k;
    }
    sum_costs(plan->y, 16, cbp, &c1, &c2);
    write_cbp_luma(&bits, cbp, cbp_y_p, cbp && c1 < c2);
    bits.nbits += FFMIN(c1, c2);
    for (int k = 0; k < 16; k++) {
        int pred = luma_pred_inter(k, a);
        int delta = (int)round(plan->y[k].dc_target) - pred, r;
        quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
        codes[k].neg = delta < 0;
        a[k] = av_clip(delta < 0 ? pred - r : pred + r, -0x7FF, 0x7FF);
    }
    write_dcs(&bits, codes, 16);
    for (int k = 0; k < 16; k++) {
        int quad = k / 4, blk = k % 4, x0, y0, c[64];
        luma_block_pos(bx, by, k, &x0, &y0);
        if (cbp >> k & 1)
            memcpy(c, plan->y[k].deq, sizeof(c));
        else
            memset(c, 0, sizeof(c));
        c[0] = a[k] * 8 + 32;
        idct_int(c);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int pv = plan->pred_y[quad][((blk >> 1) * 8 + y) * 16 + (blk & 1) * 8 + x];
                int d = av_clip(pv + c[y * 8 + x], 0, 255) - AT(&s->src.p[0], x0 + x, y0 + y);
                sse += (double)(d * d);
            }
    }

    for (int pass = 0; pass < 2; pass++) {
        int plane_id = pass ? 1 : 2;
        const B2Plane *plane = &s->src.p[plane_id];
        uint8_t (*pred)[64] = plane_id == 1 ? plan->pred_u : plan->pred_v;
        QBlock *out = plane_id == 1 ? plan->u : plan->v;
        uint32_t prev_cbp = plane_id == 1 ? cbp_u_p : cbp_v_p, ccbp = 0;
        int cb1, ca[4] = { 0 };
        DcCode ccodes[4];
        for (int k = 0; k < 4; k++) {
            int x0, y0;
            double px[64];
            chroma_block_pos(bx, by, k, &x0, &y0);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    px[y * 8 + x] = (double)AT(plane, x0 + x, y0 + y) - (double)pred[k][y * 8 + x];
            analyse_block(s->fdct, px, quant_inter, s->q, s->deadzone, &out[k]);
            if (out[k].has_ac)
                ccbp |= 1U << k;
        }
        sum_costs(out, 4, ccbp, &c1, &c2);
        cb1 = ccbp && c1 < c2;
        write_cbp_chroma(&chroma_bits, cb1 ? ccbp | ccbp << 16 : ccbp, prev_cbp);
        chroma_bits.nbits += FFMIN(c1, c2);
        for (int k = 0; k < 4; k++) {
            int pdc = chroma_pred_inter(k, ca);
            int delta = (int)round(out[k].dc_target) - pdc, r;
            quant_dc_delta(FFABS(delta), dcq, &ccodes[k].v, &r);
            ccodes[k].neg = delta < 0;
            ca[k] = av_clip(delta < 0 ? pdc - r : pdc + r, -0x7FF, 0x7FF);
        }
        write_dcs(&chroma_bits, ccodes, 4);
        for (int k = 0; k < 4; k++) {
            int x0, y0, c[64];
            chroma_block_pos(bx, by, k, &x0, &y0);
            if (ccbp >> k & 1)
                memcpy(c, out[k].deq, sizeof(c));
            else
                memset(c, 0, sizeof(c));
            c[0] = ca[k] * 8 + 32;
            idct_int(c);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int sv = AT(plane, x0 + x, y0 + y), pv = pred[k][y * 8 + x];
                    int d = av_clip(pv + c[y * 8 + x], 0, 255) - sv, d0 = pv - sv;
                    chroma_sse += (double)(d * d);
                    chroma_sse_none += (double)(d0 * d0);
                }
        }
    }
    with    = chroma_sse + s->lambda * (double)(1 + chroma_bits.nbits);
    without = chroma_sse_none + s->lambda;
    plan->chroma_on = with < without;
    bits.nbits += plan->chroma_on ? 1 + chroma_bits.nbits : 1;
    sse += plan->chroma_on ? chroma_sse : chroma_sse_none;
    *obits = bits.nbits;
    *osse = sse;
}

static double sse_prediction(const Bink2EncContext *s, int bx, int by, const Mv *mvs)
{
    uint8_t py[4][256], pu[4][64], pv[4][64];
    double sse = 0;
    predict_mb(s, bx, by, mvs, py, pu, pv);
    for (int blk = 0; blk < 4; blk++) {
        int xoff = bx * 32 + (blk & 1) * 16, yoff = by * 32 + (blk & 2) * 8, cx, cy;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int d = py[blk][y * 16 + x] - AT(&s->src.p[0], xoff + x, yoff + y);
                sse += (double)(d * d);
            }
        chroma_block_pos(bx, by, blk, &cx, &cy);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int d = pu[blk][y * 8 + x] - AT(&s->src.p[1], cx + x, cy + y);
                sse += (double)(d * d);
                d = pv[blk][y * 8 + x] - AT(&s->src.p[2], cx + x, cy + y);
                sse += (double)(d * d);
            }
    }
    return sse;
}

typedef struct Decision {
    int btype;
    Mv mvs[4];
    int four;
    ResidualPlan *res;
} Decision;

static void decide_inter(Bink2EncContext *s, SliceCtx *sc, int bx, int by, int top, int left,
                         const uint8_t *lru, uint32_t cbp_y, uint32_t cbp_v, uint32_t cbp_u,
                         uint32_t cbp_y_p, uint32_t cbp_v_p, uint32_t cbp_u_p, int q1,
                         Decision *best)
{
    static const int all_blks[4] = { 0, 1, 2, 3 };
    double lambda = s->lambda, best_cost, cost;
    Mv pred = pred1(sc, bx, top, left), cands[24], mv1, mv4[4], zero4[4] = { 0 }, m1x4[4];
    int ncands = 0, sad1, sad4 = 0, four_ok, plan_slot = 0;
    size_t mv1_bits, mv4_bits = SIZE_MAX, qbits;

    cands[ncands++] = pred;
    ncands += neighbours(sc, bx, top, left, cands + ncands);
    for (int i = 0; i < 4; i++)
        cands[ncands++] = s->prev_mvs[((size_t)by * s->bw + bx) * 4 + i];

    mv1 = search(s, sc, bx, by, all_blks, 4, cands, ncands, &sad1);
    {
        BW c = { .count = 1 };
        write_mv1(&c, mv_sub(mv1, pred));
        mv1_bits = c.nbits;
    }
    for (int b = 0; b < 4; b++) {
        int sb;
        cands[ncands] = mv1;
        mv4[b] = search(s, sc, bx, by, &b, 1, cands, ncands + 1, &sb);
        sad4 += sb;
    }
    for (int i = 0; i < 4; i++)
        m1x4[i] = mv1;
    four_ok = s->mv4 && !mv4_eq(mv4, m1x4) && sad4 < sad1;
    if (four_ok) {
        BW c = { .count = 1 };
        Mv raw[4];
        raw4(sc, bx, top, left, mv4, raw);
        write_mv4(&c, raw);
        mv4_bits = c.nbits;
    }
    {
        BW c = { .count = 1 };
        write_quant_delta(&c, s->q - q1);
        qbits = c.nbits;
    }

    best_cost = sse_prediction(s, bx, by, zero4) + lambda * (double)btype_cost(lru, 1);
    best->btype = 1;
    memset(best->mvs, 0, sizeof(best->mvs));
    best->four = 0;
    best->res = NULL;

    for (int pass = 0; pass < 2; pass++) {
        const Mv *mvs = pass ? mv4 : m1x4;
        size_t mvbits = pass ? mv4_bits : mv1_bits, bits;
        ResidualPlan *plan = &sc->plans[plan_slot];
        double sse;
        if (pass && !four_ok)
            continue;
        if (!mv4_eq(mvs, zero4)) {
            double sse_mv = sse_prediction(s, bx, by, mvs);
            cost = sse_mv + lambda * (double)(btype_cost(lru, 2) + mvbits);
            if (cost < best_cost) {
                best_cost = cost;
                best->btype = 2;
                memcpy(best->mvs, mvs, sizeof(best->mvs));
                best->four = pass;
                best->res = NULL;
            }
        }
        plan_residual(s, bx, by, mvs, cbp_y_p, cbp_v_p, cbp_u_p, plan, &bits, &sse);
        cost = sse + lambda * (double)(btype_cost(lru, 3) + mvbits + qbits + bits);
        if (cost < best_cost) {
            best_cost = cost;
            best->btype = 3;
            memcpy(best->mvs, mvs, sizeof(best->mvs));
            best->four = pass;
            best->res = plan;
            plan_slot ^= 1;   /* keep this plan; plan into the other slot next */
        }
    }
    /* The reference decoder never stores the part of the last macroblock row
     * and column past the picture rounded up to 16 pixels, and takes the DCs
     * of inter blocks there from whatever memory it finds. An intra block
     * there predicts from those DCs and can then shade its visible
     * neighbours through the DC smoothing, so keep such macroblocks inter. */
    if (bx * 32 + 32 <= FFALIGN(s->w, 16) && by * 32 + 32 <= FFALIGN(s->h, 16)) {
        size_t bits;
        double sse;
        intra_cost(s, sc, bx, by, top, left, cbp_y, cbp_v, cbp_u, &bits, &sse);
        cost = sse + lambda * (double)(btype_cost(lru, 0) + 1 + bits);
        if (cost < best_cost) {
            best->btype = 0;
            memset(best->mvs, 0, sizeof(best->mvs));
            best->four = 0;
            best->res = NULL;
        }
    }
}

static void write_prediction(Bink2EncContext *s, int bx, int by, const Mv *mvs)
{
    uint8_t py[4][256], pu[4][64], pv[4][64];
    predict_mb(s, bx, by, mvs, py, pu, pv);
    for (int blk = 0; blk < 4; blk++) {
        int xoff = bx * 32 + (blk & 1) * 16, yoff = by * 32 + (blk & 2) * 8, cx, cy;
        for (int y = 0; y < 16; y++)
            memcpy(&AT(&s->recon.p[0], xoff, yoff + y), py[blk] + y * 16, 16);
        chroma_block_pos(bx, by, blk, &cx, &cy);
        for (int y = 0; y < 8; y++) {
            memcpy(&AT(&s->recon.p[1], cx, cy + y), pu[blk] + y * 8, 8);
            memcpy(&AT(&s->recon.p[2], cx, cy + y), pv[blk] + y * 8, 8);
        }
    }
}

static void encode_residual(Bink2EncContext *s, BW *bw, int bx, int by,
                            uint32_t cbp_y_p, uint32_t cbp_v_p, uint32_t cbp_u_p,
                            const ResidualPlan *plan, uint32_t *ny, uint32_t *nv, uint32_t *nu)
{
    int dcq = quant_dc[q_dc_index(s->q)];
    uint32_t cbp = 0;
    size_t c1, c2;
    int cb1, a[16] = { 0 };
    DcCode codes[16];
    uint32_t new_cbps[2] = { 0 };

    for (int k = 0; k < 16; k++)
        if (plan->y[k].has_ac)
            cbp |= 1U << k;
    sum_costs(plan->y, 16, cbp, &c1, &c2);
    cb1 = cbp && c1 < c2;
    write_cbp_luma(bw, cbp, cbp_y_p, cb1);
    *ny = cb1 ? cbp | cbp << 16 : cbp;
    for (int k = 0; k < 16; k++) {
        int pred = luma_pred_inter(k, a);
        int delta = (int)round(plan->y[k].dc_target) - pred, r;
        quant_dc_delta(FFABS(delta), dcq, &codes[k].v, &r);
        codes[k].neg = delta < 0;
        a[k] = av_clip(delta < 0 ? pred - r : pred + r, -0x7FF, 0x7FF);
    }
    write_dcs(bw, codes, 16);
    for (int k = 0; k < 16; k++)
        if (cbp >> k & 1)
            write_block_acs(bw, plan->y[k].levels, cb1);
    for (int k = 0; k < 16; k++) {
        int quad = k / 4, blk = k % 4, x0, y0, c[64];
        luma_block_pos(bx, by, k, &x0, &y0);
        if (cbp >> k & 1)
            memcpy(c, plan->y[k].deq, sizeof(c));
        else
            memset(c, 0, sizeof(c));
        c[0] = a[k] * 8 + 32;
        idct_int(c);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int pv = plan->pred_y[quad][((blk >> 1) * 8 + y) * 16 + (blk & 1) * 8 + x];
                AT(&s->recon.p[0], x0 + x, y0 + y) = av_clip_uint8(pv + c[y * 8 + x]);
            }
    }

    bw_bit(bw, plan->chroma_on);
    if (plan->chroma_on) {
        for (int i = 0; i < 2; i++) {
            int plane_id = i ? 1 : 2;
            const QBlock *blocks = plane_id == 1 ? plan->u : plan->v;
            const uint8_t (*pred)[64] = plane_id == 1 ? plan->pred_u : plan->pred_v;
            uint32_t prev_cbp = plane_id == 1 ? cbp_u_p : cbp_v_p, ccbp = 0, full;
            int ccb1, ca[4] = { 0 };
            DcCode cc[4];
            for (int k = 0; k < 4; k++)
                if (blocks[k].has_ac)
                    ccbp |= 1U << k;
            sum_costs(blocks, 4, ccbp, &c1, &c2);
            ccb1 = ccbp && c1 < c2;
            full = ccb1 ? ccbp | ccbp << 16 : ccbp;
            write_cbp_chroma(bw, full, prev_cbp);
            new_cbps[i] = full;
            for (int k = 0; k < 4; k++) {
                int pdc = chroma_pred_inter(k, ca);
                int delta = (int)round(blocks[k].dc_target) - pdc, r;
                quant_dc_delta(FFABS(delta), dcq, &cc[k].v, &r);
                cc[k].neg = delta < 0;
                ca[k] = av_clip(delta < 0 ? pdc - r : pdc + r, -0x7FF, 0x7FF);
            }
            write_dcs(bw, cc, 4);
            for (int k = 0; k < 4; k++)
                if (ccbp >> k & 1)
                    write_block_acs(bw, blocks[k].levels, ccb1);
            for (int k = 0; k < 4; k++) {
                int x0, y0, c[64];
                chroma_block_pos(bx, by, k, &x0, &y0);
                if (ccbp >> k & 1)
                    memcpy(c, blocks[k].deq, sizeof(c));
                else
                    memset(c, 0, sizeof(c));
                c[0] = ca[k] * 8 + 32;
                idct_int(c);
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        AT(&s->recon.p[plane_id], x0 + x, y0 + y) = av_clip_uint8(pred[k][y * 8 + x] + c[y * 8 + x]);
            }
        }
    } else {
        for (int blk = 0; blk < 4; blk++) {
            int cx, cy;
            chroma_block_pos(bx, by, blk, &cx, &cy);
            for (int y = 0; y < 8; y++) {
                memcpy(&AT(&s->recon.p[1], cx, cy + y), plan->pred_u[blk] + y * 8, 8);
                memcpy(&AT(&s->recon.p[2], cx, cy + y), plan->pred_v[blk] + y * 8, 8);
            }
        }
    }
    *nv = new_cbps[0];
    *nu = new_cbps[1];
}

static void measure_dcs(Bink2EncContext *s, SliceCtx *sc, int bx, int by)
{
    const B2Plane *y = &s->recon.p[0];
    int x0 = bx * 32, y0 = by * 32, cx = bx * 16, cy = by * 16;
    int dc5 = calc_dc(y, x0 + 24, y0), dc7 = calc_dc(y, x0 + 24, y0 + 8);
    int dc13 = calc_dc(y, x0 + 24, y0 + 16);
    int dc10 = calc_dc(y, x0, y0 + 24), dc11 = calc_dc(y, x0 + 8, y0 + 24);
    int dc14 = calc_dc(y, x0 + 16, y0 + 24), dc15 = calc_dc(y, x0 + 24, y0 + 24);

    sc->ydc_cur[5] = dc5;
    sc->ydc_cur[7] = dc7;
    sc->ydc_cur[13] = dc13;
    sc->ydc_cur[15] = dc15;
    sc->ydc_new[bx * 4 + 4] = dc10;
    sc->ydc_new[bx * 4 + 5] = dc11;
    sc->ydc_new[bx * 4 + 6] = dc14;
    sc->ydc_new[bx * 4 + 7] = dc15;
    for (int pid = 1; pid <= 2; pid++) {
        const B2Plane *pl = &s->recon.p[pid];
        int *cur = pid == 1 ? sc->udc_cur : sc->vdc_cur;
        int *nw  = pid == 1 ? sc->udc_new : sc->vdc_new;
        int dc1 = calc_dc(pl, cx + 8, cy), dc2 = calc_dc(pl, cx, cy + 8), dc3 = calc_dc(pl, cx + 8, cy + 8);
        cur[1] = dc1;
        cur[2] = dc2;
        cur[3] = dc3;
        nw[bx * 2 + 2] = dc2;
        nw[bx * 2 + 3] = dc3;
    }
}

/* ---------------------------------------------------------------- slices */

static void slice_reset(Bink2EncContext *s, SliceCtx *sc)
{
    int nbw = s->bw;

    sc->ydc_prev = sc->ydc_bufs[0];
    sc->ydc_new  = sc->ydc_bufs[1];
    sc->udc_prev = sc->udc_bufs[0];
    sc->udc_new  = sc->udc_bufs[1];
    sc->vdc_prev = sc->vdc_bufs[0];
    sc->vdc_new  = sc->vdc_bufs[1];
    memset(sc->quants, 0, nbw * 4);
    sc->qpos = 0;
    sc->pqpos = nbw * 2;
    memset(sc->ydc_cur, 0, sizeof(sc->ydc_cur));
    memset(sc->udc_cur, 0, sizeof(sc->udc_cur));
    memset(sc->vdc_cur, 0, sizeof(sc->vdc_cur));
    memset(sc->ydc_prev, 0, (nbw * 4 + 4) * sizeof(int));
    memset(sc->ydc_new, 0, (nbw * 4 + 4) * sizeof(int));
    memset(sc->udc_prev, 0, (nbw * 2 + 2) * sizeof(int));
    memset(sc->udc_new, 0, (nbw * 2 + 2) * sizeof(int));
    memset(sc->vdc_prev, 0, (nbw * 2 + 2) * sizeof(int));
    memset(sc->vdc_new, 0, (nbw * 2 + 2) * sizeof(int));
    memset(sc->mvs, 0, nbw * 8 * sizeof(Mv));
    sc->mcur = 0;
    sc->mprev = nbw * 4;
    memset(sc->stats, 0, sizeof(sc->stats));
}

static int encode_slice(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    Bink2EncContext *s = avctx->priv_data;
    SliceCtx *sc = &s->slices[jobnr];
    BW *bw = &sc->bw;
    int nbw = s->bw, intra = s->intra;

    slice_reset(s, sc);
    for (int by = sc->sb; by < sc->se; by++) {
        int top = by == sc->sb;
        uint32_t cbp_y = 0, cbp_u = 0, cbp_v = 0, cbp_y_p = 0, cbp_u_p = 0, cbp_v_p = 0;
        uint8_t lru[4] = { 2, 3, 1, 0 };
        for (int bx = 0; bx < nbw; bx++) {
            int left = bx == 0, q0, q1;
            Decision dec;
            if (intra) {
                int pq = quant_pred(sc, nbw, bx * 2, top);
                write_quant_delta(bw, s->q - pq);
                sc->quants[sc->qpos + bx * 2] = s->q;
                cbp_y = encode_luma_intra(s, sc, bw, bx, by, top, left, cbp_y);
                cbp_v = encode_chroma_intra(s, sc, bw, 2, bx, by, top, left, cbp_v);
                cbp_u = encode_chroma_intra(s, sc, bw, 1, bx, by, top, left, cbp_u);
                sc->stats[0]++;
                continue;
            }
            q0 = quant_pred(sc, nbw, bx * 2, top);
            sc->quants[sc->qpos + bx * 2] = q0;
            q1 = quant_pred(sc, nbw, bx * 2 + 1, top);
            sc->quants[sc->qpos + bx * 2 + 1] = q1;
            decide_inter(s, sc, bx, by, top, left, lru, cbp_y, cbp_v, cbp_u,
                         cbp_y_p, cbp_v_p, cbp_u_p, q1, &dec);
            write_btype(bw, lru, dec.btype);
            sc->stats[dec.btype]++;
            switch (dec.btype) {
            case 0:
                write_quant_delta(bw, s->q - q0);
                sc->quants[sc->qpos + bx * 2] = s->q;
                cbp_y = encode_luma_intra(s, sc, bw, bx, by, top, left, cbp_y);
                cbp_v = encode_chroma_intra(s, sc, bw, 2, bx, by, top, left, cbp_v);
                cbp_u = encode_chroma_intra(s, sc, bw, 1, bx, by, top, left, cbp_u);
                predict_intra_mvs(sc, bx, top, left);
                break;
            case 1: {
                Mv z[4] = { 0 };
                write_prediction(s, bx, by, z);
                set4(sc, bx, z);
                break;
            }
            default:
                if (dec.four) {
                    Mv raw[4];
                    raw4(sc, bx, top, left, dec.mvs, raw);
                    write_mv4(bw, raw);
                    set4(sc, bx, dec.mvs);
                } else {
                    Mv all[4] = { dec.mvs[0], dec.mvs[0], dec.mvs[0], dec.mvs[0] };
                    write_mv1(bw, mv_sub(dec.mvs[0], pred1(sc, bx, top, left)));
                    set4(sc, bx, all);
                }
                if (dec.btype == 2) {
                    write_prediction(s, bx, by, dec.mvs);
                } else {
                    write_quant_delta(bw, s->q - q1);
                    sc->quants[sc->qpos + bx * 2 + 1] = s->q;
                    encode_residual(s, bw, bx, by, cbp_y_p, cbp_v_p, cbp_u_p, dec.res,
                                    &cbp_y_p, &cbp_v_p, &cbp_u_p);
                }
                break;
            }
            if (dec.btype)
                measure_dcs(s, sc, bx, by);
            for (int i = 0; i < 4; i++)
                s->prev_mvs_next[((size_t)by * nbw + bx) * 4 + i] = mvc(sc, bx, i);
        }
        FFSWAP(int, sc->qpos, sc->pqpos);
        FFSWAP(int, sc->mcur, sc->mprev);
        FFSWAP(int *, sc->ydc_prev, sc->ydc_new);
        FFSWAP(int *, sc->udc_prev, sc->udc_new);
        FFSWAP(int *, sc->vdc_prev, sc->vdc_new);
    }
    return bw->err;
}

/* ---------------------------------------------------------------- frames */

static void write_frame_header(Bink2EncContext *s, BW *bw, int intra)
{
    bw_write(bw, intra ? FRAME_FLAGS_INTRA : FRAME_FLAGS_INTER, 32);
    for (int i = 1; i < s->nb_slices; i++)
        bw_write(bw, 0, 32);
    if (intra) {
        int nflags = ((s->h + 15) >> 4) * 2 - 1;
        bw_bit(bw, 0);
        for (int i = 0; i < nflags; i++)
            bw_bit(bw, s->row_flag);
    }
}

/* Each slice padded to 4 bytes; the offsets of slices 1.. go in the
 * zero words that follow the frame flags. */
static int assemble(AVCodecContext *avctx, AVPacket *pkt)
{
    Bink2EncContext *s = avctx->priv_data;
    int64_t size = 0, pos = 0;
    uint8_t *out;
    int ret;

    for (int i = 0; i < s->nb_slices; i++) {
        if (s->slices[i].bw.err)
            return s->slices[i].bw.err;
        size += FFALIGN((s->slices[i].bw.nbits + 7) >> 3, 4);
    }
    if (size > INT_MAX)
        return AVERROR(ERANGE);
    if ((ret = ff_get_encode_buffer(avctx, pkt, size, 0)) < 0)
        return ret;
    out = pkt->data;
    memset(out, 0, size);
    for (int i = 0; i < s->nb_slices; i++) {
        size_t n = (s->slices[i].bw.nbits + 7) >> 3;
        if (i)
            AV_WL32(out + 4 * i, pos);
        if (n)
            memcpy(out + pos, s->slices[i].bw.buf, n);
        pos += FFALIGN(n, 4);
    }
    return 0;
}

static int encode_picture(AVCodecContext *avctx, AVPacket *pkt, int intra)
{
    Bink2EncContext *s = avctx->priv_data;
    int ret;

    for (int i = 0; i < s->nb_slices; i++)
        bw_reset(&s->slices[i].bw);
    write_frame_header(s, &s->slices[0].bw, intra);
    s->intra = intra;
    if (intra)  /* intra macroblocks leave no vectors for the next frame */
        memset(s->prev_mvs_next, 0, (size_t)s->bw * s->bh * 4 * sizeof(Mv));
    avctx->execute2(avctx, encode_slice, NULL, NULL, s->nb_slices);
    memset(s->stats, 0, sizeof(s->stats));
    for (int i = 0; i < s->nb_slices; i++)
        for (int k = 0; k < 4; k++)
            s->stats[k] += s->slices[i].stats[k];
    FFSWAP(Mv *, s->prev_mvs, s->prev_mvs_next);
    if ((ret = assemble(avctx, pkt)) < 0)
        return ret;
    frame_copy(&s->ref, &s->recon);
    s->have_ref = 1;
    return 0;
}

/* Input identical to the previous one: every macroblock skipped. */
static int encode_skip(AVCodecContext *avctx, AVPacket *pkt)
{
    Bink2EncContext *s = avctx->priv_data;

    for (int i = 0; i < s->nb_slices; i++) {
        SliceCtx *sc = &s->slices[i];
        bw_reset(&sc->bw);
        if (!i)
            write_frame_header(s, &sc->bw, 0);
        for (int by = sc->sb; by < sc->se; by++) {
            uint8_t lru[4] = { 2, 3, 1, 0 };
            for (int bx = 0; bx < s->bw; bx++)
                write_btype(&sc->bw, lru, 1);
        }
    }
    memset(s->prev_mvs, 0, (size_t)s->bw * s->bh * 4 * sizeof(Mv));
    return assemble(avctx, pkt);
}

static int bink2_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                              const AVFrame *frame, int *got_packet)
{
    Bink2EncContext *s = avctx->priv_data;
    int gop = avctx->gop_size > 0 ? avctx->gop_size : 1;
    int intra = s->frame_num % gop == 0, ret;
    int cw = (s->w + 1) / 2, ch = (s->h + 1) / 2;
    size_t ysz = (size_t)s->w * s->h, csz = (size_t)cw * ch;
    uint8_t *packed = s->last_input + ysz + 2 * csz, *p = packed;
    int same;

    for (int pl = 0; pl < 3; pl++) {
        int pw = pl ? cw : s->w, ph = pl ? ch : s->h;
        for (int y = 0; y < ph; y++, p += pw)
            memcpy(p, frame->data[pl] + (ptrdiff_t)y * frame->linesize[pl], pw);
    }
    same = s->have_last && !memcmp(packed, s->last_input, ysz + 2 * csz);
    memcpy(s->last_input, packed, ysz + 2 * csz);
    s->have_last = 1;

    if (!intra && same && s->have_ref) {
        ret = encode_skip(avctx, pkt);
    } else {
        intra |= !s->have_ref;
        for (int pl = 0; pl < 3; pl++)
            plane_fill(&s->src.p[pl], frame->data[pl], frame->linesize[pl]);
        s->q = av_clip(s->cur_q, 0, 36);
        s->lambda = s->lambda_opt > 0 ? s->lambda_opt : 0.25 * pow(2.0, s->q / 2.0);
        ret = encode_picture(avctx, pkt, intra);
        if (ret >= 0 && !intra) {
            int total = s->stats[0] + s->stats[1] + s->stats[2] + s->stats[3];
            if (s->stats[0] * 100 > total * 65 && s->since_key >= 8) {
                s->scene_cuts++;
                s->since_key = 0;
                av_packet_unref(pkt);
                intra = 1;
                ret = encode_picture(avctx, pkt, 1);
            } else {
                s->since_key++;
            }
        } else if (ret >= 0) {
            s->since_key = 0;
        }
    }
    if (ret < 0)
        return ret;

    if (s->rate > 0 && !intra) {
        double fps = av_q2d(avctx->framerate.num > 0 ? avctx->framerate : av_inv_q(avctx->time_base));
        double budget = s->rate * 1024.0 / fps, ratio;
        int step;
        s->avg = s->avg == 0.0 ? (double)pkt->size : 0.8 * s->avg + 0.2 * pkt->size;
        ratio = s->avg / budget;
        step = ratio > 1.6 ? 2 : ratio > 1.08 ? 1 : ratio < 0.5 ? -2 : ratio < 0.92 ? -1 : 0;
        s->cur_q = av_clip(s->cur_q + step, s->qmin, s->qmax);
    }
    if (intra)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->frame_num++;
    *got_packet = 1;
    return 0;
}

static av_cold int bink2_encode_init(AVCodecContext *avctx)
{
    Bink2EncContext *s = avctx->priv_data;
    static const int slice_counts[4] = { 2, 3, 4, 8 };
    int nsl = slice_counts[s->container_flags & 3], height_a, start = 0, end, prev = 0, ret;
    int bounds[MAX_SLICES];
    size_t frame_bytes;

    if (avctx->width > 0xFFFF || avctx->height > 0xFFFF) {
        av_log(avctx, AV_LOG_ERROR, "Dimensions too large\n");
        return AVERROR(EINVAL);
    }
    s->w  = avctx->width;
    s->h  = avctx->height;
    s->bw = (s->w + 31) >> 5;
    s->bh = (s->h + 31) >> 5;
    height_a = (s->h + 31) & ~31;
    end = height_a + 32 * nsl - 1;
    for (int i = 0; i < nsl - 1; i++) {
        start += ((end - start) / (nsl - i)) & ~31;
        end -= 32;
        bounds[i] = start >> 5;
    }
    bounds[nsl - 1] = height_a >> 5;
    s->nb_slices = 0;
    for (int i = 0; i < nsl; i++) {
        if (bounds[i] > prev) {
            s->slices[s->nb_slices].sb = prev;
            s->slices[s->nb_slices].se = bounds[i];
            s->nb_slices++;
        }
        prev = bounds[i];
    }

    if (s->rate <= 0 && avctx->bit_rate > 0)
        s->rate = avctx->bit_rate / 8.0 / 1024.0;
    if (avctx->flags & AV_CODEC_FLAG_QSCALE)
        s->q_opt = av_clip(avctx->global_quality / FF_QP2LAMBDA, 0, 36);
    s->cur_q = s->q_opt;
    init_forward_dct(s->fdct);

    if ((ret = frame_alloc(&s->src, s->w, s->h)) < 0 ||
        (ret = frame_alloc(&s->recon, s->w, s->h)) < 0 ||
        (ret = frame_alloc(&s->ref, s->w, s->h)) < 0)
        return ret;
    s->prev_mvs      = av_calloc((size_t)s->bw * s->bh * 4, sizeof(Mv));
    s->prev_mvs_next = av_calloc((size_t)s->bw * s->bh * 4, sizeof(Mv));
    frame_bytes = (size_t)s->w * s->h + 2 * (size_t)((s->w + 1) / 2) * ((s->h + 1) / 2);
    s->last_input = av_malloc(frame_bytes * 2);
    if (!s->prev_mvs || !s->prev_mvs_next || !s->last_input)
        return AVERROR(ENOMEM);
    for (int i = 0; i < s->nb_slices; i++) {
        SliceCtx *sc = &s->slices[i];
        sc->quants = av_mallocz(s->bw * 4);
        sc->mvs = av_calloc(s->bw * 8, sizeof(Mv));
        for (int j = 0; j < 2; j++) {
            sc->ydc_bufs[j] = av_calloc(s->bw * 4 + 4, sizeof(int));
            sc->udc_bufs[j] = av_calloc(s->bw * 2 + 2, sizeof(int));
            sc->vdc_bufs[j] = av_calloc(s->bw * 2 + 2, sizeof(int));
            if (!sc->ydc_bufs[j] || !sc->udc_bufs[j] || !sc->vdc_bufs[j])
                return AVERROR(ENOMEM);
        }
        if (!sc->quants || !sc->mvs)
            return AVERROR(ENOMEM);
    }

    avctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = 4;
    AV_WL32(avctx->extradata, s->container_flags);
    return 0;
}

static av_cold int bink2_encode_close(AVCodecContext *avctx)
{
    Bink2EncContext *s = avctx->priv_data;

    frame_free(&s->src);
    frame_free(&s->recon);
    frame_free(&s->ref);
    av_freep(&s->prev_mvs);
    av_freep(&s->prev_mvs_next);
    av_freep(&s->last_input);
    for (int i = 0; i < MAX_SLICES; i++) {
        SliceCtx *sc = &s->slices[i];
        av_freep(&sc->bw.buf);
        av_freep(&sc->quants);
        av_freep(&sc->mvs);
        av_freep(&sc->tried);
        for (int j = 0; j < 2; j++) {
            av_freep(&sc->ydc_bufs[j]);
            av_freep(&sc->udc_bufs[j]);
            av_freep(&sc->vdc_bufs[j]);
        }
    }
    return 0;
}

#define OFFSET(x) offsetof(Bink2EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "quant", "quantiser index, 0 is finest (also set by -q:v)", OFFSET(q_opt), AV_OPT_TYPE_INT, { .i64 = 12 }, 0, 36, VE },
    { "rate", "target rate in KiB/s for inter frames (0 = constant quantiser; also set by -b:v)", OFFSET(rate), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, 1e9, VE },
    { "qmin_b2", "lowest quantiser for rate control", OFFSET(qmin), AV_OPT_TYPE_INT, { .i64 = 6 }, 0, 36, VE },
    { "qmax_b2", "highest quantiser for rate control", OFFSET(qmax), AV_OPT_TYPE_INT, { .i64 = 30 }, 0, 36, VE },
    { "lambda", "rate-distortion weight (0 = 0.25 * 2^(q/2))", OFFSET(lambda_opt), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, 1e9, VE },
    { "deadzone", "AC quantiser rounding offset", OFFSET(deadzone), AV_OPT_TYPE_DOUBLE, { .dbl = 0.42 }, 0, 1, VE },
    { "search", "motion search range in pixels", OFFSET(search), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 1024, VE },
    { "mv4", "allow one vector per 16x16 block", OFFSET(mv4), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "rowflag", "value of the intra row flags", OFFSET(row_flag), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "flags_b2", "container video flags; the low 2 bits pick 2, 3, 4 or 8 slices", OFFSET(container_flags), AV_OPT_TYPE_INT, { .i64 = 0x12 }, 0, INT_MAX, VE },
    { NULL },
};

static const AVClass bink2_encoder_class = {
    .class_name = "bink2video encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault bink2_defaults[] = {
    { "g", "125" },
    { "b", "0" },
    { NULL },
};

const FFCodec ff_bink2_encoder = {
    .p.name         = "bink2video",
    CODEC_LONG_NAME("Bink video 2"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_BINKVIDEO2,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_SLICE_THREADS |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(Bink2EncContext),
    .init           = bink2_encode_init,
    FF_CODEC_ENCODE_CB(bink2_encode_frame),
    .close          = bink2_encode_close,
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P),
    .color_ranges   = AVCOL_RANGE_JPEG,
    .p.priv_class   = &bink2_encoder_class,
    .defaults       = bink2_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
