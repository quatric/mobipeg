/*
 * LZMA variant used by ADS-era (Majesco) GBA Video
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

#ifndef AVCODEC_ADSLZMA_H
#define AVCODEC_ADSLZMA_H

#include <stdint.h>

/**
 * Decode a raw LZMA stream with the fixed properties this format uses
 * (lc = 0, lp = 0, pb = 2, no end marker).
 *
 * @param src      compressed input
 * @param src_size bytes available in @p src
 * @param dst      output buffer, at least @p dst_size bytes
 * @param dst_size number of bytes to produce
 * @return number of input bytes consumed, or a negative AVERROR
 */
int ff_ads_lzma_decode_raw(const uint8_t *src, int src_size,
                           uint8_t *dst, int dst_size);

/**
 * Decode a blob carrying this format's 8-byte
 * [uint32 uncompressed_size][uint32 params] prefix.
 *
 * On success *dst points at a freshly allocated buffer the caller must free
 * with av_free(), and *dst_size holds its length.
 *
 * @return number of input bytes consumed, or a negative AVERROR
 */
int ff_ads_lzma_decode_blob(const uint8_t *src, int src_size,
                            uint8_t **dst, int *dst_size);

#endif /* AVCODEC_ADSLZMA_H */
