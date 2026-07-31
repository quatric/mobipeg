/*
 * HVQM4 video encoder (intra-only)
 * Copyright (c) 2026 quatric
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

/*
 * Targets HVQM4 1.5's I-picture format (see h4m_audio_decode.c for the
 * reference decoder this must round-trip against bit-exactly). No P/B
 * frames yet: every output frame is an I-frame.
 *
 * Per 4x4 luma/chroma block, the decoder reconstructs pixels from one of:
 *   type 1..5  DC + N "AOT" basis terms, each a scaled 4x4 patch pulled
 *              from a shared "nest" (a per-frame down-sampled DC map of
 *              this same frame, built once and reused for every block)
 *   type 6     literal 4x4 pixel block (escape)
 *   type 8     flat DC fill
 *   type 0     smooth gradient fill from neighboring blocks' DC (unused
 *              by this encoder for now - always representable via 6/8)
 * All of these share one already-decoded 8-bit DC value per block, so
 * the encoder always gets the block mean exactly right regardless of how
 * well the AOT search approximates the residual texture.
 */

#include <string.h>

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"

#define PLANE_COUNT   3
#define LUMA_IDX      0
#define CHROMA_IDX    1  /* index into the 2-wide luma/chroma-shared arrays */

#define BLK_ORG   6
#define BLK_DC    8

#define H_NEST_LANDSCAPE 70
#define V_NEST_LANDSCAPE 38

#define MAX_BASES 5

/* IntraAotBlock() reconstructs each pixel as (result[y][x]+delta)>>unk_shift,
 * and result[][] accumulates factor*basis where factor=(sum+off)*inverse
 * with inverse up to divTable[1]==0x1000==2^12; unk_shift must therefore be
 * large enough to bring that back down to pixel scale. 12 matches the
 * table's own fixed-point base exactly. */
#define HVQ_UNK_SHIFT 12

#ifdef HVQM4ENC_DEBUG_STATS
int hvqm4_dbg_dc[3], hvqm4_dbg_aot[3], hvqm4_dbg_org[3];
#endif

/* ------------------------------------------------------------------- */
/* Bit-exact match for h4m_audio_decode.c's getBit(): the decoder reads
 * 4-byte big-endian words and consumes bits MSB (bit 31) to LSB (bit 0),
 * which end to end over a byte stream is a plain MSB-first bit order. A
 * standard byte-oriented MSB-first writer therefore round-trips exactly,
 * as long as each independently addressed sub-stream has a few bytes of
 * trailing slack for the decoder's word-at-a-time overread. */

typedef struct HVQBitWriter {
    uint8_t *buf;
    int size;
    int cap;
    uint32_t acc;
    int nbits;
} HVQBitWriter;

static av_cold void bw_init(HVQBitWriter *w)
{
    memset(w, 0, sizeof(*w));
}

static av_cold void bw_free(HVQBitWriter *w)
{
    av_freep(&w->buf);
}

static int bw_reserve(HVQBitWriter *w, int extra)
{
    if (w->size + extra > w->cap) {
        int newcap = FFMAX(w->cap * 2, w->size + extra + 256);
        uint8_t *n = av_realloc(w->buf, newcap);
        if (!n)
            return AVERROR(ENOMEM);
        w->buf = n;
        w->cap = newcap;
    }
    return 0;
}

static void bw_put_bit(HVQBitWriter *w, int bit)
{
    w->acc = (w->acc << 1) | (bit & 1);
    if (++w->nbits == 8) {
        bw_reserve(w, 1);
        w->buf[w->size++] = (uint8_t)w->acc;
        w->acc = 0;
        w->nbits = 0;
    }
}

static void bw_put_bits(HVQBitWriter *w, int n, uint32_t val)
{
    while (n-- > 0)
        bw_put_bit(w, (val >> n) & 1);
}

static void bw_put_byte_raw(HVQBitWriter *w, uint8_t byte)
{
    /* used only for fixvl streams, which the decoder never bit-packs
     * (GetAotBasis()/OrgBlock() advance buf->ptr directly) */
    bw_reserve(w, 1);
    w->buf[w->size++] = byte;
}

static void bw_flush(HVQBitWriter *w)
{
    if (w->nbits) {
        w->acc <<= (8 - w->nbits);
        bw_reserve(w, 1);
        w->buf[w->size++] = (uint8_t)w->acc;
        w->acc = 0;
        w->nbits = 0;
    }
    /* the decoder always reads 4-byte words; pad so a trailing word never
     * runs past this stream's own buffer */
    bw_reserve(w, 4);
    while (w->size % 4)
        w->buf[w->size++] = 0;
}

/* ------------------------------------------------------------------- */
/* Huffman: build a tree from a symbol histogram (byte alphabet, 0..255)
 * and serialize it in the exact recursive format _readTree() expects:
 * leaf = 0-bit + 8-bit byte; branch = 1-bit + left + right. */

typedef struct HuffNode {
    int freq;
    int sym;         /* leaf symbol, -1 for internal nodes */
    int left, right; /* indices into the node pool, -1 if none */
} HuffNode;

typedef struct HuffCode {
    uint32_t bits;
    int len;
} HuffCode;

typedef struct HVQHuff {
    HuffNode nodes[511]; /* <=256 leaves -> <=255 internal nodes */
    int nb_nodes;
    int root;
    int empty; /* no symbols at all: emit a zero-length stream */
    HuffCode codes[256];
} HVQHuff;

static int huff_new_node(HVQHuff *h, int freq, int sym, int l, int r)
{
    HuffNode *n = &h->nodes[h->nb_nodes];
    n->freq = freq;
    n->sym = sym;
    n->left = l;
    n->right = r;
    return h->nb_nodes++;
}

static void huff_assign_codes(HVQHuff *h, int node, uint32_t bits, int len)
{
    HuffNode *n = &h->nodes[node];
    if (n->sym >= 0) {
        h->codes[n->sym].bits = bits;
        h->codes[n->sym].len  = len;
        return;
    }
    huff_assign_codes(h, n->left,  (bits << 1) | 0, len + 1);
    huff_assign_codes(h, n->right, (bits << 1) | 1, len + 1);
}

static void huff_build(HVQHuff *h, const int *histogram)
{
    int active[257];
    int nb_active = 0;
    int i;

    memset(h, 0, sizeof(*h));

    for (i = 0; i < 256; i++)
        if (histogram[i] > 0)
            active[nb_active++] = huff_new_node(h, histogram[i], i, -1, -1);

    if (nb_active == 0) {
        h->empty = 1;
        return;
    }
    if (nb_active == 1) {
        h->root = active[0];
        huff_assign_codes(h, h->root, 0, 0);
        return;
    }

    while (nb_active > 1) {
        int a = 0, b = 1, ia, ib, tmp;
        if (h->nodes[active[b]].freq < h->nodes[active[a]].freq) { tmp = a; a = b; b = tmp; }
        for (i = 2; i < nb_active; i++) {
            int f = h->nodes[active[i]].freq;
            if (f < h->nodes[active[a]].freq) { b = a; a = i; }
            else if (f < h->nodes[active[b]].freq) { b = i; }
        }
        ia = active[a];
        ib = active[b];
        {
            int merged = huff_new_node(h, h->nodes[ia].freq + h->nodes[ib].freq, -1, ia, ib);
            if (a < b) { active[b] = active[nb_active - 1]; active[a] = active[nb_active - 2]; }
            else       { active[a] = active[nb_active - 1]; active[b] = active[nb_active - 2]; }
            nb_active -= 2;
            active[nb_active++] = merged;
        }
    }
    h->root = active[0];
    huff_assign_codes(h, h->root, 0, 0);
}

static void huff_serialize(HVQBitWriter *w, HVQHuff *h, int node)
{
    HuffNode *n = &h->nodes[node];
    if (n->sym >= 0) {
        bw_put_bit(w, 0);
        bw_put_bits(w, 8, (uint32_t)n->sym);
    } else {
        bw_put_bit(w, 1);
        huff_serialize(w, h, n->left);
        huff_serialize(w, h, n->right);
    }
}

static void huff_emit(HVQBitWriter *w, HVQHuff *h, int sym)
{
    bw_put_bits(w, h->codes[sym].len, h->codes[sym].bits);
}

/* ------------------------------------------------------------------- */
/* decodeSOvfSym()/decodeUOvfSym() overflow-chained symbol coding: encode
 * a target sum as a chain of "overflow" symbols (one short of min/max)
 * plus one terminating in-range symbol, mirroring the decoder's
 * accumulate-until-in-range loop. */

static void plan_signed_ovf(int value, int min, int max, int *hist)
{
    while (value >= max || value <= min) {
        int step = value > 0 ? max - 1 : min + 1;
        hist[(uint8_t)step]++;
        value -= step;
    }
    hist[(uint8_t)value]++;
}

static void emit_signed_ovf(HVQBitWriter *w, HVQHuff *h, int value, int min, int max)
{
    while (value >= max || value <= min) {
        int step = value > 0 ? max - 1 : min + 1;
        huff_emit(w, h, (uint8_t)step);
        value -= step;
    }
    huff_emit(w, h, (uint8_t)value);
}

static void plan_unsigned_ovf(int value, int max, int *hist)
{
    while (value >= max) {
        hist[(uint8_t)(max - 1)]++;
        value -= (max - 1);
    }
    hist[(uint8_t)value]++;
}

static void emit_unsigned_ovf(HVQBitWriter *w, HVQHuff *h, int value, int max)
{
    while (value >= max) {
        huff_emit(w, h, (uint8_t)(max - 1));
        value -= (max - 1);
    }
    huff_emit(w, h, (uint8_t)value);
}

/* ------------------------------------------------------------------- */

typedef struct HVQPlane {
    int w, h;
    int h_blocks, v_blocks;
    int h_blocks_safe, v_blocks_safe;
    uint8_t *dc; /* (h_blocks_safe * v_blocks_safe), border = 0x7F */
} HVQPlane;

/* one decided block: how the encoder will represent it */
typedef struct BlockDecision {
    uint8_t type;          /* 1..MAX_BASES (AOT terms), BLK_ORG, or BLK_DC */
    uint8_t nb_terms;
    uint16_t term_bits[MAX_BASES];  /* packed nest offset/stride/sign/off2 */
    uint8_t term_sym[MAX_BASES];    /* bufTree0 huffman symbol (raw byte, <<2 on decode) */
    uint8_t org[4][4];              /* literal pixels, valid iff type == BLK_ORG */
} BlockDecision;

typedef struct HVQM4EncContext {
    AVClass *class;

    int width, height;
    int h_samp, v_samp;

    HVQPlane planes[PLANE_COUNT];
    BlockDecision *decisions[PLANE_COUNT]; /* h_blocks * v_blocks, raster order */

    uint8_t nest[H_NEST_LANDSCAPE * V_NEST_LANDSCAPE];
    int h_nest_size, v_nest_size;
    int is_landscape;

    int max_bases;
    int search_step;
    int org_threshold; /* mean squared error above which we fall back to a literal block */

    uint32_t disp_id;
} HVQM4EncContext;

static void plane_alloc_dims(HVQPlane *p, int pw, int ph, int hs, int vs)
{
    p->w = pw / hs;
    p->h = ph / vs;
    p->h_blocks = p->w / 4;
    p->v_blocks = p->h / 4;
    p->h_blocks_safe = p->h_blocks + 2;
    p->v_blocks_safe = p->v_blocks + 2;
}

static av_cold int hvqm4_encode_init(AVCodecContext *avctx)
{
    HVQM4EncContext *s = avctx->priv_data;
    int i;

    if (avctx->width % 8 || avctx->height % 8) {
        av_log(avctx, AV_LOG_ERROR, "hvqm4: width/height must be multiples of 8\n");
        return AVERROR(EINVAL);
    }
#ifdef HVQM4ENC_DEBUG_STATS
    av_log(avctx, AV_LOG_WARNING, "hvqm4 opts: max_bases=%d search_step=%d org_threshold=%d\n",
           s->max_bases, s->search_step, s->org_threshold);
#endif

    s->width  = avctx->width;
    s->height = avctx->height;
    s->h_samp = 2;
    s->v_samp = 2;

    s->is_landscape = s->width >= s->height;
    if (s->is_landscape) {
        s->h_nest_size = H_NEST_LANDSCAPE;
        s->v_nest_size = V_NEST_LANDSCAPE;
    } else {
        s->h_nest_size = V_NEST_LANDSCAPE;
        s->v_nest_size = H_NEST_LANDSCAPE;
    }

    plane_alloc_dims(&s->planes[0], s->width, s->height, 1, 1);
    plane_alloc_dims(&s->planes[1], s->width, s->height, s->h_samp, s->v_samp);
    plane_alloc_dims(&s->planes[2], s->width, s->height, s->h_samp, s->v_samp);

    for (i = 0; i < PLANE_COUNT; i++) {
        HVQPlane *p = &s->planes[i];
        size_t n = (size_t)p->h_blocks_safe * p->v_blocks_safe;
        p->dc = av_malloc(n);
        s->decisions[i] = av_calloc((size_t)p->h_blocks * p->v_blocks, sizeof(BlockDecision));
        if (!p->dc || !s->decisions[i])
            return AVERROR(ENOMEM);
    }

    avctx->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = 2;
    avctx->extradata[0] = s->h_samp;
    avctx->extradata[1] = s->v_samp;

#ifdef HVQM4ENC_DEBUG_STATS
    for (i = 0; i < PLANE_COUNT; i++)
        av_log(avctx, AV_LOG_WARNING, "plane%d: w=%d h=%d h_blocks=%d v_blocks=%d h_blocks_safe=%d v_blocks_safe=%d\n",
               i, s->planes[i].w, s->planes[i].h, s->planes[i].h_blocks, s->planes[i].v_blocks,
               s->planes[i].h_blocks_safe, s->planes[i].v_blocks_safe);
#endif

    return 0;
}

static av_cold int hvqm4_encode_close(AVCodecContext *avctx)
{
    HVQM4EncContext *s = avctx->priv_data;
    int i;
#ifdef HVQM4ENC_DEBUG_STATS
    av_log(avctx, AV_LOG_WARNING, "hvqm4 stats: Y dc=%d aot=%d org=%d | U dc=%d aot=%d org=%d | V dc=%d aot=%d org=%d\n",
           hvqm4_dbg_dc[0], hvqm4_dbg_aot[0], hvqm4_dbg_org[0],
           hvqm4_dbg_dc[1], hvqm4_dbg_aot[1], hvqm4_dbg_org[1],
           hvqm4_dbg_dc[2], hvqm4_dbg_aot[2], hvqm4_dbg_org[2]);
#endif
    for (i = 0; i < PLANE_COUNT; i++) {
        av_freep(&s->planes[i].dc);
        av_freep(&s->decisions[i]);
    }
    return 0;
}

static int blk_off(const HVQPlane *p, int x, int y)
{
    return (y + 1) * p->h_blocks_safe + (x + 1);
}

static void init_borders(HVQPlane *p)
{
    int x, y;
    for (x = -1; x <= p->h_blocks; x++) {
        p->dc[blk_off(p, x, -1)] = 0x7F;
        p->dc[blk_off(p, x, p->v_blocks)] = 0x7F;
    }
    for (y = -1; y <= p->v_blocks; y++) {
        p->dc[blk_off(p, -1, y)] = 0x7F;
        p->dc[blk_off(p, p->h_blocks, y)] = 0x7F;
    }
}

static void compute_dc(HVQPlane *p, const uint8_t *src, int stride)
{
    int x, y, i, j;
    init_borders(p);
    for (y = 0; y < p->v_blocks; y++) {
        for (x = 0; x < p->h_blocks; x++) {
            int sum = 0;
            const uint8_t *b = src + (size_t)y * 4 * stride + x * 4;
            for (i = 0; i < 4; i++)
                for (j = 0; j < 4; j++)
                    sum += b[i * stride + j];
            p->dc[blk_off(p, x, y)] = (sum + 8) >> 4;
        }
    }
}

/* mirror of IpicDcvDec()'s predictor */
static int dc_predict(const HVQPlane *p, int x, int y, int left_value)
{
    if (x == 0)
        return p->dc[blk_off(p, x, y - 1)];
    return (left_value + p->dc[blk_off(p, x, y - 1)] + 1) / 2;
}

/* ------------------------------------------------------------------- */
/* nest construction: mirrors MakeNest(), reading this frame's own DC map
 * (top nibble) at a fixed anchor (0,0); MakeNest's own mirror/empty-fill
 * logic already makes any anchor safe, so 0,0 keeps this simple. */

static void make_nest(HVQM4EncContext *s)
{
    HVQPlane *y = &s->planes[LUMA_IDX];
    int hn = s->h_nest_size, vn = s->v_nest_size;
    int h_blocks = FFMIN(hn, y->h_blocks);
    int v_blocks = FFMIN(vn, y->v_blocks);
    int h_mirror = FFMIN(hn - h_blocks, h_blocks);
    int v_mirror = FFMIN(vn - v_blocks, v_blocks);
    int i, j;

    for (i = 0; i < vn; i++) {
        int sy = i < v_blocks ? i : (i < v_blocks + v_mirror ? v_blocks - 1 - (i - v_blocks) : -1);
        for (j = 0; j < hn; j++) {
            int sx = j < h_blocks ? j : (j < h_blocks + h_mirror ? h_blocks - 1 - (j - h_blocks) : -1);
            uint8_t v = (sx < 0 || sy < 0) ? 0 : (y->dc[blk_off(y, sx, sy)] >> 4) & 0xF;
            s->nest[i * hn + j] = v;
        }
    }
}

static void nest_patch(HVQM4EncContext *s, uint16_t bits, uint8_t out[4][4])
{
    int hn = s->h_nest_size;
    int offset70 = bits & 0x3F;
    int offset38 = (bits >> 6) & 0x1F;
    int stride70 = (bits >> 11) & 1;
    int stride38 = (bits >> 12) & 1;
    const uint8_t *base;
    int x_stride, y_stride, x, y;

    if (s->is_landscape) {
        base = s->nest + hn * offset38 + offset70;
        x_stride = 1 << stride70;
        y_stride = hn << stride38;
    } else {
        base = s->nest + hn * offset70 + offset38;
        x_stride = 1 << stride38;
        y_stride = hn << stride70;
    }

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            out[y][x] = base[y * y_stride + x * x_stride];
}

static const int32_t hvq_div_table[16] = {
    0,
    0x1000 / (1 * 16) * 16, 0x1000 / (2 * 16) * 16, 0x1000 / (3 * 16) * 16,
    0x1000 / (4 * 16) * 16, 0x1000 / (5 * 16) * 16, 0x1000 / (6 * 16) * 16,
    0x1000 / (7 * 16) * 16, 0x1000 / (8 * 16) * 16, 0x1000 / (9 * 16) * 16,
    0x1000 / (10 * 16) * 16, 0x1000 / (11 * 16) * 16, 0x1000 / (12 * 16) * 16,
    0x1000 / (13 * 16) * 16, 0x1000 / (14 * 16) * 16, 0x1000 / (15 * 16) * 16,
};

/* Given a chosen patch and its raw-basis correlation with the current
 * residual, quantize the desired scale to an achievable
 * (running_sum + off2) * (+-inverse), where running_sum only ever grows
 * by a non-negative multiple of 4 (bufTree0 is unsigned, scale 2: the
 * Huffman symbol byte b decodes to b<<2). Returns the chosen factor and
 * updates *running_sum / fills *out_bits / *out_sym. */
static int32_t quantize_term(int64_t dot, int64_t energy, int min, int max,
                              uint16_t base_bits, int *running_sum,
                              uint16_t *out_bits, uint8_t *out_sym)
{
    int32_t inverse = hvq_div_table[max - min];
    int sign = dot < 0;
    int64_t adot = sign ? -dot : dot;
    double want; /* desired (sum + off2) */
    int off2, target_total, delta, byte;

    if (!inverse || energy <= 0)
        return 0;

    want = (double)adot * 4096.0 / ((double)energy * (double)inverse);
    target_total = (int)(want + 0.5);
    if (target_total < *running_sum)
        target_total = *running_sum;

    /* exactly one off2 in 0..3 makes (target_total-off2-running_sum) a
     * multiple of 4 */
    delta = -1;
    off2 = 0;
    {
        int cand;
        for (cand = 0; cand < 4; cand++) {
            int d = target_total - cand - *running_sum;
            if (d >= 0 && d % 4 == 0) {
                delta = d;
                off2 = cand;
                break;
            }
        }
        if (delta < 0) {
            /* target below running_sum for every off2: hold at zero growth */
            off2 = 0;
            delta = 0;
        }
    }
    byte = delta / 4;
    if (byte > 255) {
        byte = 255;
        delta = byte * 4;
    }

    *running_sum += delta;
    *out_sym = (uint8_t)byte;
    *out_bits = base_bits | (off2 << 13) | (sign << 15);

    {
        int32_t factor = (*running_sum + off2) * inverse;
        return sign ? -factor : factor;
    }
}

/* Greedy matching pursuit: coarse-then-refine nest search per term. */
static int search_aot(HVQM4EncContext *s, const int target[4][4], int max_bases,
                       BlockDecision *bd, int32_t result_out[4][4])
{
    int32_t resid[4][4], result[4][4];
    int running_sum = 0;
    int k, y, x;
    int max38 = s->is_landscape ? 31 : 63;
    int max70 = s->is_landscape ? 63 : 31;
    int coarse = FFMAX(1, s->search_step);

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            resid[y][x] = target[y][x];
            result[y][x] = 0;
        }

    bd->nb_terms = 0;
    for (k = 0; k < max_bases; k++) {
        int64_t best_score = -1, best_dot = 0, best_energy = 1;
        uint16_t best_bits = 0;
        uint8_t best_patch[4][4] = { { 0 } };
        int best_min = 0, best_max = 0;
        int off38c, off70c, pass;

        for (pass = 0; pass < 2; pass++) {
            int lo38 = 0, hi38 = max38 - 3, step38 = coarse;
            int lo70 = 0, hi70 = max70 - 3, step70 = coarse;
            if (pass == 1) {
                if (best_score < 0)
                    break;
                lo38 = FFMAX(0, ((best_bits >> 6) & 0x1F) - coarse);
                hi38 = FFMIN(max38 - 3, ((best_bits >> 6) & 0x1F) + coarse);
                lo70 = FFMAX(0, (best_bits & 0x3F) - coarse);
                hi70 = FFMIN(max70 - 3, (best_bits & 0x3F) + coarse);
                step38 = step70 = 1;
            }
            for (off38c = lo38; off38c <= hi38; off38c += step38) {
                for (off70c = lo70; off70c <= hi70; off70c += step70) {
                    int s70, s38;
                    int max_s70 = (off70c * 2 + 6 <= max70) ? 1 : 0;
                    int max_s38 = (off38c * 2 + 6 <= max38) ? 1 : 0;
                    for (s70 = 0; s70 <= max_s70; s70++) {
                        for (s38 = 0; s38 <= max_s38; s38++) {
                            uint16_t bits = (uint16_t)(off70c | (off38c << 6) | (s70 << 11) | (s38 << 12));
                            uint8_t patch[4][4];
                            int64_t dot = 0, energy = 0;
                            int mn, mx;

                            nest_patch(s, bits, patch);
                            mn = mx = patch[0][0];
                            for (y = 0; y < 4; y++)
                                for (x = 0; x < 4; x++) {
                                    if (patch[y][x] < mn) mn = patch[y][x];
                                    if (patch[y][x] > mx) mx = patch[y][x];
                                    dot += (int64_t)resid[y][x] * patch[y][x];
                                    energy += (int64_t)patch[y][x] * patch[y][x];
                                }
                            if (mx == mn || energy == 0)
                                continue;
                            {
                                int64_t score = dot < 0 ? -dot : dot;
                                /* rank by |dot|^2/energy without overflow */
                                double s2 = (double)score * (double)score / (double)energy;
                                int64_t iscore = (int64_t)s2;
                                if (iscore > best_score) {
                                    best_score = iscore;
                                    best_bits = bits;
                                    best_dot = dot;
                                    best_energy = energy;
                                    best_min = mn;
                                    best_max = mx;
                                    memcpy(best_patch, patch, sizeof(patch));
                                }
                            }
                        }
                    }
                }
            }
        }

        if (best_score <= 0)
            break;

        {
            uint16_t out_bits;
            uint8_t out_sym;
            int32_t factor = quantize_term(best_dot, best_energy, best_min, best_max,
                                            best_bits, &running_sum, &out_bits, &out_sym);
            if (factor == 0 && k > 0)
                break;

            bd->term_bits[bd->nb_terms] = out_bits;
            bd->term_sym[bd->nb_terms] = out_sym;
            bd->nb_terms++;

            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    result[y][x] += factor * (int32_t)best_patch[y][x];
            /* resid must reflect the *true* remaining error (in pixel
             * scale, i.e. after the >>unk_shift the decoder applies) for
             * the next term's search, recomputed exactly each time to
             * avoid incremental drift */
            {
                int32_t mean = 0;
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++)
                        mean += result[y][x];
                mean >>= 4;
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++)
                        resid[y][x] = target[y][x] - ((result[y][x] - mean) >> HVQ_UNK_SHIFT);
            }
        }
    }

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            result_out[y][x] = result[y][x];

    {
        int32_t mean = 0;
        int64_t err = 0;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                mean += result[y][x];
        mean >>= 4;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++) {
                int32_t v = (result[y][x] - mean) >> HVQ_UNK_SHIFT;
                int32_t d = target[y][x] - v;
                err += (int64_t)d * d;
            }
        return (int)FFMIN(err, INT32_MAX);
    }
}

/* ------------------------------------------------------------------- */
/* per-block mode decision (single pass, decisions stored for later
 * histogram + emit passes) */

static void decide_blocks(HVQM4EncContext *s, int plane_idx,
                           const uint8_t *src, int stride)
{
    HVQPlane *p = &s->planes[plane_idx];
    int x, y;

    for (y = 0; y < p->v_blocks; y++) {
        for (x = 0; x < p->h_blocks; x++) {
            BlockDecision *bd = &s->decisions[plane_idx][y * p->h_blocks + x];
            const uint8_t *b = src + (size_t)y * 4 * stride + x * 4;
            int dc = p->dc[blk_off(p, x, y)];
            int target[4][4];
            int32_t result[4][4];
            int err_dc = 0, err_aot, i, j;

            for (i = 0; i < 4; i++)
                for (j = 0; j < 4; j++) {
                    target[i][j] = (int)b[i * stride + j] - dc;
                    err_dc += target[i][j] * target[i][j];
                }

#ifdef HVQM4ENC_NO_CHROMA_AOT
            err_aot = search_aot(s, target, plane_idx == 0 ? s->max_bases : 0, bd, result);
#else
            err_aot = search_aot(s, target, s->max_bases, bd, result);
#endif
#ifdef HVQM4ENC_DEBUG_STATS
            {
                static int nprinted = 0;
                if (nprinted < 20 && err_dc > 50 && plane_idx >= 1) {
                    av_log(NULL, AV_LOG_WARNING, "blk(%d,%d) plane%d err_dc=%d err_aot=%d nb_terms=%d bits0=%04x sym0=%d\n",
                           x, y, plane_idx, err_dc, err_aot, bd->nb_terms,
                           bd->nb_terms ? bd->term_bits[0] : 0, bd->nb_terms ? bd->term_sym[0] : -1);
                    nprinted++;
                }
            }
#endif
            if (bd->nb_terms > 0 && err_aot <= err_dc) {
                bd->type = bd->nb_terms; /* 1..MAX_BASES */
            } else {
                bd->nb_terms = 0;
                bd->type = BLK_DC;
                err_aot = err_dc;
            }

            if (err_aot > s->org_threshold * 16) {
                bd->type = BLK_ORG;
                bd->nb_terms = 0;
                for (i = 0; i < 4; i++)
                    for (j = 0; j < 4; j++)
                        bd->org[i][j] = b[i * stride + j];
            }
#ifdef HVQM4ENC_FORCE_ORG
            bd->type = BLK_ORG;
            bd->nb_terms = 0;
            for (i = 0; i < 4; i++)
                for (j = 0; j < 4; j++)
                    bd->org[i][j] = b[i * stride + j];
#endif
#ifdef HVQM4ENC_FORCE_DC
            bd->type = BLK_DC;
            bd->nb_terms = 0;
#endif
#ifdef HVQM4ENC_DEBUG_STATS
            if (bd->type == BLK_DC) hvqm4_dbg_dc[plane_idx]++;
            else if (bd->type == BLK_ORG) hvqm4_dbg_org[plane_idx]++;
            else hvqm4_dbg_aot[plane_idx]++;
#endif
        }
    }
}

/* ------------------------------------------------------------------- */

typedef struct FrameStreams {
    HVQHuff huff_dc;         /* dc_values, shared across all 3 planes */
    HVQHuff huff_rle;        /* dc_rle + basis_num_run, shared */
    HVQHuff huff_buf0;       /* bufTree0, shared across all 3 planes */
    HVQHuff huff_basis;      /* basis_num, shared luma+chroma */

    HVQBitWriter dc_values[PLANE_COUNT];
    HVQBitWriter dc_rle[PLANE_COUNT];
    HVQBitWriter bufTree0[PLANE_COUNT];
    HVQBitWriter fixvl[PLANE_COUNT];
    HVQBitWriter basis_num[2];
    HVQBitWriter basis_num_run[2]; /* [0] carries the (unused) embedded tree */
} FrameStreams;

#define DC_MIN (-0x80)
#define DC_MAX (0x7F)

/* dc pass: histogram-only or emit, driven by `hist`/`w` being non-NULL */
static void walk_dc_plane(HVQM4EncContext *s, int plane_idx,
                           int *hist_dc, int *hist_rle,
                           HVQHuff *huff_dc, HVQHuff *huff_rle, HVQBitWriter *w_dc, HVQBitWriter *w_rle)
{
    HVQPlane *p = &s->planes[plane_idx];
    int x, y, run = 0;

    for (y = 0; y < p->v_blocks; y++) {
        int left_value = p->dc[blk_off(p, 0, y - 1)];
        for (x = 0; x < p->h_blocks; x++) {
            int predicted = dc_predict(p, x, y, left_value);
            int actual = p->dc[blk_off(p, x, y)];
            int delta = actual - predicted;

#ifdef HVQM4ENC_NO_DC_RLE
            if (hist_dc) plan_signed_ovf(delta, DC_MIN, DC_MAX, hist_dc);
            else         emit_signed_ovf(w_dc, huff_dc, delta, DC_MIN, DC_MAX);
#else
            if (delta == 0) {
                run++;
            } else {
                if (run > 0) {
                    if (hist_dc) { plan_signed_ovf(0, DC_MIN, DC_MAX, hist_dc); plan_unsigned_ovf(run - 1, 0xFF, hist_rle); }
                    else         { emit_signed_ovf(w_dc, huff_dc, 0, DC_MIN, DC_MAX); emit_unsigned_ovf(w_rle, huff_rle, run - 1, 0xFF); }
                    run = 0;
                }
                if (hist_dc) plan_signed_ovf(delta, DC_MIN, DC_MAX, hist_dc);
                else         emit_signed_ovf(w_dc, huff_dc, delta, DC_MIN, DC_MAX);
            }
#endif
            left_value = actual;
        }
    }
    if (run > 0) {
        if (hist_dc) { plan_signed_ovf(0, DC_MIN, DC_MAX, hist_dc); plan_unsigned_ovf(run - 1, 0xFF, hist_rle); }
        else         { emit_signed_ovf(w_dc, huff_dc, 0, DC_MIN, DC_MAX); emit_unsigned_ovf(w_rle, huff_rle, run - 1, 0xFF); }
    }
}

static void walk_basis_num_luma(HVQM4EncContext *s, int *hist, HVQHuff *huff, HVQBitWriter *w)
{
    HVQPlane *p = &s->planes[LUMA_IDX];
    int i, n = p->h_blocks * p->v_blocks;
    for (i = 0; i < n; i++) {
        int val = s->decisions[LUMA_IDX][i].type;
        if (hist) hist[(uint8_t)val]++;
        else      huff_emit(w, huff, (uint8_t)val);
    }
}

static void walk_basis_num_chroma(HVQM4EncContext *s, int *hist, HVQHuff *huff, HVQBitWriter *w)
{
    HVQPlane *p = &s->planes[1];
    int i, n = p->h_blocks * p->v_blocks;
    for (i = 0; i < n; i++) {
        int u = s->decisions[1][i].type;
        int v = s->decisions[2][i].type;
        int val = (u & 0xF) | ((v & 0xF) << 4);
        if (hist) hist[(uint8_t)val]++;
        else      huff_emit(w, huff, (uint8_t)val);
    }
}

static void walk_bufTree0(HVQM4EncContext *s, int plane_idx, int *hist, HVQHuff *huff, HVQBitWriter *w)
{
    HVQPlane *p = &s->planes[plane_idx];
    int i, k, n = p->h_blocks * p->v_blocks;
    for (i = 0; i < n; i++) {
        BlockDecision *bd = &s->decisions[plane_idx][i];
        for (k = 0; k < bd->nb_terms; k++) {
            if (hist) hist[bd->term_sym[k]]++;
            else      huff_emit(w, huff, bd->term_sym[k]);
        }
    }
}

static void walk_fixvl(HVQM4EncContext *s, int plane_idx, HVQBitWriter *w)
{
    HVQPlane *p = &s->planes[plane_idx];
    int i, k, n = p->h_blocks * p->v_blocks;
    for (i = 0; i < n; i++) {
        BlockDecision *bd = &s->decisions[plane_idx][i];
        if (bd->type == BLK_ORG) {
            int y, x;
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    bw_put_byte_raw(w, bd->org[y][x]);
        } else {
            for (k = 0; k < bd->nb_terms; k++) {
                bw_put_byte_raw(w, (uint8_t)(bd->term_bits[k] >> 8));
                bw_put_byte_raw(w, (uint8_t)(bd->term_bits[k] & 0xFF));
            }
        }
    }
}

static int build_frame_streams(HVQM4EncContext *s, FrameStreams *fs)
{
    int hist_dc[256] = { 0 }, hist_rle[256] = { 0 };
    int hist_buf0[256] = { 0 }, hist_basis[256] = { 0 };
    int i;

    memset(fs, 0, sizeof(*fs));
    for (i = 0; i < PLANE_COUNT; i++) {
        bw_init(&fs->dc_values[i]);
        bw_init(&fs->dc_rle[i]);
        bw_init(&fs->bufTree0[i]);
        bw_init(&fs->fixvl[i]);
    }
    bw_init(&fs->basis_num[0]);
    bw_init(&fs->basis_num[1]);
    bw_init(&fs->basis_num_run[0]);
    bw_init(&fs->basis_num_run[1]);

    /* ---- histogram pass ---- */
    for (i = 0; i < PLANE_COUNT; i++)
        walk_dc_plane(s, i, hist_dc, hist_rle, NULL, NULL, NULL, NULL);
    walk_basis_num_luma(s, hist_basis, NULL, NULL);
    walk_basis_num_chroma(s, hist_basis, NULL, NULL);
    for (i = 0; i < PLANE_COUNT; i++)
        walk_bufTree0(s, i, hist_buf0, NULL, NULL);
    /* basis_num_run is never emitted (no type==0 blocks are produced by
     * this encoder yet); reuse the rle tree's alphabet so its (empty)
     * stream is still well-formed */

    huff_build(&fs->huff_dc, hist_dc);
    huff_build(&fs->huff_rle, hist_rle);
    huff_build(&fs->huff_buf0, hist_buf0);
    huff_build(&fs->huff_basis, hist_basis);

    /* the first stream of each shared-tree group carries the serialized
     * tree; readTree() short-circuits to root=0 when buf.size==0, so an
     * empty group (nothing to send) needs no tree bits emitted here at
     * all - the muxer will just record a zero-length stream instead */
    if (!fs->huff_dc.empty)    huff_serialize(&fs->dc_values[0], &fs->huff_dc, fs->huff_dc.root);
    if (!fs->huff_rle.empty)   huff_serialize(&fs->basis_num_run[0], &fs->huff_rle, fs->huff_rle.root);
    if (!fs->huff_buf0.empty)  huff_serialize(&fs->bufTree0[0], &fs->huff_buf0, fs->huff_buf0.root);
    if (!fs->huff_basis.empty) huff_serialize(&fs->basis_num[0], &fs->huff_basis, fs->huff_basis.root);

    /* ---- emit pass ---- */
    for (i = 0; i < PLANE_COUNT; i++)
        walk_dc_plane(s, i, NULL, NULL, &fs->huff_dc, &fs->huff_rle, &fs->dc_values[i], &fs->dc_rle[i]);
    walk_basis_num_luma(s, NULL, &fs->huff_basis, &fs->basis_num[0]);
    walk_basis_num_chroma(s, NULL, &fs->huff_basis, &fs->basis_num[1]);
    for (i = 0; i < PLANE_COUNT; i++)
        walk_bufTree0(s, i, NULL, &fs->huff_buf0, &fs->bufTree0[i]);
    for (i = 0; i < PLANE_COUNT; i++)
        walk_fixvl(s, i, &fs->fixvl[i]);

    for (i = 0; i < PLANE_COUNT; i++) {
        bw_flush(&fs->dc_values[i]);
        bw_flush(&fs->dc_rle[i]);
        bw_flush(&fs->bufTree0[i]);
        bw_flush(&fs->fixvl[i]);
    }
    bw_flush(&fs->basis_num[0]);
    bw_flush(&fs->basis_num[1]);
    bw_flush(&fs->basis_num_run[0]);
    bw_flush(&fs->basis_num_run[1]);

    return 0;
}

static void free_frame_streams(FrameStreams *fs)
{
    int i;
    for (i = 0; i < PLANE_COUNT; i++) {
        bw_free(&fs->dc_values[i]);
        bw_free(&fs->dc_rle[i]);
        bw_free(&fs->bufTree0[i]);
        bw_free(&fs->fixvl[i]);
    }
    bw_free(&fs->basis_num[0]);
    bw_free(&fs->basis_num[1]);
    bw_free(&fs->basis_num_run[0]);
    bw_free(&fs->basis_num_run[1]);
}

/* ------------------------------------------------------------------- */
/* pack the I-picture payload exactly as HVQM4DecodeIpic() expects it */

static void put_be16(uint8_t **p, uint16_t v) { AV_WB16(*p, v); *p += 2; }
static void put_be32(uint8_t **p, uint32_t v) { AV_WB32(*p, v); *p += 4; }

static uint8_t *pack_ipic(HVQM4EncContext *s, FrameStreams *fs, int *out_size)
{
    /* stream order in the offset table, matching HVQM4DecodeIpic() */
    HVQBitWriter *streams[16] = {
        &fs->basis_num[0], &fs->basis_num_run[0], &fs->basis_num[1], &fs->basis_num_run[1],
        &fs->dc_values[0], &fs->bufTree0[0], &fs->fixvl[0],
        &fs->dc_values[1], &fs->bufTree0[1], &fs->fixvl[1],
        &fs->dc_values[2], &fs->bufTree0[2], &fs->fixvl[2],
        &fs->dc_rle[0], &fs->dc_rle[1], &fs->dc_rle[2],
    };
    int i, header_size = 8 + 16 * 4; /* dc_shift/unk_shift/pad/nest_x/nest_y + offsets */
    int data_size = 0;
    uint8_t *buf, *p;
    uint32_t *offsets = av_malloc(16 * sizeof(uint32_t));

    if (!offsets)
        return NULL;

    for (i = 0; i < 16; i++) {
        offsets[i] = data_size;
        data_size += 4 + streams[i]->size; /* size field + payload */
    }
#ifdef HVQM4ENC_DEBUG_STATS
    {
        static int printed = 0;
        if (!printed) {
            printed = 1;
            for (i = 0; i < 16; i++)
                av_log(NULL, AV_LOG_WARNING, "stream[%d] size=%d offset=%u\n", i, streams[i]->size, offsets[i]);
        }
    }
#endif

    *out_size = header_size + data_size + 4 /* overread slack */;
    buf = av_mallocz(*out_size);
    if (!buf) {
        av_free(offsets);
        return NULL;
    }

    p = buf;
    *p++ = 0; /* dc_shift */
    *p++ = HVQ_UNK_SHIFT; /* unk_shift */
    put_be16(&p, 0); /* unused */
    put_be16(&p, 0); /* nest_x */
    put_be16(&p, 0); /* nest_y */
    for (i = 0; i < 16; i++)
        put_be32(&p, offsets[i]);

    av_assert0(p == buf + header_size);
    for (i = 0; i < 16; i++) {
        put_be32(&p, (uint32_t)streams[i]->size);
        if (streams[i]->size)
            memcpy(p, streams[i]->buf, streams[i]->size);
        p += streams[i]->size;
    }

    av_free(offsets);
    return buf;
}

/* ------------------------------------------------------------------- */

static int hvqm4_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    HVQM4EncContext *s = avctx->priv_data;
    FrameStreams fs;
    uint8_t *payload;
    int payload_size, ret, i;

    if (!frame)
        return 0;

#ifdef HVQM4ENC_SWAP_UV
    {
        uint8_t *swapped[3] = { frame->data[0], frame->data[2], frame->data[1] };
        int swapped_ls[3] = { frame->linesize[0], frame->linesize[2], frame->linesize[1] };
        for (i = 0; i < PLANE_COUNT; i++)
            compute_dc(&s->planes[i], swapped[i], swapped_ls[i]);
        make_nest(s);
        for (i = 0; i < PLANE_COUNT; i++)
            decide_blocks(s, i, swapped[i], swapped_ls[i]);
    }
#else
    for (i = 0; i < PLANE_COUNT; i++)
        compute_dc(&s->planes[i], frame->data[i], frame->linesize[i]);
    make_nest(s);

    for (i = 0; i < PLANE_COUNT; i++)
        decide_blocks(s, i, frame->data[i], frame->linesize[i]);
#endif

    if ((ret = build_frame_streams(s, &fs)) < 0)
        return ret;

    payload = pack_ipic(s, &fs, &payload_size);
    free_frame_streams(&fs);
    if (!payload)
        return AVERROR(ENOMEM);

    /* frame record body: [disp_id:4][I-picture payload] - the outer
     * media-type/frame-type/size/disp_id wrapper is added by the hvqm4
     * muxer, matching hvqm4_read_packet()'s parsing. */
    if ((ret = ff_get_encode_buffer(avctx, pkt, 4 + payload_size, 0)) < 0) {
        av_free(payload);
        return ret;
    }
    AV_WB32(pkt->data, s->disp_id++); /* disp_id: this encoder is intra-only,
                                        * so display order == encode order */
    memcpy(pkt->data + 4, payload, payload_size);
    av_free(payload);

    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

#define OFFSET(x) offsetof(HVQM4EncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption hvqm4_options[] = {
    { "max_bases",   "max AOT basis terms per block (1-5)", OFFSET(max_bases),   AV_OPT_TYPE_INT, {.i64 = 3},   1, MAX_BASES, VE },
    { "search_step", "coarse nest-search stride",           OFFSET(search_step), AV_OPT_TYPE_INT, {.i64 = 4},   1, 16, VE },
    { "org_threshold", "mean squared error above which a block escapes to literal pixels", OFFSET(org_threshold), AV_OPT_TYPE_INT, {.i64 = 400}, 1, 65536, VE },
    { NULL }
};

static const AVClass hvqm4_encoder_class = {
    .class_name = "hvqm4 encoder",
    .item_name  = av_default_item_name,
    .option     = hvqm4_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_hvqm4_encoder = {
    .p.name         = "hvqm4",
    CODEC_LONG_NAME("HVQM4 (Hudson Vector Quantization Movie)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_HVQM4,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(HVQM4EncContext),
    .p.priv_class   = &hvqm4_encoder_class,
    .init           = hvqm4_encode_init,
    .close          = hvqm4_encode_close,
    FF_CODEC_ENCODE_CB(hvqm4_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P),
};
