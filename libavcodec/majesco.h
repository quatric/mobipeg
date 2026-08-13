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
 * Generic LZ+Huffman byte-stream decompressor described in US Patent
 * 7353233 ("Apparatus and method for real-time decompression of data
 * streams on limited-resource embedded devices"), inventor Alexandre
 * Ganca, assigned to Majesco Entertainment Co. DEFLATE-shaped: 288
 * literals, 32 distance codes, primary/secondary Huffman lookup tables.
 * It is NOT the ActImagine/Mobiclip VX codec (see vx.c).
 *
 * IMPORTANT - this is NOT what retail ADS-era GBA Video actually runs.
 * A Ghidra pass over Dragon Ball GT (the reference ADS cart) found its
 * decompressor to be stock LZMA, matching the canonical probability-model
 * layout byte for byte; no DEFLATE-style Huffman decoder is present in
 * that ROM at all. See doc/gba_video_ads.md for the full analysis and the
 * EWRAM/IWRAM split needed to even see that code. This file is therefore
 * an unverified port kept for reference: the patent scheme may belong to
 * another title, another asset class, or a later revision, but it has not
 * been located in any sample yet, so nothing here is on a decode path.
 *
 * Reversed from the uncompressed ARM that Dora the Explorer Volume 1 keeps at
 * the tail of its ROM; see majesco.c for the deviations from stock DEFLATE.
 */

#ifndef AVCODEC_MAJESCO_H
#define AVCODEC_MAJESCO_H

#include <stdint.h>

/* Decompresses a blob into dst, which must be exactly the size the blob's
 * uint32 prefix declares. Returns the number of bytes produced, or a negative
 * AVERROR - notably AVERROR_PATCHWELCOME for the fourth block type, which no
 * known stream uses. */
int ff_majesco_inflate(const uint8_t *src, int src_size,
                       uint8_t *dst, uint32_t dst_size);

/* First 4 bytes of a Majesco-compressed blob: little-endian output size. */
uint32_t ff_majesco_get_output_size(const uint8_t *src, int src_size);

/* Compresses src into a freshly allocated blob that ff_majesco_inflate() reads
 * back byte for byte, picking whichever of stored, fixed and dynamic Huffman
 * comes out smallest. On success *dst is the caller's to free and the return
 * value is *dst_size; on failure nothing is allocated. */
int ff_majesco_deflate(const uint8_t *src, int src_size,
                       uint8_t **dst, int *dst_size);

#endif /* AVCODEC_MAJESCO_H */
