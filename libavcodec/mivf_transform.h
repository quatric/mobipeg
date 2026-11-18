/*
 * MIVF M2Y1 4x4 transform, shared by the m2y1 encoder and decoder
 *
 * Ported from the MIVF player and toolkit, which carry this notice:
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

#ifndef AVCODEC_MIVF_TRANSFORM_H
#define AVCODEC_MIVF_TRANSFORM_H

#include <stdint.h>
#include <string.h>

/* Encoder's default keep-count for newly written files. */
#ifndef MIVF_T_NKEEP_DEFAULT
#define MIVF_T_NKEEP_DEFAULT 16
#endif

/* Legacy keep-count for files written before this format field existed. */
#define MIVF_T_NKEEP_LEGACY 4

/* Upper bounds (nkeep <= 16 -> nslot <= 64 -> mask <= 8 bytes). */
#define MIVF_T_MAX_NKEEP 16
#define MIVF_T_MAX_NSLOT 64
#define MIVF_T_TOKMAX 96

static inline int mivf_t_maskbytes(int nkeep) { return (nkeep * 4 + 7) / 8; }

/* Resolve the stored keep field (0 == legacy 4) to an effective keep-count. */
static inline int mivf_t_resolve(int stored) {
    if (stored <= 0) return MIVF_T_NKEEP_LEGACY;
    if (stored > MIVF_T_MAX_NKEEP) return MIVF_T_MAX_NKEEP;
    return stored;
}

/* Kept coefficients per quadrant, in low-frequency-first order. The first four
   entries match the legacy keep set {0,1,4,5}; later entries add detail. */
static const int mivf_t_keep[16] = {
    0, 1, 4, 5,   /* legacy low-frequency 2x2 */
    2, 8, 3, 6,   /* next ring (mid frequency) */
    9, 12, 7, 10, /* high frequency */
    13, 11, 14, 15
};

/* ---- 4x4 integer transform (identical to the historical M2Y1 transform) ---- */

static inline void mivf_t4_fwd(const int16_t input[16], int16_t output[16]) {
    int tmp[16];
    for (int y = 0; y < 4; y++) {
        int x0 = input[y * 4 + 0], x1 = input[y * 4 + 1];
        int x2 = input[y * 4 + 2], x3 = input[y * 4 + 3];
        int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        tmp[y * 4 + 0] = a0 + a1;
        tmp[y * 4 + 1] = a3 + a2;
        tmp[y * 4 + 2] = a0 - a1;
        tmp[y * 4 + 3] = a3 - a2;
    }
    for (int x = 0; x < 4; x++) {
        int x0 = tmp[0 * 4 + x], x1 = tmp[1 * 4 + x];
        int x2 = tmp[2 * 4 + x], x3 = tmp[3 * 4 + x];
        int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        output[0 * 4 + x] = (int16_t)(a0 + a1);
        output[1 * 4 + x] = (int16_t)(a3 + a2);
        output[2 * 4 + x] = (int16_t)(a0 - a1);
        output[3 * 4 + x] = (int16_t)(a3 - a2);
    }
}

static inline void mivf_t4_inv(const int16_t input[16], int16_t output[16]) {
    int tmp[16];
    for (int y = 0; y < 4; y++) {
        int x0 = input[y * 4 + 0], x1 = input[y * 4 + 1];
        int x2 = input[y * 4 + 2], x3 = input[y * 4 + 3];
        int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        tmp[y * 4 + 0] = a0 + a1;
        tmp[y * 4 + 1] = a3 + a2;
        tmp[y * 4 + 2] = a0 - a1;
        tmp[y * 4 + 3] = a3 - a2;
    }
    for (int x = 0; x < 4; x++) {
        int x0 = tmp[0 * 4 + x], x1 = tmp[1 * 4 + x];
        int x2 = tmp[2 * 4 + x], x3 = tmp[3 * 4 + x];
        int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        output[0 * 4 + x] = (int16_t)((a0 + a1 + 8) >> 4);
        output[1 * 4 + x] = (int16_t)((a3 + a2 + 8) >> 4);
        output[2 * 4 + x] = (int16_t)((a0 - a1 + 8) >> 4);
        output[3 * 4 + x] = (int16_t)((a3 - a2 + 8) >> 4);
    }
}

static inline int mivf_t_quant(int v, int q) {
    if (q < 1) q = 1;
    return (v >= 0) ? (v + q / 2) / q : -((-v + q / 2) / q);
}

static inline int mivf_t_dequant(int v, int q) {
    if (q < 1) q = 1;
    return v * q;
}

static inline int8_t mivf_t_cs8(int v) {
    if (v < -128) return -128;
    if (v > 127) return 127;
    return (int8_t)v;
}

static inline uint8_t mivf_t_cu8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* ---- variable-width sparse coding (mask of nslot bits + nonzeros) ---- */

static inline int mivf_t_pack_sparse(const int8_t *vals, int nslot,
                                     uint8_t *out, int out_cap) {
    uint64_t mask = 0;
    int maskbytes = (nslot + 7) / 8;
    int pos = maskbytes;
    if (out_cap < maskbytes) return -1;
    for (int i = 0; i < nslot; i++) {
        if (vals[i] != 0) {
            mask |= (uint64_t)1u << i;
            if (pos >= out_cap) return -1;
            out[pos++] = (uint8_t)vals[i];
        }
    }
    for (int b = 0; b < maskbytes; b++) {
        out[b] = (uint8_t)((mask >> (8 * b)) & 0xFF);
    }
    return pos;
}

static inline int mivf_t_read_sparse(const uint8_t *src, size_t n, size_t *off,
                                     int8_t *vals, int nslot) {
    int maskbytes = (nslot + 7) / 8;
    if (*off + (size_t)maskbytes > n) return -1;
    uint64_t mask = 0;
    for (int b = 0; b < maskbytes; b++) {
        mask |= (uint64_t)src[*off + b] << (8 * b);
    }
    *off += maskbytes;
    memset(vals, 0, (size_t)nslot);
    for (int i = 0; i < nslot; i++) {
        if (mask & ((uint64_t)1u << i)) {
            if (*off >= n) return -2;
            vals[i] = (int8_t)src[*off];
            *off += 1;
        }
    }
    return 0;
}

/* ---- decode: reconstruct one 8x8 block from coeffs[NSLOT] into dst ---- */

static inline void mivf_t_decode(uint8_t *dst, const uint8_t *prev,
                                 int plane_w, int plane_h,
                                 int bx, int by, int mx, int my,
                                 int qp, const int8_t *coeffs, int nkeep) {
    int dst_x0 = bx * 8, dst_y0 = by * 8;
    int src_x0 = dst_x0 + mx, src_y0 = dst_y0 + my;

    if (src_x0 < 0 || src_y0 < 0 ||
        src_x0 + 8 > plane_w || src_y0 + 8 > plane_h) {
        src_x0 = dst_x0;
        src_y0 = dst_y0;
    }

    for (int y = 0; y < 8; y++) {
        memcpy(dst + (dst_y0 + y) * plane_w + dst_x0,
               prev + (src_y0 + y) * plane_w + src_x0, 8);
    }

    int coeff_ptr = 0;
    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            int16_t deq[16];
            int16_t inv[16];
            memset(deq, 0, sizeof(deq));

            for (int k = 0; k < nkeep; k++) {
                int ci = mivf_t_keep[k];
                deq[ci] = (int16_t)((int)coeffs[coeff_ptr++] * qp);
            }

            mivf_t4_inv(deq, inv);

            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int px = qx * 4 + x, py = qy * 4 + y;
                    int dx = dst_x0 + px, dy = dst_y0 + py;
                    int sx = src_x0 + px, sy = src_y0 + py;
                    int idx4 = y * 4 + x;
                    int v = (int)prev[sy * plane_w + sx] + (int)inv[idx4];
                    dst[dy * plane_w + dx] = mivf_t_cu8(v);
                }
            }
        }
    }
}

/* ---- encode: build a transform token + closed-loop recon. Returns SAD. ----
 * mv_mode: 0 = intra (same position), 1 = motion (mx,my).
 * sparse : 0 = dense NSLOT coeff bytes, 1 = sparse (mask + nonzeros).
 * mode_dense / mode_sparse: the mode byte values to emit (caller supplies the
 * correct M_TRANSFORM / M_MVTRANSFORM / *Z variants).
 */
static inline int mivf_t_make(const uint8_t cur[64], const uint8_t pred[64],
                              int qp, int mv_mode, int mx, int my, int sparse,
                              uint8_t mode_byte,
                              uint8_t *token, int token_cap, uint8_t recon[64],
                              int *out_len, int nkeep) {
    int nslot = nkeep * 4;
    int8_t coeff_out[MIVF_T_MAX_NSLOT];
    int out_coeff = 0;

    memcpy(recon, pred, 64);

    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            int16_t residual[16], coeff[16], deq[16], inv[16];
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int idx8 = (qy * 4 + y) * 8 + (qx * 4 + x);
                    residual[y * 4 + x] = (int16_t)((int)cur[idx8] - (int)pred[idx8]);
                }
            }
            mivf_t4_fwd(residual, coeff);
            memset(deq, 0, sizeof(deq));

            for (int k = 0; k < nkeep; k++) {
                int ci = mivf_t_keep[k];
                int qv = mivf_t_quant((int)coeff[ci], qp);
                qv = (int)mivf_t_cs8(qv);
                coeff_out[out_coeff++] = (int8_t)qv;
                deq[ci] = (int16_t)mivf_t_dequant(qv, qp);
            }

            mivf_t4_inv(deq, inv);

            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int idx8 = (qy * 4 + y) * 8 + (qx * 4 + x);
                    recon[idx8] = mivf_t_cu8((int)pred[idx8] + (int)inv[y * 4 + x]);
                }
            }
        }
    }

    int pos = 0;
    token[pos++] = mode_byte;
    if (mv_mode) {
        token[pos++] = (uint8_t)(int8_t)mx;
        token[pos++] = (uint8_t)(int8_t)my;
    }
    token[pos++] = 0; /* reserved */

    if (sparse) {
        int packed = mivf_t_pack_sparse(coeff_out, nslot, token + pos, token_cap - pos);
        if (packed < 0) return -1;
        pos += packed;
    } else {
        if (pos + nslot > token_cap) return -1;
        for (int i = 0; i < nslot; i++) {
            token[pos++] = (uint8_t)coeff_out[i];
        }
    }

    int sad = 0;
    for (int i = 0; i < 64; i++) {
        int d = (int)cur[i] - (int)recon[i];
        sad += (d < 0) ? -d : d;
    }

    if (out_len) *out_len = pos;
    return sad;
}

#endif /* MIVF_TRANSFORM_H */
