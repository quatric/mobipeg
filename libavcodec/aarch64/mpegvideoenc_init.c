/*
 * NEON quantiser for the mpegvideo encoders
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
 */

#include "config.h"

#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/aarch64/cpu.h"
#include "libavcodec/idctdsp.h"
#include "libavcodec/mpegvideoenc.h"

#if HAVE_NEON && HAVE_INTRINSICS_NEON
#include <arm_neon.h>

/**
 * Same results as ff_dct_quantize_c(), coefficient for coefficient.
 *
 * The C version walks the scan twice with a 64-bit product per coefficient.
 * Every coefficient is in fact quantised on its own: with a = |coef| * qmat
 * it becomes sign(coef) * ((a + bias) >> QMAT_SHIFT) when that is at least
 * one, and zero otherwise.  So all 64 are done at once in raster order, and
 * the index of the last one is the largest scan position among the nonzero
 * results.
 */
static int dct_quantize_neon(MPVEncContext *const s, int16_t *block, int n,
                             int qscale, int *overflow)
{
    const int intra = s->c.mb_intra;
    const uint8_t *scantable;
    const uint32_t *qmat;
    int bias, dc = 0, last;

    if (intra) {
        scantable = s->c.intra_scantable.scantable;
        qmat      = (const uint32_t *)(n < 4 ? s->q_intra_matrix[qscale]
                                             : s->q_chroma_intra_matrix[qscale]);
        bias      = s->intra_quant_bias * (1 << (QMAT_SHIFT - QUANT_BIAS_SHIFT));
    } else {
        scantable = s->c.inter_scantable.scantable;
        qmat      = (const uint32_t *)s->q_inter_matrix[qscale];
        bias      = s->inter_quant_bias * (1 << (QMAT_SHIFT - QUANT_BIAS_SHIFT));
    }

    /* Outside this range the unsigned thresholds of the C version wrap. */
    if (bias >= (1 << QMAT_SHIFT) || bias < -(1 << 30))
        return ff_dct_quantize_c(s, block, n, qscale, overflow);

    s->fdsp.fdct(block);

    if (s->dct_error_sum) {
        s->dct_count[intra]++;
        s->mpvencdsp.denoise_dct(block, s->dct_error_sum[intra],
                                 s->dct_offset[intra]);
    }

    if (intra) {
        int q;

        if (!s->c.h263_aic)
            q = (n < 4 ? s->c.y_dc_scale : s->c.c_dc_scale) << 3;
        else
            q = 1 << 3;
        /* note: block[0] is assumed to be positive */
        dc       = (block[0] + (q >> 1)) / q;
        block[0] = 0;
    }

    if (s->dct_quantize_scan != scantable) {
        for (int i = 0; i < 64; i++)
            s->dct_quantize_scan_pos[scantable[i]] = i + 1;
        s->dct_quantize_scan = scantable;
    }

    const uint64x2_t vbias = vreinterpretq_u64_s64(vdupq_n_s64(bias));
    const int32x4_t  zero  = vdupq_n_s32(0);
    int32x4_t  vmax = zero;
    int16x8_t  row[8];
    uint8x8_t  nz[8];

    for (int i = 0; i < 8; i++) {
        const int16x8_t  b    = vld1q_s16(block + 8 * i);
        const uint16x8_t a    = vreinterpretq_u16_s16(vabsq_s16(b));
        const uint32x4_t a_lo = vmovl_u16(vget_low_u16(a));
        const uint32x4_t a_hi = vmovl_high_u16(a);
        const uint32x4_t q_lo = vld1q_u32(qmat + 8 * i);
        const uint32x4_t q_hi = vld1q_u32(qmat + 8 * i + 4);
        const int64x2_t  p0   = vreinterpretq_s64_u64(vmlal_u32(vbias, vget_low_u32(a_lo), vget_low_u32(q_lo)));
        const int64x2_t  p1   = vreinterpretq_s64_u64(vmlal_high_u32(vbias, a_lo, q_lo));
        const int64x2_t  p2   = vreinterpretq_s64_u64(vmlal_u32(vbias, vget_low_u32(a_hi), vget_low_u32(q_hi)));
        const int64x2_t  p3   = vreinterpretq_s64_u64(vmlal_high_u32(vbias, a_hi, q_hi));
        const int32x4_t  l_lo = vmaxq_s32(vshrn_high_n_s64(vshrn_n_s64(p0, QMAT_SHIFT), p1, QMAT_SHIFT), zero);
        const int32x4_t  l_hi = vmaxq_s32(vshrn_high_n_s64(vshrn_n_s64(p2, QMAT_SHIFT), p3, QMAT_SHIFT), zero);
        const int16x8_t  mag  = vmovn_high_s32(vmovn_s32(l_lo), l_hi);
        const int16x8_t  sign = vshrq_n_s16(b, 15);

        vmax   = vorrq_s32(vmax, vorrq_s32(l_lo, l_hi));
        row[i] = vsubq_s16(veorq_s16(mag, sign), sign);
        nz[i]  = vmovn_u16(vmovn_high_u32(vmovn_u32(vcgtq_s32(l_lo, zero)),
                                          vcgtq_s32(l_hi, zero)));
    }

    {
        const uint8_t *const pos = s->dct_quantize_scan_pos;
        const uint8x16_t m0 = vandq_u8(vcombine_u8(nz[0], nz[1]), vld1q_u8(pos));
        const uint8x16_t m1 = vandq_u8(vcombine_u8(nz[2], nz[3]), vld1q_u8(pos + 16));
        const uint8x16_t m2 = vandq_u8(vcombine_u8(nz[4], nz[5]), vld1q_u8(pos + 32));
        const uint8x16_t m3 = vandq_u8(vcombine_u8(nz[6], nz[7]), vld1q_u8(pos + 48));
        const int64x2_t  mx = vreinterpretq_s64_s32(vmax);

        last = vmaxvq_u8(vmaxq_u8(vmaxq_u8(m0, m1), vmaxq_u8(m2, m3))) - 1;
        if (intra && last < 0)
            last = 0;

        const uint64_t max = vgetq_lane_s64(mx, 0) | vgetq_lane_s64(mx, 1);
        *overflow = s->max_qcoeff < (int)((uint32_t)max | (uint32_t)(max >> 32));
    }

    /* we need this permutation so that we correct the IDCT */
    if (s->c.idsp.perm_type == FF_IDCT_PERM_PARTTRANS) {
        /* each 4x4 quarter of the block is transposed */
        for (int i = 0; i < 8; i += 4) {
            const int16x8_t t0 = vtrn1q_s16(row[i],     row[i + 1]);
            const int16x8_t t1 = vtrn2q_s16(row[i],     row[i + 1]);
            const int16x8_t t2 = vtrn1q_s16(row[i + 2], row[i + 3]);
            const int16x8_t t3 = vtrn2q_s16(row[i + 2], row[i + 3]);

            row[i]     = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2)));
            row[i + 1] = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3)));
            row[i + 2] = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t0), vreinterpretq_s32_s16(t2)));
            row[i + 3] = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(t1), vreinterpretq_s32_s16(t3)));
        }
    }

    for (int i = 0; i < 8; i++)
        vst1q_s16(block + 8 * i, row[i]);

    if (intra)
        block[0] = dc;

    if (s->c.idsp.perm_type != FF_IDCT_PERM_NONE &&
        s->c.idsp.perm_type != FF_IDCT_PERM_PARTTRANS)
        ff_block_permute(block, s->c.idsp.idct_permutation, scantable, last);

    return last;
}
#endif /* HAVE_NEON && HAVE_INTRINSICS_NEON */

av_cold void ff_dct_encode_init_aarch64(MPVEncContext *const s)
{
#if HAVE_NEON && HAVE_INTRINSICS_NEON
    int cpu_flags = av_get_cpu_flags();

    if (have_neon(cpu_flags))
        s->dct_quantize = dct_quantize_neon;
#endif
}
