/*
 * MIVF M2Y1 / M2Y2 video decoder
 *
 * The block decoder is ported from the MIVF player (source/main.c), with
 * its transform keep-count passed in instead of held in a global. It
 * carries this notice:
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "mivf_rc.h"
#include "mivf_transform.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;

#define M2Y1_MVCOPYP       14
#define M2Y1_MVQRESP       15
#define M2Y1_MVTRANSFORMP  16
#define M2Y1_MVQRESZP      17
#define M2Y1_MVTRANSFORMZP 18
#define M2Y1_GMVCOPY       19
#define M2Y1_SET_BASE_DQP 20

static inline void m2y1_unpack_mv4(u8 b, int *mx, int *my) {
    *mx = (int)(b & 15) - 8;
    *my = (int)(b >> 4) - 8;
}

enum {
    M2Y1_SKIP     = 0,
    M2Y1_RAW      = 1,
    M2Y1_DELTA    = 2,
    M2Y1_SOLID    = 3,
    M2Y1_RUN_SKIP = 4,
    M2Y1_QRES     = 5,
    M2Y1_MVCOPY   = 6,
    M2Y1_MVQRES   = 7,
    M2Y1_TRANSFORM   = 8,
    M2Y1_MVTRANSFORM = 9,
    M2Y1_QRESZ         = 10,
    M2Y1_MVQRESZ       = 11,
    M2Y1_TRANSFORMZ    = 12,
    M2Y1_MVTRANSFORMZ  = 13
};

static void m2y1_copy_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int bx,
    int by
) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        memcpy(
            dst  + (y0 + y) * plane_w + x0,
            prev + (y0 + y) * plane_w + x0,
            8
        );
    }
}

static void m2y1_raw_block(
    u8 *dst,
    const u8 *src,
    int plane_w,
    int bx,
    int by
) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        memcpy(dst + (y0 + y) * plane_w + x0, src + y * 8, 8);
    }
}

static void m2y1_solid_block(
    u8 *dst,
    int plane_w,
    int bx,
    int by,
    u8 value
) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        memset(dst + (y0 + y) * plane_w + x0, value, 8);
    }
}

static void m2y1_delta_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int bx,
    int by,
    int delta
) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        u8 *d = dst + (y0 + y) * plane_w + x0;
        const u8 *p = prev + (y0 + y) * plane_w + x0;

        for (int x = 0; x < 8; x++) {
            int v = (int)p[x] + delta;

            if (v < 0) {
                v = 0;
            } else if (v > 255) {
                v = 255;
            }

            d[x] = (u8)v;
        }
    }
}


static inline u8 m2y1_clamp_u8_int(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static void m2y1_qres_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int bx,
    int by,
    int global_delta,
    const int8_t residuals[16]
) {
    int x0 = bx * 8;
    int y0 = by * 8;

    for (int y = 0; y < 8; y++) {
        u8 *d = dst + (y0 + y) * plane_w + x0;
        const u8 *p = prev + (y0 + y) * plane_w + x0;

        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int cell = cell_y + (x >> 1);
            int v = (int)p[x] + global_delta + (int)residuals[cell];
            d[x] = m2y1_clamp_u8_int(v);
        }
    }
}


static void m2y1_mvcopy_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int plane_h,
    int bx,
    int by,
    int mx,
    int my
) {
    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int src_x0 = dst_x0 + mx;
    int src_y0 = dst_y0 + my;

    /*
        Bounds safety:
        Encoder should never emit invalid vectors, but the decoder must
        never read outside the previous plane.
    */
    if (src_x0 < 0 || src_y0 < 0 ||
        src_x0 + 8 > plane_w ||
        src_y0 + 8 > plane_h) {
        /*
            Fallback to same-position copy rather than crashing.
            This preserves byte sync and prevents off-plane reads.
        */
        src_x0 = dst_x0;
        src_y0 = dst_y0;
    }

    for (int y = 0; y < 8; y++) {
        memcpy(
            dst  + (dst_y0 + y) * plane_w + dst_x0,
            prev + (src_y0 + y) * plane_w + src_x0,
            8
        );
    }
}


static void m2y1_mvqres_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int plane_h,
    int bx,
    int by,
    int mx,
    int my,
    int global_delta,
    const int8_t residuals[16]
) {
    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int src_x0 = dst_x0 + mx;
    int src_y0 = dst_y0 + my;

    if (src_x0 < 0 || src_y0 < 0 ||
        src_x0 + 8 > plane_w ||
        src_y0 + 8 > plane_h) {
        src_x0 = dst_x0;
        src_y0 = dst_y0;
    }

    for (int y = 0; y < 8; y++) {
        u8 *d = dst + (dst_y0 + y) * plane_w + dst_x0;
        const u8 *p = prev + (src_y0 + y) * plane_w + src_x0;

        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int cell = cell_y + (x >> 1);
            int v = (int)p[x] + global_delta + (int)residuals[cell];

            d[x] = m2y1_clamp_u8_int(v);
        }
    }
}


/* ------------------------------------------------------------------------- */
/* HFIX29B M2Y1 4x4 inverse transform decoder                                */
/* ------------------------------------------------------------------------- */

/* HFIX32: transform QP is read from each plane payload, no fixed define. */

static void m2y1_transform4_inverse(
    const int16_t input[16],
    int16_t output[16]
) {
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

#define m2y1_transform_block(...) m2y1_transform_block_k(g_m2y1_nkeep, __VA_ARGS__)
static void m2y1_transform_block_k(
    int g_m2y1_nkeep,
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int plane_h,
    int bx,
    int by,
    int mx,
    int my,
    int qp,
    const int8_t *coeffs
) {
    /* Reconstruct one 8x8 block via the shared transform codec (keep g_m2y1_nkeep). */
    mivf_t_decode(dst, prev, plane_w, plane_h, bx, by, mx, my, qp, coeffs, g_m2y1_nkeep);
}


/* ------------------------------------------------------------------------- */
/* HFIX30B M2Y1 sparse zero-masked decoder helpers                           */
/* ------------------------------------------------------------------------- */

static int m2y1_read_sparse16_le(
    const u8 *src,
    size_t n,
    size_t *off,
    int8_t vals[16]
) {
    if (*off + 2 > n) {
        return -1;
    }

    u16 mask =
        (u16)src[*off] |
        ((u16)src[*off + 1] << 8);

    *off += 2;

    memset(vals, 0, 16);

    for (int i = 0; i < 16; i++) {
        if (mask & (u16)(1u << i)) {
            if (*off >= n) {
                return -2;
            }

            vals[i] = (int8_t)src[*off];
            *off += 1;
        }
    }

    return 0;
}

static void m2y1_qresz_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int plane_h,
    int bx,
    int by,
    int mx,
    int my,
    int global_delta,
    const int8_t residuals[16]
) {
    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int src_x0 = dst_x0 + mx;
    int src_y0 = dst_y0 + my;

    if (src_x0 < 0 || src_y0 < 0 ||
        src_x0 + 8 > plane_w ||
        src_y0 + 8 > plane_h) {
        src_x0 = dst_x0;
        src_y0 = dst_y0;
    }

    for (int y = 0; y < 8; y++) {
        int cell_y = (y >> 1) * 4;

        for (int x = 0; x < 8; x++) {
            int cell = cell_y + (x >> 1);

            int src_idx = (src_y0 + y) * plane_w + src_x0 + x;
            int dst_idx = (dst_y0 + y) * plane_w + dst_x0 + x;

            int v =
                (int)prev[src_idx] +
                global_delta +
                (int)residuals[cell];

            dst[dst_idx] = m2y1_clamp_u8_int(v);
        }
    }
}


/* ------------------------------------------------------------------------- */
/* HFIX37-REDUX single-block bounds-checked GMV copy                          */
/* ------------------------------------------------------------------------- */

static void m2y1_mvcopyp_block(
    u8 *dst,
    const u8 *prev,
    int plane_w,
    int plane_h,
    int bx,
    int by,
    int mx,
    int my
) {
    int dst_x0 = bx * 8;
    int dst_y0 = by * 8;

    int src_x0 = dst_x0 + mx;
    int src_y0 = dst_y0 + my;

    if (src_x0 < 0 || src_y0 < 0 ||
        src_x0 + 8 > plane_w ||
        src_y0 + 8 > plane_h) {
        src_x0 = dst_x0;
        src_y0 = dst_y0;
    }

    for (int y = 0; y < 8; y++) {
        memcpy(
            dst + (dst_y0 + y) * plane_w + dst_x0,
            prev + (src_y0 + y) * plane_w + src_x0,
            8
        );
    }
}

static int dec_m2y1_plane(
    const u8 *src,
    size_t n,
    u8 *dst,
    const u8 *prev,
    bool have_prev,
    int plane_w,
    int plane_h,
    int g_m2y1_nkeep
) {
    if ((plane_w & 7) || (plane_h & 7)) {
        return -1;
    }

    int bxcount = plane_w / 8;
    int bycount = plane_h / 8;
    u32 block_count = (u32)(bxcount * bycount);

    size_t off = 0;

    /*
        HFIX32:
        Each plane payload begins with one active QP byte.
        This must be consumed before macroblock token parsing.
    */
    if (off >= n) {
        return -50;
    }

    int current_frame_qp = (int)src[off++];

    if (current_frame_qp < 1 || current_frame_qp > 51) {
        return -51;
    }

    /*
        HFIX37-REDUX:
        Plane-local global motion vector follows QP.
        Decoder applies these values directly and never rescales chroma.
    */
    if (off + 2 > n) {
        return -80;
    }

    int g_mx = (int)(int8_t)src[off++];
    int g_my = (int)(int8_t)src[off++];

    /*
        HFIX39A:
        Base-relative transform QP state. DQP tokens set active_qp from
        base_qp + signed delta. They are not cumulative.
    */
    int base_qp = current_frame_qp;
    int active_qp = base_qp;

    u32 bi = 0;
    int bx = 0;
    int by = 0;

    while (bi < block_count) {
        if (off >= n) {
            return -2;
        }

        u8 m = src[off++];

        if (m == M2Y1_SET_BASE_DQP) {
            if (off >= n) {
                return -90;
            }

            int dqp = (int)(int8_t)src[off++];
            int q = base_qp + dqp;

            if (q < 18) {
                q = 18;
            } else if (q > 48) {
                q = 48;
            }

            active_qp = q;

            continue;
        }

        if (m == M2Y1_RUN_SKIP) {
            if (!have_prev) {
                return -3;
            }

            if (off >= n) {
                return -4;
            }

            u32 run = (u32)src[off++] + 1;

            if (bi + run > block_count) {
                return -5;
            }

            for (u32 i = 0; i < run; i++) {
                m2y1_copy_block(dst, prev, plane_w, bx, by);

                bx++;

                if (bx >= bxcount) {
                    bx = 0;
                    by++;
                }

                bi++;
            }

            continue;
        }

        if (m == M2Y1_GMVCOPY) {
            if (!have_prev) {
                return -81;
            }

            m2y1_mvcopyp_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                g_mx,
                g_my
            );

        } else if (m == M2Y1_SKIP) {
            if (!have_prev) {
                return -6;
            }

            m2y1_copy_block(dst, prev, plane_w, bx, by);

        } else if (m == M2Y1_RAW) {
            if (off + 64 > n) {
                return -7;
            }

            m2y1_raw_block(dst, src + off, plane_w, bx, by);
            off += 64;

        } else if (m == M2Y1_DELTA) {
            if (!have_prev) {
                return -8;
            }

            if (off >= n) {
                return -9;
            }

            int delta = (int)(int8_t)src[off++];
            m2y1_delta_block(dst, prev, plane_w, bx, by, delta);

                } else if (m == M2Y1_MVCOPYP) {
            if (!have_prev) return -60;
            if (off + 1 > n) return -61;
            int mx = 0, my = 0;
            m2y1_unpack_mv4(src[off++], &mx, &my);
            m2y1_mvcopy_block(dst, prev, plane_w, plane_h, bx, by, mx, my);
        } else if (m == M2Y1_MVCOPY) {
            if (!have_prev) {
                return -14;
            }

            /*
                MVCOPY payload:
                    signed mx byte
                    signed my byte
            */
            if (off + 2 > n) {
                return -15;
            }

            int mx = (int)(int8_t)src[off++];
            int my = (int)(int8_t)src[off++];

            m2y1_mvcopy_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                mx,
                my
            );

        } else if (m == M2Y1_QRESZ) {
            if (!have_prev) {
                return -30;
            }

            /*
                QRESZ payload after mode:
                    signed global delta byte
                    little-endian 16-bit sparse residual mask
                    N signed residual bytes
            */
            if (off + 1 > n) {
                return -31;
            }

            int global_delta = (int)(int8_t)src[off++];

            int8_t residuals[16];

            size_t off2 = off;
            int rr = m2y1_read_sparse16_le(src, n, &off2, residuals);

            if (rr < 0) {
                return -32;
            }

            off = off2;

            m2y1_qresz_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                0,
                0,
                global_delta,
                residuals
            );

        } else if (m == M2Y1_MVQRESZ) {
            if (!have_prev) {
                return -33;
            }

            /*
                MVQRESZ payload after mode:
                    signed mx byte
                    signed my byte
                    signed global delta byte
                    little-endian 16-bit sparse residual mask
                    N signed residual bytes
            */
            if (off + 3 > n) {
                return -34;
            }

            int mx = (int)(int8_t)src[off++];
            int my = (int)(int8_t)src[off++];
            int global_delta = (int)(int8_t)src[off++];

            int8_t residuals[16];

            size_t off2 = off;
            int rr = m2y1_read_sparse16_le(src, n, &off2, residuals);

            if (rr < 0) {
                return -35;
            }

            off = off2;

            m2y1_qresz_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                mx,
                my,
                global_delta,
                residuals
            );

        } else if (m == M2Y1_TRANSFORMZ) {
            if (!have_prev) {
                return -36;
            }

            /*
                TRANSFORMZ payload after mode:
                    reserved byte
                    little-endian 16-bit sparse coefficient mask
                    N signed coefficient bytes
            */
            if (off + 1 > n) {
                return -37;
            }

            off++; /* reserved */

            int8_t coeffs[MIVF_T_MAX_NSLOT];

            size_t off2 = off;
            int rr = mivf_t_read_sparse(src, n, &off2, coeffs, g_m2y1_nkeep * 4);

            if (rr < 0) {
                return -38;
            }

            off = off2;

            m2y1_transform_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                0,
                0,
                active_qp,
                coeffs
            );

                } else if (m == M2Y1_MVTRANSFORMZP) {
            if (!have_prev) return -62;
            if (off + 2 > n) return -63;
            int mx = 0, my = 0;
            m2y1_unpack_mv4(src[off++], &mx, &my);
            off++; /* skip reserved byte */
            int8_t coeffs[MIVF_T_MAX_NSLOT];
            size_t off2 = off;
            int rr = mivf_t_read_sparse(src, n, &off2, coeffs, g_m2y1_nkeep * 4);
            if (rr < 0) return -64;
            off = off2;
            m2y1_transform_block(dst, prev, plane_w, plane_h, bx, by, mx, my, active_qp, coeffs);
        } else if (m == M2Y1_MVTRANSFORMZ) {
            if (!have_prev) {
                return -39;
            }

            /*
                MVTRANSFORMZ payload after mode:
                    signed mx byte
                    signed my byte
                    reserved byte
                    little-endian 16-bit sparse coefficient mask
                    N signed coefficient bytes
            */
            if (off + 3 > n) {
                return -40;
            }

            int mx = (int)(int8_t)src[off++];
            int my = (int)(int8_t)src[off++];

            off++; /* reserved */

            int8_t coeffs[MIVF_T_MAX_NSLOT];

            size_t off2 = off;
            int rr = mivf_t_read_sparse(src, n, &off2, coeffs, g_m2y1_nkeep * 4);

            if (rr < 0) {
                return -41;
            }

            off = off2;

            m2y1_transform_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                mx,
                my,
                active_qp,
                coeffs
            );

        } else if (m == M2Y1_TRANSFORM) {
            if (!have_prev) {
                return -18;
            }

            /*
                TRANSFORM payload after mode:
                    reserved byte
                    16 signed coefficients
            */
            if (off + 1 + (size_t)(g_m2y1_nkeep * 4) > n) {
                return -19;
            }

            off++; /* reserved */

            int8_t coeffs[MIVF_T_MAX_NSLOT];

            for (int i = 0; i < g_m2y1_nkeep * 4; i++) {
                coeffs[i] = (int8_t)src[off++];
            }

            m2y1_transform_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                0,
                0,
                active_qp,
                coeffs
            );

        } else if (m == M2Y1_MVTRANSFORM) {
            if (!have_prev) {
                return -20;
            }

            /*
                MVTRANSFORM payload after mode:
                    signed mx byte
                    signed my byte
                    reserved byte
                    16 signed coefficients
            */
            if (off + 3 + (size_t)(g_m2y1_nkeep * 4) > n) {
                return -21;
            }

            int mx = (int)(int8_t)src[off++];
            int my = (int)(int8_t)src[off++];

            off++; /* reserved */

            int8_t coeffs[MIVF_T_MAX_NSLOT];

            for (int i = 0; i < g_m2y1_nkeep * 4; i++) {
                coeffs[i] = (int8_t)src[off++];
            }

            m2y1_transform_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                mx,
                my,
                active_qp,
                coeffs
            );

        } else if (m == M2Y1_MVQRES) {
            if (!have_prev) {
                return -16;
            }

            if (off + 19 > n) {
                return -17;
            }

            int mx = (int)(int8_t)src[off++];
            int my = (int)(int8_t)src[off++];
            int global_delta = (int)(int8_t)src[off++];

            int8_t residuals[16];

            for (int i = 0; i < 16; i++) {
                residuals[i] = (int8_t)src[off++];
            }

            m2y1_mvqres_block(
                dst,
                prev,
                plane_w,
                plane_h,
                bx,
                by,
                mx,
                my,
                global_delta,
                residuals
            );

        } else if (m == M2Y1_QRES) {
            if (!have_prev) {
                return -12;
            }

            if (off + 17 > n) {
                return -13;
            }

            int global_delta = (int)(int8_t)src[off++];

            int8_t residuals[16];

            for (int i = 0; i < 16; i++) {
                residuals[i] = (int8_t)src[off++];
            }

            m2y1_qres_block(
                dst,
                prev,
                plane_w,
                bx,
                by,
                global_delta,
                residuals
            );

        } else if (m == M2Y1_SOLID) {
            if (off >= n) {
                return -10;
            }

            u8 value = src[off++];
            m2y1_solid_block(dst, plane_w, bx, by, value);

        } else {
            return -11;
        }

        bx++;

        if (bx >= bxcount) {
            bx = 0;
            by++;
        }

        bi++;
    }

    return 0;
}


/* ------------------------------------------------------------------------- */
/* libavcodec wrapper                                                        */
/* ------------------------------------------------------------------------- */

typedef struct M2Y1DecContext {
    uint8_t *cur;         ///< Y, Cb, Cr of the frame being decoded
    uint8_t *prev;        ///< the previous decoded frame, the reference
    int have_prev;
    MivfRcO1 *rc;
    uint8_t *raw;         ///< M2Y2: the range-decoded M2Y1 token bytes
    unsigned raw_size;
} M2Y1DecContext;

static int m2y1_alloc(AVCodecContext *avctx, int w, int h)
{
    M2Y1DecContext *s = avctx->priv_data;
    int ret;

    if ((w & 15) || (h & 15) || !w || !h)
        return AVERROR_INVALIDDATA;
    if ((ret = ff_set_dimensions(avctx, w, h)) < 0)
        return ret;
    av_freep(&s->cur);
    av_freep(&s->prev);
    s->cur  = av_malloc(w * h * 3 / 2);
    s->prev = av_mallocz(w * h * 3 / 2);
    s->have_prev = 0;
    return s->cur && s->prev ? 0 : AVERROR(ENOMEM);
}

static av_cold int m2y1_decode_init(AVCodecContext *avctx)
{
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avctx->width && avctx->height)
        return m2y1_alloc(avctx, avctx->width, avctx->height);
    return 0;
}

static int m2y1_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *pkt)
{
    M2Y1DecContext *s = avctx->priv_data;
    const uint8_t *p = pkt->data, *planes;
    int w, h, keyframe, nkeep, hdr, ret;
    uint32_t sizes[3];

    if (pkt->size < 28 || memcmp(p, "M2Y", 3) || (p[3] != '1' && p[3] != '2'))
        return AVERROR_INVALIDDATA;
    hdr = p[3] == '2' ? 32 : 28;
    if (pkt->size < hdr)
        return AVERROR_INVALIDDATA;
    w = AV_RL16(p + 4);
    h = AV_RL16(p + 6);
    keyframe = p[12] == 1;
    nkeep = mivf_t_resolve(p[13]);
    for (int i = 0; i < 3; i++)
        sizes[i] = AV_RL32(p + 16 + 4 * i);
    if ((uint64_t)sizes[0] + sizes[1] + sizes[2] > 8 * 1024 * 1024)
        return AVERROR_INVALIDDATA;

    if (w != avctx->width || h != avctx->height || !s->cur)
        if ((ret = m2y1_alloc(avctx, w, h)) < 0)
            return ret;
    if (!keyframe && !s->have_prev) {
        av_log(avctx, AV_LOG_WARNING, "skipping a predicted frame with no reference\n");
        return AVERROR_INVALIDDATA;
    }

    if (hdr == 32) {
        /* M2Y2: the M2Y1 token bytes, range-coded as one block. */
        uint32_t comp = AV_RL32(p + 28);
        unsigned raw_total = sizes[0] + sizes[1] + sizes[2];

        if (comp == 0 || comp > pkt->size - 32 || !raw_total)
            return AVERROR_INVALIDDATA;
        if (!s->rc && !(s->rc = av_mallocz(sizeof(*s->rc))))
            return AVERROR(ENOMEM);
        av_fast_malloc(&s->raw, &s->raw_size, raw_total);
        if (!s->raw)
            return AVERROR(ENOMEM);
        mivf_rc_o1_decompress(s->rc, p + 32, comp, s->raw, raw_total);
        planes = s->raw;
    } else {
        if ((uint64_t)sizes[0] + sizes[1] + sizes[2] > pkt->size - 28)
            return AVERROR_INVALIDDATA;
        planes = p + 28;
    }

    for (int i = 0; i < 3; i++) {
        const size_t off = i ? (size_t)w * h + (i - 1) * (size_t)(w / 2) * (h / 2) : 0;
        const int pw = i ? w / 2 : w, ph = i ? h / 2 : h;

        if (dec_m2y1_plane(planes, sizes[i], s->cur + off, s->prev + off,
                           s->have_prev, pw, ph, nkeep)) {
            av_log(avctx, AV_LOG_ERROR, "corrupt %s plane\n", i ? "chroma" : "luma");
            return AVERROR_INVALIDDATA;
        }
        planes += sizes[i];
    }

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    av_image_copy_plane(frame->data[0], frame->linesize[0], s->cur, w, w, h);
    av_image_copy_plane(frame->data[1], frame->linesize[1], s->cur + w * h,
                        w / 2, w / 2, h / 2);
    av_image_copy_plane(frame->data[2], frame->linesize[2],
                        s->cur + w * h + (w / 2) * (h / 2), w / 2, w / 2, h / 2);
    FFSWAP(uint8_t *, s->cur, s->prev);
    s->have_prev = 1;

    frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    if (keyframe)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;
    *got_frame = 1;
    return pkt->size;
}

static void m2y1_decode_flush(AVCodecContext *avctx)
{
    M2Y1DecContext *s = avctx->priv_data;
    s->have_prev = 0;
}

static av_cold int m2y1_decode_close(AVCodecContext *avctx)
{
    M2Y1DecContext *s = avctx->priv_data;

    av_freep(&s->cur);
    av_freep(&s->prev);
    av_freep(&s->rc);
    av_freep(&s->raw);
    return 0;
}

const FFCodec ff_m2y1_decoder = {
    .p.name         = "m2y1",
    CODEC_LONG_NAME("MIVF M2Y1 video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_M2Y1,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(M2Y1DecContext),
    .init           = m2y1_decode_init,
    FF_CODEC_DECODE_CB(m2y1_decode_frame),
    .flush          = m2y1_decode_flush,
    .close          = m2y1_decode_close,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
