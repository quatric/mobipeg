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

/**
 * @file
 * The compressor the Hydrogen-era GBA Video carts use, in place of the LZMA
 * that the Dragon Ball GT lineage uses. Reversed from Dora the Explorer
 * Volume 1, which ships hyCompressionManager::Inflate as uncompressed ARM at
 * the tail of the ROM (Initialize 0x1ff96d8, UncompressBlock 0x1ff9c0c,
 * CoreExpand_Static 0x1ff9788, read-bits helper 0x1ffc388).
 *
 * Structurally it is DEFLATE - same block types, same code-length alphabet
 * and permutation, same length and distance ladders - with two deviations:
 *
 *   - bits are read most significant first, out of a 32-bit accumulator that
 *     is refilled sixteen at a time from little-endian halfwords, where
 *     DEFLATE reads least significant first;
 *   - a blob is prefixed with a plain uint32 uncompressed size and the coded
 *     data starts at src + 4, with no zlib header.
 *
 * The ROM also carries a fourth block type that DEFLATE does not have (the
 * 2-bit selector's case 3, state 7). No stream seen so far uses it, so it is
 * rejected rather than guessed at.
 */

#include <string.h>
#include "majesco.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/macros.h"

#define MAX_BITS   15
#define NB_LITERAL 288
#define NB_DIST    32
#define NB_CLEN    19

/* Length and distance ladders, interleaved exactly as the ROM keeps them: one
 * eight-byte entry per code holding { dist_base, dist_extra, len_base,
 * len_extra }, indexed by the distance code and by symbol - 0x100 alike. */
static const uint16_t majesco_dist_len_table[30][4] = {
    { 0x0001,  0,    0, 0 }, { 0x0002,  0,    3, 0 }, { 0x0003,  0,    4, 0 },
    { 0x0004,  0,    5, 0 }, { 0x0005,  1,    6, 0 }, { 0x0007,  1,    7, 0 },
    { 0x0009,  2,    8, 0 }, { 0x000d,  2,    9, 0 }, { 0x0011,  3,   10, 0 },
    { 0x0019,  3,   11, 1 }, { 0x0021,  4,   13, 1 }, { 0x0031,  4,   15, 1 },
    { 0x0041,  5,   17, 1 }, { 0x0061,  5,   19, 2 }, { 0x0081,  6,   23, 2 },
    { 0x00c1,  6,   27, 2 }, { 0x0101,  7,   31, 2 }, { 0x0181,  7,   35, 3 },
    { 0x0201,  8,   43, 3 }, { 0x0301,  8,   51, 3 }, { 0x0401,  9,   59, 3 },
    { 0x0601,  9,   67, 4 }, { 0x0801, 10,   83, 4 }, { 0x0c01, 10,   99, 4 },
    { 0x1001, 11,  115, 4 }, { 0x1801, 11,  131, 5 }, { 0x2001, 12,  163, 5 },
    { 0x3001, 12,  195, 5 }, { 0x4001, 13,  227, 5 }, { 0x6001, 13,  258, 0 },
};

/* The order the code-length alphabet's lengths arrive in. */
static const uint8_t majesco_clen_order[NB_CLEN] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

typedef struct BitReader {
    const uint8_t *buf;
    int size;
    int pos;                    /* byte offset of the next halfword */
    uint32_t acc;               /* bits are left-justified */
    int nbits;
    int overrun;
} BitReader;

/** Canonical decode tables, held as counts and symbols per length. */
typedef struct HuffTable {
    uint16_t count[MAX_BITS + 1];
    uint16_t symbol[NB_LITERAL];
    int maxbits;
} HuffTable;

static void br_init(BitReader *br, const uint8_t *buf, int size)
{
    br->buf     = buf;
    br->size    = size & ~1;
    br->pos     = 0;
    br->acc     = 0;
    br->nbits   = 0;
    br->overrun = 0;
}

/* 0x1ffc388: top up to at least n bits, sixteen at a time. */
static unsigned br_read(BitReader *br, int n)
{
    unsigned v;

    if (!n)
        return 0;
    while (br->nbits < n) {
        unsigned hw = 0;

        if (br->pos + 2 <= br->size) {
            hw = AV_RL16(br->buf + br->pos);
            br->pos += 2;
        } else {
            br->overrun = 1;
        }
        br->acc   |= hw << (16 - br->nbits);
        br->nbits += 16;
    }
    v = br->acc >> (32 - n);
    br->acc <<= n;
    br->nbits -= n;
    return v;
}

/** Build a canonical table from a list of code lengths. */
static int build_table(HuffTable *t, const uint8_t *lengths, int n)
{
    int left = 1;

    memset(t->count, 0, sizeof(t->count));
    t->maxbits = 0;

    for (int i = 0; i < n; i++) {
        if (lengths[i] > MAX_BITS)
            return AVERROR_INVALIDDATA;
        t->count[lengths[i]]++;
        if (lengths[i] > t->maxbits)
            t->maxbits = lengths[i];
    }
    if (!t->maxbits)                    /* an empty alphabet is legal */
        return 0;

    /* Reject anything that is not a complete or under-subscribed code. */
    for (int b = 1; b <= MAX_BITS; b++) {
        left <<= 1;
        left -= t->count[b];
        if (left < 0)
            return AVERROR_INVALIDDATA;
    }

    {
        uint16_t offs[MAX_BITS + 2];

        offs[1] = 0;
        for (int b = 1; b <= MAX_BITS; b++)
            offs[b + 1] = offs[b] + t->count[b];
        for (int i = 0; i < n; i++)
            if (lengths[i])
                t->symbol[offs[lengths[i]]++] = i;
    }
    return 0;
}

/** Walk the canonical code one bit at a time, most significant first. */
static int decode_symbol(BitReader *br, const HuffTable *t)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= t->maxbits; len++) {
        code |= br_read(br, 1);
        if (code - first < t->count[len])
            return t->symbol[index + code - first];
        index += t->count[len];
        first  = (first + t->count[len]) << 1;
        code <<= 1;
        if (br->overrun)
            break;
    }
    return AVERROR_INVALIDDATA;
}

static void fixed_tables(HuffTable *lit, HuffTable *dist)
{
    uint8_t l[NB_LITERAL], d[NB_DIST];

    for (int i = 0; i < NB_LITERAL; i++)
        l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < NB_DIST; i++)
        d[i] = 5;

    build_table(lit, l, NB_LITERAL);
    build_table(dist, d, NB_DIST);
}

/** Read a dynamic block's two tables (state 1, 0x1ff9df4). */
static int dynamic_tables(BitReader *br, HuffTable *lit, HuffTable *dist)
{
    uint8_t clen[NB_CLEN] = { 0 }, lengths[NB_LITERAL + NB_DIST] = { 0 };
    HuffTable cl;
    int hlit, hdist, hclen, n = 0, ret;

    hlit  = br_read(br, 5) + 257;
    hdist = br_read(br, 5) + 1;
    hclen = br_read(br, 4) + 4;

    if (hlit > NB_LITERAL || hdist > NB_DIST)
        return AVERROR_INVALIDDATA;

    for (int i = 0; i < hclen; i++)
        clen[majesco_clen_order[i]] = br_read(br, 3);
    if ((ret = build_table(&cl, clen, NB_CLEN)) < 0)
        return ret;

    while (n < hlit + hdist) {
        int sym = decode_symbol(br, &cl), rep, val = 0;

        if (sym < 0)
            return sym;
        if (sym < 16) {
            lengths[n++] = sym;
            continue;
        }
        if (sym == 16) {
            if (!n)
                return AVERROR_INVALIDDATA;
            val = lengths[n - 1];
            rep = 3 + br_read(br, 2);
        } else if (sym == 17) {
            rep = 3 + br_read(br, 3);
        } else {
            rep = 11 + br_read(br, 7);
        }
        if (n + rep > hlit + hdist)
            return AVERROR_INVALIDDATA;
        while (rep--)
            lengths[n++] = val;
    }

    if ((ret = build_table(lit, lengths, hlit)) < 0)
        return ret;
    return build_table(dist, lengths + hlit, hdist);
}

uint32_t ff_majesco_get_output_size(const uint8_t *src, int src_size)
{
    if (src_size < 4)
        return 0;
    return AV_RL32(src);
}

int ff_majesco_inflate(const uint8_t *src, int src_size,
                       uint8_t *dst, uint32_t dst_size)
{
    BitReader br;
    HuffTable lit, dist;
    uint32_t pos = 0;

    if (src_size < 6)
        return AVERROR_INVALIDDATA;

    br_init(&br, src + 4, src_size - 4);

    while (pos < dst_size && !br.overrun) {
        int type = br_read(&br, 2);

        if (type == 0) {
            /* Stored: the copy loop at 0x1ff9ad4. */
            unsigned len = br_read(&br, 16);

            if (len > dst_size - pos)
                return AVERROR_INVALIDDATA;
            for (unsigned i = 0; i < len; i++)
                dst[pos++] = br_read(&br, 8);
            continue;
        } else if (type == 1) {
            fixed_tables(&lit, &dist);
        } else if (type == 2) {
            int ret = dynamic_tables(&br, &lit, &dist);

            if (ret < 0)
                return ret;
        } else {
            /* The fourth block type; no known stream uses it. */
            return AVERROR_PATCHWELCOME;
        }

        while (pos < dst_size) {
            int sym = decode_symbol(&br, &lit);
            unsigned len, off, dcode;

            if (sym < 0)
                return sym;
            if (sym < 256) {
                dst[pos++] = sym;
                continue;
            }
            sym -= 256;
            if (!sym)                   /* end of block */
                break;
            if (sym >= 30)
                return AVERROR_INVALIDDATA;

            len = majesco_dist_len_table[sym][2] +
                  br_read(&br, majesco_dist_len_table[sym][3]);

            dcode = decode_symbol(&br, &dist);
            if ((int)dcode < 0 || dcode >= 30)
                return AVERROR_INVALIDDATA;
            off = majesco_dist_len_table[dcode][0] +
                  br_read(&br, majesco_dist_len_table[dcode][1]);

            if (off > pos)
                return AVERROR_INVALIDDATA;
            len = FFMIN(len, dst_size - pos);
            for (unsigned i = 0; i < len; i++, pos++)
                dst[pos] = dst[pos - off];

            if (br.overrun)
                return AVERROR_INVALIDDATA;
        }
    }

    return pos == dst_size ? (int)pos : AVERROR_INVALIDDATA;
}
