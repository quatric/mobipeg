/*
 * MOFLEX Fast Audio decoder
 * Copyright (c) 2015-2016 Florian Nouwt
 * Copyright (c) 2017 Adib Surani
 * Copyright (c) 2020 Paul B Mahol
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

#include <float.h>
#include <math.h>

#include "libavutil/common.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "decode.h"

typedef struct ChannelItems {
    float f[8];
    float last;
    float history[65];      /* encoder: the input just before the sub-frame */
} ChannelItems;

typedef struct FastAudioContext {
    const AVClass *class;
    float table[8][64];

    ChannelItems *ch;

    /* encoder */
    double window[64 + 256];
    double lag_window[9];
    float gain[64];
} FastAudioContext;

static av_cold int fastaudio_init(AVCodecContext *avctx)
{
    FastAudioContext *s = avctx->priv_data;

    avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

    for (int i = 0; i < 8; i++)
        s->table[0][i] = (i - 159.5f) / 160.f;
    for (int i = 0; i < 11; i++)
        s->table[0][i + 8] = (i - 37.5f) / 40.f;
    for (int i = 0; i < 27; i++)
        s->table[0][i + 8 + 11] = (i - 13.f) / 20.f;
    for (int i = 0; i < 11; i++)
        s->table[0][i + 8 + 11 + 27] = (i + 27.5f) / 40.f;
    for (int i = 0; i < 7; i++)
        s->table[0][i + 8 + 11 + 27 + 11] = (i + 152.5f) / 160.f;

    memcpy(s->table[1], s->table[0], sizeof(s->table[0]));

    for (int i = 0; i < 7; i++)
        s->table[2][i] = (i - 33.5f) / 40.f;
    for (int i = 0; i < 25; i++)
        s->table[2][i + 7] = (i - 13.f) / 20.f;

    for (int i = 0; i < 32; i++)
        s->table[3][i] = -s->table[2][31 - i];

    for (int i = 0; i < 16; i++)
        s->table[4][i] = i * 0.22f / 3.f - 0.6f;

    for (int i = 0; i < 16; i++)
        s->table[5][i] = i * 0.20f / 3.f - 0.3f;

    for (int i = 0; i < 8; i++)
        s->table[6][i] = i * 0.36f / 3.f - 0.4f;

    for (int i = 0; i < 8; i++)
        s->table[7][i] = i * 0.34f / 3.f - 0.2f;

    s->ch = av_calloc(avctx->ch_layout.nb_channels, sizeof(*s->ch));
    if (!s->ch)
        return AVERROR(ENOMEM);

    return 0;
}

static int read_bits(int bits, int *ppos, unsigned *src)
{
    int r, pos;

    pos = *ppos;
    pos += bits;
    r = src[(pos - 1) / 32] >> ((-pos) & 31);
    *ppos = pos;

    return r & ((1 << bits) - 1);
}

static const uint8_t bits[8] = { 6, 6, 5, 5, 4, 0, 3, 3, };


static int fastaudio_decode(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *pkt)
{
    FastAudioContext *s = avctx->priv_data;
    GetByteContext gb;
    int subframes;
    int ret;

    subframes = pkt->size / (40 * avctx->ch_layout.nb_channels);
    if (subframes <= 0 || subframes > INT_MAX / 256)
        return AVERROR_INVALIDDATA;
    frame->nb_samples = subframes * 256;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    bytestream2_init(&gb, pkt->data, pkt->size);

    for (int subframe = 0; subframe < subframes; subframe++) {
        for (int channel = 0; channel < avctx->ch_layout.nb_channels; channel++) {
            ChannelItems *ch = &s->ch[channel];
            float result[256] = { 0 };
            unsigned src[10];
            int inds[4], pads[4];
            float m[8];
            int pos = 0;

            for (int i = 0; i < 10; i++)
                src[i] = bytestream2_get_le32(&gb);

            for (int i = 0; i < 8; i++)
                m[7 - i] = s->table[i][read_bits(bits[i], &pos, src)];

            for (int i = 0; i < 4; i++)
                inds[3 - i] = read_bits(6, &pos, src);

            for (int i = 0; i < 4; i++)
                pads[3 - i] = read_bits(2, &pos, src);

            for (int i = 0, index5 = 0; i < 4; i++) {
                float value = av_int2float((inds[i] + 1) << 20) * powf(2.f, 116.f);

                for (int j = 0, tmp = 0; j < 21; j++) {
                    /* The last sample's three bits are the upper ones of tmp, the
                     * four bits that close the two words of this sub-block; the
                     * lowest one only selects m[2] and is no part of the sample. */
                    const int v = j == 20 ? tmp >> 1 : read_bits(3, &pos, src);

                    result[i * 64 + pads[i] + j * 3] = value * (2.0f * v - 7.0f);

                    if (j % 10 == 9)
                        tmp = 4 * tmp + read_bits(2, &pos, src);
                    if (j == 20)
                        index5 = FFMIN(2 * index5 + tmp % 2, 63);
                }

                m[2] = s->table[5][index5];
            }

            for (int i = 0; i < 256; i++) {
                float x = result[i];

                for (int j = 0; j < 8; j++) {
                    x -= m[j] * ch->f[j];
                    ch->f[j] += m[j] * x;
                }

                memmove(&ch->f[0], &ch->f[1], sizeof(float) * 7);
                ch->f[7] = x;
                ch->last = x + ch->last * 0.86f;
                result[i] = ch->last * 2.f;
            }

            memcpy(frame->extended_data[channel] + 1024 * subframe, result, 256 * sizeof(float));
        }
    }

    *got_frame = 1;

    return pkt->size;
}

static av_cold int fastaudio_close(AVCodecContext *avctx)
{
    FastAudioContext *s = avctx->priv_data;

    av_freep(&s->ch);

    return 0;
}

#include "encode.h"

/*
 * The encoder works the way the one of koori0831's MoflexTool does.  The
 * filter of a sub-frame comes from a linear prediction analysis of the
 * pre-emphasised input (the decoder de-emphasises), its reflection
 * coefficients quantized with the tables the decoder reads them from.  Each
 * of the four groups of a sub-frame is then 21 pulses, three samples apart
 * from one of four starting points and of eight levels times a common gain,
 * chosen to bring the decoder's output as close to the input as they can:
 * least squares for the amplitudes, then a descent on the quantized levels
 * for a few gains around the one the amplitudes ask for.
 *
 * An encoder that hands back a packet for every frame cannot see past the
 * end of the sub-frame, so the analysis window closes there instead of
 * reaching 64 samples further as MoflexTool's does.
 */

#define FA_ORDER   8
#define FA_GROUP   64
#define FA_PULSES  21
#define FA_HISTORY 65      /* samples before a sub-frame that the analysis reads */
#define FA_WINDOW  (FA_HISTORY - 1 + 256)

static const uint8_t fa_table_size[8] = { 64, 64, 32, 32, 16, 16, 8, 8 };

static void fa_put_bits(uint32_t *words, int *pos, int bits, unsigned value)
{
    if (!bits)
        return;
    words[*pos >> 5] |= (value & ((1U << bits) - 1)) << (32 - (*pos & 31) - bits);
    *pos += bits;
}

/* What the decoder makes of n excitation samples, in place. */
static void fa_synthesize(ChannelItems *ch, const float m[8], float *buf, int n)
{
    for (int i = 0; i < n; i++) {
        float x = buf[i];

        for (int j = 0; j < 8; j++) {
            x -= m[j] * ch->f[j];
            ch->f[j] += m[j] * x;
        }
        memmove(&ch->f[0], &ch->f[1], sizeof(float) * 7);
        ch->f[7] = x;
        ch->last = x + ch->last * 0.86f;
        buf[i] = ch->last * 2.f;
    }
}

/* Reflection coefficients of the autocorrelation r[0..FA_ORDER]. */
static void fa_levinson(const double *r, double *k)
{
    double a[FA_ORDER + 1] = { 0 }, tmp[FA_ORDER + 1];
    double err = r[0];

    for (int i = 1; i <= FA_ORDER; i++) {
        double acc = r[i], ki;

        for (int j = 1; j < i; j++)
            acc += a[j] * r[i - j];
        ki = err > 0 ? -acc / err : 0;
        ki = av_clipd(ki, -0.999, 0.999);
        k[i - 1] = ki;
        memcpy(tmp, a, sizeof(tmp));
        for (int j = 1; j < i; j++)
            a[j] = tmp[j] + ki * tmp[i - j];
        a[i] = ki;
        err *= 1 - ki * ki;
    }
}

/* Solve (A + eps I) x = b for a symmetric positive definite A. */
static void fa_solve(double A[FA_PULSES][FA_PULSES], const double *b, double *x)
{
    double L[FA_PULSES][FA_PULSES], y[FA_PULSES];
    double trace = 0, eps;

    for (int i = 0; i < FA_PULSES; i++)
        trace += A[i][i];
    eps = 1e-9 * trace / FA_PULSES + 1e-30;
    for (int i = 0; i < FA_PULSES; i++)
        for (int j = 0; j <= i; j++) {
            double s = A[i][j] + (i == j ? eps : 0);

            for (int k = 0; k < j; k++)
                s -= L[i][k] * L[j][k];
            L[i][j] = i == j ? sqrt(FFMAX(s, 1e-300)) : s / L[j][j];
        }
    for (int i = 0; i < FA_PULSES; i++) {
        double s = b[i];

        for (int k = 0; k < i; k++)
            s -= L[i][k] * y[k];
        y[i] = s / L[i][i];
    }
    for (int i = FA_PULSES - 1; i >= 0; i--) {
        double s = y[i];

        for (int k = i + 1; k < FA_PULSES; k++)
            s -= L[k][i] * x[k];
        x[i] = s / L[i][i];
    }
}

/* Starting point, gain and levels of the pulses whose response comes
 * closest to target.  h is the response to one pulse; corr[d][n] the sum of
 * h[i] * h[i + 3 * d] over the first n samples. */
static void fa_search_group(const float *h, double (*corr)[FA_GROUP + 1],
                            const float *gain_tab, const double *target,
                            int *best_pad, int *best_gain, int best_levels[FA_PULSES])
{
    double best_err = DBL_MAX;
    double G[FA_PULSES][FA_PULSES], b[FA_PULSES], amp[FA_PULSES], gq[FA_PULSES];
    int q[FA_PULSES];

    *best_pad = *best_gain = 0;
    for (int j = 0; j < FA_PULSES; j++)
        best_levels[j] = 4;

    for (int pad = 0; pad < 4; pad++) {
        double peak = 0;
        int g0 = 0;

        for (int j = 0; j < FA_PULSES; j++) {
            const int pj = pad + 3 * j;
            double sum = 0;

            for (int n = pj; n < FA_GROUP; n++)
                sum += h[n - pj] * target[n];
            b[j] = sum;
            for (int l = j; l < FA_PULSES; l++)
                G[j][l] = G[l][j] = corr[l - j][FA_GROUP - pad - 3 * l];
        }
        fa_solve(G, b, amp);
        for (int j = 0; j < FA_PULSES; j++)
            peak = FFMAX(peak, fabs(amp[j]));
        while (g0 < 63 && gain_tab[g0] * 7 < peak)
            g0++;

        for (int gi = FFMAX(0, g0 - 4); gi <= FFMIN(63, g0 + 3); gi++) {
            const double gain = gain_tab[gi];
            double err = 0;

            for (int j = 0; j < FA_PULSES; j++)
                q[j] = av_clip(lrint((amp[j] / gain + 7) / 2), 0, 7);
            /* the error of levels u = 2q - 7 is g^2 u'Gu - 2 g u'b */
            for (int j = 0; j < FA_PULSES; j++) {
                double sum = 0;

                for (int l = 0; l < FA_PULSES; l++)
                    sum += G[j][l] * (2 * q[l] - 7);
                gq[j] = sum;
            }
            for (int pass = 0; pass < 3; pass++)
                for (int j = 0; j < FA_PULSES; j++) {
                    const int u0 = 2 * q[j] - 7;
                    double best_delta = 0;
                    int best_u = u0;

                    for (int u = -7; u <= 7; u += 2) {
                        const int d = u - u0;
                        const double delta = -2 * gain * d * b[j] +
                                             gain * gain * (2 * d * gq[j] + d * d * G[j][j]);

                        if (d && delta < best_delta) {
                            best_delta = delta;
                            best_u     = u;
                        }
                    }
                    if (best_u != u0) {
                        const int d = best_u - u0;

                        for (int l = 0; l < FA_PULSES; l++)
                            gq[l] += G[l][j] * d;
                        q[j] = (best_u + 7) / 2;
                    }
                }
            for (int j = 0; j < FA_PULSES; j++)
                err += -2 * gain * (2 * q[j] - 7) * b[j] + gain * gain * (2 * q[j] - 7) * gq[j];
            if (err < best_err) {
                best_err   = err;
                *best_pad  = pad;
                *best_gain = gi;
                memcpy(best_levels, q, sizeof(q));
            }
        }
    }
}

static void fa_encode_subframe(FastAudioContext *s, ChannelItems *ch,
                               const float *samples, uint8_t *dst)
{
    float x[FA_HISTORY + 256], m[8], h[FA_GROUP];
    double win[FA_WINDOW], r[FA_ORDER + 1], refl[FA_ORDER];
    double corr[FA_PULSES][FA_GROUP + 1];
    int idx[8], gains[4], pads[4], levels[4][FA_PULSES];
    uint32_t words[10] = { 0 };
    ChannelItems impulse = { { 0 } };
    int pos = 0;

    memcpy(x, ch->history, sizeof(ch->history));
    memcpy(x + FA_HISTORY, samples, 256 * sizeof(*x));
    memcpy(ch->history, x + 256, sizeof(ch->history));

    /* the filter: 64 samples of the past and the sub-frame, pre-emphasised */
    for (int i = 0; i < FA_WINDOW; i++)
        win[i] = (x[i + 1] - 0.86f * x[i]) * s->window[i];
    for (int k = 0; k <= FA_ORDER; k++) {
        double sum = 0;

        for (int i = k; i < FA_WINDOW; i++)
            sum += win[i] * win[i - k];
        r[k] = sum * s->lag_window[k];
    }
    r[0] += 1e-9;
    fa_levinson(r, refl);
    for (int i = 0; i < 8; i++) {
        double best = DBL_MAX;

        idx[i] = 0;
        for (int j = 0; j < fa_table_size[i]; j++) {
            const double d = fabs(s->table[i][j] - refl[i]);
            if (d < best) {
                best   = d;
                idx[i] = j;
            }
        }
        m[7 - i] = s->table[i][idx[i]];
    }

    /* its response to a pulse, and that response's correlation with itself */
    memset(h, 0, sizeof(h));
    h[0] = 1;
    fa_synthesize(&impulse, m, h, FA_GROUP);
    for (int d = 0; d < FA_PULSES; d++) {
        corr[d][0] = 0;
        for (int i = 0; i < FA_GROUP; i++)
            corr[d][i + 1] = corr[d][i] +
                             (i + 3 * d < FA_GROUP ? (double)h[i] * h[i + 3 * d] : 0);
    }

    for (int g = 0; g < 4; g++) {
        const float *in = x + FA_HISTORY + g * FA_GROUP;
        ChannelItems ringing = *ch;
        float buf[FA_GROUP] = { 0 };
        double target[FA_GROUP];
        float gain;

        /* what is left to the pulses once the filter has rung out */
        fa_synthesize(&ringing, m, buf, FA_GROUP);
        for (int i = 0; i < FA_GROUP; i++)
            target[i] = in[i] - buf[i];
        fa_search_group(h, corr, s->gain, target, &pads[g], &gains[g], levels[g]);

        memset(buf, 0, sizeof(buf));
        gain = s->gain[gains[g]];
        for (int j = 0; j < FA_PULSES; j++)
            buf[pads[g] + 3 * j] = gain * (2 * levels[g][j] - 7);
        fa_synthesize(ch, m, buf, FA_GROUP);
    }

    for (int i = 0; i < 8; i++)
        fa_put_bits(words, &pos, bits[i], idx[i]);
    for (int g = 3; g >= 0; g--)
        fa_put_bits(words, &pos, 6, gains[g]);
    for (int g = 3; g >= 0; g--)
        fa_put_bits(words, &pos, 2, pads[g]);
    for (int g = 0; g < 4; g++) {
        const int *q = levels[g];

        for (int j = 0; j < 20; j++) {
            fa_put_bits(words, &pos, 3, q[j]);
            /* the last pulse and a bit of the sixth coefficient share the
             * four bits that close the two words of a group */
            if (j == 9)
                fa_put_bits(words, &pos, 2, q[20] >> 1);
            if (j == 19)
                fa_put_bits(words, &pos, 2, (q[20] & 1) << 1 | (idx[5] >> (3 - g) & 1));
        }
    }
    for (int i = 0; i < 10; i++)
        AV_WL32(dst + 4 * i, words[i]);
}

static av_cold int fastaudio_encode_init(AVCodecContext *avctx)
{
    FastAudioContext *s = avctx->priv_data;

    avctx->frame_size = 256;
    /* A Hann window that rises over the past and the first half of the
     * sub-frame and falls over the second, so that it is centred on the
     * sub-frame and done where the input is. */
    for (int i = 0; i < FA_WINDOW; i++) {
        const int rise = FA_HISTORY - 1 + 128;
        const double t = i < rise ? (i + 0.5) / (2 * rise)
                                  : 0.5 + (i - rise + 0.5) / (2 * (FA_WINDOW - rise));
        s->window[i] = 0.5 - 0.5 * cos(2 * M_PI * t);
    }
    /* a noise floor and a lag window keep the quantized filter of a pure
     * tone from ringing */
    s->lag_window[0] = 1.003;
    for (int k = 1; k <= FA_ORDER; k++)
        s->lag_window[k] = exp(-0.5 * pow(2 * M_PI * 0.004 * k, 2));
    for (int i = 0; i < 64; i++)
        s->gain[i] = av_int2float((i + 1) << 20) * powf(2.f, 116.f);
    return fastaudio_init(avctx);
}

static int fastaudio_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                                  const AVFrame *frame, int *got_packet_ptr)
{
    FastAudioContext *s = avctx->priv_data;
    const int channels = avctx->ch_layout.nb_channels;
    int ret;

    if (frame->nb_samples != 256) {
        av_log(avctx, AV_LOG_ERROR, "Frame size must be exactly 256 samples (got %d)\n", frame->nb_samples);
        return AVERROR(EINVAL);
    }
    if ((ret = ff_alloc_packet(avctx, avpkt, 40 * channels)) < 0)
        return ret;
    for (int channel = 0; channel < channels; channel++)
        fa_encode_subframe(s, &s->ch[channel], (const float *)frame->extended_data[channel],
                           avpkt->data + 40 * channel);

    *got_packet_ptr = 1;
    return 0;
}


static const AVClass fastaudio_decoder_class = {
    .class_name = "fastaudio decoder",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_fastaudio_decoder = {
    .p.name         = "fastaudio",
    CODEC_LONG_NAME("MobiClip FastAudio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_FASTAUDIO,
    .p.priv_class   = &fastaudio_decoder_class,
    .priv_data_size = sizeof(FastAudioContext),
    .init           = fastaudio_init,
    FF_CODEC_DECODE_CB(fastaudio_decode),
    .close          = fastaudio_close,
    .p.capabilities = AV_CODEC_CAP_DR1,
};

static const AVClass fastaudio_encoder_class = {
    .class_name = "fastaudio encoder",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

static void fastaudio_flush(AVCodecContext *avctx)
{
    FastAudioContext *s = avctx->priv_data;
    memset(s->ch, 0, avctx->ch_layout.nb_channels * sizeof(*s->ch));
}

const FFCodec ff_fastaudio_encoder = {
    .p.name         = "fastaudio",
    CODEC_LONG_NAME("MobiClip FastAudio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_FASTAUDIO,
    .p.priv_class   = &fastaudio_encoder_class,
    .priv_data_size = sizeof(FastAudioContext),
    .init           = fastaudio_encode_init,
    FF_CODEC_ENCODE_CB(fastaudio_encode_frame),
    .close          = fastaudio_close,
    .flush          = fastaudio_flush,
    .p.capabilities = AV_CODEC_CAP_ENCODER_FLUSH,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP),
    CODEC_CH_LAYOUTS(AV_CHANNEL_LAYOUT_MONO, AV_CHANNEL_LAYOUT_STEREO),
};
