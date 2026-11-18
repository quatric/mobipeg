/*
 * MIVF M2Y1 video encoder
 *
 * The block coder is ported from the MIVF toolkit's native encoder
 * (tools/miv2y_moflex_tier.c), with only what a library cannot do changed:
 * no process exit on allocation failure, per-encoder state instead of
 * globals, and no file or container handling (libavformat/mivfenc.c writes
 * the container). It carries this notice:
 *
 * MIT License
 *
 * Copyright (c) 2026 Micah Lagger / MIVF Player contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "mivf_rc.h"
#include "mivf_transform.h"

/* The reference's exact zero-distortion SKIP shortcut (see the comment it
 * carries in tools/miv2y_moflex_tier.c): a block identical to the one
 * behind it can only be coded as SKIP, so nothing else is tried for it. */
#define MIVF_E3_SKIP_SHORTCUT_ENABLED 1

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;

/* Shared transform codec (single source of truth with the player). */

/* Kept transform coefficients per 4x4 quadrant for newly written frames.
   Written into the M2Y1 body header so the player decodes with the same value. */

enum {
    M_SKIP     = 0,
    M_RAW      = 1,
    M_DELTA    = 2,
    M_SOLID    = 3,
    M_RUN_SKIP = 4,
    M_QRES     = 5,
    M_MVCOPY   = 6,
    M_MVQRES  = 7,
    M_TRANSFORM   = 8,
    M_MVTRANSFORM = 9,
    M_QRESZ         = 10,
    M_MVQRESZ       = 11,
    M_TRANSFORMZ    = 12,
    M_MVTRANSFORMZ  = 13,
    M_MVCOPYP       = 14,
    M_MVQRESP       = 15,
    M_MVTRANSFORMP  = 16,
    M_MVQRESZP      = 17,
    M_MVTRANSFORMZP = 18,
    M_GMVCOPY       = 19,
    M_SET_BASE_DQP = 20,
    M_GMVCOPY_INTERNAL = 1001
};

enum {
    HEADER_SIZE = 64,
    STREAM_DESC_SIZE = 32,
    PAGE_HEADER_SIZE = 32,
    PACKET_HEADER_SIZE = 16,

    PAGE_CRC = 1,
    PAGE_HAS_KEYFRAME = 2,

    PKT_KEYFRAME = 1,
    PKT_FRAME_START = 2,
    PKT_FRAME_END = 4
};

typedef struct {
    u8 *data;
    size_t size;
    size_t cap;
    int oom;
} Buf;

typedef struct {
    u32 skip;
    u32 delta;
    u32 solid;
    u32 mv;
    u32 mvqres;
    u32 qres;
    u32 tr;
    u32 mvtr;
    u32 qz;
    u32 mvqz;
    u32 trz;
    u32 mvtrz;
    u32 raw;
    u32 run_tokens;
    u32 run_blocks;
} PlaneStats;

typedef struct {
    Buf payload;
    PlaneStats stats;
    int pending_skip;
    int nkeep;
} PlaneEnc;

typedef struct {
    int count;
    s8 *mx;
    s8 *my;
} MVList;

/* --motion-search: full is the original exhaustive mvlist scan (default,
   unchanged behavior). diamond/fast are experimental iterative searches --
   see find_best_mv_64_diamond(). hybrid runs the same bounded diamond walk
   from two seeds (zero and the frame's global MV) and, only for blocks that
   still look unreliable, a small capped local refine -- see
   find_best_mv_64_hybrid(). None of these change the packed MVCOPY token
   format; the decoder only ever sees the final chosen (mx,my). */
enum { MS_FULL = 0, MS_DIAMOND = 1, MS_FAST = 2, MS_HYBRID = 3, MS_HIERARCHICAL = 4 };

typedef struct {
    int w;
    int h;
    int fps;
    int keyint;

    int y_skip;
    int y_delta;
    int y_solid;
    int y_qres;

    int c_skip;
    int c_delta;
    int c_solid;
    int c_qres;

    int mv_range;
    int y_mv_thresh;
    int c_mv_thresh;

    double lambda;
    double c_lambda;
    int qp;
    int c_qp_offset;
    int motion_search_mode;
} EncParams;


static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static u8 clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static s8 clamp_s8(int v) {
    if (v < -128) return -128;
    if (v > 127) return 127;
    return (s8)v;
}

static int round_div_int(int num, int den) {
    if (den <= 0) return 0;

    if (num >= 0) {
        return (num + den / 2) / den;
    } else {
        return -((-num + den / 2) / den);
    }
}

static void buf_init(Buf *b, size_t cap) {
    b->data = (u8*)av_malloc(cap ? cap : 1);
    b->size = 0;
    b->cap = b->data ? (cap ? cap : 1) : 0;
    b->oom = !b->data;
}

static void buf_free(Buf *b) {
    av_free(b->data);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

static void buf_reserve(Buf *b, size_t need) {
    if (b->oom || need <= b->cap) return;

    size_t nc = b->cap;
    while (nc < need) {
        nc *= 2;
        if (nc < b->cap) { b->oom = 1; return; }
    }

    u8 *p = (u8*)av_realloc(b->data, nc);
    if (!p) { b->oom = 1; return; }

    b->data = p;
    b->cap = nc;
}

static void buf_put_u8(Buf *b, u8 v) {
    buf_reserve(b, b->size + 1);
    if (b->oom) return;
    b->data[b->size++] = v;
}

static void buf_put_bytes(Buf *b, const void *p, size_t n) {
    buf_reserve(b, b->size + n);
    if (b->oom) return;
    memcpy(b->data + b->size, p, n);
    b->size += n;
}




/* ------------------------------------------------------------------------- */
/* CRC32                                                                     */
/* ------------------------------------------------------------------------- */




/* ------------------------------------------------------------------------- */
/* CLI                                                                       */
/* ------------------------------------------------------------------------- */






/* ------------------------------------------------------------------------- */
/* MIVF writer                                                               */
/* ------------------------------------------------------------------------- */




/* ------------------------------------------------------------------------- */
/* Block helpers                                                             */
/* ------------------------------------------------------------------------- */

static void get_block(const u8 *plane, int w, int bx, int by, u8 block[64]) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        memcpy(block + y * 8, plane + (y0 + y) * w + x0, 8);
    }
}

static void put_block(u8 *plane, int w, int bx, int by, const u8 block[64]) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        memcpy(plane + (y0 + y) * w + x0, block + y * 8, 8);
    }
}

static int avg_u8_64(const u8 b[64]) {
    int sum = 0;
    for (int i = 0; i < 64; i++) sum += b[i];
    return sum / 64;
}

static int max_abs_to_value_64(const u8 b[64], int value) {
    int m = 0;

    for (int i = 0; i < 64; i++) {
        int e = (int)b[i] - value;
        if (e < 0) e = -e;
        if (e > m) m = e;
    }

    return m;
}

static int avg_delta_64(const u8 cur[64], const u8 prev[64]) {
    int sum = 0;

    for (int i = 0; i < 64; i++) {
        sum += (int)cur[i] - (int)prev[i];
    }

    return clamp_int(round_div_int(sum, 64), -128, 127);
}

static int max_abs_delta_error_64(const u8 cur[64], const u8 prev[64], int d) {
    int m = 0;

    for (int i = 0; i < 64; i++) {
        int v = clamp_u8((int)prev[i] + d);
        int e = (int)cur[i] - v;
        if (e < 0) e = -e;
        if (e > m) m = e;
    }

    return m;
}

static int max_abs_diff_64(const u8 a[64], const u8 b[64]) {
    int m = 0;

    for (int i = 0; i < 64; i++) {
        int e = (int)a[i] - (int)b[i];
        if (e < 0) e = -e;
        if (e > m) m = e;
    }

    return m;
}

static void recon_delta_64(const u8 prev[64], int d, u8 out[64]) {
    for (int i = 0; i < 64; i++) {
        out[i] = clamp_u8((int)prev[i] + d);
    }
}

static void recon_solid_64(int value, u8 out[64]) {
    memset(out, value, 64);
}

/* ------------------------------------------------------------------------- */
/* QRES                                                                      */
/* ------------------------------------------------------------------------- */

static bool try_qres_64(
    const u8 cur[64],
    const u8 prev[64],
    int qres_thresh,
    u8 token[18],
    u8 recon[64]
) {
    if (qres_thresh <= 0) return false;

    int global_delta = avg_delta_64(cur, prev);
    s8 residuals[16];

    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            int err_sum = 0;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 2; xx++) {
                    int x = cx * 2 + xx;
                    int y = cy * 2 + yy;
                    int idx = y * 8 + x;

                    int pred = clamp_u8((int)prev[idx] + global_delta);
                    err_sum += (int)cur[idx] - pred;
                }
            }

            residuals[cy * 4 + cx] = clamp_s8(round_div_int(err_sum, 4));
        }
    }

    int sad = 0;

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            int cell = cell_y + (x >> 1);
            int v = clamp_u8((int)prev[idx] + global_delta + (int)residuals[cell]);

            recon[idx] = (u8)v;

            int e = (int)cur[idx] - v;
            if (e < 0) e = -e;
            sad += e;
        }
    }

    if (sad > qres_thresh) {
        return false;
    }

    token[0] = M_QRES;
    token[1] = (u8)(s8)global_delta;

    for (int i = 0; i < 16; i++) {
        token[2 + i] = (u8)residuals[i];
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* Motion search                                                             */
/* ------------------------------------------------------------------------- */

static void mvlist_free(MVList *m) {
    av_free(m->mx);
    av_free(m->my);
    m->mx = NULL;
    m->my = NULL;
    m->count = 0;
}

static void mvlist_add(MVList *m, bool *seen, int side, int range, int mx, int my) {
    if (mx < -range || mx > range || my < -range || my > range) return;

    int idx = (my + range) * side + (mx + range);

    if (seen[idx]) return;

    seen[idx] = true;

    m->mx[m->count] = (s8)mx;
    m->my[m->count] = (s8)my;
    m->count++;
}

static MVList build_mv_list(int range) {
    if (range < 0) range = 0;
    if (range > 127) range = 127;

    int side = range * 2 + 1;
    int max_count = side * side;

    MVList m;
    m.count = 0;
    m.mx = (s8*)av_malloc(max_count);
    m.my = (s8*)av_malloc(max_count);

    bool *seen = (bool*)av_calloc((size_t)max_count, sizeof(bool));
    if (!m.mx || !m.my || !seen) {
        av_free(m.mx);
        av_free(m.my);
        av_free(seen);
        m.mx = m.my = NULL;
        return m;
    }

    mvlist_add(&m, seen, side, range, 0, 0);

    for (int r = 1; r <= range; r++) {
        mvlist_add(&m, seen, side, range, -r, 0);
        mvlist_add(&m, seen, side, range,  r, 0);
    }

    for (int r = 1; r <= range; r++) {
        mvlist_add(&m, seen, side, range, 0, -r);
        mvlist_add(&m, seen, side, range, 0,  r);
    }

    for (int r = 1; r <= range; r++) {
        mvlist_add(&m, seen, side, range, -r, -r);
        mvlist_add(&m, seen, side, range,  r, -r);
        mvlist_add(&m, seen, side, range, -r,  r);
        mvlist_add(&m, seen, side, range,  r,  r);

        for (int mx = -r + 1; mx < r; mx++) {
            mvlist_add(&m, seen, side, range, mx, -r);
            mvlist_add(&m, seen, side, range, mx,  r);
        }

        for (int my = -r + 1; my < r; my++) {
            mvlist_add(&m, seen, side, range, -r, my);
            mvlist_add(&m, seen, side, range,  r, my);
        }
    }

    av_free(seen);

    return m;
}

/* E2_EXACT_SSE2_SAD_V1: exact row-vectorized SAD with strict cap. */
static inline int sad8x8_capped_exact(
    const u8 cur[64], const u8 *prev_plane, int w,
    int sx, int sy, int limit
) {
    int sad = 0;
    for (int yy = 0; yy < 8; yy++) {
        const u8 *c = cur + yy * 8;
        const u8 *p = prev_plane + (sy + yy) * w + sx;
#if defined(__SSE2__)
        __m128i cv = _mm_loadl_epi64((const __m128i*)c);
        __m128i pv = _mm_loadl_epi64((const __m128i*)p);
        sad += _mm_cvtsi128_si32(_mm_sad_epu8(cv, pv));
#else
        for (int x = 0; x < 8; x++) {
            int d = (int)c[x] - (int)p[x];
            sad += d < 0 ? -d : d;
        }
#endif
        if (sad > limit) return -1;
    }
    return sad;
}

static bool try_mvcopy_64(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    const MVList *mvlist,
    int mv_thresh,
    u8 token[3],
    u8 recon[64]
) {
    if (!prev_plane || !mvlist || mv_thresh < 0) return false;

    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int best_sad = -1;
    int best_mx = 0;
    int best_my = 0;
    int best_x0 = dst_x0;
    int best_y0 = dst_y0;

    for (int k = 0; k < mvlist->count; k++) {
        int mx = mvlist->mx[k];
        int my = mvlist->my[k];

        int sx = dst_x0 + mx;
        int sy = dst_y0 + my;

        if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
            continue;
        }

        int limit = (best_sad >= 0) ? best_sad : mv_thresh;
        int sad = sad8x8_capped_exact(cur, prev_plane, w, sx, sy, limit);
        if (sad < 0) continue;

        if (sad > mv_thresh) {
            continue;
        }

        if (best_sad < 0 || sad < best_sad) {
            best_sad = sad;
            best_mx = mx;
            best_my = my;
            best_x0 = sx;
            best_y0 = sy;

            if (sad == 0) {
                break;
            }
        }
    }

    if (best_sad < 0) {
        return false;
    }

    for (int yy = 0; yy < 8; yy++) {
        memcpy(recon + yy * 8, prev_plane + (best_y0 + yy) * w + best_x0, 8);
    }

    token[0] = M_MVCOPY;
    token[1] = (u8)(s8)best_mx;
    token[2] = (u8)(s8)best_my;

    return true;
}


static bool find_best_mv_64(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    const MVList *mvlist,
    int search_cutoff,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    if (!prev_plane || !mvlist) {
        return false;
    }

    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int best_sad = -1;
    int best_mx = 0;
    int best_my = 0;
    int best_x0 = dst_x0;
    int best_y0 = dst_y0;

    if (search_cutoff <= 0) {
        search_cutoff = 4096;
    }

    for (int k = 0; k < mvlist->count; k++) {
        int mx = mvlist->mx[k];
        int my = mvlist->my[k];

        int sx = dst_x0 + mx;
        int sy = dst_y0 + my;

        if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
            continue;
        }

        int limit = best_sad >= 0 ? best_sad : search_cutoff;
        int sad = sad8x8_capped_exact(cur, prev_plane, w, sx, sy, limit);
        if (sad < 0) continue;

        if (best_sad < 0 || sad < best_sad) {
            best_sad = sad;
            best_mx = mx;
            best_my = my;
            best_x0 = sx;
            best_y0 = sy;

            if (sad == 0) {
                break;
            }
        }
    }

    if (best_sad < 0) {
        return false;
    }

    for (int yy = 0; yy < 8; yy++) {
        memcpy(
            out_pred + yy * 8,
            prev_plane + (best_y0 + yy) * w + best_x0,
            8
        );
    }

    *out_mx = best_mx;
    *out_my = best_my;
    *out_sad = best_sad;

    return true;
}

/* --motion-search diamond/fast: SAD over an 8x8 block against a fixed
   (sx,sy) source position, aborting (returns -1) as soon as the running
   sum exceeds limit. Same formula and early-abort convention as the
   exhaustive scan in find_best_mv_64 above, factored out so the new
   iterative search below doesn't duplicate/diverge from it while also
   not touching find_best_mv_64 itself (full mode stays byte-identical). */
static int sad8x8_capped(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int sx,
    int sy,
    int limit
) {
    return sad8x8_capped_exact(cur, prev_plane, w, sx, sy, limit);
}

/* --motion-search diamond/fast: iterative cross/diamond step search.
   Starts at the zero vector, tests the 4 neighbors at +-step on each axis,
   recenters on any improvement (walking across the search window while
   recenter_at_same_step is true), and halves step when a round finds no
   improvement, until step reaches 0. Bounded to a small, predictable
   number of SAD probes (tens, not hundreds) instead of every vector in
   +-mv_range -- unlike find_best_mv_64's exhaustive mvlist scan, this can
   miss the true global-minimum vector on complex motion, which is the
   quality/size tradeoff --motion-search diamond/fast opt into.
   Bounds-checked the same way as find_best_mv_64: any candidate outside
   the plane or outside +-mv_range is skipped, so this can never emit an
   out-of-range or out-of-bounds motion vector. */
static bool find_best_mv_64_diamond(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    int mv_range,
    int search_cutoff,
    int start_step,
    bool recenter_at_same_step,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    if (!prev_plane) {
        return false;
    }

    if (mv_range < 0) {
        mv_range = 0;
    }

    if (search_cutoff <= 0) {
        search_cutoff = 4096;
    }

    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    /* The zero vector always lands inside the block's own plane bounds,
       so this initial probe is always valid (it may still come back -1
       if it exceeds search_cutoff, exactly like find_best_mv_64 treats an
       aborted probe as "no candidate yet"). */
    int cur_mx = 0;
    int cur_my = 0;
    int best_sad = sad8x8_capped(cur, prev_plane, w, dst_x0, dst_y0, search_cutoff);
    int best_mx = 0;
    int best_my = 0;

    int step = start_step;
    if (step > mv_range) step = mv_range;
    if (step < 1) step = 0;

    while (step >= 1 && best_sad != 0) {
        int cand_mx[4] = { cur_mx + step, cur_mx - step, cur_mx, cur_mx };
        int cand_my[4] = { cur_my, cur_my, cur_my + step, cur_my - step };
        bool improved = false;

        for (int k = 0; k < 4; k++) {
            int mx = clamp_int(cand_mx[k], -mv_range, mv_range);
            int my = clamp_int(cand_my[k], -mv_range, mv_range);

            if (mx == cur_mx && my == cur_my) {
                continue;
            }

            int sx = dst_x0 + mx;
            int sy = dst_y0 + my;

            if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
                continue;
            }

            int limit = (best_sad >= 0) ? best_sad : search_cutoff;
            int sad = sad8x8_capped(cur, prev_plane, w, sx, sy, limit);

            if (sad < 0) {
                continue;
            }

            if (best_sad < 0 || sad < best_sad) {
                best_sad = sad;
                best_mx = mx;
                best_my = my;
                improved = true;
            }
        }

        cur_mx = best_mx;
        cur_my = best_my;

        if (!(improved && recenter_at_same_step)) {
            step /= 2;
        }
    }

    if (best_sad < 0) {
        return false;
    }

    for (int yy = 0; yy < 8; yy++) {
        memcpy(
            out_pred + yy * 8,
            prev_plane + (dst_y0 + best_my + yy) * w + (dst_x0 + best_mx),
            8
        );
    }

    *out_mx = best_mx;
    *out_my = best_my;
    *out_sad = best_sad;

    return true;
}

/* --motion-search hybrid: identical cross/diamond walk to
   find_best_mv_64_diamond, but starting from a caller-supplied seed vector
   instead of always the zero vector. Used by find_best_mv_64_hybrid() below
   to also try descending from the frame's global MV estimate, which can
   land much closer to the true optimum on panning/high-motion content where
   a zero-seeded walk's coarse steps risk settling in the wrong local
   minimum. Bounds/range-checked the same way: the seed is clamped to
   +-mv_range, and if the clamped seed still lands outside the plane (can
   happen near edges with a large global MV), this falls back to the zero
   vector for the initial probe -- so it can never emit an out-of-range or
   out-of-bounds motion vector, exactly like find_best_mv_64_diamond. */
static bool find_best_mv_64_seeded(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    int mv_range,
    int search_cutoff,
    int start_step,
    bool recenter_at_same_step,
    int seed_mx,
    int seed_my,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    if (!prev_plane) {
        return false;
    }

    if (mv_range < 0) {
        mv_range = 0;
    }

    if (search_cutoff <= 0) {
        search_cutoff = 4096;
    }

    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int cur_mx = clamp_int(seed_mx, -mv_range, mv_range);
    int cur_my = clamp_int(seed_my, -mv_range, mv_range);

    int seed_sx = dst_x0 + cur_mx;
    int seed_sy = dst_y0 + cur_my;

    int best_sad;
    int best_mx;
    int best_my;

    if (seed_sx >= 0 && seed_sy >= 0 && seed_sx + 8 <= w && seed_sy + 8 <= h) {
        best_sad = sad8x8_capped(cur, prev_plane, w, seed_sx, seed_sy, search_cutoff);
        best_mx = cur_mx;
        best_my = cur_my;
    } else {
        cur_mx = 0;
        cur_my = 0;
        best_sad = sad8x8_capped(cur, prev_plane, w, dst_x0, dst_y0, search_cutoff);
        best_mx = 0;
        best_my = 0;
    }

    int step = start_step;
    if (step > mv_range) step = mv_range;
    if (step < 1) step = 0;

    while (step >= 1 && best_sad != 0) {
        int cand_mx[4] = { cur_mx + step, cur_mx - step, cur_mx, cur_mx };
        int cand_my[4] = { cur_my, cur_my, cur_my + step, cur_my - step };
        bool improved = false;

        for (int k = 0; k < 4; k++) {
            int mx = clamp_int(cand_mx[k], -mv_range, mv_range);
            int my = clamp_int(cand_my[k], -mv_range, mv_range);

            if (mx == cur_mx && my == cur_my) {
                continue;
            }

            int sx = dst_x0 + mx;
            int sy = dst_y0 + my;

            if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
                continue;
            }

            int limit = (best_sad >= 0) ? best_sad : search_cutoff;
            int sad = sad8x8_capped(cur, prev_plane, w, sx, sy, limit);

            if (sad < 0) {
                continue;
            }

            if (best_sad < 0 || sad < best_sad) {
                best_sad = sad;
                best_mx = mx;
                best_my = my;
                improved = true;
            }
        }

        cur_mx = best_mx;
        cur_my = best_my;

        if (!(improved && recenter_at_same_step)) {
            step /= 2;
        }
    }

    if (best_sad < 0) {
        return false;
    }

    for (int yy = 0; yy < 8; yy++) {
        memcpy(
            out_pred + yy * 8,
            prev_plane + (dst_y0 + best_my + yy) * w + (dst_x0 + best_mx),
            8
        );
    }

    *out_mx = best_mx;
    *out_my = best_my;
    *out_sad = best_sad;

    return true;
}

/* --motion-search hybrid: run find_best_mv_64_seeded from both the zero
   vector and the frame's global MV estimate (g_mx,g_my), keep whichever
   converges to the lower SAD. Costs roughly 2x a single diamond search per
   block (still tens of probes, nowhere near find_best_mv_64's O(mv_range^2)
   exhaustive scan) in exchange for recovering much of full search's quality
   on panning/high-motion content. */
static bool find_best_mv_64_hybrid(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    int mv_range,
    int search_cutoff,
    int g_mx,
    int g_my,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    int zero_mx = 0, zero_my = 0, zero_sad = 0;
    u8 zero_pred[64];
    bool zero_found = find_best_mv_64_seeded(
        cur, prev_plane, w, h, bx, by, mv_range, search_cutoff,
        4, true, 0, 0, &zero_mx, &zero_my, zero_pred, &zero_sad
    );

    if (g_mx == 0 && g_my == 0) {
        /* Global-seeded search would repeat the zero-seeded one above --
           skip the redundant second search. */
        if (!zero_found) {
            return false;
        }

        *out_mx = zero_mx;
        *out_my = zero_my;
        *out_sad = zero_sad;
        memcpy(out_pred, zero_pred, 64);
        return true;
    }

    int gmv_mx = 0, gmv_my = 0, gmv_sad = 0;
    u8 gmv_pred[64];
    bool gmv_found = find_best_mv_64_seeded(
        cur, prev_plane, w, h, bx, by, mv_range, search_cutoff,
        4, true, g_mx, g_my, &gmv_mx, &gmv_my, gmv_pred, &gmv_sad
    );

    if (!zero_found && !gmv_found) {
        return false;
    }

    if (zero_found && (!gmv_found || zero_sad <= gmv_sad)) {
        *out_mx = zero_mx;
        *out_my = zero_my;
        *out_sad = zero_sad;
        memcpy(out_pred, zero_pred, 64);
    } else {
        *out_mx = gmv_mx;
        *out_my = gmv_my;
        *out_sad = gmv_sad;
        memcpy(out_pred, gmv_pred, 64);
    }

    return true;
}

/* --motion-search hybrid: bounded +-refine_range local search around an
   existing MV candidate. Only invoked (see encode_plane) when the hybrid
   search's result still looks unreliable and the per-plane escalation
   budget hasn't been exhausted, so this can never degrade toward anything
   resembling the full O(mv_range^2) scan -- worst case is a small fixed
   window (2*refine_range+1)^2 - 1 probes, capped in count by the budget. */
static bool find_best_mv_64_local_refine(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    int mv_range,
    int refine_range,
    int center_mx,
    int center_my,
    int center_sad,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int best_mx = center_mx;
    int best_my = center_my;
    int best_sad = center_sad;

    for (int dy = -refine_range; dy <= refine_range; dy++) {
        for (int dx = -refine_range; dx <= refine_range; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }

            int mx = clamp_int(center_mx + dx, -mv_range, mv_range);
            int my = clamp_int(center_my + dy, -mv_range, mv_range);

            if (mx == best_mx && my == best_my) {
                continue;
            }

            int sx = dst_x0 + mx;
            int sy = dst_y0 + my;

            if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
                continue;
            }

            int sad = sad8x8_capped(cur, prev_plane, w, sx, sy, best_sad);

            if (sad < 0) {
                continue;
            }

            if (sad < best_sad) {
                best_sad = sad;
                best_mx = mx;
                best_my = my;
            }
        }
    }

    if (best_mx == center_mx && best_my == center_my) {
        return false;
    }

    for (int yy = 0; yy < 8; yy++) {
        memcpy(
            out_pred + yy * 8,
            prev_plane + (dst_y0 + best_my + yy) * w + (dst_x0 + best_mx),
            8
        );
    }

    *out_mx = best_mx;
    *out_my = best_my;
    *out_sad = best_sad;

    return true;
}

/* MIVF_HIERARCHICAL_MOTION_V1
   --motion-search hierarchical: deterministic coarse-to-fine integer search.

   Goal: approach find_best_mv_64's exhaustive quality across +-mv_range while
   testing only a small, bounded number of candidates per block (tens, not
   O(mv_range^2)).

   Stage 1 (seeds): (0,0) and the frame's global MV estimate (g_mx,g_my) --
   the same two seeds find_best_mv_64_hybrid uses.
   Stage 2 (coarse grid): axis values are 0, +-4, +-8, ... out to +-mv_range,
   always also including the exact +-mv_range extremes even if not a multiple
   of 4 (e.g. mv_range=12 -> -12,-8,-4,0,4,8,12 -> at most 7x7=49 candidates;
   mv_range=8 -> -8,-4,0,4,8 -> at most 5x5=25), crossed on both axes.
   Stage 3: +-4 window, step 2, around the coarse-stage winner (<=24 candidates).
   Stage 4: +-2 window, step 1, around the step-2 winner (<=24 candidates).

   Every (mx,my) actually considered is recorded in a small fixed-size stack
   array (mivf_hier_try's seen_mx/seen_my) and never evaluated twice for the
   same block -- the same vector commonly recurs across stages, after
   +-mv_range clamping, or after independently landing on the same clamped
   point from different raw offsets.

   Conventions matched exactly to the rest of this file's motion search:
     - clamp_int() bounds every raw candidate to +-mv_range, exactly like
       find_best_mv_64_diamond/seeded/local_refine.
     - Any candidate whose 8x8 window would read outside the reference plane
       is skipped outright (never clamped further), exactly like every other
       search function in this file.
     - sad8x8_capped() is reused for every SAD evaluation -- same early-abort
       formula and cutoff convention as find_best_mv_64/diamond/seeded.
     - Only a STRICTLY lower SAD replaces the incumbent best (a tie never
       replaces it) -- matches find_best_mv_64's "sad < best_sad" and
       find_best_mv_64_diamond/seeded's identical rule.
     - An exact SAD==0 match stops the ENTIRE search immediately (no further
       stage or candidate is evaluated) -- matches find_best_mv_64's
       "if (sad == 0) break;" and diamond/seeded's "best_sad != 0" loop guard.

   Bounded candidate count (documented, not just asserted): seeds <= 2, coarse
   grid <= MIVF_HIER_MAX_AXIS^2 = 256, stage-3 refine <= 24, stage-4 refine
   <= 24. MIVF_HIER_MAX_AXIS is sized generously (16) so the coarse-grid cap
   safely dominates any --mv-range this encoder is realistically run with
   (mv_range=12 only ever produces a 7x7=49 coarse grid; mv_range=16 produces
   9x9=81); MIVF_HIER_MAX_CANDIDATES (306) is the sum of all four stages'
   worst cases and sizes the fixed dedup array below -- if an unrealistically
   large --mv-range ever produced more distinct axis values than
   MIVF_HIER_MAX_AXIS allows, mivf_hier_try's coarse-grid loop simply stops
   adding further axis values rather than overflowing any buffer; this can
   only ever make the coarse grid coarser, never incorrect or unsafe. */
#define MIVF_HIER_MAX_AXIS 16
#define MIVF_HIER_MAX_CANDIDATES (2 + (MIVF_HIER_MAX_AXIS * MIVF_HIER_MAX_AXIS) + 24 + 24)

/* MIVF_OPT1_CANDIDATE_BITMAP_V1: O(1) dedup, replacing the O(n) linear scan
   this function used to be (mivf_hier_seen + seen_mx[]/seen_my[]/seen_count).
   A candidate's clamped (mx,my) maps to exactly one cell of a fixed stack
   bitmap via (my+mv_range)*side + (mx+mv_range), side = 2*mv_range+1 -- the
   same indexing scheme used elsewhere for small dense integer-vector lookup
   tables. Two calls with the same clamped (mx,my) always produce the same
   index, so "already seen" is exact, not approximate -- this changes only
   HOW a repeat is detected, never WHICH candidates count as repeats or the
   order they're proposed in, so it cannot change the final selected vector
   (see this file's own commit history / opt1_candidate_bitmap_patch.py for
   the full argument). Only valid while mv_range <= MIVF_HIER_BITMAP_MAX_RANGE
   (32 -- far above the mv_range<=16 used anywhere in this project); beyond
   that, bitmap_side is 0 and dedup is silently skipped rather than sized
   dynamically, which can only forgo some of the speedup, never correctness
   (mivf_hier_try's SAD-update rule already tolerates being called twice on
   the same candidate). */
#define MIVF_HIER_BITMAP_MAX_RANGE 32
#define MIVF_HIER_BITMAP_MAX_SIDE (2 * MIVF_HIER_BITMAP_MAX_RANGE + 1)
#define MIVF_HIER_BITMAP_MAX_CELLS (MIVF_HIER_BITMAP_MAX_SIDE * MIVF_HIER_BITMAP_MAX_SIDE)

static void mivf_hier_try(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int dst_x0,
    int dst_y0,
    int mv_range,
    int search_cutoff,
    int raw_mx,
    int raw_my,
    u8 seen_bitmap[],
    int bitmap_side,
    int *best_mx,
    int *best_my,
    int *best_sad
) {
    int mx = clamp_int(raw_mx, -mv_range, mv_range);
    int my = clamp_int(raw_my, -mv_range, mv_range);

    if (bitmap_side > 0) {
        int idx = (my + mv_range) * bitmap_side + (mx + mv_range);

        if (seen_bitmap[idx]) {
            return;
        }

        seen_bitmap[idx] = 1;
    }

    int sx = dst_x0 + mx;
    int sy = dst_y0 + my;

    if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
        return;
    }

    int limit = (*best_sad >= 0) ? *best_sad : search_cutoff;
    int sad = sad8x8_capped(cur, prev_plane, w, sx, sy, limit);

    if (sad < 0) {
        return;
    }

    if (*best_sad < 0 || sad < *best_sad) {
        *best_sad = sad;
        *best_mx = mx;
        *best_my = my;
    }
}

/* MIVF_HIERARCHICAL_MOTION_V2 diagnostic counters -- purely additive
   instrumentation. Only find_best_mv_64_hierarchical ever touches these, so
   they cannot affect full/diamond/fast/hybrid in any way; they do not
   participate in any RDO decision or token output, only end-of-run STATS
   reporting. */

typedef struct {
    s16 mx;
    s16 my;
    int sad;
} MivfHierV2Record;

/* Stage 1 (2 seeds) + stage 2 (<=9x9=81, local exhaustive +-4 center) +
   stage 3 (<=MIVF_HIER_MAX_AXIS^2=256, step-4 wide grid). */
#define MIVF_HIER_V2_MAX_RECORDS (2 + 81 + (MIVF_HIER_MAX_AXIS * MIVF_HIER_MAX_AXIS))

/* Used only for stages 1-3 (the scan that stage 4 later picks two distinct
   basins from). Deliberately uses the fixed, generous search_cutoff (not a
   tightening best-so-far cap) for every candidate, unlike mivf_hier_try --
   this is what makes every recorded SAD directly, fairly comparable to
   every other recorded SAD, which stage 4's basin selection depends on. A
   tightening cap here would let candidates abort at different, inconsistent
   limits depending on scan order, making cross-basin comparisons unsound.
   Same clamp/dedup/bounds/tie conventions as mivf_hier_try otherwise. */
static void mivf_hier_v2_scan_try(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int dst_x0,
    int dst_y0,
    int mv_range,
    int search_cutoff,
    int raw_mx,
    int raw_my,
    u8 seen_bitmap[],
    int bitmap_side,
    MivfHierV2Record records[],
    int *record_count,
    int *scan_best_sad
) {
    int mx = clamp_int(raw_mx, -mv_range, mv_range);
    int my = clamp_int(raw_my, -mv_range, mv_range);

    if (bitmap_side > 0) {
        int idx = (my + mv_range) * bitmap_side + (mx + mv_range);

        if (seen_bitmap[idx]) {
            return;
        }

        seen_bitmap[idx] = 1;
    }

    int sx = dst_x0 + mx;
    int sy = dst_y0 + my;

    if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
        return;
    }

    int sad = sad8x8_capped(cur, prev_plane, w, sx, sy, search_cutoff);

    if (sad < 0) {
        return;
    }

    if (*record_count < MIVF_HIER_V2_MAX_RECORDS) {
        records[*record_count].mx = (s16)mx;
        records[*record_count].my = (s16)my;
        records[*record_count].sad = sad;
        (*record_count)++;
    }

    if (*scan_best_sad < 0 || sad < *scan_best_sad) {
        *scan_best_sad = sad;
    }
}

/* MIVF_HIERARCHICAL_MOTION_V2: addresses v1's diagnosed wrong-basin problem
   (see mivf_hierarchical_motion_patch.py / the v1 matrix report: hierarchical
   v1 missed both the primary target vs full range-12 and the minimum bar vs
   full range-8) without making the whole grid drastically denser.

   Stage 1 (seeds): zero vector, global MV estimate.
   Stage 2 (local exhaustive center): every legal (mx,my) in [-4,+4]x[-4,+4]
   (clamped to +-mv_range for small ranges) -- guarantees full coverage of
   common low-motion vectors, the exact region v1's step-4-only coarse grid
   could skip between grid points.
   Stage 3 (step-4 wide grid): same wide grid v1 used across the full
   configured +-mv_range, always including zero and both range extremes.
   Stage 4: from every candidate recorded in stages 1-3, pick the single
   best (best1) and the best candidate from a DISTINCT basin (best2 --
   distinct meaning abs(mx-best1.mx)>=4 or abs(my-best1.my)>=4), so
   refinement isn't wasted re-polishing the same neighborhood twice.
   Stage 5: +-4 step-2 refine around best1, and independently around best2.
   Stage 6: keep whichever refined candidate has the strictly lower SAD.
   Stage 7: +-2 step-1 refine around the stage-6 winner.
   Stage 8: any SAD==0 found at any point stops the whole search immediately
   (matches v1 and every other search function in this file).

   Bounded candidate count: stages 1-3 record at most
   MIVF_HIER_V2_MAX_RECORDS (339) candidates; stage 5 evaluates at most 24
   per basin (48 total); stage 7 at most 24. All dedup uses the same O(1)
   bitmap as opt1_candidate_bitmap, shared across the whole function call, so
   no stage ever re-evaluates a point any earlier stage already tried. */
static bool find_best_mv_64_hierarchical(
    const u8 cur[64],
    const u8 *prev_plane,
    int w,
    int h,
    int bx,
    int by,
    int mv_range,
    int search_cutoff,
    int g_mx,
    int g_my,
    int *out_mx,
    int *out_my,
    u8 out_pred[64],
    int *out_sad
) {
    if (!prev_plane) {
        return false;
    }

    if (mv_range < 0) {
        mv_range = 0;
    }

    if (search_cutoff <= 0) {
        search_cutoff = 4096;
    }

    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    u8 seen_bitmap[MIVF_HIER_BITMAP_MAX_CELLS];
    int bitmap_side = 0;

    if (mv_range >= 0 && mv_range <= MIVF_HIER_BITMAP_MAX_RANGE) {
        bitmap_side = 2 * mv_range + 1;
        memset(seen_bitmap, 0, (size_t)bitmap_side * (size_t)bitmap_side);
    }


    MivfHierV2Record records[MIVF_HIER_V2_MAX_RECORDS];
    int record_count = 0;
    int scan_best_sad = -1;

    /* Stage 1: seeds. */
    mivf_hier_v2_scan_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
        0, 0, seen_bitmap, bitmap_side, records, &record_count, &scan_best_sad);

    if (scan_best_sad != 0 && (g_mx != 0 || g_my != 0)) {
        mivf_hier_v2_scan_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
            g_mx, g_my, seen_bitmap, bitmap_side, records, &record_count, &scan_best_sad);
    }

    /* Stage 2: local exhaustive center, +-4 on both axes. */
    if (scan_best_sad != 0) {
        int center_range = (mv_range < 4) ? mv_range : 4;

        for (int dy = -center_range; dy <= center_range && scan_best_sad != 0; dy++) {
            for (int dx = -center_range; dx <= center_range && scan_best_sad != 0; dx++) {
                mivf_hier_v2_scan_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
                    dx, dy, seen_bitmap, bitmap_side, records, &record_count, &scan_best_sad);
            }
        }
    }

    /* Stage 3: step-4 wide grid, always including zero and both legal
       extremes on each axis. */
    if (scan_best_sad != 0) {
        int axis[MIVF_HIER_MAX_AXIS];
        int axis_count = 0;

        axis[axis_count++] = 0;

        for (int r = 4; r <= mv_range && axis_count + 2 <= MIVF_HIER_MAX_AXIS; r += 4) {
            axis[axis_count++] = r;
            axis[axis_count++] = -r;
        }

        if (mv_range > 0 && axis_count + 2 <= MIVF_HIER_MAX_AXIS) {
            bool have_pos = false;
            bool have_neg = false;

            for (int i = 0; i < axis_count; i++) {
                if (axis[i] == mv_range) have_pos = true;
                if (axis[i] == -mv_range) have_neg = true;
            }

            if (!have_pos) axis[axis_count++] = mv_range;
            if (!have_neg) axis[axis_count++] = -mv_range;
        }

        for (int ay = 0; ay < axis_count && scan_best_sad != 0; ay++) {
            for (int ax = 0; ax < axis_count && scan_best_sad != 0; ax++) {
                mivf_hier_v2_scan_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
                    axis[ax], axis[ay], seen_bitmap, bitmap_side, records, &record_count, &scan_best_sad);
            }
        }
    }

    if (record_count == 0) {
        return false;
    }


    /* Stage 4: best1 = global min (first on tie); best2 = min among records
       in a distinct basin from best1 (first on tie). */
    int best1_idx = 0;

    for (int i = 1; i < record_count; i++) {
        if (records[i].sad < records[best1_idx].sad) {
            best1_idx = i;
        }
    }

    int best2_idx = -1;

    for (int i = 0; i < record_count; i++) {
        if (i == best1_idx) {
            continue;
        }

        int dmx = records[i].mx - records[best1_idx].mx;
        int dmy = records[i].my - records[best1_idx].my;

        if (dmx < 0) dmx = -dmx;
        if (dmy < 0) dmy = -dmy;

        if (dmx < 4 && dmy < 4) {
            continue;
        }

        if (best2_idx < 0 || records[i].sad < records[best2_idx].sad) {
            best2_idx = i;
        }
    }

    if (best2_idx >= 0) {
    }

    int best_mx = records[best1_idx].mx;
    int best_my = records[best1_idx].my;
    int best_sad = records[best1_idx].sad;

    if (best_sad == 0) {
        goto hier_v2_done;
    }

    /* Stage 5+6: +-4 step-2 refine around best1, and independently around
       best2 (if distinct), then keep whichever has the strictly lower SAD.
       Reuses mivf_hier_try (opt1's O(1)-deduped, tightening-cap search) --
       cross-basin comparison is already settled by stage 4, so refinement
       only needs the single best answer within each basin's neighborhood. */
    {
        int refined1_mx = best_mx, refined1_my = best_my, refined1_sad = best_sad;

        for (int dy = -4; dy <= 4 && refined1_sad != 0; dy += 2) {
            for (int dx = -4; dx <= 4 && refined1_sad != 0; dx += 2) {
                if (dx == 0 && dy == 0) continue;
                mivf_hier_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
                    best_mx + dx, best_my + dy, seen_bitmap, bitmap_side, &refined1_mx, &refined1_my, &refined1_sad);
            }
        }

        if (refined1_mx != best_mx || refined1_my != best_my) {
        }

        if (best2_idx >= 0 && refined1_sad != 0) {
            int base2_mx = records[best2_idx].mx;
            int base2_my = records[best2_idx].my;
            int refined2_mx = base2_mx, refined2_my = base2_my, refined2_sad = records[best2_idx].sad;


            for (int dy = -4; dy <= 4 && refined2_sad != 0; dy += 2) {
                for (int dx = -4; dx <= 4 && refined2_sad != 0; dx += 2) {
                    if (dx == 0 && dy == 0) continue;
                    mivf_hier_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
                        base2_mx + dx, base2_my + dy, seen_bitmap, bitmap_side, &refined2_mx, &refined2_my, &refined2_sad);
                }
            }

            if (refined2_sad < refined1_sad) {
                best_mx = refined2_mx;
                best_my = refined2_my;
                best_sad = refined2_sad;
            } else {
                best_mx = refined1_mx;
                best_my = refined1_my;
                best_sad = refined1_sad;
            }
        } else {
            best_mx = refined1_mx;
            best_my = refined1_my;
            best_sad = refined1_sad;
        }
    }

    /* Stage 7: +-2 step-1 refine around the stage-6 winner. */
    if (best_sad != 0) {
        int pre_step1_mx = best_mx, pre_step1_my = best_my;

        for (int dy = -2; dy <= 2 && best_sad != 0; dy++) {
            for (int dx = -2; dx <= 2 && best_sad != 0; dx++) {
                if (dx == 0 && dy == 0) continue;
                mivf_hier_try(cur, prev_plane, w, h, dst_x0, dst_y0, mv_range, search_cutoff,
                    best_mx + dx, best_my + dy, seen_bitmap, bitmap_side, &best_mx, &best_my, &best_sad);
            }
        }

        if (best_mx != pre_step1_mx || best_my != pre_step1_my) {
        }
    }

hier_v2_done:
    for (int yy = 0; yy < 8; yy++) {
        memcpy(
            out_pred + yy * 8,
            prev_plane + (dst_y0 + best_my + yy) * w + (dst_x0 + best_mx),
            8
        );
    }

    *out_mx = best_mx;
    *out_my = best_my;
    *out_sad = best_sad;

    return true;
}

static bool try_mvqres_64(
    const u8 cur[64],
    const u8 pred[64],
    int mx,
    int my,
    int qres_thresh,
    u8 token[20],
    u8 recon[64]
) {
    if (qres_thresh <= 0) {
        return false;
    }

    int global_delta = avg_delta_64(cur, pred);
    s8 residuals[16];

    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            int err_sum = 0;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 2; xx++) {
                    int x = cx * 2 + xx;
                    int y = cy * 2 + yy;
                    int idx = y * 8 + x;

                    int predicted = clamp_u8((int)pred[idx] + global_delta);
                    err_sum += (int)cur[idx] - predicted;
                }
            }

            residuals[cy * 4 + cx] = clamp_s8(round_div_int(err_sum, 4));
        }
    }

    int sad = 0;

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            int cell = cell_y + (x >> 1);

            int v = clamp_u8(
                (int)pred[idx] +
                global_delta +
                (int)residuals[cell]
            );

            recon[idx] = (u8)v;

            int e = (int)cur[idx] - v;
            if (e < 0) e = -e;
            sad += e;
        }
    }

    if (sad > qres_thresh) {
        return false;
    }

    token[0] = M_MVQRES;
    token[1] = (u8)(s8)mx;
    token[2] = (u8)(s8)my;
    token[3] = (u8)(s8)global_delta;

    for (int i = 0; i < 16; i++) {
        token[4 + i] = (u8)residuals[i];
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* Plane encoder                                                             */
/* ------------------------------------------------------------------------- */

static void plane_enc_init(PlaneEnc *e, size_t cap) {
    buf_init(&e->payload, cap);
    memset(&e->stats, 0, sizeof(e->stats));
    e->pending_skip = 0;
}

static void plane_enc_free(PlaneEnc *e) {
    buf_free(&e->payload);
    e->pending_skip = 0;
}

static void plane_flush_skips(PlaneEnc *e) {
    int n = e->pending_skip;

    while (n > 0) {
        if (n >= 3) {
            int run = n;
            if (run > 256) run = 256;

            buf_put_u8(&e->payload, M_RUN_SKIP);
            buf_put_u8(&e->payload, (u8)(run - 1));

            e->stats.run_tokens++;
            e->stats.run_blocks += (u32)run;

            n -= run;
        } else {
            buf_put_u8(&e->payload, M_SKIP);
            n--;
        }
    }

    e->pending_skip = 0;
}

static void plane_emit_skip(PlaneEnc *e) {
    e->pending_skip++;
}

static void plane_emit_token(PlaneEnc *e, const u8 *p, int n) {
    plane_flush_skips(e);
    buf_put_bytes(&e->payload, p, (size_t)n);
}


/* ------------------------------------------------------------------------- */
/* HFIX28 RDO helpers                                                        */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* HFIX38A: Normalized Sum of Squared Differences visual distortion           */
/*                                                                           */
/* D = (SSD + 4) >> 3                                                        */
/*                                                                           */
/* This keeps the scale near SAD for average per-pixel error around 8, while */
/* penalizing large visible block cliffs much harder than SAD.                */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* HFIX39A: 8x8 block variance for conservative local transform-QP selection  */
/* ------------------------------------------------------------------------- */
static int calculate_block_variance_64(const u8 block[64]) {
    int sum = 0;

    for (int i = 0; i < 64; i++) {
        sum += block[i];
    }

    int mean = sum / 64;
    int variance_sum = 0;

    for (int i = 0; i < 64; i++) {
        int diff = (int)block[i] - mean;
        variance_sum += diff * diff;
    }

    return variance_sum / 64;
}

static int rdo_nssd_64(const u8 a[64], const u8 b[64]) {
    int ssd = 0;

    for (int i = 0; i < 64; i++) {
        int d = (int)a[i] - (int)b[i];
        ssd += d * d;
    }

    return (ssd + 4) >> 3;
}

static int rdo_sad_64(const u8 a[64], const u8 b[64]) {
    int sad = 0;

    for (int i = 0; i < 64; i++) {
        int d = (int)a[i] - (int)b[i];

        if (d < 0) {
            d = -d;
        }

        sad += d;
    }

    return sad;
}

static int rdo_make_qres_64(
    const u8 cur[64],
    const u8 prev[64],
    u8 token[18],
    u8 recon[64]
) {
    int global_delta = avg_delta_64(cur, prev);
    s8 residuals[16];

    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            int err_sum = 0;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 2; xx++) {
                    int x = cx * 2 + xx;
                    int y = cy * 2 + yy;
                    int idx = y * 8 + x;

                    int pred = clamp_u8((int)prev[idx] + global_delta);
                    err_sum += (int)cur[idx] - pred;
                }
            }

            residuals[cy * 4 + cx] = clamp_s8(round_div_int(err_sum, 4));
        }
    }

    int sad = 0;

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            int cell = cell_y + (x >> 1);

            int v = clamp_u8(
                (int)prev[idx] +
                global_delta +
                (int)residuals[cell]
            );

            recon[idx] = (u8)v;

            int e = (int)cur[idx] - v;
            if (e < 0) {
                e = -e;
            }

            sad += e;
        }
    }

    token[0] = M_QRES;
    token[1] = (u8)(s8)global_delta;

    for (int i = 0; i < 16; i++) {
        token[2 + i] = (u8)residuals[i];
    }

    return sad;
}

static int rdo_make_mvqres_64(
    const u8 cur[64],
    const u8 pred[64],
    int mx,
    int my,
    u8 token[20],
    u8 recon[64]
) {
    int global_delta = avg_delta_64(cur, pred);
    s8 residuals[16];

    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            int err_sum = 0;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 2; xx++) {
                    int x = cx * 2 + xx;
                    int y = cy * 2 + yy;
                    int idx = y * 8 + x;

                    int predicted = clamp_u8((int)pred[idx] + global_delta);
                    err_sum += (int)cur[idx] - predicted;
                }
            }

            residuals[cy * 4 + cx] = clamp_s8(round_div_int(err_sum, 4));
        }
    }

    int sad = 0;

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            int cell = cell_y + (x >> 1);

            int v = clamp_u8(
                (int)pred[idx] +
                global_delta +
                (int)residuals[cell]
            );

            recon[idx] = (u8)v;

            int e = (int)cur[idx] - v;
            if (e < 0) {
                e = -e;
            }

            sad += e;
        }
    }

    token[0] = M_MVQRES;
    token[1] = (u8)(s8)mx;
    token[2] = (u8)(s8)my;
    token[3] = (u8)(s8)global_delta;

    for (int i = 0; i < 16; i++) {
        token[4 + i] = (u8)residuals[i];
    }

    return sad;
}


/* ------------------------------------------------------------------------- */
/* HFIX29A 4x4 integer transform residuals                                   */
/* ------------------------------------------------------------------------- */

static void transform4_forward(const int16_t input[16], int16_t output[16]) {
    int tmp[16];

    for (int y = 0; y < 4; y++) {
        int x0 = input[y * 4 + 0];
        int x1 = input[y * 4 + 1];
        int x2 = input[y * 4 + 2];
        int x3 = input[y * 4 + 3];

        int a0 = x0 + x3;
        int a1 = x1 + x2;
        int a2 = x1 - x2;
        int a3 = x0 - x3;

        tmp[y * 4 + 0] = a0 + a1;
        tmp[y * 4 + 1] = a3 + a2;
        tmp[y * 4 + 2] = a0 - a1;
        tmp[y * 4 + 3] = a3 - a2;
    }

    for (int x = 0; x < 4; x++) {
        int x0 = tmp[0 * 4 + x];
        int x1 = tmp[1 * 4 + x];
        int x2 = tmp[2 * 4 + x];
        int x3 = tmp[3 * 4 + x];

        int a0 = x0 + x3;
        int a1 = x1 + x2;
        int a2 = x1 - x2;
        int a3 = x0 - x3;

        output[0 * 4 + x] = (int16_t)(a0 + a1);
        output[1 * 4 + x] = (int16_t)(a3 + a2);
        output[2 * 4 + x] = (int16_t)(a0 - a1);
        output[3 * 4 + x] = (int16_t)(a3 - a2);
    }
}

static void transform4_inverse(const int16_t input[16], int16_t output[16]) {
    int tmp[16];

    for (int y = 0; y < 4; y++) {
        int x0 = input[y * 4 + 0];
        int x1 = input[y * 4 + 1];
        int x2 = input[y * 4 + 2];
        int x3 = input[y * 4 + 3];

        int a0 = x0 + x3;
        int a1 = x1 + x2;
        int a2 = x1 - x2;
        int a3 = x0 - x3;

        tmp[y * 4 + 0] = a0 + a1;
        tmp[y * 4 + 1] = a3 + a2;
        tmp[y * 4 + 2] = a0 - a1;
        tmp[y * 4 + 3] = a3 - a2;
    }

    for (int x = 0; x < 4; x++) {
        int x0 = tmp[0 * 4 + x];
        int x1 = tmp[1 * 4 + x];
        int x2 = tmp[2 * 4 + x];
        int x3 = tmp[3 * 4 + x];

        int a0 = x0 + x3;
        int a1 = x1 + x2;
        int a2 = x1 - x2;
        int a3 = x0 - x3;

        output[0 * 4 + x] = (int16_t)((a0 + a1 + 8) >> 4);
        output[1 * 4 + x] = (int16_t)((a3 + a2 + 8) >> 4);
        output[2 * 4 + x] = (int16_t)((a0 - a1 + 8) >> 4);
        output[3 * 4 + x] = (int16_t)((a3 - a2 + 8) >> 4);
    }
}

static int quant_coeff_int(int v, int q) {
    if (q < 1) {
        q = 1;
    }

    if (v >= 0) {
        return (v + q / 2) / q;
    } else {
        return -((-v + q / 2) / q);
    }
}

static int dequant_coeff_int(int v, int q) {
    if (q < 1) {
        q = 1;
    }

    return v * q;
}

static int8_t clamp_s8_coeff(int v) {
    if (v < -128) {
        return -128;
    }

    if (v > 127) {
        return 127;
    }

    return (int8_t)v;
}

#define rdo_make_transform_64(...) rdo_make_transform_64_k(g_enc_nkeep, __VA_ARGS__)
static int rdo_make_transform_64_k(
    int g_enc_nkeep,
    const u8 cur[64],
    const u8 pred[64],
    int qp,
    int mv_mode,
    int mx,
    int my,
    u8 token[96],
    u8 recon[64]
) {
    /* Dense transform via the shared codec (keep g_enc_nkeep coeffs/quadrant). */
    int len = 0;
    return mivf_t_make(cur, pred, qp, mv_mode, mx, my, 0,
                       (u8)(mv_mode ? M_MVTRANSFORM : M_TRANSFORM),
                       token, 96, recon, &len, g_enc_nkeep);
}


/* ------------------------------------------------------------------------- */
/* HFIX30A sparse zero-masked residual helpers                               */
/* ------------------------------------------------------------------------- */

static int pack_sparse16_le(
    const int8_t vals[16],
    u8 *out,
    int out_cap,
    u16 *out_mask
) {
    u16 mask = 0;
    int pos = 2;

    if (out_cap < 18) {
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        if (vals[i] != 0) {
            mask |= (u16)(1u << i);
            out[pos++] = (u8)vals[i];
        }
    }

    out[0] = (u8)(mask & 255);
    out[1] = (u8)((mask >> 8) & 255);

    *out_mask = mask;
    return pos;
}

static int rdo_make_qresz_64(
    const u8 cur[64],
    const u8 pred[64],
    int mv_mode,
    int mx,
    int my,
    u8 token[32],
    u8 recon[64],
    int *out_len
) {
    int global_delta = avg_delta_64(cur, pred);
    int8_t residuals[16];

    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            int err_sum = 0;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 2; xx++) {
                    int x = cx * 2 + xx;
                    int y = cy * 2 + yy;
                    int idx = y * 8 + x;

                    int predicted = clamp_u8((int)pred[idx] + global_delta);
                    err_sum += (int)cur[idx] - predicted;
                }
            }

            residuals[cy * 4 + cx] =
                (int8_t)clamp_s8(round_div_int(err_sum, 4));
        }
    }

    int sad = 0;

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            int cell = cell_y + (x >> 1);

            int v = clamp_u8(
                (int)pred[idx] +
                global_delta +
                (int)residuals[cell]
            );

            recon[idx] = (u8)v;

            int e = (int)cur[idx] - v;
            if (e < 0) {
                e = -e;
            }

            sad += e;
        }
    }

    int pos = 0;

    if (mv_mode) {
        token[pos++] = M_MVQRESZ;
        token[pos++] = (u8)(s8)mx;
        token[pos++] = (u8)(s8)my;
        token[pos++] = (u8)(s8)global_delta;
    } else {
        token[pos++] = M_QRESZ;
        token[pos++] = (u8)(s8)global_delta;
    }

    u16 mask = 0;

    int packed_len = pack_sparse16_le(
        residuals,
        token + pos,
        32 - pos,
        &mask
    );

    if (packed_len < 0) {
        *out_len = 0;
        return 999999;
    }

    pos += packed_len;
    *out_len = pos;

    return sad;
}

#define rdo_make_transformz_64(...) rdo_make_transformz_64_k(g_enc_nkeep, __VA_ARGS__)
static int rdo_make_transformz_64_k(
    int g_enc_nkeep,
    const u8 cur[64],
    const u8 pred[64],
    int qp,
    int mv_mode,
    int mx,
    int my,
    u8 token[96],
    u8 recon[64],
    int *out_len
) {
    /* Sparse transform via the shared codec (keep g_enc_nkeep coeffs/quadrant). */
    int sad = mivf_t_make(cur, pred, qp, mv_mode, mx, my, 1,
                          (u8)(mv_mode ? M_MVTRANSFORMZ : M_TRANSFORMZ),
                          token, 96, recon, out_len, g_enc_nkeep);
    if (sad < 0) {
        *out_len = 0;
        return 999999;
    }
    return sad;
}


/* ------------------------------------------------------------------------- */
/* HFIX36A packed 4-bit motion-vector helpers                                */
/* ------------------------------------------------------------------------- */

static inline int mv_can_pack4(int mx, int my) {
    return mx >= -8 && mx <= 7 && my >= -8 && my <= 7;
}

static inline u8 mv_pack4(int mx, int my) {
    return (u8)(((my + 8) << 4) | ((mx + 8) & 15));
}


/* ------------------------------------------------------------------------- */
/* HFIX37-REDUX single-block Global Motion Vector estimator                   */
/* ------------------------------------------------------------------------- */

static void estimate_global_mv_luma(
    const u8 *cur,
    const u8 *prev,
    int w,
    int h,
    int mv_range,
    int *out_mx,
    int *out_my
) {
    int best_mx = 0;
    int best_my = 0;
    long long best_sad = 0x7fffffffffffffffLL;

    if (!cur || !prev || w <= 0 || h <= 0) {
        *out_mx = 0;
        *out_my = 0;
        return;
    }

    if (mv_range < 0) {
        mv_range = 0;
    }

    if (mv_range > 8) {
        mv_range = 8;
    }

    /*
        Search only integer-pel vectors. Sample every other 8x8 block to keep
        encoder cost reasonable while still capturing camera pans.

        MIVF_GLOBAL_MV_SAD_CAP_V1: early-abort once this candidate's partial
        sad already exceeds best_sad. Safe because every term added to `sad`
        (both the per-pixel abs-difference and the 4096 out-of-bounds
        penalty) is nonnegative, so the partial sum is monotonically
        non-decreasing -- once it strictly exceeds best_sad, the eventual
        full sad can only be >= that partial value, so it can never satisfy
        the unchanged final "sad < best_sad" comparison below. No term is
        ever added, subtracted, or otherwise adjusted after this loop (no
        lambda, no MV penalty, nothing), so this is the complete score, not
        a partial one requiring a derived cap. Candidate order (my/mx loops)
        and the strict "<" tie-break (first candidate keeps ties) are both
        untouched, so the chosen (best_mx, best_my) -- and therefore this
        function's only output -- is bit-for-bit identical to before.
    */
    for (int my = -mv_range; my <= mv_range; my++) {
        for (int mx = -mv_range; mx <= mv_range; mx++) {
            long long sad = 0;
            bool over_budget = false;

            for (int by = 1; by < (h / 8) - 1 && !over_budget; by += 2) {
                for (int bx = 1; bx < (w / 8) - 1 && !over_budget; bx += 2) {
                    int x0 = bx * 8;
                    int y0 = by * 8;

                    int sx = x0 + mx;
                    int sy = y0 + my;

                    if (sx < 0 || sy < 0 || sx + 8 > w || sy + 8 > h) {
                        sad += 4096;
                    } else {
                        for (int yy = 0; yy < 8; yy++) {
                            const u8 *c = cur  + (y0 + yy) * w + x0;
                            const u8 *p = prev + (sy + yy) * w + sx;

                            for (int xx = 0; xx < 8; xx++) {
                                int d = (int)c[xx] - (int)p[xx];
                                sad += d < 0 ? -d : d;
                            }
                        }
                    }

                    if (sad > best_sad) {
                        over_budget = true;
                    }
                }
            }

            if (!over_budget && sad < best_sad) {
                best_sad = sad;
                best_mx = mx;
                best_my = my;
            }
        }
    }

    /*
        Clamp to HFIX36A packed-vector-safe range. This also keeps chroma
        scaling deterministic.
    */
    *out_mx = clamp_int(best_mx, -8, 7);
    *out_my = clamp_int(best_my, -8, 7);
}

static void encode_plane(
    const u8 *cur,
    const u8 *prev,
    u8 *recon_plane,
    int w,
    int h,
    bool keyframe,
    int skip_thr,
    int delta_thr,
    int solid_thr,
    int qres_thr,
    const MVList *mvlist,
    int mv_thresh,
    double lambda,
    int qp,
    int g_mx,
    int g_my,
    int mv_range,
    int motion_search_mode,
    PlaneEnc *out
) {
    const int g_enc_nkeep = out->nkeep;
    /*
        HFIX30A competitive RDO loop:
            cost = SAD + lambda * bytes

        Includes fixed residual modes and sparse zero-masked Z-modes.
    */
    (void)skip_thr;
    (void)delta_thr;
    (void)solid_thr;
    (void)qres_thr;
    (void)mv_thresh;

    /*
        HFIX32:
        Plane payload byte 0 stores active transform QP.
        Decoder consumes this byte before macroblock token parsing.
        This applies independently to Y, Cb, and Cr planes.
    */
    if (out->payload.size == 0) {
        int qp_byte = qp;

        if (qp_byte < 1) {
            qp_byte = 1;
        }

        if (qp_byte > 51) {
            qp_byte = 51;
        }

        buf_put_u8(&out->payload, (u8)qp_byte);
        buf_put_u8(&out->payload, (u8)(s8)g_mx);
        buf_put_u8(&out->payload, (u8)(s8)g_my);
    }

    int bxcount = w / 8;
    int bycount = h / 8;

    u8 b[64];
    u8 pb[64];
    u8 token_tmp[96];
    u8 recon_tmp[64];

    u8 best_token[96];
    u8 best_recon[64];

    /*
        HFIX39A:
        Base-relative DQP state. This is not cumulative in the bitstream:
            active_qp = base_qp + active_dqp
    */
    int active_dqp = 0;

    /* --motion-search hybrid: per-plane-per-frame budget for the optional
       local-refine escalation (see the MS_HYBRID branch below). Reset each
       encode_plane() call (i.e. per plane per frame), so no more than a
       fixed fraction of this plane's blocks can ever pay the extra refine
       cost, regardless of content. */
    int hybrid_refine_count = 0;
    int hybrid_refine_budget = (bxcount * bycount) / 4;

    for (int by = 0; by < bycount; by++) {
        for (int bx = 0; bx < bxcount; bx++) {
            get_block(cur, w, bx, by, b);

            /*
                HFIX39A conservative three-state QP ladder:
                    flat / gradient: QP - 4
                    normal:          QP
                    texture:         QP + 4
            */
            int block_var_dqp = calculate_block_variance_64(b);

            int block_min_dqp = 255;
            int block_max_dqp = 0;

            for (int dqp_i = 0; dqp_i < 64; dqp_i++) {
                int v = (int)b[dqp_i];

                if (v < block_min_dqp) {
                    block_min_dqp = v;
                }

                if (v > block_max_dqp) {
                    block_max_dqp = v;
                }
            }

            int block_range_dqp = block_max_dqp - block_min_dqp;

            /*
                HFIX39B:
                Flat-only local QP.

                HFIX39A used +4 on high-variance texture blocks, but that
                over-starved important edge/detail regions. HFIX39B only
                grants extra precision to smooth/gradient blocks and never
                makes any block coarser than the base QP.
            */
            int target_dqp = 0;

            if (block_var_dqp < 80 && block_range_dqp < 48) {
                target_dqp = -4;
            }

            int local_qp = clamp_int(qp + target_dqp, 18, 48);
            int dqp_rate = (target_dqp != active_dqp) ? 2 : 0;

            int best_kind = -1;
            int best_len = 0;
            int best_dqp = active_dqp;
            int candidate_dqp = active_dqp;
            double best_cost = 1.0e30;

            #define RDO_CONSIDER(kind_value, rate_bytes, token_len, token_ptr, recon_ptr, sad_value) do { \
                if ((token_len) > 0) { \
                    int visual_dist = rdo_nssd_64(b, (recon_ptr)); \
                    double candidate_cost = (double)visual_dist + lambda * (double)(rate_bytes); \
                    if (candidate_cost < best_cost) { \
                        best_cost = candidate_cost; \
                        best_kind = (kind_value); \
                        best_len = (token_len); \
                        memcpy(best_token, (token_ptr), (size_t)(token_len)); \
                        memcpy(best_recon, (recon_ptr), 64); \
                        best_dqp = candidate_dqp; \
                    } \
                } \
            } while (0)

            if (!keyframe && prev) {
                get_block(prev, w, bx, by, pb);

                /*
                    SKIP.
                */
                token_tmp[0] = M_SKIP;
                int sad_skip = rdo_sad_64(b, pb);
                RDO_CONSIDER(M_SKIP, 1, 1, token_tmp, pb, sad_skip);

#if MIVF_E3_SKIP_SHORTCUT_ENABLED
                if (sad_skip == 0) {
                    /* See MIVF_E3_ZERO_DIST_SKIP_SHORTCUT_V1 above: no
                       remaining candidate can ever beat a zero-distortion
                       SKIP, so nothing past this point can change the
                       outcome for this block. */
                    goto mivf_e3_block_rdo_done;
                }
#endif

                /*
                    DELTA.
                */
                int d = avg_delta_64(b, pb);
                token_tmp[0] = M_DELTA;
                token_tmp[1] = (u8)(s8)d;
                recon_delta_64(pb, d, recon_tmp);
                int sad_delta = rdo_sad_64(b, recon_tmp);
                RDO_CONSIDER(M_DELTA, 2, 2, token_tmp, recon_tmp, sad_delta);

                /*
                    QRES fixed spatial residual.
                */
                int sad_qres = rdo_make_qres_64(b, pb, token_tmp, recon_tmp);
                RDO_CONSIDER(M_QRES, 18, 18, token_tmp, recon_tmp, sad_qres);

                /*
                    QRESZ sparse spatial residual.
                    Same reconstruction as QRES, variable payload.
                */
                int qz_len = 0;
                int sad_qz = rdo_make_qresz_64(
                    b,
                    pb,
                    0,
                    0,
                    0,
                    token_tmp,
                    recon_tmp,
                    &qz_len
                );
                RDO_CONSIDER(M_QRESZ, qz_len, qz_len, token_tmp, recon_tmp, sad_qz);

                /*
                    TRANSFORM fixed frequency residual.
                */
                int sad_tr = rdo_make_transform_64(
                    b,
                    pb,
                    local_qp,
                    0,
                    0,
                    0,
                    token_tmp,
                    recon_tmp
                );
                candidate_dqp = target_dqp;
                RDO_CONSIDER(M_TRANSFORM, (2 + g_enc_nkeep * 4) + dqp_rate, (2 + g_enc_nkeep * 4), token_tmp, recon_tmp, sad_tr);
                candidate_dqp = active_dqp;

                /*
                    TRANSFORMZ sparse frequency residual.
                */
                int trz_len = 0;
                int sad_trz = rdo_make_transformz_64(
                    b,
                    pb,
                    local_qp,
                    0,
                    0,
                    0,
                    token_tmp,
                    recon_tmp,
                    &trz_len
                );
                candidate_dqp = target_dqp;
                RDO_CONSIDER(M_TRANSFORMZ, trz_len + dqp_rate, trz_len, token_tmp, recon_tmp, sad_trz);
                candidate_dqp = active_dqp;

                /*
                    Motion-predicted candidates.
                */
                if (mvlist) {
                    u8 pred[64];
                    int best_mx = 0;
                    int best_my = 0;
                    int best_mv_sad = 0;
                    int mv_search_cutoff = 4096;
                    bool found_mv;

                    /* --motion-search: full keeps the exact original exhaustive
                       scan (find_best_mv_64, untouched). diamond/fast use the
                       new iterative search instead -- same candidate bounds,
                       same SAD formula, same packed-token output shape, just
                       far fewer probes per block. See find_best_mv_64_diamond
                       for the speed/quality tradeoff this opts into. */
                    if (motion_search_mode == MS_FULL) {
                        found_mv = find_best_mv_64(
                            b,
                            prev,
                            w,
                            h,
                            bx,
                            by,
                            mvlist,
                            mv_search_cutoff,
                            &best_mx,
                            &best_my,
                            pred,
                            &best_mv_sad
                        );
                    } else if (motion_search_mode == MS_HYBRID) {
                        /* --motion-search hybrid: two-seed bounded diamond
                           (zero + global MV), then -- only for blocks whose
                           result still looks unreliable, and only up to a
                           fixed per-plane budget -- a small capped local
                           refine. See find_best_mv_64_hybrid() /
                           find_best_mv_64_local_refine(). */
                        found_mv = find_best_mv_64_hybrid(
                            b,
                            prev,
                            w,
                            h,
                            bx,
                            by,
                            mv_range,
                            mv_search_cutoff,
                            g_mx,
                            g_my,
                            &best_mx,
                            &best_my,
                            pred,
                            &best_mv_sad
                        );

                        if (found_mv &&
                            best_mv_sad > mv_thresh &&
                            block_var_dqp > 80 &&
                            hybrid_refine_count < hybrid_refine_budget) {
                            int refined_mx;
                            int refined_my;
                            int refined_sad;
                            u8 refined_pred[64];

                            hybrid_refine_count++;

                            if (find_best_mv_64_local_refine(
                                    b,
                                    prev,
                                    w,
                                    h,
                                    bx,
                                    by,
                                    mv_range,
                                    2,
                                    best_mx,
                                    best_my,
                                    best_mv_sad,
                                    &refined_mx,
                                    &refined_my,
                                    refined_pred,
                                    &refined_sad
                                )) {
                                best_mx = refined_mx;
                                best_my = refined_my;
                                best_mv_sad = refined_sad;
                                memcpy(pred, refined_pred, 64);
                            }
                        }
                    } else if (motion_search_mode == MS_HIERARCHICAL) {
                        /* MIVF_HIERARCHICAL_MOTION_V1: deterministic
                           coarse-to-fine search. See find_best_mv_64_hierarchical
                           for the stage breakdown and bounded candidate count. */
                        found_mv = find_best_mv_64_hierarchical(
                            b,
                            prev,
                            w,
                            h,
                            bx,
                            by,
                            mv_range,
                            mv_search_cutoff,
                            g_mx,
                            g_my,
                            &best_mx,
                            &best_my,
                            pred,
                            &best_mv_sad
                        );
                    } else {
                        int start_step = (motion_search_mode == MS_FAST) ? 2 : 4;
                        bool recenter = (motion_search_mode != MS_FAST);

                        found_mv = find_best_mv_64_diamond(
                            b,
                            prev,
                            w,
                            h,
                            bx,
                            by,
                            mv_range,
                            mv_search_cutoff,
                            start_step,
                            recenter,
                            &best_mx,
                            &best_my,
                            pred,
                            &best_mv_sad
                        );
                    }

                    if (found_mv) {
                        /*
                            HFIX37-REDUX:
                            Single-block Global Motion Vector copy.

                            This is a normal one-block token. It does not
                            accumulate runs and never changes decoder traversal.
                        */
                        if (prev) {
                            int g_sx = bx * 8 + g_mx;
                            int g_sy = by * 8 + g_my;

                            if (g_sx >= 0 && g_sy >= 0 &&
                                g_sx + 8 <= w && g_sy + 8 <= h) {
                                u8 gmv_pred[64];

                                for (int yy = 0; yy < 8; yy++) {
                                    memcpy(
                                        gmv_pred + yy * 8,
                                        prev + (g_sy + yy) * w + g_sx,
                                        8
                                    );
                                }

                                int sad_gmv = rdo_sad_64(b, gmv_pred);

                                token_tmp[0] = M_GMVCOPY;

                                RDO_CONSIDER(
                                    M_GMVCOPY_INTERNAL,
                                    1,
                                    1,
                                    token_tmp,
                                    gmv_pred,
                                    sad_gmv
                                );
                            }
                        }

                        /*
                            MVCOPY.
                        */
                        token_tmp[0] = M_MVCOPY;
                        token_tmp[1] = (u8)(s8)best_mx;
                        token_tmp[2] = (u8)(s8)best_my;
                        if (mv_can_pack4(best_mx, best_my)) {
                            u8 packed_token[2];
                            packed_token[0] = M_MVCOPYP;
                            packed_token[1] = mv_pack4(best_mx, best_my);
                            RDO_CONSIDER(M_MVCOPYP, 2, 2, packed_token, pred, best_mv_sad);
                        }
                        RDO_CONSIDER(M_MVCOPY, 3, 3, token_tmp, pred, best_mv_sad);

                        /*
                            MVQRES fixed spatial residual.
                        */
                        int sad_mvqres = rdo_make_mvqres_64(
                            b,
                            pred,
                            best_mx,
                            best_my,
                            token_tmp,
                            recon_tmp
                        );
                        RDO_CONSIDER(M_MVQRES, 20, 20, token_tmp, recon_tmp, sad_mvqres);

                        /*
                            MVQRESZ sparse spatial residual.
                        */
                        int mvqz_len = 0;
                        int sad_mvqz = rdo_make_qresz_64(
                            b,
                            pred,
                            1,
                            best_mx,
                            best_my,
                            token_tmp,
                            recon_tmp,
                            &mvqz_len
                        );
                        RDO_CONSIDER(M_MVQRESZ, mvqz_len, mvqz_len, token_tmp, recon_tmp, sad_mvqz);

                        /*
                            MVTRANSFORM fixed frequency residual.
                        */
                        int sad_mvtr = rdo_make_transform_64(
                            b,
                            pred,
                            local_qp,
                            1,
                            best_mx,
                            best_my,
                            token_tmp,
                            recon_tmp
                        );
                        candidate_dqp = target_dqp;
                        RDO_CONSIDER(M_MVTRANSFORM, (4 + g_enc_nkeep * 4) + dqp_rate, (4 + g_enc_nkeep * 4), token_tmp, recon_tmp, sad_mvtr);
                        candidate_dqp = active_dqp;

                        /*
                            MVTRANSFORMZ sparse frequency residual.
                        */
                        int mvtrz_len = 0;
                        int sad_mvtrz = rdo_make_transformz_64(
                            b,
                            pred,
                            local_qp,
                            1,
                            best_mx,
                            best_my,
                            token_tmp,
                            recon_tmp,
                            &mvtrz_len
                        );
                        candidate_dqp = target_dqp;
                        RDO_CONSIDER(M_MVTRANSFORMZ, mvtrz_len + dqp_rate, mvtrz_len, token_tmp, recon_tmp, sad_mvtrz);
                        candidate_dqp = active_dqp;
                        if (mv_can_pack4(best_mx, best_my) && mvtrz_len >= 6) {
                            u8 packed_token[96];
                            packed_token[0] = M_MVTRANSFORMZP;
                            packed_token[1] = mv_pack4(best_mx, best_my);
                            packed_token[2] = token_tmp[3]; /* reserved */
                            memcpy(packed_token + 3, token_tmp + 4, (size_t)(mvtrz_len - 4));
                            int packed_len = mvtrz_len - 1;
                            candidate_dqp = target_dqp;
                            RDO_CONSIDER(M_MVTRANSFORMZP, packed_len + dqp_rate, packed_len, packed_token, recon_tmp, sad_mvtrz);
                            candidate_dqp = active_dqp;
                        }
                    }
                }
            }

            /*
                SOLID.
            */
            int av = avg_u8_64(b);
            token_tmp[0] = M_SOLID;
            token_tmp[1] = (u8)av;
            recon_solid_64(av, recon_tmp);
            int sad_solid = rdo_sad_64(b, recon_tmp);
            RDO_CONSIDER(M_SOLID, 2, 2, token_tmp, recon_tmp, sad_solid);

            /*
                RAW (HFIX62A: Moflex-Tier Exclusion).
                Raw blocks are explicitly BANNED on P-Frames to prevent
                Rate-Distortion algorithms from hoarding bytes.
            */
            if (keyframe) {
                token_tmp[0] = M_RAW;
                memcpy(token_tmp + 1, b, 64);
                RDO_CONSIDER(M_RAW, 65, 65, token_tmp, b, 0);
            }

            #undef RDO_CONSIDER

#if MIVF_E3_SKIP_SHORTCUT_ENABLED
            mivf_e3_block_rdo_done:;
#endif

            if (best_kind < 0) {
                if (keyframe) {
                    best_token[0] = M_RAW;
                    memcpy(best_token + 1, b, 64);
                    memcpy(best_recon, b, 64);
                    best_kind = M_RAW;
                    best_len = 65;
                } else {
                    /* Hard fallback to SKIP if completely stuck on a P-Frame */
                    best_token[0] = M_SKIP;
                    memcpy(best_recon, pb, 64);
                    best_kind = M_SKIP;
                    best_len = 1;
                }
            }

            if (best_kind == M_SKIP) {
                plane_emit_skip(out);
                out->stats.skip++;
            } else if (best_kind == M_GMVCOPY_INTERNAL) {
                u8 tok[1];

                tok[0] = M_GMVCOPY;

                plane_emit_token(out, tok, 1);

                out->stats.mv++;
            } else {
                /*
                    HFIX39A emit base-relative DQP state token only when a
                    transform-family mode wins and the target offset differs
                    from the currently active offset.
                */
                if ((best_kind == M_TRANSFORM ||
                     best_kind == M_MVTRANSFORM ||
                     best_kind == M_TRANSFORMZ ||
                     best_kind == M_MVTRANSFORMZ ||
                     best_kind == M_MVTRANSFORMZP) &&
                    best_dqp != active_dqp) {
                    u8 dqp_tok[2];

                    dqp_tok[0] = M_SET_BASE_DQP;
                    dqp_tok[1] = (u8)(s8)best_dqp;

                    plane_emit_token(out, dqp_tok, 2);

                    active_dqp = best_dqp;
                }

                plane_emit_token(out, best_token, best_len);

                if (best_kind == M_DELTA) {
                    out->stats.delta++;
                } else if (best_kind == M_SOLID) {
                    out->stats.solid++;
                } else if ((best_kind == M_MVCOPY || best_kind == M_MVCOPYP)) {
                    out->stats.mv++;
                } else if (best_kind == M_MVQRES) {
                    out->stats.mvqres++;
                } else if (best_kind == M_QRES) {
                    out->stats.qres++;
                } else if (best_kind == M_TRANSFORM) {
                    out->stats.tr++;
                } else if (best_kind == M_MVTRANSFORM) {
                    out->stats.mvtr++;
                } else if (best_kind == M_QRESZ) {
                    out->stats.qz++;
                } else if (best_kind == M_MVQRESZ) {
                    out->stats.mvqz++;
                } else if (best_kind == M_TRANSFORMZ) {
                    out->stats.trz++;
                } else if ((best_kind == M_MVTRANSFORMZ || best_kind == M_MVTRANSFORMZP)) {
                    out->stats.mvtrz++;
                } else if (best_kind == M_RAW) {
                    out->stats.raw++;
                }
            }

            put_block(recon_plane, w, bx, by, best_recon);
        }
    }

    plane_flush_skips(out);
}

/* ------------------------------------------------------------------------- */
/* Main encoder                                                              */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* libavcodec wrapper                                                        */
/* ------------------------------------------------------------------------- */

typedef struct M2Y1EncContext {
    const AVClass *class;
    int qp;
    int c_qp_offset;
    double lambda;
    double c_lambda;
    int keep;
    int mv_range;
    int motion_search;
    int m2y2;

    MVList mvlist;
    uint8_t *planes;      ///< contiguous Y, Cb, Cr of the frame being coded
    uint8_t *prev;        ///< reconstruction of the previous frame
    uint8_t *recon;       ///< reconstruction of this frame
    uint8_t *rc_buf;
    size_t rc_buf_size;
    MivfRcO1 *rc;
    int64_t frame_num;
} M2Y1EncContext;

static av_cold int m2y1_encode_init(AVCodecContext *avctx)
{
    M2Y1EncContext *s = avctx->priv_data;
    const int y_size = avctx->width * avctx->height;

    if ((avctx->width & 15) || (avctx->height & 15)) {
        av_log(avctx, AV_LOG_ERROR,
               "width and height must be multiples of 16, not %dx%d\n",
               avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }
    if (avctx->width > 0xFFFF || avctx->height > 0xFFFF)
        return AVERROR(EINVAL);

    s->mvlist = build_mv_list(s->mv_range);
    s->planes = av_malloc(y_size * 3 / 2);
    s->prev   = av_mallocz(y_size * 3 / 2);
    s->recon  = av_malloc(y_size * 3 / 2);
    if (!s->mvlist.mx || !s->planes || !s->prev || !s->recon)
        return AVERROR(ENOMEM);
    if (s->m2y2) {
        s->rc = av_mallocz(sizeof(*s->rc));
        if (!s->rc)
            return AVERROR(ENOMEM);
    }
    if (s->c_lambda < 0)
        s->c_lambda = s->lambda * 1.55;
    return 0;
}

static void copy_plane(uint8_t *dst, const uint8_t *src, ptrdiff_t stride,
                       int w, int h)
{
    for (int y = 0; y < h; y++)
        memcpy(dst + y * w, src + y * stride, w);
}

static int m2y1_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    M2Y1EncContext *s = avctx->priv_data;
    const int w = avctx->width, h = avctx->height;
    const int c_w = w / 2, c_h = h / 2;
    const int y_size = w * h, c_size = c_w * c_h;
    const int c_qp = clamp_int(s->qp + s->c_qp_offset, 1, 51);
    const int keyframe = s->frame_num == 0 ||
                         (avctx->gop_size > 0 && s->frame_num % avctx->gop_size == 0) ||
                         frame->pict_type == AV_PICTURE_TYPE_I;
    uint8_t *cur[3]  = { s->planes, s->planes + y_size, s->planes + y_size + c_size };
    uint8_t *prev[3] = { s->prev,   s->prev + y_size,   s->prev + y_size + c_size };
    uint8_t *rec[3]  = { s->recon,  s->recon + y_size,  s->recon + y_size + c_size };
    PlaneEnc enc[3];
    int g_mx = 0, g_my = 0, ret = 0;
    size_t planes_size;
    uint8_t *body;

    copy_plane(cur[0], frame->data[0], frame->linesize[0], w, h);
    copy_plane(cur[1], frame->data[1], frame->linesize[1], c_w, c_h);
    copy_plane(cur[2], frame->data[2], frame->linesize[2], c_w, c_h);

    plane_enc_init(&enc[0], (size_t)(y_size + 4096));
    plane_enc_init(&enc[1], (size_t)(c_size + 2048));
    plane_enc_init(&enc[2], (size_t)(c_size + 2048));
    for (int i = 0; i < 3; i++)
        enc[i].nkeep = mivf_t_resolve(s->keep);

    if (!keyframe)
        estimate_global_mv_luma(cur[0], prev[0], w, h, 8, &g_mx, &g_my);

    encode_plane(cur[0], keyframe ? NULL : prev[0], rec[0], w, h, keyframe,
                 0, 0, 0, 0, &s->mvlist, 0, s->lambda, s->qp,
                 g_mx, g_my, s->mv_range, s->motion_search, &enc[0]);
    for (int i = 1; i < 3; i++)
        encode_plane(cur[i], keyframe ? NULL : prev[i], rec[i], c_w, c_h,
                     keyframe, 0, 0, 0, 0, &s->mvlist, 0, s->c_lambda, c_qp,
                     g_mx / 2, g_my / 2, s->mv_range, s->motion_search,
                     &enc[i]);

    if (enc[0].payload.oom || enc[1].payload.oom || enc[2].payload.oom) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    planes_size = enc[0].payload.size + enc[1].payload.size + enc[2].payload.size;

    if (s->m2y2) {
        /* M2Y2: the same token bytes, range-coded as one block per packet. */
        size_t need = planes_size * 2 + 4096, clen;
        uint8_t *raw;

        if (s->rc_buf_size < need + planes_size) {
            av_freep(&s->rc_buf);
            s->rc_buf_size = 0;
            if (!(s->rc_buf = av_malloc(need + planes_size))) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            s->rc_buf_size = need + planes_size;
        }
        raw = s->rc_buf + need;
        memcpy(raw, enc[0].payload.data, enc[0].payload.size);
        memcpy(raw + enc[0].payload.size, enc[1].payload.data, enc[1].payload.size);
        memcpy(raw + enc[0].payload.size + enc[1].payload.size,
               enc[2].payload.data, enc[2].payload.size);
        clen = mivf_rc_o1_compress(s->rc, raw, planes_size, s->rc_buf, need);
        if (clen > need) {
            ret = AVERROR_BUG;
            goto end;
        }
        if ((ret = ff_get_encode_buffer(avctx, pkt, 32 + clen, 0)) < 0)
            goto end;
        body = pkt->data;
        memset(body, 0, 32);
        memcpy(body, "M2Y2", 4);
        AV_WL32(body + 28, clen);
        memcpy(body + 32, s->rc_buf, clen);
    } else {
        if ((ret = ff_get_encode_buffer(avctx, pkt, 28 + planes_size, 0)) < 0)
            goto end;
        body = pkt->data;
        memset(body, 0, 28);
        memcpy(body, "M2Y1", 4);
        memcpy(body + 28, enc[0].payload.data, enc[0].payload.size);
        memcpy(body + 28 + enc[0].payload.size, enc[1].payload.data, enc[1].payload.size);
        memcpy(body + 28 + enc[0].payload.size + enc[1].payload.size,
               enc[2].payload.data, enc[2].payload.size);
    }
    /* The muxer rewrites the frame number with the frame's place in the file. */
    AV_WL16(body + 4, w);
    AV_WL16(body + 6, h);
    AV_WL32(body + 8, (uint32_t)s->frame_num);
    body[12] = keyframe ? 1 : 2;
    body[13] = (uint8_t)mivf_t_resolve(s->keep);
    AV_WL32(body + 16, enc[0].payload.size);
    AV_WL32(body + 20, enc[1].payload.size);
    AV_WL32(body + 24, enc[2].payload.size);

    FFSWAP(uint8_t *, s->prev, s->recon);
    if (keyframe)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->frame_num++;
    *got_packet = 1;

end:
    for (int i = 0; i < 3; i++)
        plane_enc_free(&enc[i]);
    return ret;
}

static av_cold int m2y1_encode_close(AVCodecContext *avctx)
{
    M2Y1EncContext *s = avctx->priv_data;

    mvlist_free(&s->mvlist);
    av_freep(&s->planes);
    av_freep(&s->prev);
    av_freep(&s->recon);
    av_freep(&s->rc_buf);
    av_freep(&s->rc);
    return 0;
}

#define OFFSET(x) offsetof(M2Y1EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "qp", "transform quantizer", OFFSET(qp), AV_OPT_TYPE_INT, { .i64 = 34 }, 1, 51, VE },
    { "c_qp_offset", "chroma quantizer offset", OFFSET(c_qp_offset), AV_OPT_TYPE_INT, { .i64 = 5 }, -50, 50, VE },
    { "lambda", "rate-distortion lambda; higher is smaller and lossier", OFFSET(lambda), AV_OPT_TYPE_DOUBLE, { .dbl = 20.0 }, 0, 10000, VE },
    { "c_lambda", "chroma lambda (-1: 1.55 x lambda)", OFFSET(c_lambda), AV_OPT_TYPE_DOUBLE, { .dbl = -1 }, -1, 10000, VE },
    { "keep", "transform coefficients kept per 4x4 quadrant", OFFSET(keep), AV_OPT_TYPE_INT, { .i64 = 16 }, 4, 16, VE, .unit = "keep" },
    { "mv_range", "motion search range in pixels", OFFSET(mv_range), AV_OPT_TYPE_INT, { .i64 = 4 }, 0, 127, VE },
    { "motion_search", "motion search algorithm", OFFSET(motion_search), AV_OPT_TYPE_INT, { .i64 = MS_FULL }, MS_FULL, MS_HIERARCHICAL, VE, .unit = "ms" },
        { "full",         "exhaustive",              0, AV_OPT_TYPE_CONST, { .i64 = MS_FULL },         0, 0, VE, .unit = "ms" },
        { "diamond",      "iterative diamond",       0, AV_OPT_TYPE_CONST, { .i64 = MS_DIAMOND },      0, 0, VE, .unit = "ms" },
        { "fast",         "faster diamond",          0, AV_OPT_TYPE_CONST, { .i64 = MS_FAST },         0, 0, VE, .unit = "ms" },
        { "hybrid",       "two-seed diamond",        0, AV_OPT_TYPE_CONST, { .i64 = MS_HYBRID },       0, 0, VE, .unit = "ms" },
        { "hierarchical", "coarse-to-fine grid",     0, AV_OPT_TYPE_CONST, { .i64 = MS_HIERARCHICAL }, 0, 0, VE, .unit = "ms" },
    { "m2y2", "range-code the frames (M2Y2): the same pictures in a smaller file", OFFSET(m2y2), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { NULL },
};

static const AVClass m2y1_encoder_class = {
    .class_name = "m2y1 encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault m2y1_defaults[] = {
    { "g", "240" },
    { NULL },
};

const FFCodec ff_m2y1_encoder = {
    .p.name         = "m2y1",
    CODEC_LONG_NAME("MIVF M2Y1 video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_M2Y1,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(M2Y1EncContext),
    .init           = m2y1_encode_init,
    FF_CODEC_ENCODE_CB(m2y1_encode_frame),
    .close          = m2y1_encode_close,
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P),
    .p.priv_class   = &m2y1_encoder_class,
    .defaults       = m2y1_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
