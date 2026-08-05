/*
 * Majesco embedded-device Huffman decompressor (US Patent 7353233)
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

/*
 * Generic Huffman byte-stream decompressor used underneath the "ADS-era"
 * GBA Video engine (e.g. Majesco's Dragon Ball GT cart, SFCD/mmstr
 * containers). It is NOT the ActImagine/Mobiclip VX codec (see vx.c) - it
 * is a separate, in-house, general-purpose LZ+Huffman scheme described in
 * US Patent 7353233 ("Apparatus and method for real-time decompression of
 * data streams on limited-resource embedded devices"), assigned to
 * Majesco Entertainment Co. The actual ADS video-frame layer sits on top
 * of whatever this decompresses, and has not been reverse engineered yet.
 *
 * Ported from Gericom's unfinished C# prototype (MobiclipDecoder repo,
 * commit c88b67d "Improved the encoder a lot!", LibMobiclip/Codec/Majesco).
 * That prototype never implemented the full block-decode state machine
 * (only states 0 and 1 of what is clearly a larger switch existed, and
 * Inflate() returned NULL) - this port preserves that boundary instead of
 * guessing at the missing states.
 */

#ifndef AVCODEC_MAJESCO_H
#define AVCODEC_MAJESCO_H

#include <stdint.h>

/* Returns 1 while progress toward a full decoder can still be made from
 * what has been reverse engineered, 0 once state machine states beyond
 * what's known are hit. On 0, *out_size bytes have not necessarily all
 * been produced - the caller must treat the result as unusable and this
 * as a marker that further RE (Ghidra pass) is needed for that block. */
int ff_majesco_inflate(const uint8_t *src, int src_size,
                        uint8_t *dst, uint32_t dst_size);

/* First 4 bytes of a Majesco-compressed blob: little-endian output size. */
uint32_t ff_majesco_get_output_size(const uint8_t *src, int src_size);

#endif /* AVCODEC_MAJESCO_H */
