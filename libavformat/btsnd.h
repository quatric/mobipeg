/*
 * Wii U boot sound (.btsnd) shared definitions
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

#ifndef AVFORMAT_BTSND_H
#define AVFORMAT_BTSND_H

/** Type word plus loop point. */
#define BTSND_HEADER_SIZE 8

/* The console's boot-sound player has no format negotiation: it reads the
 * header, then treats everything after it as 48 kHz stereo. Neither value is
 * stored anywhere in the file, so both are part of the format. */
#define BTSND_SAMPLE_RATE 48000
#define BTSND_CHANNELS        2

#endif /* AVFORMAT_BTSND_H */
