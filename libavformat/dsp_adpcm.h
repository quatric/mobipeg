/*
 * Nintendo GameCube/Wii/3DS DSP-ADPCM container helpers
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
 * Sizing and state helpers shared by the containers that carry Nintendo
 * DSP-ADPCM (the codec ffmpeg calls adpcm_thp): .dsp, BRSTM/BFSTM/BCSTM,
 * BNS and AST.
 *
 * All of them store the same 8-byte / 14-sample frames and all of them have
 * to answer the same two questions: how many bytes does N samples take, and
 * what were the last two decoded samples at some point in the stream (the
 * "history", which these formats cache in their headers and seek tables so a
 * player can start mid-file without decoding from the beginning).
 */

#ifndef AVFORMAT_DSP_ADPCM_H
#define AVFORMAT_DSP_ADPCM_H

#include <stdint.h>

/** Samples carried by one 8-byte DSP-ADPCM frame. */
#define FF_DSP_ADPCM_SAMPLES_PER_FRAME 14
/** Size of one DSP-ADPCM frame: a predictor/scale byte plus 14 nibbles. */
#define FF_DSP_ADPCM_BYTES_PER_FRAME    8
/** Bytes of a bare .dsp header, and of each header in a multi-channel file. */
#define FF_DSP_ADPCM_HEADER_SIZE     0x60
/** Matches the adpcm_thp decoder's own channel cap (see adpcm.c). */
#define FF_DSP_ADPCM_MAX_CHANNELS      14

/** Frames needed to hold @p samples samples of one channel. */
int64_t ff_dsp_adpcm_frame_count(int64_t samples);

/** Bytes needed to hold @p samples samples of one channel. */
int64_t ff_dsp_adpcm_byte_count(int64_t samples);

/**
 * Nibble count for @p samples samples of one channel.
 *
 * Not simply samples/2: each frame spends two nibbles on its predictor/scale
 * byte, so a full frame is 16 nibbles for 14 samples. A trailing partial
 * frame still pays the two-nibble header, but an exactly-full one is not
 * followed by an empty frame.
 */
int64_t ff_dsp_adpcm_nibble_count(int64_t samples);

/** Nibble address at which @p sample of one channel starts. */
int64_t ff_dsp_adpcm_nibble_address(int64_t sample);

/** Inverse of ff_dsp_adpcm_nibble_address(), for reading loop points back. */
int64_t ff_dsp_adpcm_nibbles_to_samples(int64_t nibbles);

/**
 * Decode @p nb_frames frames for their side effect on the decoder state.
 *
 * The recurrence is the one in the adpcm_thp decoder, so the history this
 * leaves behind is bit-identical to what a console would hold at the same
 * point in the stream. Used to fill in the loop-point history of a .dsp/BNS
 * header and the per-block seek tables of BRSTM/BFSTM/BCSTM, neither of which
 * the encoder can produce itself: the block size that decides where the
 * samples are is the muxer's choice, not the encoder's.
 *
 * @param src       ADPCM frames for one channel, nb_frames * 8 bytes
 * @param nb_frames number of frames to run through
 * @param coefs     that channel's 16 Q11 predictor coefficients
 * @param hist1     in/out: previous decoded sample
 * @param hist2     in/out: the one before that
 */
void ff_dsp_adpcm_advance(const uint8_t *src, int64_t nb_frames,
                          const int16_t *coefs, int16_t *hist1, int16_t *hist2);

#endif /* AVFORMAT_DSP_ADPCM_H */
