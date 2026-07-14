/*
 * MobiClip / ActImagine DS (.vx) Video encoder
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
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
 *
 * Produces spec-valid ActImagine ".vx" (VXDS) video streams that the matching
 * decoder (and the actimagine Python reference) reconstruct.
 *
 * Intra path: every 16x16 macroblock is split (h-split then v-split) down to
 * 8x8, each 8x8 DC-predicted per plane and its residual forward-DCT'd,
 * quantized and CAVLC-coded (a port of the reference "SimpleKeyframeOnly" +
 * "encode_residu_blocks" strategy).
 *
 * Inter path (P-frames): each 16x16 macroblock is motion-searched against up
 * to three previous reconstructed frames (ref0..ref2, exactly the ring the
 * decoder maintains), the MV is coded as a median-predicted delta (or a "skip"
 * when the MV equals the prediction), and an optional CAVLC residual is coded
 * on top of the motion-compensated prediction. A Lagrangian RD decision picks,
 * per macroblock, between the cheapest inter candidate and a full intra
 * encoding. A keyframe (all-intra frame) is emitted every `keyint` frames.
 *
 * Rate control: the VX container carries ONE global quantizer for the whole
 * stream (there is no per-frame QP in the bitstream), so classic per-frame QP
 * modulation is impossible. When a target bit rate is requested the encoder
 * instead probes the first frame at a ladder of quantizers and locks the single
 * global quantizer whose extrapolated rate is closest to the target. The chosen
 * value is handed to the muxer (as NEW_EXTRADATA side data) so it lands in the
 * file header.
 */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "put_bits.h"
#include "put_golomb.h"

#include "vx_cavlc_vlc.h"

static const uint8_t coeff_token_table_index[17] = {
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3
};

static const uint16_t quant4x4_tab[6][3] = {
    { 0x0A, 0x0D, 0x10 }, { 0x0B, 0x0E, 0x12 }, { 0x0D, 0x10, 0x14 },
    { 0x0E, 0x12, 0x17 }, { 0x10, 0x14, 0x19 }, { 0x12, 0x17, 0x1D },
};

static const uint8_t residu_mask_new_tab[32] = {
    0x00, 0x08, 0x04, 0x02, 0x01, 0x1F, 0x0F, 0x0A,
    0x05, 0x0C, 0x03, 0x10, 0x0E, 0x0D, 0x0B, 0x07,
    0x09, 0x06, 0x1E, 0x1B, 0x1A, 0x1D, 0x17, 0x15,
    0x18, 0x12, 0x11, 0x1C, 0x14, 0x13, 0x16, 0x19,
};

static const uint8_t zigzag_scan[16] = {
    0*4+0, 1*4+0, 0*4+1, 0*4+2,
    1*4+1, 2*4+0, 3*4+0, 2*4+1,
    1*4+2, 0*4+3, 1*4+3, 2*4+2,
    3*4+1, 3*4+2, 2*4+3, 3*4+3,
};

static const double dct_gradient[4][4] = {
    {  1.0,  1.0,  1.0,  1.0 },
    {  1.0,  0.5, -0.5, -1.0 },
    {  1.0, -1.0, -1.0,  1.0 },
    {  0.5, -1.0,  1.0, -0.5 },
};

static const int cavlc_suffix_limit[7] = { 0, 3, 6, 12, 24, 48, 0x8000 };

/* macroblock modes we emit (subset of the 24 the decoder understands) */
enum {
    MODE_VSPLIT_NORES   = 0,
    MODE_INTER0_NORES   = 1,   /* ref0, mv = pred,   no residual  ("skip") */
    MODE_HSPLIT_NORES   = 2,
    MODE_INTER0D_NORES  = 4,   /* ref0, mv = pred+delta, no residual */
    MODE_INTER1D_NORES  = 5,   /* ref1, delta,       no residual */
    MODE_INTER2D_NORES  = 6,   /* ref2, delta,       no residual */
    MODE_INTER1_NORES   = 9,   /* ref1, mv = pred,   no residual */
    MODE_INTER0_RES     = 12,  /* ref0, mv = pred,   residual */
    MODE_INTER2_NORES   = 14,  /* ref2, mv = pred,   no residual */
    MODE_INTRA4         = 15,
    MODE_INTER0D_RES    = 16,  /* ref0, delta,       residual */
    MODE_INTER1D_RES    = 17,  /* ref1, delta,       residual */
    MODE_INTER2D_RES    = 18,  /* ref2, delta,       residual */
    MODE_INTER1_RES     = 20,  /* ref1, mv = pred,   residual */
    MODE_INTER2_RES     = 21,  /* ref2, mv = pred,   residual */
    MODE_NOTILE_RES     = 22,  /* intra DC, residual */
    MODE_NOTILE_NORES   = 11,  /* intra DC, no residual */
};

typedef struct MV { int x, y; } MV;
typedef struct MBlock { int x, y, w, h; } MBlock;

typedef struct VXEncContext {
    const AVClass *class;
    int quantizer;               /* option: 12..161 (global QP / CQP) */
    int keyint;                  /* option: keyframe interval */
    int me_range;                /* option: motion search range (luma px) */

    uint16_t qtab[3];
    double   lambda;             /* RD Lagrange multiplier (from qtab) */

    int width, height;

    uint8_t *rec[3];             /* current reconstruction (as the decoder produces) */
    uint8_t *goal[3];            /* target VX-YUV planes from the input image */
    int stride[3];

    /* up to three previous reconstructed frames (ref0 = most recent).
     * refplane[r][p] mirrors rec[p]'s layout exactly. */
    uint8_t *refplane[3][3];
    int      nref;               /* how many references are populated */

    MV  *vectors;                /* per-16x16 MV predictor grid (mirrors decoder) */
    int  vectors_stride;

    uint8_t *coeff_y;  int coeff_y_stride;  int coeff_y_size;
    uint8_t *coeff_uv; int coeff_uv_stride; int coeff_uv_size;

    /* scratch for RD save/restore */
    uint8_t *save_coeff_y, *save_coeff_uv;

    /* rate control */
    int64_t  target_bits;        /* per-frame bit budget, 0 = CQP */
    int      rc_locked;          /* quantizer chosen yet? */
    int      lookahead;          /* frames buffered before the QP is locked */
    int      emit_extradata;     /* attach the global QP to the next packet */

    /* lookahead FIFO of input frames (RC only) */
    AVFrame **fifo;
    int       fifo_len;
    int       fifo_cap;
    int       draining;          /* EOF reached, flushing the FIFO */

    int64_t  frame_index;

    PutBitContext pb;
} VXEncContext;

/* ---- plane access (plane 0 = Y full res, 1/2 = U/V half res) ---- */

static inline int rec_get(VXEncContext *s, int plane, int x, int y)
{
    int step = plane ? 2 : 1;
    return s->rec[plane][(y / step) * s->stride[plane] + (x / step)];
}
static inline void rec_set(VXEncContext *s, int plane, int x, int y, int v)
{
    int step = plane ? 2 : 1;
    s->rec[plane][(y / step) * s->stride[plane] + (x / step)] = v;
}
static inline int goal_get(VXEncContext *s, int plane, int x, int y)
{
    int step = plane ? 2 : 1;
    return s->goal[plane][(y / step) * s->stride[plane] + (x / step)];
}

/* reference pixel fetch, byte-for-byte identical to the decoder's px_get so
 * motion compensation reconstructs the same pixels the decoder will. */
static inline int ref_px(VXEncContext *s, int r, int plane, int x, int y)
{
    int step = plane ? 2 : 1;
    int cw = s->width / step, ch = s->height / step;
    int cx = (x < 0) ? -1 : x / step;
    int cy = (y < 0) ? -1 : y / step;
    if (cx < 0) cx += cw;
    if (cy < 0) cy += ch;
    return s->refplane[r][plane][cy * s->stride[plane] + cx];
}

static inline int coeff_y_get(VXEncContext *s, int x, int y)
{
    int cx = (x < 0) ? -1 : x / 4, cy = (y < 0) ? -1 : y / 4;
    return s->coeff_y[(cy + 1) * s->coeff_y_stride + (cx + 1)];
}
static inline void coeff_y_set(VXEncContext *s, int x, int y, int v)
{
    s->coeff_y[(y / 4 + 1) * s->coeff_y_stride + (x / 4 + 1)] = v;
}
static inline int coeff_uv_get(VXEncContext *s, int x, int y)
{
    int cx = (x < 0) ? -1 : x / 8, cy = (y < 0) ? -1 : y / 8;
    return s->coeff_uv[(cy + 1) * s->coeff_uv_stride + (cx + 1)];
}
static inline void coeff_uv_set(VXEncContext *s, int x, int y, int v)
{
    s->coeff_uv[(y / 8 + 1) * s->coeff_uv_stride + (x / 8 + 1)] = v;
}

/* ---- DC prediction into the reconstruction buffer (matches decoder) ---- */

static void predict_dc(VXEncContext *s, MBlock b, int plane)
{
    int dc = 128, step = plane ? 2 : 1;
    if (b.x != 0 && b.y != 0) {
        int sx = b.w / 2, sy = b.h / 2;
        for (int i = 0; i < b.w; i++) sx += rec_get(s, plane, b.x + i, b.y - 1);
        for (int i = 0; i < b.h; i++) sy += rec_get(s, plane, b.x - 1, b.y + i);
        dc = ((sx / b.w) + (sy / b.h) + 1) / 2;
    } else if (b.x == 0 && b.y != 0) {
        int sx = b.w / 2;
        for (int i = 0; i < b.w; i++) sx += rec_get(s, plane, b.x + i, b.y - 1);
        dc = sx / b.w;
    } else if (b.x != 0 && b.y == 0) {
        int sy = b.h / 2;
        for (int i = 0; i < b.h; i++) sy += rec_get(s, plane, b.x - 1, b.y + i);
        dc = sy / b.h;
    }
    for (int j = 0; j < b.h; j += step)
        for (int i = 0; i < b.w; i += step)
            rec_set(s, plane, b.x + i, b.y + j, dc);
}

/* ---- forward transform: residual (goal - rec) -> quantized levels ---- */

static int encode_dct(VXEncContext *s, int x, int y, int plane, int level[16])
{
    int step = plane ? 2 : 1;
    int residu[4][4];
    double avg[16];
    int any = 0;

    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            residu[j][i] = goal_get(s, plane, x + i * step, y + j * step) -
                           rec_get (s, plane, x + i * step, y + j * step);

    for (int f = 0; f < 16; f++) {
        int yres = f % 4, xres = f / 4;
        double acc = 0;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                acc += residu[j][i] * dct_gradient[yres][j] * dct_gradient[xres][i];
        acc /= 16.0;
        acc *= 64.0 / s->qtab[(yres & 1) + (xres & 1)];
        avg[f] = acc;
    }

    for (int i = 0; i < 16; i++) {
        level[i] = (int)lrint(avg[zigzag_scan[i]]);
        if (level[i]) any = 1;
    }
    return any;
}

/* ---- inverse transform: add levels back to reconstruction (matches decoder) ---- */

static void idct_add(VXEncContext *s, int x, int y, int plane, const int *lvl)
{
    int dct[16], step = plane ? 2 : 1;

    for (int i = 0; i < 16; i++)
        dct[zigzag_scan[i]] = lvl[i] * s->qtab[(zigzag_scan[i] & 1) + ((zigzag_scan[i] >> 2) & 1)];

    dct[0] += 1 << 5;

    for (int i = 0; i < 4; i++) {
        int z0 = dct[i+4*0] + dct[i+4*2];
        int z1 = dct[i+4*0] - dct[i+4*2];
        int z2 = (dct[i+4*1] >> 1) - dct[i+4*3];
        int z3 = dct[i+4*1] + (dct[i+4*3] >> 1);
        dct[i+4*0] = z0 + z3; dct[i+4*1] = z1 + z2;
        dct[i+4*2] = z1 - z2; dct[i+4*3] = z0 - z3;
    }
    for (int i = 0; i < 4; i++) {
        int z0 = dct[0+4*i] + dct[2+4*i];
        int z1 = dct[0+4*i] - dct[2+4*i];
        int z2 = (dct[1+4*i] >> 1) - dct[3+4*i];
        int z3 = dct[1+4*i] + (dct[3+4*i] >> 1);
        rec_set(s, plane, x+step*i, y+step*0, av_clip_uint8(rec_get(s, plane, x+step*i, y+step*0) + ((z0+z3) >> 6)));
        rec_set(s, plane, x+step*i, y+step*1, av_clip_uint8(rec_get(s, plane, x+step*i, y+step*1) + ((z1+z2) >> 6)));
        rec_set(s, plane, x+step*i, y+step*2, av_clip_uint8(rec_get(s, plane, x+step*i, y+step*2) + ((z1-z2) >> 6)));
        rec_set(s, plane, x+step*i, y+step*3, av_clip_uint8(rec_get(s, plane, x+step*i, y+step*3) + ((z0-z3) >> 6)));
    }
}

/* ---- CAVLC symbol writers ---- */

static inline void put_vlc(PutBitContext *pb, const int8_t *len, const int8_t *bits, int sym)
{
    put_bits(pb, len[sym], bits[sym]);
}

/* Encode one 4x4 residual (level[] in zigzag order); returns total_coeff. */
static int encode_residu_cavlc(VXEncContext *s, const int level_in[16], int nc, int plane)
{
    PutBitContext *pb = &s->pb;
    int level[16];
    int n = 16;

    memcpy(level, level_in, sizeof(level));

    while (n > 0 && level[n - 1] == 0) n--;

    if (n == 0) {
        int tab = coeff_token_table_index[nc];
        put_vlc(pb, vx_coeff_token_len[tab], vx_coeff_token_bits[tab], 0);
        return 0;
    }

    int total_coeff = n;

    int runs[16], nruns = 0;
    int zeros_left = 0;
    {
        int nz[16], nnz = 0;
        int cur_run = 0;
        for (int i = n - 1; i >= 0; i--) {
            if (i < n - 1 && level[i] == 0) {
                cur_run++;
                zeros_left++;
            } else if (i < n - 1) {
                runs[nruns++] = cur_run;
                cur_run = 0;
                nz[nnz++] = level[i];
            } else {
                nz[nnz++] = level[i];
            }
        }
        for (int i = 0; i < nnz; i++) level[i] = nz[i];
        total_coeff = nnz;
        n = nnz;
    }

    int trailing_ones = 0, t1sign[3];
    while (trailing_ones < 3 && trailing_ones < total_coeff &&
           abs(level[trailing_ones]) == 1) {
        t1sign[trailing_ones] = level[trailing_ones] < 0 ? 1 : 0;
        trailing_ones++;
    }

    int coeff_token_sym = (total_coeff << 2) + trailing_ones;
    {
        int tab = coeff_token_table_index[nc];
        put_vlc(pb, vx_coeff_token_len[tab], vx_coeff_token_bits[tab], coeff_token_sym);
    }

    if (total_coeff != 16)
        put_vlc(pb, vx_total_zeros_tabs[total_coeff].len,
                    vx_total_zeros_tabs[total_coeff].bits, zeros_left);

    int run_idx = 0;
    int suffix_length = 0;
    int zl = zeros_left;
    for (int i = 0; i < total_coeff; i++) {
        if (i < trailing_ones) {
            put_bits(pb, 1, t1sign[i]);
        } else {
            int lc = level[i];
            int signbit = lc < 0 ? 1 : 0;
            int mag = abs(lc) - 1;
            int level_prefix = FFMIN(mag >> suffix_length, 15);
            int real_suffix_length = (level_prefix == 15) ? 11 : suffix_length;
            int level_suffix = mag - (level_prefix << suffix_length);

            for (int k = 0; k < level_prefix; k++) put_bits(pb, 1, 0);
            put_bits(pb, 1, 1);
            if (real_suffix_length)
                put_bits(pb, real_suffix_length, level_suffix);
            put_bits(pb, 1, signbit);

            if (abs(lc) > cavlc_suffix_limit[suffix_length + 1])
                suffix_length++;
        }

        if (i == total_coeff - 1)
            break;
        if (zl == 0)
            continue;

        int run_before = runs[run_idx++];
        if (zl < 7)
            put_vlc(pb, vx_run_tabs[zl].len, vx_run_tabs[zl].bits, run_before);
        else
            put_vlc(pb, vx_run7_len, vx_run7_bits, run_before);
        zl -= run_before;
    }

    return total_coeff;
}

/* ---- 8x8 residual block (4 luma 4x4 + interleaved U/V) ---- */

static int residu_mask_index(int mask)
{
    for (int i = 0; i < 32; i++)
        if (residu_mask_new_tab[i] == mask)
            return i;
    return 0;
}

/* Returns 1 if the block has any residual worth coding. */
static int encode_residu_block(VXEncContext *s, int x, int y, int write)
{
    int ly[4][16], lu[16], lv[16];
    int m0, m1, m2, m3, mu, mv, mask;

    m0 = encode_dct(s, x  , y  , 0, ly[0]);
    m1 = encode_dct(s, x+4, y  , 0, ly[1]);
    m2 = encode_dct(s, x  , y+4, 0, ly[2]);
    m3 = encode_dct(s, x+4, y+4, 0, ly[3]);
    mu = encode_dct(s, x, y, 1, lu);
    mv = encode_dct(s, x, y, 2, lv);

    mask = m0 * 1 + m1 * 2 + m2 * 4 + m3 * 8 + ((mu || mv) ? 16 : 0);

    if (!write)
        return mask != 0;

    idct_add(s, x  , y  , 0, ly[0]);
    idct_add(s, x+4, y  , 0, ly[1]);
    idct_add(s, x  , y+4, 0, ly[2]);
    idct_add(s, x+4, y+4, 0, ly[3]);
    idct_add(s, x, y, 1, lu);
    idct_add(s, x, y, 2, lv);

    set_ue_golomb(&s->pb, residu_mask_index(mask));

    if (mask & 1) {
        int nc = (coeff_y_get(s, x-1, y) + coeff_y_get(s, x, y-1) + 1) / 2;
        coeff_y_set(s, x, y, encode_residu_cavlc(s, ly[0], nc, 0));
    } else coeff_y_set(s, x, y, 0);

    if (mask & 2) {
        int nc = (coeff_y_get(s, x+4-1, y) + coeff_y_get(s, x+4, y-1) + 1) / 2;
        coeff_y_set(s, x+4, y, encode_residu_cavlc(s, ly[1], nc, 0));
    } else coeff_y_set(s, x+4, y, 0);

    if (mask & 4) {
        int nc = (coeff_y_get(s, x-1, y+4) + coeff_y_get(s, x, y+4-1) + 1) / 2;
        coeff_y_set(s, x, y+4, encode_residu_cavlc(s, ly[2], nc, 0));
    } else coeff_y_set(s, x, y+4, 0);

    if (mask & 8) {
        int nc = (coeff_y_get(s, x+4-1, y+4) + coeff_y_get(s, x+4, y+4-1) + 1) / 2;
        coeff_y_set(s, x+4, y+4, encode_residu_cavlc(s, ly[3], nc, 0));
    } else coeff_y_set(s, x+4, y+4, 0);

    if (mask & 16) {
        int nc = (coeff_uv_get(s, x-1, y) + coeff_uv_get(s, x, y-1) + 1) / 2;
        int cu = encode_residu_cavlc(s, lu, nc, 1);
        int cv = encode_residu_cavlc(s, lv, nc, 2);
        coeff_uv_set(s, x, y, (cu + cv + 1) / 2);
    } else coeff_uv_set(s, x, y, 0);

    return 1;
}

static void clear_total_coeff(VXEncContext *s, MBlock b)
{
    for (int y = 0; y < b.h; y += 8)
        for (int x = 0; x < b.w; x += 8) {
            coeff_y_set(s, b.x+x, b.y+y, 0);
            coeff_y_set(s, b.x+x, b.y+y+4, 0);
            coeff_y_set(s, b.x+x+4, b.y+y, 0);
            coeff_y_set(s, b.x+x+4, b.y+y+4, 0);
            coeff_uv_set(s, b.x+x, b.y+y, 0);
        }
}

static MBlock half_left(MBlock b)  { return (MBlock){ b.x, b.y, b.w/2, b.h }; }
static MBlock half_right(MBlock b) { return (MBlock){ b.x + b.w/2, b.y, b.w/2, b.h }; }
static MBlock half_up(MBlock b)    { return (MBlock){ b.x, b.y, b.w, b.h/2 }; }
static MBlock half_down(MBlock b)  { return (MBlock){ b.x, b.y + b.h/2, b.w, b.h/2 }; }

/* ---- intra macroblock (recursive split to 8x8, DC-predicted) ---- */

static void encode_mb_intra(VXEncContext *s, MBlock b)
{
    if (b.w > b.h) {                     /* v-split */
        set_ue_golomb(&s->pb, MODE_VSPLIT_NORES);
        encode_mb_intra(s, half_left(b));
        encode_mb_intra(s, half_right(b));
        if (b.w == 8 && b.h >= 8)
            clear_total_coeff(s, b);
        return;
    }
    if (b.h > 8) {                       /* h-split */
        set_ue_golomb(&s->pb, MODE_HSPLIT_NORES);
        encode_mb_intra(s, half_up(b));
        encode_mb_intra(s, half_down(b));
        if (b.w >= 8 && b.h == 8)
            clear_total_coeff(s, b);
        return;
    }

    /* 8x8 leaf: DC-predict each plane, then optionally code the residual */
    predict_dc(s, b, 0);
    predict_dc(s, b, 1);
    predict_dc(s, b, 2);

    if (encode_residu_block(s, b.x, b.y, 0)) {
        set_ue_golomb(&s->pb, MODE_NOTILE_RES);
        set_ue_golomb(&s->pb, 2);        /* Y: DC */
        set_ue_golomb(&s->pb, 0);        /* UV: DC */
        encode_residu_block(s, b.x, b.y, 1);
    } else {
        set_ue_golomb(&s->pb, MODE_NOTILE_NORES);
        set_ue_golomb(&s->pb, 2);
        set_ue_golomb(&s->pb, 0);
    }
}

/* ---- motion estimation / compensation (16x16, whole macroblock) ---- */

static inline int mv_in_bounds(VXEncContext *s, MBlock b, MV v)
{
    return !(b.x + v.x < 0 || b.x + v.x + b.w > s->width ||
             b.y + v.y < 0 || b.y + v.y + b.h > s->height);
}

static int mc_sad_luma(VXEncContext *s, MBlock b, MV v, int r)
{
    int sad = 0;
    for (int j = 0; j < b.h; j++)
        for (int i = 0; i < b.w; i++)
            sad += abs(goal_get(s, 0, b.x + i, b.y + j) -
                       ref_px(s, r, 0, b.x + i + v.x, b.y + j + v.y));
    return sad;
}

/* diamond search around the predictor + zero MV; returns best MV, sets *sad. */
static MV motion_search(VXEncContext *s, MBlock b, MV pred, int r, int *out_sad)
{
    static const int dx4[4] = {  1, -1,  0,  0 };
    static const int dy4[4] = {  0,  0,  1, -1 };
    MV cand[2] = { pred, { 0, 0 } };
    MV best = { 0, 0 };
    int best_sad = INT_MAX, have = 0;

    for (int c = 0; c < 2; c++) {
        if (!mv_in_bounds(s, b, cand[c]))
            continue;
        int sad = mc_sad_luma(s, b, cand[c], r);
        if (!have || sad < best_sad) { best = cand[c]; best_sad = sad; have = 1; }
    }
    if (!have) { *out_sad = INT_MAX; return best; }

    for (int step = FFMAX(1, s->me_range); step >= 1; step >>= 1) {
        int improved = 1;
        while (improved) {
            improved = 0;
            for (int d = 0; d < 4; d++) {
                MV t = { best.x + dx4[d] * step, best.y + dy4[d] * step };
                if (!mv_in_bounds(s, b, t))
                    continue;
                int sad = mc_sad_luma(s, b, t, r);
                if (sad < best_sad) { best = t; best_sad = sad; improved = 1; }
            }
        }
    }
    *out_sad = best_sad;
    return best;
}

/* Build the motion-compensated prediction into rec[] (mirrors predict_inter)
 * and record the MV into the predictor grid. */
static void mc_predict(VXEncContext *s, MBlock b, MV v, int r)
{
    for (int plane = 0; plane < 3; plane++) {
        int step = plane ? 2 : 1;
        for (int j = 0; j < b.h; j += step)
            for (int i = 0; i < b.w; i += step)
                rec_set(s, plane, b.x + i, b.y + j,
                        ref_px(s, r, plane, b.x + i + v.x, b.y + j + v.y));
    }
    s->vectors[(b.y / 16 + 1) * s->vectors_stride + (b.x / 16 + 1)] = v;
}

/* Emit one inter macroblock (16x16): mode word, MV delta if any, residual if
 * requested. rec[] and the predictor grid are updated to the decoded result. */
static void encode_inter_mb(VXEncContext *s, MBlock b, int r, MV mv, MV pred,
                            int residual)
{
    static const int mode_tab[2][2][3] = {
        /* [residual][delta][ref] */
        { { MODE_INTER0_NORES,  MODE_INTER1_NORES,  MODE_INTER2_NORES  },
          { MODE_INTER0D_NORES, MODE_INTER1D_NORES, MODE_INTER2D_NORES } },
        { { MODE_INTER0_RES,    MODE_INTER1_RES,    MODE_INTER2_RES    },
          { MODE_INTER0D_RES,   MODE_INTER1D_RES,   MODE_INTER2D_RES   } },
    };
    int delta = (mv.x != pred.x) || (mv.y != pred.y);
    int mode = mode_tab[!!residual][delta][r];

    set_ue_golomb(&s->pb, mode);
    if (delta) {
        set_se_golomb(&s->pb, mv.x - pred.x);
        set_se_golomb(&s->pb, mv.y - pred.y);
    }

    mc_predict(s, b, mv, r);

    if (residual) {
        for (int dy = 0; dy < b.h; dy += 8)
            for (int dx = 0; dx < b.w; dx += 8)
                encode_residu_block(s, b.x + dx, b.y + dy, 1);
    } else {
        clear_total_coeff(s, b);
    }
}

/* ---- per-macroblock RD save / restore (a 16x16 MB touches only its own
 * reconstruction region, coeff cells, one predictor-grid entry and the bit
 * writer). ---- */

typedef struct MBSave {
    PutBitContext pb;
    MV vec;
    uint8_t rec_y[16*16];
    uint8_t rec_u[8*8];
    uint8_t rec_v[8*8];
} MBSave;

static void mb_save(VXEncContext *s, MBlock b, MBSave *sv)
{
    sv->pb  = s->pb;
    sv->vec = s->vectors[(b.y / 16 + 1) * s->vectors_stride + (b.x / 16 + 1)];
    for (int j = 0; j < 16; j++)
        memcpy(sv->rec_y + j*16, s->rec[0] + (b.y+j)*s->stride[0] + b.x, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(sv->rec_u + j*8, s->rec[1] + (b.y/2+j)*s->stride[1] + b.x/2, 8);
        memcpy(sv->rec_v + j*8, s->rec[2] + (b.y/2+j)*s->stride[2] + b.x/2, 8);
    }
    memcpy(s->save_coeff_y,  s->coeff_y,  s->coeff_y_size);
    memcpy(s->save_coeff_uv, s->coeff_uv, s->coeff_uv_size);
}

static void mb_restore(VXEncContext *s, MBlock b, const MBSave *sv)
{
    s->pb = sv->pb;
    s->vectors[(b.y / 16 + 1) * s->vectors_stride + (b.x / 16 + 1)] = sv->vec;
    for (int j = 0; j < 16; j++)
        memcpy(s->rec[0] + (b.y+j)*s->stride[0] + b.x, sv->rec_y + j*16, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(s->rec[1] + (b.y/2+j)*s->stride[1] + b.x/2, sv->rec_u + j*8, 8);
        memcpy(s->rec[2] + (b.y/2+j)*s->stride[2] + b.x/2, sv->rec_v + j*8, 8);
    }
    memcpy(s->coeff_y,  s->save_coeff_y,  s->coeff_y_size);
    memcpy(s->coeff_uv, s->save_coeff_uv, s->coeff_uv_size);
}

static int64_t mb_ssd(VXEncContext *s, MBlock b)
{
    int64_t d = 0;
    for (int plane = 0; plane < 3; plane++) {
        int step = plane ? 2 : 1;
        for (int j = 0; j < b.h; j += step)
            for (int i = 0; i < b.w; i += step) {
                int e = rec_get(s, plane, b.x+i, b.y+j) - goal_get(s, plane, b.x+i, b.y+j);
                d += (int64_t)e * e;
            }
    }
    return d;
}

/* Encode a candidate (bounded by save/restore by the caller), returning the RD
 * cost J = SSD + lambda * bits. Leaves the encoder state holding the candidate. */
static double try_candidate_inter(VXEncContext *s, MBlock b, int r, MV mv, MV pred,
                                  int residual)
{
    int bits0 = put_bits_count(&s->pb);
    encode_inter_mb(s, b, r, mv, pred, residual);
    int bits = put_bits_count(&s->pb) - bits0;
    return (double)mb_ssd(s, b) + s->lambda * bits;
}

static double try_candidate_intra(VXEncContext *s, MBlock b)
{
    int bits0 = put_bits_count(&s->pb);
    encode_mb_intra(s, b);
    int bits = put_bits_count(&s->pb) - bits0;
    return (double)mb_ssd(s, b) + s->lambda * bits;
}

/* Decide + emit one 16x16 macroblock of a P-frame. */
static void encode_pframe_mb(VXEncContext *s, MBlock b, MV pred)
{
    struct { int r; MV mv; int residual; } best = { 0, { 0, 0 }, 0 };
    double best_J = DBL_MAX;
    int have_inter = 0;
    MBSave sv;

    mb_save(s, b, &sv);

    /* inter candidates: best MV per available reference, with and without
     * residual (a skip is just the no-residual case when MV == predictor). */
    for (int r = 0; r < s->nref; r++) {
        int sad;
        MV mv = motion_search(s, b, pred, r, &sad);
        if (sad == INT_MAX)
            continue;
        for (int res = 0; res < 2; res++) {
            double J = try_candidate_inter(s, b, r, mv, pred, res);
            mb_restore(s, b, &sv);
            if (J < best_J) {
                best_J = J;
                best.r = r; best.mv = mv; best.residual = res;
                have_inter = 1;
            }
        }
    }

    /* intra candidate */
    {
        double J = try_candidate_intra(s, b);
        mb_restore(s, b, &sv);
        if (!have_inter || J < best_J) {
            encode_mb_intra(s, b);   /* commit intra */
            return;
        }
    }

    encode_inter_mb(s, b, best.r, best.mv, pred, best.residual);
}

/* ---- whole-frame encoders ---- */

static void reset_frame_state(VXEncContext *s)
{
    memset(s->rec[0], 0, s->stride[0] * s->height);
    memset(s->rec[1], 0, s->stride[1] * (s->height/2));
    memset(s->rec[2], 0, s->stride[2] * (s->height/2));
    memset(s->coeff_y,  0, s->coeff_y_size);
    memset(s->coeff_uv, 0, s->coeff_uv_size);
    memset(s->vectors, 0, (size_t)(s->height/16 + 1) * s->vectors_stride * sizeof(*s->vectors));
}

static int mid_pred3(int a, int b, int c)
{
    return FFMAX(FFMIN(a, b), FFMIN(FFMAX(a, b), c));
}

static void encode_iframe(VXEncContext *s)
{
    for (int y = 0; y < s->height; y += 16)
        for (int x = 0; x < s->width; x += 16)
            encode_mb_intra(s, (MBlock){ x, y, 16, 16 });
}

static void encode_pframe(VXEncContext *s)
{
    for (int y = 0; y < s->height; y += 16) {
        for (int x = 0; x < s->width; x += 16) {
            int by = y / 16, bx = x / 16;
            MV left  = s->vectors[(by + 1) * s->vectors_stride + (bx + 0)];
            MV above = s->vectors[(by + 0) * s->vectors_stride + (bx + 1)];
            MV aright = s->vectors[(by + 0) * s->vectors_stride + (bx + 2)];
            MV pred = {
                mid_pred3(left.x, above.x, aright.x),
                mid_pred3(left.y, above.y, aright.y),
            };
            encode_pframe_mb(s, (MBlock){ x, y, 16, 16 }, pred);
        }
    }
}

/* ---- RGB24 -> VX-YUV target planes ---- */

static void rgb24_to_goal(VXEncContext *s, const AVFrame *frame)
{
    int w = s->width, h = s->height;

    for (int y = 0; y < h; y++) {
        const uint8_t *row = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
        for (int x = 0; x < w; x++) {
            int r = row[x*3+0], g = row[x*3+1], b = row[x*3+2];
            int yy = (2*r + 4*g + b + 3) / 7;
            s->goal[0][y * s->stride[0] + x] = av_clip_uint8(yy);
        }
    }
    for (int cy = 0; cy < h/2; cy++) {
        for (int cx = 0; cx < w/2; cx++) {
            int su = 0, sv = 0;
            for (int dy = 0; dy < 2; dy++) {
                const uint8_t *row = frame->data[0] + (ptrdiff_t)(cy*2+dy) * frame->linesize[0];
                for (int dx = 0; dx < 2; dx++) {
                    int r = row[(cx*2+dx)*3+0], g = row[(cx*2+dx)*3+1], b = row[(cx*2+dx)*3+2];
                    int yy = (2*r + 4*g + b + 3) / 7;
                    su += (b - yy) / 2 + 128;
                    sv += (r - yy) / 2 + 128;
                }
            }
            s->goal[1][cy * s->stride[1] + cx] = av_clip_uint8((su + 2) / 4);
            s->goal[2][cy * s->stride[2] + cx] = av_clip_uint8((sv + 2) / 4);
        }
    }
}

/* ---- reference-frame ring ---- */

static void set_qtab(VXEncContext *s, int q)
{
    int qx = q % 6, qy = q / 6;
    double qstep;
    for (int i = 0; i < 3; i++)
        s->qtab[i] = quant4x4_tab[qx][i] << qy;
    /* SSD-domain Lagrange multiplier J = SSD + lambda*bits. A residual level of
     * 1 reconstructs to ~qtab/64 in the pixel domain, so the effective quant
     * step is qtab[0]/64; H.264's lambda ~= 0.85*qstep^2 for SSD carries over. */
    qstep = s->qtab[0] / 64.0;
    s->lambda = 0.85 * qstep * qstep;
}

static void push_reference(VXEncContext *s)
{
    /* shift ref2 <- ref1 <- ref0, then copy the freshly reconstructed frame
     * into ref0 (ref0 = most recent, exactly the decoder's pic ring). */
    uint8_t *tmp[3];
    for (int p = 0; p < 3; p++) tmp[p] = s->refplane[2][p];
    for (int r = 2; r > 0; r--)
        for (int p = 0; p < 3; p++)
            s->refplane[r][p] = s->refplane[r-1][p];
    for (int p = 0; p < 3; p++) {
        s->refplane[0][p] = tmp[p];
        int h = p ? s->height/2 : s->height;
        memcpy(s->refplane[0][p], s->rec[p], (size_t)s->stride[p] * h);
    }
    if (s->nref < 3)
        s->nref++;
}

/* Encode one already-loaded frame (goal[] set) into a bitstream and return the
 * byte count. When commit is set, the reconstruction is pushed as a reference
 * and the on-disk byte-swap is applied to buf; otherwise the frame is encoded
 * only to measure its size (state still advances so references stay coherent). */
static int encode_frame_bits(VXEncContext *s, uint8_t *buf, int cap, int is_key,
                             int commit)
{
    int nbytes;
    reset_frame_state(s);
    init_put_bits(&s->pb, buf, cap);
    if (is_key) encode_iframe(s);
    else        encode_pframe(s);
    while (put_bits_count(&s->pb) % 16)
        put_bits(&s->pb, 1, 0);
    flush_put_bits(&s->pb);
    nbytes = put_bits_count(&s->pb) / 8;
    push_reference(s);
    if (commit)
        for (int i = 0; i + 1 < nbytes; i += 2)
            FFSWAP(uint8_t, buf[i], buf[i + 1]);
    return nbytes;
}

/* ---- rate control: pick the single global quantizer from a target rate ---- */

static void rc_reset_refs(VXEncContext *s)
{
    s->nref = 0;
}

/* Probe the buffered lookahead window (real I + P encoding) at a ladder of
 * quantizers and lock the one whose average frame size is closest to budget.
 * Only the low end of 12..161 is useful: each +6 doubles the quant tables, so
 * past ~q40 every residual (DC included) quantizes to zero and the picture
 * collapses toward flat grey. Retail Kirby is q28. */
static int probe_quantizer(VXEncContext *s, AVCodecContext *avctx)
{
    static const int ladder[] = { 12, 16, 20, 24, 28, 32, 36, 40 };
    int cap = s->width * s->height * 4 + 1024;
    uint8_t *scratch = av_malloc(cap);
    int chosen = s->quantizer, n = s->fifo_len;
    double best_err = DBL_MAX;

    if (!scratch || n <= 0)
        return av_clip(s->quantizer, 12, 40);

    for (int i = 0; i < FF_ARRAY_ELEMS(ladder); i++) {
        int q = ladder[i];
        int64_t total = 0;
        double avg, err;

        set_qtab(s, q);
        rc_reset_refs(s);
        for (int f = 0; f < n; f++) {
            rgb24_to_goal(s, s->fifo[f]);
            total += encode_frame_bits(s, scratch, cap, f == 0, 0) * 8;
        }
        avg = (double)total / n;
        err = fabs(avg - (double)s->target_bits);
        if (err < best_err) { best_err = err; chosen = q; }
    }

    av_free(scratch);
    rc_reset_refs(s);
    return av_clip(chosen, 12, 40);
}

/* Encode one buffered/live frame straight into an output packet. */
static int emit_frame(VXEncContext *s, AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame)
{
    int max_bytes = s->width * s->height * 4 + 1024;
    int is_key = (s->frame_index % s->keyint) == 0 || s->nref == 0;
    int ret, nbytes;

    rgb24_to_goal(s, frame);

    if ((ret = ff_get_encode_buffer(avctx, pkt, max_bytes, 0)) < 0)
        return ret;

    nbytes = encode_frame_bits(s, pkt->data, max_bytes, is_key, 1);
    av_shrink_packet(pkt, nbytes);

    if (is_key)
        pkt->flags |= AV_PKT_FLAG_KEY;

    /* hand the (RC-chosen) global quantizer to the muxer for the file header */
    if (s->emit_extradata) {
        uint8_t *sd = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, 4);
        if (sd)
            AV_WL32(sd, s->quantizer);
        s->emit_extradata = 0;
    }

    s->frame_index++;
    return 0;
}

/* ---- lifecycle ---- */

static av_cold int vxenc_init(AVCodecContext *avctx)
{
    VXEncContext *s = avctx->priv_data;

    if (avctx->width % 16 || avctx->height % 16) {
        av_log(avctx, AV_LOG_ERROR, "dimensions must be multiples of 16\n");
        return AVERROR(EINVAL);
    }
    if (s->quantizer < 12 || s->quantizer > 161) {
        av_log(avctx, AV_LOG_ERROR, "quantizer must be in 12..161\n");
        return AVERROR(EINVAL);
    }
    s->width  = avctx->width;
    s->height = avctx->height;

    if (avctx->bit_rate > 0) {
        AVRational fr = avctx->framerate.num ? avctx->framerate : (AVRational){ 30, 1 };
        s->target_bits = av_rescale(avctx->bit_rate, fr.den, fr.num);
        if (s->target_bits < 1)
            s->target_bits = 1;
        /* buffer several GOPs so the quantizer probe samples real inter
         * behaviour across more than just the (often atypical) opening scene. */
        s->lookahead = av_clip(FFMAX(s->keyint * 4, 32), 1, 96);
        s->fifo_cap = s->lookahead + 2;
        s->fifo = av_calloc(s->fifo_cap, sizeof(*s->fifo));
        if (!s->fifo)
            return AVERROR(ENOMEM);
    }

    set_qtab(s, s->quantizer);

    s->stride[0] = s->width;
    s->stride[1] = s->stride[2] = s->width / 2;
    s->rec[0]  = av_malloc(s->stride[0] * s->height);
    s->rec[1]  = av_malloc(s->stride[1] * (s->height/2));
    s->rec[2]  = av_malloc(s->stride[2] * (s->height/2));
    s->goal[0] = av_malloc(s->stride[0] * s->height);
    s->goal[1] = av_malloc(s->stride[1] * (s->height/2));
    s->goal[2] = av_malloc(s->stride[2] * (s->height/2));
    for (int r = 0; r < 3; r++) {
        s->refplane[r][0] = av_malloc(s->stride[0] * s->height);
        s->refplane[r][1] = av_malloc(s->stride[1] * (s->height/2));
        s->refplane[r][2] = av_malloc(s->stride[2] * (s->height/2));
        if (!s->refplane[r][0] || !s->refplane[r][1] || !s->refplane[r][2])
            return AVERROR(ENOMEM);
    }
    s->coeff_y_stride  = s->width / 4 + 1;
    s->coeff_uv_stride = s->width / 8 + 1;
    s->coeff_y_size    = (s->height/4 + 1) * s->coeff_y_stride;
    s->coeff_uv_size   = (s->height/8 + 1) * s->coeff_uv_stride;
    s->coeff_y  = av_malloc(s->coeff_y_size);
    s->coeff_uv = av_malloc(s->coeff_uv_size);
    s->save_coeff_y  = av_malloc(s->coeff_y_size);
    s->save_coeff_uv = av_malloc(s->coeff_uv_size);
    s->vectors_stride = s->width / 16 + 2;
    s->vectors = av_malloc((size_t)(s->height/16 + 1) * s->vectors_stride * sizeof(*s->vectors));
    if (!s->rec[0] || !s->rec[1] || !s->rec[2] || !s->goal[0] || !s->goal[1] ||
        !s->goal[2] || !s->coeff_y || !s->coeff_uv || !s->save_coeff_y ||
        !s->save_coeff_uv || !s->vectors)
        return AVERROR(ENOMEM);

    /* header quantizer (patched by the muxer if RC later picks another) */
    avctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = 4;
    AV_WL32(avctx->extradata, s->quantizer);

    return 0;
}

static void rc_lock(VXEncContext *s, AVCodecContext *avctx)
{
    s->quantizer = probe_quantizer(s, avctx);
    set_qtab(s, s->quantizer);
    AV_WL32(avctx->extradata, s->quantizer);
    s->rc_locked = 1;
    s->emit_extradata = 1;
    av_log(avctx, AV_LOG_INFO,
           "vx rate control: locked global quantizer %d (~%"PRId64" bits/frame target, "
           "%d-frame lookahead)\n", s->quantizer, s->target_bits, s->fifo_len);
}

/* pop the oldest buffered frame and encode it into pkt */
static int emit_oldest(VXEncContext *s, AVCodecContext *avctx, AVPacket *pkt)
{
    AVFrame *f = s->fifo[0];
    int ret = emit_frame(s, avctx, pkt, f);
    av_frame_free(&s->fifo[0]);
    memmove(s->fifo, s->fifo + 1, (--s->fifo_len) * sizeof(*s->fifo));
    return ret;
}

static int vxenc_encode(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *frame, int *got_packet)
{
    VXEncContext *s = avctx->priv_data;
    int ret;

    /* --- constant-quantizer path: straight 1-in/1-out --- */
    if (!s->target_bits) {
        if (!frame) { *got_packet = 0; return 0; }
        if ((ret = emit_frame(s, avctx, pkt, frame)) < 0)
            return ret;
        *got_packet = 1;
        return 0;
    }

    /* --- rate-control path: lookahead buffer, then drain --- */
    if (frame) {
        AVFrame *clone = av_frame_clone(frame);
        if (!clone)
            return AVERROR(ENOMEM);
        s->fifo[s->fifo_len++] = clone;

        if (!s->rc_locked) {
            if (s->fifo_len < s->lookahead) { /* keep buffering */
                *got_packet = 0;
                return 0;
            }
            rc_lock(s, avctx);
        }
        if ((ret = emit_oldest(s, avctx, pkt)) < 0)
            return ret;
        *got_packet = 1;
        return 0;
    }

    /* EOF: lock if we never reached the lookahead target, then drain */
    if (!s->rc_locked && s->fifo_len > 0)
        rc_lock(s, avctx);
    if (s->fifo_len > 0) {
        if ((ret = emit_oldest(s, avctx, pkt)) < 0)
            return ret;
        *got_packet = 1;
        return 0;
    }
    *got_packet = 0;
    return 0;
}

static av_cold int vxenc_close(AVCodecContext *avctx)
{
    VXEncContext *s = avctx->priv_data;
    for (int i = 0; i < 3; i++) {
        av_freep(&s->rec[i]);
        av_freep(&s->goal[i]);
        for (int r = 0; r < 3; r++)
            av_freep(&s->refplane[r][i]);
    }
    av_freep(&s->coeff_y);
    av_freep(&s->coeff_uv);
    av_freep(&s->save_coeff_y);
    av_freep(&s->save_coeff_uv);
    av_freep(&s->vectors);
    if (s->fifo) {
        for (int i = 0; i < s->fifo_len; i++)
            av_frame_free(&s->fifo[i]);
        av_freep(&s->fifo);
    }
    return 0;
}

#define OFFSET(x) offsetof(VXEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "quantizer", "VX global quantizer (12=best quality/largest .. 161=worst/smallest); "
                   "used directly unless a target bit rate is set",
      OFFSET(quantizer), AV_OPT_TYPE_INT, { .i64 = 28 }, 12, 161, VE },
    { "keyint", "maximum keyframe (all-intra frame) interval",
      OFFSET(keyint), AV_OPT_TYPE_INT, { .i64 = 12 }, 1, 600, VE },
    { "me_range", "motion search range in luma pixels",
      OFFSET(me_range), AV_OPT_TYPE_INT, { .i64 = 16 }, 0, 64, VE },
    { NULL },
};

static const AVClass vxenc_class = {
    .class_name = "vx encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_vx_encoder = {
    .p.name         = "vx",
    CODEC_LONG_NAME("ActImagine VX Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_VX,
    .priv_data_size = sizeof(VXEncContext),
    .p.priv_class   = &vxenc_class,
    .init           = vxenc_init,
    FF_CODEC_ENCODE_CB(vxenc_encode),
    .close          = vxenc_close,
    CODEC_PIXFMTS(AV_PIX_FMT_RGB24),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
