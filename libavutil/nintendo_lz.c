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

#include "common.h"
#include "error.h"
#include "nintendo_lz.h"

int avpriv_nintendo_lz10_size(const uint8_t *src, int src_size)
{
    if (src_size < 4 || src[0] != AV_NINTENDO_LZ10_TAG)
        return AVERROR_INVALIDDATA;
    return src[1] | (src[2] << 8) | (src[3] << 16);
}

int avpriv_nintendo_lz10_decompress(const uint8_t *src, int src_size,
                                    uint8_t *dst, int dst_cap)
{
    int p, o = 0, out_size = avpriv_nintendo_lz10_size(src, src_size);

    if (out_size < 0)
        return out_size;
    if (out_size > dst_cap)
        return AVERROR_INVALIDDATA;

    p = 4;
    while (o < out_size) {
        int flags, i;
        if (p >= src_size)
            return AVERROR_INVALIDDATA;
        flags = src[p++];
        for (i = 0; i < 8 && o < out_size; i++) {
            if (flags & 0x80) {
                int b0, b1, len, back, k;
                if (p + 2 > src_size)
                    return AVERROR_INVALIDDATA;
                b0 = src[p++]; b1 = src[p++];
                len  = (b0 >> 4) + 3;
                back = (((b0 & 0xF) << 8) | b1) + 1;
                if (back > o || o + len > out_size)
                    return AVERROR_INVALIDDATA;
                for (k = 0; k < len; k++, o++)
                    dst[o] = dst[o - back];
            } else {
                if (p >= src_size)
                    return AVERROR_INVALIDDATA;
                dst[o++] = src[p++];
            }
            flags = (flags << 1) & 0xFF;
        }
    }
    return out_size;
}

int avpriv_nintendo_lz10_bound(int size)
{
    /* Worst case is all literals: one flag byte per eight of them, plus the
     * 4-byte header and up to 3 bytes of tail padding. */
    return 4 + size + (size + 7) / 8 + 3;
}

/* Longest back-reference for the byte at data+offs. Returns the match length
 * (0 if none is at least 3 bytes) and stores the distance in *back_out. */
static int lz10_match(const uint8_t *data, int offs, int length, int *back_out)
{
    int maxnum  = FFMIN(18, length - offs);
    int maxback = FFMIN(0x1000, offs);
    const uint8_t *dp = data + offs;
    const uint8_t *ptr = dp - 1;
    const uint8_t *minptr = dp - maxback;
    int nr = 2, back = 1;

    if (maxnum < 3)
        return 0;
    while (minptr <= ptr) {
        if (ptr[0] == dp[0] && ptr[1] == dp[1] && ptr[2] == dp[2]) {
            int tmpnr = 3;
            while (tmpnr < maxnum && ptr[tmpnr] == dp[tmpnr])
                tmpnr++;
            if (tmpnr > nr) {
                nr = tmpnr;
                back = (int)(dp - ptr);
                if (nr == maxnum)
                    break;
            }
        }
        --ptr;
    }
    *back_out = back;
    return nr > 2 ? nr : 0;
}

int avpriv_nintendo_lz10_compress(const uint8_t *data, int data_size,
                                  uint8_t *out, int out_cap)
{
    int dstoffs = 4, length = data_size, offs = 0;
    int headeroffs = -1, nbits = 0;
    uint8_t header = 0;

    if (out_cap < 4)
        return AVERROR(ENOMEM);
    out[0] = AV_NINTENDO_LZ10_TAG;
    out[1] = data_size & 0xFF;
    out[2] = (data_size >> 8) & 0xFF;
    out[3] = (data_size >> 16) & 0xFF;

    while (offs < length) {
        int back, len, comp = 0;

        if (nbits == 0) {           /* start a new 8-token flag group */
            if (dstoffs >= out_cap)
                return AVERROR(ENOMEM);
            headeroffs = dstoffs++;
            header = 0;
        }

        len = lz10_match(data, offs, length, &back);
        if (len >= 3 && offs + 1 < length) {
            int nback, nlen = lz10_match(data, offs + 1, length, &nback);
            if (nlen > len)
                len = 0;
        }

        if (len >= 3) {
            if (dstoffs + 2 > out_cap)
                return AVERROR(ENOMEM);
            out[dstoffs++] = (((back - 1) >> 8) & 0xF) | (((len - 3) & 0xF) << 4);
            out[dstoffs++] = (back - 1) & 0xFF;
            offs += len;
            comp = 1;
        } else {
            if (dstoffs + 1 > out_cap)
                return AVERROR(ENOMEM);
            out[dstoffs++] = data[offs++];
        }

        header = (header << 1) | (comp & 1);
        if (++nbits == 8) {
            out[headeroffs] = header;
            nbits = 0;
        }
    }
    if (nbits) {                     /* flush a partial final flag group */
        header <<= (8 - nbits);
        out[headeroffs] = header;
    }
    while (dstoffs & 3) {
        if (dstoffs >= out_cap)
            return AVERROR(ENOMEM);
        out[dstoffs++] = 0;
    }
    return dstoffs;
}
