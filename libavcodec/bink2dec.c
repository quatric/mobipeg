/*
 * Bink 2 video decoder
 * Copyright (C) 2026 quatric <quatricsoftware@gmail.com>
 *
 * Ported from the Bink 2 decoder of NihAV,
 * Copyright (C) Kostya Shishkov (https://nihav.org).
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
 * Bink 2 video decoder (KB2a and later). 32x32 macroblocks are intra,
 * skip, motion or motion plus residual. KB2h and later code 8x8 blocks
 * with an integer DCT; earlier files (and frames without flag 0x2000) use
 * the older float transform and bitstream. Slices decode independently.
 */

#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#define BITSTREAM_READER_LE
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "bink2data.h"
#include "bink2dsp.h"

/* The old transform is in float; keep its rounding that of the reference. */
#pragma STDC FP_CONTRACT OFF

#define MAX_SLICES 8
#define MV_ESC 42

static const uint8_t ac_runs[14] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 64, 0 };
static const int8_t mv_syms[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -7, -6, -5, -4, -3, -2, -1, MV_ESC };

static const uint8_t luma_scan_old[64] = {
     0,  2,  1,  8,  9, 17, 10, 16,
    24,  3, 18, 25, 32, 11, 33, 26,
     4, 40, 19, 12, 27, 41, 34,  5,
    20, 48,  6, 28, 15, 42, 23, 35,
    21, 13, 14,  7, 31, 43, 49, 36,
    22, 56, 39, 50, 30, 44, 29, 51,
    57, 47, 58, 59, 63, 61, 55, 38,
    52, 62, 45, 37, 60, 46, 54, 53,
};

static const uint8_t chroma_scan_old[64] = {
     0,  1,  8,  2,  9, 16, 10, 17,
     3, 24, 11, 18, 25, 13, 14,  4,
    15,  5,  6,  7, 12, 19, 20, 21,
    22, 23, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
};

static const uint16_t ac_val_codes1[13] = { 0x004, 0x01, 0x002, 0x00, 0x08, 0x18, 0xF8, 0x178, 0x138, 0x38, 0x1B8, 0x78, 0xB8 };
static const uint8_t  ac_val_bits1[13]  = { 3, 1, 2, 4, 5, 6, 8, 9, 9, 9, 9, 9, 9 };
static const uint16_t ac_val_codes2[13] = { 0x0A, 0x001, 0x004, 0x08, 0x06, 0x00, 0x02, 0x1A, 0x2A, 0x16A, 0x1EA, 0x6A, 0xEA };
static const uint8_t  ac_val_bits2[13]  = { 6, 1, 3, 4, 3, 4, 4, 5, 7, 9, 9, 9, 9 };
static const uint16_t ac_skip_codes1[14] = { 0x00, 0x001, 0x0D, 0x15, 0x45, 0x85, 0xA5, 0x165, 0x65, 0x1E5, 0xE5, 0x25, 0x03, 0x05 };
static const uint8_t  ac_skip_bits1[14]  = { 1, 3, 4, 5, 7, 8, 8, 9, 9, 9, 9, 8, 2, 8 };
static const uint16_t ac_skip_codes2[14] = { 0x00, 0x01, 0x003, 0x07, 0x1F, 0x1B, 0x0F, 0x2F, 0x5B, 0xDB, 0x1DB, 0x3B, 0x05, 0x0B };
static const uint8_t  ac_skip_bits2[14]  = { 1, 3, 4, 4, 5, 7, 6, 6, 8, 9, 9, 6, 3, 5 };
static const uint8_t  quant_codes[16] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x30, 0x50, 0x70, 0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0 };
static const uint8_t  quant_bits[16]  = { 1, 2, 3, 4, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8 };

static const float old_quants[16] = {
    1.0, 2.0, 2.5, 3.0, 3.5, 4.0, 6.0, 7.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 64.0, 128.0,
};
static const int old_dc_quants[16] = { 4, 4, 4, 4, 4, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64, 128 };
static const uint8_t dc_extra_bits[16] = { 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 6 };

static const float qmat_intra_luma_old[64] = {
    0.125,    0.208056, 0.22864901, 0.205778, 0.22499999, 0.235708, 0.33148301,  0.248309,
    0.190718, 0.288582, 0.29449099, 0.34658501, 0.38143599, 0.47678301, 0.600528, 0.44008601,
    0.16332, 0.31714499, 0.34142101, 0.42249799, 0.604285, 0.70576, 0.68942899, 0.42807001,
    0.235175, 0.38735899, 0.46090701, 0.50122303, 0.82311302, 0.73910397, 0.69206202, 0.39741901,
    0.30000001, 0.45078799, 0.65328097, 0.74962097, 0.85000002, 0.79551601, 0.69678998, 0.38625899,
    0.392847, 0.79009801, 0.73142397, 1.004719, 1.070509, 0.80251199, 0.64313799, 0.27096599,
    0.34501299, 0.56299502, 0.60987997, 0.636379, 0.69678998, 0.60061598, 0.43934, 0.19224399,
    0.210373, 0.26309499, 0.252336, 0.25142801, 0.265553, 0.24928901, 0.188511, 0.094199002,
};
static const float qmat_chroma_old[64] = {
    0.125, 0.17338, 0.217761, 0.383793, 0.6875, 0.54016501, 0.37207201, 0.18968099,
    0.17338, 0.28056601, 0.32721299, 0.74753499, 0.95358998, 0.74923098, 0.51607901, 0.26309499,
    0.217761, 0.32721299, 0.66387498, 1.056244, 0.89826202, 0.70576, 0.48613599, 0.24783,
    0.383793, 0.74753499, 1.056244, 0.95059502, 0.80841398, 0.635167, 0.437511, 0.223041,
    0.6875, 0.95358998, 0.89826202, 0.80841398, 0.6875, 0.54016501, 0.37207201, 0.18968099,
    0.54016501, 0.74923098, 0.70576, 0.635167, 0.54016501, 0.42440501, 0.292335, 0.149031,
    0.37207201, 0.51607901, 0.48613599, 0.437511, 0.37207201, 0.292335, 0.201364, 0.102655,
    0.18968099, 0.26309499, 0.24783, 0.223041, 0.18968099, 0.149031, 0.102655, 0.052333001,
};
static const float qmat_inter_luma_old[64] = {
    0.125, 0.17338, 0.16332, 0.146984, 0.128475, 0.106393, 0.077045999, 0.043109,
    0.17338, 0.240485, 0.226532, 0.20387299, 0.17820001, 0.147571, 0.109474, 0.062454,
    0.16332, 0.226532, 0.219321, 0.202722, 0.181465, 0.149711, 0.112943, 0.062583998,
    0.146984, 0.20387299, 0.202722, 0.201647, 0.183731, 0.15397599, 0.11711, 0.065334998,
    0.128475, 0.17820001, 0.181465, 0.183731, 0.17708801, 0.155499, 0.120267, 0.068016,
    0.106393, 0.147571, 0.149711, 0.15397599, 0.155499, 0.14575601, 0.116636, 0.068494998,
    0.077045999, 0.109474, 0.112943, 0.11711, 0.120267, 0.116636, 0.098646, 0.060141001,
    0.043109, 0.062454, 0.062583998, 0.065334998, 0.068016, 0.068494998, 0.060141001, 0.038853001,
};

/* ---------------------------------------------------------------- codebooks */

enum { CB_AC1, CB_AC2, CB_MV, CB_VAL1, CB_VAL2, CB_SKIP1, CB_SKIP2, CB_QUANT, CB_NB };
#define CB_BITS 9
static VLCElem cb_tables[CB_NB][1 << CB_BITS];

static av_cold void init_codebooks(void)
{
#define INIT_CB(idx, n, bits, codes) \
    ff_vlc_init_table_sparse(cb_tables[idx], 1 << CB_BITS, CB_BITS, n, \
                             bits, 1, 1, codes, sizeof(codes[0]), sizeof(codes[0]), \
                             NULL, 0, 0, VLC_INIT_LE)
    INIT_CB(CB_AC1,   14, ac_bits1,      ac_codes1);
    INIT_CB(CB_AC2,   14, ac_bits2,      ac_codes2);
    INIT_CB(CB_MV,    16, mv_bits,       mv_codes);
    INIT_CB(CB_VAL1,  13, ac_val_bits1,  ac_val_codes1);
    INIT_CB(CB_VAL2,  13, ac_val_bits2,  ac_val_codes2);
    INIT_CB(CB_SKIP1, 14, ac_skip_bits1, ac_skip_codes1);
    INIT_CB(CB_SKIP2, 14, ac_skip_bits2, ac_skip_codes2);
    INIT_CB(CB_QUANT, 16, quant_bits,    quant_codes);
}

static inline int read_cb(GetBitContext *gb, int cb)
{
    return get_vlc2(gb, cb_tables[cb], CB_BITS, 1);
}

static inline unsigned read_unary_lim(GetBitContext *gb, unsigned len, int term)
{
    unsigned n = 0;
    while (n < len && get_bits1(gb) != term)
        n++;
    return n;
}

static int read_code_zero(GetBitContext *gb)
{
    int pfx = read_unary_lim(gb, 12, 0);
    if (pfx < 4)
        return pfx;
    return (1 << (pfx - 3)) + get_bits(gb, pfx - 3) + 2;
}

static int read_code_nz(GetBitContext *gb)
{
    int pfx = read_unary_lim(gb, 12, 0) + 1, val = pfx;
    if (pfx >= 4)
        val = (1 << (pfx - 3)) + get_bits(gb, pfx - 3) + 2;
    return get_bits1(gb) ? -val : val;
}

/* ---------------------------------------------------------------- transforms */

static void idct1d_float(const float *src, int sstep, float *dst, int dstep)
{
    float t00 =  src[sstep * 2] + src[sstep * 6];
    float t01 = (src[sstep * 2] - src[sstep * 6]) * (float)M_SQRT2 - t00;
    float t02 =  src[0] + src[sstep * 4];
    float t03 =  src[0] - src[sstep * 4];
    float t04 =  src[sstep * 3] + src[sstep * 5];
    float t05 =  src[sstep * 3] - src[sstep * 5];
    float t06 =  src[sstep * 1] + src[sstep * 7];
    float t07 =  src[sstep * 1] - src[sstep * 7];
    float t08 = t02 + t00;
    float t09 = t02 - t00;
    float t10 = t03 + t01;
    float t11 = t03 - t01;
    float t12 = t06 + t04;
    float t13 = (t06 - t04) * (float)M_SQRT2;
    float t14 = (t07 - t05) * 1.847759f;
    float t15 = t05 * 2.613126f + t14 - t12;
    float t16 = t13 - t15;
    float t17 = t07 * 1.0823922f - t14 + t16;
    dst[0]         = t08 + t12;
    dst[dstep]     = t10 + t15;
    dst[2 * dstep] = t11 + t16;
    dst[3 * dstep] = t09 - t17;
    dst[4 * dstep] = t09 + t17;
    dst[5 * dstep] = t11 - t16;
    dst[6 * dstep] = t10 - t15;
    dst[7 * dstep] = t08 - t12;
}

static void idct_float(float c[64])
{
    float tmp[64];
    c[0] += 512.5f;
    for (int i = 0; i < 8; i++)
        idct1d_float(c + i, 8, tmp + i, 8);
    for (int i = 0; i < 8; i++)
        idct1d_float(tmp + 8 * i, 1, c + 8 * i, 1);
}

/* float to int, truncating and saturating as the reference does */
static inline int ftoi(float f)
{
    if (!(f == f))
        return 0;
    if (f >= 2147483520.0f)
        return INT_MAX;
    if (f <= -2147483648.0f)
        return INT_MIN;
    return (int)f;
}

typedef int   IBlocks[4][64];
typedef float FBlocks[4][64];

/* Four 8x8 blocks in a 16x16 square; add selects residual over replace. */
static void put_mb4(uint8_t *dst, ptrdiff_t stride, IBlocks blk, int add)
{
    for (int b = 0; b < 4; b++) {
        uint8_t *d = dst + (b >> 1) * 8 * stride + (b & 1) * 8;
        bink2_idct(blk[b]);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                d[y * stride + x] = av_clip_uint8((add ? d[y * stride + x] : 0) + blk[b][y * 8 + x]);
    }
}

static void put_mb4_old(uint8_t *dst, ptrdiff_t stride, FBlocks blk, int add)
{
    for (int b = 0; b < 4; b++) {
        uint8_t *d = dst + (b >> 1) * 8 * stride + (b & 1) * 8;
        idct_float(blk[b]);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = ftoi(blk[b][y * 8 + x]) - 512;
                d[y * stride + x] = av_clip_uint8((add ? d[y * stride + x] : 0) + v);
            }
    }
}

static int calc_dc(const uint8_t *src, ptrdiff_t stride)
{
#define AVG(a, b) (((a) + (b) + 1) >> 1)
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t *s = src + i;
        int a = AVG(AVG(s[0], s[stride]), AVG(s[2 * stride], s[3 * stride]));
        int b = AVG(AVG(s[4 * stride], s[5 * stride]), AVG(s[6 * stride], s[7 * stride]));
        sum += AVG(a, b);
    }
    return sum;
#undef AVG
}

/* ---------------------------------------------------------------- motion */

typedef struct MV {
    int16_t x, y;
} MV;

static inline int mid_pred3(int a, int b, int c)
{
    if (a < b) {
        if (b < c) return b;
        return a < c ? c : a;
    }
    if (b < c)
        return a < c ? a : c;
    return b;
}

static inline MV mv_pred(MV a, MV b, MV c)
{
    return (MV){ mid_pred3(a.x, b.x, c.x), mid_pred3(a.y, b.y, c.y) };
}

static inline MV mv_add(MV a, MV b)
{
    return (MV){ (int16_t)(a.x + b.x), (int16_t)(a.y + b.y) };
}

typedef struct Plane {
    uint8_t *data;
    ptrdiff_t stride;
    int align_w, align_h;   ///< bounds a motion vector may read from
} Plane;

static inline int luma_filter(const uint8_t *s, ptrdiff_t step)
{
    int t0 = s[0] + s[step];
    int t1 = s[-step] + s[2 * step];
    int t2 = s[-2 * step] + s[3 * step];
    return (((t0 * 19) >> 1) - t1 * 2 + (t2 >> 1) + 8) >> 4;
}

static inline int luma_filter16(const int16_t *s, int step)
{
    int t0 = s[0] + s[step];
    int t1 = s[-step] + s[2 * step];
    int t2 = s[-2 * step] + s[3 * step];
    return (((t0 * 19) >> 1) - t1 * 2 + (t2 >> 1) + 8) >> 4;
}

static int mc_luma(uint8_t *dst, ptrdiff_t stride, const Plane *ref, int xoff, int yoff, MV mv)
{
    int sx = xoff + (mv.x >> 1), sy = yoff + (mv.y >> 1);
    int mode = (mv.x & 1) + (mv.y & 1) * 2;
    int dx = mv.x & 1 ? 2 : 0, ax = mv.x & 1 ? 5 : 0;
    int dy = mv.y & 1 ? 2 : 0, ay = mv.y & 1 ? 5 : 0;
    ptrdiff_t ps = ref->stride;
    const uint8_t *src;

    if (sx - dx < 0 || sx - dx + ax + 16 > ref->align_w ||
        sy - dy < 0 || sy - dy + ay + 16 > ref->align_h)
        return AVERROR_INVALIDDATA;
    src = ref->data + sy * ps + sx;
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++)
            memcpy(dst + y * stride, src + y * ps, 16);
        break;
    case 1:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                dst[y * stride + x] = av_clip_uint8(luma_filter(src + y * ps + x, 1));
        break;
    case 2:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                dst[y * stride + x] = av_clip_uint8(luma_filter(src + y * ps + x, ps));
        break;
    default: {
        int16_t tmp[21 * 16];
        for (int row = 0; row < 21; row++)
            for (int x = 0; x < 16; x++)
                tmp[row * 16 + x] = luma_filter(src + (row - 2) * ps + x, 1);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                dst[y * stride + x] = av_clip_uint8(luma_filter16(tmp + (y + 2) * 16 + x, 16));
        break;
    }
    }
    return 0;
}

static int mc_chroma(uint8_t *dst, ptrdiff_t stride, const Plane *ref, int xoff, int yoff, MV mv)
{
    int sx = xoff + (mv.x >> 2), sy = yoff + (mv.y >> 2);
    int mx = mv.x & 3, my = mv.y & 3;
    ptrdiff_t ps = ref->stride;
    const uint8_t *src;

    if (sx < 0 || sx + !!mx + 8 > ref->align_w ||
        sy < 0 || sy + !!my + 8 > ref->align_h)
        return AVERROR_INVALIDDATA;
    src = ref->data + sy * ps + sx;
    if (!mx && !my) {
        for (int y = 0; y < 8; y++)
            memcpy(dst + y * stride, src + y * ps, 8);
    } else if (!my) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                dst[y * stride + x] = bink2_chroma_interp(src[y * ps + x], src[y * ps + x + 1], mx, 2);
    } else if (!mx) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                dst[y * stride + x] = bink2_chroma_interp(src[y * ps + x], src[(y + 1) * ps + x], my, 2);
    } else {
        uint16_t tmp[9 * 8];
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 8; x++)
                tmp[y * 8 + x] = bink2_chroma_interp(src[y * ps + x], src[y * ps + x + 1], mx, 0);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                dst[y * stride + x] = bink2_chroma_interp(tmp[y * 8 + x], tmp[(y + 1) * 8 + x], my, 4);
    }
    return 0;
}

/* ---------------------------------------------------------------- DC prediction */

static inline int dc_pred2(int a, int b)
{
    return FFMIN(FFMAX(a, b), FFMAX(FFMIN(a, b), a * 2 - b));
}

static inline int dc_pred(int a, int b, int c)
{
    return FFMIN(FFMAX(FFMAX(a, b), c), FFMAX(FFMIN(FFMIN(a, b), c), b - a + c));
}

typedef struct DCInfo {
    int dcs[16];            ///< final DCs of the current macroblock
    int dc_buf[16];         ///< decoded differences
    int *prev_dcs, *new_dcs;
    int *bufs[2];
} DCInfo;

static int decode_dcs(GetBitContext *gb, int *dcs, int n, int q)
{
    int quant;
    if (!get_bits1(gb)) {
        memset(dcs, 0, n * sizeof(*dcs));
        return 0;
    }
    quant = quant_dc[FFMAX(q, 8)];
    for (int i = 0; i < n; i++) {
        dcs[i] = read_code_zero(gb);
        if (dcs[i]) {
            dcs[i] = (dcs[i] * quant + 0x1FF) >> 10;
            if (get_bits1(gb))
                dcs[i] = -dcs[i];
        }
    }
    return 1;
}

#define CL(v) av_clip(v, min_dc, max_dc)

static void luma_predict(DCInfo *d, int bx, int is_top, int is_left, int min_dc, int max_dc)
{
    const int *p = d->prev_dcs + bx * 4, *dc = d->dc_buf;
    int *l = d->dcs, a[16];

    if (is_left) {
        if (is_top) {
            a[0] = CL(dc[0] + (1 << 10));
            a[1] = CL(dc[1] + a[0]);
            a[4] = 0;
        } else {
            a[0] = CL(dc[0] + dc_pred2(p[4], p[5]));
            a[1] = CL(dc[1] + dc_pred(p[4], p[5], a[0]));
        }
        a[2] = CL(dc[2] + dc_pred2(a[0], a[1]));
        a[3] = CL(dc[3] + dc_pred(a[0], a[1], a[2]));
        if (is_top) {
            a[4] = CL(dc[4] + dc_pred2(a[1], a[3]));
            a[5] = CL(dc[5] + a[4]);
        } else {
            a[4] = CL(dc[4] + dc_pred(p[5], p[6], a[1]));
            a[5] = CL(dc[5] + dc_pred(p[6], p[7], a[4]));
        }
        a[6] = CL(dc[6] + dc_pred(a[1], a[3], a[4]));
        a[7] = CL(dc[7] + dc_pred(a[4], a[5], a[6]));
        a[8] = CL(dc[8] + dc_pred2(a[2], a[3]));
        a[9] = CL(dc[9] + dc_pred(a[2], a[3], a[8]));
        a[10] = CL(dc[10] + dc_pred2(a[8], a[9]));
    } else {
        if (is_top) {
            a[0] = CL(dc[0] + dc_pred2(l[5], l[7]));
            a[1] = CL(dc[1] + a[0]);
        } else {
            a[0] = CL(dc[0] + dc_pred(p[3], p[4], l[5]));
            a[1] = CL(dc[1] + dc_pred(p[4], p[5], a[0]));
        }
        a[2] = CL(dc[2] + dc_pred(l[5], l[7], a[0]));
        a[3] = CL(dc[3] + dc_pred(a[0], a[1], a[2]));
        if (is_top) {
            a[4] = CL(dc[4] + dc_pred2(a[1], a[3]));
            a[5] = CL(dc[5] + a[4]);
        } else {
            a[4] = CL(dc[4] + dc_pred(p[5], p[6], a[1]));
            a[5] = CL(dc[5] + dc_pred(p[6], p[7], a[4]));
        }
        a[6] = CL(dc[6] + dc_pred(a[1], a[3], a[4]));
        a[7] = CL(dc[7] + dc_pred(a[4], a[5], a[6]));
        a[8] = CL(dc[8] + dc_pred(l[7], l[13], a[2]));
        a[9] = CL(dc[9] + dc_pred(a[2], a[3], a[8]));
        a[10] = CL(dc[10] + dc_pred(l[13], l[15], a[8]));
    }
    a[11] = CL(dc[11] + dc_pred(a[8], a[9], a[10]));
    a[12] = CL(dc[12] + dc_pred(a[3], a[6], a[9]));
    a[13] = CL(dc[13] + dc_pred(a[6], a[7], a[12]));
    a[14] = CL(dc[14] + dc_pred(a[9], a[11], a[12]));
    a[15] = CL(dc[15] + dc_pred(a[12], a[13], a[14]));
    memcpy(d->dcs, a, sizeof(a));
    d->new_dcs[bx * 4 + 4] = a[10];
    d->new_dcs[bx * 4 + 5] = a[11];
    d->new_dcs[bx * 4 + 6] = a[14];
    d->new_dcs[bx * 4 + 7] = a[15];
}

static void luma_predict_inter(DCInfo *d, int min_dc, int max_dc)
{
    const int *dc = d->dc_buf;
    int *a = d->dcs;
    a[0] = CL(dc[0]);
    a[1] = CL(dc[1] + a[0]);
    a[2] = CL(dc[2] + dc_pred2(a[0], a[1]));
    a[3] = CL(dc[3] + dc_pred(a[0], a[1], a[2]));
    a[4] = CL(dc[4] + dc_pred2(a[1], a[3]));
    a[5] = CL(dc[5] + a[4]);
    a[6] = CL(dc[6] + dc_pred(a[1], a[3], a[4]));
    a[7] = CL(dc[7] + dc_pred(a[4], a[5], a[6]));
    a[8] = CL(dc[8] + dc_pred2(a[2], a[3]));
    a[9] = CL(dc[9] + dc_pred(a[2], a[3], a[8]));
    a[10] = CL(dc[10] + dc_pred2(a[8], a[9]));
    a[11] = CL(dc[11] + dc_pred(a[8], a[9], a[10]));
    a[12] = CL(dc[12] + dc_pred(a[3], a[6], a[9]));
    a[13] = CL(dc[13] + dc_pred(a[6], a[7], a[12]));
    a[14] = CL(dc[14] + dc_pred(a[9], a[11], a[12]));
    a[15] = CL(dc[15] + dc_pred(a[12], a[13], a[14]));
}

static void chroma_predict(DCInfo *d, int bx, int is_top, int is_left, int min_dc, int max_dc)
{
    const int *p = d->prev_dcs + bx * 2, *dc = d->dc_buf;
    int *l = d->dcs, a[4];

    if (is_left) {
        if (is_top) {
            a[0] = CL(dc[0] + (1 << 10));
            a[1] = CL(dc[1] + a[0]);
        } else {
            a[0] = CL(dc[0] + dc_pred2(p[2], p[3]));
            a[1] = CL(dc[1] + dc_pred(p[2], p[3], a[0]));
        }
        a[2] = CL(dc[2] + dc_pred2(a[0], a[1]));
    } else {
        if (is_top) {
            a[0] = CL(dc[0] + dc_pred2(l[1], l[3]));
            a[1] = CL(dc[1] + a[0]);
        } else {
            a[0] = CL(dc[0] + dc_pred(p[1], p[2], l[1]));
            a[1] = CL(dc[1] + dc_pred(p[2], p[3], a[0]));
        }
        a[2] = CL(dc[2] + dc_pred(l[1], l[3], a[0]));
    }
    a[3] = CL(dc[3] + dc_pred(a[0], a[1], a[2]));
    memcpy(d->dcs, a, sizeof(a));
    d->new_dcs[bx * 2 + 2] = a[2];
    d->new_dcs[bx * 2 + 3] = a[3];
}

static void chroma_predict_inter(DCInfo *d, int min_dc, int max_dc)
{
    const int *dc = d->dc_buf;
    int *a = d->dcs;
    a[0] = CL(dc[0]);
    a[1] = CL(dc[1] + a[0]);
    a[2] = CL(dc[2] + dc_pred2(a[0], a[1]));
    a[3] = CL(dc[3] + dc_pred(a[0], a[1], a[2]));
}

#undef CL

/* ---------------------------------------------------------------- block data */

static int decode_cbp_luma(GetBitContext *gb, uint32_t prev_cbp, uint32_t edge_state)
{
    int cnt = av_popcount(prev_cbp & 0xFFFF);
    int min_count = cnt < 8 ? cnt : 16 - cnt;
    uint32_t mask = cnt < 8 ? 0 : 0xFFFF, cbp = 0;

    if (get_bits1(gb)) {
        cbp = 0;
    } else if (min_count >= 4) {
        cbp = get_bits(gb, 16);
    } else {
        for (int i = 0; i < 4; i++)
            if (!get_bits1(gb))
                cbp |= get_bits(gb, 4) << (i * 4);
    }
    cbp ^= mask;
    if (!(edge_state & 0x40000) || cbp)
        if (get_bits1(gb))
            cbp |= cbp << 16;
    return cbp;
}

static uint32_t decode_cbp_chroma(GetBitContext *gb, uint32_t last_cbp)
{
    uint32_t ncbp;
    if (get_bits1(gb))
        return (last_cbp & 0xF0000) | chroma_cbps[last_cbp & 0xF];
    ncbp = get_bits(gb, 4);
    return get_bits1(gb) ? ncbp | ncbp << 16 : ncbp;
}

static int decode_acs_4blocks(GetBitContext *gb, IBlocks dst, const int32_t qmat_all[4][64],
                              int q, uint32_t cbp, uint8_t ncoded[4])
{
    int cb = cbp >> 16 ? CB_AC1 : CB_AC2;
    const int32_t *qmat = qmat_all[q & 3];
    int shift = q >> 2;

    for (int blk = 0; blk < 4; blk++) {
        int esc_len = 0, idx = 1;
        ncoded[blk] = 0;
        if (!(cbp >> blk & 1))
            continue;
        while (idx < 64) {
            int level, pos;
            if (esc_len >= 1) {
                esc_len--;
            } else {
                int sym = read_cb(gb, cb), skip;
                if (sym < 0)
                    return AVERROR_INVALIDDATA;
                skip = sym == 11 ? get_bits(gb, 6) : ac_runs[sym];
                idx += skip;
                if (idx > 63)
                    break;
                esc_len = sym == 13 ? 6 : 0;
            }
            level = read_code_nz(gb);
            pos = zigzag[idx];
            dst[blk][pos] = (int)(((unsigned)(level * qmat[pos]) << shift) + 0x40) >> 7;
            ncoded[blk]++;
            idx++;
        }
    }
    return 0;
}

static int get_new_quant(GetBitContext *gb, int prev_q)
{
    int code = read_unary_lim(gb, 4, 1), q;
    if (!code)
        return prev_q;
    if (code == 3)
        code += get_bits1(gb);
    else if (code == 4)
        code = get_bits(gb, 5) + 5;
    q = get_bits1(gb) ? prev_q - code : prev_q + code;
    if (q < 0 || q >= FF_ARRAY_ELEMS(quant_dc))
        return AVERROR_INVALIDDATA;
    return q;
}

static int decode_luma_intra(GetBitContext *gb, uint32_t prev_cbp, int q, IBlocks dst[4],
                             uint32_t edge_state, DCInfo *dc, int bx, uint32_t *ocbp,
                             uint16_t info[16])
{
    uint32_t cbp = decode_cbp_luma(gb, prev_cbp, edge_state);
    uint8_t nc[4];
    int ret;
    decode_dcs(gb, dc->dc_buf, 16, q);
    luma_predict(dc, bx, !!(edge_state & 0x88), !!(edge_state & 0x20), 0, 0x7FF);
    for (int i = 0; i < 4; i++) {
        if ((ret = decode_acs_4blocks(gb, dst[i], quant_intra_luma, q, cbp >> (i * 4), nc)) < 0)
            return ret;
        for (int j = 0; j < 4; j++) {
            dst[i][j][0] = dc->dcs[i * 4 + j] * 8 + 32;
            info[i * 4 + j] = bink2_block_info(dc->dcs[i * 4 + j], nc[j], 1);
        }
    }
    *ocbp = cbp;
    return 0;
}

static int decode_chroma_intra(GetBitContext *gb, uint32_t prev_cbp, int q, IBlocks dst,
                               uint32_t edge_state, DCInfo *dc, int bx, uint32_t *ocbp,
                               uint16_t info[4])
{
    uint32_t cbp = decode_cbp_chroma(gb, prev_cbp);
    uint8_t nc[4];
    int ret;
    decode_dcs(gb, dc->dc_buf, 4, q);
    chroma_predict(dc, bx, !!(edge_state & 0x88), !!(edge_state & 0x20), 0, 0x7FF);
    if ((ret = decode_acs_4blocks(gb, dst, quant_intra_chroma, q, cbp, nc)) < 0)
        return ret;
    for (int i = 0; i < 4; i++) {
        dst[i][0] = dc->dcs[i] * 8 + 32;
        info[i] = bink2_block_info(dc->dcs[i], nc[i], 1);
    }
    *ocbp = cbp;
    return 0;
}

static int decode_luma_inter(GetBitContext *gb, uint32_t prev_cbp, int q, IBlocks dst[4],
                             uint32_t edge_state, DCInfo *dc, uint32_t *ocbp, uint16_t info[16])
{
    uint32_t cbp = decode_cbp_luma(gb, prev_cbp, edge_state);
    int lim = edge_state & 0x40000 ? 0x7FF : 0x3FF, ret;
    uint8_t nc[4];
    decode_dcs(gb, dc->dc_buf, 16, q);
    luma_predict_inter(dc, -lim, lim);
    for (int i = 0; i < 4; i++) {
        if ((ret = decode_acs_4blocks(gb, dst[i], quant_inter, q, cbp >> (i * 4), nc)) < 0)
            return ret;
        for (int j = 0; j < 4; j++) {
            dst[i][j][0] = dc->dcs[i * 4 + j] * 8 + 32;
            info[i * 4 + j] = bink2_block_info(dc->dcs[i * 4 + j], nc[j], 0);
        }
    }
    *ocbp = cbp;
    return 0;
}

static int decode_chroma_inter(GetBitContext *gb, uint32_t prev_cbp, int q, IBlocks dst,
                               uint32_t edge_state, DCInfo *dc, uint32_t *ocbp, uint16_t info[4])
{
    uint32_t cbp = decode_cbp_chroma(gb, prev_cbp);
    int lim = edge_state & 0x40000 ? 0x7FF : 0x3FF, ret;
    uint8_t nc[4];
    decode_dcs(gb, dc->dc_buf, 4, q);
    chroma_predict_inter(dc, -lim, lim);
    if ((ret = decode_acs_4blocks(gb, dst, quant_inter, q, cbp, nc)) < 0)
        return ret;
    for (int i = 0; i < 4; i++) {
        dst[i][0] = dc->dcs[i] * 8 + 32;
        info[i] = bink2_block_info(dc->dcs[i], nc[i], 0);
    }
    *ocbp = cbp;
    return 0;
}

/* ---------------------------------------------------------------- old bitstream */

static int decode_quant_old(GetBitContext *gb, int prev_quant)
{
    int diff = read_cb(gb, CB_QUANT), q;
    if (diff < 0)
        return AVERROR_INVALIDDATA;
    if (!diff)
        return prev_quant;
    q = get_bits1(gb) ? prev_quant - diff : prev_quant + diff;
    if (q < 0 || q >= 16)
        return AVERROR_INVALIDDATA;
    return q;
}

static uint32_t decode_cbp_luma_old(GetBitContext *gb, uint32_t prev_cbp)
{
    uint32_t low, nib_hi, high = 0;
    if (get_bits1(gb)) {
        if (get_bits1(gb))
            return prev_cbp;
        low = prev_cbp & 0xFFFF;
    } else {
        uint32_t nib1 = (prev_cbp >> 4) & 0xF, new_cbp;
        if (!get_bits1(gb))
            nib1 = get_bits(gb, 4);
        new_cbp = nib1;
        if (!get_bits1(gb))
            nib1 = get_bits(gb, 4);
        new_cbp |= nib1 << 4;
        if (!get_bits1(gb))
            nib1 = get_bits(gb, 4);
        new_cbp |= nib1 << 8;
        if (!get_bits1(gb))
            nib1 = get_bits(gb, 4);
        low = new_cbp | (nib1 << 12);
    }
    nib_hi = (prev_cbp >> 20) & 0xF;
    for (int i = 0; i < 4; i++) {
        uint32_t nib = (low >> (4 * i)) & 0xF;
        int cnt = av_popcount(nib);
        if (!cnt) {
            nib_hi = 0;
        } else if (cnt == 1 || !get_bits1(gb)) {
            nib_hi = 0;
            for (int bit = 0; bit < 4; bit++)
                if ((nib >> bit & 1) && get_bits1(gb))
                    nib_hi |= 1 << bit;
        }
        nib_hi &= nib;
        high = (high >> 4) | (nib_hi << 28);
    }
    return low | high;
}

static uint32_t decode_cbp_chroma_old(GetBitContext *gb, uint32_t prev_cbp)
{
    uint32_t low, high;
    if (!get_bits1(gb)) {
        low = get_bits(gb, 4);
    } else {
        if (get_bits1(gb))
            return prev_cbp;
        low = prev_cbp & 0xF;
    }
    if (!low)
        return low;
    if (av_popcount(low) != 1) {
        high = (prev_cbp >> 16) & low;
        if (get_bits1(gb))
            return (high << 16) | low;
    }
    high = 0;
    for (int bit = 0; bit < 4; bit++)
        if ((low >> bit & 1) && get_bits1(gb))
            high |= 1 << bit;
    return (high << 16) | low;
}

static void decode_dcs_old(GetBitContext *gb, int *dcs, int n, int q, int read_bias)
{
    int bits = get_bits(gb, 3), scale = old_dc_quants[q];
    if (bits == 7)
        bits += get_bits(gb, 2);
    if (!bits) {
        memset(dcs, 0, n * sizeof(*dcs));
    } else {
        for (int c = 0; c < n; c += 4) {
            for (int i = 0; i < 4; i++)
                dcs[c + i] = get_bits(gb, bits);
            for (int i = 0; i < 4; i++) {
                if (!dcs[c + i])
                    continue;
                dcs[c + i] *= scale;
                if (get_bits1(gb))
                    dcs[c + i] = -dcs[c + i];
            }
        }
    }
    if (read_bias) {
        int add_bits = dc_extra_bits[q] + bits;
        if (add_bits < 10) {
            int pfx = get_bits(gb, 10 - add_bits), base_dc = 0;
            if (pfx > 0)
                base_dc = get_bits1(gb) ? -(pfx << bits) : pfx << bits;
            dcs[0] += base_dc * scale;
        }
    }
}

static int decode_acs_4blocks_old(GetBitContext *gb, FBlocks dst, const float *qmat,
                                  int q, uint32_t cbp, const uint8_t *scan)
{
    float quant = old_quants[q];
    for (int blk = 0; blk < 4; blk++) {
        int alt = cbp >> (16 + blk) & 1;
        int val_cb = alt ? CB_VAL2 : CB_VAL1, skip_cb = alt ? CB_SKIP2 : CB_SKIP1;
        int esc_len = 0, idx = 1;
        if (!(cbp >> blk & 1))
            continue;
        while (idx < 64) {
            int val = read_cb(gb, val_cb);
            if (val < 0)
                return AVERROR_INVALIDDATA;
            if (val) {
                int level = val >= 4 ? (1 << (val - 3)) + get_bits(gb, val - 3) + 2 : val;
                int pos = scan[idx];
                if (get_bits1(gb))
                    level = -level;
                dst[blk][pos] = (float)level * qmat[(pos & 7) * 8 + (pos >> 3)] * quant;
            }
            idx++;
            if (idx >= 64)
                break;
            if (esc_len >= 1) {
                esc_len--;
            } else {
                int sym = read_cb(gb, skip_cb);
                if (sym < 0)
                    return AVERROR_INVALIDDATA;
                idx += sym == 11 ? get_bits(gb, 6) : ac_runs[sym];
                if (idx > 63)
                    break;
                esc_len = sym == 13 ? 6 : 0;
            }
        }
    }
    return 0;
}

static int decode_luma_old(GetBitContext *gb, uint32_t prev_cbp, FBlocks dst[4], uint32_t edge_state,
                           DCInfo *dc, int bx, int *old_q, int intra, uint32_t *ocbp)
{
    uint32_t cbp = decode_cbp_luma_old(gb, prev_cbp);
    int q = decode_quant_old(gb, *old_q), ret;
    if (q < 0)
        return q;
    *old_q = q;
    if (intra) {
        int is_top = !!(edge_state & 0x88), is_left = !!(edge_state & 0x20);
        decode_dcs_old(gb, dc->dc_buf, 16, q, is_top && is_left);
        luma_predict(dc, bx, is_top, is_left, 0, 0x7FF);
    } else {
        decode_dcs_old(gb, dc->dc_buf, 16, q, 0);
        luma_predict_inter(dc, -1023, 1023);
    }
    for (int i = 0; i < 4; i++) {
        if ((ret = decode_acs_4blocks_old(gb, dst[i], intra ? qmat_intra_luma_old : qmat_inter_luma_old,
                                          q, cbp >> (i * 4), luma_scan_old)) < 0)
            return ret;
        for (int j = 0; j < 4; j++)
            dst[i][j][0] = (float)dc->dcs[i * 4 + j] * 0.125f;
    }
    *ocbp = cbp;
    return 0;
}

static int decode_chroma_old(GetBitContext *gb, uint32_t prev_cbp, FBlocks dst, uint32_t edge_state,
                             DCInfo *dc, int bx, int *old_q, int intra, uint32_t *ocbp)
{
    uint32_t cbp = decode_cbp_chroma_old(gb, prev_cbp);
    int q = decode_quant_old(gb, *old_q), ret;
    if (q < 0)
        return q;
    *old_q = q;
    if (intra) {
        int is_top = !!(edge_state & 0x88), is_left = !!(edge_state & 0x20);
        decode_dcs_old(gb, dc->dc_buf, 4, q, is_top && is_left);
        chroma_predict(dc, bx, is_top, is_left, 0, 0x7FF);
    } else {
        decode_dcs_old(gb, dc->dc_buf, 4, q, 0);
        chroma_predict_inter(dc, -1023, 1023);
    }
    if ((ret = decode_acs_4blocks_old(gb, dst, qmat_chroma_old, q, cbp, chroma_scan_old)) < 0)
        return ret;
    for (int i = 0; i < 4; i++)
        dst[i][0] = (float)dc->dcs[i] * 0.125f;
    *ocbp = cbp;
    return 0;
}

/* ---------------------------------------------------------------- slice state */

typedef struct SliceCtx {
    GetBitContext gb;
    int start_by, end_by;
    uint8_t *quants;        ///< 2 rows of bw * 2
    int qpos, pqpos;
    DCInfo y, u, v, a;
    MV *mvs;                ///< 2 rows of bw * 4
    int mv_off, prev_off;
} SliceCtx;

typedef struct Bink2Context {
    int version;            ///< revision letter, 'a' to 'n'
    int has_alpha;
    int num_slices;
    int slice_h[MAX_SLICES];
    int bw, bh;
    int w16, h16;           ///< picture size rounded up to 16, as the reference stores it
    uint8_t *planes[2][4];  ///< current and reference Y, U, V, A
    ptrdiff_t stride[4];
    int plane_h[4];
    int align_w[4], align_h[4];
    int cur, have_ref;
    uint8_t *row_flags, *col_flags;
    uint16_t *info[4];      ///< bink2_block_info() of every 8x8 block of each plane
    int info_w[4], info_h[4];
    MV *mv_grid;            ///< motion vector of every 16x16 luma block

    /* the frame being decoded */
    uint32_t frame_flags;
    int is_intra, do_alpha;
    SliceCtx slices[MAX_SLICES];
} Bink2Context;

static inline int pred_quant(const SliceCtx *sc, int pos, int is_top)
{
    int is_left = pos < 2;
    if (is_top)
        return is_left ? 16 : sc->quants[sc->qpos + pos - 2];
    if (is_left)
        return sc->quants[sc->pqpos + pos];
    return mid_pred3(sc->quants[sc->pqpos + pos - 2], sc->quants[sc->pqpos + pos],
                     sc->quants[sc->qpos + pos - 2]);
}

#define M(i)  mv[mv_pos + (i)]
#define R(i)  mv[ref_pos + (i)]

static int decode_mv_new(SliceCtx *sc, int bx, int is_top, int is_left)
{
    GetBitContext *gb = &sc->gb;
    MV *mv = sc->mvs;
    int mv_pos = sc->mv_off + bx * 4, ref_pos = sc->prev_off + bx * 4;
    int num_mv = get_bits1(gb) ? 1 : 4;

    for (int comp = 0; comp < 2; comp++)
        for (int i = 0; i < num_mv; i++) {
            int raw = read_cb(gb, CB_MV), val;
            if (raw < 0)
                return AVERROR_INVALIDDATA;
            raw = mv_syms[raw];
            if (raw != MV_ESC) {
                val = raw;
            } else {
                int nlen = read_unary_lim(gb, 12, 1) + 4;
                int uval = get_bits(gb, nlen) + (1 << nlen) - 1;
                val = uval & 1 ? -(uval >> 1) - 1 : uval >> 1;
            }
            if (!comp)
                M(i).x = val;
            else
                M(i).y = val;
        }

    if (num_mv > 1) {
        if (is_top) {
            if (!is_left) {
                M(0) = mv_add(M(0), mv_pred(M(-4), M(-3), M(-1)));
                M(2) = mv_add(M(2), mv_pred(M(-3), M(-1), M(0)));
                M(1) = mv_add(M(1), mv_pred(M(-3), M(0), M(2)));
                M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
            } else {
                M(1) = mv_add(M(1), M(0));
                M(2) = mv_add(M(2), M(0));
                M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
            }
        } else if (is_left) {
            M(0) = mv_add(M(0), mv_pred(R(0), R(2), R(3)));
            M(1) = mv_add(M(1), mv_pred(R(2), R(3), M(0)));
            M(2) = mv_add(M(2), mv_pred(R(2), M(0), M(1)));
            M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
        } else {
            M(0) = mv_add(M(0), mv_pred(R(2), R(-1), M(-3)));
            M(1) = mv_add(M(1), mv_pred(R(2), R(3), M(0)));
            M(2) = mv_add(M(2), mv_pred(M(-3), M(-1), M(0)));
            M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
        }
    } else {
        MV p = M(0);
        if (is_top) {
            if (!is_left)
                p = mv_add(p, mv_pred(M(-4), M(-3), M(-1)));
        } else if (!is_left) {
            p = mv_add(p, mv_pred(R(-1), R(2), M(-3)));
        } else {
            p = mv_add(p, mv_pred(R(0), R(2), R(3)));
        }
        M(0) = M(1) = M(2) = M(3) = p;
    }
    return 0;
}

static void decode_mv_old(SliceCtx *sc, int bx, int is_top, int is_left)
{
    GetBitContext *gb = &sc->gb;
    MV *mv = sc->mvs;
    int mv_pos = sc->mv_off + bx * 4, ref_pos = sc->prev_off + bx * 4;
    int base[2] = { 0, 0 };

    for (int comp = 0; comp < 2; comp++) {
        int bits = get_bits(gb, 3), c[4] = { 0 };
        if (bits == 7)
            bits += get_bits(gb, 2);
        if (bits) {
            for (int i = 0; i < 4; i++)
                c[i] = get_bits(gb, bits);
            for (int i = 0; i < 4; i++)
                if (c[i] && get_bits1(gb))
                    c[i] = -c[i];
        }
        if (is_top && is_left) {
            int val = get_bits(gb, 5);
            if (val && get_bits1(gb))
                val = -val;
            base[comp] = val;
        }
        for (int i = 0; i < 4; i++) {
            if (!comp)
                M(i).x = c[i];
            else
                M(i).y = c[i];
        }
    }
    if (!is_top) {
        if (is_left)
            M(0) = mv_add(M(0), mv_pred(R(0), R(2), R(3)));
        else
            M(0) = mv_add(M(0), mv_pred(R(-1), R(2), M(-3)));
        M(1) = mv_add(M(1), mv_pred(R(2), R(3), M(0)));
        M(2) = mv_add(M(2), mv_pred(R(2), M(0), M(1)));
        M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
    } else if (is_left) {
        MV b = { base[0] * 16, base[1] * 16 };
        for (int i = 0; i < 4; i++)
            M(i) = mv_add(M(i), b);
    } else {
        M(0) = mv_add(M(0), mv_pred(M(-4), M(-3), M(-1)));
        M(2) = mv_add(M(2), mv_pred(M(-3), M(-1), M(0)));
        M(1) = mv_add(M(1), mv_pred(M(-3), M(0), M(2)));
        M(3) = mv_add(M(3), mv_pred(M(0), M(1), M(2)));
    }
}

static void predict_mvs(SliceCtx *sc, int bx, int is_top, int is_left, int is_new)
{
    MV *mv = sc->mvs;
    int mv_pos = sc->mv_off + bx * 4, ref_pos = sc->prev_off + bx * 4;

    if (is_top) {
        if (is_left) {
            memset(&M(0), 0, 4 * sizeof(MV));
        } else {
            M(0) = mv_pred(M(-4), M(-3), M(-1));
            M(2) = mv_pred(M(-3), M(-1), M(0));
            M(1) = mv_pred(M(-1), M(0), M(2));
            M(3) = mv_pred(M(0), M(1), M(2));
        }
    } else if (is_left) {
        M(0) = mv_pred(R(0), R(2), R(3));
        M(1) = mv_pred(M(0), R(2), R(3));
        M(2) = mv_pred(R(2), M(1), M(0));
        M(3) = mv_pred(M(0), M(1), M(2));
    } else {
        M(0) = mv_pred(M(-3), R(-1), R(2));
        M(1) = mv_pred(M(0), R(2), R(3));
        if (is_new)
            M(2) = mv_pred(M(-3), M(-1), M(0));
        else
            M(2) = mv_pred(R(2), M(1), M(0));
        M(3) = mv_pred(M(0), M(1), M(2));
    }
}

#undef M
#undef R

/* ---------------------------------------------------------------- frames */

static int decode_flags(GetBitContext *gb, uint8_t *dst, int max, int nbits)
{
    int n = 0;
#define PUSH(b) do { if (n < max) dst[n] = (b); n++; } while (0)
    PUSH(0);
    if (!get_bits1(gb)) {
        for (int i = 0; i < nbits; i++)
            PUSH(get_bits1(gb));
    } else {
        int cur_bits = nbits, mode = 0, lastbit = 0;
        while (cur_bits > 0) {
            if (get_bits_left(gb) <= 0)
                return AVERROR_INVALIDDATA;
            if (!get_bits1(gb)) {
                int len;
                lastbit = mode == 3 ? !lastbit : get_bits1(gb);
                PUSH(lastbit);
                cur_bits--;
                len = FFMIN(cur_bits, 4);
                for (int i = 0; i < len; i++)
                    PUSH(get_bits1(gb));
                cur_bits -= len;
                mode = 2;
            } else {
                int bread = cur_bits < 4 ? 2 : cur_bits < 16 ? 4 : 5, run;
                lastbit = mode == 3 ? !lastbit : get_bits1(gb);
                run = FFMIN(mode == 3 ? bread + 1 : bread + 2, cur_bits);
                if (run != cur_bits) {
                    int add_run = get_bits(gb, bread);
                    run += add_run;
                    mode = add_run == (1 << bread) - 1 ? 1 : 3;
                }
                for (int i = 0; i < run; i++)
                    PUSH(lastbit);
                cur_bits -= run;
            }
        }
    }
#undef PUSH
    return 0;
}

static int mc_block(Bink2Context *c, int bx, int by, MV mv, int blk_no, int do_alpha)
{
    int r = c->cur ^ 1, ret;
    int xoff = bx * 32 + (blk_no & 1) * 16, yoff = by * 32 + (blk_no & 2) * 8;
    Plane ref;

    for (int p = 0; p < (do_alpha ? 4 : 3); p++) {
        int chroma = p == 1 || p == 2;
        int x = chroma ? xoff >> 1 : xoff, y = chroma ? yoff >> 1 : yoff;
        uint8_t *dst = c->planes[c->cur][p] + y * c->stride[p] + x;
        ref.data    = c->planes[r][p];
        ref.stride  = c->stride[p];
        ref.align_w = c->align_w[p];
        ref.align_h = c->align_h[p];
        ret = chroma ? mc_chroma(dst, ref.stride, &ref, x, y, mv)
                     : mc_luma(dst, ref.stride, &ref, x, y, mv);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* The reference decoder only stores the picture rounded up to 16 luma
 * pixels; the rest of the last macroblock row and column is never written,
 * and the DCs it later takes from the picture there read blank memory.
 * Model that memory as zero. */
static void clear_hidden(Bink2Context *c, int bx, int by, int do_alpha)
{
    int w16 = c->w16, h16 = c->h16;

    for (int p = 0; p < (do_alpha ? 4 : 3); p++) {
        int sh = p == 1 || p == 2, n = 32 >> sh;
        int x0 = bx * n, y0 = by * n, vw = (w16 >> sh) - x0, vh = (h16 >> sh) - y0;
        ptrdiff_t st = c->stride[p];
        uint8_t *dst = c->planes[c->cur][p] + y0 * st + x0;

        if (vw >= n && vh >= n)
            continue;
        vw = av_clip(vw, 0, n);
        vh = av_clip(vh, 0, n);
        for (int y = 0; y < n; y++)
            memset(dst + y * st + (y < vh ? vw : 0), 0, y < vh ? n - vw : n);
    }
}

static void set_dcs_from_picture(Bink2Context *c, SliceCtx *sc, int bx, int by, int do_alpha)
{
    for (int p = 0; p < (do_alpha ? 4 : 3); p++) {
        ptrdiff_t st = c->stride[p];
        if (p == 1 || p == 2) {
            DCInfo *d = p == 1 ? &sc->u : &sc->v;
            const uint8_t *src = c->planes[c->cur][p] + by * 16 * st + bx * 16;
            int dc1 = calc_dc(src + 8, st), dc2 = calc_dc(src + 8 * st, st);
            int dc3 = calc_dc(src + 8 * st + 8, st);
            d->dcs[1] = dc1;
            d->dcs[2] = dc2;
            d->dcs[3] = dc3;
            d->new_dcs[bx * 2 + 2] = dc2;
            d->new_dcs[bx * 2 + 3] = dc3;
        } else {
            DCInfo *d = p ? &sc->a : &sc->y;
            const uint8_t *src = c->planes[c->cur][p] + by * 32 * st + bx * 32;
            d->dcs[5]  = calc_dc(src + 24, st);
            d->dcs[7]  = calc_dc(src + 24 + 8 * st, st);
            d->dcs[13] = calc_dc(src + 24 + 16 * st, st);
            d->dcs[15] = calc_dc(src + 24 + 24 * st, st);
            d->new_dcs[bx * 4 + 4] = calc_dc(src + 24 * st, st);
            d->new_dcs[bx * 4 + 5] = calc_dc(src + 8 + 24 * st, st);
            d->new_dcs[bx * 4 + 6] = calc_dc(src + 16 + 24 * st, st);
            d->new_dcs[bx * 4 + 7] = d->dcs[15];
        }
    }
}

/* info[] is in decoding order: four 16x16 quarters of four 8x8 blocks */
static void set_info_luma(Bink2Context *c, int p, int bx, int by, const uint16_t info[16])
{
    int iw = c->info_w[p];
    uint16_t *d = c->info[p] + by * 4 * iw + bx * 4;
    for (int i = 0; i < 16; i++)
        d[((i >> 3) * 2 + (i >> 1 & 1)) * iw + (i >> 2 & 1) * 2 + (i & 1)] = info[i];
}

static void set_info_chroma(Bink2Context *c, int p, int bx, int by, const uint16_t info[4])
{
    int iw = c->info_w[p];
    uint16_t *d = c->info[p] + by * 2 * iw + bx * 2;
    for (int i = 0; i < 4; i++)
        d[(i >> 1) * iw + (i & 1)] = info[i];
}

static void fill_info(Bink2Context *c, int p, int bx, int by, int n, uint16_t val)
{
    int iw = c->info_w[p];
    uint16_t *d = c->info[p] + by * n * iw + bx * n;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            d[y * iw + x] = val;
}

static void put_luma(Bink2Context *c, int p, int bx, int by, IBlocks blk[4], int add)
{
    ptrdiff_t st = c->stride[p];
    uint8_t *d = c->planes[c->cur][p] + by * 32 * st + bx * 32;
    put_mb4(d,                st, blk[0], add);
    put_mb4(d + 16,           st, blk[1], add);
    put_mb4(d + 16 * st,      st, blk[2], add);
    put_mb4(d + 16 * st + 16, st, blk[3], add);
}

static void put_luma_old(Bink2Context *c, int p, int bx, int by, FBlocks blk[4], int add)
{
    ptrdiff_t st = c->stride[p];
    uint8_t *d = c->planes[c->cur][p] + by * 32 * st + bx * 32;
    put_mb4_old(d,                st, blk[0], add);
    put_mb4_old(d + 16,           st, blk[1], add);
    put_mb4_old(d + 16 * st,      st, blk[2], add);
    put_mb4_old(d + 16 * st + 16, st, blk[3], add);
}

static uint8_t *chroma_dst(Bink2Context *c, int p, int bx, int by)
{
    return c->planes[c->cur][p] + by * 16 * c->stride[p] + bx * 16;
}

static void swap_rows(SliceCtx *sc)
{
    FFSWAP(int, sc->qpos, sc->pqpos);
    FFSWAP(int, sc->mv_off, sc->prev_off);
    FFSWAP(int *, sc->y.prev_dcs, sc->y.new_dcs);
    FFSWAP(int *, sc->u.prev_dcs, sc->u.new_dcs);
    FFSWAP(int *, sc->v.prev_dcs, sc->v.new_dcs);
    FFSWAP(int *, sc->a.prev_dcs, sc->a.new_dcs);
}

/* ---------------------------------------------------------------- post-processing */

/* Taps for the edge between the 8x8 blocks at (ax, ay), left of or above,
 * and (bx, by): by coefficient class next to an intra block, otherwise all
 * four where the 16x16 motion blocks' vectors differ by 2 or more. */
static int edge_taps_frame(const Bink2Context *c, int p, int ax, int ay, int bx, int by)
{
    int iw = c->info_w[p], mshift = p == 1 || p == 2 ? 0 : 1, mw = c->bw * 2;
    uint16_t a = c->info[p][ay * iw + ax], b = c->info[p][by * iw + bx];
    MV ma, mb;

    if ((a | b) & 0x8000)
        return bink2_edge_taps(a, b);
    if ((ax >> mshift) == (bx >> mshift) && (ay >> mshift) == (by >> mshift))
        return 0;
    ma = c->mv_grid[(ay >> mshift) * mw + (ax >> mshift)];
    mb = c->mv_grid[(by >> mshift) * mw + (bx >> mshift)];
    return FFABS(ma.x - mb.x) >= 2 || FFABS(ma.y - mb.y) >= 2 ? 0xF : 0;
}

static void deblock_v(Bink2Context *c, int p, int fstep, int x, int y)
{
    ptrdiff_t st = c->stride[p];
    uint8_t *pix = c->planes[c->cur][p];
    int taps;
    if (c->col_flags[x * fstep])
        return;
    taps = edge_taps_frame(c, p, x - 1, y, x, y);
    if (taps)
        bink2_filter_edge(pix + y * 8 * st + x * 8, 1, st, taps);
}

static void deblock_h(Bink2Context *c, int p, int fstep, int x, int y)
{
    ptrdiff_t st = c->stride[p];
    uint8_t *pix = c->planes[c->cur][p];
    int taps;
    if (c->row_flags[y * fstep])
        return;
    taps = edge_taps_frame(c, p, x, y - 1, x, y);
    if (taps)
        bink2_filter_edge(pix + y * 8 * st + x * 8, st, 1, taps);
}

/* Vertical edges over a pw x ph area, then horizontal ones, leaving out
 * its borders and the edges the bitstream flags as off. */
static void deblock_plane(Bink2Context *c, int p, int fstep, int pw, int ph)
{
    int iw = FFMIN(c->info_w[p], pw >> 3), ih = FFMIN(c->info_h[p], ph >> 3);

    if (!(c->frame_flags & 0x4000))
        for (int x = 1; x < iw; x++)
            for (int y = 0; y < ih; y++)
                deblock_v(c, p, fstep, x, y);
    if (!(c->frame_flags & 0x8000))
        for (int y = 1; y < ih; y++)
            for (int x = 0; x < iw; x++)
                deblock_h(c, p, fstep, x, y);
}

static int decode_slice(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    Bink2Context *c = avctx->priv_data;
    SliceCtx *sc = &c->slices[jobnr];
    GetBitContext *gb = &sc->gb;
    uint32_t frame_flags = c->frame_flags;
    int is_intra = c->is_intra, do_alpha = c->do_alpha, bw = c->bw;
    int is_new = !!(frame_flags & 0x2000), ret;
    uint32_t row_state = frame_flags & 0x2E000;

    if (is_intra)
        row_state |= 0x10000;
    sc->qpos = 0;
    sc->pqpos = bw * 2;
    sc->mv_off = 0;
    sc->prev_off = bw * 4;

    for (int by = sc->start_by; by < sc->end_by; by++) {
        uint32_t cbp_y = 0, cbp_u = 0, cbp_v = 0, cbp_a = 0;
        uint32_t cbp_y_p = 0, cbp_u_p = 0, cbp_v_p = 0, cbp_a_p = 0;
        int q_y = 8, q_u = 8, q_v = 8, q_a = 8;
        int q_y_p = 8, q_u_p = 8, q_v_p = 8, q_a_p = 8;
        uint32_t rflags = c->row_flags[by] * 4, edge_state = 0;
        uint8_t lru[4] = { 2, 3, 1, 0 };
        int is_top = by == sc->start_by;

        row_state = (row_state & 0x3FFFFFF) | ((row_state >> 4) & 0xC000000) | (rflags << 28);
        if (by == sc->start_by)
            row_state |= 0x80;
        if (by + 2 >= sc->end_by)
            row_state |= 0x100;

        for (int bx = 0; bx < bw; bx++) {
            uint32_t blk_state = row_state | (edge_state & 0x3FC0000);
            uint32_t clflags = c->col_flags[bx] * 4, edge_state_c, edge_state_y;
            int is_left = bx == 0, btype;

            if (bx == 0)
                blk_state |= 0x20;
            if (bx + 2 < bw)
                blk_state |= 0x40;
            if (bx & 1)
                blk_state |= 0x200;
            edge_state_c = ((blk_state >> 4) & 0x3C0000) | (blk_state & 0xFC03FFFF) | ((clflags & 0xF) << 22);
            edge_state_y = (frame_flags & 0x40000) | (blk_state & 0x3FFFF);
            edge_state = edge_state_c;

            if (get_bits_left(gb) < 0)
                return AVERROR_INVALIDDATA;
            if (is_intra) {
                btype = 0;
            } else if (blk_state & 0x2000) {
                switch (read_unary_lim(gb, 3, 1)) {
                case 0:
                    btype = lru[0];
                    break;
                case 1:
                    btype = lru[1];
                    lru[1] = lru[0];
                    lru[0] = btype;
                    break;
                case 2:
                    btype = lru[3];
                    lru[3] = lru[2];
                    lru[2] = btype;
                    break;
                default:
                    btype = lru[2];
                    lru[2] = lru[1];
                    lru[1] = btype;
                    break;
                }
            } else {
                btype = get_bits(gb, 2);
            }
            if (!is_intra) {
                sc->quants[sc->qpos + bx * 2]     = pred_quant(sc, bx * 2, is_top);
                sc->quants[sc->qpos + bx * 2 + 1] = pred_quant(sc, bx * 2 + 1, is_top);
            }

            if (btype) {
                fill_info(c, 0, bx, by, 4, 0);
                fill_info(c, 1, bx, by, 2, 0);
                fill_info(c, 2, bx, by, 2, 0);
                if (do_alpha)
                    fill_info(c, 3, bx, by, 4, 0);
            }
            switch (btype) {
            case 0:
                if (is_new) {
                    IBlocks y[4] = { { { 0 } } }, u = { { 0 } }, v = { { 0 } };
                    uint16_t iy[16], iu[4], iv[4];
                    int q = get_new_quant(gb, pred_quant(sc, bx * 2, is_top));
                    if (q < 0)
                        return q;
                    sc->quants[sc->qpos + bx * 2] = q;
                    if ((ret = decode_luma_intra(gb, cbp_y, q, y, edge_state_y, &sc->y, bx, &cbp_y, iy)) < 0 ||
                        (ret = decode_chroma_intra(gb, cbp_v, q, v, edge_state_c, &sc->v, bx, &cbp_v, iv)) < 0 ||
                        (ret = decode_chroma_intra(gb, cbp_u, q, u, edge_state_c, &sc->u, bx, &cbp_u, iu)) < 0)
                        return ret;
                    set_info_luma(c, 0, bx, by, iy);
                    set_info_chroma(c, 1, bx, by, iu);
                    set_info_chroma(c, 2, bx, by, iv);
                    if (do_alpha) {
                        IBlocks a[4] = { { { 0 } } };
                        uint16_t ia[16];
                        if ((ret = decode_luma_intra(gb, cbp_a, q, a, edge_state_y, &sc->a, bx, &cbp_a, ia)) < 0)
                            return ret;
                        set_info_luma(c, 3, bx, by, ia);
                        put_luma(c, 3, bx, by, a, 0);
                    }
                    put_luma(c, 0, bx, by, y, 0);
                    put_mb4(chroma_dst(c, 1, bx, by), c->stride[1], u, 0);
                    put_mb4(chroma_dst(c, 2, bx, by), c->stride[2], v, 0);
                } else {
                    FBlocks y[4] = { { { 0 } } }, u = { { 0 } }, v = { { 0 } };
                    if ((ret = decode_luma_old(gb, cbp_y, y, edge_state_y, &sc->y, bx, &q_y, 1, &cbp_y)) < 0 ||
                        (ret = decode_chroma_old(gb, cbp_v, v, edge_state_c, &sc->v, bx, &q_v, 1, &cbp_v)) < 0 ||
                        (ret = decode_chroma_old(gb, cbp_u, u, edge_state_c, &sc->u, bx, &q_u, 1, &cbp_u)) < 0)
                        return ret;
                    if (do_alpha) {
                        FBlocks a[4] = { { { 0 } } };
                        if ((ret = decode_luma_old(gb, cbp_a, a, edge_state_y, &sc->a, bx, &q_a, 1, &cbp_a)) < 0)
                            return ret;
                        put_luma_old(c, 3, bx, by, a, 0);
                    }
                    put_luma_old(c, 0, bx, by, y, 0);
                    put_mb4_old(chroma_dst(c, 1, bx, by), c->stride[1], u, 0);
                    put_mb4_old(chroma_dst(c, 2, bx, by), c->stride[2], v, 0);
                }
                if (!is_intra)
                    predict_mvs(sc, bx, is_top, is_left, is_new);
                break;
            case 1:
                if (!c->have_ref)
                    return AVERROR_INVALIDDATA;
                for (int b = 0; b < 4; b++)
                    if ((ret = mc_block(c, bx, by, (MV){ 0, 0 }, b, do_alpha)) < 0)
                        return ret;
                memset(&sc->mvs[sc->mv_off + bx * 4], 0, 4 * sizeof(MV));
                break;
            default:
                if (is_new) {
                    if ((ret = decode_mv_new(sc, bx, is_top, is_left)) < 0)
                        return ret;
                } else {
                    decode_mv_old(sc, bx, is_top, is_left);
                }
                if (!c->have_ref)
                    return AVERROR_INVALIDDATA;
                for (int b = 0; b < 4; b++)
                    if ((ret = mc_block(c, bx, by, sc->mvs[sc->mv_off + bx * 4 + b], b, do_alpha)) < 0)
                        return ret;
                if (btype == 2)
                    break;
                if (is_new) {
                    IBlocks y[4] = { { { 0 } } }, u = { { 0 } }, v = { { 0 } };
                    uint16_t iy[16], iu[4], iv[4];
                    int q = get_new_quant(gb, pred_quant(sc, bx * 2 + 1, is_top));
                    if (q < 0)
                        return q;
                    sc->quants[sc->qpos + bx * 2 + 1] = q;
                    if ((ret = decode_luma_inter(gb, cbp_y_p, q, y, edge_state_y, &sc->y, &cbp_y_p, iy)) < 0)
                        return ret;
                    set_info_luma(c, 0, bx, by, iy);
                    if (get_bits1(gb)) {
                        if ((ret = decode_chroma_inter(gb, cbp_v_p, q, v, edge_state_y, &sc->v, &cbp_v_p, iv)) < 0 ||
                            (ret = decode_chroma_inter(gb, cbp_u_p, q, u, edge_state_y, &sc->u, &cbp_u_p, iu)) < 0)
                            return ret;
                        set_info_chroma(c, 1, bx, by, iu);
                        set_info_chroma(c, 2, bx, by, iv);
                    } else {
                        cbp_v_p = cbp_u_p = 0;
                    }
                    if (do_alpha) {
                        IBlocks a[4] = { { { 0 } } };
                        uint16_t ia[16];
                        if ((ret = decode_luma_inter(gb, cbp_a_p, q, a, edge_state_y, &sc->a, &cbp_a_p, ia)) < 0)
                            return ret;
                        set_info_luma(c, 3, bx, by, ia);
                        put_luma(c, 3, bx, by, a, 1);
                    }
                    put_luma(c, 0, bx, by, y, 1);
                    put_mb4(chroma_dst(c, 1, bx, by), c->stride[1], u, 1);
                    put_mb4(chroma_dst(c, 2, bx, by), c->stride[2], v, 1);
                } else {
                    FBlocks y[4] = { { { 0 } } }, u = { { 0 } }, v = { { 0 } };
                    if ((ret = decode_luma_old(gb, cbp_y_p, y, edge_state_y, &sc->y, bx, &q_y_p, 0, &cbp_y_p)) < 0 ||
                        (ret = decode_chroma_old(gb, cbp_v_p, v, edge_state_y, &sc->v, bx, &q_v_p, 0, &cbp_v_p)) < 0 ||
                        (ret = decode_chroma_old(gb, cbp_u_p, u, edge_state_y, &sc->u, bx, &q_u_p, 0, &cbp_u_p)) < 0)
                        return ret;
                    if (do_alpha) {
                        FBlocks a[4] = { { { 0 } } };
                        if ((ret = decode_luma_old(gb, cbp_a_p, a, edge_state_y, &sc->a, bx, &q_a_p, 0, &cbp_a_p)) < 0)
                            return ret;
                        put_luma_old(c, 3, bx, by, a, 1);
                    }
                    put_luma_old(c, 0, bx, by, y, 1);
                    put_mb4_old(chroma_dst(c, 1, bx, by), c->stride[1], u, 1);
                    put_mb4_old(chroma_dst(c, 2, bx, by), c->stride[2], v, 1);
                }
                break;
            }
            for (int b = 0; b < 4; b++) {
                MV mv = btype >= 2 ? sc->mvs[sc->mv_off + bx * 4 + b] : (MV){ 0, 0 };
                c->mv_grid[(by * 2 + (b >> 1)) * bw * 2 + bx * 2 + (b & 1)] = mv;
            }
            clear_hidden(c, bx, by, do_alpha);
            if (btype)
                set_dcs_from_picture(c, sc, bx, by, do_alpha);
        }
        swap_rows(sc);
        row_state = (row_state & ~0x190) | ((row_state & 4) << 2);
    }
    return get_bits_left(gb) < 0 ? AVERROR_INVALIDDATA : 0;
}

static int bink2_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                              int *got_frame, AVPacket *avpkt)
{
    Bink2Context *c = avctx->priv_data;
    GetBitContext gb;
    uint32_t offsets[MAX_SLICES - 1] = { 0 };
    int rets[MAX_SLICES] = { 0 };
    int ret, w = avctx->width, h = avctx->height;

    if ((ret = init_get_bits8(&gb, avpkt->data, avpkt->size)) < 0)
        return ret;
    if (avpkt->size < 4 * c->num_slices)
        return AVERROR_INVALIDDATA;

    c->is_intra = !!(avpkt->flags & AV_PKT_FLAG_KEY);
    if (!c->is_intra && !c->have_ref) {
        av_log(avctx, AV_LOG_WARNING, "inter frame without a reference\n");
        return AVERROR_INVALIDDATA;
    }
    c->frame_flags = get_bits_long(&gb, 32);
    for (int i = 0; i < c->num_slices - 1; i++) {
        offsets[i] = get_bits_long(&gb, 32);
        if (offsets[i] >= avpkt->size)
            return AVERROR_INVALIDDATA;
    }
    c->cur ^= 1;
    c->do_alpha = c->has_alpha;
    if ((c->frame_flags & 0x80000) && c->has_alpha) {
        c->do_alpha = 0;
        memset(c->planes[c->cur][3], c->frame_flags >> 24, c->stride[3] * c->plane_h[3]);
    }
    memset(c->row_flags, 0, c->bh * 4);
    memset(c->col_flags, 0, c->bw * 4);
    if (c->frame_flags & 0x10000) {
        if (!(c->frame_flags & 0x8000) &&
            (ret = decode_flags(&gb, c->row_flags, c->bh * 4, ((h + 15) >> 4) * 2 - 1)) < 0)
            goto fail;
        if (!(c->frame_flags & 0x4000) &&
            (ret = decode_flags(&gb, c->col_flags, c->bw * 4, ((w + 15) >> 4) * 2 - 1)) < 0)
            goto fail;
    }

    for (int i = 0, start = 0; i < c->num_slices; i++) {
        SliceCtx *sc = &c->slices[i];
        sc->start_by = start;
        sc->end_by   = c->slice_h[i];
        start = sc->end_by;
        if (!i) {
            sc->gb = gb;
        } else {
            init_get_bits8(&sc->gb, avpkt->data, avpkt->size);
            skip_bits_long(&sc->gb, offsets[i - 1] * 8);
        }
    }
    avctx->execute2(avctx, decode_slice, NULL, rets, c->num_slices);
    for (int i = 0; i < c->num_slices; i++)
        if (rets[i] < 0) {
            ret = rets[i];
            goto fail;
        }

    /* The reference decoder's in-loop post-processing of the newer
     * bitstream: smooth DC-only intra blocks, then deblock if the frame
     * carries edge flags. */
    if (c->frame_flags & 0x2000) {
        int nplanes = c->do_alpha ? 4 : 3;
        for (int p = 0; p < nplanes; p++)
            bink2_smooth_dc_plane(c->planes[c->cur][p], c->stride[p], c->info[p],
                                  c->info_w[p], c->info_h[p], (c->frame_flags & 0xFF) * 8);
        /* the reference keeps the picture rounded up to 16 luma pixels */
        if (c->frame_flags & 0x10000)
            for (int p = 0; p < nplanes; p++) {
                int chroma = p == 1 || p == 2, w16 = FFALIGN(w, 16), h16 = FFALIGN(h, 16);
                deblock_plane(c, p, chroma ? 2 : 1, chroma ? w16 >> 1 : w16, chroma ? h16 >> 1 : h16);
            }
    }
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        goto fail;
    for (int p = 0; p < (c->has_alpha ? 4 : 3); p++) {
        int chroma = p == 1 || p == 2;
        av_image_copy_plane(frame->data[p], frame->linesize[p], c->planes[c->cur][p], c->stride[p],
                            chroma ? (w + 1) >> 1 : w, chroma ? (h + 1) >> 1 : h);
    }
    frame->pict_type = c->is_intra ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    if (c->is_intra)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;
    c->have_ref = 1;
    *got_frame = 1;
    return avpkt->size;
fail:
    c->cur ^= 1;    /* the reference is untouched */
    return ret;
}

static av_cold int bink2_decode_init(AVCodecContext *avctx)
{
    static AVOnce init_static_once = AV_ONCE_INIT;
    Bink2Context *c = avctx->priv_data;
    int w = avctx->width, h = avctx->height, height_a, ret;
    uint32_t flags;

    if ((ret = av_image_check_size(w, h, 0, avctx)) < 0)
        return ret;
    if ((avctx->codec_tag & 0xFFFFFF) != MKTAG('K', 'B', '2', 0) || avctx->extradata_size < 4) {
        av_log(avctx, AV_LOG_ERROR, "needs the KB2x file tag and the video flags\n");
        return AVERROR_INVALIDDATA;
    }
    c->version   = avctx->codec_tag >> 24;
    flags        = AV_RL32(avctx->extradata);
    c->has_alpha = !!(flags & 0x100000);

    height_a = (h + 31) & ~31;
    if (c->version <= 'f') {
        c->num_slices = 2;
        c->slice_h[0] = (h + 32) >> 6;
    } else if (c->version == 'g') {
        if (height_a < 128) {
            c->num_slices = 1;
        } else {
            c->num_slices = 2;
            c->slice_h[0] = (h + 31) >> 6;
        }
    } else {
        static const int nsl[4] = { 2, 3, 4, 8 };
        int start = 0, end;
        c->num_slices = nsl[flags & 3];
        end = height_a + 32 * c->num_slices - 1;
        for (int i = 0; i < c->num_slices - 1; i++) {
            start += ((end - start) / (c->num_slices - i)) & ~31;
            end -= 32;
            c->slice_h[i] = start >> 5;
        }
    }
    c->slice_h[c->num_slices - 1] = height_a >> 5;

    c->bw = (w + 31) >> 5;
    c->bh = (h + 31) >> 5;
    c->w16 = FFALIGN(w, 16);
    c->h16 = FFALIGN(h, 16);
    for (int p = 0; p < 4; p++) {
        int chroma = p == 1 || p == 2, pw = chroma ? (w + 1) >> 1 : w, ph = chroma ? (h + 1) >> 1 : h;
        int al = chroma ? 15 : 31;
        c->stride[p]  = chroma ? c->bw * 16 : c->bw * 32;
        c->plane_h[p] = chroma ? c->bh * 16 : c->bh * 32;
        c->align_w[p] = (pw + al) & ~al;
        c->align_h[p] = (ph + al) & ~al;
        if (p == 3 && !c->has_alpha)
            break;
        for (int i = 0; i < 2; i++)
            if (!(c->planes[i][p] = av_mallocz(c->stride[p] * c->plane_h[p])))
                return AVERROR(ENOMEM);
        c->info_w[p] = c->stride[p] >> 3;
        c->info_h[p] = c->plane_h[p] >> 3;
        if (!(c->info[p] = av_calloc(c->info_w[p] * c->info_h[p], sizeof(*c->info[p]))))
            return AVERROR(ENOMEM);
    }
    c->mv_grid   = av_calloc(c->bw * c->bh * 4, sizeof(*c->mv_grid));
    if (!c->mv_grid)
        return AVERROR(ENOMEM);
    c->row_flags = av_mallocz(c->bh * 4);
    c->col_flags = av_mallocz(c->bw * 4);
    if (!c->row_flags || !c->col_flags)
        return AVERROR(ENOMEM);
    for (int i = 0; i < c->num_slices; i++) {
        SliceCtx *sc = &c->slices[i];
        DCInfo *dcs[4] = { &sc->y, &sc->u, &sc->v, &sc->a };
        sc->quants = av_mallocz(c->bw * 4);
        sc->mvs    = av_calloc(c->bw * 8, sizeof(*sc->mvs));
        if (!sc->quants || !sc->mvs)
            return AVERROR(ENOMEM);
        for (int d = 0; d < 4; d++) {
            int n = d == 1 || d == 2 ? c->bw * 2 + 2 : c->bw * 4 + 4;
            for (int j = 0; j < 2; j++)
                if (!(dcs[d]->bufs[j] = av_calloc(n, sizeof(int))))
                    return AVERROR(ENOMEM);
            dcs[d]->prev_dcs = dcs[d]->bufs[0];
            dcs[d]->new_dcs  = dcs[d]->bufs[1];
        }
    }

    avctx->pix_fmt     = c->has_alpha ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P;
    avctx->color_range = AVCOL_RANGE_JPEG;
    ff_thread_once(&init_static_once, init_codebooks);
    return 0;
}

static av_cold int bink2_decode_end(AVCodecContext *avctx)
{
    Bink2Context *c = avctx->priv_data;
    for (int i = 0; i < 2; i++)
        for (int p = 0; p < 4; p++)
            av_freep(&c->planes[i][p]);
    for (int p = 0; p < 4; p++)
        av_freep(&c->info[p]);
    av_freep(&c->mv_grid);
    av_freep(&c->row_flags);
    av_freep(&c->col_flags);
    for (int i = 0; i < MAX_SLICES; i++) {
        SliceCtx *sc = &c->slices[i];
        DCInfo *dcs[4] = { &sc->y, &sc->u, &sc->v, &sc->a };
        av_freep(&sc->quants);
        av_freep(&sc->mvs);
        for (int d = 0; d < 4; d++)
            for (int j = 0; j < 2; j++)
                av_freep(&dcs[d]->bufs[j]);
    }
    return 0;
}

static av_cold void bink2_decode_flush(AVCodecContext *avctx)
{
    Bink2Context *c = avctx->priv_data;
    c->have_ref = 0;
}

const FFCodec ff_bink2_decoder = {
    .p.name         = "bink2video",
    CODEC_LONG_NAME("Bink video 2"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_BINKVIDEO2,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_SLICE_THREADS,
    .priv_data_size = sizeof(Bink2Context),
    .init           = bink2_decode_init,
    FF_CODEC_DECODE_CB(bink2_decode_frame),
    .close          = bink2_decode_end,
    .flush          = bink2_decode_flush,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
