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
 * A block is [flag][initial level][64 context-table entries][samples]. Bits
 * are read from 32-bit little-endian words, most significant bit first, and a
 * block always starts on a word boundary.
 *
 * Each sample is a 1-3 bit VLC giving a rank, which a per-context permutation
 * turns into a 2-bit symbol: bit 0 selects a magnitude roughly three times
 * larger, bit 1 negates. The context is a shift register of the last three
 * symbols. Magnitude and step size are carried in a log domain, and the
 * predictor is a three-tap leaky filter.
 *
 * Both cartridge lineages share this codec; they differ only in how the
 * context table is coded. Dragon Ball GT (ADS) uses a VLC over a seven-value
 * palette with an escape; Dora the Explorer (Hydrogen) always sends the escape
 * form, a flat five bits per entry. The Hydrogen variant is what pinned the
 * sample loop down: its decoder sits in the clear as ARM code in the ROM, so
 * the loop below is a direct transcription of it, and it decodes that cart's
 * audio to a clean waveform. See doc/gba_video_ads.md.
 */

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "mathops.h"

#define CTX_ENTRIES 64

/* Level tracking starts here when a block does not carry an initial value. */
#define STEP_DEFAULT 0x8800

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

/** Coding of the per-block context table; see the file comment. */
enum CtxCoding {
    CTX_CODING_VLC,             /* ADS / Dragon Ball GT */
    CTX_CODING_FLAT,            /* Hydrogen / Dora */
};

typedef struct ADSAudioContext {
    uint8_t ctx_table[CTX_ENTRIES];
    int coding;

    /* Filter state. It survives across blocks: a block resets it only when its
     * flag bit says so. */
    int step, prev, h0, h1;
    int f0, f1, f2;
    int s0, s1;
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
 * Decode the 64-entry context table, from the ADS builder at
 * 0x02004fe0..0x020050f8 and the Hydrogen one at 0x08005ed4.
 */
static void decode_ctx_table(BitReader *br, uint8_t *table, int coding)
{
    for (int i = 0; i < CTX_ENTRIES; i++) {
        unsigned a, b, sel, mask, lo, hi, f2, f3;

        if (coding == CTX_CODING_VLC) {
            unsigned code = br_peek(br, 4);

            if (code < 12) {
                br_skip(br, ctx_vlc[code][1]);
                table[i] = ctx_palette[ctx_vlc[code][0]];
                continue;
            }
            br_skip(br, 2);                     /* the '11' escape prefix */
        }

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

/** Sample loop, transcribed from Hydrogen IWRAM 0x03002ad0. */
static int decode_samples(ADSAudioContext *s, BitReader *br,
                          uint8_t *dst, int max_samples)
{
    const uint8_t *ctab = s->ctx_table;
    int step = s->step, prev = s->prev, h0 = s->h0, h1 = s->h1;
    int f0 = s->f0, f1 = s->f1, f2 = s->f2;
    int s0 = s->s0, s1 = s->s1;
    int ctx = 0;
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
            /* 0x03002c58: the bound is 0x4000 - 1, not a 32-bit range */
            if ((unsigned)(t + 0x1fff) <= 0x3fff)
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

    s->step = step; s->prev = prev; s->h0 = h0; s->h1 = h1;
    s->f0   = f0;   s->f1   = f1;   s->f2 = f2;
    s->s0   = s0;   s->s1   = s1;

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

    /* One flag bit: clear means the block restarts the filter and carries a
     * signed 24-bit initial level, set means it continues the previous one. */
    if (!br_peek(&br, 1)) {
        br_skip(&br, 1);
        s->step = sign_extend(br_peek(&br, 24), 24);
        br_skip(&br, 24);
        s->prev = 1;
        s->h0   = 1;
        s->f0   = 0;
        s->f2   = 0;
        s->s0   = 0;
        s->s1   = 0;
    } else {
        br_skip(&br, 1);
    }

    decode_ctx_table(&br, s->ctx_table, s->coding);

    /* A block runs for a fixed number of samples that it does not state, so
     * take the demuxer's word for it; failing that, every sample costs at
     * least one bit, which cannot underestimate. */
    frame->nb_samples = avpkt->duration > 0 ? avpkt->duration : avpkt->size * 8;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    n = decode_samples(s, &br, frame->data[0], frame->nb_samples);
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

    /* The demuxer names the lineage in a one-byte extradata; without it,
     * assume the ADS carts, which are the ones with an SFCD archive. */
    s->coding = avctx->extradata_size >= 1 ? avctx->extradata[0]
                                           : CTX_CODING_VLC;
    s->step   = STEP_DEFAULT;
    s->prev   = 1;
    s->h0     = 1;

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
