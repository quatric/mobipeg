/*
 * Bink audio (DCT) encoder
 * Copyright (C) 2026 quatric <quatricsoftware@gmail.com>
 *
 * Based on the binkenc encoder of LibBink,
 * Copyright (C) 2026 Justin Marshall, also LGPL 2.1 or later.
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
 * Bink audio DCT encoder.
 *
 * Each packet holds one transform block: frame_len input samples that
 * overlap the next block by frame_len / 16, so a block advances by
 * frame_len - frame_len / 16 samples. Like a Bink file's audio packet, each
 * packet starts with the decoded size in bytes; the Bink muxer merges the
 * packets that belong to one video frame.
 */

#include <math.h>
#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"

#define MAX_FRAME_LEN 2048
#define MAX_CH 2

static const int critical_freqs[25] = {
    100, 200, 300, 400, 510, 630, 770, 920, 1080,
    1270, 1480, 1720, 2000, 2320, 2700, 3150, 3700,
    4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500,
};

static const uint8_t run_lengths[16] = {
    2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64,
};

typedef struct BitBuf {
    uint8_t *buf;
    unsigned alloc;
    uint64_t nbits;
} BitBuf;

typedef struct BinkAudioEncContext {
    const AVClass *class;
    int quality;
    double peak_bits;
    int penalty;

    int channels;
    int frame_len, overlap, hop;
    int num_bands;
    int bands[26];
    float quant_table[96];

    float *buf[MAX_CH];
    int buffered;
    int64_t total_in;
    int64_t blocks_out;

    double *re, *im;   ///< FFT work, 2 * frame_len
    double *cos_tab, *sin_tab;
    BitBuf bb;
} BinkAudioEncContext;

static int bb_put(BitBuf *b, uint32_t v, int n)
{
    if (n < 32)
        v &= (1U << n) - 1;
    if (((b->nbits + n + 7) >> 3) > b->alloc) {
        unsigned need = ((b->nbits + n + 7) >> 3) * 2 + 1024;
        uint8_t *nb = av_realloc(b->buf, need);
        if (!nb)
            return AVERROR(ENOMEM);
        memset(nb + b->alloc, 0, need - b->alloc);
        b->buf   = nb;
        b->alloc = need;
    }
    while (n > 0) {
        int off = b->nbits & 7;
        int k   = FFMIN(8 - off, n);
        b->buf[b->nbits >> 3] |= (v & ((1U << k) - 1)) << off;
        v >>= k;
        n -= k;
        b->nbits += k;
    }
    return 0;
}

static int bits_required(int v)
{
    return v ? av_log2(v) + 1 : 0;
}

static av_cold int bink_audio_encode_init(AVCodecContext *avctx)
{
    BinkAudioEncContext *s = avctx->priv_data;
    int rate = avctx->sample_rate, half;
    float root;

    s->channels = avctx->ch_layout.nb_channels;
    if (s->channels < 1 || s->channels > MAX_CH) {
        av_log(avctx, AV_LOG_ERROR, "Bink audio supports mono or stereo\n");
        return AVERROR(EINVAL);
    }
    if (rate < 1 || rate > 65535) {
        av_log(avctx, AV_LOG_ERROR, "Bink audio sample rates must be 1-65535 Hz\n");
        return AVERROR(EINVAL);
    }

    /* Each band's quantiser is set so its largest coefficient codes in about
     * peak_bits bits; one quantiser index step is 0.2206 bits. */
    {
        double bits = s->peak_bits > 0 ? s->peak_bits : 2.0 + 0.8 * s->quality;
        s->penalty = av_clip(lrint((15.0 - bits) / 0.22057), 0, 95);
    }

    s->frame_len = rate < 22050 ? 512 : rate < 44100 ? 1024 : 2048;
    s->overlap   = s->frame_len / 16;
    s->hop       = s->frame_len - s->overlap;
    avctx->frame_size = s->hop;

    root = sqrtf(s->frame_len) / 32768.0f;
    for (int i = 0; i < 96; i++)
        s->quant_table[i] = expf(i * 0.15289164787221953823f) * root;

    half = (rate + 1) / 2;
    for (s->num_bands = 1; s->num_bands < 25; s->num_bands++)
        if (half <= critical_freqs[s->num_bands - 1])
            break;
    s->bands[0] = 2;
    for (int i = 1; i < s->num_bands; i++)
        s->bands[i] = (critical_freqs[i - 1] * s->frame_len / half) & ~1;
    s->bands[s->num_bands] = s->frame_len;

    for (int c = 0; c < s->channels; c++) {
        s->buf[c] = av_calloc(s->frame_len * 2, sizeof(float));
        if (!s->buf[c])
            return AVERROR(ENOMEM);
    }
    s->re      = av_calloc(s->frame_len * 2, sizeof(double));
    s->im      = av_calloc(s->frame_len * 2, sizeof(double));
    s->cos_tab = av_calloc(s->frame_len, sizeof(double));
    s->sin_tab = av_calloc(s->frame_len, sizeof(double));
    if (!s->re || !s->im || !s->cos_tab || !s->sin_tab)
        return AVERROR(ENOMEM);
    for (int k = 0; k < s->frame_len; k++) {
        double a = -M_PI * k / (2.0 * s->frame_len);
        s->cos_tab[k] = cos(a);
        s->sin_tab[k] = sin(a);
    }
    return 0;
}

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            FFSWAP(double, re[i], re[j]);
            FFSWAP(double, im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        double wr = cos(ang), wi = sin(ang);
        int half = len >> 1;
        for (int start = 0; start < n; start += len) {
            double fr = 1.0, fi = 0.0;
            for (int k = 0; k < half; k++) {
                double *er = &re[start + k], *ei = &im[start + k];
                double *or = &re[start + k + half], *oi = &im[start + k + half];
                double tr = *or * fr - *oi * fi;
                double ti = *or * fi + *oi * fr;
                double nfr;
                *or = *er - tr;
                *oi = *ei - ti;
                *er += tr;
                *ei += ti;
                nfr = fr * wr - fi * wi;
                fi  = fr * wi + fi * wr;
                fr  = nfr;
            }
        }
    }
}

/* DCT-II scaled to the decoder's inverse transform. */
static void forward_dct(BinkAudioEncContext *s, const float *in, double *coef)
{
    int n = s->frame_len, n2 = n * 2;
    for (int i = 0; i < n; i++) {
        s->re[i] = s->re[n2 - 1 - i] = in[i];
        s->im[i] = s->im[n2 - 1 - i] = 0;
    }
    fft(s->re, s->im, n2);
    coef[0] = s->re[0] * 0.25;
    for (int k = 1; k < n; k++)
        coef[k] = 0.5 * (s->re[k] * s->cos_tab[k] - s->im[k] * s->sin_tab[k]);
}

static int write_bink_float(BitBuf *bb, double value)
{
    int neg = value < 0, power = 0, ret;
    double mag = fabs(value);
    uint32_t mant = 0;

    if (isfinite(mag) && mag > 0) {
        double scaled;
        power = FFMAX(0, (int)ceil(log2(mag)));
        power = FFMIN(power, 31);
        scaled = ldexp(mag, 23 - power);
        if (scaled >= 8388608.0 && power < 31) {
            power++;
            scaled *= 0.5;
        }
        mant = av_clipd(round(scaled), 0, 8388607);
    }
    if ((ret = bb_put(bb, power, 5)) < 0 ||
        (ret = bb_put(bb, mant, 23)) < 0)
        return ret;
    return bb_put(bb, neg && mant, 1);
}

static int encode_channel(BinkAudioEncContext *s, BitBuf *bb, const float *in)
{
    double coef[MAX_FRAME_LEN];
    int mags[MAX_FRAME_LEN];
    uint8_t negs[MAX_FRAME_LEN];
    int qidx[25];
    int penalty = s->penalty, pos, ret;

    forward_dct(s, in, coef);
    if ((ret = write_bink_float(bb, coef[0] / s->quant_table[0])) < 0 ||
        (ret = write_bink_float(bb, coef[1] / s->quant_table[0])) < 0)
        return ret;

    for (int b = 0; b < s->num_bands; b++) {
        double maxv = 0;
        int idx = 0;
        for (int c = s->bands[b]; c < s->bands[b + 1]; c++)
            maxv = FFMAX(maxv, fabs(coef[c]));
        while (idx < 95 && maxv / s->quant_table[idx] > 32767.0)
            idx++;
        idx = FFMIN(95, idx + penalty);
        qidx[b] = idx;
        if ((ret = bb_put(bb, idx, 8)) < 0)
            return ret;
    }
    for (int b = 0; b < s->num_bands; b++) {
        double q = s->quant_table[qidx[b]];
        for (int c = s->bands[b]; c < s->bands[b + 1]; c++) {
            double v = coef[c] / q;
            mags[c] = FFMIN(32767.0, round(fabs(v)));
            negs[c] = v < 0;
        }
    }

    pos = 2;
    while (pos < s->frame_len) {
        int chunks = 0, p = pos, run = -1, end, maxm = 0, width;
        while (p < s->frame_len && chunks < 64) {
            int e = FFMIN(s->frame_len, p + 8), zero = 1;
            for (int c = p; c < e; c++)
                if (mags[c]) {
                    zero = 0;
                    break;
                }
            if (!zero)
                break;
            chunks++;
            p = e;
        }
        for (int i = 15; i >= 0; i--)
            if (run_lengths[i] <= chunks) {
                run = i;
                break;
            }
        if (run >= 0) {
            if ((ret = bb_put(bb, 1, 1)) < 0 ||
                (ret = bb_put(bb, run, 4)) < 0 ||
                (ret = bb_put(bb, 0, 4)) < 0)
                return ret;
            pos = FFMIN(s->frame_len, pos + run_lengths[run] * 8);
            continue;
        }
        end = FFMIN(s->frame_len, pos + 8);
        for (int c = pos; c < end; c++)
            maxm = FFMAX(maxm, mags[c]);
        width = bits_required(maxm);
        if ((ret = bb_put(bb, 0, 1)) < 0 ||
            (ret = bb_put(bb, width, 4)) < 0)
            return ret;
        if (width)
            for (int c = pos; c < end; c++) {
                if ((ret = bb_put(bb, mags[c], width)) < 0)
                    return ret;
                if (mags[c] && (ret = bb_put(bb, negs[c], 1)) < 0)
                    return ret;
            }
        pos = end;
    }
    return 0;
}

static int bink_audio_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                   const AVFrame *frame, int *got_packet)
{
    BinkAudioEncContext *s = avctx->priv_data;
    float block[MAX_FRAME_LEN];
    int64_t pts;
    int duration, ret;

    if (frame) {
        int n = frame->nb_samples;
        if (s->buffered + n > s->frame_len * 2)
            return AVERROR_BUG;
        for (int c = 0; c < s->channels; c++)
            memcpy(s->buf[c] + s->buffered, frame->extended_data[c], n * sizeof(float));
        s->buffered += n;
        s->total_in += n;
    }

    pts = s->blocks_out * s->hop;
    if (!(s->buffered >= s->frame_len || (!frame && pts < s->total_in))) {
        *got_packet = 0;
        return 0;
    }

    duration = FFMIN(s->hop, s->total_in - pts);
    s->bb.nbits = 0;
    if (s->bb.buf)
        memset(s->bb.buf, 0, s->bb.alloc);
    if ((ret = bb_put(&s->bb, duration * 2 * s->channels, 32)) < 0 ||
        (ret = bb_put(&s->bb, 0, 2)) < 0)
        return ret;
    for (int c = 0; c < s->channels; c++) {
        int n = FFMIN(s->buffered, s->frame_len);
        memcpy(block, s->buf[c], n * sizeof(float));
        memset(block + n, 0, (s->frame_len - n) * sizeof(float));
        if ((ret = encode_channel(s, &s->bb, block)) < 0)
            return ret;
    }
    if (s->bb.nbits & 31)
        if ((ret = bb_put(&s->bb, 0, 32 - (s->bb.nbits & 31))) < 0)
            return ret;

    if ((ret = ff_get_encode_buffer(avctx, pkt, s->bb.nbits >> 3, 0)) < 0)
        return ret;
    memcpy(pkt->data, s->bb.buf, s->bb.nbits >> 3);

    {
        int consume = FFMIN(s->hop, s->buffered);
        for (int c = 0; c < s->channels; c++)
            memmove(s->buf[c], s->buf[c] + consume, (s->buffered - consume) * sizeof(float));
        s->buffered -= consume;
    }
    pkt->pts = pkt->dts = pts;
    pkt->duration = duration;
    s->blocks_out++;
    *got_packet = 1;
    return 0;
}

static av_cold int bink_audio_encode_close(AVCodecContext *avctx)
{
    BinkAudioEncContext *s = avctx->priv_data;
    for (int c = 0; c < MAX_CH; c++)
        av_freep(&s->buf[c]);
    av_freep(&s->re);
    av_freep(&s->im);
    av_freep(&s->cos_tab);
    av_freep(&s->sin_tab);
    av_freep(&s->bb.buf);
    return 0;
}

#define OFFSET(x) offsetof(BinkAudioEncContext, x)
#define AE AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "quality", "audio quality, 10 is highest", OFFSET(quality), AV_OPT_TYPE_INT, { .i64 = 6 }, 0, 10, AE },
    { "peak_bits", "bits for the largest coefficient of each band (overrides quality)", OFFSET(peak_bits), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, 15, AE },
    { NULL },
};

static const AVClass bink_audio_encoder_class = {
    .class_name = "binkaudio_dct encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_binkaudio_dct_encoder = {
    .p.name         = "binkaudio_dct",
    CODEC_LONG_NAME("Bink Audio (DCT)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_BINKAUDIO_DCT,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
                      AV_CODEC_CAP_SMALL_LAST_FRAME,
    .priv_data_size = sizeof(BinkAudioEncContext),
    .init           = bink_audio_encode_init,
    FF_CODEC_ENCODE_CB(bink_audio_encode_frame),
    .close          = bink_audio_encode_close,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP),
    CODEC_CH_LAYOUTS(AV_CHANNEL_LAYOUT_MONO, AV_CHANNEL_LAYOUT_STEREO),
    .p.priv_class   = &bink_audio_encoder_class,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
