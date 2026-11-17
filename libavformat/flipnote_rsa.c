/*
 * Flipnote RSA signatures
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

/*
 * RSASSA-PKCS1-v1_5 signing for the Flipnote muxers: RSA-1024 / SHA-1 for
 * Flipnote Studio (PPM), RSA-2048 / SHA-256 for Flipnote Studio 3D (KWZ).
 * Exponentiation runs in Montgomery form, one multiply-and-reduce per bit of
 * the private exponent instead of a shift-and-subtract per bit of every
 * product.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/sha.h"
#include "flipnote_rsa.h"

#define MAX_LIMBS (FF_FLIPNOTE_RSA_MAX_BYTES / 4)

/* DER DigestInfo prefixes */
static const uint8_t sha1_prefix[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05,
    0x00, 0x04, 0x14
};
static const uint8_t sha256_prefix[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
    0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

/* Numbers are nl 32-bit limbs, least significant first. */

static int bn_ge(const uint32_t *a, const uint32_t *b, int nl)
{
    for (int i = nl - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 1;
}

static void bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int nl)
{
    int64_t c = 0;
    for (int i = 0; i < nl; i++) {
        c    += (int64_t)a[i] - b[i];
        r[i]  = (uint32_t)c;
        c   >>= 32;
    }
}

/* r = 2 * a mod n, with a < n */
static void bn_dbl(uint32_t *r, const uint32_t *a, const uint32_t *n, int nl)
{
    uint32_t carry = 0;
    for (int i = 0; i < nl; i++) {
        uint32_t v = a[i];
        r[i]  = (v << 1) | carry;
        carry = v >> 31;
    }
    if (carry || bn_ge(r, n, nl))
        bn_sub(r, r, n, nl);
}

/* Montgomery product r = a * b / 2^(32 nl) mod n, n odd, n0 = -1/n mod 2^32 */
static void bn_mont(uint32_t *r, const uint32_t *a, const uint32_t *b,
                    const uint32_t *n, uint32_t n0, int nl)
{
    uint32_t t[MAX_LIMBS + 2] = { 0 };

    for (int i = 0; i < nl; i++) {
        uint64_t c = 0;
        uint32_t m;
        for (int j = 0; j < nl; j++) {
            c    += (uint64_t)a[j] * b[i] + t[j];
            t[j]  = (uint32_t)c;
            c   >>= 32;
        }
        c += t[nl];
        t[nl]     = (uint32_t)c;
        t[nl + 1] = (uint32_t)(c >> 32);

        m = t[0] * n0;
        c = ((uint64_t)m * n[0] + t[0]) >> 32;
        for (int j = 1; j < nl; j++) {
            c        += (uint64_t)m * n[j] + t[j];
            t[j - 1]  = (uint32_t)c;
            c       >>= 32;
        }
        c += t[nl];
        t[nl - 1] = (uint32_t)c;
        t[nl]     = t[nl + 1] + (uint32_t)(c >> 32);
    }
    if (t[nl] || bn_ge(t, n, nl))
        bn_sub(t, t, n, nl);
    memcpy(r, t, nl * sizeof(*r));
}

int ff_flipnote_rsa_sign(const uint8_t *n_be, const uint8_t *d_be, int key_bytes,
                         int hash_bits, const uint8_t *data, int64_t size,
                         uint8_t *sig)
{
    uint8_t em[FF_FLIPNOTE_RSA_MAX_BYTES];
    uint32_t n[MAX_LIMBS], d[MAX_LIMBS], m[MAX_LIMBS];
    uint32_t one[MAX_LIMBS] = { 1 }, rr[MAX_LIMBS] = { 1 };
    uint32_t x[MAX_LIMBS], acc[MAX_LIMBS], n0;
    const uint8_t *prefix;
    int prefix_len, hash_len, pad, nl;
    struct AVSHA *sha;

    if (key_bytes <= 0 || key_bytes % 4 || key_bytes > FF_FLIPNOTE_RSA_MAX_BYTES)
        return AVERROR(EINVAL);
    nl = key_bytes / 4;

    if (hash_bits == 160) {
        prefix = sha1_prefix;   prefix_len = sizeof(sha1_prefix);   hash_len = 20;
    } else if (hash_bits == 256) {
        prefix = sha256_prefix; prefix_len = sizeof(sha256_prefix); hash_len = 32;
    } else {
        return AVERROR(EINVAL);
    }
    pad = key_bytes - 3 - prefix_len - hash_len;
    if (pad < 8)
        return AVERROR(EINVAL);

    for (int i = 0; i < nl; i++) {
        n[nl - 1 - i] = AV_RB32(n_be + 4 * i);
        d[nl - 1 - i] = AV_RB32(d_be + 4 * i);
    }
    /* Montgomery reduction needs an odd modulus, and a full-length one keeps
     * the encoded message, which starts 00 01, below it */
    if (!(n[0] & 1) || !(n[nl - 1] >> 31))
        return AVERROR(EINVAL);

    sha = av_sha_alloc();
    if (!sha)
        return AVERROR(ENOMEM);
    av_sha_init(sha, hash_bits);
    while (size > 0) {
        int chunk = FFMIN(size, 1 << 20);
        av_sha_update(sha, data, chunk);
        data += chunk;
        size -= chunk;
    }
    em[0] = 0x00;
    em[1] = 0x01;
    memset(em + 2, 0xff, pad);
    em[2 + pad] = 0x00;
    memcpy(em + 3 + pad, prefix, prefix_len);
    av_sha_final(sha, em + 3 + pad + prefix_len);
    av_free(sha);

    for (int i = 0; i < nl; i++)
        m[nl - 1 - i] = AV_RB32(em + 4 * i);

    /* n0 = -1 / n mod 2^32 */
    n0 = n[0];
    for (int i = 0; i < 5; i++)
        n0 *= 2 - n[0] * n0;
    n0 = -n0;
    /* rr = 2^(64 nl) mod n */
    for (int i = 0; i < 2 * 32 * nl; i++)
        bn_dbl(rr, rr, n, nl);

    bn_mont(x, m, rr, n, n0, nl);
    bn_mont(acc, one, rr, n, n0, nl);
    for (int i = 32 * nl - 1; i >= 0; i--) {
        bn_mont(acc, acc, acc, n, n0, nl);
        if ((d[i >> 5] >> (i & 31)) & 1)
            bn_mont(acc, acc, x, n, n0, nl);
    }
    bn_mont(acc, acc, one, n, n0, nl);

    for (int i = 0; i < nl; i++)
        AV_WB32(sig + 4 * i, acc[nl - 1 - i]);
    return 0;
}
