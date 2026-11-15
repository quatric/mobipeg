/*
 * FastVideoDS ADPCM
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
 * The ADPCM of FastVideoDS (.fv) files: IMA steps with its own index
 * adaptation and no rounding term.  A block is the 16 bit sample and the 16
 * bit step index it starts from, then 256 nibbles, the low one of each byte
 * first; a frame is a block for the left channel and one for the right.
 *
 * The coder is the one of Gericom's FastVideoDSEncoder, so that the same
 * samples give the same bytes.  It lives here because the muxer needs it as
 * well, to go on coding silence where the audio ends before the video.
 */

#ifndef AVCODEC_ADPCM_FVDS_H
#define AVCODEC_ADPCM_FVDS_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"

#define FVDS_ADPCM_SAMPLES     256
#define FVDS_ADPCM_BLOCK_SIZE  (4 + FVDS_ADPCM_SAMPLES / 2)
#define FVDS_ADPCM_SAMPLE_RATE 47605

typedef struct FVDSADPCMState {
    int started;    /* 0 until a block has been coded */
    int last;
    int index;
} FVDSADPCMState;

static const int8_t fvds_adpcm_index[8] = { -3, -3, -2, -1, 2, 4, 6, 8 };

static const int16_t fvds_adpcm_step[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static av_always_inline int fvds_adpcm_expand(int last, int step, int nibble)
{
    const int diff = (step * ((nibble & 7) * 2 + 1)) >> 3;

    return av_clip(nibble & 8 ? last - diff : last + diff, -0x7FFF, 0x7FFF);
}

/* Decode one block into FVDS_ADPCM_SAMPLES samples; with a state, leave in it
 * what the next block of the same coder starts from. */
static inline void fvds_adpcm_decode_block(const uint8_t *src, int16_t *dst,
                                           FVDSADPCMState *state)
{
    int last  = (int16_t)AV_RL16(src);
    int index = FFMIN(AV_RL16(src + 2), 88);

    for (int i = 0; i < FVDS_ADPCM_SAMPLES; i++) {
        const int nibble = (src[4 + (i >> 1)] >> (4 * (i & 1))) & 15;

        last  = fvds_adpcm_expand(last, fvds_adpcm_step[index], nibble);
        index = av_clip(index + fvds_adpcm_index[nibble & 7], 0, 88);
        if (dst)
            dst[i] = last;
    }
    if (state) {
        state->started = 1;
        state->last    = last;
        state->index   = index;
    }
}

/* Code FVDS_ADPCM_SAMPLES samples into one block.  Each nibble is the one
 * that leaves the least error on its sample and, with the best nibble after
 * it, on the next one of the block; the first of equals wins. */
static inline void fvds_adpcm_encode_block(const int16_t *samples, uint8_t *dst,
                                           FVDSADPCMState *state)
{
    int last, index;

    if (!state->started) {
        /* start a few steps above the one nearest the first difference, and
         * that far below the first sample */
        const int diff = abs(samples[1] - samples[0]);
        int best = INT_MAX;

        index = 0;
        for (int i = 0; i < 89; i++) {
            const int d = abs(diff - fvds_adpcm_step[i]);
            if (d < best) {
                best  = d;
                index = i;
            }
        }
        index = FFMIN(index + 3, 88);
        last  = FFMAX(-0x7FFF, samples[0] - fvds_adpcm_step[index] / 8);
    } else {
        last  = state->last;
        index = state->index;
    }

    AV_WL16(dst, last);
    AV_WL16(dst + 2, index);
    memset(dst + 4, 0, FVDS_ADPCM_SAMPLES / 2);

    for (int i = 0; i < FVDS_ADPCM_SAMPLES; i++) {
        const int step = fvds_adpcm_step[index];
        int best = 0, best_score = INT_MAX;

        for (int k = 0; k < 16; k++) {
            const int value = fvds_adpcm_expand(last, step, k);
            int score = abs(value - samples[i]);

            if (i + 1 < FVDS_ADPCM_SAMPLES) {
                const int step2 =
                    fvds_adpcm_step[av_clip(index + fvds_adpcm_index[k & 7], 0, 88)];
                int best2 = INT_MAX;

                for (int l = 0; l < 16; l++) {
                    const int score2 =
                        abs(fvds_adpcm_expand(value, step2, l) - samples[i + 1]);
                    if (score2 < best2)
                        best2 = score2;
                }
                score += best2;
            }
            if (score < best_score) {
                best_score = score;
                best       = k;
            }
        }
        dst[4 + (i >> 1)] |= best << (4 * (i & 1));
        last  = fvds_adpcm_expand(last, step, best);
        index = av_clip(index + fvds_adpcm_index[best & 7], 0, 88);
    }
    state->started = 1;
    state->last    = last;
    state->index   = index;
}

#endif /* AVCODEC_ADPCM_FVDS_H */
