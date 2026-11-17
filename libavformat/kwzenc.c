/*
 * Flipnote Studio 3D (KWZ) muxer
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 *
 * Writes Nintendo 3DS Flipnote Studio 3D `.kwz` animation files. Video input
 * is 320x240 rgb24 rawvideo; every pixel is mapped to one of the six Flipnote
 * colours (white, black, red, yellow, green, blue), optionally with ordered
 * dithering, and spread over the three layers of a frame. Audio input is
 * optional mono s16le at 16364 Hz, stored as the BGM track in the format's
 * variable 2/4-bit ADPCM.
 *
 * Layers are compressed the way Flipnote Studio 3D does it: every 8x8 tile
 * takes the cheapest of the tile types that can represent it, and runs of
 * tiles that did not change since the previous frame are skipped. Layers of
 * notes saved by the application re-encode to the same bytes with these
 * rules.
 *
 * A note ends in an RSA-2048 / PKCS#1 v1.5 / SHA-256 signature over the rest
 * of the file. The private key is not part of this file: pass it with
 * -kwz_key (PKCS#1 or PKCS#8, DER or PEM). Without a key the signature is
 * left zeroed, which other readers accept but the console does not.
 *
 * Format reference:
 *   https://github.com/Flipnote-Collective/flipnote-studio-3d-docs/wiki/KWZ-Format
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

#include <math.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/base64.h"
#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"
#include "libavcodec/jpegtables.h"
#include "avformat.h"
#include "flipnote_rsa.h"
#include "internal.h"
#include "mux.h"

#define KWZ_W           320
#define KWZ_H           240
#define KWZ_PIXELS      (KWZ_W * KWZ_H)
#define KWZ_TILES       1200
#define KWZ_LINES       6561
#define KWZ_MAX_FRAMES  999
#define KWZ_BGM_RATE    16364
#define KWZ_SIG_LEN     256
#define KWZ_META_SIZE   28
#define KWZ_KFH_SIZE    204
#define KWZ_LAYER_MAX   (KWZ_TILES * 15)   /* a tile takes at most 115 bits */
#define KWZ_THUMB_W     80
#define KWZ_THUMB_H     64
#define KWZ_THUMB_MAX   3840               /* thumbnails of real notes stay below this */

/* frame rates indexed by in-app frame speed */
static const float kwz_framerates[11] = {
    .2f, .5f, 1, 2, 4, 6, 8, 12, 20, 24, 30
};

/* white, black, red, yellow, green, blue */
static const uint8_t kwz_rgb[6][3] = {
    { 0xff, 0xff, 0xff }, { 0x10, 0x10, 0x10 }, { 0xff, 0x10, 0x10 },
    { 0xff, 0xe7, 0x00 }, { 0x00, 0x86, 0x31 }, { 0x00, 0x38, 0xce },
};

/* the same in linear light, 0..65535 */
static const uint16_t kwz_lin_rgb[6][3] = {
    { 65535, 65535, 65535 }, {   340,   340,   340 }, { 65535,   340,   340 },
    { 65535, 52369,     0 }, {     0, 15623,  2013 }, {     0,  2592, 40449 },
};

/* linear light of the 32 levels of a 5-bit sRGB channel */
static const uint16_t kwz_lin5[32] = {
        0,   159,   340,   599,   997,  1453,  2013,  2681,
     3570,  4488,  5530,  6700,  8177,  9635, 11235, 12980,
    15122, 17187, 19407, 21787, 24658, 27386, 30282, 33350,
    37008, 40449, 44069, 47871, 52369, 56567, 60955, 65535
};

#define KWZ_CHROMA_WEIGHT 3     /* weight of hue and saturation against lightness */
#define KWZ_MIX_FLOOR     2621  /* a colour this close to a pure one is not dithered (0.04) */
#define KWZ_MIX_PENALTY   400   /* a mix also costs this fraction of the distance of its pair */

/* Layer colours as the application's own sample notes store them:
 * A = white, black; B = blue, red; C = yellow, green. */
#define KWZ_LAYER_COLOURS 0x43251000u
static const uint8_t kwz_ink_layer[6] = { 0, 0, 1, 2, 2, 1 };
static const uint8_t kwz_ink_value[6] = { 1, 2, 2, 1, 2, 1 };

/* line table indices that have a 5-bit code */
static const uint16_t kwz_common_idx[32] = {
    0x0000, 0x0CD0, 0x19A0, 0x02D9, 0x088B, 0x0051, 0x00F3, 0x0009,
    0x001B, 0x0001, 0x0003, 0x05B2, 0x1116, 0x00A2, 0x01E6, 0x0012,
    0x0036, 0x0002, 0x0006, 0x0B64, 0x08DC, 0x0144, 0x00FC, 0x0024,
    0x001C, 0x0004, 0x0334, 0x099C, 0x0668, 0x1338, 0x1004, 0x166C
};

/* weight of each pixel of a line in its line table index */
static const uint16_t kwz_line_weight[8] = { 729, 2187, 81, 243, 9, 27, 1, 3 };

/* which of the two lines of a type 7 tile each row uses */
static const uint8_t kwz_patterns[4][8] = {
    { 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 0, 1, 0, 0, 1, 0, 0 },
    { 0, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 1, 1, 0, 1 },
};

static const int8_t kwz_index_2bit[4]  = { -1, 2, -1, 2 };
static const int8_t kwz_index_4bit[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
static const int16_t kwz_step_table[80] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899
};

/* SHA-256 of the private key Flipnote Studio 3D signs with, as published in
 * the format documentation */
static const uint8_t kwz_key_sha256[32] = {
    0xE6, 0x89, 0x2F, 0xF7, 0x94, 0xE8, 0xA7, 0x68, 0xC9, 0xEC, 0xC7, 0x61,
    0x52, 0xC4, 0xE7, 0x28, 0x23, 0x51, 0x43, 0x66, 0xB3, 0xA2, 0x06, 0x29,
    0x8F, 0x5C, 0xB6, 0x03, 0xD5, 0xEB, 0x79, 0x7A
};

#define KWZ_LIMBS 64

/* one colour, or a mix of two, that the dither matrix can produce */
typedef struct KwzCand {
    int      lab[3];
    int64_t  penalty;
    uint8_t  c1, c2, r;
} KwzCand;

typedef struct KwzMuxContext {
    const AVClass *class;
    char    *author;
    char    *fsid_str;
    char    *key_path;
    int      dither;
    int      paper;
    int      loop;

    int      speed;                 /* in-app frame speed, 0..10 */
    uint8_t  fsid[10];
    int      have_key;
    uint32_t rsa_n[KWZ_LIMBS];
    uint32_t rsa_d[KWZ_LIMBS];

    /* line tables */
    uint16_t line_shift[KWZ_LINES]; /* index of the line rotated left by one pixel */
    int8_t   line_common[KWZ_LINES];/* 5-bit code of the line, or -1 */
    uint32_t tile_off[KWZ_TILES];   /* pixel offset of each tile, in stream order */
    uint8_t  layer_value[3][6];     /* pixel value of each colour in each layer */

    /* colour mapping */
    KwzCand *cand;
    int      nb_cand;
    uint8_t (*plan)[4];             /* per RGB555 colour: valid, colour 1, colour 2, ratio */
    uint8_t  bayer[8][8];
    uint8_t *idx;                   /* colour index of every pixel of the frame */
    uint8_t *first;                 /* same for the first frame, for the thumbnail */

    uint16_t *lines[2][3];          /* line indices of the current and previous frame */
    int       cur;

    uint8_t *frames;                /* concatenated layer data */
    unsigned frames_size, frames_alloc;
    uint8_t *meta;                  /* frame meta entries */
    int      nb_frames;
    int      warned;

    uint8_t *pcm;                   /* BGM input, s16le */
    unsigned pcm_size, pcm_alloc;
    int      audio_index;
} KwzMuxContext;

/* ---------- RSA-2048 signature ---------- */

/* RSASSA-PKCS1-v1_5 / SHA-256 signature of data, 256 bytes into sig */
static int kwz_rsa_sign(const KwzMuxContext *c, const uint8_t *data, int64_t size,
                        uint8_t sig[KWZ_SIG_LEN])
{
    uint8_t n[KWZ_SIG_LEN], d[KWZ_SIG_LEN];

    for (int i = 0; i < KWZ_LIMBS; i++) {
        AV_WB32(n + 4 * i, c->rsa_n[KWZ_LIMBS - 1 - i]);
        AV_WB32(d + 4 * i, c->rsa_d[KWZ_LIMBS - 1 - i]);
    }
    return ff_flipnote_rsa_sign(n, d, KWZ_SIG_LEN, 256, data, size, sig);
}

/* Read one DER element: returns its tag, sets the value and moves *p past it. */
static int kwz_der_get(const uint8_t **p, const uint8_t *end,
                       const uint8_t **val, int *len)
{
    const uint8_t *q = *p;
    int tag, n;

    if (end - q < 2)
        return -1;
    tag = *q++;
    n   = *q++;
    if (n & 0x80) {
        int k = n & 0x7f;
        if (k < 1 || k > 3 || end - q < k)
            return -1;
        for (n = 0; k > 0; k--)
            n = n << 8 | *q++;
    }
    if (end - q < n)
        return -1;
    *val = q;
    *len = n;
    *p   = q + n;
    return tag;
}

static int kwz_der_integer(const uint8_t **p, const uint8_t *end, uint32_t *limbs)
{
    const uint8_t *val;
    int len, i;

    if (kwz_der_get(p, end, &val, &len) != 0x02)
        return -1;
    while (len > 0 && !*val) {
        val++;
        len--;
    }
    if (!limbs)
        return 0;
    if (len > 4 * KWZ_LIMBS)
        return -1;
    memset(limbs, 0, KWZ_LIMBS * sizeof(*limbs));
    for (i = 0; i < len; i++)
        limbs[i >> 2] |= (uint32_t)val[len - 1 - i] << (8 * (i & 3));
    return 0;
}

/* Parse an RSA private key: PKCS#1 RSAPrivateKey or PKCS#8 PrivateKeyInfo. */
static int kwz_parse_key(AVFormatContext *s, const uint8_t *der, int size)
{
    KwzMuxContext *c = s->priv_data;
    const uint8_t *p = der, *end = der + size, *seq, *val, *q;
    int len, tag;

    if (kwz_der_get(&p, end, &seq, &len) != 0x30)
        return AVERROR_INVALIDDATA;
    end = seq + len;
    q   = seq;
    if (kwz_der_integer(&q, end, NULL) < 0)          /* version */
        return AVERROR_INVALIDDATA;
    p   = q;
    tag = kwz_der_get(&q, end, &val, &len);
    if (tag == 0x30) {                               /* PKCS#8: algorithm, then the key */
        if (kwz_der_get(&q, end, &val, &len) != 0x04)
            return AVERROR_INVALIDDATA;
        return kwz_parse_key(s, val, len);
    }
    if (kwz_der_integer(&p, end, c->rsa_n) < 0 ||    /* modulus */
        kwz_der_integer(&p, end, NULL)     < 0 ||    /* public exponent */
        kwz_der_integer(&p, end, c->rsa_d) < 0)      /* private exponent */
        return AVERROR_INVALIDDATA;
    if (!(c->rsa_n[KWZ_LIMBS - 1] >> 31) || !(c->rsa_n[0] & 1)) {
        av_log(s, AV_LOG_ERROR, "The signing key must be a 2048-bit RSA key\n");
        return AVERROR(EINVAL);
    }

    {
        struct AVSHA *sha = av_sha_alloc();
        uint8_t digest[32];
        if (!sha)
            return AVERROR(ENOMEM);
        av_sha_init(sha, 256);
        av_sha_update(sha, der, end - der);
        av_sha_final(sha, digest);
        av_free(sha);
        if (memcmp(digest, kwz_key_sha256, 32))
            av_log(s, AV_LOG_WARNING, "The signing key is not the one Flipnote Studio 3D "
                   "uses; the console will not accept the signature\n");
    }
    c->have_key = 1;
    return 0;
}

static int kwz_load_key(AVFormatContext *s)
{
    KwzMuxContext *c = s->priv_data;
    AVIOContext *pb = NULL;
    uint8_t *buf, *der = NULL;
    int size = 0, ret, n;

    buf = av_malloc(16384 + 1);
    if (!buf)
        return AVERROR(ENOMEM);
    ret = s->io_open(s, &pb, c->key_path, AVIO_FLAG_READ, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Cannot open the signing key '%s'\n", c->key_path);
        av_free(buf);
        return ret;
    }
    while (size < 16384 && (n = avio_read(pb, buf + size, 16384 - size)) > 0)
        size += n;
    ff_format_io_close(s, &pb);
    buf[size] = 0;

    if (size > 10 && !memcmp(buf, "-----BEGIN", 10)) {
        /* PEM: base64 between the armour lines */
        char *b64 = av_malloc(size + 1), *o = b64;
        const char *q = strchr((char *)buf, '\n');
        if (!b64) {
            av_free(buf);
            return AVERROR(ENOMEM);
        }
        for (; q && *q && *q != '-'; q++) {
            if (!av_isspace(*q))
                *o++ = *q;
        }
        *o  = 0;
        der = av_malloc(size);
        if (!der) {
            av_free(b64);
            av_free(buf);
            return AVERROR(ENOMEM);
        }
        size = av_base64_decode(der, b64, size);
        av_free(b64);
        ret  = size < 0 ? AVERROR_INVALIDDATA : kwz_parse_key(s, der, size);
        av_free(der);
    } else {
        ret = kwz_parse_key(s, buf, size);
    }
    av_free(buf);
    if (ret == AVERROR_INVALIDDATA)
        av_log(s, AV_LOG_ERROR, "'%s' is not an RSA private key (PKCS#1 or PKCS#8, "
               "DER or PEM)\n", c->key_path);
    return ret;
}

/* ---------- colour mapping ---------- */

/* Oklab coordinates (scaled by 65536) of a linear-light colour, with the
 * chroma axes stretched: a wrong hue, above all a tint on a grey, is far more
 * objectionable than a wrong lightness. Integer arithmetic throughout, so the
 * mapping is the same on every platform. */
static uint32_t kwz_icbrt(uint64_t v)
{
    uint32_t r = 0;
    int bit;
    for (bit = 15; bit >= 0; bit--) {
        uint64_t t = r | 1u << bit;
        if (t * t * t <= v)
            r = t;
    }
    return r;
}

static void kwz_oklab(const int *lin, int *lab)
{
    int64_t l = kwz_icbrt((27015LL * lin[0] + 35149LL * lin[1] +  3372LL * lin[2]) << 16);
    int64_t m = kwz_icbrt((13887LL * lin[0] + 44610LL * lin[1] +  7039LL * lin[2]) << 16);
    int64_t s = kwz_icbrt(( 5787LL * lin[0] + 18463LL * lin[1] + 41286LL * lin[2]) << 16);

    lab[0] = ( 13792 * l +  52011 * m -   267 * s) >> 16;
    lab[1] = KWZ_CHROMA_WEIGHT * ((129630 * l - 159160 * m + 29530 * s) >> 16);
    lab[2] = KWZ_CHROMA_WEIGHT * ((  1698 * l +  51300 * m - 52998 * s) >> 16);
}

static int64_t kwz_lab_dist(const int *a, const int *b)
{
    int64_t d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}

/* List what the dither matrix can produce: the six colours and every mix of
 * two of them, averaged in linear light. A mix is charged a little, more so
 * when its colours are far apart, which keeps near-pure areas clean and
 * prefers the calmer of two mixes that match equally well. */
static int kwz_init_colours(KwzMuxContext *c)
{
    int levels = c->dither * c->dither, lab[6][3], i, j, k, r;
    KwzCand *cand;

    c->cand = cand = av_calloc(6 + 15 * (levels - 1), sizeof(*cand));
    if (!cand)
        return AVERROR(ENOMEM);
    for (i = 0; i < 6; i++) {
        int lin[3] = { kwz_lin_rgb[i][0], kwz_lin_rgb[i][1], kwz_lin_rgb[i][2] };
        kwz_oklab(lin, lab[i]);
        memcpy(cand->lab, lab[i], sizeof(cand->lab));
        cand->c1 = cand->c2 = i;
        cand++;
    }
    for (i = 0; i < 6; i++) {
        for (j = i + 1; j < 6; j++) {
            int64_t pair = kwz_lab_dist(lab[i], lab[j]);
            for (r = 1; r < levels; r++) {
                int lin[3];
                for (k = 0; k < 3; k++)
                    lin[k] = (kwz_lin_rgb[i][k] * (levels - r) + kwz_lin_rgb[j][k] * r +
                              levels / 2) / levels;
                kwz_oklab(lin, cand->lab);
                cand->penalty = (int64_t)KWZ_MIX_FLOOR * KWZ_MIX_FLOOR +
                                pair * (FFABS(2 * r - levels) + levels) /
                                (KWZ_MIX_PENALTY * levels);
                cand->c1 = i;
                cand->c2 = j;
                cand->r  = r;
                cand++;
            }
        }
    }
    c->nb_cand = cand - c->cand;
    return 0;
}

/* Choose what reproduces an RGB555 colour best. */
static void kwz_make_plan(const KwzMuxContext *c, int key, uint8_t *plan)
{
    int lin[3] = { kwz_lin5[key >> 10], kwz_lin5[(key >> 5) & 31], kwz_lin5[key & 31] };
    int lab[3], i;
    int64_t best = INT64_MAX;

    kwz_oklab(lin, lab);
    plan[0] = 1;
    for (i = 0; i < c->nb_cand; i++) {
        const KwzCand *cand = &c->cand[i];
        int64_t d = kwz_lab_dist(lab, cand->lab) + cand->penalty;
        if (d < best) {
            best    = d;
            plan[1] = cand->c1;
            plan[2] = cand->c2;
            plan[3] = cand->r;
        }
    }
}

static void kwz_map_colours(KwzMuxContext *c, const uint8_t *rgb, uint8_t *idx)
{
    int mask = c->dither - 1, x, y;

    for (y = 0; y < KWZ_H; y++) {
        const uint8_t *row = c->bayer[y & mask];
        for (x = 0; x < KWZ_W; x++, rgb += 3) {
            int key = (rgb[0] >> 3) << 10 | (rgb[1] >> 3) << 5 | rgb[2] >> 3;
            uint8_t *plan = c->plan[key];
            if (!plan[0])
                kwz_make_plan(c, key, plan);
            *idx++ = row[x & mask] < plan[3] ? plan[2] : plan[1];
        }
    }
}

/* ---------- layer compression ---------- */

typedef struct KwzBits {
    uint8_t *p;
    uint64_t acc;
    int      n;
} KwzBits;

static inline void kwz_put(KwzBits *b, unsigned value, int bits)
{
    b->acc |= (uint64_t)value << b->n;
    b->n   += bits;
    while (b->n >= 16) {
        AV_WL16(b->p, b->acc & 0xffff);
        b->p    += 2;
        b->acc >>= 16;
        b->n    -= 16;
    }
}

/* Write one tile with the cheapest type that can represent its eight lines;
 * the lower type wins a tie. */
static void kwz_put_tile(const KwzMuxContext *c, KwzBits *b, const uint16_t *ls)
{
    int com[8], same = 1, alt = 1, k, p;
    unsigned flags = 0;

    for (k = 0; k < 8; k++) {
        com[k] = c->line_common[ls[k]];
        if (com[k] >= 0)
            flags |= 1 << k;
        if (ls[k] != ls[0])
            same = 0;
        if (ls[k] != ((k & 1) ? c->line_shift[ls[0]] : ls[0]))
            alt = 0;
    }

    if (same) {                         /* one line repeated */
        if (com[0] >= 0) {
            kwz_put(b, 0, 3);
            kwz_put(b, com[0], 5);
        } else {
            kwz_put(b, 1, 3);
            kwz_put(b, ls[0], 13);
        }
        return;
    }
    if (alt) {                          /* a line alternating with its shifted self */
        if (com[0] >= 0) {
            kwz_put(b, 2, 3);
            kwz_put(b, com[0], 5);
        } else {
            kwz_put(b, 3, 3);
            kwz_put(b, ls[0], 13);
        }
        return;
    }
    for (p = 0; p < 4; p++) {           /* two lines in a fixed pattern */
        int bi = p == 1 ? 2 : 1;
        for (k = 0; k < 8; k++) {
            if (ls[k] != ls[kwz_patterns[p][k] ? bi : 0])
                break;
        }
        if (k < 8)
            continue;
        kwz_put(b, 7, 3);
        if (com[0] >= 0 && com[bi] >= 0) {
            kwz_put(b, (p - 1) & 3, 2);
            kwz_put(b, 1, 1);
            kwz_put(b, com[0], 5);
            kwz_put(b, com[bi], 5);
        } else {
            kwz_put(b, p, 2);
            kwz_put(b, 0, 1);
            kwz_put(b, ls[0], 13);
            kwz_put(b, ls[bi], 13);
        }
        return;
    }
    kwz_put(b, 4, 3);                   /* every line on its own */
    kwz_put(b, flags, 8);
    for (k = 0; k < 8; k++) {
        if (com[k] >= 0)
            kwz_put(b, com[k], 5);
        else
            kwz_put(b, ls[k], 13);
    }
}

/* Compress one layer; with prev, tiles equal to the previous frame are
 * skipped in runs of up to 32. Returns the size in bytes. */
static int kwz_encode_layer(const KwzMuxContext *c, const uint16_t *cur,
                            const uint16_t *prev, uint8_t *out)
{
    KwzBits b = { out, 0, 0 };
    int i = 0;

    while (i < KWZ_TILES) {
        if (prev && !memcmp(cur + 8 * i, prev + 8 * i, 8 * sizeof(*cur))) {
            int run = 1;
            while (run < 32 && i + run < KWZ_TILES &&
                   !memcmp(cur + 8 * (i + run), prev + 8 * (i + run), 8 * sizeof(*cur)))
                run++;
            kwz_put(&b, 5, 3);
            kwz_put(&b, run - 1, 5);
            i += run;
        } else {
            kwz_put_tile(c, &b, cur + 8 * i);
            i++;
        }
    }
    if (b.n) {
        AV_WL16(b.p, b.acc & 0xffff);
        b.p += 2;
    }
    return b.p - out;
}

static int kwz_buf_grow(uint8_t **buf, unsigned *alloc, unsigned size, unsigned add)
{
    uint8_t *p;
    if (size + add <= *alloc)
        return 0;
    p = av_fast_realloc(*buf, alloc, size + add);
    if (!p)
        return AVERROR(ENOMEM);
    *buf = p;
    return 0;
}

static int kwz_add_frame(AVFormatContext *s, const uint8_t *idx)
{
    KwzMuxContext *c = s->priv_data;
    uint16_t **cur  = c->lines[c->cur];
    uint16_t **prev = c->lines[c->cur ^ 1];
    uint8_t *meta   = c->meta + KWZ_META_SIZE * c->nb_frames;
    int first = !c->nb_frames, l, t, r, k, ret;

    for (t = 0; t < KWZ_TILES; t++) {
        const uint8_t *px = idx + c->tile_off[t];
        for (r = 0; r < 8; r++, px += KWZ_W) {
            int line[3] = { 0, 0, 0 };
            for (k = 0; k < 8; k++) {
                int w = kwz_line_weight[k], ci = px[k];
                line[0] += c->layer_value[0][ci] * w;
                line[1] += c->layer_value[1][ci] * w;
                line[2] += c->layer_value[2][ci] * w;
            }
            cur[0][8 * t + r] = line[0];
            cur[1][8 * t + r] = line[1];
            cur[2][8 * t + r] = line[2];
        }
    }

    if ((ret = kwz_buf_grow(&c->frames, &c->frames_alloc, c->frames_size,
                            3 * KWZ_LAYER_MAX)) < 0)
        return ret;
    /* The first frame stands alone, later layers build on the previous frame. */
    AV_WL32(meta, KWZ_LAYER_COLOURS | (first ? 0x70 : 0) | (c->paper ? c->paper : 6));
    for (l = 0; l < 3; l++) {
        int size = kwz_encode_layer(c, cur[l], first ? NULL : prev[l],
                                    c->frames + c->frames_size);
        AV_WL16(meta + 4 + 2 * l, size);
        c->frames_size += size;
    }
    memcpy(meta + 0x0A, c->fsid, 10);
    meta[0x14] = meta[0x15] = meta[0x16] = 3;       /* layer depths */

    c->cur ^= 1;
    c->nb_frames++;
    return 0;
}

/* ---------- audio ---------- */

static inline void kwz_adpcm_step(int *pred, int *index, int code, int two_bit)
{
    int step = kwz_step_table[*index], diff = step >> 3;

    if (two_bit) {
        if (code & 1) diff += step;
        if (code & 2) diff  = -diff;
        *index += kwz_index_2bit[code];
    } else {
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff  = -diff;
        *index += kwz_index_4bit[code];
    }
    *index = av_clip(*index, 0, 79);
    *pred  = av_clip(*pred + diff, -2048, 2047);
}

static inline int kwz_pcm_target(const uint8_t *pcm, int n, int i)
{
    return av_clip(((int16_t)AV_RL16(pcm + 2 * FFMIN(i, n - 1)) + 8) >> 4, -2048, 2047);
}

/* Encode s16le PCM as the format's 12-bit ADPCM. A sample takes two bits
 * while the step is small or when only two bits are left in the byte, four
 * otherwise; the code is chosen looking one sample ahead. The last byte is
 * filled by holding the final sample. Returns the size in bytes. */
static int kwz_adpcm_encode(const uint8_t *pcm, int n, uint8_t *dst)
{
    int pred = 0, index = 40, bit = 0, byte = 0, len = 0, i;

    for (i = 0; i < n || bit; i++) {
        int t0 = kwz_pcm_target(pcm, n, i), t1 = kwz_pcm_target(pcm, n, i + 1);
        int two = index < 18 || bit > 4, bits = two ? 2 : 4;
        int code, best_code = 0;
        int64_t best = INT64_MAX;

        for (code = 0; code < 1 << bits; code++) {
            int p1 = pred, i1 = index, two1, code1;
            int64_t err, next = INT64_MAX;

            kwz_adpcm_step(&p1, &i1, code, two);
            err  = (int64_t)(p1 - t0) * (p1 - t0);
            two1 = i1 < 18 || ((bit + bits) & 7) > 4;
            for (code1 = 0; code1 < (two1 ? 4 : 16); code1++) {
                int p2 = p1, i2 = i1;
                kwz_adpcm_step(&p2, &i2, code1, two1);
                next = FFMIN(next, (int64_t)(p2 - t1) * (p2 - t1));
            }
            if (err + next < best) {
                best      = err + next;
                best_code = code;
            }
        }
        kwz_adpcm_step(&pred, &index, best_code, two);
        byte |= best_code << bit;
        bit  += bits;
        if (bit == 8) {
            dst[len++] = byte;
            byte = bit = 0;
        }
    }
    return len;
}

/* ---------- muxer ---------- */

static int kwz_init(AVFormatContext *s)
{
    KwzMuxContext *c = s->priv_data;
    AVRational fr = { 0, 1 };
    int i, j, k, n, best = 0;
    double diff, best_diff = 1e9, fps;

    c->audio_index = -1;
    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && i == 0) {
            if (par->codec_id != AV_CODEC_ID_RAWVIDEO || par->format != AV_PIX_FMT_RGB24 ||
                par->width != KWZ_W || par->height != KWZ_H) {
                av_log(s, AV_LOG_ERROR, "Video must be rawvideo rgb24 320x240, "
                       "e.g. -vf scale=320:240 -pix_fmt rgb24 -c:v rawvideo\n");
                return AVERROR(EINVAL);
            }
            fr = s->streams[i]->avg_frame_rate;
            if (fr.num <= 0)
                fr = s->streams[i]->r_frame_rate;
            if (fr.num <= 0)
                fr = av_inv_q(s->streams[i]->time_base);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && c->audio_index < 0) {
            if (par->codec_id != AV_CODEC_ID_PCM_S16LE || par->sample_rate != KWZ_BGM_RATE ||
                par->ch_layout.nb_channels != 1) {
                av_log(s, AV_LOG_ERROR, "Audio must be mono pcm_s16le at %d Hz, "
                       "e.g. -ar %d -ac 1 -c:a pcm_s16le\n", KWZ_BGM_RATE, KWZ_BGM_RATE);
                return AVERROR(EINVAL);
            }
            c->audio_index = i;
        } else {
            av_log(s, AV_LOG_ERROR, "Stream %d not supported: need one rgb24 video "
                   "stream (first) and at most one audio stream\n", i);
            return AVERROR(EINVAL);
        }
    }
    if (!s->nb_streams || s->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(s, AV_LOG_ERROR, "The first stream must be video\n");
        return AVERROR(EINVAL);
    }
    if (fr.num <= 0) {
        av_log(s, AV_LOG_ERROR, "Unknown frame rate, set it with -r\n");
        return AVERROR(EINVAL);
    }

    fps = av_q2d(fr);
    for (i = 0; i <= 10; i++) {
        diff = fabs(kwz_framerates[i] - fps);
        if (diff < best_diff) {
            best_diff = diff;
            best      = i;
        }
    }
    if (best_diff > 0.01)
        av_log(s, AV_LOG_WARNING, "Frame rate %.3f is not supported by Flipnote, "
               "using %.1f fps (supported: 0.2 0.5 1 2 4 6 8 12 20 24 30)\n",
               fps, kwz_framerates[best]);
    c->speed = best;

    /* author ID: given, or derived from the author name */
    if (c->fsid_str && *c->fsid_str) {
        const char *p = c->fsid_str;
        for (n = 0; *p; p++) {
            int v = av_isxdigit(*p) ? (av_isdigit(*p) ? *p - '0' : av_tolower(*p) - 'a' + 10) : -1;
            if (v < 0 && *p == '-')
                continue;
            if (v < 0 || n >= 20) {
                n = -1;
                break;
            }
            c->fsid[n >> 1] |= v << (n & 1 ? 0 : 4);
            n++;
        }
        if (n != 18 && n != 20) {
            av_log(s, AV_LOG_ERROR, "kwz_fsid must be 18 or 20 hex digits\n");
            return AVERROR(EINVAL);
        }
    } else {
        struct AVSHA *sha = av_sha_alloc();
        uint8_t digest[32];
        if (!sha)
            return AVERROR(ENOMEM);
        av_sha_init(sha, 256);
        av_sha_update(sha, c->author, strlen(c->author));
        av_sha_final(sha, digest);
        av_free(sha);
        memcpy(c->fsid + 1, digest, 8);
    }

    if (c->key_path && *c->key_path) {
        int ret = kwz_load_key(s);
        if (ret < 0)
            return ret;
    } else {
        av_log(s, AV_LOG_WARNING, "No signing key given (-kwz_key): the note is written "
               "unsigned and Flipnote Studio 3D will not open it\n");
    }

    /* line tables */
    for (i = 0; i < KWZ_LINES; i++) {
        int shifted = 0;
        for (k = 0; k < 8; k++)
            shifted += i / kwz_line_weight[(k + 1) & 7] % 3 * kwz_line_weight[k];
        c->line_shift[i] = shifted;
    }
    memset(c->line_common, -1, sizeof(c->line_common));
    for (i = 0; i < 32; i++)
        c->line_common[kwz_common_idx[i]] = i;

    /* tiles are stored large tile by large tile */
    n = 0;
    for (i = 0; i < KWZ_H; i += 128)
        for (j = 0; j < KWZ_W; j += 128)
            for (int y = i; y < FFMIN(i + 128, KWZ_H); y += 8)
                for (int x = j; x < FFMIN(j + 128, KWZ_W); x += 8)
                    c->tile_off[n++] = y * KWZ_W + x;
    av_assert0(n == KWZ_TILES);

    /* pixels of the paper colour stay transparent in every layer */
    for (i = 0; i < 6; i++) {
        if (i != c->paper)
            c->layer_value[kwz_ink_layer[i]][i] = kwz_ink_value[i];
    }

    /* threshold matrix */
    if (!c->dither)
        c->dither = 1;
    for (i = 0; i < c->dither; i++) {
        for (j = 0; j < c->dither; j++) {
            int v = 0, bit;
            for (bit = 0; (1 << bit) < c->dither; bit++) {
                int xb = (j >> bit) & 1, yb = (i >> bit) & 1;
                v = v << 2 | ((xb ^ yb) << 1 | yb);
            }
            c->bayer[i][j] = v;
        }
    }

    if ((i = kwz_init_colours(c)) < 0)
        return i;
    c->plan  = av_calloc(32768, sizeof(*c->plan));
    c->idx   = av_malloc(KWZ_PIXELS);
    c->first = av_malloc(KWZ_PIXELS);
    c->meta  = av_calloc(KWZ_MAX_FRAMES, KWZ_META_SIZE);
    if (!c->plan || !c->idx || !c->first || !c->meta)
        return AVERROR(ENOMEM);
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 3; j++) {
            c->lines[i][j] = av_calloc(KWZ_TILES * 8, sizeof(uint16_t));
            if (!c->lines[i][j])
                return AVERROR(ENOMEM);
        }
    }
    return 0;
}

static int kwz_write_header(AVFormatContext *s)
{
    return 0;
}

static int kwz_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    KwzMuxContext *c = s->priv_data;
    int ret;

    if (pkt->stream_index == c->audio_index) {
        if ((ret = kwz_buf_grow(&c->pcm, &c->pcm_alloc, c->pcm_size, pkt->size)) < 0)
            return ret;
        memcpy(c->pcm + c->pcm_size, pkt->data, pkt->size);
        c->pcm_size += pkt->size;
        return 0;
    }

    if (c->nb_frames >= KWZ_MAX_FRAMES) {
        if (!c->warned)
            av_log(s, AV_LOG_WARNING, "Flipnote is limited to %d frames, dropping the rest\n",
                   KWZ_MAX_FRAMES);
        c->warned = 1;
        return 0;
    }
    if (pkt->size != KWZ_PIXELS * 3) {
        av_log(s, AV_LOG_ERROR, "Unexpected video packet size %d\n", pkt->size);
        return AVERROR_INVALIDDATA;
    }
    kwz_map_colours(c, pkt->data, c->idx);
    if (!c->nb_frames)
        memcpy(c->first, c->idx, KWZ_PIXELS);
    return kwz_add_frame(s, c->idx);
}

static void kwz_put_name(uint8_t *dst, const char *name)
{
    const uint8_t *p = (const uint8_t *)name, *end = p + strlen(name);
    int n = 0;

    while (p < end && n < 10) {
        int32_t cp;
        if (av_utf8_decode(&cp, &p, end, 0) < 0 || cp > 0xFFFF)
            cp = '?';
        AV_WL16(dst + 2 * n, cp);
        n++;
    }
}

/* File name: author ID and both timestamps in base 32. */
static void kwz_put_filename(uint8_t *dst, const uint8_t *fsid, uint32_t created,
                             uint32_t edited)
{
    static const char alphabet[] = "cwmfjordvegbalksnthpyxquiz012345";
    uint8_t src[17];
    uint32_t value = 0;
    int bits = 0, i;

    memcpy(src, fsid, 9);
    AV_WL32(src + 9,  created);
    AV_WL32(src + 13, edited);
    for (i = 0; i < 17; i++) {
        value = value << 8 | src[i];
        bits += 8;
        while (bits >= 5) {
            *dst++ = alphabet[(value >> (bits - 5)) & 31];
            bits  -= 5;
        }
    }
    if (bits > 0)
        *dst++ = alphabet[(value << (5 - bits)) & 31];
}

/* ---------- thumbnail ---------- */

/* The thumbnail is an 80x64 baseline JPEG of the first frame, with the
 * picture in the top 60 rows. It is laid out like the ones the application
 * writes: no application segments, the frame header first, 4:2:2 sampling
 * with one chroma block per two luma blocks, the standard tables at libjpeg
 * quality 95. */

static const uint8_t kwz_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint8_t kwz_std_quant[2][64] = {
    { 16, 11, 10, 16,  24,  40,  51,  61,  12, 12, 14, 19,  26,  58,  60,  55,
      14, 13, 16, 24,  40,  57,  69,  56,  14, 17, 22, 29,  51,  87,  80,  62,
      18, 22, 37, 56,  68, 109, 103,  77,  24, 35, 55, 64,  81, 104, 113,  92,
      49, 64, 78, 87, 103, 121, 120, 101,  72, 92, 95, 98, 112, 100, 103,  99 },
    { 17, 18, 24, 47, 99, 99, 99, 99,  18, 21, 26, 66, 99, 99, 99, 99,
      24, 26, 56, 99, 99, 99, 99, 99,  47, 66, 99, 99, 99, 99, 99, 99,
      99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
      99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99 },
};

/* 8192 * cos(n * pi / 16) */
static const int16_t kwz_cos[9] = { 8192, 8035, 7568, 6811, 5793, 4551, 3135, 1598, 0 };

typedef struct KwzHuff {
    uint16_t code[256];
    uint8_t  size[256];
} KwzHuff;

typedef struct KwzJpeg {
    uint8_t *p;
    uint32_t acc;
    int      n;
    uint8_t  quant[2][64];
    KwzHuff  dc[2], ac[2];
    int      last_dc[3];
} KwzJpeg;

static void kwz_jpeg_huff(KwzHuff *h, const uint8_t *bits, const uint8_t *vals)
{
    int code = 0, len, i;
    for (len = 1; len <= 16; len++) {
        for (i = 0; i < bits[len]; i++) {
            h->code[*vals]   = code++;
            h->size[*vals++] = len;
        }
        code <<= 1;
    }
}

static void kwz_jpeg_bits(KwzJpeg *j, unsigned value, int bits)
{
    j->acc = j->acc << bits | (value & ((1u << bits) - 1));
    j->n  += bits;
    while (j->n >= 8) {
        int byte = (j->acc >> (j->n - 8)) & 0xff;
        *j->p++ = byte;
        if (byte == 0xff)
            *j->p++ = 0;
        j->n -= 8;
    }
}

static inline int kwz_jpeg_cos(int n)
{
    n &= 31;
    return n <= 8 ? kwz_cos[n] : n <= 16 ? -kwz_cos[16 - n] :
           n <= 24 ? -kwz_cos[n - 16] : kwz_cos[32 - n];
}

/* Transform, quantise and code one 8x8 block. */
static void kwz_jpeg_block(KwzJpeg *j, const uint8_t *src, int stride, int comp)
{
    const uint8_t *quant = j->quant[!!comp];
    const KwzHuff *dc = &j->dc[!!comp], *ac = &j->ac[!!comp];
    int64_t tmp[64];
    int coef[64], x, y, u, v, k, run, diff, mag, bits;

    for (y = 0; y < 8; y++) {
        for (u = 0; u < 8; u++) {
            int64_t sum = 0;
            for (x = 0; x < 8; x++)
                sum += (src[y * stride + x] - 128) * kwz_jpeg_cos((2 * x + 1) * u);
            tmp[8 * y + u] = (sum * (u ? 8192 : 5793)) >> 13;
        }
    }
    for (u = 0; u < 8; u++) {
        for (v = 0; v < 8; v++) {
            int64_t sum = 0, div = (int64_t)quant[8 * v + u] << 28;
            for (y = 0; y < 8; y++)
                sum += tmp[8 * y + u] * kwz_jpeg_cos((2 * y + 1) * v);
            sum = (sum * (v ? 8192 : 5793)) >> 13;
            coef[8 * v + u] = sum < 0 ? -(int)((-sum + div / 2) / div)
                                      :  (int)(( sum + div / 2) / div);
        }
    }

    diff = coef[0] - j->last_dc[comp];
    j->last_dc[comp] = coef[0];
    mag  = FFABS(diff);
    bits = mag ? av_log2(mag) + 1 : 0;
    kwz_jpeg_bits(j, dc->code[bits], dc->size[bits]);
    if (bits)
        kwz_jpeg_bits(j, diff < 0 ? diff - 1 : diff, bits);

    for (k = 1, run = 0; k < 64; k++) {
        int c = coef[kwz_zigzag[k]];
        if (!c) {
            run++;
            continue;
        }
        for (; run >= 16; run -= 16)
            kwz_jpeg_bits(j, ac->code[0xf0], ac->size[0xf0]);
        mag  = FFABS(c);
        bits = av_log2(mag) + 1;
        kwz_jpeg_bits(j, ac->code[run << 4 | bits], ac->size[run << 4 | bits]);
        kwz_jpeg_bits(j, c < 0 ? c - 1 : c, bits);
        run = 0;
    }
    if (run)
        kwz_jpeg_bits(j, ac->code[0], ac->size[0]);
}

static uint8_t *kwz_jpeg_table(uint8_t *p, int id, const uint8_t *bits, const uint8_t *vals)
{
    int n = 0, i;
    *p++ = id;
    for (i = 1; i <= 16; i++) {
        *p++ = bits[i];
        n   += bits[i];
    }
    memcpy(p, vals, n);
    return p + n;
}

/* Encode the planes (luma 80x64, chroma 40x64) at a libjpeg quality. */
static int kwz_jpeg_encode(KwzJpeg *j, uint8_t *out, uint8_t *const planes[3], int quality)
{
    static const uint8_t sof[] = {
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, KWZ_THUMB_H, 0x00, KWZ_THUMB_W, 0x03,
        0x01, 0x21, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01
    };
    static const uint8_t sos[] = {
        0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00
    };
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    uint8_t *p = out, *len;
    int t, i, mx, my;

    for (t = 0; t < 2; t++)
        for (i = 0; i < 64; i++)
            j->quant[t][i] = av_clip((kwz_std_quant[t][i] * scale + 50) / 100, 1, 255);

    AV_WB16(p, 0xffd8);
    p += 2;
    memcpy(p, sof, sizeof(sof));
    p += sizeof(sof);

    AV_WB16(p, 0xffdb);
    AV_WB16(p + 2, 2 + 2 * 65);
    p += 4;
    for (t = 0; t < 2; t++) {
        *p++ = t;
        for (i = 0; i < 64; i++)
            *p++ = j->quant[t][kwz_zigzag[i]];
    }

    AV_WB16(p, 0xffc4);
    len = p + 2;
    p   = kwz_jpeg_table(p + 4, 0x00, ff_mjpeg_bits_dc_luminance,   ff_mjpeg_val_dc);
    p   = kwz_jpeg_table(p,     0x01, ff_mjpeg_bits_dc_chrominance, ff_mjpeg_val_dc);
    p   = kwz_jpeg_table(p,     0x10, ff_mjpeg_bits_ac_luminance,   ff_mjpeg_val_ac_luminance);
    p   = kwz_jpeg_table(p,     0x11, ff_mjpeg_bits_ac_chrominance, ff_mjpeg_val_ac_chrominance);
    AV_WB16(len, p - len);

    memcpy(p, sos, sizeof(sos));
    p += sizeof(sos);

    j->p   = p;
    j->acc = j->n = 0;
    memset(j->last_dc, 0, sizeof(j->last_dc));
    for (my = 0; my < KWZ_THUMB_H / 8; my++) {
        for (mx = 0; mx < KWZ_THUMB_W / 16; mx++) {
            kwz_jpeg_block(j, planes[0] + 8 * my * KWZ_THUMB_W + 16 * mx,     KWZ_THUMB_W, 0);
            kwz_jpeg_block(j, planes[0] + 8 * my * KWZ_THUMB_W + 16 * mx + 8, KWZ_THUMB_W, 0);
            kwz_jpeg_block(j, planes[1] + 8 * my * KWZ_THUMB_W / 2 + 8 * mx, KWZ_THUMB_W / 2, 1);
            kwz_jpeg_block(j, planes[2] + 8 * my * KWZ_THUMB_W / 2 + 8 * mx, KWZ_THUMB_W / 2, 2);
        }
    }
    if (j->n)
        kwz_jpeg_bits(j, 0x7f, 8 - j->n);
    AV_WB16(j->p, 0xffd9);
    return j->p + 2 - out;
}

static int kwz_make_thumbnail(AVFormatContext *s, uint8_t **out, int *out_size)
{
    KwzMuxContext *c = s->priv_data;
    KwzJpeg *j = av_mallocz(sizeof(*j));
    uint8_t *jpeg = av_malloc(65536);
    uint8_t *planes[3] = { av_malloc(KWZ_THUMB_W * KWZ_THUMB_H),
                           av_malloc(KWZ_THUMB_W * KWZ_THUMB_H / 2),
                           av_malloc(KWZ_THUMB_W * KWZ_THUMB_H / 2) };
    int x, y, k, quality, size = 0, ret = AVERROR(ENOMEM);

    if (!j || !jpeg || !planes[0] || !planes[1] || !planes[2])
        goto end;
    kwz_jpeg_huff(&j->dc[0], ff_mjpeg_bits_dc_luminance,   ff_mjpeg_val_dc);
    kwz_jpeg_huff(&j->dc[1], ff_mjpeg_bits_dc_chrominance, ff_mjpeg_val_dc);
    kwz_jpeg_huff(&j->ac[0], ff_mjpeg_bits_ac_luminance,   ff_mjpeg_val_ac_luminance);
    kwz_jpeg_huff(&j->ac[1], ff_mjpeg_bits_ac_chrominance, ff_mjpeg_val_ac_chrominance);

    /* a quarter-size picture, averaged over 4x4 pixels, black below it */
    for (y = 0; y < KWZ_THUMB_H; y++) {
        for (x = 0; x < KWZ_THUMB_W; x += 2) {
            int yuv[2][3];
            for (k = 0; k < 2; k++) {
                int rgb[3] = { 0, 0, 0 }, i, l;
                if (y < KWZ_H / 4) {
                    for (i = 0; i < 4; i++) {
                        for (l = 0; l < 4; l++) {
                            const uint8_t *px = kwz_rgb[c->first[(4 * y + i) * KWZ_W +
                                                                 4 * (x + k) + l]];
                            rgb[0] += px[0];
                            rgb[1] += px[1];
                            rgb[2] += px[2];
                        }
                    }
                }
                yuv[k][0] = ( 19595 * rgb[0] + 38470 * rgb[1] +  7471 * rgb[2] + (1 << 19)) >> 20;
                yuv[k][1] = (-11059 * rgb[0] - 21709 * rgb[1] + 32768 * rgb[2] + (1 << 19)) >> 20;
                yuv[k][2] = ( 32768 * rgb[0] - 27439 * rgb[1] -  5329 * rgb[2] + (1 << 19)) >> 20;
                planes[0][y * KWZ_THUMB_W + x + k] = av_clip_uint8(yuv[k][0]);
            }
            planes[1][(y * KWZ_THUMB_W + x) / 2] =
                av_clip_uint8(((yuv[0][1] + yuv[1][1] + 1) >> 1) + 128);
            planes[2][(y * KWZ_THUMB_W + x) / 2] =
                av_clip_uint8(((yuv[0][2] + yuv[1][2] + 1) >> 1) + 128);
        }
    }

    /* lower the quality until the picture is no larger than real ones */
    for (quality = 95; quality >= 5; quality -= 5) {
        size = kwz_jpeg_encode(j, jpeg, planes, quality);
        if (size <= KWZ_THUMB_MAX)
            break;
    }
    *out      = jpeg;
    *out_size = size;
    jpeg      = NULL;
    ret       = 0;

end:
    av_free(planes[0]);
    av_free(planes[1]);
    av_free(planes[2]);
    av_free(jpeg);
    av_free(j);
    return ret;
}

static uint32_t kwz_crc32(const uint8_t *data, int64_t size)
{
    return av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0xFFFFFFFF, data, size) ^ 0xFFFFFFFF;
}

/* Start a section; returns the position of its body. */
static uint8_t *kwz_section(uint8_t *p, const char *magic, int flag, uint32_t size)
{
    memcpy(p, magic, 3);
    p[3] = flag;
    AV_WL32(p + 4, size);
    return p + 8;
}

static int kwz_write_trailer(AVFormatContext *s)
{
    KwzMuxContext *c = s->priv_data;
    int nb = c->nb_frames, i, ret;
    int64_t pcm_samples, max_samples, file_size;
    uint32_t ktn_size, kmc_size, kmi_size, ksn_size, stamp;
    int bgm_len = 0, jpeg_len = 0;
    uint8_t *buf, *p, *bgm = NULL, *jpeg = NULL;

    if (!nb) {
        av_log(s, AV_LOG_ERROR, "No video frames written\n");
        return AVERROR(EINVAL);
    }

    /* keep the BGM no longer than the animation */
    pcm_samples = c->pcm_size / 2;
    max_samples = (int64_t)(nb * (double)KWZ_BGM_RATE / kwz_framerates[c->speed]);
    pcm_samples = FFMIN(pcm_samples, max_samples);
    if (pcm_samples > 0) {
        bgm = av_malloc(pcm_samples / 2 + 2);
        if (!bgm)
            return AVERROR(ENOMEM);
        bgm_len = kwz_adpcm_encode(c->pcm, pcm_samples, bgm);
        if (pcm_samples > 60 * KWZ_BGM_RATE)
            av_log(s, AV_LOG_WARNING, "The BGM is longer than the 60 seconds "
                   "Flipnote Studio 3D records\n");
    }

    if ((ret = kwz_make_thumbnail(s, &jpeg, &jpeg_len)) < 0) {
        av_free(bgm);
        return ret;
    }

    ktn_size  = FFALIGN(4 + jpeg_len, 4);
    kmc_size  = FFALIGN(4 + c->frames_size, 4);
    kmi_size  = nb * KWZ_META_SIZE;
    ksn_size  = FFALIGN(28 + bgm_len, 4);
    file_size = 5 * 8 + (int64_t)KWZ_KFH_SIZE + ktn_size + kmc_size + kmi_size +
                ksn_size + KWZ_SIG_LEN;

    buf = av_mallocz(file_size);
    if (!buf) {
        av_free(bgm);
        av_free(jpeg);
        return AVERROR(ENOMEM);
    }

    /* file header: an original note, so root, parent and current are the same */
    stamp = (s->flags & AVFMT_FLAG_BITEXACT) ? 0 : av_gettime() / 1000000 - 946684800;
    p = kwz_section(buf, "KFH", 0x14, KWZ_KFH_SIZE);
    AV_WL32(p + 0x04, stamp);
    AV_WL32(p + 0x08, stamp);
    for (i = 0; i < 3; i++) {
        memcpy(p + 0x10 + 10 * i, c->fsid, 10);
        kwz_put_name(p + 0x2E + 22 * i, c->author);
        kwz_put_filename(p + 0x70 + 28 * i, c->fsid, stamp, stamp);
    }
    AV_WL16(p + 0xC4, nb);
    AV_WL16(p + 0xC6, 0);                           /* thumbnail frame */
    AV_WL16(p + 0xC8, c->loop ? 2 : 0);
    p[0xCA] = c->speed;
    AV_WL32(p, kwz_crc32(p + 4, KWZ_KFH_SIZE - 4));
    p += KWZ_KFH_SIZE;

    p = kwz_section(p, "KTN", 0x02, ktn_size);
    memcpy(p + 4, jpeg, jpeg_len);
    AV_WL32(p, kwz_crc32(p + 4, ktn_size - 4));
    p += ktn_size;
    av_free(jpeg);

    p = kwz_section(p, "KMC", 0x02, kmc_size);
    memcpy(p + 4, c->frames, c->frames_size);
    AV_WL32(p, kwz_crc32(p + 4, kmc_size - 4));
    p += kmc_size;

    p = kwz_section(p, "KMI", 0x05, kmi_size);
    memcpy(p, c->meta, kmi_size);
    p += kmi_size;

    p = kwz_section(p, "KSN", 0x01, ksn_size);
    AV_WL32(p, c->speed);                           /* speed the BGM was recorded at */
    AV_WL32(p + 4, bgm_len);
    if (bgm_len)
        memcpy(p + 28, bgm, bgm_len);
    AV_WL32(p + 24, kwz_crc32(p + 28, ksn_size - 28));
    p += ksn_size;
    av_free(bgm);

    ret = c->have_key ? kwz_rsa_sign(c, buf, file_size - KWZ_SIG_LEN, p) : 0;
    if (ret >= 0)
        avio_write(s->pb, buf, file_size);
    av_free(buf);
    return ret;
}

static void kwz_deinit(AVFormatContext *s)
{
    KwzMuxContext *c = s->priv_data;
    int i, j;

    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++)
            av_freep(&c->lines[i][j]);
    av_freep(&c->cand);
    av_freep(&c->plan);
    av_freep(&c->idx);
    av_freep(&c->first);
    av_freep(&c->meta);
    av_freep(&c->frames);
    av_freep(&c->pcm);
}

#define OFFSET(x) offsetof(KwzMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption kwz_options[] = {
    { "kwz_author", "author name stored in the file (max 10 characters)",
      OFFSET(author), AV_OPT_TYPE_STRING, { .str = "mobipeg" }, 0, 0, E },
    { "kwz_fsid", "author ID as hex digits (default: derived from the author name)",
      OFFSET(fsid_str), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { "kwz_key", "RSA private key file to sign the note with (PKCS#1 or PKCS#8, DER or PEM)",
      OFFSET(key_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, E },
    { "kwz_dither", "ordered dither matrix size (0 = nearest colour)",
      OFFSET(dither), AV_OPT_TYPE_INT, { .i64 = 4 }, 0, 8, E, .unit = "dither" },
        { "none", "nearest colour", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "dither" },
        { "2x2",  "2x2 matrix, smallest files", 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, E, .unit = "dither" },
        { "4x4",  "4x4 matrix", 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, E, .unit = "dither" },
        { "8x8",  "8x8 matrix, smoothest tones", 0, AV_OPT_TYPE_CONST, { .i64 = 8 }, 0, 0, E, .unit = "dither" },
    { "kwz_paper", "paper colour",
      OFFSET(paper), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 5, E, .unit = "paper" },
        { "white",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, E, .unit = "paper" },
        { "black",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, E, .unit = "paper" },
        { "red",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, E, .unit = "paper" },
        { "yellow", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 3 }, 0, 0, E, .unit = "paper" },
        { "green",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 }, 0, 0, E, .unit = "paper" },
        { "blue",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 5 }, 0, 0, E, .unit = "paper" },
    { "kwz_loop", "set the loop flag",
      OFFSET(loop), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, E },
    { NULL },
};

static const AVClass kwz_muxer_class = {
    .class_name = "Flipnote KWZ muxer",
    .item_name  = av_default_item_name,
    .option     = kwz_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_kwz_muxer = {
    .p.name           = "kwz",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Flipnote Studio 3D (KWZ)"),
    .p.extensions     = "kwz",
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_RAWVIDEO,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &kwz_muxer_class,
    .priv_data_size   = sizeof(KwzMuxContext),
    .init             = kwz_init,
    .write_header     = kwz_write_header,
    .write_packet     = kwz_write_packet,
    .write_trailer    = kwz_write_trailer,
    .deinit           = kwz_deinit,
};
