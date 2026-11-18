/*
 * MIVF order-1 binary range coder (M2Y2), shared by the m2y1 encoder and decoder
 *
 * Ported from the MIVF player and toolkit, which carry this notice:
 *
 * MIT License
 *
 * Copyright (c) 2026 Micah Lagger / MIVF Player contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef AVCODEC_MIVF_RC_H
#define AVCODEC_MIVF_RC_H

#include <stdint.h>
#include <stddef.h>

/* Binary-coder constants (LZMA conventions). */
#define MIVF_RC_MODEL_BITS  11
#define MIVF_RC_MODEL_TOTAL (1u << MIVF_RC_MODEL_BITS)   /* 2048 */
#define MIVF_RC_MOVE_BITS   5
#define MIVF_RC_TOP         (1u << 24)
#define MIVF_RC_PROB_INIT   (MIVF_RC_MODEL_TOTAL >> 1)   /* 1024 = p(0.5) */

/* Context order: how many high bits of the previous byte form the context.
   8 = full order-1 (256 contexts); 0 = order-0 (1 context). With per-packet
   reset, a small value (2 -> 4 contexts) gives the best size AND keeps the
   model cache-resident on ARM11. Encoder and decoder share this header so both
   sides always agree. NOTE: changing this changes the compressed format. */
#ifndef MIVF_RC_CTX_BITS
#define MIVF_RC_CTX_BITS  2
#endif
#define MIVF_RC_NCTX      (1 << MIVF_RC_CTX_BITS)
#define MIVF_RC_CTX_OF(prev) ((prev) >> (8 - MIVF_RC_CTX_BITS))

/* Per-context byte model: a 256-node bit-tree of adaptive probabilities.
   Index 1 is the root; for an 8-bit symbol the walk visits nodes 1..255 and
   ends on a leaf index in [256,511] whose low 8 bits are the byte. */
typedef struct {
    uint16_t probs[256];
} MivfRcModel;

static inline void mivf_rc_model_reset(MivfRcModel *m) {
    for (int i = 0; i < 256; i++) m->probs[i] = MIVF_RC_PROB_INIT;
}

/* Order-N context model. Context = top MIVF_RC_CTX_BITS bits of previous byte. */
typedef struct {
    MivfRcModel ctx[MIVF_RC_NCTX];
    /* HFIX-PERF: lazy per-packet reset. Re-initializing every context at each
       packet wastes time (and on ARM11 thrashes cache). Instead we bump a
       generation counter per packet and reset each context only the first time
       it is used. Bit-exact with a full reset: an unused context is never read. */
    uint32_t gen[MIVF_RC_NCTX];
    uint32_t cur_gen;
    uint32_t magic;
} MivfRcO1;

#define MIVF_RC_O1_MAGIC 0x4D52324Fu  /* "MR2O": detects first use on raw memory */

static inline void mivf_rc_o1_init(MivfRcO1 *m) {
    if (m->magic != MIVF_RC_O1_MAGIC) {
        /* First use on possibly-uninitialized memory: mark every context stale. */
        for (int c = 0; c < MIVF_RC_NCTX; c++) m->gen[c] = 0u;
        m->cur_gen = 0u;
        m->magic = MIVF_RC_O1_MAGIC;
    }
    m->cur_gen++;
    if (m->cur_gen == 0u) {
        /* Generation wrapped (after ~4e9 packets): force every context stale. */
        for (int c = 0; c < MIVF_RC_NCTX; c++) m->gen[c] = 0u;
        m->cur_gen = 1u;
    }
}

/* Return context c, lazily resetting it on first use within this packet. */
static inline MivfRcModel *mivf_rc_o1_ctx(MivfRcO1 *m, int c) {
    if (m->gen[c] != m->cur_gen) {
        mivf_rc_model_reset(&m->ctx[c]);
        m->gen[c] = m->cur_gen;
    }
    return &m->ctx[c];
}

/* ---------------- encoder ---------------- */
typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t  cache;
    uint64_t cache_size;
    uint8_t *out;
    size_t   cap, len;
    int      overflow;
} MivfRcEnc;

static inline void mivf_rc_enc_init(MivfRcEnc *e, uint8_t *out, size_t cap) {
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    e->cache = 0;
    e->cache_size = 1;
    e->out = out;
    e->cap = cap;
    e->len = 0;
    e->overflow = 0;
}

static inline void mivf_rc_enc_put(MivfRcEnc *e, uint8_t b) {
    if (e->len < e->cap) e->out[e->len] = b; else e->overflow = 1;
    e->len++;
}

static inline void mivf_rc_enc_shift_low(MivfRcEnc *e) {
    if ((uint32_t)(e->low >> 32) != 0 || e->low < 0xFF000000ULL) {
        uint8_t temp = e->cache;
        do {
            mivf_rc_enc_put(e, (uint8_t)(temp + (uint8_t)(e->low >> 32)));
            temp = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)(e->low >> 24);
    }
    e->cache_size++;
    /* 32-bit shift: the just-captured top byte must be discarded (a 64-bit
       shift would leave it in bits 32-39 and cause spurious carries). */
    e->low = (uint32_t)e->low << 8;
}

static inline void mivf_rc_enc_bit(MivfRcEnc *e, uint16_t *prob, int bit) {
    uint32_t bound = (e->range >> MIVF_RC_MODEL_BITS) * (uint32_t)(*prob);
    if (bit == 0) {
        e->range = bound;
        *prob = (uint16_t)(*prob + ((MIVF_RC_MODEL_TOTAL - *prob) >> MIVF_RC_MOVE_BITS));
    } else {
        e->low += bound;
        e->range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> MIVF_RC_MOVE_BITS));
    }
    while (e->range < MIVF_RC_TOP) {
        e->range <<= 8;
        mivf_rc_enc_shift_low(e);
    }
}

static inline void mivf_rc_enc_byte(MivfRcEnc *e, MivfRcModel *m, int b) {
    int node = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (b >> i) & 1;
        mivf_rc_enc_bit(e, &m->probs[node], bit);
        node = (node << 1) | bit;
    }
}

static inline size_t mivf_rc_enc_finish(MivfRcEnc *e) {
    for (int i = 0; i < 5; i++) mivf_rc_enc_shift_low(e);
    return e->len;
}

/* ---------------- decoder ---------------- */
typedef struct {
    uint32_t range, code;
    const uint8_t *in;
    size_t pos, n;
} MivfRcDec;

static inline uint8_t mivf_rc_dec_get(MivfRcDec *d) {
    return d->pos < d->n ? d->in[d->pos++] : 0;
}

static inline void mivf_rc_dec_init(MivfRcDec *d, const uint8_t *in, size_t n) {
    d->in = in;
    d->n = n;
    d->pos = 0;
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    mivf_rc_dec_get(d);  /* first byte is always 0 (encoder cache priming) */
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | mivf_rc_dec_get(d);
}

static inline int mivf_rc_dec_bit(MivfRcDec *d, uint16_t *prob) {
    uint32_t bound = (d->range >> MIVF_RC_MODEL_BITS) * (uint32_t)(*prob);
    int bit;
    if (d->code < bound) {
        d->range = bound;
        *prob = (uint16_t)(*prob + ((MIVF_RC_MODEL_TOTAL - *prob) >> MIVF_RC_MOVE_BITS));
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> MIVF_RC_MOVE_BITS));
        bit = 1;
    }
    while (d->range < MIVF_RC_TOP) {
        if (d->range == 0) break;  /* anti-hang: only on corrupt input */
        d->range <<= 8;
        d->code = (d->code << 8) | mivf_rc_dec_get(d);
    }
    return bit;
}

static inline int mivf_rc_dec_byte(MivfRcDec *d, MivfRcModel *m) {
    int node = 1;
    while (node < 256) {
        int bit = mivf_rc_dec_bit(d, &m->probs[node]);
        node = (node << 1) | bit;
    }
    return node - 256;
}

/* ------------- whole-buffer convenience (caller owns the model) ------------- */
/* Returns compressed length. Check model->... not needed; caller sizes dst. */
static inline size_t mivf_rc_o1_compress(MivfRcO1 *model, const uint8_t *src, size_t n,
                                         uint8_t *dst, size_t dstcap) {
    mivf_rc_o1_init(model);
    MivfRcEnc e;
    mivf_rc_enc_init(&e, dst, dstcap);
    int prev = 0;
    for (size_t i = 0; i < n; i++) {
        int b = src[i];
        mivf_rc_enc_byte(&e, mivf_rc_o1_ctx(model, MIVF_RC_CTX_OF(prev)), b);
        prev = b;
    }
    return mivf_rc_enc_finish(&e);
}

static inline void mivf_rc_o1_decompress(MivfRcO1 *model, const uint8_t *src, size_t n,
                                         uint8_t *dst, size_t outn) {
    mivf_rc_o1_init(model);
    MivfRcDec d;
    mivf_rc_dec_init(&d, src, n);
    int prev = 0;
    for (size_t i = 0; i < outn; i++) {
        int b = mivf_rc_dec_byte(&d, mivf_rc_o1_ctx(model, MIVF_RC_CTX_OF(prev)));
        dst[i] = (uint8_t)b;
        prev = b;
    }
}

#endif /* MIVF_RC_H */
