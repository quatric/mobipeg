/*
 * MobiClip / ActImagine DS (.vx) Video decoder - shared declarations
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

#ifndef AVCODEC_VX_H
#define AVCODEC_VX_H

#include <stdint.h>
#include "avcodec.h"
#include "get_bits.h"

/* One decoded reference picture, as seen by the VX video decode core.
 * Used both for real AVFrame-backed references (video decoder) and for
 * throwaway scratch buffers (audio decoder, which only needs to walk the
 * video bits to find where the audio portion of the packet starts). */
typedef struct VXPic {
    uint8_t *data[3];
    int linesize[3];
} VXPic;

/* Compute the 3-entry quant table from the container's "quantizer" field
 * (0 <= quantizer < 12*6+... see vx_calc_qtab for the valid range). */
int ff_vx_calc_qtab(int quantizer, uint16_t qtab[3]);

/* Decode one VX video frame (avframe payload) from gb into dst, using up to
 * 3 reference pictures (refs[i].data[0] == NULL if unavailable). On return,
 * *gb is left word-aligned exactly where the first AFrame (if any) begins,
 * matching the reference decoder's "align with word" step. */
int ff_vx_decode_vframe(AVCodecContext *avctx, GetBitContext *gb,
                                  int width, int height, const uint16_t qtab[3],
                                  VXPic *dst, const VXPic refs[3]);

#endif /* AVCODEC_VX_H */
