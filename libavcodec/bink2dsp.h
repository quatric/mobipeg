/*
 * Bink 2 video reconstruction shared by the encoder and the decoder
 * Copyright (C) 2026 quatric <quatricsoftware@gmail.com>
 *
 * The transform follows the Bink 2 decoder of NihAV,
 * Copyright (C) Kostya Shishkov (https://nihav.org); the rounding, the DC
 * smoothing and the edge filter reproduce the reference decoder's output.
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

#ifndef AVCODEC_BINK2DSP_H
#define AVCODEC_BINK2DSP_H

#include <stddef.h>
#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"

/* The reference works in 16-bit SIMD lanes: every step wraps to int16. */
#define B2W16(v)   ((int16_t)(v))
#define B2MUL_A(v) B2W16((v) + ((v) >> 2))
#define B2MUL_B(v) ((v) >> 1)
#define B2MUL_C(v) B2W16(B2W16((v) - ((v) >> 2)) - ((v) >> 4))
#define B2MUL_D(v) B2W16(B2W16((v) + ((v) >> 2)) - ((v) >> 4))
#define B2MUL_E(v) ((v) >> 2)

static av_unused void bink2_idct1d(const int16_t *src, int sstep, int16_t *dst, int dstep, int shift)
{
    int tmp00 = B2W16(src[3 * sstep] + src[5 * sstep]);
    int tmp01 = B2W16(src[3 * sstep] - src[5 * sstep]);
    int tmp02 = B2W16(B2MUL_A(src[2 * sstep]) + B2MUL_B(src[6 * sstep]));
    int tmp03 = B2W16(B2MUL_B(src[2 * sstep]) - B2MUL_A(src[6 * sstep]));
    int tmp0  = B2W16(B2W16(src[0] + src[4 * sstep]) + tmp02);
    int tmp1  = B2W16(B2W16(src[0] + src[4 * sstep]) - tmp02);
    int tmp2  = B2W16(src[0] - src[4 * sstep]);
    int tmp3  = B2W16(src[sstep] + tmp00);
    int tmp4  = B2W16(src[sstep] - tmp00);
    int tmp5  = B2W16(tmp01 + src[7 * sstep]);
    int tmp6  = B2W16(tmp01 - src[7 * sstep]);
    int tmp7  = B2W16(tmp4 + B2MUL_C(tmp6));
    int tmp8  = B2W16(B2MUL_C(tmp4) - tmp6);
    int tmp9  = B2W16(B2MUL_D(tmp3) + B2MUL_E(tmp5));
    int tmp10 = B2W16(B2MUL_E(tmp3) - B2MUL_D(tmp5));
    int tmp11 = B2W16(tmp2 + tmp03);
    int tmp12 = B2W16(tmp2 - tmp03);
    dst[0]         = B2W16(tmp0  + tmp9)  >> shift;
    dst[dstep]     = B2W16(tmp11 + tmp7)  >> shift;
    dst[2 * dstep] = B2W16(tmp12 + tmp8)  >> shift;
    dst[3 * dstep] = B2W16(tmp1  + tmp10) >> shift;
    dst[4 * dstep] = B2W16(tmp1  - tmp10) >> shift;
    dst[5 * dstep] = B2W16(tmp12 - tmp8)  >> shift;
    dst[6 * dstep] = B2W16(tmp11 - tmp7)  >> shift;
    dst[7 * dstep] = B2W16(tmp0  - tmp9)  >> shift;
}

/* The reference transforms each column, then the rows of that result,
 * storing them back as columns; coefficients are int16. */
static av_unused void bink2_idct(int c[64])
{
    int16_t in[64], tmp[64], out[64];
    for (int i = 0; i < 64; i++)
        in[i] = c[i];
    for (int i = 0; i < 8; i++)
        bink2_idct1d(in + i, 8, tmp + i, 8, 0);
    for (int i = 0; i < 8; i++)
        bink2_idct1d(tmp + 8 * i, 1, out + i, 8, 6);
    for (int i = 0; i < 64; i++)
        c[i] = out[i];
}

/* Chroma motion compensation tap: 0 to 3 quarters of the way from e0 to
 * e1, rounding to nearest at the given shift. */
static av_unused int bink2_chroma_interp(int e0, int e1, int mode, int shift)
{
    int rnd = (1 << shift) >> 1;
    switch (mode) {
    case 0:  return e0;
    case 1:  return (e0 * 3 + e1     + rnd) >> shift;
    case 2:  return (e0 * 2 + e1 * 2 + rnd) >> shift;
    default: return (e0     + e1 * 3 + rnd) >> shift;
    }
}

/* The word the post-processing keeps for each 8x8 block: the DC, the
 * number of coded coefficients as a class from 0 (8 or more) to 3 (none),
 * and whether the block is intra. Inter blocks are always class 0. */
static av_unused uint16_t bink2_block_info(int dc, int ncoded, int intra)
{
    int cls = intra ? (ncoded < 8) + (ncoded < 4) + (ncoded == 0) : 0;
    return (dc & 0x1FFF) | cls << 13 | (intra ? 0x8000 : 0);
}

/* Weight of a neighbour for each of 8 positions across an 8-pixel span: a
 * 16-bit fixed-point multiply on the near side, a shift on the far side. */
static av_unused int bink2_smooth_tap(int pos, int d)
{
    switch (pos) {
    case 0:  return (int16_t)d * 0x6800 >> 16;
    case 1:  return (int16_t)d * 0x4800 >> 16;
    case 2:  return (int16_t)d * 0x3000 >> 16;
    case 3:  return (int16_t)d * 0x1C00 >> 16;
    case 4:  return (int16_t)d >> 4;
    case 5:  return (int16_t)d >> 5;
    case 6:  return (int16_t)d >> 7;
    default: return 0;
    }
}

static av_unused int bink2_smooth_interp(int centre, int dl, int dr, int pos)
{
    return B2W16(centre + bink2_smooth_tap(pos, dl) + bink2_smooth_tap(7 - pos, dr));
}

/* A neighbour's DC if its whole info word is within thr of the centre's,
 * so that only blocks of the same kind qualify. */
static av_unused int bink2_smooth_pick(int n, int centre, int thr, int fallback)
{
    int d = FFABS(B2W16(centre - n));
    if (d == 0x8000)
        d = -0x8000;
    return thr > d ? n & 0x1FFF : fallback;
}

/* The DCs a DC-only intra block is smoothed towards, from its 3x3
 * neighbourhood of info words nb, in raster order into v: a side falls back
 * to the centre and a corner to the side next to it in the same row. */
static av_unused void bink2_smooth_select(const uint16_t nb[3][3], int thr, int v[9])
{
    int ctr = nb[1][1], c = ctr & 0x1FFF;
    v[4] = c;
    v[3] = bink2_smooth_pick(nb[1][0], ctr, thr, c);
    v[5] = bink2_smooth_pick(nb[1][2], ctr, thr, c);
    v[1] = bink2_smooth_pick(nb[0][1], ctr, thr, c);
    v[7] = bink2_smooth_pick(nb[2][1], ctr, thr, c);
    v[0] = bink2_smooth_pick(nb[0][0], ctr, thr, v[3]);
    v[2] = bink2_smooth_pick(nb[0][2], ctr, thr, v[5]);
    v[6] = bink2_smooth_pick(nb[2][0], ctr, thr, v[3]);
    v[8] = bink2_smooth_pick(nb[2][2], ctr, thr, v[5]);
}

/* Draw an intra block that has only a DC as a surface through the DCs of
 * its neighbours; nb is the 3x3 neighbourhood of info words. */
static av_unused void bink2_smooth_dc_block(uint8_t *dst, ptrdiff_t stride,
                                            const uint16_t nb[3][3], int thr)
{
    int v[9], hu[8], hc[8], hd[8];

    bink2_smooth_select(nb, thr, v);
    for (int x = 0; x < 8; x++) {
        hu[x] = bink2_smooth_interp(v[1] + 4, v[0] - v[1], v[2] - v[1], x);
        hc[x] = bink2_smooth_interp(v[4] + 4, v[3] - v[4], v[5] - v[4], x);
        hd[x] = bink2_smooth_interp(v[7] + 4, v[6] - v[7], v[8] - v[7], x);
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            dst[y * stride + x] = av_clip_uint8(bink2_smooth_interp(hc[x], B2W16(hu[x] - hc[x]),
                                                                    B2W16(hd[x] - hc[x]), y) >> 3);
}

typedef void (*Bink2SmoothBlock)(uint8_t *dst, ptrdiff_t stride, const uint16_t nb[3][3], int thr);

/* Smooth every intra DC-only block of a plane whose 8x8 blocks are
 * described by an iw x ih grid of info words. */
static av_unused void bink2_smooth_dc_plane(uint8_t *pix, ptrdiff_t stride, const uint16_t *info,
                                            int iw, int ih, int thr, Bink2SmoothBlock block)
{
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            uint16_t nb[3][3];
            if ((info[y * iw + x] & 0xE000) != 0xE000)
                continue;
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++) {
                    int xx = x + i - 1, yy = y + j - 1;
                    nb[j][i] = xx < 0 || yy < 0 || xx >= iw || yy >= ih ? 0 : info[yy * iw + xx];
                }
            block(pix + y * 8 * stride + x * 8, stride, nb, thr);
        }
}

/* Which of p1, p0, q0, q1 (bits 1, 0, 2, 3) an edge filter changes, from
 * the classes of the blocks on either side: the fewer coefficients a block
 * had, the more of it is filtered; two DC-only blocks are left alone. */
static av_unused int bink2_edge_taps(uint16_t a, uint16_t b)
{
    int ca = a >> 13 & 3, cb = b >> 13 & 3;
    if (ca == 3 && cb == 3)
        return 0;
    return (ca >= 2 ? 3 : ca) | (cb >= 2 ? 3 : cb) << 2;
}

/* Filter the edge before s across 8 pixels: p1, p0 | q0, q1 sit at
 * -2, -1, 0 and 1 times step, and next steps along the edge. */
static av_unused void bink2_filter_edge(uint8_t *s, ptrdiff_t step, ptrdiff_t next, int taps)
{
    for (int i = 0; i < 8; i++, s += next) {
        int d = s[0] - s[-step];
        int d1 = (d + 4) >> 3, d2 = (d + 2) >> 2;
        if (taps & 2) s[-2 * step] = av_clip_uint8(s[-2 * step] + d1);
        if (taps & 1) s[-step]     = av_clip_uint8(s[-step] + d2);
        if (taps & 4) s[0]         = av_clip_uint8(s[0] - d2);
        if (taps & 8) s[step]      = av_clip_uint8(s[step] - d1);
    }
}

#endif /* AVCODEC_BINK2DSP_H */
