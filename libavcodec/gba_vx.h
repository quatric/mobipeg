/*
 * ActImagine GBA VX packet framing
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

/* The demuxer preserves the source revision and its stream quantizer. */
#define GBA_VX_EXTRADATA_SIZE 8
#define GBA_VX_MAGIC_VXPP MKTAG('V', 'X', '+', '+')
#define GBA_VX_MAGIC_VXGB MKTAG('V', 'X', 'G', 'B')

#define GBA_VX_VLC_TABLE_SIZE  8192
#define GBA_VX_VALUE_TABLE_SIZE 128
#define GBA_VX_RUN_TABLE_SIZE   128
#define GBA_VX_VLC_BLOB_SIZE (GBA_VX_VLC_TABLE_SIZE + \
                              GBA_VX_VALUE_TABLE_SIZE + \
                              GBA_VX_RUN_TABLE_SIZE)
#define GBA_VX_VXPP_EXTRADATA_SIZE (GBA_VX_EXTRADATA_SIZE + \
                                    GBA_VX_VLC_BLOB_SIZE)

/* Extradata layout:
 *   +0  u32 source magic ('VX++' or 'VXGB')
 *   +4  u32 stream quantizer
 *   +8  VX++ only: 4096 little-endian VLC cells, then the 128-byte
 *       value-offset and 128-byte run-offset escape tables
 */

/* Header layout:
 *   +0  u32 magic
 *   +4  u32 leading bits to skip in the first aligned halfword
 *   +8  u32 valid bits, including the leading skip
 *   +12 u32 frames in this independently decodable segment
 */

#endif /* AVCODEC_GBA_VX_H */
