/*
 * MobiClip / ActImagine DS (.vx) Video decoder
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
 * Reverse engineered from the actimagine Python reference decoder
 * (https://github.com/CharlesVanEeckhout/actimagine), which documents the
 * ActImagine codec used by the Nintendo DS ".vx" container (as seen in e.g.
 * Mega Man ZX). The residual coding is standard H.264 CAVLC; the VLC tables
 * in vx_cavlc_vlc.h are generated straight from that reference's
 * package/vlc.py so they're guaranteed identical to it (they happen to match
 * ffmpeg's own h264_cavlc.c tables, but are built standalone here rather
 * than reusing h264_cavlc.o, which would drag in the whole H.264 decoder).
 * Everything else (container framing, macroblock partition modes, intra
 * prediction mode signalling, motion vector prediction) is specific to
 * this codec.
 */

#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"

#include "avcodec.h"
#include "bswapdsp.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "golomb.h"
#include "vx.h"
#include "vlc.h"

#define ACTIMAGINE_COEFF_TOKEN_VLC_BITS 8
#define ACTIMAGINE_TOTAL_ZEROS_VLC_BITS 9
#define ACTIMAGINE_RUN_VLC_BITS         3
#define ACTIMAGINE_RUN7_VLC_BITS        6

#include "vx_cavlc_vlc.h"

static const uint8_t coeff_token_table_index[17] = {
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3
};

static VLC vx_coeff_token_vlc[4];
static VLC vx_total_zeros_vlc[16];
static VLC vx_run_vlc[7];
static VLC vx_run7_vlc;

static av_cold void vx_cavlc_vlc_init(void)
{
    for (int i = 0; i < 4; i++)
        vlc_init(&vx_coeff_token_vlc[i], ACTIMAGINE_COEFF_TOKEN_VLC_BITS, 68,
                 vx_coeff_token_len[i], 1, 1,
                 vx_coeff_token_bits[i], 1, 1, 0);
    for (int i = 1; i <= 15; i++)
        vlc_init(&vx_total_zeros_vlc[i], ACTIMAGINE_TOTAL_ZEROS_VLC_BITS, vx_total_zeros_tabs[i].n,
                 vx_total_zeros_tabs[i].len, 1, 1,
                 vx_total_zeros_tabs[i].bits, 1, 1, 0);
    for (int i = 1; i <= 6; i++)
        vlc_init(&vx_run_vlc[i], ACTIMAGINE_RUN_VLC_BITS, vx_run_tabs[i].n,
                 vx_run_tabs[i].len, 1, 1,
                 vx_run_tabs[i].bits, 1, 1, 0);
    vlc_init(&vx_run7_vlc, ACTIMAGINE_RUN7_VLC_BITS, 15,
             vx_run7_len, 1, 1,
             vx_run7_bits, 1, 1, 0);
}

static AVOnce vx_cavlc_once = AV_ONCE_INIT;

static const uint16_t quant4x4_tab[6][3] = {
    { 0x0A, 0x0D, 0x10 },
    { 0x0B, 0x0E, 0x12 },
    { 0x0D, 0x10, 0x14 },
    { 0x0E, 0x12, 0x17 },
    { 0x10, 0x14, 0x19 },
    { 0x12, 0x17, 0x1D },
};

static const uint8_t residu_mask_new_tab[32] = {
    0x00, 0x08, 0x04, 0x02, 0x01, 0x1F, 0x0F, 0x0A,
    0x05, 0x0C, 0x03, 0x10, 0x0E, 0x0D, 0x0B, 0x07,
    0x09, 0x06, 0x1E, 0x1B, 0x1A, 0x1D, 0x17, 0x15,
    0x18, 0x12, 0x11, 0x1C, 0x14, 0x13, 0x16, 0x19,
};

/* level[i] applies to zigzag position i; zigzag_scan[i] gives the raster
 * (row-major within a 4x4 block) position for that zigzag index. This is
 * the "swapped 2bit" zigzag actimagine uses (not the standard H.264 one). */
static const uint8_t zigzag_scan[16] = {
    0*4+0, 1*4+0, 0*4+1, 0*4+2,
    1*4+1, 2*4+0, 3*4+0, 2*4+1,
    1*4+2, 0*4+3, 1*4+3, 2*4+2,
    3*4+1, 3*4+2, 2*4+3, 3*4+3,
};

static const int cavlc_suffix_limit[7] = { 0, 3, 6, 12, 24, 48, 0x8000 };

typedef struct MBlock {
    int x, y, w, h;
} MBlock;

typedef struct MV {
    int x, y;
} MV;

typedef struct VXDecCtx {
    AVCodecContext *avctx;
    GetBitContext *gb;
    int width, height;
    const uint16_t *qtab;

    VXPic dst;
    const VXPic *refs[3];

    uint8_t *coeff_y;
    int coeff_y_stride;
    uint8_t *coeff_uv;
    int coeff_uv_stride;

    MV *vectors;
    int vectors_stride;

    int error;
} VXDecCtx;

static int mid_pred3(int a, int b, int c)
{
    return FFMAX(FFMIN(a, b), FFMIN(FFMAX(a, b), c));
}

static inline int px_get(const uint8_t *data, int linesize, int width, int height,
                          int plane, int x, int y)
{
    int step = plane ? 2 : 1;
    int cw = width / step, ch = height / step;
    int cx = (x < 0) ? -1 : x / step;
    int cy = (y < 0) ? -1 : y / step;
    if (cx < 0) cx += cw;
    if (cy < 0) cy += ch;
    return data[cy * linesize + cx];
}

static inline void px_set(uint8_t *data, int linesize, int plane, int x, int y, int val)
{
    int step = plane ? 2 : 1;
    data[(y / step) * linesize + (x / step)] = val;
}

static inline int dst_get(VXDecCtx *c, int plane, int x, int y)
{
    return px_get(c->dst.data[plane], c->dst.linesize[plane], c->width, c->height, plane, x, y);
}

static inline void dst_set(VXDecCtx *c, int plane, int x, int y, int val)
{
    px_set(c->dst.data[plane], c->dst.linesize[plane], plane, x, y, val);
}

static inline int coeff_y_get(VXDecCtx *c, int x, int y)
{
    int cx = (x < 0) ? -1 : x / 4;
    int cy = (y < 0) ? -1 : y / 4;
    return c->coeff_y[(cy + 1) * c->coeff_y_stride + (cx + 1)];
}

static inline void coeff_y_set(VXDecCtx *c, int x, int y, int val)
{
    c->coeff_y[(y / 4 + 1) * c->coeff_y_stride + (x / 4 + 1)] = val;
}

static inline int coeff_uv_get(VXDecCtx *c, int x, int y)
{
    int cx = (x < 0) ? -1 : x / 8;
    int cy = (y < 0) ? -1 : y / 8;
    return c->coeff_uv[(cy + 1) * c->coeff_uv_stride + (cx + 1)];
}

static inline void coeff_uv_set(VXDecCtx *c, int x, int y, int val)
{
    c->coeff_uv[(y / 8 + 1) * c->coeff_uv_stride + (x / 8 + 1)] = val;
}

/* ---- intra prediction (4x4, matches h264pred.py bit-for-bit) ---- */

static void pred4x4_vertical(VXDecCtx *c, int x, int y)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, dst_get(c, 0, x + i, y - 1));
}

static void pred4x4_horizontal(VXDecCtx *c, int x, int y)
{
    for (int j = 0; j < 4; j++) {
        int v = dst_get(c, 0, x - 1, y + j);
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, v);
    }
}

static void pred4x4_128_dc(VXDecCtx *c, int x, int y)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, 128);
}

static void pred4x4_top_dc(VXDecCtx *c, int x, int y)
{
    int dc = 2;
    for (int i = 0; i < 4; i++)
        dc += dst_get(c, 0, x + i, y - 1);
    dc /= 4;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, dc);
}

static void pred4x4_left_dc(VXDecCtx *c, int x, int y)
{
    int dc = 2;
    for (int j = 0; j < 4; j++)
        dc += dst_get(c, 0, x - 1, y + j);
    dc /= 4;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, dc);
}

static void pred4x4_dc(VXDecCtx *c, int x, int y)
{
    int dc = 4;
    for (int i = 0; i < 4; i++)
        dc += dst_get(c, 0, x + i, y - 1);
    for (int j = 0; j < 4; j++)
        dc += dst_get(c, 0, x - 1, y + j);
    dc /= 8;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, dc);
}

static void pred4x4_down_left(VXDecCtx *c, int x, int y)
{
    int t[8], p[7];
    for (int i = 0; i < 8; i++) t[i] = dst_get(c, 0, x + i, y - 1);
    p[0] = (t[0] + 2*t[1] + t[2] + 2) >> 2;
    p[1] = (t[1] + 2*t[2] + t[3] + 2) >> 2;
    p[2] = (t[2] + 2*t[3] + t[4] + 2) >> 2;
    p[3] = (t[3] + 2*t[4] + t[5] + 2) >> 2;
    p[4] = (t[4] + 2*t[5] + t[6] + 2) >> 2;
    p[5] = (t[5] + 2*t[6] + t[7] + 2) >> 2;
    p[6] = (t[6] + 3*t[7] + 2) >> 2;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[i + j]);
}

static void pred4x4_down_right(VXDecCtx *c, int x, int y)
{
    int l[4], t[4], lt, p[7];
    lt = dst_get(c, 0, x - 1, y - 1);
    for (int i = 0; i < 4; i++) t[i] = dst_get(c, 0, x + i, y - 1);
    for (int j = 0; j < 4; j++) l[j] = dst_get(c, 0, x - 1, y + j);
    p[0] = (l[3] + 2*l[2] + l[1] + 2) >> 2;
    p[1] = (l[2] + 2*l[1] + l[0] + 2) >> 2;
    p[2] = (l[1] + 2*l[0] + lt + 2) >> 2;
    p[3] = (l[0] + 2*lt + t[0] + 2) >> 2;
    p[4] = (lt + 2*t[0] + t[1] + 2) >> 2;
    p[5] = (t[0] + 2*t[1] + t[2] + 2) >> 2;
    p[6] = (t[1] + 2*t[2] + t[3] + 2) >> 2;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[3 + i - j]);
}

static void pred4x4_vertical_right(VXDecCtx *c, int x, int y)
{
    int l[3], t[4], lt, p[10];
    lt = dst_get(c, 0, x - 1, y - 1);
    for (int i = 0; i < 4; i++) t[i] = dst_get(c, 0, x + i, y - 1);
    for (int j = 0; j < 3; j++) l[j] = dst_get(c, 0, x - 1, y + j);
    p[0] = (l[0] + 2*l[1] + l[2] + 2) >> 2;
    p[1] = (lt + 2*l[0] + l[1] + 2) >> 2;
    p[2] = (l[0] + 2*lt + t[0] + 2) >> 2;
    p[3] = (lt + t[0] + 1) >> 1;
    p[4] = (lt + 2*t[0] + t[1] + 2) >> 2;
    p[5] = (t[0] + t[1] + 1) >> 1;
    p[6] = (t[0] + 2*t[1] + t[2] + 2) >> 2;
    p[7] = (t[1] + t[2] + 1) >> 1;
    p[8] = (t[1] + 2*t[2] + t[3] + 2) >> 2;
    p[9] = (t[2] + t[3] + 1) >> 1;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[3 + 2*i - j]);
}

static void pred4x4_horizontal_down(VXDecCtx *c, int x, int y)
{
    int l[4], t[3], lt, p[10];
    lt = dst_get(c, 0, x - 1, y - 1);
    for (int i = 0; i < 3; i++) t[i] = dst_get(c, 0, x + i, y - 1);
    for (int j = 0; j < 4; j++) l[j] = dst_get(c, 0, x - 1, y + j);
    p[0] = (t[0] + 2*t[1] + t[2] + 2) >> 2;
    p[1] = (lt + 2*t[0] + t[1] + 2) >> 2;
    p[2] = (l[0] + 2*lt + t[0] + 2) >> 2;
    p[3] = (lt + l[0] + 1) >> 1;
    p[4] = (lt + 2*l[0] + l[1] + 2) >> 2;
    p[5] = (l[0] + l[1] + 1) >> 1;
    p[6] = (l[0] + 2*l[1] + l[2] + 2) >> 2;
    p[7] = (l[1] + l[2] + 1) >> 1;
    p[8] = (l[1] + 2*l[2] + l[3] + 2) >> 2;
    p[9] = (l[2] + l[3] + 1) >> 1;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[3 - i + 2*j]);
}

static void pred4x4_vertical_left(VXDecCtx *c, int x, int y)
{
    int t[7], p[10];
    for (int i = 0; i < 7; i++) t[i] = dst_get(c, 0, x + i, y - 1);
    p[0] = (t[0] + t[1] + 1) >> 1;
    p[1] = (t[0] + 2*t[1] + t[2] + 2) >> 2;
    p[2] = (t[1] + t[2] + 1) >> 1;
    p[3] = (t[1] + 2*t[2] + t[3] + 2) >> 2;
    p[4] = (t[2] + t[3] + 1) >> 1;
    p[5] = (t[2] + 2*t[3] + t[4] + 2) >> 2;
    p[6] = (t[3] + t[4] + 1) >> 1;
    p[7] = (t[3] + 2*t[4] + t[5] + 2) >> 2;
    p[8] = (t[4] + t[5] + 1) >> 1;
    p[9] = (t[4] + 2*t[5] + t[6] + 2) >> 2;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[2*i + j]);
}

static void pred4x4_horizontal_up(VXDecCtx *c, int x, int y)
{
    int l[4], p[7];
    for (int j = 0; j < 4; j++) l[j] = dst_get(c, 0, x - 1, y + j);
    p[0] = (l[0] + l[1] + 1) >> 1;
    p[1] = (l[0] + 2*l[1] + l[2] + 2) >> 2;
    p[2] = (l[1] + l[2] + 1) >> 1;
    p[3] = (l[1] + 2*l[2] + l[3] + 2) >> 2;
    p[4] = (l[2] + l[3] + 1) >> 1;
    p[5] = (l[2] + 2*l[3] + l[3] + 2) >> 2;
    p[6] = l[3];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            dst_set(c, 0, x + i, y + j, p[FFMIN(i + 2*j, 6)]);
}

static int predict_notile_uv(VXDecCtx *c, MBlock b);

static int predict4(VXDecCtx *c, MBlock block)
{
    int cache[5][5];
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++)
            cache[j][i] = 9;

    for (int y2 = 0; y2 < block.h / 4; y2++) {
        for (int x2 = 0; x2 < block.w / 4; x2++) {
            int mode = FFMIN(cache[1 + y2 - 1][1 + x2], cache[1 + y2][1 + x2 - 1]);
            if (mode == 9)
                mode = 2;

            if (!get_bits1(c->gb)) {
                int val = get_bits(c->gb, 3);
                mode = val + (val >= mode ? 1 : 0);
            }
            cache[1 + y2][1 + x2] = mode;

            int dx = block.x + x2 * 4, dy = block.y + y2 * 4;
            switch (mode) {
            case 0: pred4x4_vertical(c, dx, dy); break;
            case 1: pred4x4_horizontal(c, dx, dy); break;
            case 2:
                if (dx != 0 && dy != 0) pred4x4_dc(c, dx, dy);
                else if (dx != 0) pred4x4_left_dc(c, dx, dy);
                else if (dy != 0) pred4x4_top_dc(c, dx, dy);
                else pred4x4_128_dc(c, dx, dy);
                break;
            case 3: pred4x4_down_left(c, dx, dy); break;
            case 4: pred4x4_down_right(c, dx, dy); break;
            case 5: pred4x4_vertical_right(c, dx, dy); break;
            case 6: pred4x4_horizontal_down(c, dx, dy); break;
            case 7: pred4x4_vertical_left(c, dx, dy); break;
            case 8: pred4x4_horizontal_up(c, dx, dy); break;
            default:
                return AVERROR_INVALIDDATA;
            }
        }
    }
    return predict_notile_uv(c, block);
}

/* ---- block/plane-level prediction ---- */

static void predict_dc(VXDecCtx *c, MBlock b, int plane)
{
    int dc = 128;
    if (b.x != 0 && b.y != 0) {
        int sx = b.w / 2, sy = b.h / 2;
        for (int i = 0; i < b.w; i++) sx += dst_get(c, plane, b.x + i, b.y - 1);
        for (int i = 0; i < b.h; i++) sy += dst_get(c, plane, b.x - 1, b.y + i);
        dc = ((sx / b.w) + (sy / b.h) + 1) / 2;
    } else if (b.x == 0 && b.y != 0) {
        int sx = b.w / 2;
        for (int i = 0; i < b.w; i++) sx += dst_get(c, plane, b.x + i, b.y - 1);
        dc = sx / b.w;
    } else if (b.x != 0 && b.y == 0) {
        int sy = b.h / 2;
        for (int i = 0; i < b.h; i++) sy += dst_get(c, plane, b.x - 1, b.y + i);
        dc = sy / b.h;
    }
    int step = plane ? 2 : 1;
    for (int j = 0; j < b.h; j += step)
        for (int i = 0; i < b.w; i += step)
            dst_set(c, plane, b.x + i, b.y + j, dc);
}

static void predict_horizontal(VXDecCtx *c, MBlock b, int plane)
{
    int step = plane ? 2 : 1;
    for (int j = 0; j < b.h; j += step) {
        int px = dst_get(c, plane, b.x - 1, b.y + j);
        for (int i = 0; i < b.w; i += step)
            dst_set(c, plane, b.x + i, b.y + j, px);
    }
}

static void predict_vertical(VXDecCtx *c, MBlock b, int plane)
{
    int step = plane ? 2 : 1;
    for (int i = 0; i < b.w; i += step) {
        int px = dst_get(c, plane, b.x + i, b.y - 1);
        for (int j = 0; j < b.h; j += step)
            dst_set(c, plane, b.x + i, b.y + j, px);
    }
}

static void predict_plane_intern(VXDecCtx *c, MBlock b, int plane)
{
    int step = plane ? 2 : 1;

    if (b.w == step && b.h == step)
        return;

    if (b.w == step && b.h > step) {
        int top = dst_get(c, plane, b.x, b.y - 1);
        int bot = dst_get(c, plane, b.x, b.y + b.h - 1);
        dst_set(c, plane, b.x, b.y + b.h / 2 - 1, (top + bot) / 2);
        MBlock up = { b.x, b.y, b.w, b.h / 2 };
        MBlock down = { b.x, b.y + b.h / 2, b.w, b.h / 2 };
        predict_plane_intern(c, up, plane);
        predict_plane_intern(c, down, plane);
    } else if (b.w > step && b.h == step) {
        int left = dst_get(c, plane, b.x - 1, b.y);
        int right = dst_get(c, plane, b.x + b.w - 1, b.y);
        dst_set(c, plane, b.x + b.w / 2 - 1, b.y, (left + right) / 2);
        MBlock l = { b.x, b.y, b.w / 2, b.h };
        MBlock r = { b.x + b.w / 2, b.y, b.w / 2, b.h };
        predict_plane_intern(c, l, plane);
        predict_plane_intern(c, r, plane);
    } else {
        int bl = dst_get(c, plane, b.x - 1, b.y + b.h - 1);
        int tr = dst_get(c, plane, b.x + b.w - 1, b.y - 1);
        int br = dst_get(c, plane, b.x + b.w - 1, b.y + b.h - 1);
        int bc = (bl + br) / 2;
        int cr = (tr + br) / 2;
        dst_set(c, plane, b.x + b.w / 2 - 1, b.y + b.h - 1, bc);
        dst_set(c, plane, b.x + b.w - 1, b.y + b.h / 2 - 1, cr);
        int wbig = (b.w == 4 * step || b.w == 16 * step);
        int hbig = (b.h == 4 * step || b.h == 16 * step);
        int cc;
        if (wbig != hbig) {
            int cl = dst_get(c, plane, b.x - 1, b.y + b.h / 2 - 1);
            cc = (cl + cr) / 2;
        } else {
            int tc = dst_get(c, plane, b.x + b.w / 2 - 1, b.y - 1);
            cc = (tc + bc) / 2;
        }
        dst_set(c, plane, b.x + b.w / 2 - 1, b.y + b.h / 2 - 1, cc);
        MBlock ul = { b.x, b.y, b.w / 2, b.h / 2 };
        MBlock ur = { b.x + b.w / 2, b.y, b.w / 2, b.h / 2 };
        MBlock dl = { b.x, b.y + b.h / 2, b.w / 2, b.h / 2 };
        MBlock dr = { b.x + b.w / 2, b.y + b.h / 2, b.w / 2, b.h / 2 };
        predict_plane_intern(c, ul, plane);
        predict_plane_intern(c, ur, plane);
        predict_plane_intern(c, dl, plane);
        predict_plane_intern(c, dr, plane);
    }
}

static void predict_plane(VXDecCtx *c, MBlock b, int plane, int param)
{
    int bl = dst_get(c, plane, b.x - 1, b.y + b.h - 1);
    int tr = dst_get(c, plane, b.x + b.w - 1, b.y - 1);
    int px = (bl + tr + 1) / 2 + param;
    dst_set(c, plane, b.x + b.w - 1, b.y + b.h - 1, px);
    predict_plane_intern(c, b, plane);
}

static int predict_mb_plane(VXDecCtx *c, MBlock b)
{
    int p;
    p = get_se_golomb(c->gb);
    if (p < -(1 << 16) || p >= (1 << 16)) return AVERROR_INVALIDDATA;
    predict_plane(c, b, 0, p * 2);
    p = get_se_golomb(c->gb);
    if (p < -(1 << 16) || p >= (1 << 16)) return AVERROR_INVALIDDATA;
    predict_plane(c, b, 1, p * 2);
    p = get_se_golomb(c->gb);
    if (p < -(1 << 16) || p >= (1 << 16)) return AVERROR_INVALIDDATA;
    predict_plane(c, b, 2, p * 2);
    return 0;
}

static int predict_notile_uv(VXDecCtx *c, MBlock b)
{
    int mode = get_ue_golomb_31(c->gb);
    switch (mode) {
    case 0: predict_dc(c, b, 1); predict_dc(c, b, 2); break;
    case 1: predict_horizontal(c, b, 1); predict_horizontal(c, b, 2); break;
    case 2: predict_vertical(c, b, 1); predict_vertical(c, b, 2); break;
    case 3: predict_plane(c, b, 1, 0); predict_plane(c, b, 2, 0); break;
    default: return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int predict_notile(VXDecCtx *c, MBlock b)
{
    int mode = get_ue_golomb_31(c->gb);
    switch (mode) {
    case 0: predict_vertical(c, b, 0); break;
    case 1: predict_horizontal(c, b, 0); break;
    case 2: predict_dc(c, b, 0); break;
    case 3: predict_plane(c, b, 0, 0); break;
    default: return AVERROR_INVALIDDATA;
    }
    return predict_notile_uv(c, b);
}

static int mv_in_bounds(VXDecCtx *c, MBlock b, MV v)
{
    return !(b.x + v.x < 0 || b.x + v.x + b.w > c->width ||
             b.y + v.y < 0 || b.y + v.y + b.h > c->height);
}

static int predict_inter(VXDecCtx *c, MBlock b, MV pred_vec, int has_delta, const VXPic *ref)
{
    MV vec = pred_vec;
    if (!ref || !ref->data[0])
        return AVERROR_INVALIDDATA;
    if (has_delta) {
        vec.x += get_se_golomb(c->gb);
        vec.y += get_se_golomb(c->gb);
    }
    if (!mv_in_bounds(c, b, vec))
        return AVERROR_INVALIDDATA;

    c->vectors[(b.y / 16 + 1) * c->vectors_stride + (b.x / 16 + 1)] = vec;

    for (int plane = 0; plane < 3; plane++) {
        int step = plane ? 2 : 1;
        for (int j = 0; j < b.h; j += step) {
            for (int i = 0; i < b.w; i += step) {
                int val = px_get(ref->data[plane], ref->linesize[plane], c->width, c->height,
                                  plane, b.x + i + vec.x, b.y + j + vec.y);
                dst_set(c, plane, b.x + i, b.y + j, val);
            }
        }
    }
    return 0;
}

static int predict_inter_dc(VXDecCtx *c, MBlock b)
{
    MV vec;
    int dcy, dcu, dcv;
    const VXPic *ref = c->refs[0];

    vec.x = get_se_golomb(c->gb);
    vec.y = get_se_golomb(c->gb);
    if (!ref || !ref->data[0])
        return AVERROR_INVALIDDATA;
    if (!mv_in_bounds(c, b, vec))
        return AVERROR_INVALIDDATA;

    dcy = get_se_golomb(c->gb);
    if (dcy < -(1 << 16) || dcy >= (1 << 16)) return AVERROR_INVALIDDATA;
    dcy *= 2;
    dcu = get_se_golomb(c->gb);
    if (dcu < -(1 << 16) || dcu >= (1 << 16)) return AVERROR_INVALIDDATA;
    dcu *= 2;
    dcv = get_se_golomb(c->gb);
    if (dcv < -(1 << 16) || dcv >= (1 << 16)) return AVERROR_INVALIDDATA;
    dcv *= 2;

    /* NB: unlike predict_inter, predict_inter_dc does NOT record its motion
     * vector into the predictor array (matches both the actimagine Python
     * reference and Gericom's decoder). */

    int dc[3] = { dcy, dcu, dcv };
    for (int plane = 0; plane < 3; plane++) {
        int step = plane ? 2 : 1;
        for (int j = 0; j < b.h; j += step) {
            for (int i = 0; i < b.w; i += step) {
                int val = px_get(ref->data[plane], ref->linesize[plane], c->width, c->height,
                                  plane, b.x + i + vec.x, b.y + j + vec.y);
                dst_set(c, plane, b.x + i, b.y + j, av_clip_uint8(val + dc[plane]));
            }
        }
    }
    return 0;
}

/* ---- residual (CAVLC) decode ---- */

static void idct_add(VXDecCtx *c, int x, int y, int plane, const int *dctv)
{
    int dct[16];
    int step = plane ? 2 : 1;

    for (int i = 0; i < 16; i++)
        dct[zigzag_scan[i]] = dctv[i] * c->qtab[(zigzag_scan[i] & 1) + ((zigzag_scan[i] >> 2) & 1)];

    dct[0] += 1 << 5;

    for (int i = 0; i < 4; i++) {
        int z0 = dct[i + 4*0] + dct[i + 4*2];
        int z1 = dct[i + 4*0] - dct[i + 4*2];
        int z2 = (dct[i + 4*1] >> 1) - dct[i + 4*3];
        int z3 = dct[i + 4*1] + (dct[i + 4*3] >> 1);

        dct[i + 4*0] = z0 + z3;
        dct[i + 4*1] = z1 + z2;
        dct[i + 4*2] = z1 - z2;
        dct[i + 4*3] = z0 - z3;
    }

    for (int i = 0; i < 4; i++) {
        int z0 = dct[0 + 4*i] + dct[2 + 4*i];
        int z1 = dct[0 + 4*i] - dct[2 + 4*i];
        int z2 = (dct[1 + 4*i] >> 1) - dct[3 + 4*i];
        int z3 = dct[1 + 4*i] + (dct[3 + 4*i] >> 1);

        dst_set(c, plane, x + step*i, y + step*0,
                av_clip_uint8(dst_get(c, plane, x + step*i, y + step*0) + ((z0 + z3) >> 6)));
        dst_set(c, plane, x + step*i, y + step*1,
                av_clip_uint8(dst_get(c, plane, x + step*i, y + step*1) + ((z1 + z2) >> 6)));
        dst_set(c, plane, x + step*i, y + step*2,
                av_clip_uint8(dst_get(c, plane, x + step*i, y + step*2) + ((z1 - z2) >> 6)));
        dst_set(c, plane, x + step*i, y + step*3,
                av_clip_uint8(dst_get(c, plane, x + step*i, y + step*3) + ((z0 - z3) >> 6)));
    }
}

/* Returns total_coeff (0..16) on success, negative AVERROR on failure. */
static int decode_residu_cavlc(VXDecCtx *c, int x, int y, int nc, int plane)
{
    GetBitContext *gb = c->gb;
    int coeff_token, total_coeff, trailing_ones, zeros_left;
    int level[16] = { 0 };
    int wpos = 15;

    if (nc < 0 || nc > 16)
        return AVERROR_INVALIDDATA;

    coeff_token = get_vlc2(gb, vx_coeff_token_vlc[coeff_token_table_index[nc]].table,
                           ACTIMAGINE_COEFF_TOKEN_VLC_BITS, 2);
    if (coeff_token < 0)
        return AVERROR_INVALIDDATA;

    trailing_ones = coeff_token & 3;
    total_coeff = coeff_token >> 2;
    if (total_coeff == 0)
        return 0;
    if (total_coeff > 16)
        return AVERROR_INVALIDDATA;

    if (total_coeff == 16) {
        zeros_left = 0;
    } else {
        zeros_left = get_vlc2(gb, vx_total_zeros_vlc[total_coeff].table, ACTIMAGINE_TOTAL_ZEROS_VLC_BITS, 1);
        if (zeros_left < 0)
            return AVERROR_INVALIDDATA;
        wpos -= (16 - (total_coeff + zeros_left));
    }

    {
        int suffix_length = 0;
        int to = total_coeff;
        int trailing = trailing_ones;
        while (1) {
            int val;
            if (trailing > 0) {
                trailing--;
                val = get_bits1(gb) ? -1 : 1;
            } else {
                int level_prefix = 0;
                int level_suffix, level_code;
                while (get_bits1(gb) == 0) {
                    level_prefix++;
                    if (level_prefix > 32)
                        return AVERROR_INVALIDDATA;
                }
                if (level_prefix == 15)
                    level_suffix = get_bits(gb, 11);
                else
                    level_suffix = suffix_length ? get_bits(gb, suffix_length) : 0;
                level_code = (level_prefix << suffix_length) + level_suffix + 1;
                if (level_code > cavlc_suffix_limit[suffix_length + 1])
                    suffix_length++;
                if (get_bits1(gb))
                    level_code = -level_code;
                val = level_code;
            }
            if (wpos < 0)
                return AVERROR_INVALIDDATA;
            level[wpos--] = val;
            to--;
            if (to == 0)
                break;
            if (zeros_left != 0) {
                int run_before;
                if (zeros_left < 7)
                    run_before = get_vlc2(gb, vx_run_vlc[zeros_left].table, ACTIMAGINE_RUN_VLC_BITS, 1);
                else
                    run_before = get_vlc2(gb, vx_run7_vlc.table, ACTIMAGINE_RUN7_VLC_BITS, 2);
                if (run_before < 0)
                    return AVERROR_INVALIDDATA;
                zeros_left -= run_before;
                wpos -= run_before;
            }
        }
    }

    idct_add(c, x, y, plane, level);
    return total_coeff;
}

static int decode_residu_blocks(VXDecCtx *c, MBlock b)
{
    for (int y = 0; y < b.h; y += 8) {
        for (int x = 0; x < b.w; x += 8) {
            int idx = get_ue_golomb_31(c->gb);
            int mask, r;

            if (idx < 0 || idx > 0x1F)
                return AVERROR_INVALIDDATA;
            mask = residu_mask_new_tab[idx];

            if (mask & 1) {
                int nc = (coeff_y_get(c, b.x+x-1, b.y+y) + coeff_y_get(c, b.x+x, b.y+y-1) + 1) / 2;
                r = decode_residu_cavlc(c, b.x+x, b.y+y, nc, 0);
                if (r < 0) return r;
                coeff_y_set(c, b.x+x, b.y+y, r);
            } else coeff_y_set(c, b.x+x, b.y+y, 0);

            if (mask & 2) {
                int nc = (coeff_y_get(c, b.x+x+4-1, b.y+y) + coeff_y_get(c, b.x+x+4, b.y+y-1) + 1) / 2;
                r = decode_residu_cavlc(c, b.x+x+4, b.y+y, nc, 0);
                if (r < 0) return r;
                coeff_y_set(c, b.x+x+4, b.y+y, r);
            } else coeff_y_set(c, b.x+x+4, b.y+y, 0);

            if (mask & 4) {
                int nc = (coeff_y_get(c, b.x+x-1, b.y+y+4) + coeff_y_get(c, b.x+x, b.y+y+4-1) + 1) / 2;
                r = decode_residu_cavlc(c, b.x+x, b.y+y+4, nc, 0);
                if (r < 0) return r;
                coeff_y_set(c, b.x+x, b.y+y+4, r);
            } else coeff_y_set(c, b.x+x, b.y+y+4, 0);

            if (mask & 8) {
                int nc = (coeff_y_get(c, b.x+x+4-1, b.y+y+4) + coeff_y_get(c, b.x+x+4, b.y+y+4-1) + 1) / 2;
                r = decode_residu_cavlc(c, b.x+x+4, b.y+y+4, nc, 0);
                if (r < 0) return r;
                coeff_y_set(c, b.x+x+4, b.y+y+4, r);
            } else coeff_y_set(c, b.x+x+4, b.y+y+4, 0);

            if (mask & 16) {
                int nc = (coeff_uv_get(c, b.x+x-1, b.y+y) + coeff_uv_get(c, b.x+x, b.y+y-1) + 1) / 2;
                int ru = decode_residu_cavlc(c, b.x+x, b.y+y, nc, 1);
                if (ru < 0) return ru;
                int rv = decode_residu_cavlc(c, b.x+x, b.y+y, nc, 2);
                if (rv < 0) return rv;
                coeff_uv_set(c, b.x+x, b.y+y, (ru + rv + 1) / 2);
            } else coeff_uv_set(c, b.x+x, b.y+y, 0);
        }
    }
    return 0;
}

static void clear_total_coeff(VXDecCtx *c, MBlock b)
{
    for (int y = 0; y < b.h; y += 8) {
        for (int x = 0; x < b.w; x += 8) {
            coeff_y_set(c, b.x+x, b.y+y, 0);
            coeff_y_set(c, b.x+x, b.y+y+4, 0);
            coeff_y_set(c, b.x+x+4, b.y+y, 0);
            coeff_y_set(c, b.x+x+4, b.y+y+4, 0);
            coeff_uv_set(c, b.x+x, b.y+y, 0);
        }
    }
}

static MBlock half_left(MBlock b)  { return (MBlock){ b.x, b.y, b.w/2, b.h }; }
static MBlock half_right(MBlock b) { return (MBlock){ b.x + b.w/2, b.y, b.w/2, b.h }; }
static MBlock half_up(MBlock b)    { return (MBlock){ b.x, b.y, b.w, b.h/2 }; }
static MBlock half_down(MBlock b)  { return (MBlock){ b.x, b.y + b.h/2, b.w, b.h/2 }; }

static int decode_mb(VXDecCtx *c, MBlock b, MV pred_vec)
{
    int mode = get_ue_golomb_31(c->gb);
    int ret;

    switch (mode) {
    case 0: /* v-split, no residu */
        if (b.w == 2) return AVERROR_INVALIDDATA;
        if ((ret = decode_mb(c, half_left(b), pred_vec)) < 0) return ret;
        if ((ret = decode_mb(c, half_right(b), pred_vec)) < 0) return ret;
        if (b.w == 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 1: /* no delta, no residu, ref 0 */
        if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[0])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 2: /* h-split, no residu */
        if (b.h == 2) return AVERROR_INVALIDDATA;
        if ((ret = decode_mb(c, half_up(b), pred_vec)) < 0) return ret;
        if ((ret = decode_mb(c, half_down(b), pred_vec)) < 0) return ret;
        if (b.w >= 8 && b.h == 8) clear_total_coeff(c, b);
        break;
    case 3: /* unpredicted delta ref0 + dc offset, no residu */
        if ((ret = predict_inter_dc(c, b)) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 4: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[0])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 5: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[1])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 6: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[2])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 7: if ((ret = predict_mb_plane(c, b)) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 8: /* v-split, residu */
        if (b.w == 2) return AVERROR_INVALIDDATA;
        if ((ret = decode_mb(c, half_left(b), pred_vec)) < 0) return ret;
        if ((ret = decode_mb(c, half_right(b), pred_vec)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 9: if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[1])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 10: if ((ret = predict_inter_dc(c, b)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 11: if ((ret = predict_notile(c, b)) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 12: if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[0])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 13: /* h-split, residu */
        if (b.h == 2) return AVERROR_INVALIDDATA;
        if ((ret = decode_mb(c, half_up(b), pred_vec)) < 0) return ret;
        if ((ret = decode_mb(c, half_down(b), pred_vec)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 14: if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[2])) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 15: if ((ret = predict4(c, b)) < 0) return ret;
        if (b.w >= 8 && b.h >= 8) clear_total_coeff(c, b);
        break;
    case 16: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[0])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 17: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[1])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 18: if ((ret = predict_inter(c, b, pred_vec, 1, c->refs[2])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 19: if ((ret = predict4(c, b)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 20: if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[1])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 21: if ((ret = predict_inter(c, b, pred_vec, 0, c->refs[2])) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 22: if ((ret = predict_notile(c, b)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    case 23: if ((ret = predict_mb_plane(c, b)) < 0) return ret;
        if ((ret = decode_residu_blocks(c, b)) < 0) return ret;
        break;
    default:
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

int ff_vx_calc_qtab(int quantizer, uint16_t qtab[3])
{
    int qx, qy;
    if (quantizer < 12 || quantizer > 161)
        return AVERROR_INVALIDDATA;
    qx = quantizer % 6;
    qy = quantizer / 6;
    for (int i = 0; i < 3; i++)
        qtab[i] = quant4x4_tab[qx][i] << qy;
    return 0;
}

int ff_vx_decode_vframe(AVCodecContext *avctx, GetBitContext *gb,
                                  int width, int height, const uint16_t qtab[3],
                                  VXPic *dst, const VXPic refs[3])
{
    VXDecCtx c = { 0 };
    int ret;

    ret = ff_thread_once(&vx_cavlc_once, vx_cavlc_vlc_init);
    if (ret)
        return AVERROR_UNKNOWN;

    c.avctx = avctx;
    c.gb = gb;
    c.width = width;
    c.height = height;
    c.qtab = qtab;
    c.dst = *dst;
    for (int i = 0; i < 3; i++)
        c.refs[i] = (refs[i].data[0]) ? &refs[i] : NULL;

    c.coeff_y_stride = width / 4 + 1;
    c.coeff_uv_stride = width / 8 + 1;
    c.coeff_y = av_calloc((size_t)(height / 4 + 1) * c.coeff_y_stride, 1);
    c.coeff_uv = av_calloc((size_t)(height / 8 + 1) * c.coeff_uv_stride, 1);
    c.vectors_stride = width / 16 + 2;
    c.vectors = av_calloc((size_t)(height / 16 + 1) * c.vectors_stride, sizeof(*c.vectors));
    if (!c.coeff_y || !c.coeff_uv || !c.vectors) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (int y = 0; y < height; y += 16) {
        for (int x = 0; x < width; x += 16) {
            MBlock b = { x, y, 16, 16 };
            int by = y / 16, bx = x / 16;
            MV left  = c.vectors[(by + 1) * c.vectors_stride + (bx + 0)];
            MV above = c.vectors[(by + 0) * c.vectors_stride + (bx + 1)];
            MV aboveright = c.vectors[(by + 0) * c.vectors_stride + (bx + 2)];
            MV pred_vec = {
                mid_pred3(left.x, above.x, aboveright.x),
                mid_pred3(left.y, above.y, aboveright.y),
            };
            if (get_bits_left(gb) < 0) {
                ret = AVERROR_INVALIDDATA;
                goto end;
            }
            if ((ret = decode_mb(&c, b, pred_vec)) < 0)
                goto end;
        }
    }

    {
        int rem = get_bits_count(gb) & 15;
        if (rem)
            skip_bits(gb, 16 - rem);
    }
    ret = 0;

end:
    av_freep(&c.coeff_y);
    av_freep(&c.coeff_uv);
    av_freep(&c.vectors);
    return ret;
}

/* ---- AVCodec wrapper (video output stream) ---- */

typedef struct VXVContext {
    AVFrame *pic[4];
    int cur;
    uint16_t qtab[3];
    BswapDSPContext bdsp;
    uint8_t *bitstream;
    unsigned int bitstream_size;
} VXVContext;

/* The VX bitstream stores a bit-shift YCoCg-style approximation of RGB rather
 * than a standard YCbCr colorspace (Gericom's reference documents the exact
 * reconstruction). No AVCOL_SPC value describes it and swscale can't convert
 * it, so the decoder does the conversion itself and outputs RGB24; the U/V
 * planes are centered at 128. */
static void vx_yuv_to_rgb24(AVCodecContext *avctx, const AVFrame *yuv, AVFrame *rgb)
{
    int w = avctx->width, h = avctx->height;

    for (int y = 0; y < h; y++) {
        const uint8_t *ly = yuv->data[0] + (ptrdiff_t)y * yuv->linesize[0];
        const uint8_t *lu = yuv->data[1] + (ptrdiff_t)(y >> 1) * yuv->linesize[1];
        const uint8_t *lv = yuv->data[2] + (ptrdiff_t)(y >> 1) * yuv->linesize[2];
        uint8_t *out = rgb->data[0] + (ptrdiff_t)y * rgb->linesize[0];

        for (int x = 0; x < w; x++) {
            int Y  = ly[x];
            int cu = lu[x >> 1] - 128;
            int cv = lv[x >> 1] - 128;
            out[x * 3 + 0] = av_clip_uint8(Y + 2 * cv);          /* r = y + (v << 1) */
            out[x * 3 + 1] = av_clip_uint8(Y - (cu >> 1) - cv);  /* g = y - (u >> 1) - v */
            out[x * 3 + 2] = av_clip_uint8(Y + 2 * cu);          /* b = y + (u << 1) */
        }
    }
}

static av_cold int vx_dec_init(AVCodecContext *avctx)
{
    VXVContext *s = avctx->priv_data;
    int quantizer = 0;

    avctx->pix_fmt = AV_PIX_FMT_RGB24;
    if (avctx->width % 16 || avctx->height % 16)
        return AVERROR_INVALIDDATA;

    if (avctx->extradata && avctx->extradata_size >= 4)
        quantizer = AV_RL32(avctx->extradata);

    if (ff_vx_calc_qtab(quantizer, s->qtab) < 0)
        return AVERROR_INVALIDDATA;

    ff_bswapdsp_init(&s->bdsp);

    for (int i = 0; i < 4; i++) {
        s->pic[i] = av_frame_alloc();
        if (!s->pic[i])
            return AVERROR(ENOMEM);
    }
    return 0;
}

static int vx_dec_decode(AVCodecContext *avctx, AVFrame *rframe,
                                   int *got_frame, AVPacket *pkt)
{
    VXVContext *s = avctx->priv_data;
    AVFrame *frame = s->pic[s->cur];
    GetBitContext gb;
    VXPic dst = { { 0 } }, refs[3] = { { { 0 } } };
    int ret;

    av_fast_padded_malloc(&s->bitstream, &s->bitstream_size, pkt->size);
    if (!s->bitstream)
        return AVERROR(ENOMEM);

    /* Internal reconstruction frames are YUV420P (motion compensation and
     * intra prediction operate in the codec's native plane domain); they are
     * private to the decoder and reused as reference frames, so allocate them
     * lazily and never ref them out. The public output frame is RGB24. */
    if (!frame->data[0]) {
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width  = avctx->width;
        frame->height = avctx->height;
        if ((ret = av_frame_get_buffer(frame, 0)) < 0)
            return ret;
    }

    /* The reference decoder starts each frame's plane buffers zeroed; some
     * directional intra prediction modes can read pixels that have not yet
     * been written this frame (e.g. top-right extension), so match that. */
    for (int i = 0; i < 3; i++) {
        int h = (i == 0) ? avctx->height : avctx->height / 2;
        for (int j = 0; j < h; j++)
            memset(frame->data[i] + j * frame->linesize[i], 0, (i == 0 ? avctx->width : avctx->width / 2));
    }

    s->bdsp.bswap16_buf((uint16_t *)s->bitstream, (uint16_t *)pkt->data, (pkt->size + 1) >> 1);

    ret = init_get_bits8(&gb, s->bitstream, FFALIGN(pkt->size, 2));
    if (ret < 0)
        return ret;

    for (int i = 0; i < 3; i++) {
        dst.data[i] = frame->data[i];
        dst.linesize[i] = frame->linesize[i];
    }
    for (int i = 0; i < 3; i++) {
        AVFrame *rp = s->pic[((s->cur - 1 - i) % 4 + 4) % 4];
        if (rp->data[0]) {
            for (int p = 0; p < 3; p++) {
                refs[i].data[p] = rp->data[p];
                refs[i].linesize[p] = rp->linesize[p];
            }
        }
    }

    ret = ff_vx_decode_vframe(avctx, &gb, avctx->width, avctx->height, s->qtab, &dst, refs);
    if (ret < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, rframe, 0)) < 0)
        return ret;
    vx_yuv_to_rgb24(avctx, frame, rframe);

    s->cur = (s->cur + 1) % 4;
    *got_frame = 1;
    return pkt->size;
}

static av_cold void vx_dec_flush(AVCodecContext *avctx)
{
    VXVContext *s = avctx->priv_data;
    for (int i = 0; i < 4; i++)
        av_frame_unref(s->pic[i]);
}

static av_cold int vx_dec_close(AVCodecContext *avctx)
{
    VXVContext *s = avctx->priv_data;
    av_freep(&s->bitstream);
    s->bitstream_size = 0;
    for (int i = 0; i < 4; i++)
        av_frame_free(&s->pic[i]);
    return 0;
}

const FFCodec ff_vx_decoder = {
    .p.name         = "vx",
    CODEC_LONG_NAME("ActImagine VX Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_VX,
    .priv_data_size = sizeof(VXVContext),
    .init           = vx_dec_init,
    FF_CODEC_DECODE_CB(vx_dec_decode),
    .flush          = vx_dec_flush,
    .close          = vx_dec_close,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
