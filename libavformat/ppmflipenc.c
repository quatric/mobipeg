/*
 * Flipnote Studio (PPM) muxer with RSA signing
 * Copyright (c) 2026
 *
 * Writes Nintendo DSi Flipnote Studio `.ppm` animation files. Video input is
 * 256x192 gray (ink where the value is below 128, on white paper, single
 * layer); audio input is optional mono s16le at 8192 Hz, stored as the BGM
 * track. The file is signed in the trailer with RSA-1024 / PKCS#1 v1.5 /
 * SHA-1 over everything preceding the last 0x90 bytes, as Flipnote Studio
 * expects when importing a note. The signing key is embedded and the modular
 * arithmetic is self-contained, so no external crypto library is needed.
 *
 * Format reference:
 *   https://github.com/Flipnote-Collective/flipnote-studio-docs/wiki/PPM-format
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

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/random_seed.h"
#include "libavutil/sha.h"
#include "libavutil/time.h"
#include "avformat.h"
#include "mux.h"

#define PPM_W          256
#define PPM_H          192
#define PPM_PIXELS     (PPM_W * PPM_H)
#define PPM_MAX_FRAMES 999
#define PPM_HDR_SIZE   0x6A0
#define PPM_TRAILER    0x90
#define PPM_SIG_LEN    0x80
#define PPM_BGM_RATE   8192

/* in-app frame speed 1..8 */
static const float ppm_framerates[9] = { 0.5f, 0.5f, 1, 2, 4, 6, 12, 20, 30 };

static const int8_t ppm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t ppm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/* RSA-1024 modulus / private exponent (big endian) */
static const uint8_t ppm_rsa_n[128] = {
    0xc2, 0x3c, 0xbc, 0x13, 0x2f, 0xaa, 0x12, 0x7e, 0x5b, 0xfe, 0x82, 0x3c,
    0xb0, 0x8b, 0xfb, 0x0c, 0xd1, 0x35, 0x01, 0xf7, 0x4c, 0x6a, 0x3a, 0xfb,
    0x82, 0xa6, 0x37, 0x6e, 0x11, 0x38, 0xcf, 0xa0, 0xdd, 0x85, 0xc0, 0xc7,
    0x9b, 0xc4, 0xd8, 0xdd, 0x28, 0x8a, 0x87, 0x53, 0x20, 0xee, 0xe0, 0x0b,
    0xeb, 0x43, 0xa0, 0x43, 0x25, 0xce, 0xa0, 0x29, 0x46, 0xd9, 0xd4, 0x4d,
    0xbb, 0x04, 0x66, 0x68, 0x08, 0xf1, 0xf8, 0xf7, 0x34, 0x11, 0x6f, 0xec,
    0xc0, 0x33, 0xa3, 0x3d, 0x12, 0x31, 0xf0, 0x43, 0xa0, 0x40, 0x06, 0xbd,
    0x2e, 0xd9, 0x37, 0x05, 0xef, 0x11, 0xa0, 0xda, 0xe4, 0x3d, 0x30, 0x15,
    0xb3, 0xf4, 0x07, 0xdb, 0x55, 0x0f, 0x75, 0x36, 0x37, 0xeb, 0x35, 0x6a,
    0x34, 0x7f, 0xb5, 0x0f, 0x99, 0xf7, 0xef, 0xd5, 0x5b, 0xe2, 0xc6, 0x64,
    0xe4, 0xd4, 0x10, 0xad, 0x6a, 0xf6, 0x71, 0x07,
};

static const uint8_t ppm_rsa_d[128] = {
    0x26, 0xa7, 0x53, 0x7e, 0x35, 0xf4, 0x10, 0x6e, 0x18, 0xfc, 0x93, 0x0a,
    0x64, 0xcf, 0xd6, 0x8b, 0xfc, 0x6e, 0x22, 0x10, 0x06, 0xa2, 0xf5, 0x4d,
    0xeb, 0xf8, 0x0b, 0xfb, 0xd9, 0x6d, 0x86, 0xd4, 0x2a, 0x81, 0x5d, 0x95,
    0xdb, 0x87, 0xd6, 0xe8, 0x5c, 0x13, 0x05, 0x4f, 0x23, 0xb4, 0xa5, 0xdd,
    0x79, 0x5a, 0x21, 0xe0, 0x7a, 0xfb, 0x9d, 0x9f, 0xd9, 0x3b, 0xac, 0x26,
    0x33, 0xdb, 0x72, 0x4d, 0x54, 0x87, 0xa3, 0x7e, 0xd7, 0x74, 0x5f, 0xf5,
    0x6e, 0xc3, 0xcc, 0xee, 0x2b, 0xbf, 0xd0, 0xcf, 0x00, 0xeb, 0xe8, 0xc9,
    0xaf, 0xde, 0x25, 0xe8, 0xf6, 0x29, 0x08, 0xfe, 0x72, 0x6a, 0x93, 0xc4,
    0x8f, 0x64, 0x51, 0xd7, 0x9e, 0x20, 0x4d, 0xa5, 0x4a, 0xef, 0xf2, 0x7f,
    0x39, 0xe3, 0xd9, 0xcb, 0xab, 0x51, 0x96, 0x07, 0x08, 0xd5, 0xac, 0x2c,
    0xaa, 0x00, 0x83, 0x72, 0xfc, 0x1e, 0x12, 0xc1,
};

/* DER DigestInfo prefix for SHA-1 */
static const uint8_t ppm_sha1_prefix[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05,
    0x00, 0x04, 0x14
};

typedef struct PpmMuxContext {
    const AVClass *class;
    char    *author;

    int      speed;            /* in-app frame speed, 1..8 */
    uint8_t *prev;             /* previous frame bits */
    uint8_t *first;            /* first frame bits, for the thumbnail */
    uint8_t *cur;

    uint8_t *frames;           /* concatenated frame data */
    unsigned frames_size, frames_alloc;
    uint32_t *offsets;
    int      nb_frames;

    uint8_t *pcm;              /* BGM input, s16le */
    unsigned pcm_size, pcm_alloc;
    int      audio_index;
} PpmMuxContext;

/* ---------- minimal 1024-bit modular arithmetic ---------- */

#define BN_LIMBS 32
typedef uint32_t bn_t[BN_LIMBS];

static void bn_load(bn_t r, const uint8_t *be)
{
    int i;
    for (i = 0; i < BN_LIMBS; i++)
        r[BN_LIMBS - 1 - i] = AV_RB32(be + 4 * i);
}

static void bn_store(uint8_t *be, const bn_t a)
{
    int i;
    for (i = 0; i < BN_LIMBS; i++)
        AV_WB32(be + 4 * i, a[BN_LIMBS - 1 - i]);
}

static int bn_ge(const bn_t a, const bn_t b)
{
    int i;
    for (i = BN_LIMBS - 1; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return 1;
}

static uint32_t bn_add(bn_t r, const bn_t a, const bn_t b)
{
    uint64_t c = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        c    += (uint64_t)a[i] + b[i];
        r[i]  = (uint32_t)c;
        c   >>= 32;
    }
    return (uint32_t)c;
}

static void bn_sub(bn_t r, const bn_t a, const bn_t b)
{
    int64_t c = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        c    += (int64_t)a[i] - b[i];
        r[i]  = (uint32_t)c;
        c   >>= 32;
    }
}

/* r = a + b mod n, with a, b < n */
static void bn_addmod(bn_t r, const bn_t a, const bn_t b, const bn_t n)
{
    uint32_t carry = bn_add(r, a, b);
    if (carry || bn_ge(r, n))
        bn_sub(r, r, n);
}

/* r = a * b mod n (double and add), r must not alias a or b */
static void bn_mulmod(bn_t r, const bn_t a, const bn_t b, const bn_t n)
{
    int i;
    memset(r, 0, sizeof(bn_t));
    for (i = BN_LIMBS * 32 - 1; i >= 0; i--) {
        bn_addmod(r, r, r, n);
        if ((b[i >> 5] >> (i & 31)) & 1)
            bn_addmod(r, r, a, n);
    }
}

/* r = base ^ e mod n */
static void bn_modexp(bn_t r, const bn_t base, const bn_t e, const bn_t n)
{
    bn_t acc, tmp;
    int i;
    memset(acc, 0, sizeof(acc));
    acc[0] = 1;
    for (i = BN_LIMBS * 32 - 1; i >= 0; i--) {
        bn_mulmod(tmp, acc, acc, n);
        if ((e[i >> 5] >> (i & 31)) & 1)
            bn_mulmod(acc, tmp, base, n);
        else
            memcpy(acc, tmp, sizeof(acc));
    }
    memcpy(r, acc, sizeof(bn_t));
}

/* RSASSA-PKCS1-v1_5 / SHA-1 signature of data, 128 bytes into sig */
static int ppm_rsa_sign(const uint8_t *data, int size, uint8_t sig[PPM_SIG_LEN])
{
    uint8_t em[PPM_SIG_LEN];
    struct AVSHA *sha = av_sha_alloc();
    bn_t m, n, d, s;
    int pad;

    if (!sha)
        return AVERROR(ENOMEM);
    av_sha_init(sha, 160);
    av_sha_update(sha, data, size);

    pad = PPM_SIG_LEN - 3 - sizeof(ppm_sha1_prefix) - 20;
    em[0] = 0x00;
    em[1] = 0x01;
    memset(em + 2, 0xff, pad);
    em[2 + pad] = 0x00;
    memcpy(em + 3 + pad, ppm_sha1_prefix, sizeof(ppm_sha1_prefix));
    av_sha_final(sha, em + 3 + pad + sizeof(ppm_sha1_prefix));
    av_free(sha);

    bn_load(m, em);
    bn_load(n, ppm_rsa_n);
    bn_load(d, ppm_rsa_d);
    bn_modexp(s, m, d, n);
    bn_store(sig, s);
    return 0;
}

/* ---------- frame encoding ---------- */

static int buf_grow(uint8_t **buf, unsigned *alloc, unsigned size, unsigned add)
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

/* Encode one 1-bit layer; line types first (48 bytes), then line data.
 * out must hold 48 + 192 * 32 bytes. Returns the encoded size. */
static int ppm_encode_layer(const uint8_t *bits, uint8_t *out)
{
    uint8_t types[PPM_H];
    uint8_t *data = out + PPM_H / 4;
    int y, c, i;

    for (y = 0; y < PPM_H; y++) {
        const uint8_t *line = bits + y * PPM_W;
        uint8_t chunk[PPM_W / 8];
        int nz = 0, nf = 0;
        uint32_t hdr;

        for (c = 0; c < PPM_W / 8; c++) {
            int v = 0;
            for (i = 0; i < 8; i++)
                v |= line[c * 8 + i] << i;
            chunk[c] = v;
            nz += v != 0x00;
            nf += v != 0xff;
        }
        if (!nz) {
            types[y] = 0;
        } else if (4 + nz <= 4 + nf && 4 + nz < 32) {
            types[y] = 1;
            hdr = 0;
            for (c = 0; c < PPM_W / 8; c++)
                if (chunk[c])
                    hdr |= 0x80000000u >> c;
            AV_WB32(data, hdr);
            data += 4;
            for (c = 0; c < PPM_W / 8; c++)
                if (chunk[c])
                    *data++ = chunk[c];
        } else if (4 + nf < 32) {
            types[y] = 2;
            hdr = 0;
            for (c = 0; c < PPM_W / 8; c++)
                if (chunk[c] != 0xff)
                    hdr |= 0x80000000u >> c;
            AV_WB32(data, hdr);
            data += 4;
            for (c = 0; c < PPM_W / 8; c++)
                if (chunk[c] != 0xff)
                    *data++ = chunk[c];
        } else {
            types[y] = 3;
            memcpy(data, chunk, sizeof(chunk));
            data += sizeof(chunk);
        }
    }
    for (y = 0; y < PPM_H; y += 4)
        out[y / 4] = types[y] | types[y + 1] << 2 | types[y + 2] << 4 | types[y + 3] << 6;
    return data - out;
}

static int ppm_add_frame(AVFormatContext *s, const uint8_t *bits)
{
    PpmMuxContext *c = s->priv_data;
    static uint8_t key_buf[PPM_H / 4 + PPM_H * 32], diff_buf[PPM_H / 4 + PPM_H * 32];
    uint8_t xored[PPM_PIXELS];
    int key_size, diff_size = INT_MAX, i, ret;
    int use_key, n;
    uint8_t *dst;

    key_size = ppm_encode_layer(bits, key_buf);
    if (c->nb_frames) {
        for (i = 0; i < PPM_PIXELS; i++)
            xored[i] = bits[i] ^ c->prev[i];
        diff_size = ppm_encode_layer(xored, diff_buf);
    }
    use_key = key_size <= diff_size;

    /* header byte, line types of layer 1 and (empty) layer 2, then layer 1
     * bitmap data; layer 2 has no data */
    n = use_key ? key_size : diff_size;
    if ((ret = buf_grow(&c->frames, &c->frames_alloc, c->frames_size, 1 + n + PPM_H / 4)) < 0)
        return ret;
    c->offsets[c->nb_frames] = c->frames_size;
    dst = c->frames + c->frames_size;
    /* paper white, both layers black; bit 7 marks a key frame */
    *dst++ = 0x0b | (use_key ? 0x80 : 0);
    memcpy(dst, use_key ? key_buf : diff_buf, PPM_H / 4);
    dst += PPM_H / 4;
    memset(dst, 0, PPM_H / 4);
    dst += PPM_H / 4;
    memcpy(dst, (use_key ? key_buf : diff_buf) + PPM_H / 4, n - PPM_H / 4);
    dst += n - PPM_H / 4;
    c->frames_size = dst - c->frames;

    if (!c->nb_frames)
        memcpy(c->first, bits, PPM_PIXELS);
    memcpy(c->prev, bits, PPM_PIXELS);
    c->nb_frames++;
    return 0;
}

/* ---------- audio ---------- */

static int ppm_adpcm_encode(const int16_t *pcm, int n, uint8_t *dst)
{
    int pred = 0, idx = 0, i;

    for (i = 0; i < n; i++) {
        int step = ppm_step_table[idx];
        int diff = pcm[i] - pred;
        int code = 0, dq;

        if (diff < 0) {
            code = 8;
            diff = -diff;
        }
        if (diff >= step) { code |= 4; diff -= step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; }
        step >>= 1;
        if (diff >= step)   code |= 1;

        step = ppm_step_table[idx];
        dq   = step >> 3;
        if (code & 1) dq += step >> 2;
        if (code & 2) dq += step >> 1;
        if (code & 4) dq += step;
        pred = av_clip_int16(pred + ((code & 8) ? -dq : dq));
        idx  = av_clip(idx + ppm_index_table[code], 0, 88);

        /* low nibble first */
        if (i & 1)
            dst[i >> 1] |= code << 4;
        else
            dst[i >> 1]  = code;
    }
    return (n + 1) / 2;
}

/* ---------- muxer ---------- */

static int ppm_init(AVFormatContext *s)
{
    PpmMuxContext *c = s->priv_data;
    AVRational fr = { 0, 1 };
    int i, best = 1;
    double diff, best_diff = 1e9, fps;

    c->audio_index = -1;
    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && i == 0) {
            if (par->codec_id != AV_CODEC_ID_RAWVIDEO || par->format != AV_PIX_FMT_GRAY8 ||
                par->width != PPM_W || par->height != PPM_H) {
                av_log(s, AV_LOG_ERROR, "Video must be rawvideo gray 256x192, "
                       "e.g. -vf scale=256:192,format=gray -c:v rawvideo\n");
                return AVERROR(EINVAL);
            }
            fr = s->streams[i]->avg_frame_rate;
            if (fr.num <= 0)
                fr = s->streams[i]->r_frame_rate;
            if (fr.num <= 0)
                fr = av_inv_q(s->streams[i]->time_base);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && c->audio_index < 0) {
            if (par->codec_id != AV_CODEC_ID_PCM_S16LE || par->sample_rate != PPM_BGM_RATE ||
                par->ch_layout.nb_channels != 1) {
                av_log(s, AV_LOG_ERROR, "Audio must be mono pcm_s16le at %d Hz, "
                       "e.g. -ar %d -ac 1 -c:a pcm_s16le\n", PPM_BGM_RATE, PPM_BGM_RATE);
                return AVERROR(EINVAL);
            }
            c->audio_index = i;
        } else {
            av_log(s, AV_LOG_ERROR, "Stream %d not supported: need one gray video "
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
    for (i = 1; i <= 8; i++) {
        diff = fabs(ppm_framerates[i] - fps);
        if (diff < best_diff) {
            best_diff = diff;
            best      = i;
        }
    }
    if (best_diff > 0.01)
        av_log(s, AV_LOG_WARNING, "Frame rate %.3f is not supported by Flipnote, "
               "using %.1f fps (supported: 0.5 1 2 4 6 12 20 30)\n",
               fps, ppm_framerates[best]);
    c->speed = best;

    c->offsets = av_calloc(PPM_MAX_FRAMES, sizeof(*c->offsets));
    c->prev    = av_mallocz(PPM_PIXELS);
    c->first   = av_mallocz(PPM_PIXELS);
    c->cur     = av_mallocz(PPM_PIXELS);
    if (!c->offsets || !c->prev || !c->first || !c->cur)
        return AVERROR(ENOMEM);
    return 0;
}

static int ppm_write_header(AVFormatContext *s)
{
    return 0;
}

static int ppm_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    PpmMuxContext *c = s->priv_data;
    int i, ret;

    if (pkt->stream_index == c->audio_index) {
        if ((ret = buf_grow(&c->pcm, &c->pcm_alloc, c->pcm_size, pkt->size)) < 0)
            return ret;
        memcpy(c->pcm + c->pcm_size, pkt->data, pkt->size);
        c->pcm_size += pkt->size;
        return 0;
    }

    if (c->nb_frames >= PPM_MAX_FRAMES) {
        av_log(s, AV_LOG_WARNING, "Flipnote is limited to %d frames, dropping the rest\n",
               PPM_MAX_FRAMES);
        return 0;
    }
    if (pkt->size != PPM_PIXELS) {
        av_log(s, AV_LOG_ERROR, "Unexpected video packet size %d\n", pkt->size);
        return AVERROR_INVALIDDATA;
    }
    for (i = 0; i < PPM_PIXELS; i++)
        c->cur[i] = pkt->data[i] < 128;
    return ppm_add_frame(s, c->cur);
}

static void ppm_put_name(uint8_t *dst, const char *name)
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

static void ppm_put_thumbnail(uint8_t *dst, const uint8_t *bits)
{
    int tx, ty, x, y;

    /* 64x48, 4 bpp, 8x8 tiles, low nibble first; 0 = white, 1 = dark */
    for (ty = 0; ty < 6; ty++) {
        for (tx = 0; tx < 8; tx++) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    int px = (tx * 8 + x) * 4, py = (ty * 8 + y) * 4;
                    int ink = 0, bx, by;
                    for (by = 0; by < 4; by++)
                        for (bx = 0; bx < 4; bx++)
                            ink |= bits[(py + by) * PPM_W + px + bx];
                    if (x & 1)
                        *dst++ |= ink << 4;
                    else
                        *dst = ink;
                }
            }
        }
    }
}

static int ppm_write_trailer(AVFormatContext *s)
{
    PpmMuxContext *c = s->priv_data;
    int nb = c->nb_frames, i, ret;
    int table_len = nb * 4;
    uint32_t anim_len, sound_flags_off;
    int64_t file_size;
    int pcm_samples, bgm_len = 0;
    uint8_t *buf, *p, *bgm = NULL;
    uint8_t mac_name[13];
    uint32_t seed;
    int64_t stamp;

    if (!nb) {
        av_log(s, AV_LOG_ERROR, "No video frames written\n");
        return AVERROR(EINVAL);
    }

    /* keep the BGM no longer than the animation */
    pcm_samples = c->pcm_size / 2;
    {
        int64_t max = (int64_t)nb * PPM_BGM_RATE * 1 / ppm_framerates[c->speed];
        if (pcm_samples > max)
            pcm_samples = max;
    }
    if (pcm_samples > 0) {
        bgm = av_mallocz(pcm_samples / 2 + 1);
        if (!bgm)
            return AVERROR(ENOMEM);
        {
            int16_t *pcm = av_malloc_array(pcm_samples, sizeof(*pcm));
            if (!pcm) {
                av_free(bgm);
                return AVERROR(ENOMEM);
            }
            for (i = 0; i < pcm_samples; i++)
                pcm[i] = AV_RL16(c->pcm + 2 * i);
            bgm_len = ppm_adpcm_encode(pcm, pcm_samples, bgm);
            av_free(pcm);
        }
    }

    anim_len = 8 + table_len + c->frames_size;
    sound_flags_off = PPM_HDR_SIZE + anim_len + nb;
    sound_flags_off = FFALIGN(sound_flags_off, 4);
    file_size = sound_flags_off + 32 + bgm_len + PPM_TRAILER;

    buf = av_mallocz(file_size);
    if (!buf) {
        av_free(bgm);
        return AVERROR(ENOMEM);
    }

    /* file header */
    memcpy(buf, "PARA", 4);
    AV_WL32(buf + 0x04, anim_len);
    AV_WL32(buf + 0x08, bgm_len);
    AV_WL16(buf + 0x0C, nb - 1);
    AV_WL16(buf + 0x0E, 0x24);
    /* 0x10 lock = 0, 0x12 thumbnail frame = 0 */

    seed  = av_get_random_seed();
    stamp = av_gettime() / 1000000 - 946684800;
    for (i = 0; i < 13; i++) {
        seed = seed * 1664525u + 1013904223u;
        mac_name[i] = "0123456789ABCDEF"[(seed >> 24) & 15];
    }
    /* original note: root, parent and current authors and files are identical */
    ppm_put_name(buf + 0x14, c->author);
    ppm_put_name(buf + 0x2A, c->author);
    ppm_put_name(buf + 0x40, c->author);
    for (i = 0; i < 8; i++) {
        buf[0x56 + i] = buf[0x5E + i] = buf[0x8A + i] = (seed >> (i * 3)) & 0xff;
        seed = seed * 1664525u + 1013904223u;
    }
    for (i = 0; i < 3; i++)
        buf[0x66 + i] = buf[0x78 + i] = (seed >> (i * 8)) & 0xff;
    memcpy(buf + 0x69, mac_name, 13);
    memcpy(buf + 0x7B, mac_name, 13);
    memcpy(buf + 0x92, mac_name + 5, 8);
    AV_WL32(buf + 0x9A, (uint32_t)stamp);
    ppm_put_thumbnail(buf + 0xA0, c->first);

    /* animation data */
    p = buf + PPM_HDR_SIZE;
    AV_WL16(p, table_len);
    p += 8;
    for (i = 0; i < nb; i++)
        AV_WL32(p + 4 * i, c->offsets[i]);
    p += table_len;
    memcpy(p, c->frames, c->frames_size);

    /* sound effect flags (none) are zero; sound header */
    p = buf + sound_flags_off;
    AV_WL32(p, bgm_len);
    p[16] = 8 - c->speed;
    p[17] = 8 - c->speed;
    if (bgm_len)
        memcpy(p + 32, bgm, bgm_len);
    av_free(bgm);

    ret = ppm_rsa_sign(buf, file_size - PPM_TRAILER, buf + file_size - PPM_TRAILER);
    if (ret >= 0)
        avio_write(s->pb, buf, file_size);
    av_free(buf);
    return ret;
}

static void ppm_deinit(AVFormatContext *s)
{
    PpmMuxContext *c = s->priv_data;
    av_freep(&c->offsets);
    av_freep(&c->prev);
    av_freep(&c->first);
    av_freep(&c->cur);
    av_freep(&c->frames);
    av_freep(&c->pcm);
}

#define OFFSET(x) offsetof(PpmMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption ppm_options[] = {
    { "ppm_author", "author name stored in the file (max 10 characters)",
      OFFSET(author), AV_OPT_TYPE_STRING, { .str = "mobipeg" }, 0, 0, E },
    { NULL },
};

static const AVClass ppm_muxer_class = {
    .class_name = "Flipnote PPM muxer",
    .item_name  = av_default_item_name,
    .option     = ppm_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_flipnote_ppm_muxer = {
    .p.name           = "flipnote_ppm",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Flipnote Studio (PPM)"),
    .p.extensions     = "ppm",
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_RAWVIDEO,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &ppm_muxer_class,
    .priv_data_size   = sizeof(PpmMuxContext),
    .init             = ppm_init,
    .write_header     = ppm_write_header,
    .write_packet     = ppm_write_packet,
    .write_trailer    = ppm_write_trailer,
    .deinit           = ppm_deinit,
};
