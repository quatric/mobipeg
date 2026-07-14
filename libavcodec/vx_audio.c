/*
 * MobiClip / ActImagine DS (.vx) Audio decoder
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
 *
 * The VX container interleaves one video frame's bits followed by N 128-sample
 * audio frames (AFrames) in a single word-aligned bitstream; the demuxer hands
 * this decoder the raw payload prefixed with a 4-byte AFrame count. To find
 * where the audio bits start, this decoder replays the video bit-consumption
 * using the shared ff_vx_decode_vframe() core into a throwaway
 * buffer (bit position only depends on encoded values, not on reference pixel
 * content, so a zeroed scratch frame is fine for this purpose).
 *
 * The audio codec itself is a custom 128-sample-per-frame LPC/pulse codec
 * with state (LPC filter, scale, and the last two frames' pulses/samples)
 * carried across AFrames for the lifetime of the stream, reverse engineered
 * from the actimagine Python reference decoder
 * (https://github.com/CharlesVanEeckhout/actimagine).
 */

#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "vx.h"

#define AFRAME_SAMPLES 128

typedef struct AudioExtraData {
    int16_t lpc_codebooks[3][64][8];
    uint16_t scale_modifiers[8];
    int32_t lpc_base[8];
    uint32_t scale_initial;
} AudioExtraData;

static const uint8_t pulse_values_len[4] = { 42, 40, 32, 24 };
static const uint8_t pulse_data_len[4]   = { 8, 5, 4, 3 };
static const uint8_t pulse_distance[4]   = { 3, 3, 4, 5 };

typedef struct VXAState {
    AudioExtraData ed;

    /* per-channel state carried across AFrames */
    int have_prev, have_prev2;
    int pulses_prev[AFRAME_SAMPLES];
    int pulses_prev2[AFRAME_SAMPLES];
    int samples_prev[8];
    int scale_prev;
    int lpc_filter_prev[8];
    int influence_prev[8];
} VXAState;

typedef struct VXAudioContext {
    int width, height;          /* 0x0: no leading video bitstream (.mods) */
    int nch;
    uint16_t qtab[3];
    VXAState st[2];

    uint8_t *scratch_y, *scratch_u, *scratch_v;
    int scratch_ls_y, scratch_ls_uv;
} VXAudioContext;

static void parse_one_extradata(AudioExtraData *ed, const uint8_t *p)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 64; j++)
            for (int k = 0; k < 8; k++) {
                ed->lpc_codebooks[i][j][k] = (int16_t)AV_RL16(p);
                p += 2;
            }
    for (int i = 0; i < 8; i++) { ed->scale_modifiers[i] = AV_RL16(p); p += 2; }
    for (int i = 0; i < 8; i++) { ed->lpc_base[i] = (int32_t)AV_RL32(p); p += 4; }
    ed->scale_initial = AV_RL32(p);
}

static av_cold int vx_audio_init(AVCodecContext *avctx)
{
    VXAudioContext *s = avctx->priv_data;
    int prefix, quantizer;

    /* extradata: [u32 quantizer][u32 width][u32 height][u32 channels]
     * followed by one 3124-byte codec-info block per channel. The legacy
     * mono layout (12-byte prefix, one block) is still accepted.
     * width == height == 0 means the packets carry no leading video
     * bitstream (.mods framing); otherwise the video bits are replayed to
     * locate the audio (.vx framing). */
    if (!avctx->extradata || avctx->extradata_size < 12 + (int)sizeof(AudioExtraData))
        return AVERROR_INVALIDDATA;

    quantizer = AV_RL32(avctx->extradata);
    s->width  = AV_RL32(avctx->extradata + 4);
    s->height = AV_RL32(avctx->extradata + 8);
    if (avctx->extradata_size == 12 + (int)sizeof(AudioExtraData)) {
        prefix = 12;
        s->nch = 1;
    } else {
        if (avctx->extradata_size < 16 + (int)sizeof(AudioExtraData))
            return AVERROR_INVALIDDATA;
        prefix = 16;
        s->nch = AV_RL32(avctx->extradata + 12);
    }
    if (s->nch < 1 || s->nch > 2 ||
        avctx->extradata_size < prefix + s->nch * (int)sizeof(AudioExtraData))
        return AVERROR_INVALIDDATA;

    s->qtab[0] = s->qtab[1] = s->qtab[2] = 0;
    if (s->width || s->height) {
        if (s->width <= 0 || s->height <= 0 || s->width % 16 || s->height % 16)
            return AVERROR_INVALIDDATA;
        if (ff_vx_calc_qtab(quantizer, s->qtab) < 0)
            return AVERROR_INVALIDDATA;

        s->scratch_ls_y  = FFALIGN(s->width, 16);
        s->scratch_ls_uv = FFALIGN(s->width / 2, 16);
        s->scratch_y = av_calloc((size_t)s->scratch_ls_y * s->height, 1);
        s->scratch_u = av_calloc((size_t)s->scratch_ls_uv * (s->height / 2), 1);
        s->scratch_v = av_calloc((size_t)s->scratch_ls_uv * (s->height / 2), 1);
        if (!s->scratch_y || !s->scratch_u || !s->scratch_v)
            return AVERROR(ENOMEM);
    }

    for (int c = 0; c < s->nch; c++)
        parse_one_extradata(&s->st[c].ed,
                            avctx->extradata + prefix + c * sizeof(AudioExtraData));

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_uninit(&avctx->ch_layout);
    av_channel_layout_default(&avctx->ch_layout, s->nch);

    return 0;
}

static av_cold int vx_audio_close(AVCodecContext *avctx)
{
    VXAudioContext *s = avctx->priv_data;
    av_freep(&s->scratch_y);
    av_freep(&s->scratch_u);
    av_freep(&s->scratch_v);
    return 0;
}

static av_cold void vx_audio_flush(AVCodecContext *avctx)
{
    VXAudioContext *s = avctx->priv_data;
    for (int c = 0; c < s->nch; c++)
        s->st[c].have_prev = s->st[c].have_prev2 = 0;
}

static void unpack_pulse_values(int mode, const uint16_t *pulse_data, int *out, int *out_qty)
{
    int n = 0;

    if (mode == 0) {
        for (int i = 0; i < 8; i++)
            for (int j = 16 - 3; j >= 0; j -= 3)
                out[n++] = (pulse_data[i] >> j) & 0x7;
        out[n++] = ((pulse_data[0] & 1) << 2) | ((pulse_data[1] & 1) << 1) | (pulse_data[2] & 1);
        out[n++] = ((pulse_data[3] & 1) << 2) | ((pulse_data[4] & 1) << 1) | (pulse_data[5] & 1);
        for (int i = 0; i < n; i++)
            out[i] = out[i] * 2 - 7;
    } else {
        int words = pulse_data_len[mode];
        for (int i = 0; i < words; i++)
            for (int j = 16 - 2; j >= 0; j -= 2)
                out[n++] = (pulse_data[i] >> j) & 0x3;
        for (int i = 0; i < n; i++)
            out[i] = out[i] * 2 - 3;
    }
    *out_qty = n;
}

/* Decode one 128-sample AFrame into out[] with the given interleave stride.
 * Returns 0 on success. */
static int decode_aframe(AVCodecContext *avctx, VXAState *s, GetBitContext *gb,
                         int16_t *out, int stride)
{
    int word1, word2;
    int prev_frame_offset, scale_modifier_index, pulse_start_position, pulse_packing_mode;
    int lpc_idx[3];
    uint16_t pulse_data[8];
    int pulse_values[42], pulse_qty;
    int distance;
    int pulses[AFRAME_SAMPLES];
    int samples[AFRAME_SAMPLES];
    int scale;
    int lpc_filter[8];
    int influence[8];
    int quarters[4][8];

    if (get_bits_left(gb) < 32)
        return AVERROR_INVALIDDATA;

    word1 = get_bits(gb, 16);
    word2 = get_bits(gb, 16);

    prev_frame_offset    = (word1 >> 9) & 0x7f;
    scale_modifier_index = (word1 >> 6) & 0x7;
    pulse_start_position  = (word2 >> 14) & 0x3;
    pulse_packing_mode    = (word2 >> 12) & 0x3;
    lpc_idx[0] = word1 & 0x3f;
    lpc_idx[1] = (word2 >> 6) & 0x3f;
    lpc_idx[2] = word2 & 0x3f;

    if (get_bits_left(gb) < 16 * pulse_data_len[pulse_packing_mode])
        return AVERROR_INVALIDDATA;
    for (int i = 0; i < pulse_data_len[pulse_packing_mode]; i++)
        pulse_data[i] = get_bits(gb, 16);
    unpack_pulse_values(pulse_packing_mode, pulse_data, pulse_values, &pulse_qty);

    if (prev_frame_offset != 0x7f && !s->have_prev)
        return AVERROR_INVALIDDATA;

    scale = s->have_prev ? s->scale_prev : (int)s->ed.scale_initial;
    if (prev_frame_offset == 0x7f)
        scale = (int)s->ed.scale_initial;
    scale = (scale * (int)s->ed.scale_modifiers[scale_modifier_index]) >> 13;

    if (prev_frame_offset == 0x7f) {
        for (int i = 0; i < 8; i++) lpc_filter[i] = s->ed.lpc_base[i];
    } else {
        for (int i = 0; i < 8; i++) lpc_filter[i] = s->lpc_filter_prev[i];
    }
    for (int k = 0; k < 8; k++) {
        int sum = 0;
        for (int i = 0; i < 3; i++)
            sum += s->ed.lpc_codebooks[i][lpc_idx[i]][k];
        lpc_filter[k] += sum;
    }

    distance = pulse_distance[pulse_packing_mode];

    if (prev_frame_offset < 0x7e) {
        int concat[AFRAME_SAMPLES * 2];
        for (int i = 0; i < AFRAME_SAMPLES; i++)
            concat[i] = s->have_prev2 ? s->pulses_prev2[i] : 0;
        for (int i = 0; i < AFRAME_SAMPLES; i++)
            concat[AFRAME_SAMPLES + i] = s->have_prev ? s->pulses_prev[i] : 0;
        for (int i = 0; i < AFRAME_SAMPLES; i++) {
            int volume = FFMIN3(8, i + 1, AFRAME_SAMPLES - i);
            pulses[i] = (concat[i + 0x7f - prev_frame_offset] * volume) >> 4;
        }
    } else {
        memset(pulses, 0, sizeof(pulses));
    }

    for (int i = 0; i < AFRAME_SAMPLES; i++) {
        int diff = i - pulse_start_position;
        if (diff >= 0 && diff % distance == 0) {
            int idx = diff / distance;
            if (idx < pulse_qty)
                pulses[i] += pulse_values[idx] * scale;
        }
    }

    /* prev_sample_influence: recursive lattice-style transform of lpc_filter */
    {
        int psi[8], old[8];
        int len = 0;
        for (int i = 0; i < 8; i++) {
            int coeff = lpc_filter[i];
            memcpy(old, psi, sizeof(int) * len);
            for (int j = 0; j < i; j++)
                psi[j] = old[j] + (int)(((int64_t)old[i - j - 1] * coeff) >> 15);
            psi[i] = coeff;
            len = i + 1;
        }
        for (int i = 0; i < 8; i++)
            influence[i] = -(psi[i] >> 1);
    }

    if (prev_frame_offset != 0x7f) {
        for (int j = 0; j < 8; j++) quarters[3][j] = influence[j];
        for (int j = 0; j < 8; j++) quarters[1][j] = (s->influence_prev[j] + quarters[3][j]) >> 1;
        for (int j = 0; j < 8; j++) quarters[0][j] = (s->influence_prev[j] + quarters[1][j]) >> 1;
        for (int j = 0; j < 8; j++) quarters[2][j] = (quarters[1][j] + quarters[3][j]) >> 1;
    } else {
        for (int q = 0; q < 4; q++)
            for (int j = 0; j < 8; j++)
                quarters[q][j] = influence[j];
    }

    for (int i = 0; i < AFRAME_SAMPLES; i++) {
        const int *inf = quarters[i * 4 / AFRAME_SAMPLES];
        int64_t sample = (int64_t)pulses[i] * 0x4000;
        for (int j = 0; j < 8; j++) {
            int si = i - 1 - j;
            int prev_sample = (si < 0) ? s->samples_prev[8 + si] : samples[si];
            sample += (int64_t)prev_sample * inf[j];
        }
        samples[i] = (int)(sample >> 14);
    }

    for (int i = 0; i < AFRAME_SAMPLES; i++)
        out[i * stride] = av_clip_int16(samples[i]);

    memcpy(s->pulses_prev2, s->pulses_prev, sizeof(s->pulses_prev2));
    s->have_prev2 = s->have_prev;
    memcpy(s->pulses_prev, pulses, sizeof(s->pulses_prev));
    memcpy(s->samples_prev, &samples[AFRAME_SAMPLES - 8], sizeof(s->samples_prev));
    s->scale_prev = scale;
    memcpy(s->lpc_filter_prev, lpc_filter, sizeof(s->lpc_filter_prev));
    memcpy(s->influence_prev, influence, sizeof(s->influence_prev));
    s->have_prev = 1;

    return 0;
}

static int vx_audio_decode(AVCodecContext *avctx, AVFrame *frame,
                                     int *got_frame, AVPacket *pkt)
{
    VXAudioContext *s = avctx->priv_data;
    GetBitContext gb;
    VXPic dst = { { 0 } }, refs[3] = { { { 0 } } };
    uint32_t nb_aframes;
    uint8_t *bitstream;
    int ret;

    if (pkt->size < 4)
        return AVERROR_INVALIDDATA;
    nb_aframes = AV_RL32(pkt->data);
    if (nb_aframes == 0) {
        *got_frame = 0;
        return pkt->size;
    }

    bitstream = av_malloc(FFALIGN(pkt->size - 4, 2));
    if (!bitstream)
        return AVERROR(ENOMEM);
    memcpy(bitstream, pkt->data + 4, pkt->size - 4);
    if ((pkt->size - 4) & 1)
        bitstream[pkt->size - 4] = 0;
    for (int i = 0; i + 1 < FFALIGN(pkt->size - 4, 2); i += 2)
        FFSWAP(uint8_t, bitstream[i], bitstream[i + 1]);

    ret = init_get_bits8(&gb, bitstream, FFALIGN(pkt->size - 4, 2));
    if (ret < 0)
        goto end;

    if (s->width) {
        memset(s->scratch_y, 0, (size_t)s->scratch_ls_y * s->height);
        memset(s->scratch_u, 0, (size_t)s->scratch_ls_uv * (s->height / 2));
        memset(s->scratch_v, 0, (size_t)s->scratch_ls_uv * (s->height / 2));
        dst.data[0] = s->scratch_y; dst.linesize[0] = s->scratch_ls_y;
        dst.data[1] = s->scratch_u; dst.linesize[1] = s->scratch_ls_uv;
        dst.data[2] = s->scratch_v; dst.linesize[2] = s->scratch_ls_uv;

        /* We only replay the video to advance the bit reader to where the audio
         * frames begin; the resulting pixels are thrown away. But inter-coded
         * frames (Mega Man ZX etc.) branch through predict_inter, which bails
         * out on a NULL reference and would abort the replay before the audio
         * bits. The number of bits consumed does not depend on reference pixel
         * content (only on the bitstream and in-bounds MVs), so point every
         * reference at the same zeroed scratch planes to keep the replay
         * flowing. (.mods packets carry no video bits: width == 0 skips this.) */
        for (int i = 0; i < 3; i++) {
            refs[i].data[0] = s->scratch_y; refs[i].linesize[0] = s->scratch_ls_y;
            refs[i].data[1] = s->scratch_u; refs[i].linesize[1] = s->scratch_ls_uv;
            refs[i].data[2] = s->scratch_v; refs[i].linesize[2] = s->scratch_ls_uv;
        }

        ret = ff_vx_decode_vframe(avctx, &gb, s->width, s->height, s->qtab, &dst, refs);
        if (ret < 0)
            goto end;
    }

    if (nb_aframes % s->nch) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    frame->nb_samples = nb_aframes / s->nch * AFRAME_SAMPLES;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        goto end;

    /* AFrames interleave ch0,ch1 within the packet */
    for (uint32_t i = 0; i < nb_aframes; i++) {
        int c = i % s->nch, b = i / s->nch;
        int16_t *out = (int16_t *)frame->data[0] +
                       (size_t)b * AFRAME_SAMPLES * s->nch + c;
        ret = decode_aframe(avctx, &s->st[c], &gb, out, s->nch);
        if (ret < 0)
            goto end;
    }

    *got_frame = 1;
    ret = pkt->size;

end:
    av_free(bitstream);
    return ret;
}

const FFCodec ff_vx_audio_decoder = {
    .p.name         = "vx_audio",
    CODEC_LONG_NAME("ActImagine VX Audio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_VX_AUDIO,
    .priv_data_size = sizeof(VXAudioContext),
    .init           = vx_audio_init,
    .close          = vx_audio_close,
    FF_CODEC_DECODE_CB(vx_audio_decode),
    .flush          = vx_audio_flush,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
