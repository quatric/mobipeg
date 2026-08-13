/*
 * LZMA encoder for ADS-era (Majesco) GBA Video
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
 * The exact inverse of ff_ads_lzma_decode_raw() in adslzma.c: stock LZMA with
 * lc = 0, lp = 0, pb = 2 and no end marker, which is what Dragon Ball GT's
 * IWRAM decompressor at 0x03001ca4 actually runs.
 *
 * Every probability-coded decision here mirrors the decoder's rc_bit() one
 * for one - same bound formula, same shift-5 adaptation - so the two range
 * coders track an identical probability model as the stream progresses. That
 * is what makes an encoder for an adaptive coder possible at all: the decoder
 * never has to be told the model, only how to reach the same predictions
 * independently, and it does that by making the same update after every bit
 * the encoder also made.
 *
 * The parser (choosing between a literal, a fresh-distance match, a
 * repeated-distance match and repeating a match's distance for exactly one
 * byte, "short rep") is a plain greedy hash-chain search, not the multi-pass
 * optimal parse a general-purpose LZMA encoder would run. That costs some
 * ratio; it does not cost correctness, since decode_raw() places no
 * requirement on how a stream was chosen, only on its syntax.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "adslzma.h"

/* Probability model layout: identical to adslzma.c, and it has to stay that
 * way - the decoder's addressing is transcribed from the ROM, and this table
 * is what keeps both sides pointed at the same probabilities. */
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

#define MIN_MATCH  2
#define MAX_MATCH  273               /* 2 + the len coder's 16 + 255 */

#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)
#define MAX_CHAIN  96                /* how hard the hash chain is searched */

/* ------------------------------------------------------------------------- */
/* Range encoder                                                            */
/* ------------------------------------------------------------------------- */

typedef struct RangeEncoder {
    uint64_t low;                   /* holds one carry bit above the 32-bit range */
    uint32_t range;
    uint8_t  cache;
    int64_t  cache_size;
    uint8_t *buf;
    int      size;
    int      pos;
    int      overflow;
} RangeEncoder;

static void rc_put_byte(RangeEncoder *rc, uint8_t b)
{
    if (rc->pos < rc->size)
        rc->buf[rc->pos] = b;
    else
        rc->overflow = 1;
    rc->pos++;
}

static void rc_init_enc(RangeEncoder *rc, uint8_t *buf, int size)
{
    rc->low        = 0;
    rc->range      = 0xFFFFFFFFu;
    rc->cache      = 0;
    rc->cache_size = 1;
    rc->buf        = buf;
    rc->size       = size;
    rc->pos        = 0;
    rc->overflow   = 0;
}

/**
 * Flush the top byte of @p low, carrying into whatever 0xFF run is pending.
 *
 * The decoder discards the stream's first byte outright (rc_init: pos = 1),
 * which is exactly the byte this emits before any bit has been coded - the
 * standard LZMA encoder's cache starts at zero and cache_size at one, so the
 * very first shift always writes a bare zero with nothing to carry into yet.
 */
static void rc_shift_low(RangeEncoder *rc)
{
    if ((uint32_t)(rc->low >> 32) != 0 || rc->low < 0xFF000000ULL) {
        uint8_t temp = rc->cache;

        do {
            rc_put_byte(rc, (uint8_t)(temp + (uint8_t)(rc->low >> 32)));
            temp = 0xFF;
        } while (--rc->cache_size != 0);
        rc->cache = (uint8_t)((uint32_t)rc->low >> 24);
    }
    rc->cache_size++;
    rc->low = (uint64_t)(uint32_t)(rc->low << 8);
}

static av_always_inline void rc_normalize(RangeEncoder *rc)
{
    while (rc->range < (1u << 24)) {
        rc->range <<= 8;
        rc_shift_low(rc);
    }
}

/** The exact inverse of adslzma.c's rc_bit(): same bound, same adaptation. */
static av_always_inline void rc_encode_bit(RangeEncoder *rc, uint16_t *probs,
                                           int idx, int bit)
{
    uint32_t bound = (rc->range >> 11) * probs[idx];

    if (!bit) {
        rc->range   = bound;
        probs[idx] += (2048 - probs[idx]) >> 5;
    } else {
        rc->low    += bound;
        rc->range  -= bound;
        probs[idx] -= probs[idx] >> 5;
    }
    rc_normalize(rc);
}

/** The inverse of rc_direct(): uniform bits, most significant first. */
static void rc_encode_direct(RangeEncoder *rc, uint32_t v, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--) {
        rc->range >>= 1;
        if ((v >> i) & 1)
            rc->low += rc->range;
        rc_normalize(rc);
    }
}

/** The inverse of rc_tree(): symbol bits chosen most significant first. */
static void rc_encode_tree(RangeEncoder *rc, uint16_t *probs, int base,
                           int nbits, unsigned sym)
{
    int m = 1;

    for (int i = nbits - 1; i >= 0; i--) {
        int bit = (sym >> i) & 1;

        rc_encode_bit(rc, probs, base + m, bit);
        m = (m << 1) | bit;
    }
}

/** The inverse of rc_rtree(): same walk order, bit i of sym at step i. */
static void rc_encode_rtree(RangeEncoder *rc, uint16_t *probs, int base,
                            int nbits, unsigned sym)
{
    int m = 1;

    for (int i = 0; i < nbits; i++) {
        int bit = (sym >> i) & 1;

        rc_encode_bit(rc, probs, base + m, bit);
        m = (m << 1) | bit;
    }
}

/** Push the last coded bits out. Five bytes clears cache_size and low both. */
static void rc_flush(RangeEncoder *rc)
{
    for (int i = 0; i < 5; i++)
        rc_shift_low(rc);
}

/* ------------------------------------------------------------------------- */
/* Length coder: choice -> choice2 -> low(8) / mid(8) / high(256)           */
/* ------------------------------------------------------------------------- */

/** @p raw is the length coder's own 0..271 range; add 2 for the real length. */
static void encode_len(RangeEncoder *rc, uint16_t *probs, int base,
                       int pos_state, int raw)
{
    if (raw < 8) {
        rc_encode_bit(rc, probs, base, 0);
        rc_encode_tree(rc, probs, base + 2 + (pos_state << 3), 3, raw);
    } else if (raw < 16) {
        rc_encode_bit(rc, probs, base, 1);
        rc_encode_bit(rc, probs, base + 1, 0);
        rc_encode_tree(rc, probs, base + 2 + 128 + (pos_state << 3), 3, raw - 8);
    } else {
        rc_encode_bit(rc, probs, base, 1);
        rc_encode_bit(rc, probs, base + 1, 1);
        rc_encode_tree(rc, probs, base + 2 + 256, 8, raw - 16);
    }
}

/* ------------------------------------------------------------------------- */
/* Literals                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * The inverse of the decoder's literal path. With lc = lp = 0 there is one
 * plain context (state < 7) and one matched-literal context that XORs against
 * the byte the current rep0 distance predicts (state >= 7) - the same
 * "diverge and fall back to plain bits" walk the decoder does, just choosing
 * bits instead of reading them.
 */
static void encode_literal(RangeEncoder *rc, uint16_t *probs, int state,
                           const uint8_t *dst, int pos, uint32_t rep0,
                           uint8_t sym)
{
    if (state < 7) {
        int m = 1;

        for (int i = 7; i >= 0; i--) {
            int bit = (sym >> i) & 1;

            rc_encode_bit(rc, probs, P_LITERAL + m, bit);
            m = (m << 1) | bit;
        }
    } else {
        unsigned match_byte = dst[pos - rep0 - 1];
        int m = 1, i;

        for (i = 7; i >= 0; i--) {
            int match_bit = (match_byte >> 7) & 1;
            int bit        = (sym >> i) & 1;

            match_byte <<= 1;
            rc_encode_bit(rc, probs, P_LITERAL + 0x100 + (match_bit << 8) + m,
                         bit);
            m = (m << 1) | bit;
            if (match_bit != bit) {
                for (i--; i >= 0; i--) {
                    bit = (sym >> i) & 1;
                    rc_encode_bit(rc, probs, P_LITERAL + m, bit);
                    m = (m << 1) | bit;
                }
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Distance (pos_slot / spec_pos / align)                                   */
/* ------------------------------------------------------------------------- */

/**
 * The inverse of the decoder's pos_slot walk.
 *
 * A slot's top two bits are dist's highest set bit and the one below it;
 * everything under that is either a handful of reverse-coded (adaptive) bits
 * for small slots or a run of uniform direct bits plus four reverse-coded
 * "align" bits for large ones. This recovers that same two-bit prefix from
 * @p dist and reproduces the exact addressing the decoder computes for
 * P_SPEC_POS, offset by the slot itself - a quirk of the layout, not a bug:
 * the decoder does the same, so both sides land on the same probabilities.
 */
static void encode_distance(RangeEncoder *rc, uint16_t *probs, int len_state,
                            uint32_t dist)
{
    unsigned pos_slot;

    if (dist < 4) {
        pos_slot = dist;
    } else {
        int nb = av_log2(dist);

        pos_slot = ((unsigned)nb << 1) | ((dist >> (nb - 1)) & 1);
    }

    rc_encode_tree(rc, probs, P_POS_SLOT + (len_state << 6), 6, pos_slot);

    if (pos_slot >= 4) {
        int nb_direct   = (int)(pos_slot >> 1) - 1;
        uint32_t base   = (uint32_t)(2 | (pos_slot & 1)) << nb_direct;
        uint32_t rem    = dist - base;

        if (pos_slot < 14) {
            rc_encode_rtree(rc, probs, P_SPEC_POS + base - pos_slot - 1,
                            nb_direct, rem);
        } else {
            rc_encode_direct(rc, rem >> 4, nb_direct - 4);
            rc_encode_rtree(rc, probs, P_ALIGN, 4, rem & 0xF);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Match finder                                                             */
/* ------------------------------------------------------------------------- */

static av_always_inline unsigned hash3(const uint8_t *p)
{
    return (((unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]) * 2654435761u)
           >> (32 - HASH_BITS);
}

static av_always_inline int match_len(const uint8_t *src, int size,
                                      int pos, int cand, int limit)
{
    int len = 0;

    while (len < limit && src[cand + len] == src[pos + len])
        len++;
    return len;
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                             */
/* ------------------------------------------------------------------------- */

int ff_ads_lzma_encode_raw(const uint8_t *src, int src_size,
                           uint8_t *dst, int dst_size)
{
    RangeEncoder rc;
    uint16_t *probs;
    int32_t *head = NULL, *prev = NULL;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    int state = 0, pos = 0, ret = 0;

    if (src_size < 0)
        return AVERROR_INVALIDDATA;

    probs = av_malloc_array(NB_PROBS, sizeof(*probs));
    if (!probs)
        return AVERROR(ENOMEM);
    for (int i = 0; i < NB_PROBS; i++)
        probs[i] = 1024;

    if (src_size > 0) {
        head = av_malloc_array(HASH_SIZE, sizeof(*head));
        prev = av_malloc_array(src_size, sizeof(*prev));
        if (!head || !prev) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        memset(head, 0xff, HASH_SIZE * sizeof(*head));
    }

    rc_init_enc(&rc, dst, dst_size);

    while (pos < src_size) {
        int pos_state = pos & ((1 << PB) - 1);
        int limit = FFMIN(MAX_MATCH, src_size - pos);

        int hash_len = 0, hash_cand = 0;
        int rep_len = 0, rep_idx = -1;
        uint32_t reps[4] = { rep0, rep1, rep2, rep3 };

        /* Repeated distances first: much cheaper to code than a fresh one,
         * so they are worth taking even when a hash match is a little
         * longer. */
        for (int i = 0; i < 4; i++) {
            int cand = pos - (int)reps[i] - 1;
            int len;

            if (cand < 0 || (i > 0 && reps[i] == reps[i - 1]))
                continue;
            len = match_len(src, src_size, pos, cand, limit);
            if (len > rep_len) {
                rep_len = len;
                rep_idx = i;
            }
        }

        /* Hash-chain search for a fresh distance. */
        if (pos + 3 <= src_size) {
            unsigned h = hash3(src + pos);
            int chain = MAX_CHAIN;

            for (int cand = head[h]; cand >= 0 && chain--; cand = prev[cand]) {
                int len = match_len(src, src_size, pos, cand, limit);

                if (len > hash_len) {
                    hash_len  = len;
                    hash_cand = cand;
                    if (len >= limit)
                        break;
                }
            }
            prev[pos] = head[h];
            head[h]   = pos;
        }

        if (rep_len >= 2 && rep_len + 1 >= hash_len) {
            /* Emit a repeated-distance match. */
            int raw = rep_len - 2;

            rc_encode_bit(&rc, probs, P_IS_MATCH + (state << 4) + pos_state, 1);
            rc_encode_bit(&rc, probs, P_IS_REP + state, 1);

            if (rep_idx == 0) {
                rc_encode_bit(&rc, probs, P_IS_REP_G0 + state, 0);
                rc_encode_bit(&rc, probs,
                             P_IS_REP0_LONG + (state << 4) + pos_state, 1);
            } else {
                rc_encode_bit(&rc, probs, P_IS_REP_G0 + state, 1);
                if (rep_idx == 1) {
                    rc_encode_bit(&rc, probs, P_IS_REP_G1 + state, 0);
                } else {
                    rc_encode_bit(&rc, probs, P_IS_REP_G1 + state, 1);
                    rc_encode_bit(&rc, probs, P_IS_REP_G2 + state,
                                 rep_idx == 3);
                }
            }
            encode_len(&rc, probs, P_REP_LEN_CODER, pos_state, raw);

            if (rep_idx == 3) rep3 = rep2;
            if (rep_idx >= 2) rep2 = rep1;
            if (rep_idx >= 1) rep1 = rep0;
            rep0 = reps[rep_idx];

            state = state < 7 ? 8 : 11;
            pos  += rep_len;
        } else if (hash_len >= 3 || (hash_len == 2 && pos - hash_cand <= 128)) {
            /* Emit a fresh-distance match. */
            uint32_t dist = (uint32_t)(pos - hash_cand - 1);
            int raw = hash_len - 2;
            int len_state = FFMIN(raw, 3);

            rc_encode_bit(&rc, probs, P_IS_MATCH + (state << 4) + pos_state, 1);
            rc_encode_bit(&rc, probs, P_IS_REP + state, 0);

            encode_len(&rc, probs, P_LEN_CODER, pos_state, raw);
            encode_distance(&rc, probs, len_state, dist);

            rep3 = rep2; rep2 = rep1; rep1 = rep0; rep0 = dist;
            state = state < 7 ? 7 : 10;
            pos  += hash_len;
        } else if (pos - (int)rep0 - 1 >= 0 &&
                  src[pos] == src[pos - (int)rep0 - 1]) {
            /* Short rep: one byte at the existing rep0 distance. */
            rc_encode_bit(&rc, probs, P_IS_MATCH + (state << 4) + pos_state, 1);
            rc_encode_bit(&rc, probs, P_IS_REP + state, 1);
            rc_encode_bit(&rc, probs, P_IS_REP_G0 + state, 0);
            rc_encode_bit(&rc, probs,
                         P_IS_REP0_LONG + (state << 4) + pos_state, 0);

            state = state < 7 ? 9 : 11;
            pos  += 1;
        } else {
            /* Literal. */
            uint8_t sym = src[pos];

            rc_encode_bit(&rc, probs, P_IS_MATCH + (state << 4) + pos_state, 0);
            encode_literal(&rc, probs, state, src, pos, rep0, sym);

            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            pos  += 1;
        }
    }

    rc_flush(&rc);

    if (rc.overflow) {
        ret = AVERROR_BUG;
        goto out;
    }
    ret = rc.pos;

out:
    av_freep(&probs);
    av_freep(&head);
    av_freep(&prev);
    return ret;
}

int ff_ads_lzma_encode_blob(const uint8_t *src, int src_size,
                            uint8_t **dstp, int *dst_sizep)
{
    /* Worst case for adaptive coding of near-random data still lands close
     * to one byte in, one byte out; the extra half gives slack for the
     * warm-up cost before probabilities settle. */
    int cap = 8 + src_size + src_size / 2 + 4096;
    uint8_t *buf;
    int ret;

    buf = av_malloc(cap);
    if (!buf)
        return AVERROR(ENOMEM);

    AV_WL32(buf, (uint32_t)src_size);
    /* Dictionary-size selector word, matching the pattern the demuxer's
     * LZMA/Inflate sniff looks for (see gbavideo.c parse_video_resource());
     * decode_raw() itself never reads this. */
    AV_WL32(buf + 4, 0x00010002);

    ret = ff_ads_lzma_encode_raw(src, src_size, buf + 8, cap - 8);
    if (ret < 0) {
        av_freep(&buf);
        return ret;
    }

    *dstp      = buf;
    *dst_sizep = 8 + ret;
    return *dst_sizep;
}
