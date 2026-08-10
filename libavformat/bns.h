/*
 * Wii BNS (banner sound) shared definitions
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

#ifndef AVFORMAT_BNS_H
#define AVFORMAT_BNS_H

/** Byte-order mark and version, as one big-endian word: 0xFEFF then 1.0. */
#define BNS_VERSION 0xFEFF0100u

/** An IMD5 wrapper is a fixed 0x20-byte prologue. */
#define BNS_IMD5_SIZE 0x20

/** Per-channel DSP block: 16 coefficients plus the state fields after them. */
#define BNS_CHANNEL_DSP_SIZE 0x2E

#endif /* AVFORMAT_BNS_H */
