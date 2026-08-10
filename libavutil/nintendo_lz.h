/*
 * Nintendo LZ10 (compression type 0x10)
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

/**
 * @file
 * Nintendo's LZ77 variant, tagged 0x10 in the one-byte compression-type field
 * its BIOS decompression routines take. It wraps all sorts of Nintendo assets
 * -- RocketVideo frames on the DS, Wii banner sound files -- so it lives here
 * rather than in whichever library happened to need it first.
 *
 * Stream layout: a 4-byte header (0x10, then a 24-bit little-endian
 * decompressed size), then groups of eight tokens preceded by a flag byte,
 * MSB first. A clear flag bit means one literal byte; a set bit means a
 * two-byte back-reference, 3..18 bytes long from 1..4096 bytes back.
 */

#ifndef AVUTIL_NINTENDO_LZ_H
#define AVUTIL_NINTENDO_LZ_H

#include <stdint.h>

/** Type byte that opens an LZ10 stream. */
#define AV_NINTENDO_LZ10_TAG 0x10

/**
 * Decompressed size an LZ10 stream declares, or a negative AVERROR if the
 * buffer is too short or is not LZ10 at all. Lets a caller size its output
 * buffer before committing to the decompression.
 */
int avpriv_nintendo_lz10_size(const uint8_t *src, int src_size);

/**
 * Decompress an LZ10 stream.
 *
 * @return number of bytes written, or a negative AVERROR. Fails rather than
 *         truncating if dst_cap is smaller than the declared size.
 */
int avpriv_nintendo_lz10_decompress(const uint8_t *src, int src_size,
                                    uint8_t *dst, int dst_cap);

/** A buffer of this size can always hold the compression of @p size bytes. */
int avpriv_nintendo_lz10_bound(int size);

/**
 * Compress into LZ10, with lazy matching: a match is deferred by one byte when
 * the next position yields a strictly longer one, which never costs more and
 * usually shrinks the stream.
 *
 * Back-references of distance 1 are emitted. The BIOS routine that
 * decompresses into main RAM handles them, and the reference tools produce
 * them, so this stays byte-compatible with existing Nintendo tooling -- but
 * it means the output must not be fed to the VRAM decompression entry point.
 *
 * @return compressed length, padded to a multiple of 4, or a negative AVERROR
 *         if it would exceed out_cap.
 */
int avpriv_nintendo_lz10_compress(const uint8_t *data, int data_size,
                                  uint8_t *out, int out_cap);

#endif /* AVUTIL_NINTENDO_LZ_H */
