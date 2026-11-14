/*
 * MobiClip / ActImagine DS (.vx / .mods "SX") Audio encoder
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
 * The ActImagine "VX audio" codec and the Mobiclip DS "SX / FastAudio-codebook"
 * codec are the SAME codec (verified bit-exact: the .mods SX bitstream +
 * codebook decode perfectly under the VX semantics). It is a backward-adaptive
 * sparse-excitation LPC coder, 128 samples per frame:
 *
 *   sample[i] = (pulses[i]*0x4000 + sum_j sample[i-1-j]*influence[j]) >> 14
 *
 * per frame a 16-bit header carries prev_frame_offset (0x7F=intra, 0x7E=inter
 * with cleared excitation baseline, <0x7E=inter with the previous frames'
 * excitation copied at that lag = a long-term/pitch predictor), a 3-bit gain
 * index into scale_modifiers (multiplicative: scale = prev_scale*mod >> 13),
 * and three 6-bit indices into per-file trained codebooks whose 8-dim entries
 * are DELTAS added to the running reflection-coefficient filter. A second
 * 16-bit word carries the pulse grid phase and packing mode; then the grid
 * pulses themselves.
 *
 * This encoder is a port of the standalone SX encoder (~/sx-port/c/sx_encode.c,
 * built against Nintendo's bit-exact decoder blob):
 *   - buffers the whole stream (CAP_DELAY), then trains the three 64-entry
 *     codebooks by 3-stage residual k-means over the inter-frame differential
 *     PARCOR of the input; frame 0's PARCOR becomes lpc_base;
 *   - per frame: 3-stage VQ of (target PARCOR - running filter) with a
 *     stability fallback, then a joint closed-loop search over
 *     prev_frame_offset (LTP lag) x gain index x grid phase, choosing each
 *     grid pulse by analysis-by-synthesis against the exact decoder recursion;
 *   - commits state by replaying the exact decoder math on what was emitted,
 *     so encoder state == decoder state (no drift).
 *
 * The trained codebook (3124 bytes per channel) is handed to the muxer via
 * AV_PKT_DATA_NEW_EXTRADATA on the first output packet. Packets are one
 * 128-sample period each; for stereo the channels' frames are concatenated
 * ch0,ch1 within the packet (the .mods interleaving).
 */

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/qsort.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"

#define AFRAME_SAMPLES 128
#define NUM_PULSES     42        /* packing mode 0: densest grid */
#define PULSE_DISTANCE 3
#define VQN            64
#define MAX_CH         2

#define VXA_EXTRADATA_SIZE (3*64*8*2 + 8*2 + 8*4 + 4)   /* 3124 per channel */

/* gain ladder (Q13 ratios 0.35..4.0 around unity) and initial gain seed;
 * same values the standalone SX encoder validated against the HW decoder. */
static const uint16_t scale_lut[8] = { 2896, 4096, 5793, 8192, 11585, 16384, 23170, 32767 };
#define INIT_ACC 16

typedef struct VXAChan {
    /* committed decoder-mirror state (identical fields to the decoder) */
    int pulses_prev[AFRAME_SAMPLES];
    int pulses_prev2[AFRAME_SAMPLES];
    int samples_prev[8];
    int scale_prev;
    int lpc_filter_prev[8];
    int influence_prev[8];
    int have_prev, have_prev2;

    /* trained model */
    int16_t vq[3][VQN][8];
    int32_t base[8];
} VXAChan;

/* The closed-loop search walks (prev_frame_offset, gain, grid phase) triples.
 * Candidates are numbered in the order the reference search visits them --
 * pfo ascending, then gain, then phase -- and the winner is the lowest-numbered
 * candidate with the smallest error, whatever order they are actually tried in. */
#define VXA_NUM_SMI   8
#define VXA_NUM_PAD   4
#define VXA_CAND(pfo, smi, pad) (((pfo) * VXA_NUM_SMI + (smi)) * VXA_NUM_PAD + (pad))
/* The lag search is cut into one job per (gain, grid phase): small enough for
 * the pool to balance across unequal cores, and each a run of full groups. */
#define VXA_JOBS      (VXA_NUM_SMI * VXA_NUM_PAD)

typedef struct VXAResult {
    double err;                  /* < 0: nothing found */
    int    cand;
    uint8_t pv[NUM_PULSES];
} VXAResult;

/* Candidates are evaluated VXA_LANES at a time.  Each is a serial recursion,
 * so one alone is bound by the latency of its divide and multiply chain;
 * side by side they cost little more than one.  A group shares its grid
 * phase, which lines every lane's pulses up on the same samples. */
#define VXA_LANES 16

/* Lags 0..0x7d, then VXA_LANES all-zero columns for the lag-free candidates
 * (and for a group to run off the end of the lags into). */
#define VXA_BL_STRIDE (0x7e + VXA_LANES)

typedef struct VXAGroup {
    const int32_t *baseline;     /* [AFRAME_SAMPLES][VXA_BL_STRIDE], lane = column */
    int n;                       /* lanes in use */
    int pad;
    int32_t scale[VXA_LANES];
    int     cand[VXA_LANES];
    double  err[VXA_LANES];
    int32_t pv[NUM_PULSES][VXA_LANES];
} VXAGroup;

/* Everything one block's search needs that does not depend on the candidate. */
typedef struct VXASearch {
    const VXAChan *ch;
    int16_t target[AFRAME_SAMPLES];
    int     quarters[4][8];
    int     scale[VXA_NUM_SMI];          /* < 1: gain unusable, skipped */
    int     smi_order[VXA_NUM_SMI];      /* usable gains, most promising first */
    int     nb_smi;
    int     lagged;                      /* lags 0..0x7d are to be searched */
    /* Smallest error any job has completed so far, as the bits of a double.
     * Only ever used to abandon candidates that are already worse, so its
     * timing cannot change which candidate wins. */
    atomic_uint_least64_t bound;
    VXAResult *res;                      /* best so far, one per thread */
    int     nb_res;
    /* excitation baseline of every lag, one row per sample */
    int32_t baseline[AFRAME_SAMPLES][VXA_BL_STRIDE];
} VXASearch;

typedef struct VXAudioEncContext {
    const AVClass *class;
    int nch;
    int ltp;                     /* option: search LTP lags (slow, better) */
    char *intra_periods_str;     /* option: CSV of period indices to force intra */

    VXAChan ch[MAX_CH];
    VXASearch *search;           /* [nch] */

    /* whole-stream buffer, deinterleaved */
    int16_t *buf[MAX_CH];
    int64_t  nsamp, bufcap;

    int32_t (*pcd)[MAX_CH][8];   /* per-block PARCOR targets (Q15) */
    int64_t  nblk, blk_pos;
    int      trained, sent_extradata;

    /* Period indices (sorted) at which to emit an INTRA aframe regardless of the
     * running state.  The mods container re-primes its SX audio decoder at every
     * video keyframe (retail writes an intra aframe there); encode.py passes the
     * keyframe period positions here so our stream matches and doesn't glitch on
     * the game's per-keyframe reset. */
    int64_t *kf_periods;
    int      kf_n;
} VXAudioEncContext;

static int vxa_is_kf_period(const VXAudioEncContext *s, int64_t bi);

/* ---------- analysis ---------- */

static void parcor_block(const int16_t *x, int n, int rate, double k[8])
{
    double r[9] = {0}, a[9] = {0}, e;
    for (int i = 0; i <= 8; i++) {
        double s = 0;
        for (int j = 0; j < n - i; j++) s += (double)x[j] * x[j + i];
        r[i] = s;
    }
    /* white-noise floor + lag window (~200 Hz bandwidth expansion) keeps the
     * reflection coeffs inside |k|<1 even after quantization */
    r[0] *= 1.0003;
    for (int i = 1; i <= 8; i++)
        r[i] *= exp(-0.5 * pow(2 * M_PI * 200.0 * i / rate, 2));
    for (int i = 0; i < 8; i++) k[i] = 0;
    e = r[0];
    if (e <= 0) return;
    for (int i = 1; i <= 8; i++) {
        double s = r[i], na[9], ki;
        for (int j = 1; j < i; j++) s += a[j] * r[i - j];
        ki = -s / e;
        ki = av_clipd(ki, -0.9999, 0.9999);
        k[i - 1] = ki;
        for (int j = 1; j < i; j++) na[j] = a[j] + ki * a[i - j];
        for (int j = 1; j < i; j++) a[j] = na[j];
        a[i] = ki;
        e *= 1.0 - ki * ki;
        if (e <= 0) break;
    }
}

static inline int qs16(double v)
{
    return (int)av_clip(lrint(v), -32768, 32767);
}

/* ---------- 3-stage residual VQ training (k-means) ---------- */

static void kmeans64(double (*data)[8], int64_t nv, int16_t out[VQN][8])
{
    double cen[VQN][8];
    for (int v = 0; v < VQN; v++)
        for (int j = 0; j < 8; j++)
            cen[v][j] = nv ? data[(int64_t)v * nv / VQN][j] : 0;
    for (int it = 0; it < 18; it++) {
        double sum[VQN][8] = {{0}};
        int64_t cnt[VQN] = {0};
        for (int64_t i = 0; i < nv; i++) {
            double best = 1e30; int bv = 0;
            for (int v = 0; v < VQN; v++) {
                double d = 0;
                for (int j = 0; j < 8; j++) { double e = data[i][j] - cen[v][j]; d += e * e; }
                if (d < best) { best = d; bv = v; }
            }
            for (int j = 0; j < 8; j++) sum[bv][j] += data[i][j];
            cnt[bv]++;
        }
        for (int v = 0; v < VQN; v++)
            if (cnt[v])
                for (int j = 0; j < 8; j++) cen[v][j] = sum[v][j] / cnt[v];
    }
    for (int v = 0; v < VQN; v++)
        for (int j = 0; j < 8; j++)
            out[v][j] = (int16_t)qs16(cen[v][j]);
}

static int vq_nearest(const int16_t cb[VQN][8], const double t[8])
{
    double best = 1e30; int bi = 0;
    for (int v = 0; v < VQN; v++) {
        double d = 0;
        for (int j = 0; j < 8; j++) { double e = t[j] - cb[v][j]; d += e * e; }
        if (d < best) { best = d; bi = v; }
    }
    return bi;
}

/* ---------- exact decoder math (mirrors vx_audio.c) ---------- */

static void compute_influence(const int lpc[8], int influence[8])
{
    int psi[8], old[8], len = 0;
    for (int i = 0; i < 8; i++) {
        int coeff = lpc[i];
        memcpy(old, psi, sizeof(int) * len);
        for (int j = 0; j < i; j++)
            psi[j] = old[j] + (int)(((int64_t)old[i - j - 1] * coeff) >> 15);
        psi[i] = coeff;
        len = i + 1;
    }
    for (int i = 0; i < 8; i++)
        influence[i] = -(psi[i] >> 1);
}

/* excitation baseline the decoder builds before adding grid pulses */
static void build_baseline(const VXAChan *c, int pfo, int baseline[AFRAME_SAMPLES])
{
    if (pfo < 0x7e) {
        int concat[AFRAME_SAMPLES * 2];
        for (int i = 0; i < AFRAME_SAMPLES; i++)
            concat[i] = c->have_prev2 ? c->pulses_prev2[i] : 0;
        for (int i = 0; i < AFRAME_SAMPLES; i++)
            concat[AFRAME_SAMPLES + i] = c->have_prev ? c->pulses_prev[i] : 0;
        for (int i = 0; i < AFRAME_SAMPLES; i++) {
            int volume = FFMIN3(8, i + 1, AFRAME_SAMPLES - i);
            baseline[i] = (concat[i + 0x7f - pfo] * volume) >> 4;
        }
    } else {
        memset(baseline, 0, sizeof(int) * AFRAME_SAMPLES);
    }
}

/* per-quarter influence sets, exactly as the decoder interpolates them */
static void make_quarters(const VXAChan *c, int intra, const int influence[8],
                          int quarters[4][8])
{
    if (!intra) {
        for (int j = 0; j < 8; j++) quarters[3][j] = influence[j];
        for (int j = 0; j < 8; j++) quarters[1][j] = (c->influence_prev[j] + quarters[3][j]) >> 1;
        for (int j = 0; j < 8; j++) quarters[0][j] = (c->influence_prev[j] + quarters[1][j]) >> 1;
        for (int j = 0; j < 8; j++) quarters[2][j] = (quarters[1][j] + quarters[3][j]) >> 1;
    } else {
        for (int q = 0; q < 4; q++)
            for (int j = 0; j < 8; j++)
                quarters[q][j] = influence[j];
    }
}

/* Synthesize one frame with the exact decoder recursion. If choose != NULL,
 * grid pulses are selected on the fly by analysis-by-synthesis against
 * target[] and written to choose[]; otherwise pulse values come from pv[].
 * Returns the squared error vs target (if given). When commit is set, the
 * channel's decoder-mirror state is advanced. */
static double run_aframe(VXAChan *c, int pfo, int smi, const int idx[3],
                         int pad, const int32_t scale_initial,
                         const int16_t *target, int *choose, const int *pv,
                         int commit)
{
    int scale, lpc[8], influence[8], quarters[4][8];
    int pulses[AFRAME_SAMPLES], samples[AFRAME_SAMPLES];
    int intra = (pfo == 0x7f);
    double err = 0;

    scale = c->have_prev ? c->scale_prev : (int)scale_initial;
    if (intra)
        scale = (int)scale_initial;
    scale = (scale * (int)scale_lut[smi]) >> 13;

    if (intra)
        for (int i = 0; i < 8; i++) lpc[i] = c->base[i];
    else
        for (int i = 0; i < 8; i++) lpc[i] = c->lpc_filter_prev[i];
    for (int k = 0; k < 8; k++)
        lpc[k] += c->vq[0][idx[0]][k] + c->vq[1][idx[1]][k] + c->vq[2][idx[2]][k];

    build_baseline(c, pfo, pulses);
    compute_influence(lpc, influence);
    make_quarters(c, intra, influence, quarters);

    for (int i = 0; i < AFRAME_SAMPLES; i++) {
        const int *inf = quarters[i * 4 / AFRAME_SAMPLES];
        int diff = i - pad;
        int64_t P = 0;

        for (int j = 0; j < 8; j++) {
            int si = i - 1 - j;
            int prev = (si < 0) ? c->samples_prev[8 + si] : samples[si];
            P += (int64_t)prev * inf[j];
        }

        if (diff >= 0 && diff % PULSE_DISTANCE == 0 && diff / PULSE_DISTANCE < NUM_PULSES) {
            int k = diff / PULSE_DISTANCE, val;
            if (choose) {
                /* sample ~= baseline + val*scale + P>>14; solve for val, snap
                 * to the odd grid {-7..7} (matches the standalone encoder) */
                int pr = (int)(P >> 14);
                int q = scale > 0 ?
                        (int)lrint(((double)(target[i] - pr - pulses[i]) / scale + 7.0) / 2.0) : 3;
                q = av_clip(q, 0, 7);
                choose[k] = q;
                val = 2 * q - 7;
            } else {
                val = 2 * pv[k] - 7;
            }
            pulses[i] += val * scale;
        }

        {
            int64_t sample = (int64_t)pulses[i] * 0x4000 + P;
            samples[i] = (int)(sample >> 14);
        }
        if (target) {
            double d = (double)samples[i] - target[i];
            err += d * d;
        }
    }

    if (commit) {
        memcpy(c->pulses_prev2, c->pulses_prev, sizeof(c->pulses_prev2));
        c->have_prev2 = c->have_prev;
        memcpy(c->pulses_prev, pulses, sizeof(c->pulses_prev));
        memcpy(c->samples_prev, &samples[AFRAME_SAMPLES - 8], sizeof(c->samples_prev));
        c->scale_prev = scale;
        memcpy(c->lpc_filter_prev, lpc, sizeof(c->lpc_filter_prev));
        memcpy(c->influence_prev, influence, sizeof(c->influence_prev));
        c->have_prev = 1;
    }
    return err;
}

/* ---------- training over the buffered stream ---------- */

static int train(AVCodecContext *avctx)
{
    VXAudioEncContext *s = avctx->priv_data;
    int64_t nblk = (s->nsamp + AFRAME_SAMPLES - 1) / AFRAME_SAMPLES;
    double (*d)[8] = NULL;
    int ret = 0;

    if (!nblk)
        nblk = 1;
    s->nblk = nblk;
    s->pcd = av_calloc(nblk, sizeof(*s->pcd));
    d = av_calloc(nblk > 1 ? nblk - 1 : 1, sizeof(*d));
    if (!s->pcd || !d) { ret = AVERROR(ENOMEM); goto end; }

    for (int c = 0; c < s->nch; c++) {
        VXAChan *ch = &s->ch[c];

        for (int64_t b = 0; b < nblk; b++) {
            int16_t blk[AFRAME_SAMPLES] = {0};
            int64_t off = b * AFRAME_SAMPLES;
            int n = (int)FFMIN(AFRAME_SAMPLES, s->nsamp - off);
            double k[8];
            if (n > 0) memcpy(blk, s->buf[c] + off, n * sizeof(int16_t));
            parcor_block(blk, AFRAME_SAMPLES, avctx->sample_rate, k);
            for (int j = 0; j < 8; j++)
                s->pcd[b][c][j] = qs16(k[j] * 32768.0);
        }

        for (int j = 0; j < 8; j++) ch->base[j] = s->pcd[0][c][j];

        {
            int64_t nd = nblk > 1 ? nblk - 1 : 0;
            for (int64_t t = 0; t < nd; t++)
                for (int j = 0; j < 8; j++)
                    d[t][j] = s->pcd[t + 1][c][j] - s->pcd[t][c][j];
            kmeans64(d, nd, ch->vq[0]);
            memset(ch->vq[0][0], 0, sizeof(ch->vq[0][0]));
            for (int64_t t = 0; t < nd; t++) {
                int i0 = vq_nearest(ch->vq[0], d[t]);
                for (int j = 0; j < 8; j++) d[t][j] -= ch->vq[0][i0][j];
            }
            kmeans64(d, nd, ch->vq[1]);
            memset(ch->vq[1][0], 0, sizeof(ch->vq[1][0]));
            for (int64_t t = 0; t < nd; t++) {
                int i1 = vq_nearest(ch->vq[1], d[t]);
                for (int j = 0; j < 8; j++) d[t][j] -= ch->vq[1][i1][j];
            }
            kmeans64(d, nd, ch->vq[2]);
            memset(ch->vq[2][0], 0, sizeof(ch->vq[2][0]));
        }
    }
    s->trained = 1;
end:
    av_free(d);
    return ret;
}

static void write_extradata_blob(VXAudioEncContext *s, uint8_t *p)
{
    for (int c = 0; c < s->nch; c++) {
        for (int i = 0; i < 3; i++)
            for (int v = 0; v < VQN; v++)
                for (int j = 0; j < 8; j++) { AV_WL16(p, s->ch[c].vq[i][v][j]); p += 2; }
        for (int j = 0; j < 8; j++) { AV_WL16(p, scale_lut[j]); p += 2; }
        for (int j = 0; j < 8; j++) { AV_WL32(p, s->ch[c].base[j]); p += 4; }
        AV_WL32(p, INIT_ACC); p += 4;
    }
}

/* ---------- per-frame encode (after training) ---------- */

/* Eight-tap prediction from the eight samples before the current one
 * (h[0] oldest .. h[7] newest).  Integer and exact, so the taps may be summed
 * in any order; the newest sample goes in last to keep it off the critical
 * path of the recursion. */
static av_always_inline int64_t vxa_predict(const int *h, const int *inf)
{
    uint64_t p = (uint64_t)((int64_t)h[0] * inf[7]) + (uint64_t)((int64_t)h[1] * inf[6]);
    uint64_t q = (uint64_t)((int64_t)h[2] * inf[5]) + (uint64_t)((int64_t)h[3] * inf[4]);
    uint64_t r = (uint64_t)((int64_t)h[4] * inf[3]) + (uint64_t)((int64_t)h[5] * inf[2]);

    p += q;
    r += (uint64_t)((int64_t)h[6] * inf[1]);
    return (int64_t)(p + r + (uint64_t)((int64_t)h[7] * inf[0]));
}

/* One lane of one sample of the closed-loop search: the recursion of
 * run_aframe() in choose mode. */
static av_always_inline void vxa_lane_step(VXAGroup *g, int32_t (*hist)[8 + AFRAME_SAMPLES],
                                           const int *inf, int l, int i, int t,
                                           int pulse, int place, int k)
{
    const int64_t P = vxa_predict(hist[l] + i, inf);
    int sample;

    if (place) {
        /* sample ~= baseline + val*scale + P>>14; solve for val, snap
         * to the odd grid {-7..7} (matches the standalone encoder) */
        int pr = (int)(P >> 14);
        int v = (int)lrint(((double)(t - pr - pulse) / g->scale[l] + 7.0) / 2.0);
        v = av_clip(v, 0, 7);
        g->pv[k][l] = v;
        pulse += (2 * v - 7) * g->scale[l];
    }

    sample = (int)(((int64_t)pulse * 0x4000 + P) >> 14);
    hist[l][8 + i] = sample;
    {
        double d = (double)sample - t;
        g->err[l] += d * d;
    }
}

/* Run a group of candidates of the closed-loop search side by side, on state
 * prepared once per block instead of once per candidate.  A candidate's error
 * only ever grows, so once every lane is worse than `bound` -- an error some
 * candidate has already achieved -- none of them can win and the group is
 * abandoned.  Lanes left with err > bound are to be ignored by the caller. */
static av_unused void vxa_search_group_c(const VXASearch *q, VXAGroup *g, double bound)
{
    int32_t hist[VXA_LANES][8 + AFRAME_SAMPLES];
    const int n = g->n;
    int next = g->pad, k = 0;

    for (int l = 0; l < n; l++) {
        for (int j = 0; j < 8; j++)
            hist[l][j] = q->ch->samples_prev[j];
        g->err[l] = 0;
    }

    for (int i = 0; i < AFRAME_SAMPLES; i++) {
        const int *inf = q->quarters[i >> 5];
        const int32_t *bl = g->baseline + i * VXA_BL_STRIDE;
        const int t = q->target[i];

        if (i == next) {
            for (int l = 0; l < n; l++)
                vxa_lane_step(g, hist, inf, l, i, t, bl[l], 1, k);
            next = ++k < NUM_PULSES ? i + PULSE_DISTANCE : -1;
        } else {
            for (int l = 0; l < n; l++)
                vxa_lane_step(g, hist, inf, l, i, t, bl[l], 0, 0);
        }

        if ((i & 7) == 7) {
            int alive = 0;
            for (int l = 0; l < n; l++)
                alive |= !(g->err[l] > bound);
            if (!alive)
                return;
        }
    }
}

#if ARCH_AARCH64 && HAVE_INTRINSICS_NEON
#include <arm_neon.h>

/* The same recursion, four lanes to a vector.  Every operation is the one the
 * scalar code compiles to on this architecture -- 32-bit wrapping integer
 * arithmetic, a 64-bit prediction sum, and the same divide, round and fused
 * multiply-add in double precision -- so the lanes come out bit for bit. */
static void vxa_search_group_neon(const VXASearch *q, VXAGroup *g, double bound)
{
    int32_t hist[8 + AFRAME_SAMPLES][VXA_LANES];
    float64x2_t err[VXA_LANES / 2], scale_d[VXA_LANES / 2];
    int32x4_t scale[VXA_LANES / 4];
    const float64x2_t seven_d = vdupq_n_f64(7.0), half_d = vdupq_n_f64(0.5);
    const float64x2_t bound_d = vdupq_n_f64(bound);
    const int32x4_t seven = vdupq_n_s32(7), zero = vdupq_n_s32(0);
    int next = g->pad, k = 0;

    for (int j = 0; j < 8; j++)
        for (int l = 0; l < VXA_LANES; l++)
            hist[j][l] = q->ch->samples_prev[j];

    for (int v = 0; v < VXA_LANES / 4; v++) {
        scale[v] = vld1q_s32(g->scale + 4 * v);
        scale_d[2 * v]     = vcvtq_f64_s64(vmovl_s32(vget_low_s32(scale[v])));
        scale_d[2 * v + 1] = vcvtq_f64_s64(vmovl_high_s32(scale[v]));
    }
    /* unused lanes start out, and so stay, beyond any bound */
    for (int l = 0; l < VXA_LANES; l += 2) {
        const double e[2] = { l < g->n ? 0 : INFINITY, l + 1 < g->n ? 0 : INFINITY };
        err[l / 2] = vld1q_f64(e);
    }

    for (int i = 0; i < AFRAME_SAMPLES; i++) {
        const int *inf = q->quarters[i >> 5];
        const int32_t *bl = g->baseline + i * VXA_BL_STRIDE;
        const int32x4_t t = vdupq_n_s32(q->target[i]);
        const float64x2_t t_d = vdupq_n_f64(q->target[i]);
        const int place = i == next;
        int32x4_t c[8];

        /* tap j multiplies the sample j + 1 back, which is row i + 7 - j */
        for (int j = 0; j < 8; j++)
            c[j] = vdupq_n_s32(inf[j]);

        for (int v = 0; v < VXA_LANES / 4; v++) {
            int64x2_t lo = vdupq_n_s64(0), hi = vdupq_n_s64(0);
            int32x4_t pulse = vld1q_s32(bl + 4 * v), sample;
            float64x2_t d0, d1;

            for (int j = 7; j >= 0; j--) {
                const int32x4_t x = vld1q_s32(hist[i + 7 - j] + 4 * v);
                lo = vmlal_s32(lo, vget_low_s32(x), vget_low_s32(c[j]));
                hi = vmlal_high_s32(hi, x, c[j]);
            }

            if (place) {
                const int32x4_t pr  = vcombine_s32(vshrn_n_s64(lo, 14), vshrn_n_s64(hi, 14));
                const int32x4_t num = vsubq_s32(vsubq_s32(t, pr), pulse);
                float64x2_t n0 = vcvtq_f64_s64(vmovl_s32(vget_low_s32(num)));
                float64x2_t n1 = vcvtq_f64_s64(vmovl_high_s32(num));
                int32x4_t val;

                n0 = vrndxq_f64(vmulq_f64(vaddq_f64(vdivq_f64(n0, scale_d[2 * v]),     seven_d), half_d));
                n1 = vrndxq_f64(vmulq_f64(vaddq_f64(vdivq_f64(n1, scale_d[2 * v + 1]), seven_d), half_d));
                val = vcombine_s32(vmovn_s64(vcvtq_s64_f64(n0)), vmovn_s64(vcvtq_s64_f64(n1)));
                val = vminq_s32(vmaxq_s32(val, zero), seven);
                vst1q_s32(g->pv[k] + 4 * v, val);
                pulse = vmlaq_s32(pulse, vsubq_s32(vshlq_n_s32(val, 1), seven), scale[v]);
            }

            sample = vcombine_s32(vshrn_n_s64(vaddq_s64(lo, vshll_n_s32(vget_low_s32(pulse), 14)), 14),
                                  vshrn_n_s64(vaddq_s64(hi, vshll_high_n_s32(pulse, 14)), 14));
            vst1q_s32(hist[8 + i] + 4 * v, sample);

            d0 = vsubq_f64(vcvtq_f64_s64(vmovl_s32(vget_low_s32(sample))), t_d);
            d1 = vsubq_f64(vcvtq_f64_s64(vmovl_high_s32(sample)), t_d);
            err[2 * v]     = vfmaq_f64(err[2 * v],     d0, d0);
            err[2 * v + 1] = vfmaq_f64(err[2 * v + 1], d1, d1);
        }

        if (place)
            next = ++k < NUM_PULSES ? i + PULSE_DISTANCE : -1;

        if ((i & 7) == 7) {
            uint64x2_t dead = vcgtq_f64(err[0], bound_d);
            for (int l = 1; l < VXA_LANES / 2; l++)
                dead = vandq_u64(dead, vcgtq_f64(err[l], bound_d));
            if (vgetq_lane_u64(dead, 0) & vgetq_lane_u64(dead, 1))
                break;
        }
    }

    for (int l = 0; l < VXA_LANES / 2; l++)
        vst1q_f64(g->err + 2 * l, err[l]);
}
#define vxa_search_group vxa_search_group_neon
#else
#define vxa_search_group vxa_search_group_c
#endif

/* Evaluate the prepared group and fold any improvement into *best. */
static void vxa_run_group(VXASearch *q, VXAGroup *g, VXAResult *best)
{
    /* errors are never negative, so their bit patterns order the same way the
     * values do */
    uint64_t shared = atomic_load_explicit(&q->bound, memory_order_relaxed);
    double bound = av_int2double(shared);

    if (best->err >= 0 && best->err < bound)
        bound = best->err;
    vxa_search_group(q, g, bound);

    for (int l = 0; l < g->n; l++) {
        const double err = g->err[l];
        uint64_t bits;

        if (err > bound)
            continue;
        if (!(best->err < 0 || err < best->err ||
              (err == best->err && g->cand[l] < best->cand)))
            continue;
        best->err  = err;
        best->cand = g->cand[l];
        for (int k = 0; k < NUM_PULSES; k++)
            best->pv[k] = g->pv[k][l];
        bits = av_double2int(err);
        while (bits < shared &&
               !atomic_compare_exchange_weak_explicit(&q->bound, &shared, bits,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
            ;
    }
}

/* The lag-free candidates (pfo is 0x7e, or 0x7f for an intra frame): every
 * usable gain at every grid phase.  smi_err receives the smallest error seen
 * for each gain. */
static void vxa_search_base(VXASearch *q, int pfo, VXAResult *best, double *smi_err)
{
    VXAGroup g;

    if (!q->nb_smi)
        return;
    g.baseline = &q->baseline[0][0x7e];
    g.n        = q->nb_smi;
    for (int l = 0; l < VXA_LANES; l++)
        g.scale[l] = q->scale[q->smi_order[l < g.n ? l : 0]];

    for (int pad = 0; pad < VXA_NUM_PAD; pad++) {
        g.pad = pad;
        for (int l = 0; l < g.n; l++)
            g.cand[l] = VXA_CAND(pfo, q->smi_order[l], pad);
        vxa_run_group(q, &g, best);
        for (int l = 0; l < g.n; l++) {
            const int smi = q->smi_order[l];
            if (smi_err[smi] < 0 || g.err[l] < smi_err[smi])
                smi_err[smi] = g.err[l];
        }
    }
}

/* Lags 0..0x7d for one gain and one grid phase. */
static int vxa_search_job(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    VXAudioEncContext *s = avctx->priv_data;
    VXASearch *q = &s->search[jobnr / VXA_JOBS];
    const int si = jobnr % VXA_JOBS / VXA_NUM_PAD;
    VXAGroup g;
    int smi;

    if (!q->lagged || si >= q->nb_smi)
        return 0;
    smi = q->smi_order[si];

    g.pad = jobnr % VXA_NUM_PAD;
    for (int l = 0; l < VXA_LANES; l++)
        g.scale[l] = q->scale[smi];
    for (int pfo = 0; pfo < 0x7e; pfo += VXA_LANES) {
        g.baseline = &q->baseline[0][pfo];
        g.n        = FFMIN(VXA_LANES, 0x7e - pfo);
        for (int l = 0; l < g.n; l++)
            g.cand[l] = VXA_CAND(pfo + l, smi, g.pad);
        vxa_run_group(q, &g, &q->res[threadnr]);
    }
    return 0;
}

/* Per-block set-up for channel c: quantize the filter update into idx[], then
 * run the part of the search that is not worth farming out.  Returns the
 * forced-intra flag. */
static int encode_block_prepare(VXAudioEncContext *s, int c, int64_t bi,
                                int idx[3])
{
    VXAChan *ch = &s->ch[c];
    VXASearch *q = &s->search[c];
    int64_t off = bi * AFRAME_SAMPLES;
    int n = (int)FFMIN(AFRAME_SAMPLES, s->nsamp - off);
    int intra = !ch->have_prev || (s->kf_n && vxa_is_kf_period(s, bi));

    idx[0] = idx[1] = idx[2] = 0;
    memset(q->target, 0, sizeof(q->target));
    if (n > 0) memcpy(q->target, s->buf[c] + off, n * sizeof(int16_t));

    /* filter update: 3-stage differential VQ vs the committed running filter */
    {
        const int32_t *ref = intra ? ch->base : (const int32_t[8]){
            ch->lpc_filter_prev[0], ch->lpc_filter_prev[1], ch->lpc_filter_prev[2],
            ch->lpc_filter_prev[3], ch->lpc_filter_prev[4], ch->lpc_filter_prev[5],
            ch->lpc_filter_prev[6], ch->lpc_filter_prev[7] };
        double t[8];
        for (int j = 0; j < 8; j++) t[j] = (double)s->pcd[bi][c][j] - ref[j];
        idx[0] = vq_nearest(ch->vq[0], t);
        for (int j = 0; j < 8; j++) t[j] -= ch->vq[0][idx[0]][j];
        idx[1] = vq_nearest(ch->vq[1], t);
        for (int j = 0; j < 8; j++) t[j] -= ch->vq[1][idx[1]][j];
        idx[2] = vq_nearest(ch->vq[2], t);

        /* stability fallback: drop stages until |k| stays well inside 1 */
        for (int tries = 0; tries < 3; tries++) {
            int bad = 0;
            for (int j = 0; j < 8; j++) {
                long v = (long)ref[j] + ch->vq[0][idx[0]][j] + ch->vq[1][idx[1]][j] + ch->vq[2][idx[2]][j];
                if (v > 31000 || v < -31000) { bad = 1; break; }
            }
            if (!bad) break;
            if (tries == 0) idx[2] = 0;
            else if (tries == 1) idx[1] = 0;
            else idx[0] = 0;
        }
    }

    /* candidate-independent decoder state, exactly as run_aframe() derives it */
    {
        int lpc[8], influence[8];

        for (int k = 0; k < 8; k++)
            lpc[k] = (intra ? ch->base[k] : ch->lpc_filter_prev[k]) +
                     ch->vq[0][idx[0]][k] + ch->vq[1][idx[1]][k] + ch->vq[2][idx[2]][k];
        compute_influence(lpc, influence);
        make_quarters(ch, intra, influence, q->quarters);

        q->nb_smi = 0;
        for (int smi = 0; smi < VXA_NUM_SMI; smi++) {
            int base_scale = intra ? (int)INIT_ACC : ch->scale_prev;
            q->scale[smi] = (base_scale * (int)scale_lut[smi]) >> 13;
            if (q->scale[smi] >= 1)      /* the decoder can't recover from 0 */
                q->smi_order[q->nb_smi++] = smi;
        }
    }

    q->ch = ch;
    atomic_store_explicit(&q->bound, av_double2int(INFINITY), memory_order_relaxed);
    for (int j = 0; j < q->nb_res; j++)
        q->res[j].err = -1;

    /* The lag-free candidates come first: there are few of them, and the
     * error they reach lets whole groups of lagged candidates be dropped
     * early -- soonest for the gains that fit worst, so those go last. */
    {
        int pfo = intra ? 0x7f : 0x7e;
        double smi_err[VXA_NUM_SMI];

        for (int smi = 0; smi < VXA_NUM_SMI; smi++)
            smi_err[smi] = -1;
        vxa_search_base(q, pfo, &q->res[0], smi_err);

        for (int a = 1; a < q->nb_smi; a++) {
            int smi = q->smi_order[a], b = a;
            while (b > 0 && smi_err[q->smi_order[b - 1]] > smi_err[smi]) {
                q->smi_order[b] = q->smi_order[b - 1];
                b--;
            }
            q->smi_order[b] = smi;
        }
    }

    /* joint closed-loop search: prev_frame_offset (LTP lag) x gain x phase */
    if (!intra && s->ltp && ch->have_prev) {
        /* excitation baselines the decoder builds before adding grid pulses */
        int concat[AFRAME_SAMPLES * 2];

        for (int i = 0; i < AFRAME_SAMPLES; i++) {
            concat[i]                  = ch->have_prev2 ? ch->pulses_prev2[i] : 0;
            concat[AFRAME_SAMPLES + i] = ch->pulses_prev[i];
        }
        for (int i = 0; i < AFRAME_SAMPLES; i++) {
            int volume = FFMIN3(8, i + 1, AFRAME_SAMPLES - i);
            for (int pfo = 0; pfo < 0x7e; pfo++)
                q->baseline[i][pfo] = (concat[i + 0x7f - pfo] * volume) >> 4;
        }

        q->lagged = 1;
    } else {
        q->lagged = 0;
    }
    return intra;
}

/* Pick the winner, commit it with the exact decoder math and pack the frame. */
static void encode_block_finish(VXAudioEncContext *s, int c, int intra,
                                const int idx[3], uint8_t *out20)
{
    VXAChan *ch = &s->ch[c];
    VXASearch *q = &s->search[c];
    const VXAResult *best = NULL;
    int best_pfo = intra ? 0x7f : 0x7e, best_smi = 7, best_pad = 0;
    int best_pv[NUM_PULSES] = {0};

    for (int j = 0; j < q->nb_res; j++) {
        const VXAResult *r = &q->res[j];
        if (r->err < 0)
            continue;
        if (!best || r->err < best->err ||
            (r->err == best->err && r->cand < best->cand))
            best = r;
    }

    if (best) {
        best_pad = best->cand % VXA_NUM_PAD;
        best_smi = best->cand / VXA_NUM_PAD % VXA_NUM_SMI;
        best_pfo = best->cand / (VXA_NUM_PAD * VXA_NUM_SMI);
        for (int i = 0; i < NUM_PULSES; i++)
            best_pv[i] = best->pv[i];
    } else { /* every gain collapsed; force the largest */
        run_aframe(ch, best_pfo, best_smi, idx, best_pad, INIT_ACC,
                   q->target, best_pv, NULL, 0);
    }

    /* commit with the exact decoder math */
    run_aframe(ch, best_pfo, best_smi, idx, best_pad, INIT_ACC,
               q->target, NULL, best_pv, 1);

    /* pack: header words + 8 pulse words (mode 0), little-endian on disk */
    {
        uint16_t w[10];
        w[0] = (uint16_t)((best_pfo << 9) | (best_smi << 6) | idx[0]);
        w[1] = (uint16_t)((best_pad << 14) | (0 << 12) | (idx[1] << 6) | idx[2]);
        for (int i = 0; i < 8; i++) {
            uint16_t v = 0;
            for (int j = 0; j < 5; j++)
                v |= (uint16_t)(best_pv[5*i + j] & 7) << (16 - 3 - 3*j);
            w[2 + i] = v;
        }
        w[2] |= (best_pv[40] >> 2) & 1;
        w[3] |= (best_pv[40] >> 1) & 1;
        w[4] |= (best_pv[40] >> 0) & 1;
        w[5] |= (best_pv[41] >> 2) & 1;
        w[6] |= (best_pv[41] >> 1) & 1;
        w[7] |= (best_pv[41] >> 0) & 1;
        for (int i = 0; i < 10; i++)
            AV_WL16(out20 + 2*i, w[i]);
    }
}

/* Encode one 128-sample period of every channel into out (20 bytes each). */
static void encode_block(AVCodecContext *avctx, int64_t bi, uint8_t *out)
{
    VXAudioEncContext *s = avctx->priv_data;
    int idx[MAX_CH][3], intra[MAX_CH], lagged = 0;

    for (int c = 0; c < s->nch; c++) {
        intra[c] = encode_block_prepare(s, c, bi, idx[c]);
        lagged  |= s->search[c].lagged;
    }

    /* The channels are independent within a period, so their lag searches
     * share one batch of jobs. */
    if (lagged)
        avctx->execute2(avctx, vxa_search_job, NULL, NULL, s->nch * VXA_JOBS);

    for (int c = 0; c < s->nch; c++)
        encode_block_finish(s, c, intra[c], idx[c], out + 20 * c);
}

/* ---------- lifecycle ---------- */

static int cmp_int64(const int64_t *a, const int64_t *b)
{
    return FFDIFFSIGN(*a, *b);
}

static av_cold int vxa_init(AVCodecContext *avctx)
{
    VXAudioEncContext *s = avctx->priv_data;

    s->nch = avctx->ch_layout.nb_channels;
    if (s->nch < 1 || s->nch > MAX_CH) {
        av_log(avctx, AV_LOG_ERROR, "vx audio supports mono/stereo only\n");
        return AVERROR(EINVAL);
    }
    avctx->frame_size = AFRAME_SAMPLES;

    s->search = av_calloc(s->nch, sizeof(*s->search));
    if (!s->search)
        return AVERROR(ENOMEM);
    for (int c = 0; c < s->nch; c++) {
        VXASearch *q = &s->search[c];

        q->nb_res = (avctx->active_thread_type & FF_THREAD_SLICE) ?
                    FFMAX(avctx->thread_count, 1) : 1;
        q->res    = av_calloc(q->nb_res, sizeof(*q->res));
        if (!q->res)
            return AVERROR(ENOMEM);
    }

    /* placeholder extradata (real trained codebook follows as side data) */
    avctx->extradata = av_mallocz(s->nch * VXA_EXTRADATA_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = s->nch * VXA_EXTRADATA_SIZE;

    /* parse the forced-intra period list (CSV) into a sorted array */
    if (s->intra_periods_str && *s->intra_periods_str) {
        const char *p = s->intra_periods_str;
        int cap = 16;
        s->kf_periods = av_malloc_array(cap, sizeof(*s->kf_periods));
        if (!s->kf_periods) return AVERROR(ENOMEM);
        while (*p) {
            char *end;
            long long v = strtoll(p, &end, 10);
            if (end == p) break;
            if (s->kf_n >= cap) {
                int64_t *n = av_realloc_array(s->kf_periods, cap * 2, sizeof(*n));
                if (!n) return AVERROR(ENOMEM);
                s->kf_periods = n; cap *= 2;
            }
            s->kf_periods[s->kf_n++] = v;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        /* vxa_is_kf_period() binary-searches this list */
        AV_QSORT(s->kf_periods, s->kf_n, int64_t, cmp_int64);
    }

    return 0;
}

/* binary search: is `bi` one of the forced-intra keyframe periods? */
static int vxa_is_kf_period(const VXAudioEncContext *s, int64_t bi)
{
    int lo = 0, hi = s->kf_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (s->kf_periods[mid] == bi) return 1;
        if (s->kf_periods[mid] < bi)  lo = mid + 1;
        else                          hi = mid - 1;
    }
    return 0;
}

static int vxa_encode(AVCodecContext *avctx, AVPacket *pkt,
                      const AVFrame *frame, int *got_packet)
{
    VXAudioEncContext *s = avctx->priv_data;
    int ret;

    *got_packet = 0;

    if (frame) {                          /* buffering pass */
        int64_t need = s->nsamp + frame->nb_samples;
        if (need > s->bufcap) {
            int64_t ncap = FFMAX(need, s->bufcap ? s->bufcap * 2 : (int64_t)1 << 18);
            for (int c = 0; c < s->nch; c++) {
                int16_t *nb = av_realloc(s->buf[c], ncap * sizeof(int16_t));
                if (!nb)
                    return AVERROR(ENOMEM);
                s->buf[c] = nb;
            }
            s->bufcap = ncap;
        }
        {
            const int16_t *in = (const int16_t *)frame->data[0];
            for (int i = 0; i < frame->nb_samples; i++)
                for (int c = 0; c < s->nch; c++)
                    s->buf[c][s->nsamp + i] = in[i * s->nch + c];
        }
        s->nsamp += frame->nb_samples;
        return 0;
    }

    /* flush: train once, then emit one packet per 128-sample period */
    if (!s->nsamp)
        return 0;
    if (!s->trained) {
        if ((ret = train(avctx)) < 0)
            return ret;
        write_extradata_blob(s, avctx->extradata);
        av_log(avctx, AV_LOG_INFO,
               "vx audio: trained %d codebook set(s) over %"PRId64" frames\n",
               s->nch, s->nblk);
    }
    if (s->blk_pos >= s->nblk)
        return 0;

    if ((ret = ff_get_encode_buffer(avctx, pkt, 20 * s->nch, 0)) < 0)
        return ret;

    encode_block(avctx, s->blk_pos, pkt->data);

    /* The muxer buffers the whole file (the codebook is trained over the entire
     * stream), so ffmpeg's size/time counters read 0 during the encode. This
     * per-block analysis-by-synthesis pass is the slow, otherwise-silent phase
     * (minutes without -ltp 0), so emit our own progress every ~2%. */
    if (s->nblk >= 50) {
        int step = (int)(s->nblk / 50);
        if (s->blk_pos % step == 0)
            av_log(avctx, AV_LOG_INFO, "vx audio: encoding %d%% (block %"PRId64"/%"PRId64")\r",
                   (int)(100 * s->blk_pos / s->nblk), s->blk_pos, s->nblk);
    }

    if (!s->sent_extradata) {
        uint8_t *sd = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                              s->nch * VXA_EXTRADATA_SIZE);
        if (sd)
            write_extradata_blob(s, sd);
        s->sent_extradata = 1;
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    pkt->pts = pkt->dts = s->blk_pos * AFRAME_SAMPLES;
    pkt->duration = AFRAME_SAMPLES;
    s->blk_pos++;
    *got_packet = 1;
    return 0;
}

static av_cold int vxa_close(AVCodecContext *avctx)
{
    VXAudioEncContext *s = avctx->priv_data;
    for (int c = 0; c < MAX_CH; c++)
        av_freep(&s->buf[c]);
    av_freep(&s->pcd);
    av_freep(&s->kf_periods);
    if (s->search)
        for (int c = 0; c < s->nch; c++)
            av_freep(&s->search[c].res);
    av_freep(&s->search);
    return 0;
}

#define OFFSET(x) offsetof(VXAudioEncContext, x)
#define AE AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption vxa_options[] = {
    { "ltp", "search long-term-prediction lags (slower, better quality)",
      OFFSET(ltp), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, AE },
    { "intra_periods", "CSV of 128-sample period indices to force intra (mods "
      "keyframe audio re-prime); set by encode.py, not for manual use",
      OFFSET(intra_periods_str), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AE },
    { NULL },
};

static const AVClass vxa_class = {
    .class_name = "vx_audio encoder",
    .item_name  = av_default_item_name,
    .option     = vxa_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_vx_audio_encoder = {
    .p.name         = "vx_audio",
    CODEC_LONG_NAME("ActImagine VX Audio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_VX_AUDIO,
    .priv_data_size = sizeof(VXAudioEncContext),
    .p.priv_class   = &vxa_class,
    .init           = vxa_init,
    FF_CODEC_ENCODE_CB(vxa_encode),
    .close          = vxa_close,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
                      AV_CODEC_CAP_SMALL_LAST_FRAME |
                      AV_CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
