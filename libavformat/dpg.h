/*
 * Nintendo DS DPG (nDs-mPeG) shared definitions
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
 * DPG is the video container MoonShell plays on the Nintendo DS: an MPEG-1
 * video elementary stream and an MP2 audio elementary stream stored side by
 * side, with a small little-endian header saying where each begins.
 *
 * Five versions exist, each adding fields to the end of the header:
 *
 *   DPG0  0x24 bytes  the base header
 *   DPG1  0x24 bytes  as DPG0; stereo audio
 *   DPG2  0x30 bytes  adds the GOP index and the pixel format
 *   DPG3  0x30 bytes  as DPG2
 *   DPG4  0x34 bytes  adds a "THM0" marker; a thumbnail follows the header
 *
 * There has never been an official specification. The field table here
 * follows dpg4x's, which is the closest thing to a reference encoder. Two
 * details are not stated there and are noted where they are used: how DPG2+
 * encodes a fractional frame rate, and what a GOP index entry contains.
 */

#ifndef AVFORMAT_DPG_H
#define AVFORMAT_DPG_H

#define DPG_HEADER_SIZE_V0   0x24
#define DPG_HEADER_SIZE_V2   0x30
#define DPG_HEADER_SIZE_V4   0x34

/* 256x192 16-bit pixels: the DS screen, as raw values with no image header. */
#define DPG_THUMB_WIDTH       256
#define DPG_THUMB_HEIGHT      192
#define DPG_THUMB_SIZE       (DPG_THUMB_WIDTH * DPG_THUMB_HEIGHT * 2)

/* MoonShell's pixel format field. 3 is the 24-bit mode every current build
 * uses; the lower values are older 15/18/21-bit modes. */
#define DPG_PIXEL_FORMAT_RGB24  3

static inline int ff_dpg_header_size(int version)
{
    if (version >= 4)
        return DPG_HEADER_SIZE_V4;
    if (version >= 2)
        return DPG_HEADER_SIZE_V2;
    return DPG_HEADER_SIZE_V0;
}

#endif /* AVFORMAT_DPG_H */
