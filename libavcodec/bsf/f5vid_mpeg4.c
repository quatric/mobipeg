/*
 * Factor 5 .vid custom-entropy -> standard MPEG-4 ASP bitstream filter
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 *
 * This filter owns the actual bitstream *conversion* for the f5vid demuxer
 * (libavformat/f5vid.c): Factor 5's Carmen Sandiego GC .vid format carries
 * DivX/MPEG-4 ASP video encoded with Factor 5's own custom entropy coding
 * (custom AC VLC tables, custom signed motion-vector VLC, an extra bit
 * after large intra DC sizes, no VOL/VOP start codes). The demuxer only
 * extracts the raw F5 MB payload plus a tiny per-frame metadata header
 * (frame type/quant/fcode/rounding); this filter turns that into a
 * standards-compliant MPEG-4 ASP VOP bitstream (VOL extradata synthesis on
 * init, VOP header synthesis + I/P MB transcode per packet) that the
 * generic mpeg4 decoder accepts unmodified.
 *
 * Input packet layout (as produced by the f5vid demuxer): an 8-byte
 * metadata header followed by the untouched F5 MB bitstream for one frame:
 *   byte 0: vop_type (0 = I, 1 = P)
 *   byte 1: quant (1..31)
 *   byte 2: fwd fcode (1..7, P only; ignored for I)
 *   byte 3: rounding (P vop_rounding_type bit; ignored for I)
 *   bytes 4-7: reserved, currently 0
 *   bytes 8..: raw F5 MB data for the frame
 *
 * Output: a clean MPEG-4 VOP (start code + header + MB data), matching
 * what the native mpeg4 decoder expects once given the VOL this filter
 * places in par_out->extradata.
 *
 * The VLC tables and transcode logic below are moved byte-for-byte out of
 * the demuxer's former inline transcoder; see libavformat/f5vid.c for the
 * container-format documentation this used to also describe.
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

#include "libavutil/intreadwrite.h"
#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavcodec/bsf.h"
#include "libavcodec/bsf_internal.h"

#define F5VID_TIME_RES   60000
#define F5VID_TINC_BITS  16 /* bit_length(60000-1) */

/* ---- minimal MPEG-4 bit writer (VOL/VOP synthesis) ---- */
typedef struct F5BitW {
    uint8_t *buf;
    int size;   /* allocated */
    int len;    /* bytes used */
    uint32_t cache;
    int nbits;  /* bits in cache */
} F5BitW;

static int f5bw_ensure(F5BitW *w, int extra)
{
    if (w->len + extra > w->size) {
        int ns = FFMAX(w->size * 2, w->len + extra + 64);
        uint8_t *nb = av_realloc(w->buf, ns);
        if (!nb)
            return AVERROR(ENOMEM);
        w->buf = nb;
        w->size = ns;
    }
    return 0;
}

static int f5bw_put(F5BitW *w, uint32_t val, int n)
{
    if (n <= 0)
        return 0;
    if (n == 32) {
        /* avoid UB of 32-bit shift */
        int ret = f5bw_put(w, val >> 16, 16);
        if (ret < 0)
            return ret;
        return f5bw_put(w, val & 0xFFFFu, 16);
    }
    w->cache = (w->cache << n) | (val & ((1u << n) - 1));
    w->nbits += n;
    while (w->nbits >= 8) {
        int ret;
        w->nbits -= 8;
        if ((ret = f5bw_ensure(w, 1)) < 0)
            return ret;
        w->buf[w->len++] = (w->cache >> w->nbits) & 0xFF;
    }
    return 0;
}

static int f5bw_flush(F5BitW *w)
{
    if (w->nbits > 0)
        return f5bw_put(w, 0, 8 - w->nbits);
    return 0;
}

/* Synthesize a minimal MPEG-4 VOL (Simple, rectangular, progressive,
 * H.263 quant, no resync/partition/sprite). Matches the proven graft VOL. */
static int f5vid_build_vol(F5BitW *w, int width, int height, uint32_t time_res)
{
    int ret;
    if ((ret = f5bw_put(w, 0x00000120, 32)) < 0) return ret; /* vol_start_code */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* random_accessible_vol */
    if ((ret = f5bw_put(w, 1, 8))  < 0) return ret; /* video_object_type = Simple */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* is_object_layer_identifier */
    if ((ret = f5bw_put(w, 1, 4))  < 0) return ret; /* aspect_ratio_info 1:1 */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* vol_control_parameters */
    if ((ret = f5bw_put(w, 0, 2))  < 0) return ret; /* vol_shape rectangular */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, time_res, 16)) < 0) return ret;
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* fixed_vop_rate */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, width, 13))  < 0) return ret;
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, height, 13)) < 0) return ret;
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* interlaced */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* obmc_disable */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* sprite_enable */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* not_8_bit */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* quant_type H.263 */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* complexity_estimation_disable */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* resync_marker_disable */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* data_partitioned */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* newpred_enable */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* reduced_resolution_vop_enable */
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* scalability */
    return f5bw_flush(w);
}

/* AC VLC tables: 12-bit 3-level lookup, entry (len<<17)|val with
 * val=(last<<16)|(run<<8)|level, 0x1bff = escape. Dumped from the game
 * binary; verified bit-exact against MPEG-4 intra TCOEF (including all
 * three escape forms and run/level offset tables). */
static const uint32_t f5ac_l1[112] = {
    0x000f0401, 0x000f0301, 0x000e0601, 0x000f0501, 0x000e0701, 0x000e0202, 0x000e0103, 0x000e0009 ,
    0x000d0002, 0x000d0002, 0x000c0501, 0x000c0501, 0x000d0201, 0x000d0201, 0x000d0101, 0x000d0101 ,
    0x000c0401, 0x000c0401, 0x000c0301, 0x000c0301, 0x000c0008, 0x000c0008, 0x000c0007, 0x000c0007 ,
    0x000c0102, 0x000c0102, 0x000c0006, 0x000c0006, 0x000a0201, 0x000a0201, 0x000a0201, 0x000a0201 ,
    0x000a0005, 0x000a0005, 0x000a0005, 0x000a0005, 0x000a0004, 0x000a0004, 0x000a0004, 0x000a0004 ,
    0x00090001, 0x00090001, 0x00090001, 0x00090001, 0x00090001, 0x00090001, 0x00090001, 0x00090001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002 ,
    0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002, 0x00060002 ,
    0x00080101, 0x00080101, 0x00080101, 0x00080101, 0x00080101, 0x00080101, 0x00080101, 0x00080101 ,
    0x00080003, 0x00080003, 0x00080003, 0x00080003, 0x00080003, 0x00080003, 0x00080003, 0x00080003 ,
};
static const uint32_t f5ac_l2[96] = {
    0x00140012, 0x00140011, 0x00130e01, 0x00130e01, 0x00130d01, 0x00130d01, 0x00130c01, 0x00130c01 ,
    0x00130b01, 0x00130b01, 0x00130a01, 0x00130a01, 0x00130102, 0x00130102, 0x00130004, 0x00130004 ,
    0x00120c01, 0x00120c01, 0x00120b01, 0x00120b01, 0x00120702, 0x00120702, 0x00120602, 0x00120602 ,
    0x00120502, 0x00120502, 0x00120303, 0x00120303, 0x00120203, 0x00120203, 0x00120106, 0x00120106 ,
    0x00120105, 0x00120105, 0x00120010, 0x00120010, 0x00120402, 0x00120402, 0x0012000f, 0x0012000f ,
    0x0012000e, 0x0012000e, 0x0012000d, 0x0012000d, 0x00110801, 0x00110801, 0x00110801, 0x00110801 ,
    0x00110701, 0x00110701, 0x00110701, 0x00110701, 0x00110601, 0x00110601, 0x00110601, 0x00110601 ,
    0x00110003, 0x00110003, 0x00110003, 0x00110003, 0x00100a01, 0x00100a01, 0x00100a01, 0x00100a01 ,
    0x00100901, 0x00100901, 0x00100901, 0x00100901, 0x00100801, 0x00100801, 0x00100801, 0x00100801 ,
    0x00110901, 0x00110901, 0x00110901, 0x00110901, 0x00100302, 0x00100302, 0x00100302, 0x00100302 ,
    0x00100104, 0x00100104, 0x00100104, 0x00100104, 0x0010000c, 0x0010000c, 0x0010000c, 0x0010000c ,
    0x0010000b, 0x0010000b, 0x0010000b, 0x0010000b, 0x0010000a, 0x0010000a, 0x0010000a, 0x0010000a ,
};
static const uint32_t f5ac_l3[120] = {
    0x00170007, 0x00170007, 0x00170006, 0x00170006, 0x00160016, 0x00160016, 0x00160015, 0x00160015 ,
    0x00150202, 0x00150202, 0x00150202, 0x00150202, 0x00150103, 0x00150103, 0x00150103, 0x00150103 ,
    0x00150005, 0x00150005, 0x00150005, 0x00150005, 0x00140d01, 0x00140d01, 0x00140d01, 0x00140d01 ,
    0x00140503, 0x00140503, 0x00140503, 0x00140503, 0x00140802, 0x00140802, 0x00140802, 0x00140802 ,
    0x00140403, 0x00140403, 0x00140403, 0x00140403, 0x00140304, 0x00140304, 0x00140304, 0x00140304 ,
    0x00140204, 0x00140204, 0x00140204, 0x00140204, 0x00140107, 0x00140107, 0x00140107, 0x00140107 ,
    0x00140014, 0x00140014, 0x00140014, 0x00140014, 0x00140013, 0x00140013, 0x00140013, 0x00140013 ,
    0x00160017, 0x00160017, 0x00160018, 0x00160018, 0x00160108, 0x00160108, 0x00160902, 0x00160902 ,
    0x00170302, 0x00170302, 0x00170402, 0x00170402, 0x00170f01, 0x00170f01, 0x00171001, 0x00171001 ,
    0x00180019, 0x0018001a, 0x0018001b, 0x00180109, 0x00180603, 0x0018010a, 0x00180205, 0x00180703 ,
    0x00180e01, 0x00190008, 0x00190502, 0x00190602, 0x00191101, 0x00191201, 0x00191301, 0x00191401 ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
};

typedef struct F5BitR {
    const uint8_t *buf;
    int nbits;
    int pos;
    int err;
} F5BitR;

static uint32_t f5br_show(F5BitR *r, int n)
{
    uint32_t v = 0;
    int i;
    if (n <= 0 || n > 25 || r->pos + n > r->nbits) {
        r->err = 1;
        return 0;
    }
    for (i = 0; i < n; i++)
        v = (v << 1) | ((r->buf[(r->pos + i) >> 3] >> (7 - ((r->pos + i) & 7))) & 1);
    return v;
}

static uint32_t f5br_get(F5BitR *r, int n)
{
    uint32_t v = f5br_show(r, n);
    if (!r->err)
        r->pos += n;
    return v;
}

/* Copy n bits from reader to writer (F5BitW). Returns 0 or <0 (OOM). */
static int f5_copy_bits(F5BitR *r, F5BitW *w, int n)
{
    int ret;
    while (n >= 8) {
        if ((ret = f5bw_put(w, f5br_get(r, 8), 8)) < 0)
            return ret;
        n -= 8;
    }
    if (n > 0)
        if ((ret = f5bw_put(w, f5br_get(r, n), n)) < 0)
            return ret;
    return r->err ? -1 : 0;
}

/* Intra MCBPC VLC (peek-only). Returns 0..7 or -1. *len = codeword bits. */
static int f5_vlc_mcbpc_intra(F5BitR *r, int *len)
{
    uint32_t p;
    if (r->pos + 9 > r->nbits)
        return -1;
    p = f5br_show(r, 9);
    if (r->err)
        return -1;
    if (p == 1) { *len = 9; return 8; }
    if ((p >> 8) & 1) { *len = 1; return 0; }                            /* 1 */
    switch ((p >> 6) & 7) {
    case 1: *len = 3; return 1;                                          /* 001 */
    case 2: *len = 3; return 2;                                          /* 010 */
    case 3: *len = 3; return 3;                                          /* 011 */
    default: break;
    }
    if (((p >> 5) & 15) == 1) { *len = 4; return 4; }                    /* 0001 */
    switch ((p >> 3) & 63) {
    case 1: *len = 6; return 5;                                          /* 000001 */
    case 2: *len = 6; return 6;                                          /* 000010 */
    case 3: *len = 6; return 7;                                          /* 000011 */
    default: return -1;
    }
}

/* CBPY VLC (peek-only, max 6 bits). Returns 0..15 or -1. */
static int f5_vlc_cbpy(F5BitR *r, int *len)
{
    uint32_t p;
    if (r->pos + 6 > r->nbits)
        return -1;
    p = f5br_show(r, 6);
    if (r->err)
        return -1;
    if ((p >> 4) == 3) { *len = 2; return 15; }                          /* 11 */
    switch ((p >> 2) & 15) {
    case  3: *len = 4; return 0;                                         /* 0011 */
    case  9: *len = 4; return 3;                                         /* 1001 */
    case  7: *len = 4; return 5;                                         /* 0111 */
    case 11: *len = 4; return 7;                                         /* 1011 */
    case  5: *len = 4; return 10;                                        /* 0101 */
    case 10: *len = 4; return 11;                                        /* 1010 */
    case  4: *len = 4; return 12;                                        /* 0100 */
    case  8: *len = 4; return 13;                                        /* 1000 */
    case  6: *len = 4; return 14;                                        /* 0110 */
    default: break;
    }
    switch ((p >> 1) & 31) {
    case  2: *len = 5; return 8;                                         /* 00010 */
    case  3: *len = 5; return 4;                                         /* 00011 */
    case  4: *len = 5; return 2;                                         /* 00100 */
    case  5: *len = 5; return 1;                                         /* 00101 */
    default: break;
    }
    switch (p & 63) {
    case  2: *len = 6; return 6;                                         /* 000010 */
    case  3: *len = 6; return 9;                                         /* 000011 */
    default: return -1;
    }
}

/* Intra DC size VLC, luma (max 11 bits) / chroma (max 12 bits).
 * Follows F5's exact unary+table structure. Returns size 0..12 or -1.
 * (The all-zero 12-bit chroma input is accepted as size 3 like F5.) */
static int f5_vlc_dc_luma(F5BitR *r, int *len)
{
    uint32_t p;
    int l;
    if (r->pos + 11 > r->nbits)
        return -1;
    p = f5br_show(r, 11);
    if (r->err)
        return -1;
    for (l = 11; l > 3; l--) {
        if ((p >> (11 - l)) == 1) { *len = l; return l + 1; }
    }
    /* top-3 fallback (sizes 0..4) */
    switch ((p >> 8) & 7) {
    case 3: *len = 3; return 0;                                          /* 011 */
    case 6: case 7: *len = 2; return 1;                                  /* 11x */
    case 4: case 5: *len = 2; return 2;                                  /* 10x */
    case 2: *len = 3; return 3;                                          /* 010 */
    case 1: *len = 3; return 4;                                          /* 001 */
    default: return -1;
    }
}

static int f5_vlc_dc_chroma(F5BitR *r, int *len)
{
    uint32_t p;
    int l;
    if (r->pos + 12 > r->nbits)
        return -1;
    p = f5br_show(r, 12);
    if (r->err)
        return -1;
    for (l = 12; l > 2; l--) {
        if ((p >> (12 - l)) == 1) { *len = l; return l; }
    }
    switch ((p >> 10) & 3) {
    case 3: *len = 2; return 0;                                          /* 11 */
    case 2: *len = 2; return 1;                                          /* 10 */
    case 1: *len = 2; return 2;                                          /* 01 */
    case 0: *len = 2; return 3;                                          /* 00: F5 permissive */
    default: return -1;
    }
}

/* Intra AC coefficient block skip. Copies the block's bits (VLCs + signs)
 * from reader to writer. Returns 0 on a complete block, -1 on invalid VLC
 * or overrun. Escape bodies are consumed by length (values ignored). */
static int f5_skip_ac_block(F5BitR *r, F5BitW *w)
{
    for (;;) {
        uint32_t peek, e, v;
        int ln, last;
        if (r->pos + 12 > r->nbits)
            return -1;
        peek = f5br_show(r, 12);
        if (r->err)
            return -1;
        if (peek < 8)
            return -1; /* 9+ leading zeros: no valid AC codeword */
        if (peek < 0x80)
            e = f5ac_l3[peek - 8];
        else if (peek < 0x200)
            e = f5ac_l2[(peek >> 2) - 0x20];
        else
            e = f5ac_l1[(peek >> 5) - 0x10];
        ln = e >> 17;
        v = e & 0x1ffff;
        if (ln > 16 || v == 0x1bff) {
            int t, last, nb;
            uint32_t p2, e2;
            int ln2, v2;
            if (v != 0x1bff || ln > 16)
                return -1;
            /* escape prefix (emitted) */
            f5_copy_bits(r, w, ln);
            if (r->err)
                return -1;
            if (r->pos + 2 > r->nbits)
                return -1;
            /* Peek the 2-bit escape type, then consume+emit exactly the
             * right number of type bits (2 for third/second, 1 for first). */
            t = f5br_show(r, 2);
            if (r->err)
                return -1;
            nb = (t == 3 || t == 2) ? 2 : 1;
            f5_copy_bits(r, w, nb);
            if (r->err)
                return -1;
            if (t == 3) {
                /* third escape: last(1) run(6) marker(1) level(12) marker(1).
                 * F5's encoder sometimes leaves marker bits zero (its own
                 * decoder only consumes them, never checks); force them to 1
                 * so the standard decoder accepts the block. */
                if (r->pos + 1 + 6 + 1 + 12 + 1 > r->nbits)
                    return -1;
                last = f5br_get(r, 1);
                f5bw_put(w, last, 1);
                if (f5_copy_bits(r, w, 6) < 0)   /* run */
                    return -1;
                f5br_get(r, 1);                   /* F5 marker bit (dropped) */
                if (f5bw_put(w, 1, 1) < 0)       /* standard marker bit */
                    return -1;
                if (f5_copy_bits(r, w, 12) < 0)  /* level */
                    return -1;
                f5br_get(r, 1);                   /* F5 marker bit (dropped) */
                if (f5bw_put(w, 1, 1) < 0)
                    return -1;
                if (r->err)
                    return -1;
                if (last)
                    return 0;
                continue;
            }
            /* first (t=0/1) or second (t=2) escape: followup VLC + sign. */
            if (r->pos + 12 > r->nbits)
                return -1;
            p2 = f5br_show(r, 12);
            if (r->err)
                return -1;
            if (p2 < 8)
                return -1;
            if (p2 < 0x80)
                e2 = f5ac_l3[p2 - 8];
            else if (p2 < 0x200)
                e2 = f5ac_l2[(p2 >> 2) - 0x20];
            else
                e2 = f5ac_l1[(p2 >> 5) - 0x10];
            ln2 = e2 >> 17;
            v2 = e2 & 0x1ffff;
            if (ln2 > 16 || v2 == 0x1bff)
                return -1;
            f5_copy_bits(r, w, ln2);
            if (r->err)
                return -1;
            /* sign bit */
            f5_copy_bits(r, w, 1);
            if (r->err)
                return -1;
            if ((v2 >> 16) & 1)
                return 0;
            continue;
        }
        last = (v >> 16) & 1;
        f5_copy_bits(r, w, ln);
        if (r->err)
            return -1;
        f5_copy_bits(r, w, 1); /* sign */
        if (r->err)
            return -1;
        if (last)
            return 0;
    }
}

/* Parse one I-frame MB header (mcbpc + ac_pred + cbpy [+dquant]).
 * Copies header bits to w. Returns cbp (6-bit coded pattern), or -1 on
 * invalid/overrun. *dquant set if a dquant delta was present (value ignored
 * for bit filtering; quant only affects dequant values, not bit layout, at
 * the thresholds used here). */
static int f5_parse_mb_header(F5BitR *r, F5BitW *w, int *dquant)
{
    int cbpc, lenc, cbpy, leny;
    *dquant = 0;
    for (;;) {
        cbpc = f5_vlc_mcbpc_intra(r, &lenc);
        if (cbpc < 0 || r->err)
            return -1;
        f5_copy_bits(r, w, lenc);
        if (r->err)
            return -1;
        if (cbpc != 8)
            break;
    }
    /* ac_pred_flag */
    f5_copy_bits(r, w, 1);
    if (r->err)
        return -1;
    cbpy = f5_vlc_cbpy(r, &leny);
    if (cbpy < 0 || r->err)
        return -1;
    f5_copy_bits(r, w, leny);
    if (r->err)
        return -1;
    if (cbpc & 4) {
        *dquant = 1;
        f5_copy_bits(r, w, 2);
        if (r->err)
            return -1;
    }
    return (cbpc & 3) | (cbpy << 2);
}

/* Parse one intra block's DC (luma if is_luma else chroma), copying its bits.
 * Drops F5's extra bit after sizes >8 (not part of MPEG-4). Returns 0 ok. */
static int f5_filter_dc(F5BitR *r, F5BitW *w, int is_luma)
{
    int sz, len;
    sz = is_luma ? f5_vlc_dc_luma(r, &len) : f5_vlc_dc_chroma(r, &len);
    if (sz < 0 || r->err)
        return -1;
    f5_copy_bits(r, w, len);
    if (r->err)
        return -1;
    if (sz > 0) {
        f5_copy_bits(r, w, sz);
        if (r->err)
            return -1;
    }
    if (sz > 8) {
        /* F5-only extra bit: consume from input, do not emit. */
        if (r->pos + 1 > r->nbits) {
            r->err = 1;
            return -1;
        }
        r->pos += 1;
    }
    return 0;
}

/* Emit a minimal concealment intra MB (no coded blocks, all-DC predicted),
 * keeping the stream at a full frame when the input truncates. 22 bits:
 * mcbpc 0 ('1'), ac_pred off, cbpy 0 ('0011'), 4x luma DC size 0 ('011'),
 * 2x chroma DC size 0 ('11'). */
static int f5_pad_imb(F5BitW *w)
{
    int i, ret;
    if ((ret = f5bw_put(w, 1, 1)) < 0)
        return ret;
    if ((ret = f5bw_put(w, 0, 1)) < 0)
        return ret;
    if ((ret = f5bw_put(w, 3, 4)) < 0)
        return ret;
    for (i = 0; i < 4; i++)
        if ((ret = f5bw_put(w, 3, 3)) < 0)
            return ret;
    for (i = 0; i < 2; i++)
        if ((ret = f5bw_put(w, 3, 2)) < 0)
            return ret;
    return 0;
}

/* Parse+copy one full intra MB (header + 6 blocks). Returns 0 ok, -1 fail.
 * MBs with quant-driven DC skipping are not expected here (quant always
 * below threshold for shipped content); DC is always parsed. */
static int f5_filter_mb(F5BitR *r, F5BitW *w)
{
    int cbp, dq, i;
    cbp = f5_parse_mb_header(r, w, &dq);
    if (cbp < 0 || r->err)
        return -1;
    for (i = 0; i < 6; i++) {
        int coded = (cbp >> 5) & 1;
        cbp = (cbp << 1) & 0x7f;
        if (f5_filter_dc(r, w, i < 4) < 0)
            return -1;
        if (!coded)
            continue;
        if (f5_skip_ac_block(r, w) < 0)
            return -1;
    }
    return r->err ? -1 : 0;
}

/* I-frame MB filter with resync. Copies mb_size input bytes (F5 MB stream)
 * to a clean MPEG-4 MB stream in out (F5BitW, caller frees .buf).
 * Drops DC extra bits; on invalid VLC, skips input to the next position
 * where an MB header plus 3 following MBs validate (max 3000 bits ahead),
 * up to 40 resyncs per frame; then truncates. Always succeeds (possibly
 * with zero MBs); the caller emits what was produced and lets the decoder
 * conceal the rest. I-frames carry no video packet headers (they decode
 * cleanly either way; the VOL enables markers for P recovery). */
static void f5_filter_iframe(const uint8_t *in, int in_size,
                             int mb_w, int mb_h, F5BitW *out)
{
    F5BitR r = { in, in_size * 8, 0, 0 };
    int nmb = mb_w * mb_h, m, resyncs = 0;
    (void)nmb;
    for (m = 0; m < mb_w * mb_h; m++) {
        int save_pos = r.pos;
        size_t save_len = out->len;
        uint32_t save_cache = out->cache;
        int save_nbits = out->nbits;
        F5BitR t;
        int ok, adv, k;
        r.err = 0;
        if (f5_filter_mb(&r, out) == 0)
            continue;
        /* rollback partial MB, then resync-scan */
        out->len = save_len;
        out->cache = save_cache;
        out->nbits = save_nbits;
        ok = 0;
        for (adv = 1; adv <= 3000; adv++) {
            if (save_pos + adv >= r.nbits)
                break;
            t.buf = r.buf;
            t.nbits = r.nbits;
            t.pos = save_pos + adv;
            t.err = 0;
            for (k = 0; k < 4; k++) {
                F5BitW tmp = { NULL, 0, 0, 0, 0 };
                F5BitR c = t;
                if (f5_filter_mb(&c, &tmp) < 0) {
                    av_free(tmp.buf);
                    break;
                }
                av_free(tmp.buf);
                t = c;
            }
            if (k == 4) {
                ok = 1;
                break;
            }
        }
        if (!ok || resyncs >= 40) {
            /* Truncate: pad the rest of the frame with concealment MBs so
             * the decoder still sees a full frame. */
            for (; m < mb_w * mb_h; m++)
                if (f5_pad_imb(out) < 0)
                    break;
            break;
        }
        r.pos = save_pos + adv;
        r.err = 0;
        resyncs++;
        /* Re-parse MB m from the new offset and emit it: dropping it would
         * shorten the stream and shift every later MB into the wrong slot. */
        if (f5_filter_mb(&r, out) == 0)
            continue;
        out->len = save_len;
        out->cache = save_cache;
        out->nbits = save_nbits;
        for (; m < mb_w * mb_h; m++)
            if (f5_pad_imb(out) < 0)
                break;
        break;
    }
    (void)nmb;
}

static const uint32_t f5ac_e1[112] = {
    0x000e1081, 0x000e1071, 0x000e1061, 0x000e1051, 0x000e00c1, 0x000e00b1, 0x000e00a1, 0x000e0004 ,
    0x000c1041, 0x000c1041, 0x000c1031, 0x000c1031, 0x000c1021, 0x000c1021, 0x000c1011, 0x000c1011 ,
    0x000c0091, 0x000c0091, 0x000c0081, 0x000c0081, 0x000c0071, 0x000c0071, 0x000c0061, 0x000c0061 ,
    0x000c0012, 0x000c0012, 0x000c0003, 0x000c0003, 0x000a0051, 0x000a0051, 0x000a0051, 0x000a0051 ,
    0x000a0041, 0x000a0041, 0x000a0041, 0x000a0041, 0x000a0031, 0x000a0031, 0x000a0031, 0x000a0031 ,
    0x00081001, 0x00081001, 0x00081001, 0x00081001, 0x00081001, 0x00081001, 0x00081001, 0x00081001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001, 0x00040001 ,
    0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011 ,
    0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011, 0x00060011 ,
    0x00080021, 0x00080021, 0x00080021, 0x00080021, 0x00080021, 0x00080021, 0x00080021, 0x00080021 ,
    0x00080002, 0x00080002, 0x00080002, 0x00080002, 0x00080002, 0x00080002, 0x00080002, 0x00080002 ,
};
static const uint32_t f5ac_e2[96] = {
    0x00140009, 0x00140008, 0x00121181, 0x00121181, 0x00121171, 0x00121171, 0x00121161, 0x00121161 ,
    0x00121151, 0x00121151, 0x00121141, 0x00121141, 0x00121131, 0x00121131, 0x00121121, 0x00121121 ,
    0x00121111, 0x00121111, 0x00121002, 0x00121002, 0x00120161, 0x00120161, 0x00120151, 0x00120151 ,
    0x00120141, 0x00120141, 0x00120131, 0x00120131, 0x00120121, 0x00120121, 0x00120111, 0x00120111 ,
    0x00120101, 0x00120101, 0x001200f1, 0x001200f1, 0x00120042, 0x00120042, 0x00120032, 0x00120032 ,
    0x00120007, 0x00120007, 0x00120006, 0x00120006, 0x00101101, 0x00101101, 0x00101101, 0x00101101 ,
    0x001010f1, 0x001010f1, 0x001010f1, 0x001010f1, 0x001010e1, 0x001010e1, 0x001010e1, 0x001010e1 ,
    0x001010d1, 0x001010d1, 0x001010d1, 0x001010d1, 0x001010c1, 0x001010c1, 0x001010c1, 0x001010c1 ,
    0x001010b1, 0x001010b1, 0x001010b1, 0x001010b1, 0x001010a1, 0x001010a1, 0x001010a1, 0x001010a1 ,
    0x00101091, 0x00101091, 0x00101091, 0x00101091, 0x001000e1, 0x001000e1, 0x001000e1, 0x001000e1 ,
    0x001000d1, 0x001000d1, 0x001000d1, 0x001000d1, 0x00100022, 0x00100022, 0x00100022, 0x00100022 ,
    0x00100013, 0x00100013, 0x00100013, 0x00100013, 0x00100005, 0x00100005, 0x00100005, 0x00100005 ,
};
static const uint32_t f5ac_e3[120] = {
    0x00161012, 0x00161012, 0x00161003, 0x00161003, 0x0016000b, 0x0016000b, 0x0016000a, 0x0016000a ,
    0x001411c1, 0x001411c1, 0x001411c1, 0x001411c1, 0x001411b1, 0x001411b1, 0x001411b1, 0x001411b1 ,
    0x001411a1, 0x001411a1, 0x001411a1, 0x001411a1, 0x00141191, 0x00141191, 0x00141191, 0x00141191 ,
    0x00140092, 0x00140092, 0x00140092, 0x00140092, 0x00140082, 0x00140082, 0x00140082, 0x00140082 ,
    0x00140072, 0x00140072, 0x00140072, 0x00140072, 0x00140062, 0x00140062, 0x00140062, 0x00140062 ,
    0x00140052, 0x00140052, 0x00140052, 0x00140052, 0x00140033, 0x00140033, 0x00140033, 0x00140033 ,
    0x00140023, 0x00140023, 0x00140023, 0x00140023, 0x00140014, 0x00140014, 0x00140014, 0x00140014 ,
    0x0016000c, 0x0016000c, 0x00160015, 0x00160015, 0x00160171, 0x00160171, 0x00160181, 0x00160181 ,
    0x001611d1, 0x001611d1, 0x001611e1, 0x001611e1, 0x001611f1, 0x001611f1, 0x00161201, 0x00161201 ,
    0x00180016, 0x00180024, 0x00180043, 0x00180053, 0x00180063, 0x001800a2, 0x00180191, 0x001801a1 ,
    0x00181211, 0x00181221, 0x00181231, 0x00181241, 0x00181251, 0x00181261, 0x00181271, 0x00181281 ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
    0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff, 0x000e1bff ,
};
static const uint32_t f5mv_m1[14] = {
    0x00080003, 0x0009fffd, 0x00060002, 0x00060002, 0x0007fffe, 0x0007fffe, 0x00040001, 0x00040001 ,
    0x00040001, 0x00040001, 0x0005ffff, 0x0005ffff, 0x0005ffff, 0x0005ffff ,
};
static const uint32_t f5mv_m2[96] = {
    0x0014000c, 0x0015fff4, 0x0014000b, 0x0015fff5, 0x0012000a, 0x0012000a, 0x0013fff6, 0x0013fff6 ,
    0x00120009, 0x00120009, 0x0013fff7, 0x0013fff7, 0x00120008, 0x00120008, 0x0013fff8, 0x0013fff8 ,
    0x000e0007, 0x000e0007, 0x000e0007, 0x000e0007, 0x000e0007, 0x000e0007, 0x000e0007, 0x000e0007 ,
    0x000ffff9, 0x000ffff9, 0x000ffff9, 0x000ffff9, 0x000ffff9, 0x000ffff9, 0x000ffff9, 0x000ffff9 ,
    0x000e0006, 0x000e0006, 0x000e0006, 0x000e0006, 0x000e0006, 0x000e0006, 0x000e0006, 0x000e0006 ,
    0x000ffffa, 0x000ffffa, 0x000ffffa, 0x000ffffa, 0x000ffffa, 0x000ffffa, 0x000ffffa, 0x000ffffa ,
    0x000e0005, 0x000e0005, 0x000e0005, 0x000e0005, 0x000e0005, 0x000e0005, 0x000e0005, 0x000e0005 ,
    0x000ffffb, 0x000ffffb, 0x000ffffb, 0x000ffffb, 0x000ffffb, 0x000ffffb, 0x000ffffb, 0x000ffffb ,
    0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004 ,
    0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004, 0x000c0004 ,
    0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc ,
    0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc, 0x000dfffc ,
};
static const uint32_t f5mv_m3[124] = {
    0x00180020, 0x0019ffe0, 0x0018001f, 0x0019ffe1, 0x0016001e, 0x0016001e, 0x0017ffe2, 0x0017ffe2 ,
    0x0016001d, 0x0016001d, 0x0017ffe3, 0x0017ffe3, 0x0016001c, 0x0016001c, 0x0017ffe4, 0x0017ffe4 ,
    0x0016001b, 0x0016001b, 0x0017ffe5, 0x0017ffe5, 0x0016001a, 0x0016001a, 0x0017ffe6, 0x0017ffe6 ,
    0x00160019, 0x00160019, 0x0017ffe7, 0x0017ffe7, 0x00140018, 0x00140018, 0x00140018, 0x00140018 ,
    0x0015ffe8, 0x0015ffe8, 0x0015ffe8, 0x0015ffe8, 0x00140017, 0x00140017, 0x00140017, 0x00140017 ,
    0x0015ffe9, 0x0015ffe9, 0x0015ffe9, 0x0015ffe9, 0x00140016, 0x00140016, 0x00140016, 0x00140016 ,
    0x0015ffea, 0x0015ffea, 0x0015ffea, 0x0015ffea, 0x00140015, 0x00140015, 0x00140015, 0x00140015 ,
    0x0015ffeb, 0x0015ffeb, 0x0015ffeb, 0x0015ffeb, 0x00140014, 0x00140014, 0x00140014, 0x00140014 ,
    0x0015ffec, 0x0015ffec, 0x0015ffec, 0x0015ffec, 0x00140013, 0x00140013, 0x00140013, 0x00140013 ,
    0x0015ffed, 0x0015ffed, 0x0015ffed, 0x0015ffed, 0x00140012, 0x00140012, 0x00140012, 0x00140012 ,
    0x0015ffee, 0x0015ffee, 0x0015ffee, 0x0015ffee, 0x00140011, 0x00140011, 0x00140011, 0x00140011 ,
    0x0015ffef, 0x0015ffef, 0x0015ffef, 0x0015ffef, 0x00140010, 0x00140010, 0x00140010, 0x00140010 ,
    0x0015fff0, 0x0015fff0, 0x0015fff0, 0x0015fff0, 0x0014000f, 0x0014000f, 0x0014000f, 0x0014000f ,
    0x0015fff1, 0x0015fff1, 0x0015fff1, 0x0015fff1, 0x0014000e, 0x0014000e, 0x0014000e, 0x0014000e ,
    0x0015fff2, 0x0015fff2, 0x0015fff2, 0x0015fff2, 0x0014000d, 0x0014000d, 0x0014000d, 0x0014000d ,
    0x0015fff3, 0x0015fff3, 0x0015fff3, 0x0015fff3 ,
};

/* ---- F5 P-frame MB transcode ----
 * Parses P-frame MBs with F5's exact grammar and emits standard MPEG-4
 * P-MB bits. P-mcbpc codewords are bit-identical to H.263 inter MCBPC
 * (verified); only motion uses F5's custom signed VLC, remapped here to
 * standard magnitude+sign+residual. Resync driver mirrors the I path.
 */

/* P-MCBPC VLC (peek-only). Returns type 0..4, or 8 for 9-bit stuffing,
 * or -1. *len = codeword bits, *extra = chroma-cbp bits (0..3). */
static int f5_vlc_mcbpc_p(F5BitR *r, int *len, int *extra)
{
    uint32_t p;
    if (r->pos + 9 > r->nbits)
        return -1;
    p = f5br_show(r, 9);
    if (r->err)
        return -1;
    if (p == 1) { *len = 9; *extra = 0; return 8; }                  /* stuffing */
    if ((p >> 8) & 1) { *len = 1; *extra = 0; return 0; }            /* 1 */
    switch ((p >> 6) & 7) {
    case 2: *len = 3; *extra = 0; return 2;                          /* 010 */
    case 3: *len = 3; *extra = 0; return 1;                          /* 011 */
    default: break;
    }
    switch ((p >> 5) & 15) {
    case 2: *len = 4; *extra = 2; return 0;                          /* 0010 */
    case 3: *len = 4; *extra = 1; return 0;                          /* 0011 */
    default: break;
    }
    if (((p >> 4) & 31) == 3) { *len = 5; *extra = 0; return 3; }    /* 00011 */
    switch ((p >> 3) & 63) {
    case 4: *len = 6; *extra = 0; return 4;                          /* 000100 */
    case 5: *len = 6; *extra = 3; return 0;                          /* 000101 */
    default: break;
    }
    switch ((p >> 2) & 127) {
    case  3: *len = 7; *extra = 3; return 3;                         /* 0000011 */
    case  4: *len = 7; *extra = 2; return 2;                         /* 0000100 */
    case  5: *len = 7; *extra = 1; return 2;                         /* 0000101 */
    case  6: *len = 7; *extra = 2; return 1;                         /* 0000110 */
    case  7: *len = 7; *extra = 1; return 1;                         /* 0000111 */
    default: break;
    }
    switch ((p >> 1) & 255) {
    case  3: *len = 8; *extra = 2; return 3;                         /* 00000011 */
    case  4: *len = 8; *extra = 1; return 3;                         /* 00000100 */
    case  5: *len = 8; *extra = 3; return 2;                         /* 00000101 */
    default: break;
    }
    switch (p & 511) {
    case  2: *len = 9; *extra = 3; return 4;                         /* 000000010 */
    case  3: *len = 9; *extra = 2; return 4;                         /* 000000011 */
    case  4: *len = 9; *extra = 1; return 4;                         /* 000000100 */
    case  5: *len = 9; *extra = 3; return 1;                         /* 000000101 */
    default: return -1;
    }
}

/* Standard H.263 motion magnitude -> (code, len). Index = magnitude 0..32
 * (0 = zero vector '1'). From ff_mvtab. */
static const uint8_t f5mv_std[33][2] = {
    { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 3, 6 }, { 5, 7 },
    { 4, 7 }, { 3, 7 }, { 11, 9 }, { 10, 9 }, { 9, 9 }, { 17, 10 },
    { 16, 10 }, { 15, 10 }, { 14, 10 }, { 13, 10 }, { 12, 10 },
    { 11, 10 }, { 10, 10 }, { 9, 10 }, { 8, 10 }, { 7, 10 },
    { 6, 10 }, { 5, 10 }, { 4, 10 }, { 7, 11 }, { 6, 11 },
    { 5, 11 }, { 4, 11 }, { 3, 11 }, { 2, 11 }, { 3, 12 }, { 2, 12 },
};

/* Parse one F5 motion value, emit standard (mag code + sign + residual).
 * reslen = residual bits (fcode-1). Returns 0 ok, -1 on invalid/overrun. */
static int f5_motion_remap(F5BitR *r, F5BitW *w, int reslen)
{
    uint32_t pk, e;
    int ln, v, m, code, clen, i;
    uint32_t bit;
    if (r->pos + 1 > r->nbits)
        return -1;
    bit = f5br_get(r, 1);
    if (r->err)
        return -1;
    if (bit) {
        /* zero vector: identical '1' in both grammars (already consumed) */
        if (f5bw_put(w, 1, 1) < 0)
            return -1;
        return r->err ? -1 : 0;
    }
    if (r->pos + 12 > r->nbits)
        return -1;
    pk = f5br_show(r, 12);
    if (r->err)
        return -1;
    if (pk < 4)
        return -1;
    if (pk < 0x80)
        e = f5mv_m3[pk - 4];
    else if (pk < 0x200)
        e = f5mv_m2[(pk >> 2) - 0x20];
    else
        e = f5mv_m1[(pk >> 8) - 2];
    ln = e >> 17;
    v = (int)(e & 0xFFFF);
    if (v & 0x8000)
        v -= 0x10000;
    if (ln > 16 || ln <= 0 || v == 0)
        return -1;
    r->pos += ln; /* consume F5 codeword (not emitted) */
    m = v < 0 ? -v : v;
    if (m > 32)
        return -1;
    if (reslen > 0) {
        if (r->pos + reslen > r->nbits)
            return -1;
    }
    code = f5mv_std[m][0];
    clen = f5mv_std[m][1];
    if (f5bw_put(w, code, clen) < 0)
        return -1;
    if (f5bw_put(w, v < 0 ? 1 : 0, 1) < 0)
        return -1;
    for (i = 0; i < reslen; i++) {
        int b;
        if (r->pos + 1 > r->nbits)
            return -1;
        b = f5br_get(r, 1);
        if (r->err)
            return -1;
        if (f5bw_put(w, b, 1) < 0)
            return -1;
    }
    return 0;
}

/* Inter AC block copy (E-tables, last flag at bit 12). Mirrors
 * f5_skip_ac_block but for the inter packing. Returns 0 ok, -1 fail. */
static int f5_skip_ac_block_inter(F5BitR *r, F5BitW *w)
{
    for (;;) {
        uint32_t peek, e, v;
        int ln, last;
        if (r->pos + 12 > r->nbits)
            return -1;
        peek = f5br_show(r, 12);
        if (r->err)
            return -1;
        if (peek < 8)
            return -1; /* 9+ leading zeros: no valid AC codeword */
        if (peek < 0x80)
            e = f5ac_e3[peek - 8];
        else if (peek < 0x200)
            e = f5ac_e2[(peek >> 2) - 0x20];
        else
            e = f5ac_e1[(peek >> 5) - 0x10];
        ln = e >> 17;
        v = e & 0x1ffff;
        if (ln > 16 || v == 0x1bff) {
            int t, nb;
            uint32_t p2, e2;
            int ln2, v2;
            if (v != 0x1bff || ln > 16)
                return -1;
            f5_copy_bits(r, w, ln);
            if (r->err)
                return -1;
            if (r->pos + 2 > r->nbits)
                return -1;
            t = f5br_show(r, 2);
            if (r->err)
                return -1;
            nb = (t == 3 || t == 2) ? 2 : 1;
            f5_copy_bits(r, w, nb);
            if (r->err)
                return -1;
            if (t == 3) {
                /* third escape: last(1) run(6) marker(1) level(12) marker(1).
                 * F5's encoder sometimes leaves marker bits zero (its own
                 * decoder only consumes them, never checks); force them to 1
                 * so the standard decoder accepts the block. */
                if (r->pos + 1 + 6 + 1 + 12 + 1 > r->nbits)
                    return -1;
                last = f5br_get(r, 1);
                f5bw_put(w, last, 1);
                if (f5_copy_bits(r, w, 6) < 0)   /* run */
                    return -1;
                f5br_get(r, 1);                   /* F5 marker bit (dropped) */
                if (f5bw_put(w, 1, 1) < 0)       /* standard marker bit */
                    return -1;
                if (f5_copy_bits(r, w, 12) < 0)  /* level */
                    return -1;
                f5br_get(r, 1);                   /* F5 marker bit (dropped) */
                if (f5bw_put(w, 1, 1) < 0)
                    return -1;
                if (r->err)
                    return -1;
                if (last)
                    return 0;
                continue;
            }
            if (r->pos + 12 > r->nbits)
                return -1;
            p2 = f5br_show(r, 12);
            if (r->err)
                return -1;
            if (p2 < 8)
                return -1;
            if (p2 < 0x80)
                e2 = f5ac_e3[p2 - 8];
            else if (p2 < 0x200)
                e2 = f5ac_e2[(p2 >> 2) - 0x20];
            else
                e2 = f5ac_e1[(p2 >> 5) - 0x10];
            ln2 = e2 >> 17;
            v2 = e2 & 0x1ffff;
            if (ln2 > 16 || v2 == 0x1bff)
                return -1;
            f5_copy_bits(r, w, ln2);
            if (r->err)
                return -1;
            f5_copy_bits(r, w, 1);
            if (r->err)
                return -1;
            if ((v2 >> 12) & 1)
                return 0;
            continue;
        }
        last = (v >> 12) & 1;
        f5_copy_bits(r, w, ln);
        if (r->err)
            return -1;
        f5_copy_bits(r, w, 1); /* sign */
        if (r->err)
            return -1;
        if (last)
            return 0;
    }
}

/* Transcode one P-frame MB (F5 in -> standard MPEG-4 out).
 * reslen = motion residual bits (fcode-1). Returns 0 ok, -1 fail. */
static int f5_filter_pmb(F5BitR *r, F5BitW *w, int reslen)
{
    int skip, typ, extra, len, cbpy, leny, cbp, i;
    if (r->pos + 1 > r->nbits)
        return -1;
    skip = f5br_get(r, 1);
    if (r->err)
        return -1;
    if (f5bw_put(w, skip, 1) < 0) /* COD: 1=skipped, same as F5 */
        return -1;
    if (skip)
        return 0; /* skipped MB: nothing else coded */
    for (;;) {
        typ = f5_vlc_mcbpc_p(r, &len, &extra);
        if (typ < 0 || r->err)
            return -1;
        f5_copy_bits(r, w, len);
        if (r->err)
            return -1;
        if (typ != 8)
            break;
    }
    if (typ == 8)
        return -1; /* unreachable: loop above retries stuffing */
    if (typ == 3 || typ == 4) {
        /* intra path (same as I): ac_pred + cbpy + dquant(iff 4) + blocks */
        f5_copy_bits(r, w, 1);
        if (r->err)
            return -1;
        cbpy = f5_vlc_cbpy(r, &leny);
        if (cbpy < 0 || r->err)
            return -1;
        f5_copy_bits(r, w, leny);
        if (r->err)
            return -1;
        if (typ == 4) {
            f5_copy_bits(r, w, 2);
            if (r->err)
                return -1;
        }
        cbp = (cbpy << 2) | extra;
        for (i = 0; i < 6; i++) {
            int coded = (cbp >> 5) & 1;
            cbp = (cbp << 1) & 0x7f;
            if (f5_filter_dc(r, w, i < 4) < 0)
                return -1;
            if (!coded)
                continue;
            if (f5_skip_ac_block(r, w) < 0)
                return -1;
        }
        return r->err ? -1 : 0;
    }
    /* inter path: cbpy, dquant(iff type 1), motion, inter blocks */
    cbpy = f5_vlc_cbpy(r, &leny);
    if (cbpy < 0 || r->err)
        return -1;
    f5_copy_bits(r, w, leny);
    if (r->err)
        return -1;
    if (typ == 1) {
        f5_copy_bits(r, w, 2);
        if (r->err)
            return -1;
    }
    cbp = (((15 - cbpy) & 15) << 2) | extra;
    {
        int nmv = (typ == 2) ? 4 : 1;
        for (i = 0; i < nmv; i++) {
            if (f5_motion_remap(r, w, reslen) < 0)
                return -1;
            if (f5_motion_remap(r, w, reslen) < 0)
                return -1;
        }
    }
    for (i = 0; i < 6; i++) {
        int coded = (cbp >> 5) & 1;
        cbp = (cbp << 1) & 0x7f;
        if (!coded)
            continue;
        if (f5_skip_ac_block_inter(r, w) < 0)
            return -1;
    }
    return r->err ? -1 : 0;
}

#define F5_PRESYNC_WEAK    4

/* P-frame MB filter with resync. Mirrors f5_filter_iframe but transcodes
 * P-MBs (motion remap with reslen residual bits). */
static void f5_filter_pframe(const uint8_t *in, int in_size,
                             int mb_w, int mb_h, int reslen, F5BitW *out)
{
    F5BitR r = { in, in_size * 8, 0, 0 };
    int m, resyncs = 0;
    for (m = 0; m < mb_w * mb_h; m++) {
        int save_pos = r.pos;
        size_t save_len = out->len;
        uint32_t save_cache = out->cache;
        int save_nbits = out->nbits;
        F5BitR t;
        int ok, adv, k;
        r.err = 0;
        if (f5_filter_pmb(&r, out, reslen) == 0)
            continue;
        out->len = save_len;
        out->cache = save_cache;
        out->nbits = save_nbits;
        ok = 0;
        for (adv = 1; adv <= 3000; adv++) {
            if (save_pos + adv >= r.nbits)
                break;
            t.buf = r.buf;
            t.nbits = r.nbits;
            t.pos = save_pos + adv;
            t.err = 0;
            for (k = 0; k < F5_PRESYNC_WEAK; k++) {
                F5BitW tmp = { NULL, 0, 0, 0, 0 };
                F5BitR c = t;
                if (f5_filter_pmb(&c, &tmp, reslen) < 0) {
                    av_free(tmp.buf);
                    break;
                }
                av_free(tmp.buf);
                t = c;
            }
            if (k == F5_PRESYNC_WEAK) {
                ok = 1;
                break;
            }
        }
        if (!ok || resyncs >= 40) {
            /* Truncate: pad the rest of the frame with skipped MBs so the
             * decoder still sees a full frame (a short stream makes it
             * overread into padding). Skipped P-MBs copy the reference. */
            for (; m < mb_w * mb_h; m++)
                if (f5bw_put(out, 1, 1) < 0)
                    break;
            break;
        }
        r.pos = save_pos + adv;
        r.err = 0;
        resyncs++;
        /* Re-parse MB m from the new offset and emit it: dropping it would
         * shorten the stream and shift every later MB into the wrong slot.
         * Validated above, so this succeeds barring OOM. */
        if (f5_filter_pmb(&r, out, reslen) == 0)
            continue;
        out->len = save_len;
        out->cache = save_cache;
        out->nbits = save_nbits;
        for (; m < mb_w * mb_h; m++)
            if (f5bw_put(out, 1, 1) < 0)
                break;
        break;
    }
}

/* Synthesize a VOP header (verid=1, rectangular, progressive). type: 0=I,1=P.
 * intra_dc_threshold index 0 (=99, forces intra DC VLC on). tinc is absolute
 * (60000 Hz); emitted as modulo_time_base + remainder. rounding is the P-VOP
 * no_rounding bit (from the container coded bit). */
static int f5vid_build_vop(F5BitW *w, int type, uint32_t tinc, int nbits,
                           int quant, int fwd, int bwd, int rounding,
                           int *hdr_bits)
{
    int ret, start;
    uint32_t tb, trem;
    if ((ret = f5bw_put(w, 0x000001B6, 32)) < 0) return ret; /* vop_start_code */
    start = w->len * 8 + w->nbits; /* == 32 */
    if ((ret = f5bw_put(w, type, 2)) < 0) return ret; /* vop_coding_type */
    tb = (F5VID_TIME_RES > 1) ? tinc / F5VID_TIME_RES : 0;
    trem = (F5VID_TIME_RES > 1) ? tinc % F5VID_TIME_RES : tinc;
    if (tb > 64)
        tb = 64; /* sanity cap; container ts stays well below this */
    for (uint32_t i = 0; i < tb; i++)
        if ((ret = f5bw_put(w, 1, 1)) < 0) return ret;
    if ((ret = f5bw_put(w, 0, 1))  < 0) return ret; /* modulo_time_base end */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, trem, nbits)) < 0) return ret;
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* marker */
    if ((ret = f5bw_put(w, 1, 1))  < 0) return ret; /* vop_coded */
    if (type == 1 && (ret = f5bw_put(w, rounding, 1)) < 0) return ret; /* vop_rounding_type */
    if ((ret = f5bw_put(w, 0, 3))  < 0) return ret; /* intra_dc_threshold idx 0 */
    if ((ret = f5bw_put(w, quant, 5)) < 0) return ret; /* vop_quant */
    if (type != 0 && (ret = f5bw_put(w, fwd, 3)) < 0) return ret;
    if (type == 2 && (ret = f5bw_put(w, bwd, 3)) < 0) return ret;
    *hdr_bits = (w->len * 8 + w->nbits) - start;
    return 0; /* NOTE: no flush here; MB data follows at bit granularity.
               * Bit-exactness with the byte-aligned MB payload is handled
               * by the caller (see f5vid_mpeg4_filter). */
}

/* ---- bitstream filter glue ---- */

typedef struct F5VidMpeg4Context {
    int mb_w, mb_h;
} F5VidMpeg4Context;

static int f5vid_mpeg4_init(AVBSFContext *ctx)
{
    F5VidMpeg4Context *s = ctx->priv_data;
    F5BitW vw = { NULL, 0, 0, 0, 0 };
    int ret;
    int width  = ctx->par_in->width;
    int height = ctx->par_in->height;
    if (width <= 0 || height <= 0) {
        av_log(ctx, AV_LOG_ERROR, "f5vid_mpeg4: missing width/height\n");
        return AVERROR_INVALIDDATA;
    }
    s->mb_w = (width + 15) / 16;
    s->mb_h = (height + 15) / 16;
    ctx->par_out->codec_id = AV_CODEC_ID_MPEG4;
    if ((ret = f5vid_build_vol(&vw, width, height, F5VID_TIME_RES)) < 0) {
        av_free(vw.buf);
        return ret;
    }
    av_freep(&ctx->par_out->extradata);
    ctx->par_out->extradata      = vw.buf;
    ctx->par_out->extradata_size = vw.len;
    return 0;
}

/* Reads the 8-byte F5 metadata header + raw F5 MB payload the demuxer
 * packetized, and writes a clean MPEG-4 VOP packet (start code + header
 * bit-spliced with the transcoded MB data). */
static int f5vid_mpeg4_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    F5VidMpeg4Context *s = ctx->priv_data;
    AVPacket *in;
    const uint8_t *mbdata;
    uint8_t *mb_out = NULL, *out = NULL;
    int mb_size, mb_out_size;
    int vop_type, quant, fwd, rounding;
    int hdr_bytes, hdr_bits, out_size;
    F5BitW fw = { NULL, 0, 0, 0, 0 };
    F5BitW vw = { NULL, 0, 0, 0, 0 };
    int ret;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    if (in->size < 8) {
        av_log(ctx, AV_LOG_ERROR, "f5vid_mpeg4: packet too small\n");
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    vop_type = in->data[0];
    quant    = in->data[1];
    fwd      = in->data[2];
    rounding = in->data[3];
    mbdata   = in->data + 8;
    mb_size  = in->size - 8;

    if (vop_type == 0) {
        f5_filter_iframe(mbdata, mb_size, s->mb_w, s->mb_h, &fw);
    } else {
        int reslen = fwd > 1 ? fwd - 1 : 0;
        f5_filter_pframe(mbdata, mb_size, s->mb_w, s->mb_h, reslen, &fw);
    }
    if ((ret = f5bw_flush(&fw)) < 0)
        goto fail;
    mb_out = fw.buf;
    mb_out_size = fw.len;
    fw.buf = NULL;

    if ((ret = f5vid_build_vop(&vw, vop_type, (uint32_t)in->pts,
                               F5VID_TINC_BITS, quant, fwd, 2, rounding,
                               &hdr_bits)) < 0)
        goto fail;

    {
        int hdr_full_bytes = hdr_bits / 8;
        int hdr_rem_bits = hdr_bits % 8;
        hdr_bytes = 4 + hdr_full_bytes;
        out_size = hdr_bytes + mb_out_size + 1;
        out = av_malloc(out_size);
        if (!out) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        memcpy(out, vw.buf, hdr_bytes);
        if (hdr_rem_bits) {
            /* bit-splice: remaining header bits then MB bits */
            int i;
            uint8_t *dst = out + hdr_bytes;
            int dst_size = mb_out_size + 1;
            uint32_t acc;
            int acc_bits, dst_len = 0;
            av_assert0(vw.nbits == hdr_rem_bits);
            acc = vw.nbits ? (vw.cache & ((1u << vw.nbits) - 1)) : 0;
            acc_bits = vw.nbits;
            for (i = 0; i < mb_out_size; i++) {
                acc = (acc << 8) | mb_out[i];
                acc_bits += 8;
                while (acc_bits >= 8) {
                    acc_bits -= 8;
                    if (dst_len < dst_size)
                        dst[dst_len++] = (acc >> acc_bits) & 0xFF;
                }
            }
            if (acc_bits > 0 && dst_len < dst_size)
                dst[dst_len++] = (acc << (8 - acc_bits)) & 0xFF;
            out_size = hdr_bytes + dst_len;
        } else {
            memcpy(out + hdr_bytes, mb_out, mb_out_size);
            out_size = hdr_bytes + mb_out_size;
        }
    }

    {
        uint8_t *final_buf = out;
        ret = av_packet_from_data(pkt, final_buf, out_size);
        if (ret < 0) {
            av_free(final_buf);
            goto fail;
        }
        out = NULL; /* ownership moved into pkt */
    }
    ret = av_packet_copy_props(pkt, in);
    if (ret < 0) {
        av_packet_unref(pkt);
        goto fail;
    }
    if (vop_type == 0)
        pkt->flags |= AV_PKT_FLAG_KEY;

    ret = 0;
fail:
    av_free(mb_out);
    av_free(vw.buf);
    av_free(fw.buf);
    av_free(out);
    av_packet_free(&in);
    return ret;
}

static const enum AVCodecID f5vid_mpeg4_codec_ids[] = {
    AV_CODEC_ID_MPEG4, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_f5vid_mpeg4_bsf = {
    .p.name         = "f5vid_mpeg4",
    .p.codec_ids    = f5vid_mpeg4_codec_ids,
    .priv_data_size = sizeof(F5VidMpeg4Context),
    .init           = f5vid_mpeg4_init,
    .filter         = f5vid_mpeg4_filter,
};
