/*
 * MIVF IA4M ADPCM decoder
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
 * IA4M, the mono IMA ADPCM of MIVF files. Each packet is one video
 * frame's worth of sound and stands alone:
 *   "IA4M" | frame u32 | samples u16 | channels u8 (1) | 0 |
 *   first sample s16 | step index u8 | 0 | nibble bytes u32 | nibbles
 * The first sample is stored as is; the rest are low-nibble-first IMA codes.
 */

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"

#include "adpcm_data.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

static av_cold int ia4m_decode_init(AVCodecContext *avctx)
{
    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout  = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    return 0;
}

static int ia4m_nibble(int nibble, int *predictor, int *index)
{
    const int step = ff_adpcm_step_table[*index];
    int diff = step >> 3;

    if (nibble & 4)
        diff += step;
    if (nibble & 2)
        diff += step >> 1;
    if (nibble & 1)
        diff += step >> 2;
    *predictor = av_clip_int16(nibble & 8 ? *predictor - diff : *predictor + diff);
    *index     = av_clip(*index + ff_adpcm_index_table[nibble], 0, 88);
    return *predictor;
}

static int ia4m_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *pkt)
{
    const uint8_t *p = pkt->data;
    int nb_samples, predictor, index, ret;
    uint32_t nibble_bytes;
    int16_t *dst;

    if (pkt->size < 20 || AV_RL32(p) != MKTAG('I', 'A', '4', 'M') || p[10] != 1)
        return AVERROR_INVALIDDATA;
    nb_samples   = AV_RL16(p + 8);
    predictor    = (int16_t)AV_RL16(p + 12);
    index        = av_clip(p[14], 0, 88);
    nibble_bytes = AV_RL32(p + 16);
    if (!nb_samples || nibble_bytes > pkt->size - 20)
        return AVERROR_INVALIDDATA;

    frame->nb_samples = nb_samples;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    dst = (int16_t *)frame->data[0];

    /* Samples past the coded ones repeat the last value, as the player's
     * buffer would hold them. */
    dst[0] = predictor;
    for (int i = 1; i < nb_samples; i++) {
        const int k = i - 1;
        dst[i] = k / 2 < nibble_bytes
               ? ia4m_nibble(p[20 + k / 2] >> (k & 1 ? 4 : 0) & 15, &predictor, &index)
               : predictor;
    }
    *got_frame = 1;
    return pkt->size;
}

const FFCodec ff_adpcm_ima_mivf_decoder = {
    .p.name         = "adpcm_ima_mivf",
    CODEC_LONG_NAME("ADPCM IMA MIVF (IA4M)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ADPCM_IMA_MIVF,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .init           = ia4m_decode_init,
    FF_CODEC_DECODE_CB(ia4m_decode_frame),
};
