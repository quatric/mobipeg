/*
 * MODS muxer
 * Copyright (c) 2026
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * Retail-faithful Nintendo DS Mobiclip (.mods) writer.
 *
 * Layout reverse-engineered from retail reference files (stupi-test*-{adpcm,
 * fastaudio,pcm,codebook}.mods) and pleonex/PlayMobic Binary2Mods:
 *
 *   0x00 char[6]  "MODSN3"           0x06 u16  video codec (0x000A)
 *   0x08 u32      frame count
 *   0x0C u32      width              0x10 u32  height
 *   0x14 u32      fps (16.16-ish: num*0x1000000/den)
 *   0x18 u16      audio codec        0x1A u16  channels
 *                 (0=none 1=FastAudio-codebook/SX 2=FastAudio 3=IMA-ADPCM 4=PCM16)
 *   0x1C u32      sample rate
 *   0x20 u32      largest frame chunk size + 4  (DS read-buffer size)
 *   0x24 u32      audio-codec-info offset  (SX codebook location; 0 otherwise)
 *   0x28 u32      key-frame table offset   0x2C u32  key-frame count
 *   0x30 u32      'HE\0\0' header-done marker
 *   0x34 ...      interleaved frame chunks
 *   <kf off> ...  key-frame table: [u32 frame_number][u32 data_offset] pairs
 *
 * Each frame chunk word is (chunk_size << 14) | audio_block_count, where
 * chunk_size counts BOTH the video bitstream and the audio that follows it.
 * There is NO explicit video/audio split: the DS video decoder consumes the
 * video bitstream and the remaining bytes of the chunk are the audio blocks
 * (audio_block_count of them).  We therefore write [video][audio] with no
 * suffix, exactly as retail does.
 */

#include "avformat.h"
#include "mux.h"
#include "avio_internal.h"
#include "internal.h"
#include "mo.h"
#include "mo_audioenc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

/* Retail audio codec ids (header @0x18). */
enum {
    MODS_ACODEC_NONE      = 0,
    MODS_ACODEC_CODEBOOK  = 1,  /* FastAudio-codebook / SX (needs codebooks) */
    MODS_ACODEC_FASTAUDIO = 2,
    MODS_ACODEC_ADPCM     = 3,
    MODS_ACODEC_PCM       = 4,
};

/* Fixed header field offsets we patch in the trailer. */
#define MODS_OFF_FRAME_COUNT  0x08
#define MODS_OFF_MAX_FRAME    0x20
#define MODS_OFF_KF_OFF       0x28
#define MODS_OFF_KF_COUNT     0x2C
#define MODS_FRAME_DATA_START 0x34

typedef struct MODSMuxContext {
    const AVClass *class;   /* MUST be first (the muxer has a priv_class) */
    uint32_t frame_count;
    int audio_codec;        /* -mo_audio selection (MOBI_AUDIO_* / 3=none / 4=codebook) */
    int video_index;
    int audio_index;
    int aenc_inited;
    int audio_block;        /* bytes per encoded audio block (0 = no/loose audio) */
    int channels;
    MobiAudioEnc aenc;

    /* Per-frame audio pacing: each video frame should carry the audio that plays
     * during it (~rate/fps samples), not every block buffered so far. Retail
     * spreads blocks evenly (e.g. 7,6,6,6,...); bursting them (8,8,4,...) makes
     * audio under/overflow per frame. Track owed samples and emit whole blocks. */
    double   spf;           /* audio samples that elapse per video frame */
    double   audio_credit;  /* owed audio samples not yet attached to a frame */
    int      block_samples; /* samples represented by one audio_block (256) */

    /* Pending video frame, held back so the audio that arrives before the next
     * video frame can be appended to its chunk. */
    uint8_t *pend_vid;
    int      pend_vid_size;
    int      has_pend;
    int      pend_keyframe;

    /* Accumulator of encoded audio bytes destined for the next written chunk. */
    uint8_t *abuf;
    int      abuf_size;
    int      abuf_cap;

    /* DS .mods native IMA-ADPCM path. The retail layout is per-frame planar:
     *   [u32 frame hdr][ch0 hdr u32][ch0 nibbles][ch1 hdr u32][ch1 nibbles]
     * with one standard-IMA header per channel per frame (NOT ffmpeg's
     * ADPCM_IMA_MOFLEX, which interleaves per-256-sample-block). We therefore
     * buffer raw interleaved PCM and encode it ourselves at flush time. */
    int       ds_adpcm;     /* 1 => use the native DS IMA path below */
    int16_t  *pcmbuf;       /* interleaved s16 PCM, channels-interleaved */
    int       pcm_nsamp;    /* buffered sample-frames (per channel) */
    int       pcm_cap;      /* capacity in sample-frames */
    uint8_t  *ds_s0, *ds_s1; /* per-channel IMA nibble scratch (pre-interleave) */
    int       ds_scap;
    int       ima_pred[2];  /* per-channel IMA state, carried across frames */
    int       ima_idx[2];
    int       audio_started; /* 1 once the 12-byte audio header has been written */

    /* Key-frame table: [frame_number][data_offset] pairs gathered as we go. */
    uint32_t *kf;           /* 2*kf_count u32s */
    int       kf_count;
    int       kf_cap;

    uint32_t max_chunk;     /* largest chunk size written (excl. the 4-byte word) */
} MODSMuxContext;

/* Standard IMA-ADPCM tables (the DS uses the canonical IMA codec). */
static const int16_t mods_ima_step[89] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
        73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
        408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
        1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,
        7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
        22385,24623,27086,29794,32767 };
static const int8_t mods_ima_idx[16] = {
        -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };

/* Encode `n` (even) mono S16 samples (taken from src with the given stride, in
 * sample-frames) to IMA nibbles, low-nibble-first, n/2 bytes into out. Samples
 * past `valid` are treated as silence (final-frame padding). The IMA state
 * (predictor/step-index) is carried in/out via *pred,*idx: the DS decodes the
 * whole movie as one continuous per-channel IMA stream, so state MUST persist
 * across frames (resetting per frame produces garbage on hardware). */
static void mods_ima_encode(const int16_t *src, int stride, int n, int valid,
                            uint8_t *out, int *predp, int *idxp)
{
    int pred = *predp, idx = *idxp;
    for (int i = 0; i < n; i++) {
        int s = (i < valid) ? src[i * stride] : 0;
        int step = mods_ima_step[idx];
        int diff = s - pred, nib = 0, d, delta;
        if (diff < 0) { nib = 8; diff = -diff; }
        if (diff >= step)        { nib |= 4; diff -= step; }
        if (diff >= (step >> 1)) { nib |= 2; diff -= step >> 1; }
        if (diff >= (step >> 2)) { nib |= 1; }
        /* Reconstruct EXACTLY as the DS Mobiclip IMA decoder does: PER-TERM
         * accumulation (d = step>>3, then += step / step>>1 / step>>2 per bit),
         * NOT the single-shift ((2*delta+1)*step)>>3. Proven by decoding retail
         * stupi-test1-adpcm.mods at its (verified-correct) audio offsets: single
         * shift drifts (R-channel MAE ~4544, growing over the clip = the "crusty,
         * right louder than left" hardware symptom), per-term matches (MAE ~63).
         * The two differ only by per-sample rounding (sum-of-floors vs floor-of-
         * sum) but the error accumulates across the continuous stream. */
        delta = nib & 7;
        d = step >> 3;
        if (nib & 4) d += step;
        if (nib & 2) d += step >> 1;
        if (nib & 1) d += step >> 2;
        pred = (nib & 8) ? pred - d : pred + d;
        if (pred > 32767)  pred = 32767;
        if (pred < -32768) pred = -32768;
        idx += mods_ima_idx[nib];
        if (idx < 0)  idx = 0;
        if (idx > 88) idx = 88;
        if ((i & 1) == 0) out[i >> 1]  = nib;
        else              out[i >> 1] |= nib << 4;
    }
    *predp = pred;
    *idxp  = idx;
}



static int mods_record_keyframe(MODSMuxContext *m, uint32_t frame_no, uint32_t off)
{
    if (m->kf_count >= m->kf_cap) {
        int nc = m->kf_cap ? m->kf_cap * 2 : 64;
        uint32_t *nk = av_realloc(m->kf, (size_t)nc * 2 * sizeof(uint32_t));
        if (!nk)
            return AVERROR(ENOMEM);
        m->kf = nk;
        m->kf_cap = nc;
    }
    m->kf[m->kf_count * 2 + 0] = frame_no;
    m->kf[m->kf_count * 2 + 1] = off;
    m->kf_count++;
    return 0;
}

/* Write the pending video frame as one chunk, appending whole audio blocks.
 * When `final` is set (trailer), all remaining buffered audio is emitted,
 * including a trailing partial block. */
static int mods_flush_pending(AVFormatContext *s, int final)
{
    MODSMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int vsize, asize, total, nblocks, vfield, vwrite;
    int kf_word = 0;   /* 4-byte keyframe "unknown" word for the aenc path */
    /* FastAudio is a stateful, block-granular codec read at the video-decoder-stop
     * offset (like DS ADPCM). PCM is raw 4-byte stereo frames placed after the
     * padded video and tolerates no such treatment (a 2-byte decoder-stop offset
     * would swap L/R). So the keyframe word + decoder-stop alignment apply to
     * FastAudio only; PCM keeps its original (hardware-correct) layout. */
    int fa = (m->aenc_inited && m->audio_codec == MOBI_AUDIO_FASTAUDIO);
    int64_t chunk_off;

    if (!m->has_pend)
        return 0;

    vsize = m->pend_vid_size;

    /* The DS reads frame data via 32-bit word access, so every chunk's payload
     * MUST be a multiple of 4 bytes and every frame starts on a 4-byte boundary
     * (verified against retail). For NON-ADPCM modes we simply pad the video up
     * to 4 bytes (audio block sizes are already %4).
     *
     * For ADPCM the DS reads the audio immediately after where its VIDEO decoder
     * stops, i.e. at ceil(mb_bits/16)*2 bytes. x264's payload is the MB data plus
     * an RBSP stop bit (the last 1 bit) and zero padding. We find the stop bit to
     * recover mb_bits, set the video field to that 16-bit boundary (= the decoder
     * "consumed" position), place the audio there, and pad the CHUNK END (not the
     * video) to keep the frame 4-aligned. Trimming trailing zeros alone is wrong
     * when the stop bit falls beyond the 16-bit boundary (mb_bits % 16 == 0). */
    /* The DS reads audio from where its VIDEO decoder STOPS (= ceil(mb_bits/16)*2),
     * NOT after the full zero-padded video payload. This applies to EVERY audio
     * codec (PlayMobic ModsPacketReader advances the packet by the bytes the video
     * decoder consumed). ADPCM always did this; FastAudio/PCM previously placed
     * audio after the padded video (vsize rounded to 4), leaving a 2-4 byte gap
     * before the decoder-stop offset. Stateless PCM tolerated the shift, but
     * stateful, block-granular FastAudio desynced every frame -> loud garbage.
     * Place audio at the decoder-stop boundary whenever audio is attached. */
    if (m->ds_adpcm || fa || m->audio_codec == MOBI_AUDIO_PCM) {
        int e = vsize, mb_bits = 0;
        while (e > 0 && m->pend_vid[e - 1] == 0)
            e--;
        if (e > 0) {
            int lb = m->pend_vid[e - 1], k = 0;
            while (!((lb >> k) & 1)) k++;   /* lowest set bit = RBSP stop bit */
            mb_bits = (e - 1) * 8 + (7 - k);
        }
        vfield = ((mb_bits + 15) / 16) * 2; /* decoder "consumed", 16-bit aligned */
        vwrite = FFMIN(vfield, vsize);      /* real MB bytes; rest zero-filled */
    } else {
        vfield = vsize + ((-vsize) & 3);
        vwrite = vsize;
    }

    int ds_samp = 0;   /* DS path: PCM sample-frames consumed this chunk */

    if (m->ds_adpcm) {
        /* Native DS IMA path: pace whole 256-sample blocks, encode planar. */
        int avail_blocks = m->pcm_nsamp / m->block_samples;
        int target;
        if (final) {
            nblocks = (m->pcm_nsamp + m->block_samples - 1) / m->block_samples;
        } else {
            m->audio_credit += m->spf;
            target = (int)(m->audio_credit / m->block_samples);
            if (target < 0)
                target = 0;
            nblocks = FFMIN(target, avail_blocks);
            m->audio_credit -= (double)nblocks * m->block_samples;
        }
        if (nblocks < 0) nblocks = 0;
        if (nblocks > 0x3FFF) nblocks = 0x3FFF;

        ds_samp = nblocks * m->block_samples;          /* samples/ch */
        int chbytes = nblocks * (m->block_samples / 2); /* nibbles/ch -> bytes */
        int blkbytes = m->block_samples / 2;            /* 128 bytes per 256-sample block */
        /* The 12-byte audio header ([u32 ?][ch0 init pred,idx][ch1 init pred,idx])
         * is written ONCE, on the first frame that carries audio. Subsequent
         * frames are pure continuous nibbles starting at the video-decoder stop
         * offset (verified against retail: only frame 0 has the 12-byte overhead;
         * every other frame's chunk is exactly nibbles + 0..2 pad). Emitting the
         * header every frame shifts the DS's per-frame nibble read by 24 samples,
         * corrupting state and dragging pitch ("low pitch, distorted"). */
        /* The DS expects a per-channel IMA header (stepIndex u16 + predictor
         * s16) inline before each channel's FIRST block on EVERY video
         * keyframe, preceded by a 4-byte "unknown" word (the decoder skips it).
         * Overhead = 4 + channels*4. Required so the audio stream stays byte-
         * aligned across keyframes; without it the DS reads audio data as
         * headers from the 2nd keyframe on (crust) and reads ch1's header out
         * of ch0's data from the very start (constant R-vs-L imbalance). */
        int hdr = (nblocks > 0 && m->pend_keyframe) ? (4 + 4 * m->channels) : 0;
        asize = hdr + nblocks * (2 * blkbytes);

        if (asize > m->abuf_cap) {
            int nc = m->abuf_cap ? m->abuf_cap * 2 : 8192;
            while (nc < asize) nc *= 2;
            uint8_t *nb = av_realloc(m->abuf, nc);
            if (!nb) return AVERROR(ENOMEM);
            m->abuf = nb; m->abuf_cap = nc;
        }
        if (chbytes > m->ds_scap) {
            int nc = m->ds_scap ? m->ds_scap * 2 : 8192;
            while (nc < chbytes) nc *= 2;
            uint8_t *s0 = av_realloc(m->ds_s0, nc);
            uint8_t *s1 = av_realloc(m->ds_s1, nc);
            if (!s0 || !s1) { av_free(s0); av_free(s1); return AVERROR(ENOMEM); }
            m->ds_s0 = s0; m->ds_s1 = s1; m->ds_scap = nc;
        }
        if (nblocks > 0) {
            int valid = FFMIN(ds_samp, m->pcm_nsamp);
            /* Per-channel header = IMA state at the START of this frame (so the
             * DS can resume/seek); state then persists across frames. */
            int h_p0 = m->ima_pred[0], h_i0 = m->ima_idx[0];
            int h_p1 = m->ima_pred[1], h_i1 = m->ima_idx[1];
            mods_ima_encode(m->pcmbuf + 0, m->channels, ds_samp, valid, m->ds_s0,
                            &m->ima_pred[0], &m->ima_idx[0]);
            mods_ima_encode(m->pcmbuf + (m->channels > 1 ? 1 : 0), m->channels,
                            ds_samp, valid, m->ds_s1,
                            &m->ima_pred[1], &m->ima_idx[1]);
            uint8_t *p = m->abuf;
            if (hdr) {
                AV_WL32(p, 0); p += 4;          /* per-keyframe "unknown" (DS skips it) */
                m->audio_started = 1;
            } else {
                (void)h_p0; (void)h_i0; (void)h_p1; (void)h_i1;
            }
            for (int b = 0; b < nblocks; b++) {
                /* Inline keyblock header (stepIndex u16, predictor s16) precedes
                 * each channel's first block on keyframes — order is idx, pred. */
                if (hdr && b == 0) { AV_WL16(p, h_i0); AV_WL16(p + 2, h_p0); p += 4; }
                memcpy(p, m->ds_s0 + b * blkbytes, blkbytes); p += blkbytes;
                if (hdr && b == 0) { AV_WL16(p, h_i1); AV_WL16(p + 2, h_p1); p += 4; }
                memcpy(p, m->ds_s1 + b * blkbytes, blkbytes); p += blkbytes;
            }
        } else {
            asize = 0;
        }
    } else if (m->aenc_inited && m->audio_block > 0) {
        int abytes;
        if (final) {
            mobi_aenc_flush(&m->aenc);
            while (1) {
                AVPacket apkt = {0};
                int ret = mobi_aenc_recv(&m->aenc, &apkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    return ret;
                uint8_t *nb = av_realloc(m->abuf, m->abuf_size + apkt.size);
                if (!nb) {
                    av_packet_unref(&apkt);
                    return AVERROR(ENOMEM);
                }
                m->abuf = nb;
                memcpy(m->abuf + m->abuf_size, apkt.data, apkt.size);
                m->abuf_size += apkt.size;
                av_packet_unref(&apkt);
            }
            abytes  = m->abuf_size;
            nblocks = (abytes + m->audio_block - 1) / m->audio_block;
            if (m->audio_codec == MOBI_AUDIO_PCM)
                abytes = nblocks * m->audio_block;
        } else {
            /* Attach only the blocks that elapse during this frame (paced via
             * the sample-credit accumulator), capped by what's available.
             * Count BOTH already-encoded bytes in abuf AND encodable PCM still
             * in the encoder's input ring — avil from abuf alone would under-
             * count when a previous frame left a partial block behind. */
            int avail_encoded = m->abuf_size / m->audio_block;
            int avail_pending  = m->aenc.nsamp / m->block_samples;
            int avail = avail_encoded + avail_pending;
            m->audio_credit += m->spf;
            int target = (int)(m->audio_credit / m->block_samples);
            if (target < 0)
                target = 0;
            nblocks = FFMIN(target, avail);
            abytes  = nblocks * m->audio_block;
            m->audio_credit -= (double)nblocks * m->block_samples;

            /* Drain the encoder until abuf holds at least abytes. */
            while (m->abuf_size < abytes) {
                AVPacket apkt = {0};
                int ret = mobi_aenc_recv(&m->aenc, &apkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    return ret;
                uint8_t *nb = av_realloc(m->abuf, m->abuf_size + apkt.size);
                if (!nb) {
                    av_packet_unref(&apkt);
                    return AVERROR(ENOMEM);
                }
                m->abuf = nb;
                memcpy(m->abuf + m->abuf_size, apkt.data, apkt.size);
                m->abuf_size += apkt.size;
                av_packet_unref(&apkt);
            }
        }
        /* Every N3 keyframe carries a 4-byte word before the first audio block
         * that the DS audio decoder skips (PlayMobic ModsPacketReader). */
        kf_word = (fa && m->pend_keyframe && abytes > 0) ? 4 : 0;
        asize   = abytes + kf_word;
    } else if (m->aenc_inited) {
        /* Loose audio (no fixed block): attach all buffered bytes. */
        kf_word = (fa && m->pend_keyframe && m->abuf_size > 0) ? 4 : 0;
        asize   = m->abuf_size + kf_word;
        nblocks = 0;
    } else {
        asize   = 0;
        nblocks = 0;
    }

    /* Total chunk size (vfield is already 16-bit aligned).
     * We add a 4-byte suffix to unambiguously store the audio size and pad length
     * for FFmpeg's demuxer, avoiding the need for the racy sx_video_len hack. */
    int pad = 0;
    if (m->ds_adpcm || fa || m->audio_codec == MOBI_AUDIO_PCM)
        pad = (-(vfield + asize + 4)) & 3;
    total = vfield + asize + pad + 4;
    
    if (total > 0x3FFFF) {
        /* Pathological: drop this frame's audio rather than overflow the field. */
        asize = 0; nblocks = 0; kf_word = 0;
        pad = (m->ds_adpcm || fa || m->audio_codec == MOBI_AUDIO_PCM) ? (-(vfield + 4)) & 3 : 0;
        total = vfield + pad + 4;
    }
    if (nblocks > 0x3FFF)
        nblocks = 0x3FFF;

    chunk_off = avio_tell(pb);
    if (m->pend_keyframe) {
        int ret = mods_record_keyframe(m, m->frame_count, (uint32_t)chunk_off);
        if (ret < 0)
            return ret;
    }

    uint8_t *deint_buf = NULL;
    if (m->aenc_inited && m->audio_codec == MOBI_AUDIO_PCM && nblocks > 0) {
        int block_bytes = m->audio_block;
        int block_samples = m->block_samples;
        int valid_samples = m->abuf_size / (m->channels * 2);
        deint_buf = av_mallocz(nblocks * block_bytes);
        if (!deint_buf)
            return AVERROR(ENOMEM);
        for (int b = 0; b < nblocks; b++) {
            const int16_t *src = (const int16_t *)(m->abuf + b * block_bytes);
            int16_t *dst = (int16_t *)(deint_buf + b * block_bytes);
            int block_start = b * block_samples;
            for (int ch = 0; ch < m->channels; ch++) {
                for (int i = 0; i < block_samples; i++) {
                    int s_idx = block_start + i;
                    if (s_idx < valid_samples) {
                        dst[ch * block_samples + i] = src[i * m->channels + ch];
                    } else {
                        dst[ch * block_samples + i] = 0;
                    }
                }
            }
        }
    }

    avio_wl32(pb, ((uint32_t)total << 14) | (uint32_t)nblocks);
    avio_write(pb, m->pend_vid, vwrite);
    for (int i = vwrite; i < vfield; i++)
        avio_w8(pb, 0);            /* zero-fill video to its decoder boundary */
    if (kf_word)
        avio_wl32(pb, 0);             /* keyframe "unknown" word (DS skips it) */
    if (asize - kf_word > 0) {
        if (deint_buf)
            avio_write(pb, deint_buf, asize - kf_word);
        else
            avio_write(pb, m->abuf, asize - kf_word);
    }
    /* Write padding and our 4-byte unambiguous suffix for the demuxer */
    for (int i = 0; i < pad; i++)
        avio_w8(pb, 0);
    avio_w8(pb, pad);
    avio_w8(pb, 0);
    avio_wl16(pb, asize);

    if ((uint32_t)total > m->max_chunk)
        m->max_chunk = total;

    if (m->ds_adpcm) {
        /* Consume the PCM sample-frames just encoded (DS path owns m->abuf as
         * scratch only). */
        int consume = FFMIN(ds_samp, m->pcm_nsamp);
        if (consume < m->pcm_nsamp)
            memmove(m->pcmbuf, m->pcmbuf + consume * m->channels,
                    (size_t)(m->pcm_nsamp - consume) * m->channels * sizeof(int16_t));
        m->pcm_nsamp -= consume;
    } else if (asize - kf_word < m->abuf_size) {
        /* Retain any encoded audio bytes not yet emitted (tail alignment).
         * Only asize-kf_word bytes came from abuf; the keyframe word is not. */
        int consumed = asize - kf_word;
        memmove(m->abuf, m->abuf + consumed, m->abuf_size - consumed);
        m->abuf_size -= consumed;
    } else {
        m->abuf_size = 0;
    }

    av_free(deint_buf);

    m->frame_count++;
    av_freep(&m->pend_vid);
    m->pend_vid_size = 0;
    m->has_pend = 0;
    return 0;
}

static int mods_write_header(AVFormatContext *s)
{
    MODSMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st;
    AVRational fps;
    int retail_codec, no_audio;

    m->video_index = -1;
    m->audio_index = -1;
    for (int i = 0; i < s->nb_streams; i++) {
        enum AVMediaType t = s->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && m->video_index < 0)
            m->video_index = i;
        else if (t == AVMEDIA_TYPE_AUDIO && m->audio_index < 0)
            m->audio_index = i;
    }
    if (m->video_index < 0)
        return AVERROR(EINVAL);

    st = s->streams[m->video_index];

    /* -mo_audio selection: fastaudio=0 adpcm=1 pcm=2 none=3 codebook=4.
     * codebook (retail SX) needs a per-channel codebook we cannot yet
     * generate (FFmpeg ships an SX decoder but no encoder). Reject early
     * rather than emit a file the DS will choke on. */
    if (m->audio_codec == 4) {
        av_log(s, AV_LOG_ERROR,
               "mods: -mo_audio codebook (FastAudio/SX) is not supported by "
               "this muxer yet (no SX encoder / codebook generator). Use "
               "adpcm, fastaudio, pcm, or none.\n");
        return AVERROR(ENOSYS);
    }

    no_audio = (m->audio_codec == 3) || m->audio_index < 0;
    if (no_audio) {
        retail_codec  = MODS_ACODEC_NONE;
        m->channels   = 0;
    } else {
        AVCodecParameters *ap = s->streams[m->audio_index]->codecpar;
        if (m->audio_codec < 0)
            m->audio_codec = MOBI_AUDIO_ADPCM;
        m->channels = ap->ch_layout.nb_channels ? ap->ch_layout.nb_channels : 2;
        switch (m->audio_codec) {
        case MOBI_AUDIO_FASTAUDIO:
            retail_codec   = MODS_ACODEC_FASTAUDIO;
            m->audio_block = m->channels * 40;   /* 256 samples/block */
            break;
        case MOBI_AUDIO_PCM:
            retail_codec   = MODS_ACODEC_PCM;
            m->audio_block = m->channels * 256 * 2; /* 256 samples/block, s16 */
            break;
        case MOBI_AUDIO_ADPCM:
        default:
            retail_codec   = MODS_ACODEC_ADPCM;
            m->audio_block = m->channels * 132;  /* (unused on the DS path) */
            m->ds_adpcm    = 1;                  /* use native DS IMA encoder */
            break;
        }
        /* All three codecs use 256-sample blocks; pace ~rate/fps samples/frame. */
        m->block_samples = 256;
        {
            AVRational vfr = st->avg_frame_rate;
            double fps_val = (vfr.num > 0 && vfr.den > 0)
                           ? (double)vfr.num / vfr.den : 30.0;
            int rate = ap->sample_rate ? ap->sample_rate : 32000;
            m->spf = (double)rate / fps_val;
        }
    }

    /* Magic + video codec id (0x000A). */
    avio_write(pb, "MODSN3\x0a\x00", 8);

    avio_wl32(pb, 0);                       /* 0x08 frame count (patched) */
    avio_wl32(pb, st->codecpar->width);     /* 0x0C */
    avio_wl32(pb, st->codecpar->height);    /* 0x10 */

    fps = st->avg_frame_rate;
    if (fps.num > 0 && fps.den > 0)
        avio_wl32(pb, (uint32_t)((uint64_t)fps.num * 0x1000000ULL / fps.den));
    else
        avio_wl32(pb, 0x1E00000);           /* 0x14 */

    avio_wl16(pb, retail_codec);            /* 0x18 audio codec */
    avio_wl16(pb, no_audio ? 0 : m->channels); /* 0x1A channels */
    if (!no_audio) {
        AVCodecParameters *ap = s->streams[m->audio_index]->codecpar;
        avio_wl32(pb, ap->sample_rate ? ap->sample_rate : 32000); /* 0x1C */
    } else {
        avio_wl32(pb, 0);                   /* 0x1C */
    }

    avio_wl32(pb, 0);                       /* 0x20 max frame size (patched) */
    avio_wl32(pb, 0);                       /* 0x24 audio-codec-info off (none/0) */
    avio_wl32(pb, 0);                       /* 0x28 key-frame table off (patched) */
    avio_wl32(pb, 0);                       /* 0x2C key-frame count (patched) */
    avio_wl32(pb, FORMAT_HEADER_DONE);      /* 0x30 'HE' header-done marker */

    if (!no_audio && !m->ds_adpcm) {
        AVCodecParameters *ap = s->streams[m->audio_index]->codecpar;
        int ret = mobi_aenc_init(&m->aenc, m->audio_codec,
                                 AV_CODEC_ID_ADPCM_IMA_MOFLEX,
                                 &ap->ch_layout, ap->sample_rate,
                                 /* fastaudio_ds_mods = */ 1);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to init MODS audio encoder\n");
            return ret;
        }
        m->aenc_inited = 1;
    }

    m->frame_count = 0;
    return 0;
}

static int mods_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MODSMuxContext *m = s->priv_data;

    if (m->ds_adpcm && pkt->stream_index == m->audio_index) {
        /* Buffer raw interleaved S16 PCM; encoded to DS IMA at frame flush. */
        int nb = pkt->size / (m->channels * (int)sizeof(int16_t));
        if (nb > 0) {
            int need = m->pcm_nsamp + nb;
            if (need > m->pcm_cap) {
                int nc = m->pcm_cap ? m->pcm_cap * 2 : 8192;
                while (nc < need) nc *= 2;
                int16_t *nbuf = av_realloc(m->pcmbuf,
                                           (size_t)nc * m->channels * sizeof(int16_t));
                if (!nbuf)
                    return AVERROR(ENOMEM);
                m->pcmbuf = nbuf; m->pcm_cap = nc;
            }
            memcpy(m->pcmbuf + (size_t)m->pcm_nsamp * m->channels,
                   pkt->data, (size_t)nb * m->channels * sizeof(int16_t));
            m->pcm_nsamp += nb;
        }
        return 0;
    }

    if (m->aenc_inited && pkt->stream_index == m->audio_index) {
        int nb = pkt->size / (m->aenc.channels * (int)sizeof(int16_t));
        if (nb > 0) {
            int ret = mobi_aenc_send_pcm(&m->aenc, (const int16_t *)pkt->data, nb);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    if (pkt->stream_index != m->video_index)
        return 0;

    uint8_t *stripped = NULL;
    int stripped_size = 0;
    const uint8_t *data = pkt->data;
    int size = pkt->size;

    if (ff_has_annexb_startcode(pkt->data, pkt->size)) {
        int ret = ff_extract_mobiclip_payload(pkt->data, pkt->size,
                                              &stripped, &stripped_size);
        if (ret < 0)
            return ret;
        data = stripped;
        size = stripped_size;
    }

    if (size > 0x3FFFF) {
        av_free(stripped);
        return AVERROR(EINVAL);
    }

    int ret = mods_flush_pending(s, 0);
    if (ret < 0) { av_free(stripped); return ret; }

    m->pend_vid = av_malloc(size ? size : 1);
    if (!m->pend_vid) { av_free(stripped); return AVERROR(ENOMEM); }
    memcpy(m->pend_vid, data, size);
    m->pend_vid_size  = size;
    m->has_pend       = 1;
    /* Keyframe-ness MUST match the actual intra-frame bit in the bitstream, not
     * AV_PKT_FLAG_KEY: x264 flags only IDR frames, but scene-cut intra frames
     * are coded I without the flag, so the flag undercounts real I-frames and
     * MO_GetNbIFrame would disagree with the stream. The decoder bswap16's the
     * bitstream then reads MSB-first, so the I-frame bit is bit 7 of byte 1. */
    m->pend_keyframe  = (size >= 2 && (data[1] & 0x80)) ? 1 : 0;

    av_free(stripped);
    return 0;
}

static int mods_write_trailer(AVFormatContext *s)
{
    MODSMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t kf_off;

    mods_flush_pending(s, 1);

    /* Key-frame table: [u32 frame_number][u32 data_offset] pairs. */
    kf_off = avio_tell(pb);
    for (int i = 0; i < m->kf_count; i++) {
        avio_wl32(pb, m->kf[i * 2 + 0]);
        avio_wl32(pb, m->kf[i * 2 + 1]);
    }

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        int64_t cur = avio_tell(pb);
        avio_seek(pb, MODS_OFF_FRAME_COUNT, SEEK_SET);
        avio_wl32(pb, m->frame_count);
        avio_seek(pb, MODS_OFF_MAX_FRAME, SEEK_SET);
        avio_wl32(pb, m->max_chunk + 4);     /* largest read size incl. word */
        avio_seek(pb, MODS_OFF_KF_OFF, SEEK_SET);
        avio_wl32(pb, (uint32_t)kf_off);
        avio_wl32(pb, m->kf_count);          /* 0x2C immediately follows */
        avio_seek(pb, cur, SEEK_SET);
    } else {
        av_log(s, AV_LOG_WARNING,
               "mods: non-seekable output; header offsets left unpatched.\n");
    }

    return 0;
}

static void mods_deinit(AVFormatContext *s)
{
    MODSMuxContext *m = s->priv_data;
    if (m->aenc_inited)
        mobi_aenc_close(&m->aenc);
    av_freep(&m->pend_vid);
    av_freep(&m->abuf);
    av_freep(&m->kf);
    av_freep(&m->pcmbuf);
    av_freep(&m->ds_s0);
    av_freep(&m->ds_s1);
}

#include "libavutil/opt.h"
#define OFFSET(x) offsetof(MODSMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption mods_options[] = {
    { "mo_audio", "Mobiclip audio codec", OFFSET(audio_codec), AV_OPT_TYPE_INT, {.i64 = -1}, -1, 4, ENC, "mo_audio" },
    { "fastaudio", "FastAudio",        0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, ENC, "mo_audio" },
    { "adpcm",     "IMA-ADPCM",        0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, ENC, "mo_audio" },
    { "pcm",       "PCM16",            0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, ENC, "mo_audio" },
    { "none",      "No audio",         0, AV_OPT_TYPE_CONST, {.i64 = 3}, 0, 0, ENC, "mo_audio" },
    { "codebook",  "FastAudio/SX (unsupported)", 0, AV_OPT_TYPE_CONST, {.i64 = 4}, 0, 0, ENC, "mo_audio" },
    { NULL },
};

static const AVClass mods_muxer_class = {
    .class_name = "mods",
    .item_name  = av_default_item_name,
    .option     = mods_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_mods_muxer = {
    .p.name           = "mods",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Mobiclip MODS"),
    .p.extensions     = "mods",
    .priv_data_size = sizeof(MODSMuxContext),
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_MOBICLIP,
    .write_header   = mods_write_header,
    .write_packet   = mods_write_packet,
    .write_trailer  = mods_write_trailer,
    .deinit         = mods_deinit,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &mods_muxer_class,
};
