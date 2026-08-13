/*
 * Majesco embedded-device Huffman compressor (US Patent 7353233)
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
 * The exact inverse of ff_majesco_inflate() in majesco.c, which is the
 * compressor the Hydrogen-era GBA Video carts use in place of the LZMA that
 * the Dragon Ball GT lineage uses.
 *
 * Everything that makes this not just zlib lives in the bitstream layer, so
 * the deviations from stock DEFLATE are worth restating from the writer's
 * side:
 *
 *   - bits go out most significant first, gathered into 16-bit little-endian
 *     halfwords, where DEFLATE writes least significant first and reverses
 *     each Huffman code. Here the canonical code is written in natural order,
 *     because the reader consumes it MSB first (see bw_put);
 *   - a block header is a bare 2-bit type with no BFINAL flag. Nothing in the
 *     stream marks the end: the uint32 uncompressed size in the 4-byte prefix
 *     is what stops the decoder, so that count has to be exact;
 *   - a stored block is bit-packed - 2-bit type, 16-bit length, then that many
 *     8-bit reads straight out of the bit accumulator - with none of DEFLATE's
 *     byte alignment or NLEN check.
 *
 * The length and distance ladders, the code-length alphabet and its
 * permutation are stock DEFLATE, so those tables are shared verbatim with the
 * decoder.
 *
 * Three block encodings are built for every input and the cheapest wins:
 * stored, fixed Huffman and dynamic Huffman. Stored is what keeps the
 * incompressible case bounded, and it is also the encoding a round-trip test
 * can check without trusting the Huffman path.
 */

#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "majesco.h"

#define MAX_BITS      15
#define NB_FIXED_LIT  288       /* the fixed table's alphabet, as the ROM builds it */
#define NB_LITERAL    286       /* symbols we may actually emit: 0..285 */
#define NB_DIST       30
#define NB_CLEN       19
#define MAX_CLEN_BITS 7         /* a code length arrives in 3 bits, so 0..7 */

#define MIN_MATCH     3
#define MAX_MATCH     258
#define WINDOW        32768     /* the longest distance the ladder reaches */

#define HASH_BITS     15
#define HASH_SIZE     (1 << HASH_BITS)

/* How hard to look for a match. The streams this feeds are a few hundred KB,
 * so a deep chain costs little and the ROM-side decoder does not care. */
#define MAX_CHAIN     128

/* Length and distance ladders, indexed exactly as majesco.c indexes them:
 * { dist_base, dist_extra, len_base, len_extra }, by distance code and by
 * symbol - 0x100 alike. */
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

static const uint8_t majesco_clen_order[NB_CLEN] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ------------------------------------------------------------------------- */
/* Bit writer                                                                */
/* ------------------------------------------------------------------------- */

typedef struct BitWriter {
    uint8_t *buf;
    int      size;              /* capacity in bytes */
    int      pos;               /* bytes emitted */
    uint32_t acc;               /* pending bits, right-justified */
    int      nbits;
    int      overflow;
} BitWriter;

static void bw_init(BitWriter *bw, uint8_t *buf, int size)
{
    bw->buf      = buf;
    bw->size     = size;
    bw->pos      = 0;
    bw->acc      = 0;
    bw->nbits    = 0;
    bw->overflow = 0;
}

/**
 * Append the low @p n bits of @p v, most significant first.
 *
 * The reader tops its accumulator up sixteen bits at a time out of
 * little-endian halfwords and consumes from the top, so a completed halfword
 * goes out as soon as one is available. @p n is never more than 16, which
 * keeps the pending count under 32.
 */
static void bw_put(BitWriter *bw, unsigned v, int n)
{
    if (n <= 0)
        return;

    bw->acc    = (bw->acc << n) | (v & ((1u << n) - 1));
    bw->nbits += n;

    while (bw->nbits >= 16) {
        unsigned hw = (bw->acc >> (bw->nbits - 16)) & 0xffff;

        if (bw->pos + 2 <= bw->size) {
            AV_WL16(bw->buf + bw->pos, hw);
            bw->pos += 2;
        } else {
            bw->overflow = 1;
        }
        bw->nbits -= 16;
    }
    bw->acc &= (1u << bw->nbits) - 1;
}

/**
 * Round out to a halfword, then leave one spare.
 *
 * br_read() refills greedily - it pulls a whole halfword in to satisfy a
 * one-bit request - so a decoder finishing on a halfword boundary can flag an
 * overrun on bits it never actually uses. The slack halfword keeps that from
 * happening; the decoder stops on the uncompressed size long before reading
 * it.
 */
static void bw_flush(BitWriter *bw)
{
    if (bw->nbits)
        bw_put(bw, 0, 16 - bw->nbits);
    bw_put(bw, 0, 16);
}

/* ------------------------------------------------------------------------- */
/* Huffman                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct HuffEnc {
    uint16_t code[NB_FIXED_LIT];
    uint8_t  len[NB_FIXED_LIT];
} HuffEnc;

/**
 * Derive code lengths from symbol frequencies, none longer than @p maxbits.
 *
 * Plain Huffman by repeated merging of the two lightest nodes, which is
 * O(n^2) over an alphabet of at most 288 - not worth a heap. When the tree
 * comes out deeper than the format allows, the frequency spread is halved and
 * it is rebuilt: flattening the distribution shortens the longest code, and
 * once every weight has bottomed out at one the depth is ceil(log2(n)), well
 * inside both limits. That is a hair off optimal in the rare deep case and
 * much less code than package-merge.
 */
static void huff_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *lens)
{
    uint32_t work[NB_FIXED_LIT];
    uint32_t f[2 * NB_FIXED_LIT];
    int      parent[2 * NB_FIXED_LIT];
    uint8_t  active[2 * NB_FIXED_LIT];

    for (int i = 0; i < n; i++)
        work[i] = freq[i];

    for (int attempt = 0;; attempt++) {
        int nn = n, used = 0, maxlen = 0;

        memset(lens, 0, n);
        for (int i = 0; i < n; i++) {
            f[i]      = work[i];
            active[i] = work[i] > 0;
            parent[i] = -1;
            used     += active[i];
        }

        if (!used)                          /* an empty alphabet is legal */
            return;
        if (used == 1) {
            /* One symbol cannot fill a code, and the reader accepts an
             * under-subscribed one, so a single bit is enough. */
            for (int i = 0; i < n; i++)
                if (work[i])
                    lens[i] = 1;
            return;
        }

        for (;;) {
            int a = -1, b = -1;

            for (int i = 0; i < nn; i++) {
                if (!active[i])
                    continue;
                if (a < 0 || f[i] < f[a]) {
                    b = a;
                    a = i;
                } else if (b < 0 || f[i] < f[b]) {
                    b = i;
                }
            }
            if (b < 0)
                break;

            active[a] = active[b] = 0;
            f[nn]      = f[a] + f[b];
            parent[a]  = parent[b] = nn;
            parent[nn] = -1;
            active[nn] = 1;
            nn++;
        }

        for (int i = 0; i < n; i++) {
            int d = 0;

            if (!work[i])
                continue;
            for (int p = parent[i]; p >= 0; p = parent[p])
                d++;
            lens[i] = d;
            maxlen  = FFMAX(maxlen, d);
        }

        if (maxlen <= maxbits)
            return;

        for (int i = 0; i < n; i++)
            if (work[i])
                work[i] = attempt < 40 ? (work[i] + 1) >> 1 : 1;
    }
}

/**
 * Assign canonical codes over the same @p n symbols the reader will build its
 * table from, because the first code of each length depends on how many
 * shorter ones there are. Symbols of equal length take consecutive codes in
 * increasing symbol order, which is the order build_table() files them in.
 */
static void assign_codes(HuffEnc *h, int n)
{
    uint16_t count[MAX_BITS + 1] = { 0 };
    uint16_t next[MAX_BITS + 2]  = { 0 };

    for (int i = 0; i < n; i++)
        count[h->len[i]]++;
    count[0] = 0;

    for (int b = 1; b <= MAX_BITS; b++)
        next[b + 1] = (next[b] + count[b]) << 1;

    for (int i = 0; i < n; i++)
        if (h->len[i])
            h->code[i] = next[h->len[i]]++;
}

static void fixed_encoders(HuffEnc *lit, HuffEnc *dist)
{
    memset(lit, 0, sizeof(*lit));
    memset(dist, 0, sizeof(*dist));

    for (int i = 0; i < NB_FIXED_LIT; i++)
        lit->len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 32; i++)
        dist->len[i] = 5;

    assign_codes(lit, NB_FIXED_LIT);
    assign_codes(dist, 32);
}

/* ------------------------------------------------------------------------- */
/* LZ77                                                                      */
/* ------------------------------------------------------------------------- */

/** A literal is len == 0 with the byte in dist. */
typedef struct Token {
    uint16_t len;
    uint16_t dist;
} Token;

static av_always_inline unsigned hash3(const uint8_t *p)
{
    return (((unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2]) * 2654435761u)
           >> (32 - HASH_BITS);
}

/** Largest length code whose base fits @p len; 258 lands on code 29 exactly. */
static int len_symbol(int len)
{
    for (int i = 29; i >= 1; i--)
        if (len >= majesco_dist_len_table[i][2])
            return i;
    return 1;
}

static int dist_symbol(int dist)
{
    for (int i = 29; i >= 0; i--)
        if (dist >= majesco_dist_len_table[i][0])
            return i;
    return 0;
}

/**
 * Greedy match search with one step of lazy evaluation: a match is only taken
 * if the next position does not offer a longer one, which is the cheap half of
 * what zlib does and recovers most of the difference from plain greedy.
 */
static int lz77(const uint8_t *src, int src_size, Token *tok,
                uint32_t *lit_freq, uint32_t *dist_freq)
{
    int32_t *head = NULL, *prev = NULL;
    int ntok = 0, pos = 0;

    head = av_malloc_array(HASH_SIZE, sizeof(*head));
    prev = av_malloc_array(src_size > 0 ? src_size : 1, sizeof(*prev));
    if (!head || !prev) {
        av_freep(&head);
        av_freep(&prev);
        return AVERROR(ENOMEM);
    }
    memset(head, 0xff, HASH_SIZE * sizeof(*head));

    while (pos < src_size) {
        int best_len = 0, best_dist = 0;

        if (pos + MIN_MATCH <= src_size) {
            unsigned h = hash3(src + pos);
            int chain = MAX_CHAIN;

            for (int cand = head[h]; cand >= 0 && chain--; cand = prev[cand]) {
                int dist = pos - cand, len = 0;
                int limit = FFMIN(MAX_MATCH, src_size - pos);

                if (dist <= 0 || dist > WINDOW)
                    break;
                while (len < limit && src[cand + len] == src[pos + len])
                    len++;
                if (len > best_len) {
                    best_len  = len;
                    best_dist = dist;
                    if (len >= MAX_MATCH)
                        break;
                }
            }
            prev[pos] = head[h];
            head[h]   = pos;

            /* Lazy step: keep the literal if pos + 1 beats us. */
            if (best_len >= MIN_MATCH && best_len < MAX_MATCH &&
                pos + 1 + MIN_MATCH <= src_size) {
                unsigned h1 = hash3(src + pos + 1);
                int chain1 = MAX_CHAIN, next_best = 0;

                for (int cand = head[h1]; cand >= 0 && chain1--;
                     cand = prev[cand]) {
                    int dist = pos + 1 - cand, len = 0;
                    int limit = FFMIN(MAX_MATCH, src_size - pos - 1);

                    if (dist <= 0 || dist > WINDOW)
                        break;
                    while (len < limit && src[cand + len] == src[pos + 1 + len])
                        len++;
                    next_best = FFMAX(next_best, len);
                    if (len >= MAX_MATCH)
                        break;
                }
                if (next_best > best_len)
                    best_len = 0;
            }
        }

        if (best_len >= MIN_MATCH) {
            tok[ntok].len  = best_len;
            tok[ntok].dist = best_dist;
            lit_freq[256 + len_symbol(best_len)]++;
            dist_freq[dist_symbol(best_dist)]++;
            ntok++;

            /* Register the interior of the match so later positions can see
             * into it, then step over it. */
            for (int i = 1; i < best_len; i++) {
                int p = pos + i;

                if (p + MIN_MATCH <= src_size) {
                    unsigned hh = hash3(src + p);

                    prev[p]  = head[hh];
                    head[hh] = p;
                }
            }
            pos += best_len;
        } else {
            tok[ntok].len  = 0;
            tok[ntok].dist = src[pos];
            lit_freq[src[pos]]++;
            ntok++;
            pos++;
        }
    }

    av_freep(&head);
    av_freep(&prev);
    return ntok;
}

/* ------------------------------------------------------------------------- */
/* Dynamic block header                                                      */
/* ------------------------------------------------------------------------- */

typedef struct ClenTok {
    uint8_t sym;
    uint8_t extra_bits;
    uint8_t extra_val;
} ClenTok;

/** Run-length code a concatenated length vector into the 0..18 alphabet. */
static int rle_lengths(const uint8_t *lens, int n, ClenTok *tok,
                       uint32_t *freq)
{
    int ntok = 0, i = 0;

    while (i < n) {
        int val = lens[i], run = 1;

        while (i + run < n && lens[i + run] == val)
            run++;
        i += run;

        if (!val) {
            while (run >= 3) {
                int r = FFMIN(run, 138);

                if (r <= 10) {
                    tok[ntok++] = (ClenTok){ 17, 3, r - 3 };
                    freq[17]++;
                } else {
                    tok[ntok++] = (ClenTok){ 18, 7, r - 11 };
                    freq[18]++;
                }
                run -= r;
            }
        } else {
            tok[ntok++] = (ClenTok){ val, 0, 0 };
            freq[val]++;
            run--;

            while (run >= 3) {
                int r = FFMIN(run, 6);

                tok[ntok++] = (ClenTok){ 16, 2, r - 3 };
                freq[16]++;
                run -= r;
            }
        }
        while (run-- > 0) {
            tok[ntok++] = (ClenTok){ val, 0, 0 };
            freq[val]++;
        }
    }
    return ntok;
}

/* ------------------------------------------------------------------------- */
/* Block emitters                                                            */
/* ------------------------------------------------------------------------- */

static void emit_stored(BitWriter *bw, const uint8_t *src, int src_size)
{
    int pos = 0;

    /* A stored run's length is sixteen bits, and the decoder needs at least
     * one block even for empty input to make progress. */
    do {
        int len = FFMIN(src_size - pos, 0xffff);

        bw_put(bw, 0, 2);
        bw_put(bw, len, 16);
        for (int i = 0; i < len; i++)
            bw_put(bw, src[pos + i], 8);
        pos += len;
    } while (pos < src_size);
}

static void emit_tokens(BitWriter *bw, const Token *tok, int ntok,
                        const HuffEnc *lit, const HuffEnc *dist)
{
    for (int i = 0; i < ntok; i++) {
        if (!tok[i].len) {
            unsigned c = tok[i].dist;

            bw_put(bw, lit->code[c], lit->len[c]);
            continue;
        }
        {
            int ls = len_symbol(tok[i].len);
            int ds = dist_symbol(tok[i].dist);

            bw_put(bw, lit->code[256 + ls], lit->len[256 + ls]);
            bw_put(bw, tok[i].len - majesco_dist_len_table[ls][2],
                   majesco_dist_len_table[ls][3]);

            bw_put(bw, dist->code[ds], dist->len[ds]);
            bw_put(bw, tok[i].dist - majesco_dist_len_table[ds][0],
                   majesco_dist_len_table[ds][1]);
        }
    }
    bw_put(bw, lit->code[256], lit->len[256]);          /* end of block */
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

int ff_majesco_deflate(const uint8_t *src, int src_size,
                       uint8_t **dstp, int *dst_sizep)
{
    uint32_t lit_freq[NB_FIXED_LIT] = { 0 };
    uint32_t dist_freq[NB_DIST]     = { 0 };
    uint32_t clen_freq[NB_CLEN]     = { 0 };
    uint8_t  lengths[NB_LITERAL + NB_DIST];
    HuffEnc  fixed_lit, fixed_dist, dyn_lit, dyn_dist, cl;
    ClenTok  clen_tok[NB_LITERAL + NB_DIST];
    Token   *tok = NULL;
    BitWriter bw;
    uint8_t *out = NULL;
    int64_t  cap;
    int ntok, nclen, hlit, hdist, hclen;
    int64_t  stored_bits, fixed_bits, dyn_bits, hdr_bits;
    int ret;

    if (src_size < 0 || src_size > (1 << 26))
        return AVERROR_INVALIDDATA;

    tok = av_malloc_array(src_size + 1, sizeof(*tok));
    if (!tok)
        return AVERROR(ENOMEM);

    lit_freq[256] = 1;                          /* end of block, always sent */

    ret = lz77(src, src_size, tok, lit_freq, dist_freq);
    if (ret < 0)
        goto fail;
    ntok = ret;

    /* --- costs ---------------------------------------------------------- */

    /* Stored: a 2-bit type and a 16-bit run length per 64 KB, plus the bytes. */
    stored_bits = 8LL * src_size +
                  18LL * (src_size / 0xffff + 1);

    fixed_encoders(&fixed_lit, &fixed_dist);

    fixed_bits = 2;
    for (int i = 0; i < NB_FIXED_LIT; i++)
        fixed_bits += (int64_t)lit_freq[i] * fixed_lit.len[i];
    for (int i = 0; i < NB_DIST; i++)
        fixed_bits += (int64_t)dist_freq[i] * (fixed_dist.len[i] +
                      majesco_dist_len_table[i][1]);
    for (int i = 1; i < 30; i++)
        fixed_bits += (int64_t)lit_freq[256 + i] * majesco_dist_len_table[i][3];

    /* Dynamic: derive both tables, then price the header that carries them. */
    memset(&dyn_lit, 0, sizeof(dyn_lit));
    memset(&dyn_dist, 0, sizeof(dyn_dist));
    huff_lengths(lit_freq, NB_LITERAL, MAX_BITS, dyn_lit.len);
    huff_lengths(dist_freq, NB_DIST, MAX_BITS, dyn_dist.len);

    hlit = NB_LITERAL;
    while (hlit > 257 && !dyn_lit.len[hlit - 1])
        hlit--;
    hdist = NB_DIST;
    while (hdist > 1 && !dyn_dist.len[hdist - 1])
        hdist--;

    assign_codes(&dyn_lit, hlit);
    assign_codes(&dyn_dist, hdist);

    memcpy(lengths, dyn_lit.len, hlit);
    memcpy(lengths + hlit, dyn_dist.len, hdist);
    nclen = rle_lengths(lengths, hlit + hdist, clen_tok, clen_freq);

    memset(&cl, 0, sizeof(cl));
    huff_lengths(clen_freq, NB_CLEN, MAX_CLEN_BITS, cl.len);
    assign_codes(&cl, NB_CLEN);

    hclen = NB_CLEN;
    while (hclen > 4 && !cl.len[majesco_clen_order[hclen - 1]])
        hclen--;

    hdr_bits = 2 + 5 + 5 + 4 + 3LL * hclen;
    for (int i = 0; i < nclen; i++)
        hdr_bits += cl.len[clen_tok[i].sym] + clen_tok[i].extra_bits;

    dyn_bits = hdr_bits;
    for (int i = 0; i < hlit; i++)
        dyn_bits += (int64_t)lit_freq[i] * dyn_lit.len[i];
    for (int i = 0; i < hdist; i++)
        dyn_bits += (int64_t)dist_freq[i] * (dyn_dist.len[i] +
                    majesco_dist_len_table[i][1]);
    for (int i = 1; i < 30; i++)
        dyn_bits += (int64_t)lit_freq[256 + i] * majesco_dist_len_table[i][3];

    /* --- emit the cheapest --------------------------------------------- */

    cap = 4 + (FFMIN3(stored_bits, fixed_bits, dyn_bits) + 7) / 8 + 64;
    out = av_malloc(cap);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    AV_WL32(out, src_size);
    bw_init(&bw, out + 4, cap - 4);

    if (stored_bits <= fixed_bits && stored_bits <= dyn_bits) {
        emit_stored(&bw, src, src_size);
    } else if (fixed_bits <= dyn_bits) {
        bw_put(&bw, 1, 2);
        emit_tokens(&bw, tok, ntok, &fixed_lit, &fixed_dist);
    } else {
        bw_put(&bw, 2, 2);
        bw_put(&bw, hlit - 257, 5);
        bw_put(&bw, hdist - 1, 5);
        bw_put(&bw, hclen - 4, 4);
        for (int i = 0; i < hclen; i++)
            bw_put(&bw, cl.len[majesco_clen_order[i]], 3);
        for (int i = 0; i < nclen; i++) {
            bw_put(&bw, cl.code[clen_tok[i].sym], cl.len[clen_tok[i].sym]);
            bw_put(&bw, clen_tok[i].extra_val, clen_tok[i].extra_bits);
        }
        emit_tokens(&bw, tok, ntok, &dyn_lit, &dyn_dist);
    }

    bw_flush(&bw);

    if (bw.overflow) {
        ret = AVERROR_BUG;
        goto fail;
    }

    av_freep(&tok);
    *dstp      = out;
    *dst_sizep = 4 + bw.pos;
    return *dst_sizep;

fail:
    av_freep(&tok);
    av_freep(&out);
    return ret;
}
