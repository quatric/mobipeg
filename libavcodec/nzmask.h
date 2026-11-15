/*
 * Which coefficients of a block are not zero, in scan order
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

#ifndef AVCODEC_NZMASK_H
#define AVCODEC_NZMASK_H

#include <stdint.h>

#include "config.h"
#include "libavutil/attributes.h"

#if ARCH_AARCH64 && HAVE_NEON && HAVE_INTRINSICS_NEON
#include <arm_neon.h>
#endif

/**
 * An entropy coder that walks a block in scan order spends most of its time
 * on the zeros and on mispredicting which coefficient is one.  With this
 * mask it visits the nonzero coefficients only.
 *
 * @return a mask whose bit i is set when block[scan[i]] is not zero
 */
static av_always_inline uint64_t ff_block_nonzero_mask(const int16_t block[64],
                                                       const uint8_t scan[64])
{
#if ARCH_AARCH64 && HAVE_NEON && HAVE_INTRINSICS_NEON
    static const uint8_t bit[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128,
    };
    const uint8x16_t bits = vld1q_u8(bit);
    uint8x16x4_t nz;
    uint8x16_t m[4];

    /* one byte per coefficient, in raster order */
    for (int i = 0; i < 4; i++) {
        const int16x8_t a = vld1q_s16(block + 16 * i);
        const int16x8_t b = vld1q_s16(block + 16 * i + 8);

        nz.val[i] = vuzp1q_u8(vreinterpretq_u8_u16(vtstq_s16(a, a)),
                              vreinterpretq_u8_u16(vtstq_s16(b, b)));
    }
    /* put them in scan order and keep one bit of each */
    for (int i = 0; i < 4; i++)
        m[i] = vandq_u8(vqtbl4q_u8(nz, vld1q_u8(scan + 16 * i)), bits);

    /* add up each group of eight bytes */
    m[0] = vpaddq_u8(vpaddq_u8(m[0], m[1]), vpaddq_u8(m[2], m[3]));
    m[0] = vpaddq_u8(m[0], m[0]);

    return vgetq_lane_u64(vreinterpretq_u64_u8(m[0]), 0);
#else
    uint64_t mask = 0;

    for (int i = 0; i < 64; i++)
        mask |= (uint64_t)(block[scan[i]] != 0) << i;

    return mask;
#endif
}

#endif /* AVCODEC_NZMASK_H */
