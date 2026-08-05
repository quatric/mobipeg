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

/* See majesco.h for background. This is a direct port of the reverse
 * engineered pieces from Gericom's C# prototype - it does not invent
 * anything past what that prototype had working, so ff_majesco_inflate()
 * still bails out once the block-decode state machine runs off the end
 * of the known states (only "read block-type selector" and "read code
 * length alphabet + build its Huffman table" were ever reversed). */

#include <string.h>
#include "majesco.h"
#include "libavutil/attributes.h"

#define MAJESCO_LITERALS      (256 + 32)
#define MAJESCO_DISTANCES     32
#define MAJESCO_CODE_MAXBITS  15
#define MAJESCO_PRIMARY_BITS  8

/* Distance code base value / extra bits, and length code base / extra
 * bits, interleaved exactly as in the C# mDistanceAndBytesToCopy table.
 * Unused until the literal/length/distance decode loop is reversed and
 * implemented; kept here so that work has the table ready to go. */
static av_unused const uint16_t majesco_dist_len_table[30 * 4] = {
     0x0001,  0,     0,  0,
     0x0002,  0,     3,  0,
     0x0003,  0,     4,  0,
     0x0004,  0,     5,  0,
     0x0005,  1,     6,  0,
     0x0007,  1,     7,  0,
     0x0009,  2,     8,  0,
     0x000d,  2,     9,  0,
     0x0011,  3,    10,  0,
     0x0019,  3,    11,  1,
     0x0021,  4,    13,  1,
     0x0031,  4,    15,  1,
     0x0041,  5,    17,  1,
     0x0061,  5,    19,  2,
     0x0081,  6,    23,  2,
     0x00c1,  6,    27,  2,
     0x0101,  7,    31,  2,
     0x0181,  7,    35,  3,
     0x0201,  8,    43,  3,
     0x0301,  8,    51,  3,
     0x0401,  9,    59,  3,
     0x0601,  9,    67,  4,
     0x0801, 10,    83,  4,
     0x0c01, 10,    99,  4,
     0x1001, 11,   115,  4,
     0x1801, 11,   131,  5,
     0x2001, 12,   163,  5,
     0x3001, 12,   195,  5,
     0x4001, 13,   227,  5,
     0x6001, 13,   258,  0,
};

/* Order in which the code-length-alphabet symbol lengths are read;
 * unk_3002DA0 in the C# prototype (20 entries, despite v22 topping out
 * at 4 + 15 == 19 in practice - the trailing 0 is never reached). */
static const uint8_t majesco_clen_order[20] = {
    0x10, 0x11, 0x12, 0, 8, 7, 9, 6, 0xA, 5, 0xB, 4, 0xC, 3, 0xD, 2, 0xE, 1, 0xF, 0
};

typedef struct MajescoHuffNode {
    uint16_t value;
    uint8_t  length;
} MajescoHuffNode;

typedef struct MajescoHuffTable {
    MajescoHuffNode *root; /* caller-provided storage, big enough for the
                               primary table plus every secondary table */
    unsigned smallest_length;
    unsigned largest_length;
} MajescoHuffTable;

typedef struct MajescoBitReader {
    const uint8_t *data;
    int size;
    int offset;
    uint32_t bits;
    int nr_bits_left;
} MajescoBitReader;

static void majesco_fill_bits(MajescoBitReader *br)
{
    uint16_t word = 0;
    if (br->offset + 1 < br->size)
        word = br->data[br->offset] | (br->data[br->offset + 1] << 8);
    else if (br->offset < br->size)
        word = br->data[br->offset];
    br->bits |= (uint32_t)word << (16 - br->nr_bits_left);
    br->offset += 2;
    br->nr_bits_left += 16;
}

static uint32_t majesco_read_bits(MajescoBitReader *br, int nr_bits)
{
    uint32_t result;
    if (br->nr_bits_left < nr_bits)
        majesco_fill_bits(br);
    result = br->bits >> (32 - nr_bits);
    br->nr_bits_left -= nr_bits;
    br->bits <<= nr_bits;
    return result;
}

/* Faithful port of CreateDecodeTable(): builds a primary lookup table of
 * 1<<MAJESCO_PRIMARY_BITS entries, plus secondary tables appended after
 * it for any codes longer than MAJESCO_PRIMARY_BITS bits. Returns the
 * total number of MajescoHuffNode entries written into table->root, or 0
 * if the symbol set doesn't fit within MAJESCO_CODE_MAXBITS. */
static unsigned majesco_create_decode_table(MajescoHuffTable *table,
                                             unsigned symbols_count,
                                             const uint8_t *symbol_lengths)
{
    uint8_t length_occurrence[MAJESCO_CODE_MAXBITS + 1] = { 0 };
    MajescoHuffNode symbol_list[MAJESCO_LITERALS];
    MajescoHuffNode *symbol_length_pos[MAJESCO_CODE_MAXBITS + 1];
    MajescoHuffNode *last_pos = symbol_list;
    unsigned used_symbols_count;
    MajescoHuffNode *cur_table_node, *cur_symbol;
    const uint8_t *cur_len_occ;
    int code_decal;
    unsigned counter;

    for (counter = 0; counter < symbols_count; counter++)
        length_occurrence[symbol_lengths[counter]]++;

    table->smallest_length = MAJESCO_CODE_MAXBITS + 1;
    table->largest_length = 0;
    for (counter = 1; counter <= MAJESCO_CODE_MAXBITS; counter++) {
        symbol_length_pos[counter] = last_pos;
        last_pos += length_occurrence[counter];
        if (length_occurrence[counter]) {
            if (counter < table->smallest_length)
                table->smallest_length = counter;
            if (counter > table->largest_length)
                table->largest_length = counter;
        }
    }

    for (counter = 0; counter < symbols_count; counter++) {
        uint8_t len = symbol_lengths[counter];
        if (len) {
            MajescoHuffNode *node = symbol_length_pos[len]++;
            node->value = (uint16_t)counter;
            node->length = len;
        }
    }

    used_symbols_count =
        (unsigned)(symbol_length_pos[MAJESCO_CODE_MAXBITS] - symbol_list);

    code_decal = 1 << (MAJESCO_PRIMARY_BITS - table->smallest_length);
    cur_table_node = table->root;
    cur_symbol = symbol_list;
    cur_len_occ = length_occurrence + table->smallest_length;
    for (; code_decal != 0 &&
           cur_len_occ <= length_occurrence + table->largest_length;
         code_decal >>= 1) {
        unsigned symbols_left = *cur_len_occ++;
        for (; symbols_left > 0; symbols_left--, cur_symbol++) {
            int n;
            for (n = code_decal; n > 0; n--)
                *cur_table_node++ = *cur_symbol;
        }
    }

    if (table->largest_length <= MAJESCO_PRIMARY_BITS)
        return 1 << MAJESCO_PRIMARY_BITS;

    {
        MajescoHuffNode *next_table_node =
            table->root + (1 << MAJESCO_PRIMARY_BITS);
        unsigned remaining_bits = table->largest_length - MAJESCO_PRIMARY_BITS;
        unsigned initial_remaining_bits = remaining_bits;
        uint32_t current_symbol_code = (1u << table->largest_length) - 1;

        cur_len_occ = length_occurrence + table->largest_length;
        code_decal = 0;
        cur_symbol = symbol_list + used_symbols_count - 1;
        cur_table_node = next_table_node;

        for (; remaining_bits != 0;
             current_symbol_code >>= 1, remaining_bits--, code_decal++) {
            unsigned symbols_left = *cur_len_occ--;
            for (; symbols_left > 0; symbols_left--, cur_symbol--) {
                int n;
                uint32_t last_symbol_code;
                for (n = 1 << code_decal; n > 0; n--)
                    *next_table_node++ = *cur_symbol;
                last_symbol_code = current_symbol_code >> remaining_bits;
                current_symbol_code--;
                if (((current_symbol_code >> remaining_bits) ^
                     last_symbol_code) != 0) {
                    cur_table_node = next_table_node - 1;
                    cur_table_node->length =
                        (uint8_t)(32 - initial_remaining_bits);
                    cur_table_node->value =
                        (uint16_t)(next_table_node - cur_table_node - 1);
                    code_decal = symbols_left == 1 ? -1 : 0;
                    initial_remaining_bits =
                        symbols_left == 1 ? remaining_bits - 1 : remaining_bits;
                }
            }
        }
        return (unsigned)(next_table_node - table->root);
    }
}

uint32_t ff_majesco_get_output_size(const uint8_t *src, int src_size)
{
    if (src_size < 4)
        return 0;
    return src[0] | (src[1] << 8) | (src[2] << 16) | ((uint32_t)src[3] << 24);
}

/* Runs the block-decode state machine as far as it has actually been
 * reverse engineered (state 0: read the 2-bit block-type selector;
 * state 1: read the code-length alphabet and build its Huffman table),
 * then returns 0. Nothing beyond that point (the literal/length/distance
 * decode loop that would actually produce output bytes) has a known
 * implementation anywhere - Gericom's own Inflate() returned NULL. */
int ff_majesco_inflate(const uint8_t *src, int src_size,
                        uint8_t *dst, uint32_t dst_size)
{
    MajescoBitReader br = { 0 };
    uint32_t declared_size;
    int state;
    MajescoHuffNode clen_table_storage[1 << MAJESCO_PRIMARY_BITS];
    MajescoHuffTable clen_table;

    if (src_size < 4)
        return 0;
    declared_size = ff_majesco_get_output_size(src, src_size);
    if (declared_size != dst_size)
        return 0;

    br.data = src;
    br.size = src_size;
    br.offset = 4;

    switch (majesco_read_bits(&br, 2)) {
    case 0: state = 5; break;
    case 1: state = 2; break;
    case 2: state = 1; break;
    case 3: state = 7; break;
    default: state = -1; break;
    }

    if (state == 1) {
        uint8_t symbol_lengths[19] = { 0 };
        uint32_t v22;
        int i;

        majesco_read_bits(&br, 5); /* v20, unused past table selection */
        majesco_read_bits(&br, 5); /* v21, unused past table selection */
        v22 = majesco_read_bits(&br, 4) + 4;

        for (i = 0; i < (int)v22 && i < 19; i++)
            symbol_lengths[majesco_clen_order[i]] =
                (uint8_t)majesco_read_bits(&br, 3);

        clen_table.root = clen_table_storage;
        majesco_create_decode_table(&clen_table, 19, symbol_lengths);
    }

    /* Every state beyond this (actually walking literal/length/distance
     * codes to fill dst) is unreversed - do not fabricate it. */
    (void)dst;
    return 0;
}
