/*
 * LZMA variant used by ADS-era (Majesco) GBA Video
 * Copyright (c) 2026 the FFmpeg developers
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
 * Ported from the IWRAM routine at 0x03001ca4 in Dragon Ball GT's main.bin.
 *
 * It is stock LZMA with lc = 0, lp = 0 and pb = 2 and no end marker; streams
 * carry an 8-byte [uint32 uncompressed_size][uint32 params] prefix in place of
 * the usual LZMA properties header. Because lc and lp are both zero there is
 * a single literal context, which keeps the probability model small.
 */

#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "defs.h"
#include "adslzma.h"

/* Probability model layout, transcribed from the ROM (byte offsets halved to
 * probability units). It matches the canonical LZMA layout exactly. */
#define P_IS_MATCH      (0x000 / 2)
#define P_IS_REP        (0x180 / 2)
#define P_IS_REP_G0     (0x198 / 2)
#define P_IS_REP_G1     (0x1b0 / 2)
#define P_IS_REP_G2     (0x1c8 / 2)
#define P_IS_REP0_LONG  (0x1e0 / 2)
#define P_POS_SLOT      (0x360 / 2)
#define P_SPEC_POS      (0x560 / 2)
#define P_ALIGN         (0x644 / 2)
#define P_LEN_CODER     (0x664 / 2)
#define P_REP_LEN_CODER (0xa68 / 2)
#define P_LITERAL       (0xe6c / 2)
#define NB_PROBS        (P_LITERAL + 0x300)

#define PB 2

typedef struct RangeCoder {
    const uint8_t *buf;
    int size;
    int pos;
    uint32_t range;
    uint32_t code;
} RangeCoder;

static uint8_t rc_byte(RangeCoder *rc)
{
    return rc->pos < rc->size ? rc->buf[rc->pos++] : (rc->pos++, 0);
}

static void rc_init(RangeCoder *rc, const uint8_t *buf, int size)
{
    rc->buf   = buf;
    rc->size  = size;
    rc->pos   = 1;                      /* the first byte is discarded */
    rc->code  = 0;
    rc->range = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        rc->code = (rc->code << 8) | rc_byte(rc);
}

static void rc_normalize(RangeCoder *rc)
{
    if (rc->range < (1u << 24)) {
        rc->range <<= 8;
        rc->code    = (rc->code << 8) | rc_byte(rc);
    }
}

static int rc_bit(RangeCoder *rc, uint16_t *probs, int idx)
{
    uint32_t bound = (rc->range >> 11) * probs[idx];
    int bit;

    if (rc->code < bound) {
        rc->range   = bound;
        probs[idx] += (2048 - probs[idx]) >> 5;
        bit = 0;
    } else {
        rc->range  -= bound;
        rc->code   -= bound;
        probs[idx] -= probs[idx] >> 5;
        bit = 1;
    }
    rc_normalize(rc);
    return bit;
}

static uint32_t rc_direct(RangeCoder *rc, int n)
{
    uint32_t res = 0;

    for (int i = 0; i < n; i++) {
        rc->range >>= 1;
        res <<= 1;
        if (rc->code >= rc->range) {
            rc->code -= rc->range;
            res |= 1;
        }
        rc_normalize(rc);
    }
    return res;
}

static int rc_tree(RangeCoder *rc, uint16_t *probs, int base, int nbits)
{
    int m = 1;

    for (int i = 0; i < nbits; i++)
        m = (m << 1) | rc_bit(rc, probs, base + m);
    return m - (1 << nbits);
}

static int rc_rtree(RangeCoder *rc, uint16_t *probs, int base, int nbits)
{
    int m = 1, res = 0;

    for (int i = 0; i < nbits; i++) {
        int bit = rc_bit(rc, probs, base + m);
        m    = (m << 1) | bit;
        res |= bit << i;
    }
    return res;
}

/** choice -> choice2 -> low(8) / mid(8) / high(256) */
static int decode_len(RangeCoder *rc, uint16_t *probs, int base, int pos_state)
{
    if (!rc_bit(rc, probs, base))
        return rc_tree(rc, probs, base + 2 + (pos_state << 3), 3);
    if (!rc_bit(rc, probs, base + 1))
        return 8 + rc_tree(rc, probs, base + 2 + 128 + (pos_state << 3), 3);
    return 16 + rc_tree(rc, probs, base + 2 + 256, 8);
}

int ff_ads_lzma_decode_raw(const uint8_t *src, int src_size,
                           uint8_t *dst, int dst_size)
{
    uint16_t *probs;
    RangeCoder rc;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    int state = 0, pos = 0;

    if (src_size < 5 || dst_size < 0)
        return AVERROR_INVALIDDATA;

    probs = av_malloc_array(NB_PROBS, sizeof(*probs));
    if (!probs)
        return AVERROR(ENOMEM);
    for (int i = 0; i < NB_PROBS; i++)
        probs[i] = 1024;

    rc_init(&rc, src, src_size);

    while (pos < dst_size) {
        int pos_state = pos & ((1 << PB) - 1);
        int len;

        if (!rc_bit(&rc, probs, P_IS_MATCH + (state << 4) + pos_state)) {
            /* lc == lp == 0, so there is exactly one literal context */
            int sym = 1;

            if (state < 7) {
                while (sym < 0x100)
                    sym = (sym << 1) | rc_bit(&rc, probs, P_LITERAL + sym);
            } else {
                unsigned match_byte = dst[pos - rep0 - 1];

                while (sym < 0x100) {
                    int match_bit = (match_byte >> 7) & 1;
                    int bit;

                    match_byte <<= 1;
                    bit = rc_bit(&rc, probs,
                                 P_LITERAL + 0x100 + (match_bit << 8) + sym);
                    sym = (sym << 1) | bit;
                    if (match_bit != bit) {
                        while (sym < 0x100)
                            sym = (sym << 1) | rc_bit(&rc, probs,
                                                      P_LITERAL + sym);
                        break;
                    }
                }
            }
            dst[pos++] = sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }

        if (rc_bit(&rc, probs, P_IS_REP + state)) {
            if (!rc_bit(&rc, probs, P_IS_REP_G0 + state)) {
                if (!rc_bit(&rc, probs,
                            P_IS_REP0_LONG + (state << 4) + pos_state)) {
                    if (pos < (int)rep0 + 1)
                        goto fail;
                    state = state < 7 ? 9 : 11;
                    dst[pos] = dst[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t dist;

                if (!rc_bit(&rc, probs, P_IS_REP_G1 + state)) {
                    dist = rep1;
                } else {
                    if (!rc_bit(&rc, probs, P_IS_REP_G2 + state)) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len   = decode_len(&rc, probs, P_REP_LEN_CODER, pos_state) + 2;
            state = state < 7 ? 8 : 11;
        } else {
            int len_state, pos_slot;

            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;

            len       = decode_len(&rc, probs, P_LEN_CODER, pos_state);
            state     = state < 7 ? 7 : 10;
            len_state = FFMIN(len, 3);
            pos_slot  = rc_tree(&rc, probs, P_POS_SLOT + (len_state << 6), 6);

            if (pos_slot < 4) {
                rep0 = pos_slot;
            } else {
                int nb_direct = (pos_slot >> 1) - 1;

                rep0 = (2 | (pos_slot & 1)) << nb_direct;
                if (pos_slot < 14) {
                    rep0 += rc_rtree(&rc, probs,
                                     P_SPEC_POS + rep0 - pos_slot - 1,
                                     nb_direct);
                } else {
                    rep0 += rc_direct(&rc, nb_direct - 4) << 4;
                    rep0 += rc_rtree(&rc, probs, P_ALIGN, 4);
                }
            }
            if (rep0 == 0xFFFFFFFFu)        /* end marker */
                break;
            len += 2;
        }

        if (pos < (int)rep0 + 1)
            goto fail;
        len = FFMIN(len, dst_size - pos);
        for (int i = 0; i < len; i++, pos++)
            dst[pos] = dst[pos - rep0 - 1];
    }

    av_free(probs);
    return FFMIN(rc.pos, src_size);

fail:
    av_free(probs);
    return AVERROR_INVALIDDATA;
}

int ff_ads_lzma_decode_blob(const uint8_t *src, int src_size,
                            uint8_t **dst, int *dst_size)
{
    uint32_t out_size;
    uint8_t *out;
    int used;

    if (src_size < 8)
        return AVERROR_INVALIDDATA;

    out_size = AV_RL32(src);
    /* Frames are small; refuse anything that cannot plausibly be one. */
    if (!out_size || out_size > (1 << 22))
        return AVERROR_INVALIDDATA;

    out = av_malloc(out_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!out)
        return AVERROR(ENOMEM);

    used = ff_ads_lzma_decode_raw(src + 8, src_size - 8, out, out_size);
    if (used < 0) {
        av_free(out);
        return used;
    }

    *dst      = out;
    *dst_size = out_size;
    return used + 8;
}
