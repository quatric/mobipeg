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

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"

#include "dsp_adpcm.h"

int64_t ff_dsp_adpcm_frame_count(int64_t samples)
{
    return (samples + FF_DSP_ADPCM_SAMPLES_PER_FRAME - 1) /
           FF_DSP_ADPCM_SAMPLES_PER_FRAME;
}

int64_t ff_dsp_adpcm_byte_count(int64_t samples)
{
    return ff_dsp_adpcm_frame_count(samples) * FF_DSP_ADPCM_BYTES_PER_FRAME;
}

int64_t ff_dsp_adpcm_nibble_count(int64_t samples)
{
    int64_t whole = samples / FF_DSP_ADPCM_SAMPLES_PER_FRAME;
    int64_t rest  = samples % FF_DSP_ADPCM_SAMPLES_PER_FRAME;

    /* A trailing partial frame still carries its predictor/scale byte; an
     * exactly-full stream is not padded with an empty frame's worth. */
    return whole * 16 + (rest ? rest + 2 : 0);
}

int64_t ff_dsp_adpcm_nibble_address(int64_t sample)
{
    return sample / FF_DSP_ADPCM_SAMPLES_PER_FRAME * 16 +
           sample % FF_DSP_ADPCM_SAMPLES_PER_FRAME + 2;
}

int64_t ff_dsp_adpcm_nibbles_to_samples(int64_t nibbles)
{
    int64_t frames = nibbles / 16;
    int64_t rest   = nibbles % 16;

    /* Nibbles 0 and 1 of a frame are its header, so an address pointing at
     * them names the frame's first sample. */
    return frames * FF_DSP_ADPCM_SAMPLES_PER_FRAME + FFMAX(rest - 2, 0);
}

/* libavcodec's sign_extend() lives in mathops.h, which libavformat does not
 * pull in; the nibbles here are 4-bit two's complement. */
static inline int nibble_value(int nibble)
{
    return nibble > 7 ? nibble - 16 : nibble;
}

void ff_dsp_adpcm_advance(const uint8_t *src, int64_t nb_frames,
                          const int16_t *coefs, int16_t *hist1, int16_t *hist2)
{
    int s1 = *hist1, s2 = *hist2;

    for (int64_t f = 0; f < nb_frames; f++) {
        int header  = *src++;
        int index   = (header >> 4) & 7;
        unsigned e  = header & 0x0F;
        int factor1 = coefs[index * 2];
        int factor2 = coefs[index * 2 + 1];

        for (int n = 0; n < FF_DSP_ADPCM_SAMPLES_PER_FRAME; n++) {
            int byte = src[n >> 1];
            int nib  = nibble_value((n & 1) ? byte & 0x0F : byte >> 4);
            int sample = ((s1 * factor1 + s2 * factor2) >> 11) +
                         nib * (1 << e);

            sample = av_clip_int16(sample);
            s2 = s1;
            s1 = sample;
        }
        src += FF_DSP_ADPCM_BYTES_PER_FRAME - 1;
    }

    *hist1 = s1;
    *hist2 = s2;
}
