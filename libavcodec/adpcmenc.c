/*
 * Copyright (c) 2001-2003 The FFmpeg project
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 *
 * first version by Francois Revol (revol@free.fr)
 * fringe ADPCM codecs (e.g., DK3, DK4, Westwood)
 *   by Mike Melanson (melanson@pcisys.net)
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

#include <float.h>
#include <math.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "put_bits.h"
#include "bytestream.h"
#include "adpcm.h"
#include "adpcm_data.h"
#include "codec_internal.h"
#include "encode.h"

static const int16_t afc_coeffs[2][16] = {
    { 0, 2048, 0, 1024, 4096, 3584, 3072, 4608, 4200, 4800, 5120, 2048, 1024, -1024, -1024, -2048 },
    { 0, 0, 2048, 1024, -2048, -1536, -1024, -2560, -2248, -2300, -3072, -2048, -1024, 1024, 0, 0 }
};

/* --- Nintendo DSP-ADPCM coefficient generation ---------------------------
 * Ported from the public reference encoder (jackoalan/gc-dspadpcm-encode,
 * itself derived from BrawlLib's AudioConverter).  Derives the 8 predictor
 * coefficient pairs (Q11) per channel by autocorrelation + Levinson-style
 * refinement over the whole channel, exactly like Nintendo's DSPADPCM tool.
 * The stock fixed thp_coeffs table below is a poor generic predictor that
 * leaves large residuals and audible static, so THP now uses these instead. */
typedef double tvec[3];

static inline void thp_inner_product_merge(tvec vecOut, short pcmBuf[14])
{
    for (int i = 0; i <= 2; i++) {
        vecOut[i] = 0.0;
        for (int x = 0; x < 14; x++)
            vecOut[i] -= pcmBuf[x - i] * pcmBuf[x];
    }
}

static inline void thp_outer_product_merge(tvec mtxOut[3], short pcmBuf[14])
{
    for (int x = 1; x <= 2; x++)
        for (int y = 1; y <= 2; y++) {
            mtxOut[x][y] = 0.0;
            for (int z = 0; z < 14; z++)
                mtxOut[x][y] += pcmBuf[z - x] * pcmBuf[z - y];
        }
}

static int thp_analyze_ranges(tvec mtx[3], int *vecIdxsOut)
{
    double recips[3];
    double val, tmp, min, max;

    for (int x = 1; x <= 2; x++) {
        val = FFMAX(fabs(mtx[x][1]), fabs(mtx[x][2]));
        if (val < DBL_EPSILON)
            return 1;
        recips[x] = 1.0 / val;
    }

    int maxIndex = 0;
    for (int i = 1; i <= 2; i++) {
        for (int x = 1; x < i; x++) {
            tmp = mtx[x][i];
            for (int y = 1; y < x; y++)
                tmp -= mtx[x][y] * mtx[y][i];
            mtx[x][i] = tmp;
        }

        val = 0.0;
        for (int x = i; x <= 2; x++) {
            tmp = mtx[x][i];
            for (int y = 1; y < i; y++)
                tmp -= mtx[x][y] * mtx[y][i];

            mtx[x][i] = tmp;
            tmp = fabs(tmp) * recips[x];
            if (tmp >= val) {
                val = tmp;
                maxIndex = x;
            }
        }

        if (maxIndex != i) {
            for (int y = 1; y <= 2; y++) {
                tmp = mtx[maxIndex][y];
                mtx[maxIndex][y] = mtx[i][y];
                mtx[i][y] = tmp;
            }
            recips[maxIndex] = recips[i];
        }

        vecIdxsOut[i] = maxIndex;

        if (mtx[i][i] == 0.0)
            return 1;

        if (i != 2) {
            tmp = 1.0 / mtx[i][i];
            for (int x = i + 1; x <= 2; x++)
                mtx[x][i] *= tmp;
        }
    }

    min = 1.0e10;
    max = 0.0;
    for (int i = 1; i <= 2; i++) {
        tmp = fabs(mtx[i][i]);
        if (tmp < min) min = tmp;
        if (tmp > max) max = tmp;
    }

    return (min / max < 1.0e-10) ? 1 : 0;
}

static void thp_bidirectional_filter(tvec mtx[3], int *vecIdxs, tvec vecOut)
{
    double tmp;

    for (int i = 1, x = 0; i <= 2; i++) {
        int index = vecIdxs[i];
        tmp = vecOut[index];
        vecOut[index] = vecOut[i];
        if (x != 0)
            for (int y = x; y <= i - 1; y++)
                tmp -= vecOut[y] * mtx[i][y];
        else if (tmp != 0.0)
            x = i;
        vecOut[i] = tmp;
    }

    for (int i = 2; i > 0; i--) {
        tmp = vecOut[i];
        for (int y = i + 1; y <= 2; y++)
            tmp -= vecOut[y] * mtx[i][y];
        vecOut[i] = tmp / mtx[i][i];
    }

    vecOut[0] = 1.0;
}

static int thp_quadratic_merge(tvec inOutVec)
{
    double v0, v1, v2 = inOutVec[2];
    double tmp = 1.0 - (v2 * v2);

    if (tmp == 0.0)
        return 1;

    v0 = (inOutVec[0] - (v2 * v2)) / tmp;
    v1 = (inOutVec[1] - (inOutVec[1] * v2)) / tmp;

    inOutVec[0] = v0;
    inOutVec[1] = v1;

    return fabs(v1) > 1.0;
}

static void thp_finish_record(tvec in, tvec out)
{
    for (int z = 1; z <= 2; z++) {
        if (in[z] >= 1.0)
            in[z] = 0.9999999999;
        else if (in[z] <= -1.0)
            in[z] = -0.9999999999;
    }
    out[0] = 1.0;
    out[1] = (in[2] * in[1]) + in[1];
    out[2] = in[2];
}

static void thp_matrix_filter(tvec src, tvec dst)
{
    tvec mtx[3];

    mtx[2][0] = 1.0;
    for (int i = 1; i <= 2; i++)
        mtx[2][i] = -src[i];

    for (int i = 2; i > 0; i--) {
        double val = 1.0 - (mtx[i][i] * mtx[i][i]);
        for (int y = 1; y <= i; y++)
            mtx[i - 1][y] = ((mtx[i][i] * mtx[i][y]) + mtx[i][y]) / val;
    }

    dst[0] = 1.0;
    for (int i = 1; i <= 2; i++) {
        dst[i] = 0.0;
        for (int y = 1; y <= i; y++)
            dst[i] += mtx[i][y] * dst[i - y];
    }
}

static void thp_merge_finish_record(tvec src, tvec dst)
{
    tvec tmp;
    double val = src[0];

    dst[0] = 1.0;
    for (int i = 1; i <= 2; i++) {
        double v2 = 0.0;
        for (int y = 1; y < i; y++)
            v2 += dst[y] * src[i - y];

        if (val > 0.0)
            dst[i] = -(v2 + src[i]) / val;
        else
            dst[i] = 0.0;

        tmp[i] = dst[i];

        for (int y = 1; y < i; y++)
            dst[y] += dst[i] * dst[i - y];

        val *= 1.0 - (dst[i] * dst[i]);
    }

    thp_finish_record(tmp, dst);
}

static double thp_contrast_vectors(tvec source1, tvec source2)
{
    double val  = (source2[2] * source2[1] + -source2[1]) / (1.0 - source2[2] * source2[2]);
    double val1 = (source1[0] * source1[0]) + (source1[1] * source1[1]) + (source1[2] * source1[2]);
    double val2 = (source1[0] * source1[1]) + (source1[1] * source1[2]);
    double val3 = source1[0] * source1[2];
    return val1 + (2.0 * val * val2) + (2.0 * (-source2[1] * val + -source2[2]) * val3);
}

static void thp_filter_records(tvec vecBest[8], int exp, tvec records[], int recordCount)
{
    tvec bufferList[8];
    int buffer1[8];
    tvec buffer2;
    int index;
    double value, tempVal = 0;

    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < exp; y++) {
            buffer1[y] = 0;
            for (int i = 0; i <= 2; i++)
                bufferList[y][i] = 0.0;
        }
        for (int z = 0; z < recordCount; z++) {
            index = 0;
            value = 1.0e30;
            for (int i = 0; i < exp; i++) {
                tempVal = thp_contrast_vectors(vecBest[i], records[z]);
                if (tempVal < value) {
                    value = tempVal;
                    index = i;
                }
            }
            buffer1[index]++;
            thp_matrix_filter(records[z], buffer2);
            for (int i = 0; i <= 2; i++)
                bufferList[index][i] += buffer2[i];
        }

        for (int i = 0; i < exp; i++)
            if (buffer1[i] > 0)
                for (int y = 0; y <= 2; y++)
                    bufferList[i][y] /= buffer1[i];

        for (int i = 0; i < exp; i++)
            thp_merge_finish_record(bufferList[i], vecBest[i]);
    }
}

/* Derive 8 predictor coefficient pairs (Q11) into coefsOut[16] from the whole
 * channel of PCM.  Returns 0 on success, AVERROR(ENOMEM) on allocation failure. */
static int thp_correlate_coefs(const int16_t *source, int samples, int16_t *coefsOut)
{
    int numFrames = (samples + 13) / 14;
    int frameSamples;
    short pcmHistBuffer[2][14] = { { 0 } };
    tvec vec1, vec2, mtx[3];
    int vecIdxs[3];
    tvec vecBest[8];
    int recordCount = 0;
    const int16_t *src = source;

    short *blockBuffer = av_calloc(0x3800, sizeof(*blockBuffer));
    tvec  *records     = av_calloc(FFMAX(numFrames * 2, 1), sizeof(*records));
    if (!blockBuffer || !records) {
        av_free(blockBuffer);
        av_free(records);
        return AVERROR(ENOMEM);
    }

    for (int x = samples; x > 0;) {
        if (x > 0x3800) {
            frameSamples = 0x3800;
            x -= 0x3800;
        } else {
            frameSamples = x;
            memset(blockBuffer, 0, 0x3800 * sizeof(*blockBuffer));
            x = 0;
        }

        memcpy(blockBuffer, src, frameSamples * sizeof(short));
        src += frameSamples;

        for (int i = 0; i < frameSamples;) {
            for (int z = 0; z < 14; z++)
                pcmHistBuffer[0][z] = pcmHistBuffer[1][z];
            for (int z = 0; z < 14; z++)
                pcmHistBuffer[1][z] = (i < frameSamples) ? blockBuffer[i++] : 0;

            thp_inner_product_merge(vec1, pcmHistBuffer[1]);
            if (fabs(vec1[0]) > 10.0) {
                thp_outer_product_merge(mtx, pcmHistBuffer[1]);
                if (!thp_analyze_ranges(mtx, vecIdxs)) {
                    thp_bidirectional_filter(mtx, vecIdxs, vec1);
                    if (!thp_quadratic_merge(vec1)) {
                        thp_finish_record(vec1, records[recordCount]);
                        recordCount++;
                    }
                }
            }
        }
    }

    vec1[0] = 1.0; vec1[1] = 0.0; vec1[2] = 0.0;

    for (int z = 0; z < recordCount; z++) {
        thp_matrix_filter(records[z], vecBest[0]);
        for (int y = 1; y <= 2; y++)
            vec1[y] += vecBest[0][y];
    }
    if (recordCount)
        for (int y = 1; y <= 2; y++)
            vec1[y] /= recordCount;

    thp_merge_finish_record(vec1, vecBest[0]);

    {
        int exp = 1;
        for (int w = 0; w < 3;) {
            vec2[0] = 0.0; vec2[1] = -1.0; vec2[2] = 0.0;
            for (int i = 0; i < exp; i++)
                for (int y = 0; y <= 2; y++)
                    vecBest[exp + i][y] = (0.01 * vec2[y]) + vecBest[i][y];
            ++w;
            exp = 1 << w;
            thp_filter_records(vecBest, exp, records, recordCount);
        }
    }

    for (int z = 0; z < 8; z++) {
        double d;
        d = -vecBest[z][1] * 2048.0;
        coefsOut[z * 2]     = d > 0.0 ? (d > 32767.0 ? 32767 : (int16_t)lround(d))
                                      : (d < -32768.0 ? -32768 : (int16_t)lround(d));
        d = -vecBest[z][2] * 2048.0;
        coefsOut[z * 2 + 1] = d > 0.0 ? (d > 32767.0 ? 32767 : (int16_t)lround(d))
                                      : (d < -32768.0 ? -32768 : (int16_t)lround(d));
    }

    av_free(records);
    av_free(blockBuffer);
    return 0;
}

/**
 * @file
 * ADPCM encoders
 * See ADPCM decoder reference documents for codec information.
 */

#define CASE_0(codec_id, ...)
#define CASE_1(codec_id, ...) \
    case codec_id:            \
    { __VA_ARGS__ }           \
    break;
#define CASE_2(enabled, codec_id, ...) \
        CASE_ ## enabled(codec_id, __VA_ARGS__)
#define CASE_3(config, codec_id, ...) \
        CASE_2(config, codec_id, __VA_ARGS__)
#define CASE(codec, ...) \
        CASE_3(CONFIG_ ## codec ## _ENCODER, AV_CODEC_ID_ ## codec, __VA_ARGS__)

typedef struct TrellisPath {
    int nibble;
    int prev;
} TrellisPath;

typedef struct TrellisNode {
    uint32_t ssd;
    int path;
    int sample1;
    int sample2;
    int step;
} TrellisNode;

typedef struct ADPCMEncodeContext {
    AVClass *class;
    int block_size;

    ADPCMChannelStatus status[6];
    TrellisPath *paths;
    TrellisNode *node_buf;
    TrellisNode **nodep_buf;
    uint8_t *trellis_hash;

    /* THP: per-channel DSP-ADPCM coefficients (8 pairs, Q11) derived from the
     * audio via ff_DSPCorrelateCoefs on the first frame, replacing the broken
     * fixed thp_coeffs table. Emitted as side data so the muxer writes them
     * into the file header for the decoder. */
    int16_t thp_dsp_coeffs[6][16];
    int     thp_coeffs_ready;

    /* THP buffers the whole input so coefficients can be derived over the entire
     * channel (they must be constant stream-wide: the THP muxer re-slices the
     * ADPCM across video-frame boundaries, so every block shares one table). */
    int16_t *thp_pcm[6];
    int      thp_pcm_len;   /* samples per channel accumulated */
    int      thp_pcm_cap;   /* capacity per channel */
    int      thp_flushed;   /* single output packet already emitted */
} ADPCMEncodeContext;

#define FREEZE_INTERVAL 128

static av_cold int adpcm_encode_init(AVCodecContext *avctx)
{
    ADPCMEncodeContext *s = avctx->priv_data;
    int channels = avctx->ch_layout.nb_channels;

    /*
     * AMV's block size has to match that of the corresponding video
     * stream. Relax the POT requirement.
     */
    if (avctx->codec->id != AV_CODEC_ID_ADPCM_IMA_AMV &&
        (s->block_size & (s->block_size - 1))) {
        av_log(avctx, AV_LOG_ERROR, "block size must be power of 2\n");
        return AVERROR(EINVAL);
    }

    if (avctx->trellis) {
        int frontier, max_paths;

        if ((unsigned)avctx->trellis > 16U) {
            av_log(avctx, AV_LOG_ERROR, "invalid trellis size\n");
            return AVERROR(EINVAL);
        }

        if (avctx->codec->id == AV_CODEC_ID_ADPCM_IMA_SSI ||
            avctx->codec->id == AV_CODEC_ID_ADPCM_IMA_APM ||
            avctx->codec->id == AV_CODEC_ID_ADPCM_ARGO    ||
            avctx->codec->id == AV_CODEC_ID_ADPCM_IMA_WS) {
            /*
             * The current trellis implementation doesn't work for extended
             * runs of samples without periodic resets. Disallow it.
             */
            av_log(avctx, AV_LOG_ERROR, "trellis not supported\n");
            return AVERROR_PATCHWELCOME;
        }

        frontier  = 1 << avctx->trellis;
        max_paths =  frontier * FREEZE_INTERVAL;
        if (!FF_ALLOC_TYPED_ARRAY(s->paths,        max_paths)    ||
            !FF_ALLOC_TYPED_ARRAY(s->node_buf,     2 * frontier) ||
            !FF_ALLOC_TYPED_ARRAY(s->nodep_buf,    2 * frontier) ||
            !FF_ALLOC_TYPED_ARRAY(s->trellis_hash, 65536))
            return AVERROR(ENOMEM);
    }

    avctx->bits_per_coded_sample = av_get_bits_per_sample(avctx->codec->id);

    switch (avctx->codec->id) {
    CASE(ADPCM_IMA_WAV,
        /* each 16 bits sample gives one nibble
           and we have 4 bytes per channel overhead */
        avctx->frame_size = (s->block_size - 4 * channels) * 8 /
                            (4 * channels) + 1;
        /* seems frame_size isn't taken into account...
           have to buffer the samples :-( */
        avctx->block_align = s->block_size;
        avctx->bits_per_coded_sample = 4;
        avctx->bit_rate = avctx->block_align * 8LL * avctx->sample_rate / avctx->frame_size;
        ) /* End of CASE */
    CASE(ADPCM_IMA_QT,
        avctx->frame_size  = 64;
        avctx->block_align = 34 * channels;
        ) /* End of CASE */
    CASE(ADPCM_MS,
        uint8_t *extradata;
        /* each 16 bits sample gives one nibble
           and we have 7 bytes per channel overhead */
        avctx->frame_size = (s->block_size - 7 * channels) * 2 / channels + 2;
        avctx->bits_per_coded_sample = 4;
        avctx->block_align     = s->block_size;
        avctx->bit_rate = avctx->block_align * 8LL * avctx->sample_rate / avctx->frame_size;
        if (!(avctx->extradata = av_malloc(32 + AV_INPUT_BUFFER_PADDING_SIZE)))
            return AVERROR(ENOMEM);
        avctx->extradata_size = 32;
        extradata = avctx->extradata;
        bytestream_put_le16(&extradata, avctx->frame_size);
        bytestream_put_le16(&extradata, 7); /* wNumCoef */
        for (int i = 0; i < 7; i++) {
            bytestream_put_le16(&extradata, ff_adpcm_AdaptCoeff1[i] * 4);
            bytestream_put_le16(&extradata, ff_adpcm_AdaptCoeff2[i] * 4);
        }
        ) /* End of CASE */
    CASE(ADPCM_YAMAHA,
        avctx->frame_size  = s->block_size * 2 / channels;
        avctx->block_align = s->block_size;
        ) /* End of CASE */
    CASE(ADPCM_SWF,
        avctx->frame_size  = 4096; /* Hardcoded according to the SWF spec. */
        avctx->block_align = (2 + channels * (22 + 4 * (avctx->frame_size - 1)) + 7) / 8;
        ) /* End of CASE */
    case AV_CODEC_ID_ADPCM_IMA_SSI:
    case AV_CODEC_ID_ADPCM_IMA_ALP:
        avctx->frame_size  = s->block_size * 2 / channels;
        avctx->block_align = s->block_size;
        break;
    CASE(ADPCM_IMA_AMV,
        avctx->frame_size  = s->block_size;
        avctx->block_align = 8 + (FFALIGN(avctx->frame_size, 2) / 2);
        ) /* End of CASE */
    CASE(ADPCM_IMA_APM,
        avctx->frame_size  = s->block_size * 2 / channels;
        avctx->block_align = s->block_size;

        if (!(avctx->extradata = av_mallocz(28 + AV_INPUT_BUFFER_PADDING_SIZE)))
            return AVERROR(ENOMEM);
        avctx->extradata_size = 28;
        ) /* End of CASE */
    CASE(ADPCM_ARGO,
        avctx->frame_size = 32;
        avctx->block_align = 17 * channels;
        ) /* End of CASE */
    CASE(ADPCM_IMA_WS,
        /* each 16 bits sample gives one nibble */
        avctx->frame_size = s->block_size * 2 / channels;
        avctx->block_align = s->block_size;
        ) /* End of CASE */
    CASE(ADPCM_IMA_MOFLEX,
        /* One subframe = 256 samples; header 4 bytes/ch + 128 bytes/ch of nibbles */
        avctx->frame_size  = 256;
        avctx->block_align = channels * 132;
        ) /* End of CASE */
    
    CASE(ADPCM_THP,
        /* GameCube-style DSP-ADPCM: 14 samples per 8-byte block, per channel.
         * frame_size must be a multiple of 14. The coefficient table is emitted
         * as packet side data; the THP muxer writes it into each frame header. */
        if (!avctx->frame_size)
            avctx->frame_size = FFMAX(14, (s->block_size / 14) * 14);
        avctx->frame_size  = FFMAX(14, (avctx->frame_size / 14) * 14);
        avctx->block_align = channels * (avctx->frame_size / 14) * 8;
        avctx->bits_per_coded_sample = 4;
        ) /* End of CASE */
    default:
        av_unreachable("there is a case for every codec using adpcm_encode_init()");
    }

    return 0;
}

static av_cold int adpcm_encode_close(AVCodecContext *avctx)
{
    ADPCMEncodeContext *s = avctx->priv_data;
    av_freep(&s->paths);
    av_freep(&s->node_buf);
    av_freep(&s->nodep_buf);
    av_freep(&s->trellis_hash);
    for (int ch = 0; ch < 6; ch++)
        av_freep(&s->thp_pcm[ch]);

    return 0;
}


static inline uint8_t adpcm_ima_compress_sample(ADPCMChannelStatus *c,
                                                int16_t sample)
{
    int delta  = sample - c->prev_sample;
    int step   = ff_adpcm_step_table[c->step_index];
    int nibble = FFMIN(7, (abs(delta) * 8 + step) / (step * 2)) + (delta < 0) * 8;
    c->prev_sample += ((step *
                        ff_adpcm_yamaha_difflookup[nibble]) / 8);
    c->prev_sample = av_clip_int16(c->prev_sample);
    c->step_index  = av_clip(c->step_index + ff_adpcm_index_table[nibble], 0, 88);
    return nibble;
}

static inline uint8_t adpcm_ima_alp_compress_sample(ADPCMChannelStatus *c, int16_t sample)
{
    const int delta  = sample - c->prev_sample;
    const int step   = ff_adpcm_step_table[c->step_index];
    const int sign   = (delta < 0) * 8;

    int nibble = FFMIN(abs(delta) * 4 / step, 7);
    int diff   = (step * nibble) >> 2;
    if (sign)
        diff = -diff;

    nibble = sign | nibble;

    c->prev_sample += diff;
    c->prev_sample  = av_clip_int16(c->prev_sample);
    c->step_index   = av_clip(c->step_index + ff_adpcm_index_table[nibble], 0, 88);
    return nibble;
}

static inline uint8_t adpcm_ima_qt_compress_sample(ADPCMChannelStatus *c,
                                                   int16_t sample)
{
    int delta  = sample - c->prev_sample;
    int diff, step = ff_adpcm_step_table[c->step_index];
    int nibble = 8*(delta < 0);

    delta= abs(delta);
    diff = delta + (step >> 3);

    if (delta >= step) {
        nibble |= 4;
        delta  -= step;
    }
    step >>= 1;
    if (delta >= step) {
        nibble |= 2;
        delta  -= step;
    }
    step >>= 1;
    if (delta >= step) {
        nibble |= 1;
        delta  -= step;
    }
    diff -= delta;

    if (nibble & 8)
        c->prev_sample -= diff;
    else
        c->prev_sample += diff;

    c->prev_sample = av_clip_int16(c->prev_sample);
    c->step_index  = av_clip(c->step_index + ff_adpcm_index_table[nibble], 0, 88);

    return nibble;
}

static inline uint8_t adpcm_ms_compress_sample(ADPCMChannelStatus *c,
                                               int16_t sample)
{
    int predictor, nibble, bias;

    predictor = (((c->sample1) * (c->coeff1)) +
                (( c->sample2) * (c->coeff2))) / 64;

    nibble = sample - predictor;
    if (nibble >= 0)
        bias =  c->idelta / 2;
    else
        bias = -c->idelta / 2;

    nibble = (nibble + bias) / c->idelta;
    nibble = av_clip_intp2(nibble, 3) & 0x0F;

    predictor += ((nibble & 0x08) ? (nibble - 0x10) : nibble) * c->idelta;

    c->sample2 = c->sample1;
    c->sample1 = av_clip_int16(predictor);

    c->idelta = (ff_adpcm_AdaptationTable[nibble] * c->idelta) >> 8;
    if (c->idelta < 16)
        c->idelta = 16;

    return nibble;
}

static inline uint8_t adpcm_yamaha_compress_sample(ADPCMChannelStatus *c,
                                                   int16_t sample)
{
    int nibble, delta;

    if (!c->step) {
        c->predictor = 0;
        c->step      = 127;
    }

    delta = sample - c->predictor;

    nibble = FFMIN(7, abs(delta) * 4 / c->step) + (delta < 0) * 8;

    c->predictor += ((c->step * ff_adpcm_yamaha_difflookup[nibble]) / 8);
    c->predictor = av_clip_int16(c->predictor);
    c->step = (c->step * ff_adpcm_yamaha_indexscale[nibble]) >> 8;
    c->step = av_clip(c->step, 127, 24576);

    return nibble;
}

static void adpcm_compress_trellis(AVCodecContext *avctx,
                                   const int16_t *samples, uint8_t *dst,
                                   ADPCMChannelStatus *c, int n, int stride)
{
    //FIXME 6% faster if frontier is a compile-time constant
    ADPCMEncodeContext *s = avctx->priv_data;
    const int frontier = 1 << avctx->trellis;
    const int version  = avctx->codec->id;
    TrellisPath *paths       = s->paths, *p;
    TrellisNode *node_buf    = s->node_buf;
    TrellisNode **nodep_buf  = s->nodep_buf;
    TrellisNode **nodes      = nodep_buf; // nodes[] is always sorted by .ssd
    TrellisNode **nodes_next = nodep_buf + frontier;
    int pathn = 0, froze = -1, i, j, k, generation = 0;
    uint8_t *hash = s->trellis_hash;
    memset(hash, 0xff, 65536 * sizeof(*hash));

    memset(nodep_buf, 0, 2 * frontier * sizeof(*nodep_buf));
    nodes[0]          = node_buf + frontier;
    nodes[0]->ssd     = 0;
    nodes[0]->path    = 0;
    nodes[0]->step    = c->step_index;
    nodes[0]->sample1 = c->sample1;
    nodes[0]->sample2 = c->sample2;
    if (version == AV_CODEC_ID_ADPCM_IMA_WAV ||
        version == AV_CODEC_ID_ADPCM_IMA_QT  ||
        version == AV_CODEC_ID_ADPCM_IMA_AMV ||
        version == AV_CODEC_ID_ADPCM_SWF)
        nodes[0]->sample1 = c->prev_sample;
    if (version == AV_CODEC_ID_ADPCM_MS)
        nodes[0]->step = c->idelta;
    if (version == AV_CODEC_ID_ADPCM_YAMAHA) {
        if (c->step == 0) {
            nodes[0]->step    = 127;
            nodes[0]->sample1 = 0;
        } else {
            nodes[0]->step    = c->step;
            nodes[0]->sample1 = c->predictor;
        }
    }

    for (i = 0; i < n; i++) {
        TrellisNode *t = node_buf + frontier*(i&1);
        TrellisNode **u;
        int sample   = samples[i * stride];
        int heap_pos = 0;
        memset(nodes_next, 0, frontier * sizeof(TrellisNode*));
        for (j = 0; j < frontier && nodes[j]; j++) {
            // higher j have higher ssd already, so they're likely
            // to yield a suboptimal next sample too
            const int range = (j < frontier / 2) ? 1 : 0;
            const int step  = nodes[j]->step;
            int nidx;
            if (version == AV_CODEC_ID_ADPCM_MS) {
                const int predictor = ((nodes[j]->sample1 * c->coeff1) +
                                       (nodes[j]->sample2 * c->coeff2)) / 64;
                const int div  = (sample - predictor) / step;
                const int nmin = av_clip(div-range, -8, 6);
                const int nmax = av_clip(div+range, -7, 7);
                for (nidx = nmin; nidx <= nmax; nidx++) {
                    const int nibble = nidx & 0xf;
                    int dec_sample   = predictor + nidx * step;
#define STORE_NODE(NAME, STEP_INDEX)\
                    int d;\
                    uint32_t ssd;\
                    int pos;\
                    TrellisNode *u;\
                    uint8_t *h;\
                    dec_sample = av_clip_int16(dec_sample);\
                    d = sample - dec_sample;\
                    ssd = nodes[j]->ssd + d*(unsigned)d;\
                    /* Check for wraparound, skip such samples completely. \
                     * Note, changing ssd to a 64 bit variable would be \
                     * simpler, avoiding this check, but it's slower on \
                     * x86 32 bit at the moment. */\
                    if (ssd < nodes[j]->ssd)\
                        goto next_##NAME;\
                    /* Collapse any two states with the same previous sample value. \
                     * One could also distinguish states by step and by 2nd to last
                     * sample, but the effects of that are negligible.
                     * Since nodes in the previous generation are iterated
                     * through a heap, they're roughly ordered from better to
                     * worse, but not strictly ordered. Therefore, an earlier
                     * node with the same sample value is better in most cases
                     * (and thus the current is skipped), but not strictly
                     * in all cases. Only skipping samples where ssd >=
                     * ssd of the earlier node with the same sample gives
                     * slightly worse quality, though, for some reason. */ \
                    h = &hash[(uint16_t) dec_sample];\
                    if (*h == generation)\
                        goto next_##NAME;\
                    if (heap_pos < frontier) {\
                        pos = heap_pos++;\
                    } else {\
                        /* Try to replace one of the leaf nodes with the new \
                         * one, but try a different slot each time. */\
                        pos = (frontier >> 1) +\
                              (heap_pos & ((frontier >> 1) - 1));\
                        if (ssd > nodes_next[pos]->ssd)\
                            goto next_##NAME;\
                        heap_pos++;\
                    }\
                    *h = generation;\
                    u  = nodes_next[pos];\
                    if (!u) {\
                        av_assert1(pathn < FREEZE_INTERVAL << avctx->trellis);\
                        u = t++;\
                        nodes_next[pos] = u;\
                        u->path = pathn++;\
                    }\
                    u->ssd  = ssd;\
                    u->step = STEP_INDEX;\
                    u->sample2 = nodes[j]->sample1;\
                    u->sample1 = dec_sample;\
                    paths[u->path].nibble = nibble;\
                    paths[u->path].prev   = nodes[j]->path;\
                    /* Sift the newly inserted node up in the heap to \
                     * restore the heap property. */\
                    while (pos > 0) {\
                        int parent = (pos - 1) >> 1;\
                        if (nodes_next[parent]->ssd <= ssd)\
                            break;\
                        FFSWAP(TrellisNode*, nodes_next[parent], nodes_next[pos]);\
                        pos = parent;\
                    }\
                    next_##NAME:;
                    STORE_NODE(ms, FFMAX(16,
                               (ff_adpcm_AdaptationTable[nibble] * step) >> 8));
                }
            } else if (version == AV_CODEC_ID_ADPCM_IMA_WAV ||
                       version == AV_CODEC_ID_ADPCM_IMA_QT  ||
                       version == AV_CODEC_ID_ADPCM_IMA_AMV ||
                       version == AV_CODEC_ID_ADPCM_SWF) {
#define LOOP_NODES(NAME, STEP_TABLE, STEP_INDEX)\
                const int predictor = nodes[j]->sample1;\
                const int div = (sample - predictor) * 4 / STEP_TABLE;\
                int nmin = av_clip(div - range, -7, 6);\
                int nmax = av_clip(div + range, -6, 7);\
                if (nmin <= 0)\
                    nmin--; /* distinguish -0 from +0 */\
                if (nmax < 0)\
                    nmax--;\
                for (nidx = nmin; nidx <= nmax; nidx++) {\
                    const int nibble = nidx < 0 ? 7 - nidx : nidx;\
                    int dec_sample = predictor +\
                                    (STEP_TABLE *\
                                     ff_adpcm_yamaha_difflookup[nibble]) / 8;\
                    STORE_NODE(NAME, STEP_INDEX);\
                }
                LOOP_NODES(ima, ff_adpcm_step_table[step],
                           av_clip(step + ff_adpcm_index_table[nibble], 0, 88));
            } else { //AV_CODEC_ID_ADPCM_YAMAHA
                LOOP_NODES(yamaha, step,
                           av_clip((step * ff_adpcm_yamaha_indexscale[nibble]) >> 8,
                                   127, 24576));
#undef LOOP_NODES
#undef STORE_NODE
            }
        }

        u = nodes;
        nodes = nodes_next;
        nodes_next = u;

        generation++;
        if (generation == 255) {
            memset(hash, 0xff, 65536 * sizeof(*hash));
            generation = 0;
        }

        // prevent overflow
        if (nodes[0]->ssd > (1 << 28)) {
            for (j = 1; j < frontier && nodes[j]; j++)
                nodes[j]->ssd -= nodes[0]->ssd;
            nodes[0]->ssd = 0;
        }

        // merge old paths to save memory
        if (i == froze + FREEZE_INTERVAL) {
            p = &paths[nodes[0]->path];
            for (k = i; k > froze; k--) {
                dst[k] = p->nibble;
                p = &paths[p->prev];
            }
            froze = i;
            pathn = 0;
            // other nodes might use paths that don't coincide with the frozen one.
            // checking which nodes do so is too slow, so just kill them all.
            // this also slightly improves quality, but I don't know why.
            memset(nodes + 1, 0, (frontier - 1) * sizeof(TrellisNode*));
        }
    }

    p = &paths[nodes[0]->path];
    for (i = n - 1; i > froze; i--) {
        dst[i] = p->nibble;
        p = &paths[p->prev];
    }

    c->predictor  = nodes[0]->sample1;
    c->sample1    = nodes[0]->sample1;
    c->sample2    = nodes[0]->sample2;
    c->step_index = nodes[0]->step;
    c->step       = nodes[0]->step;
    c->idelta     = nodes[0]->step;
}

#if CONFIG_ADPCM_ARGO_ENCODER
static inline int adpcm_argo_compress_nibble(const ADPCMChannelStatus *cs, int16_t s,
                                             int shift, int flag)
{
    int nibble;

    if (flag)
        nibble = 4 * s - 8 * cs->sample1 + 4 * cs->sample2;
    else
        nibble = 4 * s - 4 * cs->sample1;

    return (nibble >> shift) & 0x0F;
}

static int64_t adpcm_argo_compress_block(ADPCMChannelStatus *cs, PutBitContext *pb,
                                         const int16_t *samples, int nsamples,
                                         int shift, int flag)
{
    int64_t error = 0;

    if (pb) {
        put_bits(pb, 4, shift - 2);
        put_bits(pb, 1, 0);
        put_bits(pb, 1, !!flag);
        put_bits(pb, 2, 0);
    }

    for (int n = 0; n < nsamples; n++) {
        /* Compress the nibble, then expand it to see how much precision we've lost. */
        int nibble = adpcm_argo_compress_nibble(cs, samples[n], shift, flag);
        int16_t sample = ff_adpcm_argo_expand_nibble(cs, nibble, shift, flag);

        error += abs(samples[n] - sample);

        if (pb)
            put_bits(pb, 4, nibble);
    }

    return error;
}
#endif

/* Encode one GameCube DSP-ADPCM block of up to 14 samples for a single
 * channel. The reconstruction here is bit-identical to the ADPCM_THP decoder
 * in adpcm.c, so the encoded nibbles decode back exactly. coefs is the flat
 * 16-entry (8 predictor pairs, Q11) table; hist1/hist2 carry per-channel
 * history and are updated to the last two reconstructed samples. */
static void adpcm_thp_encode_block(const int16_t *samples, int count,
                                   uint8_t *dst, const int16_t *coefs,
                                   int *hist1, int *hist2)
{
    int best_index = 0, best_exp = 0;
    int64_t best_err = INT64_MAX;
    uint8_t best_nib[14] = { 0 };
    int best_h1 = *hist1, best_h2 = *hist2;

    for (int index = 0; index < 8; index++) {
        int64_t f1 = coefs[index * 2], f2 = coefs[index * 2 + 1];
        int h1 = *hist1, h2 = *hist2, maxres = 0;

        for (int n = 0; n < count; n++) {
            int pred = (int)((h1 * f1 + h2 * f2) >> 11);
            int res  = FFABS(samples[n] - pred);
            if (res > maxres)
                maxres = res;
            h2 = h1; h1 = samples[n];
        }

        int exp0 = 0;
        while (exp0 < 15 && (maxres >> exp0) > 7)
            exp0++;

        /* Try the estimated exponent and the next one up (clamp overflow). */
        for (int e = exp0; e <= FFMIN(15, exp0 + 1); e++) {
            int lh1 = *hist1, lh2 = *hist2;
            int64_t err = 0;
            uint8_t nib[14] = { 0 };

            for (int n = 0; n < count; n++) {
                int pred = (int)((lh1 * f1 + lh2 * f2) >> 11);
                int diff = samples[n] - pred;
                int q    = diff >= 0 ?  ((diff  + (1 << e) / 2) >> e)
                                     : -(((-diff) + (1 << e) / 2) >> e);
                int recon, d;
                q = av_clip(q, -8, 7);
                recon = av_clip_int16(pred + q * (1 << e));
                d = samples[n] - recon;
                err += (int64_t)d * d;
                nib[n] = q & 0x0F;
                lh2 = lh1; lh1 = recon;
            }

            if (err < best_err) {
                best_err   = err;
                best_index = index;
                best_exp   = e;
                best_h1    = lh1;
                best_h2    = lh2;
                for (int n = 0; n < count; n++)
                    best_nib[n] = nib[n];
            }
        }
    }

    dst[0] = (best_index << 4) | best_exp;
    for (int n = 0; n < 14; n++) {
        if (n & 1)
            dst[1 + n / 2] |= best_nib[n];
        else
            dst[1 + n / 2]  = best_nib[n] << 4;
    }
    *hist1 = best_h1;
    *hist2 = best_h2;
}

/* THP is encoded specially: the whole input is buffered so the DSP-ADPCM
 * predictor coefficients can be derived over the entire channel (they must be
 * constant stream-wide), then everything is emitted as one packet at drain. */
static int adpcm_encode_thp(AVCodecContext *avctx, AVPacket *avpkt,
                            const AVFrame *frame, int *got_packet_ptr)
{
    ADPCMEncodeContext *c = avctx->priv_data;
    int channels = avctx->ch_layout.nb_channels;
    int ret;

    if (frame) {
        const int16_t *const *samples_p = (const int16_t *const *)frame->extended_data;
        int need = c->thp_pcm_len + frame->nb_samples;

        if (need > c->thp_pcm_cap) {
            int ncap = FFMAX(need, c->thp_pcm_cap ? c->thp_pcm_cap * 2 : 1 << 16);
            for (int ch = 0; ch < channels; ch++) {
                int16_t *nb = av_realloc(c->thp_pcm[ch], ncap * sizeof(int16_t));
                if (!nb)
                    return AVERROR(ENOMEM);
                c->thp_pcm[ch] = nb;
            }
            c->thp_pcm_cap = ncap;
        }
        for (int ch = 0; ch < channels; ch++)
            memcpy(c->thp_pcm[ch] + c->thp_pcm_len, samples_p[ch],
                   frame->nb_samples * sizeof(int16_t));
        c->thp_pcm_len += frame->nb_samples;
        return 0;   /* buffer only; nothing emitted yet */
    }

    /* Draining. */
    if (c->thp_flushed || c->thp_pcm_len == 0)
        return 0;

    int nb     = c->thp_pcm_len;
    int blocks = (nb + 13) / 14;
    int pkt_size = blocks * 8 * channels;
    uint8_t *dst, *side;

    if ((ret = ff_get_encode_buffer(avctx, avpkt, pkt_size, 0)) < 0)
        return ret;
    dst = avpkt->data;

    for (int ch = 0; ch < channels; ch++) {
        int h1 = 0, h2 = 0;
        ret = thp_correlate_coefs(c->thp_pcm[ch], nb, c->thp_dsp_coeffs[ch]);
        if (ret < 0)
            return ret;
        for (int b = 0; b < blocks; b++) {
            int count = FFMIN(14, nb - b * 14);
            adpcm_thp_encode_block(&c->thp_pcm[ch][b * 14], count,
                                   dst + (ch * blocks + b) * 8,
                                   c->thp_dsp_coeffs[ch], &h1, &h2);
        }
    }

    side = av_packet_new_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, 32 * channels);
    if (!side)
        return AVERROR(ENOMEM);
    for (int ch = 0; ch < channels; ch++)
        for (int i = 0; i < 16; i++)
            AV_WB16(side + ch * 32 + i * 2, c->thp_dsp_coeffs[ch][i]);

    avpkt->pts      = 0;
    avpkt->duration = nb;
    c->thp_flushed  = 1;
    *got_packet_ptr = 1;
    return 0;
}

static int adpcm_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                              const AVFrame *frame, int *got_packet_ptr)
{
    int st, pkt_size, ret;
    const int16_t *samples;
    const int16_t *const *samples_p;
    uint8_t *dst;
    ADPCMEncodeContext *c = avctx->priv_data;
    int channels = avctx->ch_layout.nb_channels;

    if (avctx->codec_id == AV_CODEC_ID_ADPCM_THP)
        return adpcm_encode_thp(avctx, avpkt, frame, got_packet_ptr);

    samples = (const int16_t *)frame->data[0];
    samples_p = (const int16_t *const *)frame->extended_data;
    st = channels == 2;

    if (avctx->codec_id == AV_CODEC_ID_ADPCM_IMA_SSI ||
        avctx->codec_id == AV_CODEC_ID_ADPCM_IMA_ALP ||
        avctx->codec_id == AV_CODEC_ID_ADPCM_IMA_APM ||
        avctx->codec_id == AV_CODEC_ID_ADPCM_IMA_WS)
        pkt_size = (frame->nb_samples * channels + 1) / 2;
    else if (avctx->codec_id == AV_CODEC_ID_ADPCM_THP)
        /* Emit exactly the bytes used (8 per 14-sample block per channel) so a
         * partial final frame doesn't leave the muxer to mis-split channels. */
        pkt_size = ((frame->nb_samples + 13) / 14) * 8 * channels;
    else
        pkt_size = avctx->block_align;
    if ((ret = ff_get_encode_buffer(avctx, avpkt, pkt_size, 0)) < 0)
        return ret;
    dst = avpkt->data;

    switch(avctx->codec->id) {
    CASE(ADPCM_IMA_WAV,
        int blocks = (frame->nb_samples - 1) / 8;

        for (int ch = 0; ch < channels; ch++) {
            ADPCMChannelStatus *status = &c->status[ch];
            status->prev_sample = samples_p[ch][0];
            status->step_index = 0;
            bytestream_put_le16(&dst, status->prev_sample);
            *dst++ = status->step_index;
            *dst++ = 0; /* unknown */
        }

        /* stereo: 4 bytes (8 samples) for left, 4 bytes for right */
        if (avctx->trellis > 0) {
            uint8_t *buf;
            if (!FF_ALLOC_TYPED_ARRAY(buf, channels * blocks * 8))
                return AVERROR(ENOMEM);
            for (int ch = 0; ch < channels; ch++) {
                adpcm_compress_trellis(avctx, &samples_p[ch][1],
                                       buf + ch * blocks * 8, &c->status[ch],
                                       blocks * 8, 1);
            }
            for (int i = 0; i < blocks; i++) {
                for (int ch = 0; ch < channels; ch++) {
                    uint8_t *buf1 = buf + ch * blocks * 8 + i * 8;
                    for (int j = 0; j < 8; j += 2)
                        *dst++ = buf1[j] | (buf1[j + 1] << 4);
                }
            }
            av_free(buf);
        } else {
            for (int i = 0; i < blocks; i++) {
                for (int ch = 0; ch < channels; ch++) {
                    ADPCMChannelStatus *status = &c->status[ch];
                    const int16_t *smp = &samples_p[ch][1 + i * 8];
                    for (int j = 0; j < 8; j += 2) {
                        uint8_t v = adpcm_ima_compress_sample(status, smp[j    ]);
                        v        |= adpcm_ima_compress_sample(status, smp[j + 1]) << 4;
                        *dst++ = v;
                    }
                }
            }
        }
        ) /* End of CASE */
    CASE(ADPCM_IMA_QT,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        for (int ch = 0; ch < channels; ch++) {
            ADPCMChannelStatus *status = &c->status[ch];
            status->prev_sample = samples_p[ch][0];
            put_bits(&pb, 9, (status->prev_sample & 0xFFFF) >> 7);
            put_bits(&pb, 7,  status->step_index);
            if (avctx->trellis > 0) {
                uint8_t buf[64];
                adpcm_compress_trellis(avctx, &samples_p[ch][0], buf, status,
                                       64, 1);
                for (int i = 0; i < 64; i++)
                    put_bits(&pb, 4, buf[i ^ 1]);
                status->prev_sample = status->predictor;
            } else {
                for (int i = 0; i < 64; i += 2) {
                    int t1, t2;
                    t1 = adpcm_ima_qt_compress_sample(status, samples_p[ch][i    ]);
                    t2 = adpcm_ima_qt_compress_sample(status, samples_p[ch][i + 1]);
                    put_bits(&pb, 4, t2);
                    put_bits(&pb, 4, t1);
                }
            }
        }

        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_IMA_SSI,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        av_assert0(avctx->trellis == 0);

        for (int i = 0; i < frame->nb_samples; i++) {
            for (int ch = 0; ch < channels; ch++) {
                put_bits(&pb, 4, adpcm_ima_qt_compress_sample(c->status + ch, *samples++));
            }
        }

        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_IMA_ALP,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        av_assert0(avctx->trellis == 0);

        for (int n = frame->nb_samples / 2; n > 0; n--) {
            for (int ch = 0; ch < channels; ch++) {
                put_bits(&pb, 4, adpcm_ima_alp_compress_sample(c->status + ch, *samples++));
                put_bits(&pb, 4, adpcm_ima_alp_compress_sample(c->status + ch, samples[st]));
            }
            samples += channels;
        }

        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_SWF,
        const int n = frame->nb_samples - 1;
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        /* NB: This is safe as we don't have AV_CODEC_CAP_SMALL_LAST_FRAME. */
        av_assert0(n == 4095);

        // store AdpcmCodeSize
        put_bits(&pb, 2, 2);    // set 4-bit flash adpcm format

        // init the encoder state
        for (int i = 0; i < channels; i++) {
            // clip step so it fits 6 bits
            c->status[i].step_index = av_clip_uintp2(c->status[i].step_index, 6);
            put_sbits(&pb, 16, samples[i]);
            put_bits(&pb, 6, c->status[i].step_index);
            c->status[i].prev_sample = samples[i];
        }

        if (avctx->trellis > 0) {
            uint8_t buf[8190 /* = 2 * n */];
            adpcm_compress_trellis(avctx, samples + channels, buf,
                                   &c->status[0], n, channels);
            if (channels == 2)
                adpcm_compress_trellis(avctx, samples + channels + 1,
                                       buf + n, &c->status[1], n,
                                       channels);
            for (int i = 0; i < n; i++) {
                put_bits(&pb, 4, buf[i]);
                if (channels == 2)
                    put_bits(&pb, 4, buf[n + i]);
            }
        } else {
            for (int i = 1; i < frame->nb_samples; i++) {
                put_bits(&pb, 4, adpcm_ima_compress_sample(&c->status[0],
                         samples[channels * i]));
                if (channels == 2)
                    put_bits(&pb, 4, adpcm_ima_compress_sample(&c->status[1],
                             samples[2 * i + 1]));
            }
        }
        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_MS,
        for (int i = 0; i < channels; i++) {
            int predictor = 0;
            *dst++ = predictor;
            c->status[i].coeff1 = ff_adpcm_AdaptCoeff1[predictor];
            c->status[i].coeff2 = ff_adpcm_AdaptCoeff2[predictor];
        }
        for (int i = 0; i < channels; i++) {
            if (c->status[i].idelta < 16)
                c->status[i].idelta = 16;
            bytestream_put_le16(&dst, c->status[i].idelta);
        }
        for (int i = 0; i < channels; i++)
            c->status[i].sample2= *samples++;
        for (int i = 0; i < channels; i++) {
            c->status[i].sample1 = *samples++;
            bytestream_put_le16(&dst, c->status[i].sample1);
        }
        for (int i = 0; i < channels; i++)
            bytestream_put_le16(&dst, c->status[i].sample2);

        if (avctx->trellis > 0) {
            const int n  = avctx->block_align - 7 * channels;
            uint8_t *buf = av_malloc(2 * n);
            if (!buf)
                return AVERROR(ENOMEM);
            if (channels == 1) {
                adpcm_compress_trellis(avctx, samples, buf, &c->status[0], n,
                                       channels);
                for (int i = 0; i < n; i += 2)
                    *dst++ = (buf[i] << 4) | buf[i + 1];
            } else {
                adpcm_compress_trellis(avctx, samples,     buf,
                                       &c->status[0], n, channels);
                adpcm_compress_trellis(avctx, samples + 1, buf + n,
                                       &c->status[1], n, channels);
                for (int i = 0; i < n; i++)
                    *dst++ = (buf[i] << 4) | buf[n + i];
            }
            av_free(buf);
        } else {
            for (int i = 7 * channels; i < avctx->block_align; i++) {
                int nibble;
                nibble  = adpcm_ms_compress_sample(&c->status[ 0], *samples++) << 4;
                nibble |= adpcm_ms_compress_sample(&c->status[st], *samples++);
                *dst++  = nibble;
            }
        }
        ) /* End of CASE */
    CASE(ADPCM_YAMAHA,
        int n = frame->nb_samples / 2;
        if (avctx->trellis > 0) {
            uint8_t *buf = av_malloc(2 * n * 2);
            if (!buf)
                return AVERROR(ENOMEM);
            n *= 2;
            if (channels == 1) {
                adpcm_compress_trellis(avctx, samples, buf, &c->status[0], n,
                                       channels);
                for (int i = 0; i < n; i += 2)
                    *dst++ = buf[i] | (buf[i + 1] << 4);
            } else {
                adpcm_compress_trellis(avctx, samples,     buf,
                                       &c->status[0], n, channels);
                adpcm_compress_trellis(avctx, samples + 1, buf + n,
                                       &c->status[1], n, channels);
                for (int i = 0; i < n; i++)
                    *dst++ = buf[i] | (buf[n + i] << 4);
            }
            av_free(buf);
        } else
            for (n *= channels; n > 0; n--) {
                int nibble;
                nibble  = adpcm_yamaha_compress_sample(&c->status[ 0], *samples++);
                nibble |= adpcm_yamaha_compress_sample(&c->status[st], *samples++) << 4;
                *dst++  = nibble;
            }
        ) /* End of CASE */
    CASE(ADPCM_IMA_APM,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        av_assert0(avctx->trellis == 0);

        for (int n = frame->nb_samples / 2; n > 0; n--) {
            for (int ch = 0; ch < channels; ch++) {
                put_bits(&pb, 4, adpcm_ima_qt_compress_sample(c->status + ch, *samples++));
                put_bits(&pb, 4, adpcm_ima_qt_compress_sample(c->status + ch, samples[st]));
            }
            samples += channels;
        }

        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_IMA_AMV,
        av_assert0(channels == 1);

        c->status[0].prev_sample = *samples;
        bytestream_put_le16(&dst, c->status[0].prev_sample);
        bytestream_put_byte(&dst, c->status[0].step_index);
        bytestream_put_byte(&dst, 0);
        bytestream_put_le32(&dst, avctx->frame_size);

        if (avctx->trellis > 0) {
            const int n  = frame->nb_samples >> 1;
            uint8_t *buf = av_malloc(2 * n);

            if (!buf)
                return AVERROR(ENOMEM);

            adpcm_compress_trellis(avctx, samples, buf, &c->status[0], 2 * n, channels);
            for (int i = 0; i < n; i++)
                bytestream_put_byte(&dst, (buf[2 * i] << 4) | buf[2 * i + 1]);

            samples += 2 * n;
            av_free(buf);
        } else for (int n = frame->nb_samples >> 1; n > 0; n--) {
            int nibble;
            nibble  = adpcm_ima_compress_sample(&c->status[0], *samples++) << 4;
            nibble |= adpcm_ima_compress_sample(&c->status[0], *samples++) & 0x0F;
            bytestream_put_byte(&dst, nibble);
        }

        if (avctx->frame_size & 1) {
            int nibble = adpcm_ima_compress_sample(&c->status[0], *samples++) << 4;
            bytestream_put_byte(&dst, nibble);
        }
        ) /* End of CASE */
    CASE(ADPCM_ARGO,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        av_assert0(frame->nb_samples == 32);

        for (int ch = 0; ch < channels; ch++) {
            int64_t error  = INT64_MAX, tmperr = INT64_MAX;
            int     shift  = 2, flag = 0;
            int     saved1 = c->status[ch].sample1;
            int     saved2 = c->status[ch].sample2;

            /* Find the optimal coefficients, bail early if we find a perfect result. */
            for (int s = 2; s < 18 && tmperr != 0; s++) {
                for (int f = 0; f < 2 && tmperr != 0; f++) {
                    c->status[ch].sample1 = saved1;
                    c->status[ch].sample2 = saved2;
                    tmperr = adpcm_argo_compress_block(c->status + ch, NULL, samples_p[ch],
                                                       frame->nb_samples, s, f);
                    if (tmperr < error) {
                        shift = s;
                        flag  = f;
                        error = tmperr;
                    }
                }
            }

            /* Now actually do the encode. */
            c->status[ch].sample1 = saved1;
            c->status[ch].sample2 = saved2;
            adpcm_argo_compress_block(c->status + ch, &pb, samples_p[ch],
                                      frame->nb_samples, shift, flag);
        }

        flush_put_bits(&pb);
        ) /* End of CASE */
    CASE(ADPCM_IMA_WS,
        PutBitContext pb;
        init_put_bits(&pb, dst, pkt_size);

        av_assert0(avctx->trellis == 0);
        for (int n = frame->nb_samples / 2; n > 0; n--) {
            /* stereo: 1 byte (2 samples) for left, 1 byte for right */
            for (int ch = 0; ch < channels; ch++) {
                int t1, t2;
                t1 = adpcm_ima_compress_sample(&c->status[ch], *samples++);
                t2 = adpcm_ima_compress_sample(&c->status[ch], samples[st]);
                put_bits(&pb, 4, t2);
                put_bits(&pb, 4, t1);
            }
            samples += channels;
        }
        flush_put_bits(&pb);
        ) /* End of CASE */
    
    /* ADPCM_THP is handled entirely by adpcm_encode_thp() (whole-stream
     * buffering + derived coefficients), dispatched before this switch. */

    CASE(ADPCM_IMA_MOFLEX,
        /* Header: [LE16 step_index][LE16 predictor] per channel */
        for (int ch = 0; ch < channels; ch++) {
            ADPCMChannelStatus *cs = &c->status[ch];
            bytestream_put_le16(&dst, cs->step_index);
            bytestream_put_le16(&dst, cs->prev_sample);
        }
        /* 256 IMA ADPCM nibble samples per channel, interleaved by channel */
        for (int ch = 0; ch < channels; ch++) {
            ADPCMChannelStatus *cs = &c->status[ch];
            for (int n = 0; n < 256; n += 2) {
                uint8_t nibble;
                nibble  = adpcm_ima_compress_sample(cs, samples_p[ch][n])     & 0x0F;
                nibble |= adpcm_ima_compress_sample(cs, samples_p[ch][n + 1]) << 4;
                *dst++ = nibble;
            }
        }
        ) /* End of CASE */
    default:
        return AVERROR(EINVAL);
    }

    *got_packet_ptr = 1;
    return 0;
}

static const enum AVSampleFormat sample_fmts[] = {
    AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE
};

static const enum AVSampleFormat sample_fmts_p[] = {
    AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_NONE
};

static const AVChannelLayout ch_layouts_mono_stereo[] = {
    AV_CHANNEL_LAYOUT_MONO,
    AV_CHANNEL_LAYOUT_STEREO,
    { 0 },
};

static const AVOption options[] = {
    {
        .name        = "block_size",
        .help        = "set the block size",
        .offset      = offsetof(ADPCMEncodeContext, block_size),
        .type        = AV_OPT_TYPE_INT,
        .default_val = {.i64 = 1024},
        .min         = 32,
        .max         = 8192, /* Is this a reasonable upper limit? */
        .flags       = AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM
    },
    { NULL }
};

static const AVClass adpcm_encoder_class = {
    .class_name = "ADPCM encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

#define ADPCM_ENCODER_0(id_, name_, sample_fmts_, capabilities_, long_name_, ...)
#define ADPCM_ENCODER_1(id_, name_, sample_fmts_, capabilities_, long_name_, ...) \
const FFCodec ff_ ## name_ ## _encoder = {                                 \
    .p.name         = #name_,                                              \
    CODEC_LONG_NAME(long_name_),                                           \
    .p.type         = AVMEDIA_TYPE_AUDIO,                                  \
    .p.id           = id_,                                                 \
    .p.capabilities = capabilities_ | AV_CODEC_CAP_DR1 |                   \
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,               \
    CODEC_SAMPLEFMTS_ARRAY(sample_fmts_),                                  \
    .priv_data_size = sizeof(ADPCMEncodeContext),                          \
    .init           = adpcm_encode_init,                                   \
    FF_CODEC_ENCODE_CB(adpcm_encode_frame),                                \
    .close          = adpcm_encode_close,                                  \
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,                           \
    __VA_ARGS__,                                                           \
};
#define ADPCM_ENCODER_2(enabled, codec_id, name, sample_fmts, capabilities, long_name, ...) \
    ADPCM_ENCODER_ ## enabled(codec_id, name, sample_fmts, capabilities, long_name, __VA_ARGS__)
#define ADPCM_ENCODER_3(config, codec_id, name, sample_fmts, capabilities, long_name, ...) \
    ADPCM_ENCODER_2(config, codec_id, name, sample_fmts, capabilities, long_name, __VA_ARGS__)
#define ADPCM_ENCODER(codec, name, sample_fmts, capabilities, long_name, ...) \
    ADPCM_ENCODER_3(CONFIG_ ## codec ## _ENCODER, AV_CODEC_ID_ ## codec, \
                    name, sample_fmts, capabilities, long_name, __VA_ARGS__)

#define MONO_STEREO CODEC_CH_LAYOUTS_ARRAY(ch_layouts_mono_stereo)
#define AVCLASS .p.priv_class = &adpcm_encoder_class

ADPCM_ENCODER(ADPCM_ARGO,    adpcm_argo,    sample_fmts_p, 0,                             "ADPCM Argonaut Games",                   MONO_STEREO)
ADPCM_ENCODER(ADPCM_IMA_AMV, adpcm_ima_amv, sample_fmts,   0,                             "ADPCM IMA AMV",                          CODEC_CH_LAYOUTS(AV_CHANNEL_LAYOUT_MONO), CODEC_SAMPLERATES(22050), AVCLASS)
ADPCM_ENCODER(ADPCM_IMA_APM, adpcm_ima_apm, sample_fmts,   AV_CODEC_CAP_SMALL_LAST_FRAME, "ADPCM IMA Ubisoft APM",                  MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_IMA_ALP, adpcm_ima_alp, sample_fmts,   AV_CODEC_CAP_SMALL_LAST_FRAME, "ADPCM IMA High Voltage Software ALP",    MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_IMA_QT,  adpcm_ima_qt,  sample_fmts_p, 0,                             "ADPCM IMA QuickTime",                    MONO_STEREO)
ADPCM_ENCODER(ADPCM_IMA_SSI, adpcm_ima_ssi, sample_fmts,   AV_CODEC_CAP_SMALL_LAST_FRAME, "ADPCM IMA Simon & Schuster Interactive", MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_IMA_WAV,    adpcm_ima_wav,    sample_fmts_p, 0, "ADPCM IMA WAV",              MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_IMA_MOFLEX, adpcm_ima_moflex, sample_fmts_p, 0, "ADPCM IMA MobiClip MOFLEX", MONO_STEREO)
ADPCM_ENCODER(ADPCM_THP,        adpcm_thp,        sample_fmts_p, AV_CODEC_CAP_DELAY, "ADPCM Nintendo THP", MONO_STEREO)
ADPCM_ENCODER(ADPCM_IMA_WS,  adpcm_ima_ws,  sample_fmts,   AV_CODEC_CAP_SMALL_LAST_FRAME, "ADPCM IMA Westwood",                     MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_MS,      adpcm_ms,      sample_fmts,   0,                             "ADPCM Microsoft",                        MONO_STEREO, AVCLASS)
ADPCM_ENCODER(ADPCM_SWF,     adpcm_swf,     sample_fmts,   0,                             "ADPCM Shockwave Flash",                  MONO_STEREO, CODEC_SAMPLERATES(11025, 22050, 44100))
ADPCM_ENCODER(ADPCM_YAMAHA,  adpcm_yamaha,  sample_fmts,   0,                             "ADPCM Yamaha",                           MONO_STEREO, AVCLASS)
