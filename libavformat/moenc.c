/*
 * MO muxer
 * Copyright (c) 2022 Spotlight Deveaux
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

#include "avformat.h"
#include "libavutil/mem.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"
#include "mo.h"
#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/sha.h"
#include "libavcodec/adpcm_data.h"
#include "libavcodec/avcodec.h"

/* ================================================================== *
 *  Clip-certificate ("cc") signing — see big comment at mo_sign_cc.   *
 *  Hard-coded license material recovered from the licensed toolchain  *
 *  (Testing Account license; pc + cc keypair are a matched set).      *
 * ================================================================== */

/* "pc" tag: 1280-bit RSA license signature (verified by the Wii RootPublicKey).
 * The cc modulus below is bytes [1..65) of this license's decrypted payload, so
 * pc and cc MUST be emitted together as a matched pair. */
static const uint8_t mo_pc_signature[160] = {
    0x0d,0x6d,0xe0,0xf2,0xe5,0x88,0xdd,0x29,0x08,0xff,0x3c,0x83,0xbc,0x36,0xf6,0x52,
    0xc4,0x52,0x4c,0xa9,0x0b,0x17,0x99,0x76,0xe4,0xb6,0xb1,0xb5,0xd4,0x36,0x7c,0x93,
    0x78,0x0a,0xcc,0x54,0x97,0x83,0x17,0x67,0x5f,0x2a,0x53,0x8b,0x49,0x9c,0x96,0x80,
    0xf6,0x8d,0xb5,0x9c,0xba,0x49,0x74,0x96,0x0c,0x41,0xa6,0xa0,0x25,0x7f,0x00,0xc2,
    0xe5,0x66,0x73,0xb9,0xff,0x8d,0x60,0xcc,0xa9,0x82,0xe3,0xfc,0xaf,0x16,0x21,0x5f,
    0x60,0x87,0x0e,0x27,0x66,0x41,0x85,0x49,0x53,0x39,0xb4,0x74,0xa5,0x82,0xfc,0x30,
    0xd4,0x6f,0x25,0x8a,0xaa,0xa9,0x2b,0xaa,0xc7,0x0f,0xfc,0xba,0x07,0xc5,0x66,0x6f,
    0xa7,0xc3,0x8e,0x8d,0x54,0x88,0xfd,0x80,0xf7,0x7b,0x30,0x68,0x15,0x5a,0x31,0x46,
    0x48,0x27,0xf5,0x8a,0x10,0xc8,0x45,0xb0,0xe0,0x3c,0xce,0xcd,0x5e,0x87,0x46,0xf8,
    0x41,0xff,0x85,0x50,0x22,0xcd,0x54,0x86,0x36,0xb6,0xfd,0x8d,0x46,0x53,0xd5,0xfb,
};

/* "cc" RSA-512 public modulus N (big-endian) = pc-license payload[1..65). */
static const uint8_t mo_cc_modulus[64] = {
    0xe4,0x95,0x73,0x0d,0x4e,0xca,0xa0,0x47,0x00,0xd3,0xa7,0x3e,0xe6,0x09,0xd3,0x08,
    0x7e,0x9d,0x55,0x58,0xb6,0xbd,0xaa,0x21,0x19,0x78,0x4f,0xe9,0xe7,0xe9,0x68,0x69,
    0x8d,0xf1,0x12,0x5f,0x7b,0xaa,0xcb,0x3f,0x2c,0x45,0x15,0x48,0x4d,0xcb,0x9f,0x5a,
    0x3c,0x0f,0x17,0x03,0x61,0xb7,0x5d,0xc7,0x64,0xe8,0x10,0x32,0x54,0xac,0x51,0x65,
};

/* "cc" RSA-512 private exponent d (big-endian) = .lic blob at offset 0x16C.
 * (Public exponent is 3; signing uses d.) */
static const uint8_t mo_cc_priv_d[64] = {
    0x98,0x63,0xa2,0x08,0xdf,0x31,0xc0,0x2f,0x55,0xe2,0x6f,0x7f,0x44,0x06,0x8c,0xb0,
    0x54,0x68,0xe3,0x90,0x79,0xd3,0xc6,0xc0,0xbb,0xa5,0x8a,0x9b,0xef,0xf0,0xf0,0x45,
    0x1c,0x0e,0x38,0x04,0xe3,0x20,0x88,0x20,0xeb,0x5d,0x42,0x8f,0x50,0xeb,0x06,0x0f,
    0xe2,0x0b,0x84,0x9c,0xfd,0x92,0x52,0x6f,0xae,0xc2,0xb5,0x2b,0x95,0x5d,0xc1,0xeb,
};

/* ---- Minimal fixed-size RSA-512 (m ^ d mod N), 16 x uint32 little-endian ---- */
#define MO_BN_LIMBS 16

typedef struct { uint32_t l[MO_BN_LIMBS];   } mo_bn512;
typedef struct { uint32_t l[2*MO_BN_LIMBS]; } mo_bn1024;

static void mo_bn_from_be(mo_bn512 *r, const uint8_t be[64])
{
    for (int i = 0; i < MO_BN_LIMBS; i++)
        r->l[i] = AV_RB32(be + (MO_BN_LIMBS - 1 - i) * 4);
}

static void mo_bn_to_be(const mo_bn512 *a, uint8_t be[64])
{
    for (int i = 0; i < MO_BN_LIMBS; i++)
        AV_WB32(be + (MO_BN_LIMBS - 1 - i) * 4, a->l[i]);
}

static void mo_bn_mul(mo_bn1024 *r, const mo_bn512 *a, const mo_bn512 *b)
{
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < MO_BN_LIMBS; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < MO_BN_LIMBS; j++) {
            uint64_t cur = (uint64_t)a->l[i] * b->l[j] + r->l[i + j] + carry;
            r->l[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        r->l[i + MO_BN_LIMBS] += (uint32_t)carry;
    }
}

/* r = x (1024-bit) mod n (512-bit), binary long division.
 * rem kept in 17 limbs so the shift-left can momentarily exceed 512 bits. */
static void mo_bn_mod(mo_bn512 *r, const mo_bn1024 *x, const mo_bn512 *n)
{
    uint32_t rem[MO_BN_LIMBS + 1];
    memset(rem, 0, sizeof(rem));

    for (int bit = 2*MO_BN_LIMBS*32 - 1; bit >= 0; bit--) {
        uint32_t carry = 0;
        for (int i = 0; i < MO_BN_LIMBS + 1; i++) {
            uint32_t nc = rem[i] >> 31;
            rem[i] = (rem[i] << 1) | carry;
            carry = nc;
        }
        rem[0] |= (x->l[bit >> 5] >> (bit & 31)) & 1u;

        int ge = rem[MO_BN_LIMBS] != 0;
        if (!ge) {
            ge = 1;
            for (int i = MO_BN_LIMBS - 1; i >= 0; i--)
                if (rem[i] != n->l[i]) { ge = rem[i] > n->l[i]; break; }
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i < MO_BN_LIMBS; i++) {
                uint64_t cur = (uint64_t)rem[i] - n->l[i] - borrow;
                rem[i] = (uint32_t)cur;
                borrow = (cur >> 32) & 1;
            }
            rem[MO_BN_LIMBS] -= (uint32_t)borrow;
        }
    }
    memcpy(r->l, rem, sizeof(r->l));
}

static void mo_bn_mulmod(mo_bn512 *r, const mo_bn512 *a, const mo_bn512 *b,
                         const mo_bn512 *n)
{
    mo_bn1024 prod;
    mo_bn_mul(&prod, a, b);
    mo_bn_mod(r, &prod, n);
}

/* r = base ^ exp mod n  (exp big-endian) */
static void mo_bn_modexp(mo_bn512 *r, const mo_bn512 *base,
                         const uint8_t exp_be[64], const mo_bn512 *n)
{
    mo_bn512 result = {{0}};
    result.l[0] = 1;
    for (int byte = 0; byte < 64; byte++)
        for (int bit = 7; bit >= 0; bit--) {
            mo_bn_mulmod(&result, &result, &result, n);
            if ((exp_be[byte] >> bit) & 1)
                mo_bn_mulmod(&result, &result, base, n);
        }
    *r = result;
}

/* Compute H = SHA1 over the MOC5 header structure:
 *   header[0x08:0x1C]  (TL section + V2 tag-header; excludes magic/size/W/H)
 *   then for each tag from 0x24: its 4-byte tag-header, plus its payload for
 *   EVERY tag except "pc"(0x6370) and "cc"(0x6363).
 * `hdr` is the fully-assembled header (the cc payload may be present but is
 * skipped here, matching the Wii's verification). */
static int mo_cc_metadata_hash(const uint8_t *hdr, int len, uint8_t out[20])
{
    struct AVSHA *sha = av_sha_alloc();
    if (!sha)
        return AVERROR(ENOMEM);
    av_sha_init(sha, 160);

    if (len >= 0x1c)
        av_sha_update(sha, hdr + 0x08, 0x1c - 0x08);

    int o = 0x24;
    while (o + 4 <= len) {
        uint16_t tag = AV_RL16(hdr + o);
        uint16_t wl  = AV_RL16(hdr + o + 2);
        int sz = wl * 4 + 4;
        if (sz < 4 || o + sz > len)
            break;
        av_sha_update(sha, hdr + o, 4);                  /* tag header always */
        /* Payload is hashed for every tag EXCEPT the two signature tags
         * (pc, cc) and the FastAudio/ADPCM config tags.  The SDK omits the
         * audio-config payload from the cc hash for FastAudio (A2/A3) AND for
         * IMA-ADPCM (A8/A9); PCM ('AP') and Vorbis ('AV') ARE hashed.  Verified
         * against MoDump: an A9 clip whose cc hashes the A9 payload is rejected
         * "File is damaged"; excluding it makes MoDump accept the certificate. */
        if (tag != FORMAT_RSA && tag != FORMAT_POSSIBLY_CAPTIONS &&
            tag != FORMAT_FASTAUDIO && tag != FORMAT_FASTAUDIO_STEREO &&
            tag != FORMAT_ADPCM && tag != FORMAT_ADPCM_STEREO)
            av_sha_update(sha, hdr + o + 4, sz - 4);
        if (tag == FORMAT_HEADER_DONE)
            break;
        o += sz;
    }

    av_sha_final(sha, out);
    av_free(sha);
    return 0;
}

/* Produce the 64-byte "cc" tag payload signing header buffer `hdr`.
 *   m (big-endian, 64 bytes): 01 | nonce | 06 02 | H[20] | nonce/pad...
 *   cc = m ^ d mod N
 * The nonce bytes are not validated by the player, so they are fixed (0) for
 * deterministic output. */
static int mo_sign_cc(const uint8_t *hdr, int len, uint8_t cc_out[64])
{
    uint8_t h[20], msg[64];
    int ret = mo_cc_metadata_hash(hdr, len, h);
    if (ret < 0)
        return ret;

    memset(msg, 0, sizeof(msg));
    msg[0] = 0x01;                  /* block type        */
    msg[2] = 0x06; msg[3] = 0x02;   /* certifier marker  */
    memcpy(msg + 4, h, 20);         /* SHA-1 metadata hash */

    mo_bn512 N, M, S;
    mo_bn_from_be(&N, mo_cc_modulus);
    mo_bn_from_be(&M, msg);
    mo_bn_modexp(&S, &M, mo_cc_priv_d, &N);
    mo_bn_to_be(&S, cc_out);
    return 0;
}

/* Wii MobiClip ADPCM block layout (matches modec.c demuxer + Kaitai spec):
 *   For each subframe (256 samples per channel) and for each channel:
 *     u16le step_index (sign-extended)
 *     u16le predictor   (sign-extended)
 *     128 bytes of nibble pairs (low nibble = even sample, high nibble = odd)
 *   Total per subframe per channel: 132 bytes. */
#define WII_ADPCM_SAMPLES_PER_BLOCK   256
#define WII_ADPCM_BYTES_PER_BLOCK     132

typedef struct MoAdpcmChannel {
    int predictor;
    int step_index;
} MoAdpcmChannel;

/* Maximum keyframe entries pre-allocated in the header.
 * Each entry is 8 bytes (chunk_offset u32 + frame_index u32).
 * 4096 entries => 32 KB header overhead, supports ~68 min at 1fps GOP or
 * sub-sampled for denser GOPs. */
#define KI_MAX_ENTRIES 512

typedef struct MoMuxContext {
    const AVClass *class;
    int header_size;

    /* Offset of the TL frame_count field within the file, so we can
     * seek back in the trailer and write the final frame count. */
    int64_t frame_count_offset;
    int64_t max_frame_size_offset;
    uint32_t max_chunk_size;
    uint32_t current_chunk_size;

    /* Keyframe index — built during encoding, patched in the trailer */
    int64_t  ki_length_offset; /* file offset of the KI u16 length field */
    uint32_t *ki_kf_offsets;   /* chunk byte offsets for each keyframe */
    uint32_t *ki_kf_frames;    /* frame indices for each keyframe */
    int       ki_count;        /* keyframes recorded so far */

    /* Buffered video packet waiting for audio to pair with */
    uint8_t *video_buf;
    int      video_size;
    int64_t  video_pts;
    int      video_is_key;     /* AV_PKT_FLAG_KEY set on pending video pkt */

    /* Buffered audio packet (non-PCM-as-ADPCM modes: Vorbis, FastAudio) */
    uint8_t *audio_buf;
    int      audio_size;
    int64_t  audio_pts;

    /* Vorbis packet FIFO: each chunk carries exactly ONE Vorbis packet
     * (retail .mo layout; the demuxer reads chunk audio as a single packet).
     * Concatenating packets loses everything after the first on decode. */
    uint8_t **vq_data;
    int      *vq_size;
    int       vq_count;
    int       vq_cap;
    uint16_t  vorbis_seq;      /* running [seq] counter for retail AV sections */

    /* PCM->Wii ADPCM queue (used when adpcm_mode is set) */
    int adpcm_mode;            /* 1 if repacking PCM into Wii ADPCM blocks */
    int channels;              /* set on first packet */
    int16_t *pcm_buf;          /* interleaved s16 samples */
    int pcm_samples;           /* sample frames (per channel) currently buffered */
    int pcm_capacity;          /* sample-frame capacity of pcm_buf */
    int samples_per_chunk_base;
    int samples_per_chunk_rem;
    int samples_per_chunk_acc;
    int adpcm_target_acc;       /* fractional PCM target accumulator */
    int adpcm_samples_written;  /* decoded ADPCM samples already emitted */
    MoAdpcmChannel adpcm[2];

    int first_video_written;
    int chunk_count;           /* number of video chunks written (= frame count) */
    int audio_codec;           /* resolved: 0=fastaudio, 1=adpcm, 2=pcm, 3=vorbis */
    int mobi_dbg;
    int mobi_muxdbg;
    int64_t file_end;          /* actual end-of-file position after last sequential write */

    /* In-muxer PCM→FastAudio encoder (used when -mo_audio fastaudio and input is PCM_S16LE) */
    AVCodecContext *fa_enc;
    AVFrame        *fa_frame;
    int             fa_active; /* 1 if in-muxer fastaudio encoding is active */

    /* In-muxer PCM→Vorbis encoder (used when -mo_audio vorbis and input is PCM_S16LE),
     * so the user doesn't have to pre-encode with -c:a libvorbis. Output packets
     * are queued into vq_* exactly like externally-supplied Vorbis packets. */
    AVCodecContext *vorbis_enc;
    AVFrame        *vorbis_frame;
    int             vorbis_from_pcm;

    /* Set by mo_write_trailer before its final flush_chunk so the in-muxer
     * audio encoders drain ALL remaining buffered PCM into the last chunk
     * instead of just one video-frame's worth (otherwise the trailing ~100ms
     * of audio that outlasts the video is dropped). */
    int             draining;
} MoMuxContext;

static int append_pcm(MoMuxContext *mo, const int16_t *data, int sample_frames)
{
    int needed = mo->pcm_samples + sample_frames;
    if (needed > mo->pcm_capacity) {
        int new_cap = mo->pcm_capacity ? mo->pcm_capacity * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        int16_t *nb = av_realloc(mo->pcm_buf,
                                  (size_t)new_cap * mo->channels * sizeof(int16_t));
        if (!nb)
            return AVERROR(ENOMEM);
        mo->pcm_buf = nb;
        mo->pcm_capacity = new_cap;
    }
    memcpy(mo->pcm_buf + mo->pcm_samples * mo->channels, data,
           (size_t)sample_frames * mo->channels * sizeof(int16_t));
    mo->pcm_samples += sample_frames;
    return 0;
}

/* IMA ADPCM compression matching the standard expand_nibble used by the
 * Wii MobiClip decoder (shift=3, signed predictor stored as s16). */
/* IMA ADPCM compression matching the standard expand_nibble used by the
 * Wii MobiClip decoder (shift=3, signed predictor stored as s16). */
static uint8_t mo_ima_compress(MoAdpcmChannel *c, int16_t sample)
{
    int delta = sample - c->predictor;
    int step  = ff_adpcm_step_table[c->step_index];
    int nibble = FFMIN(7, abs(delta) * 4 / step) | ((delta < 0) ? 8 : 0);
    int diff  = ((2 * (nibble & 7) + 1) * step) >> 3;
    int pred  = c->predictor + ((nibble & 8) ? -diff : diff);

    if (pred >  32767) pred =  32767;
    if (pred < -32768) pred = -32768;
    c->predictor  = pred;
    c->step_index = av_clip(c->step_index + ff_adpcm_index_table[nibble], 0, 88);
    return (uint8_t)nibble;
}

/* Encode `subframes` Wii MobiClip ADPCM blocks into `out`. If the PCM
 * buffer doesn't hold enough samples, the tail is padded with silence so the
 * decoder always sees a full 132-byte/channel block per subframe.
 * Returns bytes written. */
static int encode_adpcm_blocks(MoMuxContext *mo, uint8_t *out, int subframes)
{
    int written = 0;
    int available_samples = mo->pcm_samples;

    for (int sf = 0; sf < subframes; sf++) {
        for (int ch = 0; ch < mo->channels; ch++) {
            MoAdpcmChannel *c = &mo->adpcm[ch];
            out[written++] = c->step_index & 0xFF;
            out[written++] = (c->step_index >> 8) & 0xFF;
            out[written++] = c->predictor & 0xFF;
            out[written++] = (c->predictor >> 8) & 0xFF;

            int sf_start = sf * WII_ADPCM_SAMPLES_PER_BLOCK;
            for (int n = 0; n < WII_ADPCM_SAMPLES_PER_BLOCK; n += 2) {
                int16_t s0 = 0, s1 = 0;
                if (sf_start + n < available_samples) {
                    s0 = mo->pcm_buf[(sf_start + n) * mo->channels + ch];
                }
                if (sf_start + n + 1 < available_samples) {
                    s1 = mo->pcm_buf[(sf_start + n + 1) * mo->channels + ch];
                }
                uint8_t lo = mo_ima_compress(c, s0);
                uint8_t hi = mo_ima_compress(c, s1);
                out[written++] = (hi << 4) | lo;
            }
        }
    }

    int consumed_samples = subframes * WII_ADPCM_SAMPLES_PER_BLOCK;
    if (consumed_samples >= mo->pcm_samples) {
        mo->pcm_samples = 0;
    } else {
        int remaining = mo->pcm_samples - consumed_samples;
        memmove(mo->pcm_buf,
                mo->pcm_buf + consumed_samples * mo->channels,
                (size_t)remaining * mo->channels * sizeof(int16_t));
        mo->pcm_samples = remaining;
    }
    return written;
}

static int mo_write_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    MoMuxContext *mo = s->priv_data;

    mo->first_video_written = 0;

    int no_audio = (mo->audio_codec == 4);
    int min_streams = no_audio ? 1 : 2;

    if (s->nb_streams < min_streams || s->nb_streams > 2) {
        av_log(s, AV_LOG_ERROR, "Exactly %s stream%s (video%s) %s required!\n",
               no_audio ? "one" : "two",
               no_audio ? "" : "s",
               no_audio ? "" : " + audio",
               no_audio ? "is" : "are");
        return AVERROR(EINVAL);
    }

    AVStream *vst = s->streams[0];
    AVCodecParameters *vpars = vst->codecpar;
    if (vpars->codec_type != AVMEDIA_TYPE_VIDEO ||
        (vpars->codec_id != AV_CODEC_ID_MOBICLIP &&
         vpars->codec_id != AV_CODEC_ID_H264)) {
        av_log(s, AV_LOG_ERROR, "Stream 0 must be MobiClip or H.264 video!\n");
        return AVERROR(EINVAL);
    }

    /* The x264 encoder with -mobiclip 1 outputs standard H.264 format
     * (Annex-B wrapped NALs), not pure MobiClip. Keep it as H.264 for decoding. */
    // vpars->codec_id = AV_CODEC_ID_MOBICLIP;

    /* Default to 624x352 (standard Wii channel resolution) if dimensions not set */
    if (vpars->width <= 0) vpars->width = 624;
    if (vpars->height <= 0) vpars->height = 352;

    /* Warn if input dimensions differ from standard Wii resolution */
    if (vpars->width > 640 || vpars->height > 368) {
        av_log(s, AV_LOG_WARNING,
               "Video is %dx%d — larger than standard Wii resolution (624x352). "
               "Add -vf scale=624:352 to scale for Wii compatibility.\n",
               vpars->width, vpars->height);
    }

    /* The MobiClip decoder processes 16x16 macroblocks and requires both
     * dimensions to be exact multiples of 16. */
    if (vpars->width & 15 || vpars->height & 15) {
        int w16 = (vpars->width  + 15) & ~15;
        int h16 = (vpars->height + 15) & ~15;
        av_log(s, AV_LOG_ERROR,
               "MobiClip requires width and height to be multiples of 16 "
               "(got %dx%d). Scale first, e.g.:\n"
               "  -vf scale=%d:%d\n",
               vpars->width, vpars->height, w16, h16);
        return AVERROR(EINVAL);
    }

    /* Check for audio stream - optional for "no audio" mode */
    AVStream *ast = s->nb_streams > 1 ? s->streams[1] : NULL;
    AVCodecParameters *apars = ast ? ast->codecpar : NULL;
    int has_audio = ast && apars->codec_type == AVMEDIA_TYPE_AUDIO;

    /* If user explicitly requested no audio, or no audio stream provided,
     * treat as no-audio mode. */
    if (!no_audio) {
        if (!has_audio) {
            av_log(s, AV_LOG_ERROR, "Audio stream required!\n");
            return AVERROR(EINVAL);
        }
        if (apars->codec_id != AV_CODEC_ID_PCM_S16LE &&
            apars->codec_id != AV_CODEC_ID_VORBIS    &&
            apars->codec_id != AV_CODEC_ID_FASTAUDIO) {
            av_log(s, AV_LOG_ERROR,
                   "Unsupported audio codec - use pcm_s16le (default, will be packed "
                   "into Wii MobiClip ADPCM), libvorbis, or fastaudio.\n");
            return AVERROR_PATCHWELCOME;
        }

        /* Decide channels and ADPCM packing mode */
        mo->channels = apars->ch_layout.nb_channels ? apars->ch_layout.nb_channels : 1;
        if (mo->channels < 1 || mo->channels > 2) {
            av_log(s, AV_LOG_ERROR, "Only mono or stereo audio is supported (got %d).\n",
                   mo->channels);
            return AVERROR(EINVAL);
        }
        /* For fastaudio mode, always buffer PCM so we can do in-muxer encoding.
         * For adpcm/pcm modes, also buffer PCM. Vorbis passes through raw. */
        mo->adpcm_mode = (apars->codec_id == AV_CODEC_ID_PCM_S16LE) ? 1 : 0;
        {
            AVRational fr = vst->r_frame_rate;
            if (fr.num <= 0 || fr.den <= 0)
                fr = vst->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0)
                fr = (AVRational){30, 1};
            mo->samples_per_chunk_base = (int)((int64_t)apars->sample_rate * fr.den / fr.num);
            mo->samples_per_chunk_rem  = (int)((int64_t)apars->sample_rate * fr.den % fr.num);
            mo->samples_per_chunk_acc  = 0;
        }

        /* Initialise ADPCM channel state */
        for (int i = 0; i < 2; i++) {
            mo->adpcm[i].predictor = 0;
            mo->adpcm[i].step_index = 0;
        }
    } else {
        mo->channels = 0;
        mo->adpcm_mode = 0;
    }

    /* -------- Build the .mo container header (Kaitai-compliant) -------- */
    avio_wl32(pb, MO_TAG);          /* "MOC5" */
    avio_wl32(pb, 0);               /* header length placeholder */
    mo->header_size = 8;

    /* FORMAT_LENGTH ('TL') */
    avio_wl16(pb, FORMAT_LENGTH);
    avio_wl16(pb, 3);
    {
        /* Prefer r_frame_rate (always set for output streams at write_header
         * time); fall back to avg_frame_rate, then a 30fps default. */
        AVRational fr = vst->r_frame_rate;
        if (fr.num <= 0 || fr.den <= 0)
            fr = vst->avg_frame_rate;
        uint32_t fps_fixed = (fr.num > 0 && fr.den > 0)
            ? (uint32_t)((double)fr.num / fr.den * 256.0 + 0.5)
            : 7680;
        avio_wl32(pb, fps_fixed);
    }
    /* Record offset of frame_count so mo_write_trailer can patch it.
     * Write a 100,000 frame_count placeholder. The demuxer has a bug where it
     * allocates frame_count + 1 BYTES but then treats it as an array of INTs.
     * 100k bytes safely supports ~25k frames. */
    mo->frame_count_offset = avio_tell(pb);
    avio_wl32(pb, 0);                 /* placeholder */
    mo->max_frame_size_offset = avio_tell(pb);
    avio_wl32(pb, 0);
    mo->header_size += 4 + 12;

    /* FORMAT_VIDEO ('V2') */
    avio_wl16(pb, FORMAT_VIDEO);
    avio_wl16(pb, 2);
    if (mo->mobi_dbg)
        av_log(s, AV_LOG_DEBUG, "MOENC: writing V2 width=%d height=%d\n",
               vpars->width, vpars->height);
    avio_wl32(pb, vpars->width);
    avio_wl32(pb, vpars->height);
    mo->header_size += 4 + 8;

    /* FORMAT_RSA ('pc') — 160-byte RSA-1280 license signature, verified on the
     * Wii against the SDK RootPublicKey. This MUST be the same license whose
     * keypair signs the 'cc' tag (mo_cc_modulus / mo_cc_priv_d), since the Wii
     * derives the cc verification modulus from this pc payload. */
    {
        avio_wl16(pb, FORMAT_RSA);
        avio_wl16(pb, 40);
        avio_write(pb, mo_pc_signature, 160);
        mo->header_size += 4 + 160;
    }

    /* Resolve the on-disk audio codec early — it decides whether the 'P\xc6'
     * descriptor is emitted (see below). 0=fastaudio 1=adpcm 2=pcm 3=vorbis 4=no audio. */
    int final_codec = mo->audio_codec;
    if (final_codec == -1) {
        if (apars->codec_id == AV_CODEC_ID_FASTAUDIO) final_codec = 0;
        else if (apars->codec_id == AV_CODEC_ID_PCM_S16LE) final_codec = 1;
        else if (apars->codec_id == AV_CODEC_ID_VORBIS) final_codec = 3;
    }
    /* no_audio is already defined at the start of mo_write_header */

    /* FORMAT_UNKNOWN_AUDIO ('P' 0xc6) — a fixed 8-byte audio descriptor that
     * SDK-produced .mo files carry between 'pc' and the audio-format tag.
     * Its presence is CODEC-DEPENDENT: retail FastAudio (A2/A3) and PCM ('AP')
     * clips include it (payload constant (959, 32)); retail IMA-ADPCM clips
     * (testF, 'A9') OMIT it entirely.  Emitting it for ADPCM produces a header
     * layout the SDK does not expect, so skip it there.  When present it is
     * hashed into the cc signature like any other non-signature tag.
     * For no_audio, also skip this descriptor.
     * Retail Vorbis ('AV') clips (e.g. Excitebike Info Video) also OMIT it: the
     * header goes straight 'pc' -> 'AV'.  Emitting it for Vorbis produces a tag
     * layout the SDK/Wii does not expect, so skip it for codec 3 as well. */
    if (!no_audio && final_codec != 1 && final_codec != 3) {
        avio_wl16(pb, FORMAT_UNKNOWN_AUDIO);
        avio_wl16(pb, 2);
        avio_wl32(pb, 959);
        avio_wl32(pb, 32);
        mo->header_size += 4 + 8;
    }

    /* Audio chunk header */
    if (final_codec == -1) {
        av_log(s, AV_LOG_ERROR, "Unsupported audio codec\n");
        return AVERROR_PATCHWELCOME;
    }
    mo->audio_codec = final_codec;
    /* adpcm(1) and pcm(2) both accept PCM_S16LE and buffer in pcm_buf.
     * fastaudio(0) with PCM input also buffers in pcm_buf for in-muxer encoding.
     * no_audio (4) has no audio processing. */
    mo->adpcm_mode = (!no_audio && (final_codec == 1 || final_codec == 2 ||
                      (final_codec == 0 && apars->codec_id == AV_CODEC_ID_PCM_S16LE))) ? 1 : 0;

    switch (final_codec) {
    case 1:
    case 2:
        /* Pack as Wii MobiClip ADPCM or PCM in the file */
        avio_w8(pb, 'A');
        if (final_codec == 1)
            avio_w8(pb, (mo->channels == 2) ? '9' : '8');
        else
            avio_w8(pb, 'P');
        avio_wl16(pb, 2);
        avio_wl32(pb, apars->sample_rate);
        avio_wl32(pb, mo->channels);
        mo->header_size += 4 + 8;
        break;

    case 0:
        avio_wl16(pb, (mo->channels == 2) ? FORMAT_FASTAUDIO_STEREO : FORMAT_FASTAUDIO);
        avio_wl16(pb, 2);
        avio_wl32(pb, apars->sample_rate);
        avio_wl32(pb, mo->channels);
        mo->header_size += 4 + 8;
        break;

    case 3: {
        avio_wl16(pb, FORMAT_VORBIS);

        const uint8_t *ed;
        int ed_size;

        if (apars->codec_id == AV_CODEC_ID_VORBIS) {
            /* Audio is already Vorbis: use its extradata, pass packets through. */
            if (!apars->extradata || apars->extradata_size < 6) {
                av_log(s, AV_LOG_ERROR, "Vorbis stream missing extradata.\n");
                return AVERROR(EINVAL);
            }
            ed = apars->extradata;
            ed_size = apars->extradata_size;
        } else if (apars->codec_id == AV_CODEC_ID_PCM_S16LE) {
            /* In-muxer PCM->Vorbis: open libvorbis and use its extradata so
             * `-mo_audio vorbis` works without an explicit -c:a libvorbis. */
            const AVCodec *vc = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
            if (!vc) {
                av_log(s, AV_LOG_ERROR, "No Vorbis encoder available "
                       "(build without libvorbis?).\n");
                return AVERROR_ENCODER_NOT_FOUND;
            }
            mo->vorbis_enc = avcodec_alloc_context3(vc);
            if (!mo->vorbis_enc)
                return AVERROR(ENOMEM);
            av_channel_layout_from_mask(&mo->vorbis_enc->ch_layout, AV_CH_LAYOUT_STEREO);
            mo->vorbis_enc->sample_rate = apars->sample_rate;
            mo->vorbis_enc->sample_fmt  = AV_SAMPLE_FMT_FLTP;
            mo->vorbis_enc->bit_rate    = 0;          /* VBR */
            mo->vorbis_enc->global_quality = (int)(FF_QP2LAMBDA * 5); /* ~q5 */
            mo->vorbis_enc->flags |= AV_CODEC_FLAG_QSCALE;
            mo->vorbis_enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
            if (avcodec_open2(mo->vorbis_enc, vc, NULL) < 0 ||
                !mo->vorbis_enc->extradata) {
                av_log(s, AV_LOG_ERROR, "Failed to open in-muxer Vorbis encoder.\n");
                avcodec_free_context(&mo->vorbis_enc);
                return AVERROR(EINVAL);
            }
            mo->vorbis_frame = av_frame_alloc();
            if (!mo->vorbis_frame) {
                avcodec_free_context(&mo->vorbis_enc);
                return AVERROR(ENOMEM);
            }
            mo->vorbis_frame->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_copy(&mo->vorbis_frame->ch_layout, &mo->vorbis_enc->ch_layout);
            mo->vorbis_from_pcm = 1;
            ed = mo->vorbis_enc->extradata;
            ed_size = mo->vorbis_enc->extradata_size;
        } else {
            av_log(s, AV_LOG_ERROR, "-mo_audio vorbis needs PCM or Vorbis input.\n");
            return AVERROR(EINVAL);
        }
        int nheaders = ed[0] + 1;
        if (nheaders < 2)
            return AVERROR(EINVAL);

        int offset = 1;
        int lace_sizes[2] = {0, 0};
        for (int h = 0; h < 2; h++) {
            int sz = 0;
            while (offset < ed_size && ed[offset] == 255) { sz += 255; offset++; }
            if (offset >= ed_size) return AVERROR(EINVAL);
            sz += ed[offset++];
            lace_sizes[h] = sz;
        }
        int p1_size = lace_sizes[0];
        int p2_size = lace_sizes[1];
        int p3_size = ed_size - offset - p1_size - p2_size;
        if (p3_size < 0) return AVERROR(EINVAL);
        const uint8_t *p1 = ed + offset;
        const uint8_t *p2 = p1 + p1_size;
        const uint8_t *p3 = p2 + p2_size;

        int payload_bytes = 4 + p1_size + 4 + p2_size + 4 + p3_size;
        int padded = (payload_bytes + 3) & ~3;
        avio_wl16(pb, (uint16_t)(padded / 4));

        avio_wl32(pb, p1_size); avio_write(pb, p1, p1_size);
        avio_wl32(pb, p2_size); avio_write(pb, p2, p2_size);
        avio_wl32(pb, p3_size); avio_write(pb, p3, p3_size);

        for (int i = payload_bytes; i < padded; i++)
            avio_w8(pb, 0);

        mo->header_size += 4 + padded;
        break;
    }
    case 4:
        /* No audio - write FORMAT_NO_AUDIO marker */
        avio_wl16(pb, FORMAT_NO_AUDIO);
        avio_wl16(pb, 0);
        mo->header_size += 4;
        break;
    default:
        av_assert0(0);
    }

    /* Open in-muxer fastaudio encoder when fastaudio mode + PCM input */
    if (final_codec == 0 && apars->codec_id == AV_CODEC_ID_PCM_S16LE) {
        const AVCodec *fac = avcodec_find_encoder(AV_CODEC_ID_FASTAUDIO);
        if (!fac) {
            av_log(s, AV_LOG_ERROR, "fastaudio encoder not found\n");
            return AVERROR_ENCODER_NOT_FOUND;
        }
        mo->fa_enc = avcodec_alloc_context3(fac);
        if (!mo->fa_enc)
            return AVERROR(ENOMEM);
        av_channel_layout_copy(&mo->fa_enc->ch_layout, &apars->ch_layout);
        mo->fa_enc->sample_rate = apars->sample_rate;
        mo->fa_enc->sample_fmt  = AV_SAMPLE_FMT_FLTP;
        mo->fa_enc->frame_size  = 256;
        int ret2 = avcodec_open2(mo->fa_enc, fac, NULL);
        if (ret2 < 0) {
            avcodec_free_context(&mo->fa_enc);
            av_log(s, AV_LOG_ERROR, "Failed to open fastaudio encoder\n");
            return ret2;
        }
        mo->fa_frame = av_frame_alloc();
        if (!mo->fa_frame) {
            avcodec_free_context(&mo->fa_enc);
            return AVERROR(ENOMEM);
        }
        mo->fa_frame->nb_samples     = 256;
        mo->fa_frame->format         = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&mo->fa_frame->ch_layout, &apars->ch_layout);
        if (av_frame_get_buffer(mo->fa_frame, 0) < 0) {
            av_frame_free(&mo->fa_frame);
            avcodec_free_context(&mo->fa_enc);
            return AVERROR(ENOMEM);
        }
        mo->fa_active = 1;
    }

    mo->ki_kf_offsets = av_malloc(512 * sizeof(uint32_t));
    mo->ki_kf_frames  = av_malloc(512 * sizeof(uint32_t));
    if (!mo->ki_kf_offsets || !mo->ki_kf_frames) {
        av_freep(&mo->ki_kf_offsets);
        av_freep(&mo->ki_kf_frames);
        return AVERROR(ENOMEM);
    }
    mo->ki_count = 0;

    mo->ki_length_offset = avio_tell(pb);

    /* KI placeholder — 4 bytes. Real KI entries + cc + HE written in trailer. */
    avio_wl16(pb, FORMAT_KEYINDEX);
    avio_wl16(pb, 0);

    /* header_size is patched in write_trailer after KI/cc/HE are inserted. */
    int64_t final_header_pos = avio_tell(pb);
    avio_seek(pb, 4, SEEK_SET);
    avio_wl32(pb, (uint32_t)(final_header_pos - 8));
    avio_seek(pb, final_header_pos, SEEK_SET);

    return 0;
}

static int flush_chunk(AVFormatContext *s)
{
    MoMuxContext *mo = s->priv_data;
    AVIOContext  *pb = s->pb;

    if (!mo->video_buf)
        return 0;

    int is_vorbis = (mo->audio_codec == 3);
    int no_audio = (mo->audio_codec == 4);

    uint8_t *audio_payload = NULL;
    int audio_payload_size = 0;   /* bytes the encoder writes for audio */
    int audio_chunk_size = 0;     /* value reported via chunk_size header */
    int free_audio_payload = 0;

    if (no_audio) {
        /* No audio - leave audio_payload_size = 0 */
    } else if (mo->adpcm_mode && mo->audio_codec == 1) {
        /* ADPCM packets decode in fixed 256-sample blocks.  Do not round every
         * video frame up independently: at 30fps/32000Hz that turns 1066.66
         * samples into 1280 samples each frame, making playback slow/choppy.
         * Instead keep a cumulative target and emit only the number of full
         * blocks needed to stay caught up with the video timeline. */
        int target_samples = mo->samples_per_chunk_base;
        {
            AVStream *vst = s->streams[0];
            AVRational fr = vst->r_frame_rate;
            int den;
            if (fr.num <= 0 || fr.den <= 0)
                fr = vst->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0)
                fr = (AVRational){30, 1};
            den = fr.num;
            mo->samples_per_chunk_acc += mo->samples_per_chunk_rem;
            if (mo->samples_per_chunk_acc >= den) {
                target_samples++;
                mo->samples_per_chunk_acc -= den;
            }
        }
        mo->adpcm_target_acc += target_samples;

        int desired_blocks = (mo->adpcm_target_acc + WII_ADPCM_SAMPLES_PER_BLOCK - 1) /
                             WII_ADPCM_SAMPLES_PER_BLOCK;
        int written_blocks = mo->adpcm_samples_written / WII_ADPCM_SAMPLES_PER_BLOCK;
        int subframes = desired_blocks - written_blocks;
        int block_bytes = mo->channels * WII_ADPCM_BYTES_PER_BLOCK;
        int total;

        if (subframes < 0)
            subframes = 0;
         
        /* For ADPCM, always encode at least some audio to ensure non-zero audio chunks.
         * If no audio data available yet, request at least 1 block of silence. */
        if (subframes == 0 && mo->pcm_samples < WII_ADPCM_SAMPLES_PER_BLOCK)
            subframes = 1;
         
        total = subframes * block_bytes;

        if (total > 0) {
            audio_payload = av_malloc(total);
            if (!audio_payload) return AVERROR(ENOMEM);
            free_audio_payload = 1;
            audio_payload_size = encode_adpcm_blocks(mo, audio_payload, subframes);
            audio_chunk_size = audio_payload_size;
            mo->adpcm_samples_written += subframes * WII_ADPCM_SAMPLES_PER_BLOCK;
        }
    } else if (mo->adpcm_mode && mo->audio_codec == 2) {
        /* PCM: emit exactly the per-video-frame sample count (sample_rate/fps,
         * fractional accumulator) so MoDump's per-frame audio-sync check passes.
         * Draining all buffered samples gave a lumpy distribution it rejects.
         * Short tails are padded with silence; surplus stays buffered for the
         * next chunk. */
        int target = mo->samples_per_chunk_base;
        {
            AVStream *vst = s->streams[0];
            AVRational fr = vst->r_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = vst->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = (AVRational){30, 1};
            mo->samples_per_chunk_acc += mo->samples_per_chunk_rem;
            if (mo->samples_per_chunk_acc >= fr.num) {
                target++;
                mo->samples_per_chunk_acc -= fr.num;
            }
        }
        int bytes = target * mo->channels * (int)sizeof(int16_t);
        audio_payload = av_mallocz(bytes);
        if (!audio_payload) return AVERROR(ENOMEM);
        free_audio_payload = 1;
        int avail = FFMIN(target, mo->pcm_samples);
        memcpy(audio_payload, mo->pcm_buf,
               (size_t)avail * mo->channels * sizeof(int16_t));
        audio_payload_size = bytes;
        audio_chunk_size = bytes;
        if (avail < mo->pcm_samples) {
            int rem = mo->pcm_samples - avail;
            memmove(mo->pcm_buf, mo->pcm_buf + avail * mo->channels,
                    (size_t)rem * mo->channels * sizeof(int16_t));
            mo->pcm_samples = rem;
        } else {
            mo->pcm_samples = 0;
        }
    } else if (is_vorbis && mo->vq_count > 0) {
        /* Retail Wii .mo Vorbis ('AV') section format — matches Nintendo SDK
         * output (Excitebike Info Video, basic.mo):
         *   [LE16 seq][LE16 sample_offset] followed by this chunk's Vorbis
         *   packet(s) concatenated with NO size table.  The Wii's Tremor
         *   self-delimits each packet by consuming exactly its bits (our patched
         *   vorbis decoder does the same).  The section is zero-padded to a
         *   4-byte multiple and the chunk's trailing alignment pad is folded into
         *   chunk_size (see fold_audio below), exactly like retail.
         *
         * sample_offset is 0: our audio is encoded frame-synchronously (the
         * in-muxer Vorbis encoder is fed per video frame), so each section begins
         * at its video frame's sample position.  seq is a running section index
         * (retail uses it for A/V book-keeping; the demuxer keys only on
         * seq != 0xFFFF to pick this retail layout).
         *
         * NOTE: this replaced an earlier custom [0xFFFF][num][size][data] layout
         * that a real Wii cannot parse (it would read 0xFFFF as seq). */
        int npkts = mo->vq_count;
        int concat = 0;
        for (int i = 0; i < npkts; i++)
            concat += mo->vq_size[i];
        int payload_bytes = 4 + concat;          /* [seq][soff] + packets */
        int padded = (payload_bytes + 3) & ~3;   /* 4-align the section */

        uint8_t *mp = av_mallocz(padded);
        if (!mp) return AVERROR(ENOMEM);
        free_audio_payload = 1;

        AV_WL16(mp,     mo->vorbis_seq);
        AV_WL16(mp + 2, 0);                       /* sample_offset (frame-locked) */
        int off = 4;
        for (int i = 0; i < npkts; i++) {
            memcpy(mp + off, mo->vq_data[i], mo->vq_size[i]);
            off += mo->vq_size[i];
            av_free(mo->vq_data[i]);
        }
        mo->vorbis_seq += npkts;
        mo->vq_count = 0;

        audio_payload      = mp;
        audio_payload_size = padded;
        audio_chunk_size   = padded;
    } else if (!is_vorbis && mo->audio_buf && mo->audio_size > 0) {
        audio_payload = mo->audio_buf;
        audio_payload_size = mo->audio_size;
        audio_chunk_size = audio_payload_size;
    }

    /* In-muxer PCM→FastAudio encoding.  Emit exactly the number of 256-sample
     * FastAudio frames needed to keep the cumulative audio timeline matched to
     * the video timeline (a fractional sample-per-frame accumulator, identical
     * to the ADPCM path).  Draining all buffered samples instead produced a
     * lumpy 4/8-blocks-per-frame pattern; MoDump requires the steady ~6/7 that
     * sample_rate/fps implies (it validates per-frame audio sample count). The
     * adpcm_target_acc / adpcm_samples_written counters are unused by the
     * FastAudio path, so we reuse them as cumulative target/encoded samples. */
    if (mo->audio_codec == 0 && mo->fa_active && mo->fa_enc) {
        int target_samples = mo->samples_per_chunk_base;
        {
            AVStream *vst = s->streams[0];
            AVRational fr = vst->r_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = vst->avg_frame_rate;
            if (fr.num <= 0 || fr.den <= 0) fr = (AVRational){30, 1};
            mo->samples_per_chunk_acc += mo->samples_per_chunk_rem;
            if (mo->samples_per_chunk_acc >= fr.num) {
                target_samples++;
                mo->samples_per_chunk_acc -= fr.num;
            }
        }
        mo->adpcm_target_acc += target_samples;
        int want_frames = (mo->adpcm_target_acc + 255) / 256;          /* ceil */
        int done_frames = mo->adpcm_samples_written / 256;
        if (mo->draining) {
            /* Final chunk: encode every remaining buffered sample so the audio
             * tail that outlasts the video isn't dropped. */
            int total_frames = done_frames + (mo->pcm_samples + 255) / 256;
            if (total_frames > want_frames)
                want_frames = total_frames;
        }
        int frames_to_encode = want_frames - done_frames;
        if (frames_to_encode < 0)
            frames_to_encode = 0;
        if (frames_to_encode == 0 && mo->pcm_samples < 256)
            frames_to_encode = 1; /* keep at least one (silent) frame flowing */
        mo->adpcm_samples_written += frames_to_encode * 256;

        uint8_t *fa_out = av_malloc((size_t)frames_to_encode * 40 * mo->channels
                                    + AV_INPUT_BUFFER_PADDING_SIZE);
        int fa_out_len = 0;
        if (fa_out) {
            for (int fi = 0; fi < frames_to_encode; fi++) {
                /* Convert 256 s16 samples per channel → float planar */
                for (int ch = 0; ch < mo->channels; ch++) {
                    float *dst = (float *)mo->fa_frame->extended_data[ch];
                    for (int n = 0; n < 256; n++) {
                        int si = fi * 256 + n;
                        int16_t s = (si < mo->pcm_samples)
                                    ? mo->pcm_buf[si * mo->channels + ch]
                                    : 0;
                        dst[n] = s / 32767.0f;
                    }
                }
                mo->fa_frame->nb_samples = 256;
                AVPacket *fa_pkt = av_packet_alloc();
                if (fa_pkt) {
                    int got = 0;
                    if (avcodec_send_frame(mo->fa_enc, mo->fa_frame) >= 0 &&
                        avcodec_receive_packet(mo->fa_enc, fa_pkt) >= 0) {
                        memcpy(fa_out + fa_out_len, fa_pkt->data, fa_pkt->size);
                        fa_out_len += fa_pkt->size;
                        (void)got;
                    }
                    av_packet_free(&fa_pkt);
                }
            }

            /* Remove consumed samples from pcm_buf */
            int consumed = frames_to_encode * 256;
            if (consumed >= mo->pcm_samples) {
                mo->pcm_samples = 0;
            } else {
                int remaining = mo->pcm_samples - consumed;
                memmove(mo->pcm_buf, mo->pcm_buf + consumed * mo->channels,
                        (size_t)remaining * mo->channels * sizeof(int16_t));
                mo->pcm_samples = remaining;
            }

            if (fa_out_len > 0) {
                audio_payload      = fa_out;
                audio_payload_size = fa_out_len;
                audio_chunk_size   = fa_out_len;
                free_audio_payload = 1;
            } else {
                av_free(fa_out);
                /* Emit one silent frame as fallback */
                audio_payload = av_mallocz(40 * mo->channels);
                if (audio_payload) {
                    free_audio_payload = 1;
                    audio_payload_size = 40 * mo->channels;
                    audio_chunk_size   = audio_payload_size;
                }
            }
        }
    } else if (mo->audio_codec == 0 && !mo->fa_active && audio_payload_size == 0) {
        /* FastAudio pass-through (encoder output): ensure at least one silent frame */
        audio_payload = av_mallocz(40 * mo->channels);
        if (audio_payload) {
            free_audio_payload = 1;
            audio_payload_size = 40 * mo->channels;
            audio_chunk_size = audio_payload_size;
        }
    }

    /* Calculate payload sizes and alignment padding.
     * The demuxer expects each chunk (video + audio) to end at a 4-byte boundary.
     * It computes audio_size = chunk_size - video_size - 8, then reads
     * 1-4 bytes of trailing padding which it appends to the audio packet. */
    int video_pad = (4 - (mo->video_size % 4)) % 4;
    int video_written = mo->video_size + video_pad;

    /* Retail/SDK .mo folds the chunk's 1-4 trailing alignment-pad bytes into the
     * audio tail: the audio is written in full, but chunk_size is reported
     * `trail_pad` bytes short so the next chunk begins immediately after the
     * audio (which is itself 4-aligned for block codecs).  The demuxer rounds
     * the reported audio_size back up using those pad bytes.  MoDump rejects
     * files where chunk_size instead counts the full audio plus a separate pad
     * (our previous behaviour left every chunk 4 bytes too long).  Apply the
     * fold for the block-based audio codecs (FastAudio, ADPCM); PCM/Vorbis keep
     * an explicit pad since the demuxer does not round their audio_size up. */
    /* Block codecs (FastAudio 0, ADPCM 1) fold the trailing pad into the audio
     * tail on EVERY chunk (their block size is large, so the demuxer can round
     * a folded audio_size back up unambiguously).  PCM (2) has a 1-frame block
     * (channels*2 == 4 bytes) equal to the pad granularity, so folding a pad-4
     * chunk would be indistinguishable from a whole frame and lose audio; retail
     * folds only PCM's FIRST chunk (the pad-3 quirk) and leaves later PCM chunks
     * unfolded (audio_size already frame-aligned + a separate pad). */
    /* Vorbis (3): retail folds the pad too.  Our section is already 4-aligned
     * (padded above), so the generic fold path applies whenever the chunk
     * actually carries an audio section. */
    int fold_audio = (mo->audio_codec == 0 || mo->audio_codec == 1) ||
                     (mo->audio_codec == 2 && mo->chunk_count == 0) ||
                     (mo->audio_codec == 3 && audio_chunk_size > 0);

    int64_t chunk_start = avio_tell(pb);
    uint32_t chunk_full = (uint32_t)(video_written + audio_chunk_size + 8);
    int trail_pad = (int)(4 - ((chunk_start + chunk_full) % 4));
    if (trail_pad == 0) trail_pad = 4;        /* demuxer formula never yields 0 */
    /* SDK quirk verified against retail (basic.mo, testF.mo) and accepted by
     * MoDump: the VERY FIRST chunk reports its folded audio one byte longer
     * (trailing pad 3 instead of 4); every later chunk uses 4.  Replicating
     * this is required — MoDump rejects a first chunk whose audio_size is the
     * "natural" N*block-4. */
    if (fold_audio && mo->chunk_count == 0 && trail_pad == 4)
        trail_pad = 3;
    /* Retail Vorbis ('AV') clips fold by 3 on EVERY audio chunk, not just the
     * first: every chunk_size is ≡ 1 (mod 4) -> the demuxer always recovers a
     * 3-byte pad (verified against the Excitebike Info Video).  Match that so
     * our chunk framing is byte-identical to the SDK's. */
    if (fold_audio && mo->audio_codec == 3 && trail_pad == 4)
        trail_pad = 3;
    uint32_t chunk_size = fold_audio ? chunk_full - trail_pad : chunk_full;
    if (chunk_size > mo->max_chunk_size)
        mo->max_chunk_size = chunk_size;

    /* Record keyframe */
    if (mo->video_is_key && mo->ki_kf_offsets) {
        if (mo->ki_count < 512) {
            mo->ki_kf_offsets[mo->ki_count] = (uint32_t)chunk_start;
            mo->ki_kf_frames [mo->ki_count] = (uint32_t)mo->chunk_count;
            mo->ki_count++;
        }
    }

    /* Chunk header: LITTLE ENDIAN (standard for MOC5) */
    if (mo->mobi_muxdbg)
        fprintf(stderr,"[CHUNK] #%d pos=%lld cs=%u vs=%u aus=%d\n",
            mo->chunk_count, (long long)chunk_start, chunk_size, video_written, audio_chunk_size);
    avio_wl32(pb, chunk_size);
    avio_wl32(pb, (uint32_t)video_written);

    /* Video payload (+ alignment padding) */
    avio_write(pb, mo->video_buf, mo->video_size);
    for (int i = 0; i < video_pad; i++)
        avio_w8(pb, 0);

    /* Audio payload — Vorbis payload already contains its own 4-byte header
     * ([LE16 0xFFFF][LE16 num_packets] for multi-packet format). */
    if (audio_payload_size > 0)
        avio_write(pb, audio_payload, audio_payload_size);

    /* Trailing chunk padding: the demuxer computes
     *   raw_end = chunk_pos + chunk_size
     *   pad     = 4 - (raw_end % 4)     // always 1-4
     *   next    = raw_end + pad
     * and seeks to `next`. We must write those pad bytes so
     * the next chunk header lands at the right position.
     *
     * For folded (block-audio) chunks we already wrote the full audio; its last
     * `trail_pad` bytes ARE the alignment padding the demuxer re-reads, so we
     * must NOT emit extra pad bytes (that is exactly the 4-byte-too-long bug).
     * For non-folded chunks (PCM/Vorbis) write the pad explicitly. */
    if (!fold_audio) {
        for (int i = 0; i < trail_pad; i++)
            avio_w8(pb, 0);
    }

    /* Cleanup */
    av_freep(&mo->video_buf);
    mo->video_size  = 0;
    mo->video_is_key = 0;
    if (free_audio_payload)
        av_freep(&audio_payload);
    if (!mo->adpcm_mode) {
        av_freep(&mo->audio_buf);
        mo->audio_size = 0;
    }

    /* Track file end for the trailer's ff_format_shift_data call */
    mo->file_end = avio_tell(pb);

    mo->chunk_count++;
    return 0;
}

/*
 * Extract the pure MobiClip bitstream payload from an x264 Annex-B packet.
 *
 * x264 with -mobiclip 1 outputs H.264 Annex-B format: start codes + NAL
 * header byte + payload.  I-frames include SPS/PPS/SEI NALs before the IDR
 * slice; P-frames are usually just one non-IDR slice NAL.
 *
 * The MobiClip decoder (mobiclip.c) calls bswap16_buf on the raw packet data
 * and then reads bits directly — it expects the RAW MobiClip bitstream with:
 *   - No start codes
 *   - No NAL header byte
 *   - No emulation-prevention 0x03 bytes
 *   - All slice payloads concatenated
 *
 * Returns 0 on success (*out_data / *out_size set); caller must av_free(*out_data).
 * Returns AVERROR_INVALIDDATA if no slice NAL found.
 */
int ff_extract_mobiclip_payload(const uint8_t *src, int src_size,
                                uint8_t **out_data, int *out_size)
{
    uint8_t *out = av_malloc(src_size + AV_INPUT_BUFFER_PADDING_SIZE);
    int out_len = 0;
    int i = 0;

    if (!out)
        return AVERROR(ENOMEM);

    while (i < src_size) {
        /* Find next Annex-B start code */
        int sc_pos = -1, sc_len = 0;
        for (int j = i; j + 2 < src_size; j++) {
            if (j + 3 < src_size &&
                src[j]==0 && src[j+1]==0 && src[j+2]==0 && src[j+3]==1) {
                sc_pos = j; sc_len = 4; break;
            }
            if (src[j]==0 && src[j+1]==0 && src[j+2]==1) {
                sc_pos = j; sc_len = 3; break;
            }
        }
        if (sc_pos < 0) break;

        int nal_hdr = sc_pos + sc_len;
        if (nal_hdr >= src_size) break;

        int nal_type = src[nal_hdr] & 0x1f;
        int payload_start = nal_hdr + 1; /* skip the NAL header byte */

        /* Find end of this NAL (next start code or EOF).
         * IMPORTANT: for slice NALs (type 1-5), x264 in mobiclip mode does NOT
         * add emulation-prevention bytes (0x000003), so the raw bitstream data
         * can naturally contain 0x000001 sequences that look like start codes.
         * Stopping at such a "false" start code truncates the frame and causes
         * decoder desync.  Since each x264 packet contains at most one slice NAL
         * (SPS/PPS always precede it), extend a slice NAL to the end of the
         * packet rather than searching for the next start code. */
        int nal_end;
        if (nal_type >= 1 && nal_type <= 5) {
            /* Slice NAL: use full remaining packet to avoid false start code hits */
            nal_end = src_size;
        } else {
            /* Non-slice NAL (SPS, PPS, SEI, etc.): scan for the next start code */
            nal_end = src_size;
            for (int j = payload_start; j + 2 < src_size; j++) {
                if ((j + 3 < src_size &&
                     src[j]==0 && src[j+1]==0 && src[j+2]==0 && src[j+3]==1) ||
                    (src[j]==0 && src[j+1]==0 && src[j+2]==1)) {
                    nal_end = j;
                    break;
                }
            }
        }

        /* Only copy slice NALs (types 1-5); skip SPS(7), PPS(8), SEI(6), etc. */
        if (nal_type >= 1 && nal_type <= 5) {
            int copy_len = nal_end - payload_start;
            memcpy(out + out_len, src + payload_start, copy_len);
            out_len += copy_len;
        }

        i = nal_end;
    }

    if (out_len == 0) {
        av_free(out);
        return AVERROR_INVALIDDATA;
    }

    memset(out + out_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *out_data = out;
    *out_size = out_len;
    return 0;
}

/* Append one encoded Vorbis packet to the per-chunk queue. */
static int queue_vorbis_pkt(MoMuxContext *mo, const uint8_t *data, int size)
{
    if (mo->vq_count == mo->vq_cap) {
        int ncap = mo->vq_cap ? mo->vq_cap * 2 : 16;
        uint8_t **nd = av_realloc_array(mo->vq_data, ncap, sizeof(*nd));
        if (!nd) return AVERROR(ENOMEM);
        mo->vq_data = nd;
        int *ns = av_realloc_array(mo->vq_size, ncap, sizeof(*ns));
        if (!ns) return AVERROR(ENOMEM);
        mo->vq_size = ns;
        mo->vq_cap = ncap;
    }
    mo->vq_data[mo->vq_count] = av_memdup(data, size);
    if (!mo->vq_data[mo->vq_count]) return AVERROR(ENOMEM);
    mo->vq_size[mo->vq_count] = size;
    mo->vq_count++;
    return 0;
}

/* Drain the in-muxer Vorbis encoder, queueing every output packet. */
static int drain_vorbis_enc(MoMuxContext *mo)
{
    for (;;) {
        AVPacket pkt = {0};
        int ret = avcodec_receive_packet(mo->vorbis_enc, &pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        ret = queue_vorbis_pkt(mo, pkt.data, pkt.size);
        av_packet_unref(&pkt);
        if (ret < 0)
            return ret;
    }
}

/* Send one fixed-size FLTP frame (front `n` samples of pcm_buf) to the encoder. */
static int send_vorbis_frame(MoMuxContext *mo, int n)
{
    int ret;
    int enc_ch = mo->vorbis_enc->ch_layout.nb_channels;
    av_frame_unref(mo->vorbis_frame);
    mo->vorbis_frame->format = AV_SAMPLE_FMT_FLTP;
    mo->vorbis_frame->nb_samples = n;
    av_channel_layout_copy(&mo->vorbis_frame->ch_layout, &mo->vorbis_enc->ch_layout);
    if ((ret = av_frame_get_buffer(mo->vorbis_frame, 0)) < 0)
        return ret;
    for (int ch = 0; ch < enc_ch; ch++) {
        float *dst = (float *)mo->vorbis_frame->extended_data[ch];
        int src_ch = (ch < mo->channels) ? ch : 0; /* mono -> duplicate to stereo */
        for (int i = 0; i < n; i++)
            dst[i] = mo->pcm_buf[i * mo->channels + src_ch] / 32768.0f;
    }
    /* Consume n sample-frames from the front of pcm_buf. */
    if (n < mo->pcm_samples)
        memmove(mo->pcm_buf, mo->pcm_buf + n * mo->channels,
                (size_t)(mo->pcm_samples - n) * mo->channels * sizeof(int16_t));
    mo->pcm_samples -= n;
    if ((ret = avcodec_send_frame(mo->vorbis_enc, mo->vorbis_frame)) < 0)
        return ret;
    return drain_vorbis_enc(mo);
}

/* Encode one PCM_S16LE packet (or NULL to flush) to Vorbis, queueing output.
 * libvorbis requires fixed frame_size frames (no variable-frame-size cap), so
 * buffer PCM and feed exact frame_size chunks; the final flush sends the
 * remaining partial frame (SMALL_LAST_FRAME) then drains. */
static int encode_pcm_to_vorbis(MoMuxContext *mo, const AVPacket *pkt)
{
    int ret;
    int fs = mo->vorbis_enc->frame_size > 0 ? mo->vorbis_enc->frame_size : 64;

    if (pkt) {
        int nb = pkt->size / (mo->channels * (int)sizeof(int16_t));
        if (nb > 0 && (ret = append_pcm(mo, (const int16_t *)pkt->data, nb)) < 0)
            return ret;
        while (mo->pcm_samples >= fs)
            if ((ret = send_vorbis_frame(mo, fs)) < 0)
                return ret;
        return 0;
    }

    /* Flush: emit any remaining partial frame, then signal EOF and drain. */
    if (mo->pcm_samples > 0 && (ret = send_vorbis_frame(mo, mo->pcm_samples)) < 0)
        return ret;
    if ((ret = avcodec_send_frame(mo->vorbis_enc, NULL)) < 0 && ret != AVERROR_EOF)
        return ret;
    return drain_vorbis_enc(mo);
}

static int mo_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MoMuxContext *mo = s->priv_data;
    int ret;

    if (pkt->stream_index == 0) {
        /* Video — flush any prior video first */
        if (mo->video_buf) {
            ret = flush_chunk(s);
            if (ret < 0) return ret;
        }

        uint8_t *v_payload = NULL;
        int v_payload_size = 0;

        /* Extract the pure MobiClip bitstream from the Annex-B NAL packet.
         * Strips SPS/PPS/SEI NALs, the NAL header byte, and emulation-prevention
         * bytes so the decoder receives raw mobiclip bitstream data. */
        ret = ff_extract_mobiclip_payload(pkt->data, pkt->size, &v_payload, &v_payload_size);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to extract MobiClip payload from video packet\n");
            return ret;
        }

        mo->video_buf    = v_payload;
        mo->video_size   = v_payload_size;
        mo->video_pts    = pkt->pts;
        mo->video_is_key = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        mo->first_video_written = 1;

        /* Defer flushing until audio arrives so ADPCM chunks are paired with
         * real PCM instead of synthetic silence.  If another video arrives
         * first, the pending frame is flushed at the top of this branch.
         * For no-audio mode, flush immediately since no audio will arrive. */
        if (mo->audio_codec == 4) {
            ret = flush_chunk(s);
            if (ret < 0) return ret;
        }
    } else {
        /* Audio */
        if (mo->adpcm_mode) {
            int sample_frames = pkt->size / (mo->channels * (int)sizeof(int16_t));
            if (sample_frames > 0) {
                ret = append_pcm(mo, (const int16_t *)pkt->data, sample_frames);
                if (ret < 0) return ret;
            }
        } else if (mo->audio_codec == 3) {
            if (mo->vorbis_from_pcm) {
                /* PCM in -> encode to Vorbis -> queue resulting packets. */
                ret = encode_pcm_to_vorbis(mo, pkt);
                if (ret < 0) return ret;
            } else {
                /* Already-Vorbis: queue each packet separately. */
                ret = queue_vorbis_pkt(mo, pkt->data, pkt->size);
                if (ret < 0) return ret;
            }
        } else {
            /* Accumulate audio packets (FastAudio passthrough) */
            uint8_t *new_buf = av_realloc(mo->audio_buf, mo->audio_size + pkt->size);
            if (!new_buf) return AVERROR(ENOMEM);
            mo->audio_buf = new_buf;
            memcpy(mo->audio_buf + mo->audio_size, pkt->data, pkt->size);
            if (mo->audio_size == 0)
                mo->audio_pts = pkt->pts;
            mo->audio_size += pkt->size;
        }

        if (mo->video_buf) {
            /* For Vorbis, DON'T flush on audio: let the next video packet (or
             * the trailer) flush this frame's chunk. Otherwise the last video
             * chunk is flushed before the encoder's trailing flush packets are
             * queued, and those final ~100 ms of audio are dropped ("N Vorbis
             * packets left unmuxed"). Deferring keeps the last video frame
             * buffered so the trailer's flush_chunk packs the tail packets. */
            int defer_for_vorbis = (mo->audio_codec == 3);
            /* FastAudio (in-muxer PCM->fastaudio) also drains pcm_buf inside
             * flush_chunk on a per-video-frame target, so flushing the last
             * video chunk before all audio is buffered drops the tail. Defer
             * it to the next video / trailer too. */
            int defer_for_fastaudio = (mo->audio_codec == 0 && mo->fa_active);
            if (!defer_for_vorbis && !defer_for_fastaudio &&
                (!mo->adpcm_mode || mo->audio_codec != 1 ||
                 mo->pcm_samples >= mo->samples_per_chunk_base)) {
                ret = flush_chunk(s);
                if (ret < 0) return ret;
            }
        }
    }

    return 0;
}

static int mo_write_trailer(AVFormatContext *s)
{
    MoMuxContext *mo = s->priv_data;
    AVIOContext  *pb = s->pb;

    /* Flush the in-muxer Vorbis encoder so trailing packets are queued and
     * packed by the final flush_chunk below. */
    if (mo->vorbis_from_pcm && mo->vorbis_enc)
        encode_pcm_to_vorbis(mo, NULL);

    /* Drain ALL remaining buffered audio into the final chunk. */
    mo->draining = 1;
    if (mo->video_buf)
        flush_chunk(s);

    if (!mo->adpcm_mode) {
        av_freep(&mo->audio_buf);
        mo->audio_size = 0;
    }

    if (mo->vq_count > 0)
        av_log(s, AV_LOG_WARNING, "%d Vorbis packet(s) left unmuxed at end of "
               "stream (audio slightly longer than video).\n", mo->vq_count);
    for (int i = 0; i < mo->vq_count; i++)
        av_freep(&mo->vq_data[i]);
    av_freep(&mo->vq_data);
    av_freep(&mo->vq_size);
    mo->vq_count = mo->vq_cap = 0;

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        /* Save file_end from context (recorded during sequential writes).
         * We cannot use avio_seek(pb, 0, SEEK_END) here because
         * AVIO write-only contexts lose track of true file end
         * after backward seeks. */
        int64_t file_end = mo->file_end;

        avio_seek(pb, mo->frame_count_offset, SEEK_SET);
        avio_wl32(pb, (uint32_t)mo->chunk_count);
        avio_seek(pb, mo->max_frame_size_offset, SEEK_SET);
        avio_wl32(pb, mo->max_chunk_size);


        /* Insert KI table + cc chunk + HE marker.
         * write_header left a 4-byte KI placeholder at ki_length_offset.
         * We shift everything after ki_length_offset by:
         *   KI tag+len(4) + KI entries(ki_count*8) + cc(4+64=68) + HE(4) - placeholder(4)
         * = ki_count*8 + 72
        * Then write KI + cc + HE in place, and patch header_size. */
        if (mo->ki_length_offset > 0 && mo->ki_count > 0) {
            int ret;
            /* shift by exactly what we need to write: KI(4+ki_count*8) + cc(68) + HE(4).
             * The old 4-byte placeholder at ki_length_offset is included in the shift.
             * After shift, bytes [ki_length_offset, ki_length_offset+insert) are free to write.
             * We write ki_count*8+76 bytes, leaving 4 bytes of shifted placeholder at end. */
            int insert = mo->ki_count * 8 + 72;

            avio_flush(pb);  /* ensure frame_count and max_chunk_size writes are on disk */
            /* Seek to the actual end of file so ff_format_shift_data
             * sees the correct pos_end.  We cannot use SEEK_END here because
             * after backward header-patch seeks the AVIO write cursor is
             * no longer at the real end. */
            avio_seek(pb, file_end, SEEK_SET);
            ret = ff_format_shift_data(s, mo->ki_length_offset, insert);
            if (ret < 0) return ret;

            avio_flush(pb);  /* flush shifted data before seeking back */

            /* Write KI at ki_length_offset */
            avio_seek(pb, mo->ki_length_offset, SEEK_SET);
            avio_wl16(pb, FORMAT_KEYINDEX);
            avio_wl16(pb, (uint16_t)(mo->ki_count * 2));
            for (int i = 0; i < mo->ki_count; i++) {
                avio_wl32(pb, mo->ki_kf_offsets[i] + insert);
                avio_wl32(pb, mo->ki_kf_frames[i]);
            }

            /* Write cc chunk right after KI, payload zeroed for now. The real
             * RSA-512 clip-certificate is computed below over the finalized
             * header and patched in. */
            int64_t cc_payload_off = avio_tell(pb) + 4;   /* after the cc tag header */
            avio_wl16(pb, FORMAT_POSSIBLY_CAPTIONS);
            avio_wl16(pb, 16);  /* 64 bytes / 4 */
            for (int i = 0; i < 64; i++)
                avio_w8(pb, 0);

            /* Write HE marker */
            avio_wl16(pb, FORMAT_HEADER_DONE);
            avio_wl16(pb, 0);

            /* Patch header_size at byte 4.
             * Total bytes written from ki_length_offset:
             *   KI tag+len(4) + KI entries(ki_count*8) + cc tag+len(4) + cc payload(64) + HE(4)
             *   = ki_count*8 + 76
             * header_size field = (end_of_header) - 8 */
            int64_t new_header_end = mo->ki_length_offset + mo->ki_count * 8 + 76;
            avio_seek(pb, 4, SEEK_SET);
            avio_wl32(pb, (uint32_t)(new_header_end - 8));

            /* --- Sign the clip certificate (cc) ---
             * Compute H over the finalized header structure, RSA-512 sign it
             * with the hard-coded license key, and patch the 64-byte cc payload
             * in place. The output pb is write-only, so (like
             * ff_format_shift_data) we re-open the URL for reading to get the
             * header bytes back. The on-disk cc payload is still zero here and
             * is skipped by the hash anyway. */
            if (new_header_end <= INT_MAX) {
                int hlen = (int)new_header_end;
                uint8_t *hbuf = av_malloc(hlen);
                AVIOContext *read_pb = NULL;
                if (!hbuf)
                    return AVERROR(ENOMEM);
                avio_flush(pb);
                ret = s->io_open(s, &read_pb, s->url, AVIO_FLAG_READ, NULL);
                if (ret >= 0) {
                    int got = avio_read(read_pb, hbuf, hlen);
                    ff_format_io_close(s, &read_pb);
                    if (got == hlen) {
                        uint8_t cc[64];
                        ret = mo_sign_cc(hbuf, hlen, cc);
                        if (ret < 0) { av_free(hbuf); return ret; }
                        avio_seek(pb, cc_payload_off, SEEK_SET);
                        avio_write(pb, cc, 64);
                        if (mo->mobi_dbg)
                            av_log(s, AV_LOG_DEBUG,
                                   "MOENC: signed cc tag (RSA-512) over %d-byte header\n", hlen);
                    } else {
                        av_log(s, AV_LOG_WARNING,
                               "MOENC: short read signing cc (got %d/%d); left unsigned\n",
                               got, hlen);
                    }
                } else {
                    av_log(s, AV_LOG_WARNING,
                           "MOENC: could not re-open %s to sign cc; left unsigned\n", s->url);
                }
                av_free(hbuf);
            }
        }
        avio_seek(pb, 0, SEEK_END);
    }

    av_freep(&mo->video_buf);
    av_freep(&mo->audio_buf);
    av_freep(&mo->pcm_buf);
    av_freep(&mo->ki_kf_offsets);
    av_freep(&mo->ki_kf_frames);
    av_frame_free(&mo->fa_frame);
    avcodec_free_context(&mo->fa_enc);
    av_frame_free(&mo->vorbis_frame);
    avcodec_free_context(&mo->vorbis_enc);
    return 0;
}

#include "libavutil/opt.h"
#define OFFSET(x) offsetof(MoMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption mo_options[] = {
    { "mo_audio", "MobiClip audio codec", OFFSET(audio_codec), AV_OPT_TYPE_INT, {.i64 = 1}, -1, 4, ENC, "mo_audio" },
    { "fastaudio", "FastAudio", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, ENC, "mo_audio" },
    { "adpcm", "ADPCM", 0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, ENC, "mo_audio" },
    { "pcm", "PCM", 0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, ENC, "mo_audio" },
    { "vorbis", "Vorbis", 0, AV_OPT_TYPE_CONST, {.i64 = 3}, 0, 0, ENC, "mo_audio" },
    { "none", "No audio", 0, AV_OPT_TYPE_CONST, {.i64 = 4}, 0, 0, ENC, "mo_audio" },
    { "mobi_dbg", "MobiClip Debug", OFFSET(mobi_dbg), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, ENC },
    { "mobi_muxdbg", "MobiClip Muxer Debug", OFFSET(mobi_muxdbg), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, ENC },
    { NULL },
};

static const AVClass mo_muxer_class = {
    .class_name = "mobiclip_mo",
    .item_name  = av_default_item_name,
    .option     = mo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_mo_muxer = {
    .p.name           = "mobiclip_mo",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MobiClip MO"),
    .p.extensions     = "mo",
    .priv_data_size = sizeof(MoMuxContext),
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_MOBICLIP,
    .write_header   = mo_write_header,
    .write_packet   = mo_write_packet,
    .write_trailer  = mo_write_trailer,
    .p.priv_class     = &mo_muxer_class,
};
