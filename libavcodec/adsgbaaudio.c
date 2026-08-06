/*
 * ADS-era (Majesco) GBA Video audio decoder
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
 * Audio half of the ADS-era GBA Video stack: adaptive predictive signed 8-bit
 * PCM, not IMA ADPCM.
 *
 * A block is [preamble][64 context-table entries][samples]. Bits are read from
 * 32-bit little-endian words, most significant bit first.
 *
 * Each sample is a 1-3 bit VLC giving a rank, which a per-context permutation
 * turns into a 2-bit symbol: bit 0 selects a magnitude roughly three times
 * larger, bit 1 negates. The context is a shift register of the last three
 * symbols. Magnitude and step size are carried in a log domain, and the
 * predictor is a three-tap leaky filter.
 *
 * The context table is transmitted per block, VLC-coded over a seven-value
 * palette; that part is verified bit-exact against hardware. The sample loop
 * below is transcribed from the IWRAM routine at 0x03002b4c but is not yet
 * exact - it produces correct silence and plausible amplitude, then degrades.
 * See doc/gba_video_ads.md.
 */

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define CTX_ENTRIES 64

/* main.bin 0x8a04: seven permutations of {0,1,2,3}, two bits each */
static const uint8_t ctx_palette[7] = { 0xd8, 0x72, 0xe4, 0x4e, 0x78, 0xd2, 0xe1 };

/* main.bin 0x8a0b: top four bits of the accumulator -> palette index, length */
static const uint8_t ctx_vlc[12][2] = {
    { 0, 2 }, { 0, 2 }, { 0, 2 }, { 0, 2 },
    { 1, 3 }, { 1, 3 }, { 2, 3 }, { 2, 3 },
    { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 },
};

/* main.bin 0x8a00, indexed by a one-hot pair in the escape path */
static const uint8_t ctx_escape[4] = { 0x0c, 0x0d, 0x08, 0x06 };

/* IWRAM 0x03002db8: top three bits -> (rank << 3) | length */
static const uint8_t sample_vlc[8] = {
    0x01, 0x01, 0x01, 0x01, 0x12, 0x12, 0x23, 0x33
};

typedef struct BitReader {
    const uint8_t *buf;
    int size;
    int pos;                    /* byte offset of the next word */
    uint64_t acc;               /* bits are left-justified in the top 32 */
    int nbits;
} BitReader;

typedef struct ADSAudioContext {
    uint8_t ctx_table[CTX_ENTRIES];
    int preamble_bits;
} ADSAudioContext;

static void br_init(BitReader *br, const uint8_t *buf, int size)
{
    br->buf   = buf;
    br->size  = size & ~3;
    br->pos   = 0;
    br->acc   = 0;
    br->nbits = 0;
}

/* Words are little-endian and consumed most significant bit first. */
static void br_fill(BitReader *br)
{
    while (br->nbits <= 32 && br->pos + 4 <= br->size) {
        br->acc  |= (uint64_t)AV_RL32(br->buf + br->pos) << (32 - br->nbits);
        br->pos  += 4;
        br->nbits += 32;
    }
}

static unsigned br_peek(BitReader *br, int n)
{
    if (br->nbits < n)
        br_fill(br);
    return (unsigned)(br->acc >> (64 - n));
}

static void br_skip(BitReader *br, int n)
{
    br->acc  <<= n;
    br->nbits -= n;
}

static int br_exhausted(const BitReader *br)
{
    return br->nbits <= 0 && br->pos + 4 > br->size;
}

/**
 * Decode the 64-entry context table, from the builder at
 * 0x02004fe0..0x020050f8.
 */
static void decode_ctx_table(BitReader *br, uint8_t *table)
{
    for (int i = 0; i < CTX_ENTRIES; i++) {
        unsigned code = br_peek(br, 4);
        unsigned a, b, sel, mask, lo, hi, f2, f3;

        if (code < 12) {
            br_skip(br, ctx_vlc[code][1]);
            table[i] = ctx_palette[ctx_vlc[code][0]];
            continue;
        }

        br_skip(br, 2);                         /* the '11' escape prefix */
        a   = br_peek(br, 2); br_skip(br, 2);
        b   = br_peek(br, 2); br_skip(br, 2);
        sel = br_peek(br, 1); br_skip(br, 1);

        /* 0x02005098 builds a one-hot value out of the two fields */
        mask = (1u << a) | (1u << b);
        lo   = ctx_escape[mask & 3];
        hi   = ctx_escape[mask >> 2];
        if (sel) {
            f2 = hi >> 2;                       /* 0x020050b2: swapped */
            f3 = lo & 3;
        } else {
            f2 = lo & 3;                        /* 0x020050cc */
            f3 = hi >> 2;
        }
        table[i] = a | (b << 2) | (f2 << 4) | (f3 << 6);
    }
}

/** Sample loop, from IWRAM 0x03002b4c. */
static int decode_samples(BitReader *br, const uint8_t *ctab,
                          uint8_t *dst, int max_samples)
{
    int step = 0, prev = 0, h0 = 0, h1 = 0;
    int f0 = 0, f1 = 0, f2 = 0;
    int ctx = 0, s0 = 0, s1 = 0;
    int n = 0;

    while (n < max_samples && !br_exhausted(br)) {
        unsigned vlc = sample_vlc[br_peek(br, 3)];
        int sym, mag, delta, acc, newh, out, sb, sgn, r4;

        br_skip(br, vlc & 0xf);
        sym = (ctab[ctx] >> (vlc >> 3)) & 3;
        ctx = sym | ((ctx << 2) & 0x3f);

        /* magnitude and step live in a log domain: bits 0-6 are the mantissa
         * and bits 7-10 the exponent */
        r4 = (step >> 8) + 0x74;
        if (ctx & 1)
            r4 += 0xf9;
        mag = (((r4 & 0x7f) | 0x80) << 7) >> (0xe - ((r4 >> 7) & 0xf));
        if (ctx & 2)
            mag = -mag;

        delta = step >> 6;
        if (ctx & 1) {
            int t = 0x36c0 - delta + 0x20;
            int nf = delta + (t >> 5);
            if (delta > 0x12e0)
                nf = 0x1400;
            step += nf + (-step >> 6);
        } else {
            int t = -(delta + 0x2c0);
            int nf = delta + (t >> 5);
            if (delta < 0x248)
                nf = 0x220;
            step += nf + (-step >> 6);
        }

        acc  = ((h1 * (f1 >> 2)) >> 11) + ((h0 * (f0 >> 2)) >> 11) +
               ((prev * f2) >> 11);
        newh = mag + (acc >> 1);
        h1   = h0;
        if (!newh)
            newh = 1;
        h0 = newh;

        /* round towards zero, then drop the fixed-point tail */
        out = (newh + (int)((unsigned)(newh >> 31) >> 26)) >> 6;
        dst[n++] = av_clip_int8(out) + 0x80;

        sb  = mag + ((prev * f2) >> 12);
        sgn = (unsigned)sb >> 31;
        f0 -= f0 >> 8;
        f1 -= f1 >> 7;
        if (sb) {
            int t = (sgn == s0) ? -f0 : f0;

            f0 += (sgn == s0) ? 0xc0 : -0xc0;
            if ((unsigned)(t + 0x1fffffff) <= 0x3fffffff)
                f1 += t >> 5;
            else
                f1 += t < 0 ? -0x100 : 0xff;
            f1 += (sgn == s1) ? 0x80 : -0x80;
        }
        if (f0 > 0x3c00 - f1)
            f0 = 0x3c00 - f1;

        f2 -= f2 >> 8;
        f2 += ((mag ^ prev) >= 0) ? 0x20 : -0x20;

        s1   = s0;
        s0   = sgn;
        prev = mag;
    }

    return n;
}

static int ads_audio_decode(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame_ptr, AVPacket *avpkt)
{
    ADSAudioContext *s = avctx->priv_data;
    BitReader br;
    int ret, n;

    if (avpkt->size < 8)
        return AVERROR_INVALIDDATA;

    br_init(&br, avpkt->data, avpkt->size);

    /* A block opens with a short preamble whose meaning is not yet known;
     * 25 bits is what hardware was measured consuming. */
    br_peek(&br, 32);
    br_skip(&br, s->preamble_bits);

    decode_ctx_table(&br, s->ctx_table);

    /* Every sample costs at least one bit, so this cannot underestimate. */
    frame->nb_samples = avpkt->size * 8;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    n = decode_samples(&br, s->ctx_table, frame->data[0],
                       frame->nb_samples);
    if (n <= 0)
        return AVERROR_INVALIDDATA;

    frame->nb_samples = n;
    *got_frame_ptr    = 1;

    return avpkt->size;
}

static av_cold int ads_audio_init(AVCodecContext *avctx)
{
    ADSAudioContext *s = avctx->priv_data;

    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    avctx->sample_fmt  = AV_SAMPLE_FMT_U8;
    s->preamble_bits   = 25;

    return 0;
}

const FFCodec ff_ads_gba_audio_decoder = {
    .p.name         = "ads_gba_audio",
    CODEC_LONG_NAME("ADS-era GBA Video audio (Majesco)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ADS_GBA_AUDIO,
    .priv_data_size = sizeof(ADSAudioContext),
    .init           = ads_audio_init,
    FF_CODEC_DECODE_CB(ads_audio_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_U8),
};
