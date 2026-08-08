/*
 * ActImagine GBA VX++ packet framing
 * Copyright (c) 2026 the FFmpeg developers
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef AVCODEC_GBA_VX_H
#define AVCODEC_GBA_VX_H

#include "libavutil/macros.h"

/* A packet is one independently decodable container seek segment. The payload
 * is the source's little-endian halfwords byte-swapped into ordinary MSB-first
 * bitstream order. All header fields are little-endian. */
#define GBA_VX_PACKET_MAGIC MKTAG('G', 'V', 'X', '1')
#define GBA_VX_PACKET_HEADER_SIZE 16

/* Header layout:
 *   +0  u32 magic
 *   +4  u32 leading bits to skip in the first aligned halfword
 *   +8  u32 valid bits, including the leading skip
 *   +12 u32 frames in this independently decodable segment
 */

#endif /* AVCODEC_GBA_VX_H */
