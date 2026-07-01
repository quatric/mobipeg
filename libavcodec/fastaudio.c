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

#include "libavutil/intfloat.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "decode.h"

typedef struct ChannelItems {
    float f[8];
    float last;
} ChannelItems;

typedef struct FastAudioContext {
    const AVClass *class;
    float table[8][64];

    ChannelItems *ch;
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

static void set_sample(int i, int j, int v, float *result, int *pads, float value)
{
    result[i * 64 + pads[i] + j * 3] = value * (2 * v - 7);
}

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
                    float v_out;
                    if (j == 20) {
                        /* tmp is a 4-bit value (0-15) from two pairs of 2-bit reads.
                         * PlayMobic: results[..] = scale * (sample60 - 7)
                         * i.e. map 0-15 linearly to -7..+8, not 2*(tmp/2)-7. */
                        v_out = (float)tmp - 7.0f;
                    } else {
                        v_out = 2.0f * read_bits(3, &pos, src) - 7.0f;
                    }
                    result[i * 64 + pads[i] + j * 3] = value * v_out;

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

static void write_bits(int bits, int *ppos, uint32_t *src, uint32_t val)
{
    int pos = *ppos;
    pos += bits;
    int shift = (-pos) & 31;
    src[(pos - 1) / 32] |= (val & ((1U << bits) - 1)) << shift;
    *ppos = pos;
}

static void compute_lpc(const float *samples, int nb_samples, float *lpc)
{
    double r[9] = { 0 };
    for (int i = 0; i <= 8; i++) {
        double sum = 0;
        for (int j = 0; j < nb_samples - i; j++) {
            sum += (double)samples[j] * samples[j + i];
        }
        r[i] = sum;
    }

    double a[9] = { 0 };
    double new_a[9] = { 0 };
    double error = r[0];

    memset(lpc, 0, 8 * sizeof(float));

    if (error < 1e-9) {
        return;
    }

    for (int i = 1; i <= 8; i++) {
        double sum = r[i];
        for (int j = 1; j < i; j++) {
            sum += a[j] * r[i - j];
        }
        double k = -sum / error;
        if (k < -1.0) k = -1.0;
        if (k > 1.0) k = 1.0;
        lpc[i - 1] = k;
        new_a[i] = k;
        for (int j = 1; j < i; j++) {
            new_a[j] = a[j] + k * a[i - j];
        }
        memcpy(a, new_a, (i + 1) * sizeof(*a));
        error *= (1.0 - k * k);
        if (error < 1e-9)
            break;
    }
}

static int find_best_index(const float *table, int size, float val)
{
    int best_idx = 0;
    float min_diff = fabsf(table[0] - val);
    for (int i = 1; i < size; i++) {
        float diff = fabsf(table[i] - val);
        if (diff < min_diff) {
            min_diff = diff;
            best_idx = i;
        }
    }
    return best_idx;
}

static float get_residual_energy(const float *y, int nb_samples, const float *m, const ChannelItems *ch)
{
    float f0 = ch->f[0];
    float f1 = ch->f[1];
    float f2 = ch->f[2];
    float f3 = ch->f[3];
    float f4 = ch->f[4];
    float f5 = ch->f[5];
    float f6 = ch->f[6];
    float f7 = ch->f[7];
    float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
    float m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7];
    float energy = 0;
    for (int i = 0; i < nb_samples; i++) {
        float x = y[i];
        x -= m0 * f0; f0 += m0 * x;
        x -= m1 * f1; f1 += m1 * x;
        x -= m2 * f2; f2 += m2 * x;
        x -= m3 * f3; f3 += m3 * x;
        x -= m4 * f4; f4 += m4 * x;
        x -= m5 * f5; f5 += m5 * x;
        x -= m6 * f6; f6 += m6 * x;
        x -= m7 * f7; f7 += m7 * x;

        f0 = f1;
        f1 = f2;
        f2 = f3;
        f3 = f4;
        f4 = f5;
        f5 = f6;
        f6 = f7;
        f7 = x;
        energy += x * x;
    }
    return energy;
}

static av_cold int fastaudio_encode_init(AVCodecContext *avctx)
{
    avctx->frame_size = 256;
    return fastaudio_init(avctx);
}

static int fastaudio_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                                  const AVFrame *frame, int *got_packet_ptr)
{
    FastAudioContext *s = avctx->priv_data;
    int channels = avctx->ch_layout.nb_channels;
    int ret;

    if (frame->nb_samples != 256) {
        av_log(avctx, AV_LOG_ERROR, "Frame size must be exactly 256 samples (got %d)\n", frame->nb_samples);
        return AVERROR(EINVAL);
    }

    if ((ret = ff_alloc_packet(avctx, avpkt, 40 * channels)) < 0)
        return ret;

    memset(avpkt->data, 0, 40 * channels);

    uint8_t *dst = avpkt->data;

    for (int channel = 0; channel < channels; channel++) {
        ChannelItems *ch = &s->ch[channel];
        const float *input = (const float *)frame->extended_data[channel];
        float y[256];
        float prev_sample = ch->last * 2.f;

        for (int k = 0; k < 256; k++) {
            y[k] = (input[k] - 0.86f * prev_sample) / 2.f;
            prev_sample = input[k];
        }

        float m_opt[8];
        compute_lpc(y, 256, m_opt);

        int idx[8] = { 0 };
        float m[8] = { 0 };

        /* Decoder uses m[j]=table[7-j][...]: the wide/fine tables (table[0]/[1],
         * 64 entries, ±1) back the high stages m[7]/m[6], coarse narrow tables
         * back m[0]/m[1].  The largest reflection coefficients k_1,k_2
         * (m_opt[0],m_opt[1]) need that range, so map m[j] <- m_opt[7-j]. */
        idx[0] = find_best_index(s->table[7], 8, m_opt[7]);
        idx[1] = find_best_index(s->table[6], 8, m_opt[6]);
        idx[3] = find_best_index(s->table[4], 16, m_opt[4]);
        idx[4] = find_best_index(s->table[3], 32, m_opt[3]);
        idx[5] = find_best_index(s->table[2], 32, m_opt[2]);
        idx[6] = find_best_index(s->table[1], 64, m_opt[1]);
        idx[7] = find_best_index(s->table[0], 64, m_opt[0]);

        m[0] = s->table[7][idx[0]];
        m[1] = s->table[6][idx[1]];
        m[3] = s->table[4][idx[3]];
        m[4] = s->table[3][idx[4]];
        m[5] = s->table[2][idx[5]];
        m[6] = s->table[1][idx[6]];
        m[7] = s->table[0][idx[7]];

        int best_idx5 = 0;
        float min_energy = 1e30f;
        for (int i5 = 0; i5 < 16; i5++) {
            m[2] = s->table[5][i5];
            float energy = get_residual_energy(y, 256, m, ch);
            if (energy < min_energy) {
                min_energy = energy;
                best_idx5 = i5;
            }
        }
        idx[2] = best_idx5;
        m[2] = s->table[5][best_idx5];

        float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];
        float m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7];

        int pad[4] = { 0 };
        int scale_idx[4] = { 0 };
        int v[4][21] = { {0} };
        int high_2_bits[4] = { 0 };
        int low_2_bits[4] = { 0 };

        float q_scale_tab[64];
        for (int s_idx = 0; s_idx < 64; s_idx++) {
            q_scale_tab[s_idx] = av_int2float((s_idx + 1) << 20) * powf(2.f, 116.f);
        }

        for (int i = 0; i < 4; i++) {
            int desired_bit = (best_idx5 >> (3 - i)) & 1;
            int best_pad = 0, best_scale = 0;
            int best_v[21] = { 0 };
            float min_err = 1e30f;

            /* Closed-loop analysis-by-synthesis: the decoder excites only the
             * sparse positions pad+j*3 and runs zeros in between, so the lattice
             * state at each coded sample depends on the previously-quantized
             * (and zero) excitations.  Evaluate every (pad,scale) by replaying
             * the EXACT decoder lattice from the real state, choosing each coded
             * excitation against the current state, and scoring by how well the
             * lattice output matches the de-emphasised target y. */
            for (int p = 0; p < 4; p++) {
                for (int s_idx = 0; s_idx < 64; s_idx++) {
                    float q_scale = q_scale_tab[s_idx];
                    float f0 = ch->f[0];
                    float f1 = ch->f[1];
                    float f2 = ch->f[2];
                    float f3 = ch->f[3];
                    float f4 = ch->f[4];
                    float f5 = ch->f[5];
                    float f6 = ch->f[6];
                    float f7 = ch->f[7];
                    float err = 0;
                    int cur_v[21] = { 0 };
                    int next_excite = p;
                    int idx_3 = 0;
                    for (int k = 0; k < 64; k++) {
                        float e = 0;
                        if (k == next_excite) {
                            if (idx_3 < 21) {
                                float e_ideal = y[i * 64 + k];
                                e_ideal += m0 * f0 + m1 * f1 + m2 * f2 + m3 * f3 +
                                           m4 * f4 + m5 * f5 + m6 * f6 + m7 * f7;
                                /* The j==20 excitation encodes a 4-bit value tmp = 2*q + desired_bit.
                                 * The decoder reconstructs: excitation = q_scale * (tmp - 7).
                                 * So we solve: q_scale*(2*q + desired_bit - 7) ≈ e_ideal
                                 * → q = round((e_ideal/q_scale + 7 - desired_bit) / 2) */
                                int q;
                                if (idx_3 == 20) {
                                    q = (int)lrintf((e_ideal / q_scale + 7.0f - desired_bit) / 2.0f);
                                } else {
                                    q = (int)lrintf((e_ideal / q_scale + 7.0f) / 2.0f);
                                }
                                if (q < 0) q = 0;
                                if (q > 7) q = 7;
                                cur_v[idx_3] = q;
                                if (idx_3 == 20) {
                                    e = q_scale * (2.0f * q + desired_bit - 7.0f);
                                } else {
                                    e = q_scale * (2.0f * q - 7.0f);
                                }
                                idx_3++;
                            }
                            next_excite += 3;
                        }
                        float x = e;
                        x -= m0 * f0; f0 += m0 * x;
                        x -= m1 * f1; f1 += m1 * x;
                        x -= m2 * f2; f2 += m2 * x;
                        x -= m3 * f3; f3 += m3 * x;
                        x -= m4 * f4; f4 += m4 * x;
                        x -= m5 * f5; f5 += m5 * x;
                        x -= m6 * f6; f6 += m6 * x;
                        x -= m7 * f7; f7 += m7 * x;

                        f0 = f1;
                        f1 = f2;
                        f2 = f3;
                        f3 = f4;
                        f4 = f5;
                        f5 = f6;
                        f6 = f7;
                        f7 = x;

                        float d = x - y[i * 64 + k];
                        err += d * d;
                    }
                    if (err < min_err) {
                        min_err = err;
                        best_pad = p;
                        best_scale = s_idx;
                        memcpy(best_v, cur_v, sizeof(best_v));
                    }
                }
            }

            pad[i] = best_pad;
            scale_idx[i] = best_scale;
            memcpy(v[i], best_v, sizeof(best_v));

            int v20 = best_v[20];
            high_2_bits[i] = v20 >> 1;
            low_2_bits[i] = ((v20 & 1) << 1) | desired_bit;

            /* Commit the chosen block on the real channel state. */
            float q_scale = q_scale_tab[best_scale];
            float f0 = ch->f[0];
            float f1 = ch->f[1];
            float f2 = ch->f[2];
            float f3 = ch->f[3];
            float f4 = ch->f[4];
            float f5 = ch->f[5];
            float f6 = ch->f[6];
            float f7 = ch->f[7];
            int next_excite = best_pad;
            int idx_3 = 0;
            for (int k = 0; k < 64; k++) {
                float x = 0;
                if (k == next_excite) {
                    if (idx_3 < 21) {
                        /* Reconstruct decoder's excitation for j==20: tmp = 2*q + desired_bit,
                         * excitation = q_scale * (tmp - 7) = q_scale * (2*q + desired_bit - 7). */
                        if (idx_3 == 20) {
                            x = q_scale * (2.0f * best_v[idx_3] + desired_bit - 7.0f);
                        } else {
                            x = q_scale * (2.0f * best_v[idx_3] - 7.0f);
                        }
                        idx_3++;
                    }
                    next_excite += 3;
                }

                x -= m0 * f0; f0 += m0 * x;
                x -= m1 * f1; f1 += m1 * x;
                x -= m2 * f2; f2 += m2 * x;
                x -= m3 * f3; f3 += m3 * x;
                x -= m4 * f4; f4 += m4 * x;
                x -= m5 * f5; f5 += m5 * x;
                x -= m6 * f6; f6 += m6 * x;
                x -= m7 * f7; f7 += m7 * x;

                f0 = f1;
                f1 = f2;
                f2 = f3;
                f3 = f4;
                f4 = f5;
                f5 = f6;
                f6 = f7;
                f7 = x;

                ch->last = x + ch->last * 0.86f;
            }
            ch->f[0] = f0;
            ch->f[1] = f1;
            ch->f[2] = f2;
            ch->f[3] = f3;
            ch->f[4] = f4;
            ch->f[5] = f5;
            ch->f[6] = f6;
            ch->f[7] = f7;
        }

        uint32_t src[10] = { 0 };
        int ppos = 0;

        write_bits(6, &ppos, src, idx[7]);
        write_bits(6, &ppos, src, idx[6]);
        write_bits(5, &ppos, src, idx[5]);
        write_bits(5, &ppos, src, idx[4]);
        write_bits(4, &ppos, src, idx[3]);
        write_bits(3, &ppos, src, idx[1]);
        write_bits(3, &ppos, src, idx[0]);

        write_bits(6, &ppos, src, scale_idx[3]);
        write_bits(6, &ppos, src, scale_idx[2]);
        write_bits(6, &ppos, src, scale_idx[1]);
        write_bits(6, &ppos, src, scale_idx[0]);
        write_bits(2, &ppos, src, pad[3]);
        write_bits(2, &ppos, src, pad[2]);
        write_bits(2, &ppos, src, pad[1]);
        write_bits(2, &ppos, src, pad[0]);

        for (int i = 0; i < 4; i++) {
            for (int j = 0; j <= 9; j++) {
                write_bits(3, &ppos, src, v[i][j]);
            }
            write_bits(2, &ppos, src, high_2_bits[i]);

            for (int j = 10; j <= 19; j++) {
                write_bits(3, &ppos, src, v[i][j]);
            }
            write_bits(2, &ppos, src, low_2_bits[i]);
        }

        for (int w = 0; w < 10; w++) {
            AV_WL32(dst + 40 * channel + w * 4, src[w]);
        }
    }

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
