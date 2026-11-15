/*
 * FastVideoDS ADPCM decoder and encoder
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

#include "config_components.h"

#include "libavutil/channel_layout.h"

#include "adpcm_fvds.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"

#if CONFIG_ADPCM_IMA_FVDS_DECODER
static av_cold int adpcm_fvds_decode_init(AVCodecContext *avctx)
{
    if (avctx->ch_layout.nb_channels < 1 || avctx->ch_layout.nb_channels > 2)
        return AVERROR(EINVAL);
    avctx->sample_fmt = AV_SAMPLE_FMT_S16P;
    return 0;
}

static int adpcm_fvds_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                   int *got_frame, AVPacket *pkt)
{
    const int channels = avctx->ch_layout.nb_channels;
    const int blocks = pkt->size / (channels * FVDS_ADPCM_BLOCK_SIZE);
    const uint8_t *src = pkt->data;
    int ret;

    if (!blocks)
        return AVERROR_INVALIDDATA;
    frame->nb_samples = blocks * FVDS_ADPCM_SAMPLES;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int i = 0; i < blocks; i++)
        for (int ch = 0; ch < channels; ch++, src += FVDS_ADPCM_BLOCK_SIZE)
            fvds_adpcm_decode_block(src, (int16_t *)frame->extended_data[ch] +
                                         i * FVDS_ADPCM_SAMPLES, NULL);
    *got_frame = 1;
    return blocks * channels * FVDS_ADPCM_BLOCK_SIZE;
}

const FFCodec ff_adpcm_ima_fvds_decoder = {
    .p.name         = "adpcm_ima_fvds",
    CODEC_LONG_NAME("ADPCM IMA FastVideoDS"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ADPCM_IMA_FVDS,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .init           = adpcm_fvds_decode_init,
    FF_CODEC_DECODE_CB(adpcm_fvds_decode_frame),
};
#endif

#if CONFIG_ADPCM_IMA_FVDS_ENCODER
typedef struct FVDSADPCMEncContext {
    FVDSADPCMState state[2];
} FVDSADPCMEncContext;

static av_cold int adpcm_fvds_encode_init(AVCodecContext *avctx)
{
    avctx->frame_size = FVDS_ADPCM_SAMPLES;
    avctx->bit_rate   = (int64_t)avctx->ch_layout.nb_channels * FVDS_ADPCM_BLOCK_SIZE * 8 *
                        avctx->sample_rate / FVDS_ADPCM_SAMPLES;
    return 0;
}

static int adpcm_fvds_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                   const AVFrame *frame, int *got_packet)
{
    FVDSADPCMEncContext *s = avctx->priv_data;
    const int channels = avctx->ch_layout.nb_channels;
    int ret;

    /* the last frame comes padded with silence to a whole block */
    if (frame->nb_samples != FVDS_ADPCM_SAMPLES)
        return AVERROR(EINVAL);
    if ((ret = ff_get_encode_buffer(avctx, pkt, channels * FVDS_ADPCM_BLOCK_SIZE, 0)) < 0)
        return ret;
    for (int ch = 0; ch < channels; ch++)
        fvds_adpcm_encode_block((const int16_t *)frame->extended_data[ch],
                                pkt->data + ch * FVDS_ADPCM_BLOCK_SIZE, &s->state[ch]);
    *got_packet = 1;
    return 0;
}

const FFCodec ff_adpcm_ima_fvds_encoder = {
    .p.name         = "adpcm_ima_fvds",
    CODEC_LONG_NAME("ADPCM IMA FastVideoDS"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ADPCM_IMA_FVDS,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(FVDSADPCMEncContext),
    .init           = adpcm_fvds_encode_init,
    FF_CODEC_ENCODE_CB(adpcm_fvds_encode_frame),
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16P),
    CODEC_SAMPLERATES(FVDS_ADPCM_SAMPLE_RATE),
    CODEC_CH_LAYOUTS(AV_CHANNEL_LAYOUT_STEREO),
};
#endif
