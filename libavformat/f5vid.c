/*
 * Factor 5 DivX .vid demuxer (Carmen Sandiego: The Secret of the Stolen Drums, GC)
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 *
 * Container (all integers big-endian, all chunk lengths include the 8-byte
 * tag+length header itself):
 *
 *   VID1 len 0x20: 24 bytes payload (constant 00 00 00 00 01 00 00 13 ...).
 *   HEAD len 0xa0: u32 0, then VIDH (0x20) + AUDH (0x60) + 20 bytes zeros.
 *     VIDH payload (24B): u32 1 | u16 w | u16 h | u32 frame_count
 *                         | u32 max_FRAM_len | u32 fps_num | u16 fps_den | u16 0
 *       e.g. 640x480 display, 2997/100 (29.97 fps). The coded video is the
 *       full VIDH size (640x480 MPEG-4 ASP, DivX 5.02, 40x30 MBs).
  *     AUDH payload (88B): u32 0 | "APCM" | u32 rate_BE (32000)
  *                         | u16 channels_LE (2) | 74 bytes footer.
  *       In Demo/A2M the first 64 footer bytes are nonzero (purpose
  *       unknown — they are NOT the DSP coef table: feeding them as one
  *       clips the decoder to full scale; they are not 8-byte-aligned
  *       audio either); Bam/LS/River carry zeros.
 *   FRAM len variable: 8-byte header + 24 bytes zeros, then VIDD + AUDD.
 *     VIDD: 8-byte header + 12-byte inner header + DivX video MB data.
 *       inner: 00 00 00 00 | 00 01 code sub | 32-bit timestamp (30000 Hz,
 *       1001 ticks per frame at 29.97 fps). The timestamp is NOT always
 *       byte-aligned: the M4 frame header is packed, and a P-VOP's fields
 *       stop one bit short of a byte, so its timestamp begins at inner bit
 *       63 rather than 64 (see f5vid_inner_ts).
 *       The inner header is a packed bit header (see M4BitstreamParser::
 *       parseHeader in the game binary): after the 16-bit version (=1):
 *       type = code>>6 (0=I,1=P), X = (code>>5)&1, then if X: flag/acdc/dp/
 *       ext/coded bits, coded/f/quant fields and (for P) forward fcode, all
 *       continued into sub and (for P with X=0) the timestamp byte. Decoded:
 *       I (0x20, X=1): quant = sub&31 (2-9), f_idx = sub>>5 (always 0).
 *       P (0x40/0x50, X=0): coded = code bit4 (rounding: 0 for 0x40, 1 for
 *       0x50), quant = sub>>4 (2-10), fwd fcode = (sub>>1)&7 (2-4).
 *       MB data starts immediately after inner (no prologue skip, no byte
 *       transform; bytes are fed to the bit reader MSB-first as stored).
 *       The DivX VOP headers (VOL/VOP start codes, type, time, quant, fcode)
 *       are stripped by the container; the demuxer re-synthesizes a minimal
 *       VOL (as extradata, 640x480) and per-packet VOP headers so the native
 *       mpeg4 decoder accepts the stream. use_intra_dc_vlc is forced on;
 *       time_res = 60000 to match container ts.
 *       I-frame MB data uses standard MPEG-4 VLCs/tables (verified against
  *       the game binary's M4 tables) with one deviation: after an intra DC
  *       with size>8, the encoder emits 1 extra bit which plain MPEG-4 does
  *       not have; the demuxer drops it. A resync scan (skip to the next
  *       valid MB) guards against damage, but every shipped frame parses
  *       cleanly without it.
 *       KNOWN GAPS: P-frame motion uses a custom signed VLC, remapped here
 *       to standard magnitude+sign+residual (residual length fwd-1);
 *       B/GMC types (2/3) are asserted unsupported (absent from all
 *       shipped files).
  *     AUDD: 8-byte header + 8-byte inner header + audio data.
  *       inner: 00 00 00 00 | BE32 (AUDD_len - 32).
  *       "APCM" audio at 32000 Hz stereo, ~37 kB/s: DSP-ADPCM with 8-byte
  *       frames (predictor/scale header + 14 nibbles, THP-compatible framing;
  *       14 samples/8 bytes = 4.57 bits/sample = 36.6 kB/s stereo @32kHz).
  *       Frames are interleaved L,R,L,R...; the demuxer deinterleaves to
  *       channel-major for the adpcm_thp decoder. The per-file coef table
  *       is not present (AUDH's 64 nonzero bytes are something else — see
  *       above), so the demuxer exposes a zero table (memoryless decode:
  *       plausible level and duration, harsh spectrum). TODO: identify the
  *       footer bytes (game binary audio decoder) for full quality.
  *
  *   Timing: file order is display order and the container clock is strictly
  *   increasing once P-VOP timestamps are unpacked from the right bit offset,
  *   so packet pts/dts follow it (doubled into a 60000 Hz stream timebase,
  *   2002 ticks = 1 frame at 29.97 fps). A file whose clock ever went
  *   backwards would fall back to a synthesized one-frame advance.
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
#include "config_components.h"
#include "demux.h"
#include "internal.h"
#include "mux.h"

#include "avformat.h"

typedef struct F5VIDIndex {
    int64_t pos;        /* file offset of FRAM tag */
    uint32_t video_ts;  /* container ts, F5VID_TIME_RES units */
    uint32_t audio_size;/* AUDD payload bytes in this FRAM */
    int is_key;         /* VIDD code 0x20 (I) */
} F5VIDIndex;

typedef struct F5VIDDemuxContext {
    int current_frame;
    int frame_count;
    int handle_audio_packet;
    uint32_t audio_size;
    int64_t audio_sample_pos;
    int64_t next_fram_pos; /* file offset of next FRAM (after current AUDD) */
    int64_t video_dts; /* decode-order clock (monotonic; pts may jump) */
    F5VIDIndex *index;
    int nb_index;
} F5VIDDemuxContext;

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

#if CONFIG_F5VID_DEMUXER

/* Synthesize a minimal MPEG-4 VOL (Simple, rectangular, progressive,
 * H.263 quant, no resync/partition/sprite). Matches the proven graft VOL. */
#define F5VID_TIME_RES 60000
#define F5VID_TINC_BITS 16 /* bit_length(60000-1) */
static int f5vid_build_vol(F5BitW *w, int width, int height, uint32_t time_res)
{
    int ret;
    if ((ret = f5bw_put(w, 0x00000120, 32)) < 0) return ret; /* vol_start_code */
    /* NOTE: start code written raw above; following puts are bit-exact */
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

#endif /* CONFIG_F5VID_DEMUXER */

/* ---- F5 I-frame MB bit filter ----
 * Parses I-frame MB data with F5's exact VLC grammar (all tables verified
 * against the game binary; they match MPEG-4) and emits a clean MPEG-4 MB
 * stream: identical bits except (a) the 1 extra bit F5 emits after an intra
 * DC with size>8 is dropped, and (b) on invalid VLC the filter resyncs
 * (skips input bits to the next valid MB) or truncates. P-frames go
 * through the P transcode below (best-effort; see KNOWN GAPS above).
 */

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
    /* skip 9-bit stuffing words when peeking: caller handles via loop;
     * here report stuffing as value 8 (caller consumes 9 and retries). */
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

#if CONFIG_F5VID_DEMUXER

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

#endif /* CONFIG_F5VID_DEMUXER */

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

#if CONFIG_F5VID_DEMUXER

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

#endif /* CONFIG_F5VID_DEMUXER */

#if CONFIG_F5VID_DEMUXER

/* Container timestamp from a 12-byte VIDD inner header, in F5VID_TIME_RES
 * units. The M4 frame header (M4BitstreamParser::parseHeader, 800fca90) is a
 * packed bit header, so the 32-bit timestamp is only byte-aligned when the
 * preceding fields happen to fill a whole byte:
 *
 *   version(16) type(2) ext(1) [ext ? 4 bits] rounding(1) dc_thr(3) quant(5)
 *   [type ? fcode(3)] timestamp(32)
 *
 * I-VOPs in these files set ext (4 sub-bits, no fcode) and so reach 32 bits;
 * P-VOPs clear ext but add fcode and reach only 31, which puts the timestamp
 * one bit earlier than a plain AV_RB32 of inner+8. Reading it byte-aligned
 * doubles every P timestamp, which is what made the container clock look
 * non-monotonic. Ticks are 30000 Hz (1001 per frame at 29.97 fps), doubled
 * here into the 60000 Hz stream timebase. */
static uint32_t f5vid_inner_ts(const uint8_t *inner)
{
    uint32_t raw = AV_RB32(inner + 8);
    if ((inner[6] >> 6) != 0)                    /* not an I-VOP: 31-bit header */
        raw = (raw >> 1) | ((uint32_t)(inner[7] & 1) << 31);
    return raw * 2;
}

/* Synthesize a VOP header (verid=1, rectangular, progressive). type: 0=I,1=P,2=B.
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
               * by the caller (see read_packet). */
}

static int f5vid_probe(const AVProbeData *p)
{
    if (p->buf_size < 0x30)
        return 0;
    /* VID1 magic + BE32 len 0x20, then HEAD magic */
    if (AV_RB32(p->buf) != MKBETAG('V','I','D','1'))
        return 0;
    if (AV_RB32(p->buf + 4) != 0x20)
        return 0;
    if (AV_RB32(p->buf + 0x20) != MKBETAG('H','E','A','D'))
        return 0;
    if (AV_RB32(p->buf + 0x2c) != MKBETAG('V','I','D','H'))
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int f5vid_read_header(AVFormatContext *s)
{
    F5VIDDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t vid1_len, head_len, vidh_len, audh_len;
    uint8_t vidh[24], audh[16];
    int coded_w, coded_h, ret;
    F5BitW vw = { NULL, 0, 0, 0, 0 };

    AVStream *vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_MPEG4;

    AVStream *ast = avformat_new_stream(s, NULL);
    if (!ast)
        return AVERROR(ENOMEM);
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    /* APCM is DSP-ADPCM (8-byte frames: predictor/scale + 14 nibbles,
     * THP-compatible framing). The per-file coef table is not present,
     * so expose a zero table (memoryless decode: plausible level and
     * duration, harsh spectrum). */
    ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_THP;

    /* VID1 */
    if (avio_rb32(pb) != MKBETAG('V','I','D','1'))
        return AVERROR_INVALIDDATA;
    vid1_len = avio_rb32(pb);
    if (vid1_len != 0x20)
        return AVERROR_INVALIDDATA;
    avio_skip(pb, vid1_len - 8);

    /* HEAD */
    if (avio_rb32(pb) != MKBETAG('H','E','A','D'))
        return AVERROR_INVALIDDATA;
    head_len = avio_rb32(pb);
    if (head_len < 0xa0)
        return AVERROR_INVALIDDATA;
    /* u32 0 */
    avio_skip(pb, 4);

    /* VIDH */
    if (avio_rb32(pb) != MKBETAG('V','I','D','H'))
        return AVERROR_INVALIDDATA;
    vidh_len = avio_rb32(pb);
    if (vidh_len != 0x20)
        return AVERROR_INVALIDDATA;
    if (avio_read(pb, vidh, 24) != 24)
        return AVERROR_EOF;
    vst->codecpar->width  = AV_RB16(vidh + 4);
    vst->codecpar->height = AV_RB16(vidh + 6);
    m->frame_count = AV_RB32(vidh + 8);
    /* vidh+12 = max FRAM len (== largest FRAM chunk, informational) */
    {
        uint32_t fps_num = AV_RB32(vidh + 16);
        uint32_t fps_den = AV_RB16(vidh + 20);
        if (!fps_num || !fps_den) {
            fps_num = 2997; fps_den = 100;
        }
        /* Packet pts/dts are container ticks (60000 Hz); keep the stream
         * timebase in those units and advertise frame rate separately. */
        avpriv_set_pts_info(vst, 1, 1, F5VID_TIME_RES);
        vst->r_frame_rate = (AVRational){ fps_num, fps_den };
        vst->avg_frame_rate = (AVRational){ fps_num, fps_den };
        vst->duration = (int64_t)m->frame_count * F5VID_TIME_RES * fps_den / fps_num;
    }
    /* Coded video is the full VIDH display size (640x480 MPEG-4 ASP). */
    coded_w = vst->codecpar->width;
    coded_h = vst->codecpar->height;

    /* Synthesize VOL extradata (decoder needs it; container strips it). */
    if ((ret = f5vid_build_vol(&vw, coded_w, coded_h, F5VID_TIME_RES)) < 0)
        return ret;
    vst->codecpar->extradata = vw.buf;
    vst->codecpar->extradata_size = vw.len;
    /* vw.buf ownership transferred; do not free on success */

    /* AUDH */
    if (avio_rb32(pb) != MKBETAG('A','U','D','H')) {
        av_freep(&vst->codecpar->extradata);
        vst->codecpar->extradata_size = 0;
        return AVERROR_INVALIDDATA;
    }
    audh_len = avio_rb32(pb);
    if (audh_len < 16) {
        av_freep(&vst->codecpar->extradata);
        vst->codecpar->extradata_size = 0;
        return AVERROR_INVALIDDATA;
    }
    if (avio_read(pb, audh, 16) != 16) {
        av_freep(&vst->codecpar->extradata);
        vst->codecpar->extradata_size = 0;
        return AVERROR_EOF;
    }
    /* audh layout: u32 0 | "APCM" | u32 rate_BE | u16 channels +
     * 74 bytes footer (see above) */
    if (AV_RB32(audh + 4) != MKBETAG('A','P','C','M'))
        av_log(s, AV_LOG_WARNING, "f5vid: unexpected audio tag %08X\n",
               AV_RB32(audh + 4));
    ast->codecpar->sample_rate = AV_RB32(audh + 8);
    if (!ast->codecpar->sample_rate)
        ast->codecpar->sample_rate = 32000;
    /* channels are LE u16 at audh+12; the 74 bytes after it are a footer
     * of unknown purpose (see above) */
    {
        uint16_t ch = AV_RL16(audh + 12);
        if (ch == 1)
            ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        else
            ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        ast->codecpar->block_align = 8; /* one DSP-ADPCM frame */
    }
    /* Zero coef table (see above); 32 bytes per channel. */
    {
        int nch = ast->codecpar->ch_layout.nb_channels;
        uint8_t *tab;
        if (nch != 1 && nch != 2)
            nch = 2;
        tab = av_mallocz(32 * nch);
        if (!tab) {
            av_freep(&vst->codecpar->extradata);
            vst->codecpar->extradata_size = 0;
            return AVERROR(ENOMEM);
        }
        ast->codecpar->extradata = tab;
        ast->codecpar->extradata_size = 32 * nch;
    }
    if (audh_len > 16)
        avio_skip(pb, audh_len - 8 - 16);
    /* HEAD trailing padding */
    {
        int64_t cur = avio_tell(pb);
        int64_t head_end = 0x20 + head_len;
        if (cur < head_end)
            avio_skip(pb, head_end - cur);
    }

    avpriv_set_pts_info(ast, 1, 1, ast->codecpar->sample_rate);

    m->current_frame = 0;
    m->handle_audio_packet = 0;
    m->audio_size = 0;
    m->audio_sample_pos = 0;
    m->next_fram_pos = avio_tell(pb);
    m->video_dts = 0;
    m->index = NULL;
    m->nb_index = 0;
    /* Build a FRAM index (for seeking). Scan tag+len chain; each FRAM
     * contributes (pos, video_ts, audio_size, is_key). */
    {
        int64_t pos = m->next_fram_pos;
        int64_t fsize = avio_size(pb);
        int cap = 0;
        if (fsize > 0 && avio_seek(pb, pos, SEEK_SET) >= 0) {
            for (;;) {
                uint32_t tag, flen, vlen, alen;
                int64_t aoff;
                uint8_t inner[12];
                F5VIDIndex *ni;
                if (fsize > 0 && pos + 8 > fsize)
                    break;
                if (avio_seek(pb, pos, SEEK_SET) < 0)
                    break;
                if (avio_feof(pb))
                    break;
                tag = avio_rb32(pb);
                if (tag != MKBETAG('F','R','A','M'))
                    break;
                flen = avio_rb32(pb);
                if (flen < 32 + 8 + 12 + 8 + 8)
                    break;
                if (fsize > 0 && pos + flen > fsize)
                    break;
                /* VIDD inner is at pos+8+24+8; read code/sub/ts */
                if (avio_seek(pb, pos + 8 + 24 + 8, SEEK_SET) < 0)
                    break;
                if (avio_rb32(pb) != MKBETAG('V','I','D','D'))
                    break;
                vlen = avio_rb32(pb);
                if (vlen < 8 + 12 || avio_read(pb, inner, 12) != 12)
                    break;
                /* AUDD follows VIDD */
                aoff = pos + 8 + 24 + 8 + vlen;
                if (avio_seek(pb, aoff, SEEK_SET) < 0)
                    break;
                if (avio_rb32(pb) != MKBETAG('A','U','D','D'))
                    break;
                alen = avio_rb32(pb);
                if (alen < 8 + 8)
                    break;
                if (m->nb_index >= cap) {
                    cap = cap ? cap * 2 : 256;
                    ni = av_realloc_array(m->index, cap, sizeof(*ni));
                    if (!ni)
                        break;
                    m->index = ni;
                }
                m->index[m->nb_index].pos = pos;
                m->index[m->nb_index].video_ts = f5vid_inner_ts(inner);
                m->index[m->nb_index].audio_size = (alen > 16) ? (alen - 8 - 8) : 0;
                m->index[m->nb_index].is_key = (inner[6] == 0x20);
                m->nb_index++;
                pos += flen;
            }
            avio_seek(pb, m->next_fram_pos, SEEK_SET);
        }
    }
    return 0;
}

static int f5vid_read_seek(AVFormatContext *s, int stream_index,
                           int64_t timestamp, int flags)
{
    F5VIDDemuxContext *m = s->priv_data;
    int idx = -1;
    int i;
    int64_t asp = 0;
    int ach = 2, pairdiv = 2;
    /* timestamp is in stream timebase */
    if (stream_index < 0 || stream_index >= (int)s->nb_streams || m->nb_index <= 0)
        return AVERROR(ENOSYS);
    ach = s->streams[stream_index]->codecpar->ch_layout.nb_channels;
    pairdiv = (ach == 1) ? 1 : 2;
    if (s->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        /* video tb is 1/60000; find last FRAM with file-order dts <= ts.
         * dts = fram_index*2002, so fram_index = ts/2002. */
        int64_t fi = timestamp / 2002;
        if (fi < 0) fi = 0;
        if (fi >= m->nb_index) fi = m->nb_index - 1;
        /* back up to a keyframe unless precise seek requested */
        if (!(flags & AVSEEK_FLAG_ANY)) {
            while (fi > 0 && !m->index[fi].is_key)
                fi--;
        }
        idx = (int)fi;
    } else {
        /* audio tb is 1/sample_rate; find FRAM containing `timestamp`
         * (cumulative samples). Stereo frames interleave L,R (14 samp/frame
         * per channel); mono has no interleave. Audio has no keyframes;
         * callers seeking video separately keep A/V aligned. */
        int rate = s->streams[stream_index]->codecpar->sample_rate;
        int64_t want = timestamp;
        int64_t acc = 0;
        if (rate <= 0)
            return AVERROR(ENOSYS);
        idx = m->nb_index - 1;
        for (i = 0; i < m->nb_index; i++) {
            uint32_t nsamp = (m->index[i].audio_size / 8 / pairdiv) * 14;
            if (acc + (int64_t)nsamp > want)
                break;
            acc += nsamp;
        }
        if (i < m->nb_index)
            idx = i;
        /* audio_sample_pos must be recomputed to idx start */
        asp = 0;
        for (i = 0; i < idx; i++)
            asp += (m->index[i].audio_size / 8 / pairdiv) * 14;
    }
    if (idx < 0)
        return AVERROR(EINVAL);
    if (avio_seek(s->pb, m->index[idx].pos, SEEK_SET) < 0)
        return AVERROR(EIO);
    m->current_frame = idx;
    m->handle_audio_packet = 0;
    m->audio_size = 0;
    m->next_fram_pos = m->index[idx].pos;
    /* recompute clocks to idx */
    m->video_dts = (int64_t)idx * 2002;
    if (s->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        m->audio_sample_pos = asp;
    } else {
        asp = 0;
        for (i = 0; i < idx; i++)
            asp += (m->index[i].audio_size / 8 / pairdiv) * 14;
        m->audio_sample_pos = asp;
    }
    return 0;
}

static int f5vid_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    F5VIDDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    if (m->handle_audio_packet) {
        /* Audio tail of the current FRAM: AUDD payload after its 8-byte
         * inner header. Frames are 8B DSP-ADPCM (PS + 14 nibbles),
         * interleaved L,R,L,R...; the adpcm_thp decoder wants channel-major
         * (all L then all R), so deinterleave here. */
        uint8_t *raw;
        uint8_t *out;
        uint32_t nframes, nsamp;
        int ch = s->streams[1]->codecpar->ch_layout.nb_channels;
        if (ch != 2)
            ch = 2;
        raw = av_malloc(m->audio_size);
        if (!raw)
            return AVERROR(ENOMEM);
        ret = avio_read(pb, raw, m->audio_size);
        if (ret < (int)m->audio_size) {
            av_free(raw);
            return ret < 0 ? ret : AVERROR_EOF;
        }
        nframes = m->audio_size / 8;
        if (m->audio_size % 8)
            av_log(s, AV_LOG_WARNING, "f5vid: audio size %u not a multiple of 8\n",
                   m->audio_size);
        if (ch == 1) {
            /* mono: no deinterleave */
            nsamp = nframes * 14;
            if ((ret = av_packet_from_data(pkt, raw, nframes * 8)) < 0) {
                av_free(raw);
                return ret;
            }
            pkt->stream_index = 1;
            pkt->pts = pkt->dts = m->audio_sample_pos;
            pkt->duration = nsamp;
            m->audio_sample_pos += nsamp;
            avio_seek(pb, m->next_fram_pos, SEEK_SET);
            m->handle_audio_packet = 0;
            return ret;
        }
        if (nframes & 1) {
            av_log(s, AV_LOG_WARNING, "f5vid: odd DSP frame count %u\n", nframes);
            nframes &= ~1u;
        }
        out = av_malloc(nframes * 8);
        if (!out) {
            av_free(raw);
            return AVERROR(ENOMEM);
        }
        /* deinterleave: even frames -> L half, odd frames -> R half */
        for (uint32_t i = 0; i < nframes / 2; i++) {
            memcpy(out + i * 8, raw + (2 * i) * 8, 8);
            memcpy(out + (nframes / 2 + i) * 8, raw + (2 * i + 1) * 8, 8);
        }
        av_free(raw);
        nsamp = (nframes / 2) * 14;
        if ((ret = av_packet_from_data(pkt, out, nframes * 8)) < 0) {
            av_free(out);
            return ret;
        }
        pkt->stream_index = 1;
        pkt->pts = pkt->dts = m->audio_sample_pos;
        pkt->duration = nsamp;
        m->audio_sample_pos += nsamp;
        avio_seek(pb, m->next_fram_pos, SEEK_SET);
        m->handle_audio_packet = 0;
        return ret;
    }

    if (avio_feof(pb))
        return AVERROR_EOF;

    /* FRAM */
    { uint32_t _ft = avio_rb32(pb); if (_ft != MKBETAG('F','R','A','M')) return AVERROR_EOF; }
    {
        uint32_t fram_len = avio_rb32(pb);
        int64_t fram_pos = avio_tell(pb) - 8;
        int64_t fram_end = fram_pos + fram_len;
        if (fram_len < 32 + 8 + 12 + 8 + 8)
            return AVERROR_INVALIDDATA;
        avio_skip(pb, 24); /* 24 bytes zeros between FRAM header and VIDD */

        /* VIDD */
        if (avio_rb32(pb) != MKBETAG('V','I','D','D'))
            return AVERROR_INVALIDDATA;
        {
            uint32_t vidd_len = avio_rb32(pb);
            uint8_t inner[12];
            uint8_t *mbdata;
            uint32_t mb_size;
            int vop_type; /* 0=I (0x20), 1=P (0x40/0x50) */
            int quant, fwd, rounding;
            uint32_t frame_ts;
            F5BitW vw = { NULL, 0, 0, 0, 0 };
            uint8_t *out;
            int out_size, hdr_bytes, hdr_bits;
            if (vidd_len < 8 + 12)
                return AVERROR_INVALIDDATA;
            if (avio_read(pb, inner, 12) != 12)
                return AVERROR_EOF;
            /* inner: 00 00 00 00 | 00 01 code sub | BE32 ts (presentation
             * label; not used for timing, see below). Bit layout (MSB first,
             * see M4BitstreamParser::parseHeader): version(16)=1, type(2)=
             * code>>6 (0=I,1=P), X(1)=(code>>5)&1, then if X: flag/acdc/dp/
             * ext/coded single bits, coded/f/quant fields, and for P the
             * forward fcode. All shipped files use X=1,I (0x20, all flag
             * bits 0) and X=0,P (0x40/0x50, coded bit = rounding). */
            vop_type = inner[6] >> 6;
            frame_ts = f5vid_inner_ts(inner);
            if (vop_type == 0) {
                quant = inner[7] & 31;
                fwd = 2;
                rounding = 0;
            } else if (vop_type == 1) {
                quant = inner[7] >> 4;
                fwd = (inner[7] >> 1) & 7;
                rounding = (inner[6] >> 4) & 1;
                if (!fwd)
                    fwd = 1;
            } else {
                /* B/GMC (2/3): absent from shipped files; emit as P. */
                vop_type = 1;
                quant = inner[7] >> 4;
                fwd = (inner[7] >> 1) & 7;
                rounding = (inner[6] >> 4) & 1;
                if (!fwd)
                    fwd = 1;
            }
            if (!quant)
                quant = 4;
            if (quant > 31)
                quant = 31;
            /* MB data starts immediately after inner (no skip/transform). */
            mb_size = vidd_len - 8 - 12;
            mbdata = av_malloc(mb_size);
            if (!mbdata)
                return AVERROR(ENOMEM);
            if (avio_read(pb, mbdata, mb_size) != (int)mb_size) {
                av_free(mbdata);
                return AVERROR_EOF;
            }

            /* I-frames: run the MB bit filter (drops F5's DC extra bits,
             * resyncs past isolated bad bits, truncates on persistent
             * failure) so the mpeg4 decoder gets a clean stream. P-frames:
             * transcode MBs (motion remap to standard magnitude+sign+
             * residual with fwd-1 residual bits) with the same resync. */
            {
                F5BitW fw = { NULL, 0, 0, 0, 0 };
                int use_filtered = 0;
                int mb_w = (s->streams[0]->codecpar->width + 15) / 16;
                int mb_h = (s->streams[0]->codecpar->height + 15) / 16;
                if (mb_w < 1)
                    mb_w = 40;
                if (mb_h < 1)
                    mb_h = 30;
                if (vop_type == 0) {
                    f5_filter_iframe(mbdata, mb_size, mb_w, mb_h, &fw);
                    use_filtered = 1;
                } else {
                    int reslen = fwd > 1 ? fwd - 1 : 0;
                    f5_filter_pframe(mbdata, mb_size, mb_w, mb_h,
                                     reslen, &fw);
                    use_filtered = 1;
                }
                if (use_filtered) {
                    /* Flush filtered bits to bytes and use them as MB data. */
                    if (f5bw_flush(&fw) < 0) {
                        av_free(mbdata);
                        av_free(fw.buf);
                        return AVERROR(ENOMEM);
                    }
                    av_free(mbdata);
                    mbdata = fw.buf;
                    mb_size = fw.len;
                    fw.buf = NULL;
                }
            }

            /* Build packet: VOP start + VOP header (bit-exact, then MB data
             * follows at bit granularity). */
            /* Adopt the container clock whenever it does not move backwards
             * (it never does, once the P-VOP timestamp is unpacked correctly);
             * otherwise keep the synthesized one-frame advance. */
            if (frame_ts >= (uint32_t)m->video_dts)
                m->video_dts = frame_ts;
            if ((ret = f5vid_build_vop(&vw, vop_type, (uint32_t)m->video_dts,
                                       F5VID_TINC_BITS,
                                       quant, fwd, 2, rounding, &hdr_bits)) < 0) {
                av_free(mbdata);
                av_free(vw.buf);
                return ret;
            }
            /* vw.buf: 4B start + header whole bytes (+ rem bits in cache). */
            {
                int hdr_full_bytes = hdr_bits / 8;
                int hdr_rem_bits = hdr_bits % 8;
                /* vw.buf currently: 4B start + hdr_full_bytes (+ rem in cache) */
                hdr_bytes = 4 + hdr_full_bytes;
                out_size = hdr_bytes + mb_size + 1;
                out = av_malloc(out_size);
                if (!out) {
                    av_free(mbdata);
                    av_free(vw.buf);
                    return AVERROR(ENOMEM);
                }
                memcpy(out, vw.buf, hdr_bytes);
                if (hdr_rem_bits) {
                    /* bit-splice: remaining header bits then MB bits */
                    int i;
                    uint8_t *dst = out + hdr_bytes;
                    int dst_size = mb_size + 1;
                    uint32_t acc;
                    int acc_bits, dst_len = 0;
                    av_assert0(vw.nbits == hdr_rem_bits);
                    acc = vw.nbits ? (vw.cache & ((1u << vw.nbits) - 1)) : 0;
                    acc_bits = vw.nbits;
                    for (i = 0; i < mb_size; i++) {
                        acc = (acc << 8) | mbdata[i];
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
                    memcpy(out + hdr_bytes, mbdata, mb_size);
                    out_size = hdr_bytes + mb_size;
                }
            }
            av_free(mbdata);
            av_free(vw.buf);

            if ((ret = av_packet_from_data(pkt, out, out_size)) < 0) {
                av_free(out);
                return ret;
            }
            /* Keyframe = I-VOP */
            if (vop_type == 0)
                pkt->flags |= AV_PKT_FLAG_KEY;
            pkt->stream_index = 0;
            /* File order is display order and, once the P-VOP timestamp is
             * unpacked from the right bit offset, the container clock is
             * strictly increasing by one frame across every shipped file. Use
             * it, so frames the encoder spaced unevenly keep their real
             * timing; fall back to the synthesized clock if a file ever
             * disagrees. (m->video_dts was already advanced to frame_ts
             * above so the VOP header and the packet carry the same clock.) */
            pkt->pts = pkt->dts = m->video_dts;
            pkt->duration = 2002;
            m->video_dts += 2002;
        }

        /* AUDD (audio tail served on the next call) */
        if (avio_rb32(pb) != MKBETAG('A','U','D','D'))
            return AVERROR_INVALIDDATA;
        {
            uint32_t audd_len = avio_rb32(pb);
            uint8_t inner[8];
            if (audd_len < 8 + 8)
                return AVERROR_INVALIDDATA;
            if (avio_read(pb, inner, 8) != 8)
                return AVERROR_EOF;
            m->audio_size = audd_len - 8 - 8;
        }
        m->next_fram_pos = fram_end;
        m->handle_audio_packet = (m->audio_size > 0);
        if (!m->handle_audio_packet)
            avio_seek(pb, m->next_fram_pos, SEEK_SET);
        m->current_frame++;
        return ret;
    }
}

static int f5vid_read_close(AVFormatContext *s)
{
    F5VIDDemuxContext *m = s->priv_data;
    for (unsigned i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->extradata) {
            av_freep(&s->streams[i]->codecpar->extradata);
            s->streams[i]->codecpar->extradata_size = 0;
        }
    }
    av_freep(&m->index);
    m->nb_index = 0;
    return 0;
}

const FFInputFormat ff_f5vid_demuxer = {
    .p.name           = "f5vid",
    .p.long_name      = "Factor 5 DivX .vid (Carmen Sandiego GC)",
    .p.extensions     = "vid",
    .p.flags          = AVFMT_GENERIC_INDEX,
    .read_probe       = f5vid_probe,
    .read_header      = f5vid_read_header,
    .read_packet      = f5vid_read_packet,
    .read_seek        = f5vid_read_seek,
    .read_close       = f5vid_read_close,
    .priv_data_size   = sizeof(F5VIDDemuxContext),
};

#endif /* CONFIG_F5VID_DEMUXER */

#if CONFIG_F5VID_MUXER

/* ---- Factor 5 .vid muxer ----
 * Inverse of the demuxer above: takes the MPEG-4 packets the f5vid demuxer
 * emits (synthesized VOL extradata + VOP headers around an otherwise F5
 * MB stream) and re-packs them into a .vid container (VID1/HEAD/FRAM with
 * VIDD+AUDD). Only that packet layout is accepted (strict VOP header
 * checks); there is no MPEG-4 video encoder in this tree, so vid->vid
 * container work is the only producer of such packets.
 *
 * Video inversion per field: COD/MCBPC/CBPY/dquant/ac_pred copied verbatim
 * (codewords are identical to the standard ones); intra DC re-gains its
 * F5-only extra bit after sizes > 8 (value lost by the demuxer, emitted 0 —
 * the game decoder only consumes it); motion is parsed as standard
 * magnitude+sign+residual and re-emitted in F5's custom signed VLC;
 * coefficient blocks (all escapes) are verbatim.
 *
 * Audio is DSP-ADPCM (adpcm_thp) in channel-major 8-byte frames, as emitted
 * by the encoder and by the demuxer; it is re-interleaved L,R,L,R per FRAM.
 *
 * Everything is buffered and the file is written in write_trailer, so VIDH
 * frame_count/max_FRAM_len are exact and non-seekable output works.
 */

typedef struct F5MUXFrame {
    uint8_t *mb;      /* F5 MB stream for this frame */
    int mb_size;
    int is_key;
    int quant, fwd, rounding;
    int64_t ts30;     /* container timestamp, 30000 Hz ticks */
} F5MUXFrame;

typedef struct F5VIDMuxContext {
    F5MUXFrame *frames;
    int nb_frames, cap_frames;
    uint8_t *al;      /* channel-major audio FIFOs, 8-byte DSP frames */
    int al_len, al_size;
    uint8_t *ar;
    int ar_len, ar_size;
    int channels;     /* 0 = no audio stream */
    int rate;
    uint8_t coef[64]; /* DSP-ADPCM table, 32 bytes per channel */
    int have_coef;
    int width, height;
    int fps_num, fps_den;
} F5VIDMuxContext;

/* Reverse motion map: F5 codeword for a signed mvd (-32..32, never 0),
 * entry (len<<24)|codeword, 0 = unknown. Built once by scanning all 4096
 * peeks; the VLC is prefix-consistent so the first hit per value is the
 * canonical (shortest) code. */
static uint32_t f5mv_enc[65];
static int f5mv_enc_done;

static void f5mv_enc_build(void)
{
    int pk;
    if (f5mv_enc_done)
        return;
    memset(f5mv_enc, 0, sizeof(f5mv_enc));
    for (pk = 4; pk < 4096; pk++) {
        uint32_t e;
        int ln, v;
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
        if (ln <= 0 || ln > 12 || v < -32 || v > 32 || v == 0)
            continue;
        if (!f5mv_enc[v + 32])
            f5mv_enc[v + 32] = ((uint32_t)ln << 24) |
                               (uint32_t)(pk >> (12 - ln));
    }
    f5mv_enc_done = 1;
}

/* Parse our synthesized VOP header. Returns 0 with type (0=I,1=P), quant,
 * fwd, rounding and the MB-data bit offset, or -1 for anything else. */
static int f5mux_parse_vop(const uint8_t *data, int size, int *type,
                           int *quant, int *fwd, int *rounding, int *mb_pos)
{
    F5BitR r = { data, size * 8, 0, 0 };
    int tb = 0;
    if (size < 8 || AV_RB32(data) != 0x1B6)
        return -1;
    r.pos = 32;
    *type = f5br_get(&r, 2);
    if (*type != 0 && *type != 1)
        return -1;
    while (f5br_get(&r, 1)) {
        if (++tb > 64)
            return -1;
    }
    if (f5br_get(&r, 1) != 1)   /* marker */
        return -1;
    f5br_get(&r, 16);           /* time increment */
    if (f5br_get(&r, 1) != 1)   /* marker */
        return -1;
    if (f5br_get(&r, 1) != 1)   /* vop_coded */
        return -1;
    *rounding = 0;
    if (*type == 1)
        *rounding = f5br_get(&r, 1);
    if (f5br_get(&r, 3) != 0)   /* intra_dc_threshold idx must be 0 */
        return -1;
    *quant = f5br_get(&r, 5);
    if (*quant < 1 || *quant > 31)
        return -1;
    *fwd = 1;
    if (*type != 0) {
        *fwd = f5br_get(&r, 3);
        if (*fwd < 1 || *fwd > 7)
            return -1;
    }
    if (r.err)
        return -1;
    *mb_pos = r.pos;
    return 0;
}

/* Parse one standard motion value, emit F5 (leading 0 + signed-VLC codeword
 * + residual). reslen = fcode-1. Returns 0 ok, -1 fail. */
static int f5mux_motion(F5BitR *r, F5BitW *w, int reslen)
{
    int m, ln, sign, i, flen;
    uint32_t p, enc, fcode;
    for (ln = 1; ln <= 12; ln++) {
        p = f5br_show(r, ln);
        if (r->err)
            return -1;
        for (m = 0; m <= 32; m++)
            if (f5mv_std[m][0] == p && f5mv_std[m][1] == ln)
                goto found;
    }
    return -1;
found:
    r->pos += ln; /* consume standard magnitude code */
    if (m == 0)
        return f5bw_put(w, 1, 1); /* zero vector, same in both grammars */
    sign = f5br_get(r, 1);
    if (r->err)
        return -1;
    enc = f5mv_enc[(sign ? -m : m) + 32];
    if (!enc)
        return -1;
    flen = enc >> 24;
    fcode = enc & 0xFFFFFF;
    if (f5bw_put(w, 0, 1) < 0)
        return -1;
    if (f5bw_put(w, fcode, flen) < 0)
        return -1;
    for (i = 0; i < reslen; i++) {
        int b = f5br_get(r, 1);
        if (r->err)
            return -1;
        if (f5bw_put(w, b, 1) < 0)
            return -1;
    }
    return 0;
}

/* Parse one intra DC (standard), re-adding F5's extra bit after sizes > 8
 * (emitted 0; the value is lost by the demuxer and the game decoder only
 * consumes it). Returns 0 ok, -1 fail. */
static int f5mux_dc(F5BitR *r, F5BitW *w, int is_luma)
{
    int sz, len;
    sz = is_luma ? f5_vlc_dc_luma(r, &len) : f5_vlc_dc_chroma(r, &len);
    if (sz < 0 || r->err)
        return -1;
    if (f5_copy_bits(r, w, len + sz) < 0)
        return -1;
    if (sz > 8 && f5bw_put(w, 0, 1) < 0)
        return -1;
    return 0;
}

/* Transcode one P-frame MB (standard MPEG-4 in -> F5 out). reslen = motion
 * residual bits (fcode-1). Returns 0 ok, -1 fail. */
static int f5mux_pmb(F5BitR *r, F5BitW *w, int reslen)
{
    int skip, typ, extra, len, cbpy, leny, cbp, i;
    if (r->pos + 1 > r->nbits)
        return -1;
    skip = f5br_get(r, 1);
    if (r->err)
        return -1;
    if (f5bw_put(w, skip, 1) < 0)
        return -1;
    if (skip)
        return 0;
    for (;;) {
        typ = f5_vlc_mcbpc_p(r, &len, &extra);
        if (typ < 0 || r->err)
            return -1;
        if (f5_copy_bits(r, w, len) < 0)
            return -1;
        if (typ != 8)
            break;
    }
    if (typ == 8)
        return -1;
    if (typ == 3 || typ == 4) {
        if (f5_copy_bits(r, w, 1) < 0)
            return -1;
        cbpy = f5_vlc_cbpy(r, &leny);
        if (cbpy < 0 || r->err)
            return -1;
        if (f5_copy_bits(r, w, leny) < 0)
            return -1;
        if (typ == 4 && f5_copy_bits(r, w, 2) < 0)
            return -1;
        cbp = (cbpy << 2) | extra;
        for (i = 0; i < 6; i++) {
            int coded = (cbp >> 5) & 1;
            cbp = (cbp << 1) & 0x7f;
            if (f5mux_dc(r, w, i < 4) < 0)
                return -1;
            if (!coded)
                continue;
            if (f5_skip_ac_block(r, w) < 0)
                return -1;
        }
        return r->err ? -1 : 0;
    }
    cbpy = f5_vlc_cbpy(r, &leny);
    if (cbpy < 0 || r->err)
        return -1;
    if (f5_copy_bits(r, w, leny) < 0)
        return -1;
    if (typ == 1 && f5_copy_bits(r, w, 2) < 0)
        return -1;
    cbp = (((15 - cbpy) & 15) << 2) | extra;
    {
        int nmv = (typ == 2) ? 4 : 1;
        for (i = 0; i < nmv; i++) {
            if (f5mux_motion(r, w, reslen) < 0)
                return -1;
            if (f5mux_motion(r, w, reslen) < 0)
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

/* Transcode one I-frame MB (standard MPEG-4 in -> F5 out).
 * Returns 0 ok, -1 fail. */
static int f5mux_imb(F5BitR *r, F5BitW *w)
{
    int cbp, i, cbpc, lenc, cbpy, leny;
    for (;;) {
        cbpc = f5_vlc_mcbpc_intra(r, &lenc);
        if (cbpc < 0 || r->err)
            return -1;
        if (f5_copy_bits(r, w, lenc) < 0)
            return -1;
        if (cbpc != 8)
            break;
    }
    if (f5_copy_bits(r, w, 1) < 0) /* ac_pred_flag */
        return -1;
    cbpy = f5_vlc_cbpy(r, &leny);
    if (cbpy < 0 || r->err)
        return -1;
    if (f5_copy_bits(r, w, leny) < 0)
        return -1;
    if ((cbpc & 4) && f5_copy_bits(r, w, 2) < 0)
        return -1;
    cbp = (cbpc & 3) | (cbpy << 2);
    for (i = 0; i < 6; i++) {
        int coded = (cbp >> 5) & 1;
        cbp = (cbp << 1) & 0x7f;
        if (f5mux_dc(r, w, i < 4) < 0)
            return -1;
        if (!coded)
            continue;
        if (f5_skip_ac_block(r, w) < 0)
            return -1;
    }
    return r->err ? -1 : 0;
}

static void f5mux_free_frames(F5VIDMuxContext *m)
{
    int i;
    for (i = 0; i < m->nb_frames; i++)
        av_free(m->frames[i].mb);
    av_freep(&m->frames);
    m->nb_frames = m->cap_frames = 0;
}

static void f5mux_free_audio(F5VIDMuxContext *m)
{
    av_freep(&m->al);
    av_freep(&m->ar);
    m->al_len = m->al_size = m->ar_len = m->ar_size = 0;
}

static int f5vid_write_header(AVFormatContext *s)
{
    F5VIDMuxContext *m = s->priv_data;
    AVStream *vst = NULL, *ast = NULL;
    unsigned i;
    if (s->nb_streams < 1 || s->nb_streams > 2) {
        av_log(s, AV_LOG_ERROR, "f5vid: need 1 video + 0/1 audio streams\n");
        return AVERROR(EINVAL);
    }
    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (vst) {
                av_log(s, AV_LOG_ERROR, "f5vid: only one video stream\n");
                return AVERROR(EINVAL);
            }
            vst = s->streams[i];
        } else if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (ast) {
                av_log(s, AV_LOG_ERROR, "f5vid: only one audio stream\n");
                return AVERROR(EINVAL);
            }
            ast = s->streams[i];
        }
    }
    if (!vst || vst->codecpar->codec_id != AV_CODEC_ID_MPEG4) {
        av_log(s, AV_LOG_ERROR,
               "f5vid: video must be MPEG-4 in the f5vid demuxer layout\n");
        return AVERROR(EINVAL);
    }
    if (ast && ast->codecpar->codec_id != AV_CODEC_ID_ADPCM_THP) {
        av_log(s, AV_LOG_ERROR, "f5vid: audio must be DSP-ADPCM (adpcm_thp)\n");
        return AVERROR(EINVAL);
    }
    m->width  = vst->codecpar->width;
    m->height = vst->codecpar->height;
    if (m->width <= 0 || m->height <= 0) {
        av_log(s, AV_LOG_ERROR, "f5vid: video dimensions missing\n");
        return AVERROR(EINVAL);
    }
    m->fps_num = 2997;
    m->fps_den = 100;
    if (vst->avg_frame_rate.den > 0 && vst->avg_frame_rate.num > 0) {
        m->fps_num = vst->avg_frame_rate.num;
        m->fps_den = vst->avg_frame_rate.den;
    }
    avpriv_set_pts_info(vst, 1, 1, F5VID_TIME_RES);
    if (ast) {
        m->channels = ast->codecpar->ch_layout.nb_channels;
        if (m->channels != 1 && m->channels != 2)
            m->channels = 2;
        m->rate = ast->codecpar->sample_rate;
        if (m->rate <= 0)
            m->rate = 32000;
        avpriv_set_pts_info(ast, 1, 1, m->rate);
        /* Carry the DSP coef table (stream copy from our demuxer); fresh
         * encodes supply it per-packet (see write_audio). */
        if (ast->codecpar->extradata &&
            ast->codecpar->extradata_size >= 32 * m->channels) {
            memcpy(m->coef, ast->codecpar->extradata, 32 * m->channels);
            m->have_coef = 1;
        }
    }
    f5mv_enc_build();
    return 0;
}

static int f5vid_write_video(AVFormatContext *s, AVPacket *pkt)
{
    F5VIDMuxContext *m = s->priv_data;
    AVStream *vst = s->streams[pkt->stream_index];
    F5MUXFrame *fr;
    F5BitR r;
    F5BitW w = { NULL, 0, 0, 0, 0 };
    int type, quant, fwd, rounding, mb_pos, mb_w, mb_h, nmb, i;
    if (m->nb_frames >= m->cap_frames) {
        int cap = m->cap_frames ? m->cap_frames * 2 : 256;
        F5MUXFrame *nf = av_realloc_array(m->frames, cap, sizeof(*nf));
        if (!nf)
            return AVERROR(ENOMEM);
        m->frames = nf;
        m->cap_frames = cap;
    }
    fr = &m->frames[m->nb_frames];
    memset(fr, 0, sizeof(*fr));
    if (f5mux_parse_vop(pkt->data, pkt->size, &type, &quant, &fwd,
                        &rounding, &mb_pos) < 0) {
        av_log(s, AV_LOG_ERROR,
               "f5vid: video packet %d is not in the f5vid demuxer layout\n",
               m->nb_frames);
        return AVERROR_INVALIDDATA;
    }
    if (m->nb_frames == 0 && type != 0) {
        av_log(s, AV_LOG_ERROR, "f5vid: first frame must be an I-VOP\n");
        return AVERROR_INVALIDDATA;
    }
    fr->is_key   = type == 0;
    fr->quant    = quant;
    fr->fwd      = fwd;
    fr->rounding = rounding;
    if (pkt->pts != AV_NOPTS_VALUE)
        fr->ts30 = av_rescale_q(pkt->pts, vst->time_base, (AVRational){ 1, 30000 });
    else
        fr->ts30 = (int64_t)m->nb_frames * 1001;
    mb_w = (m->width + 15) / 16;
    mb_h = (m->height + 15) / 16;
    nmb = mb_w * mb_h;
    /* The stream ends right after the last MB (plus flush zeros), but VLC
     * peeks look up to 12 bits ahead; pad the tail with zeros (which can
     * only ever complete the already-validated last MB). */
    {
        uint8_t *padded = av_malloc(pkt->size + 2);
        if (!padded)
            return AVERROR(ENOMEM);
        memcpy(padded, pkt->data, pkt->size);
        padded[pkt->size] = padded[pkt->size + 1] = 0;
        r.buf = padded;
        r.nbits = (pkt->size + 2) * 8;
        r.pos = mb_pos;
        r.err = 0;
        for (i = 0; i < nmb; i++) {
            int ret = type == 0 ? f5mux_imb(&r, &w)
                                : f5mux_pmb(&r, &w, fwd > 1 ? fwd - 1 : 0);
            if (ret < 0 || r.err)
                break;
        }
        av_free(padded);
        if (i != nmb) {
            av_log(s, AV_LOG_ERROR, "f5vid: MB %d of frame %d does not parse\n",
                   i, m->nb_frames);
            av_free(w.buf);
            return AVERROR_INVALIDDATA;
        }
    }
    if (f5bw_flush(&w) < 0) {
        av_free(w.buf);
        return AVERROR(ENOMEM);
    }
    /* Trailing pad: shipped files end their MB stream with generous zero
     * padding, which VLC lookahead peeks rely on past the last consumed
     * bit. The filter output ends right at the last MB, so add two zero
     * bytes the same way (ignored by every reader after 1200 MBs). */
    if (f5bw_put(&w, 0, 16) < 0 || f5bw_flush(&w) < 0) {
        av_free(w.buf);
        return AVERROR(ENOMEM);
    }
    fr->mb = w.buf;
    fr->mb_size = w.len;
    m->nb_frames++;
    return 0;
}

static int f5vid_write_audio(AVFormatContext *s, AVPacket *pkt)
{
    F5VIDMuxContext *m = s->priv_data;
    int frames, half, n;
    uint8_t *nb;
    size_t side_size;
    const uint8_t *side;
    if (!m->have_coef) {
        side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                       &side_size);
        if (side && side_size >= 32 * (size_t)m->channels) {
            memcpy(m->coef, side, 32 * m->channels);
            m->have_coef = 1;
        }
    }
    if (pkt->size % 8) {
        av_log(s, AV_LOG_WARNING, "f5vid: audio size %d not a multiple of 8\n",
               pkt->size);
    }
    frames = pkt->size / 8;
    if (m->channels == 2) {
        if (frames & 1) {
            av_log(s, AV_LOG_WARNING, "f5vid: odd DSP frame count %d\n", frames);
            frames &= ~1;
        }
        half = frames / 2 * 8;
        n = m->al_len + half;
        if (n > m->al_size) {
            int ns = FFMAX(m->al_size * 2, n + 64);
            nb = av_realloc(m->al, ns);
            if (!nb)
                return AVERROR(ENOMEM);
            m->al = nb;
            m->al_size = ns;
        }
        memcpy(m->al + m->al_len, pkt->data, half);
        m->al_len += half;
        n = m->ar_len + half;
        if (n > m->ar_size) {
            int ns = FFMAX(m->ar_size * 2, n + 64);
            nb = av_realloc(m->ar, ns);
            if (!nb)
                return AVERROR(ENOMEM);
            m->ar = nb;
            m->ar_size = ns;
        }
        memcpy(m->ar + m->ar_len, pkt->data + half, half);
        m->ar_len += half;
    } else {
        n = m->al_len + frames * 8;
        if (n > m->al_size) {
            int ns = FFMAX(m->al_size * 2, n + 64);
            nb = av_realloc(m->al, ns);
            if (!nb)
                return AVERROR(ENOMEM);
            m->al = nb;
            m->al_size = ns;
        }
        memcpy(m->al + m->al_len, pkt->data, frames * 8);
        m->al_len += frames * 8;
    }
    return 0;
}

static int f5vid_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    enum AVMediaType t = s->streams[pkt->stream_index]->codecpar->codec_type;
    if (t == AVMEDIA_TYPE_VIDEO)
        return f5vid_write_video(s, pkt);
    if (t == AVMEDIA_TYPE_AUDIO)
        return f5vid_write_audio(s, pkt);
    return AVERROR(EINVAL);
}

static void f5vid_write_inner(uint8_t out[12], const F5MUXFrame *fr)
{
    uint32_t ts = (uint32_t)fr->ts30;
    out[0] = out[1] = out[2] = out[3] = 0;
    out[4] = 0;
    out[5] = 1;
    if (fr->is_key) {
        out[6] = 0x20;
        out[7] = fr->quant & 31;
        AV_WB32(out + 8, ts);
    } else {
        out[6] = 0x40 | ((fr->rounding & 1) << 4);
        out[7] = ((fr->quant & 15) << 4) | ((fr->fwd & 7) << 1);
        AV_WB32(out + 8, ts << 1);
    }
}

static int f5vid_write_trailer(AVFormatContext *s)
{
    F5VIDMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int i, maxfram = 0;
    int al_frames = m->al_len / 8; /* per-channel DSP frames */
    int aoff = 0;                  /* running per-channel frame offset */
    /* VID1 */
    avio_wb32(pb, MKBETAG('V', 'I', 'D', '1'));
    avio_wb32(pb, 0x20);
    for (i = 0; i < 6; i++)
        avio_wb32(pb, i == 1 ? 0x01000013 : 0);
    /* HEAD */
    avio_wb32(pb, MKBETAG('H', 'E', 'A', 'D'));
    avio_wb32(pb, 0xa0);
    avio_wb32(pb, 0);
    avio_wb32(pb, MKBETAG('V', 'I', 'D', 'H'));
    avio_wb32(pb, 0x20);
    avio_wb32(pb, 1);
    avio_wb16(pb, m->width);
    avio_wb16(pb, m->height);
    avio_wb32(pb, m->nb_frames);
    {
        int64_t fpos = avio_tell(pb);
        avio_wb32(pb, 0); /* max_FRAM_len, patched below */
        avio_wb32(pb, m->fps_num);
        avio_wb16(pb, m->fps_den);
        avio_wb16(pb, 0);
        avio_wb32(pb, MKBETAG('A', 'U', 'D', 'H'));
        avio_wb32(pb, 0x60);
        avio_wb32(pb, 0);
        avio_wb32(pb, MKBETAG('A', 'P', 'C', 'M'));
        avio_wb32(pb, m->rate ? m->rate : 32000);
        avio_wl16(pb, m->channels ? m->channels : 2);
        if (m->have_coef)
            avio_write(pb, m->coef, 32 * (m->channels ? m->channels : 2));
        for (i = 0; i < 74 - (m->have_coef ? 32 * (m->channels ? m->channels : 2) : 0); i++)
            avio_w8(pb, 0);
        for (i = 0; i < 20; i++)
            avio_w8(pb, 0);
        /* FRAMs */
        for (i = 0; i < m->nb_frames; i++) {
            F5MUXFrame *fr = &m->frames[i];
            int base = al_frames / m->nb_frames;
            int rem = al_frames % m->nb_frames;
            int nf = base + (i < rem ? 1 : 0);
            int k;
            uint32_t vidd_len = 8 + 12 + fr->mb_size;
            uint32_t aud_bytes, audd_len, fram_len;
            uint8_t inner[12];
            aud_bytes = nf * 8 * (m->channels == 1 ? 1 : 2);
            audd_len = 8 + 8 + aud_bytes;
            fram_len = 8 + 24 + vidd_len + audd_len;
            if ((int)fram_len > maxfram)
                maxfram = fram_len;
            avio_wb32(pb, MKBETAG('F', 'R', 'A', 'M'));
            avio_wb32(pb, fram_len);
            for (k = 0; k < 24; k++)
                avio_w8(pb, 0);
            avio_wb32(pb, MKBETAG('V', 'I', 'D', 'D'));
            avio_wb32(pb, vidd_len);
            f5vid_write_inner(inner, fr);
            avio_write(pb, inner, 12);
            avio_write(pb, fr->mb, fr->mb_size);
            avio_wb32(pb, MKBETAG('A', 'U', 'D', 'D'));
            avio_wb32(pb, audd_len);
            for (k = 0; k < 4; k++)
                avio_w8(pb, 0);
            avio_wb32(pb, audd_len - 32);
            for (k = 0; k < nf; k++) {
                avio_write(pb, m->al + aoff * 8, 8);
                if (m->channels != 1)
                    avio_write(pb, m->ar + aoff * 8, 8);
                aoff++;
            }
        }
        if (s->pb->seekable) {
            int64_t end = avio_tell(pb);
            avio_seek(pb, fpos, SEEK_SET);
            avio_wb32(pb, maxfram);
            avio_seek(pb, end, SEEK_SET);
        }
    }
    f5mux_free_frames(m);
    f5mux_free_audio(m);
    return 0;
}

static void f5vid_write_deinit(AVFormatContext *s)
{
    F5VIDMuxContext *m = s->priv_data;
    f5mux_free_frames(m);
    f5mux_free_audio(m);
}

const FFOutputFormat ff_f5vid_muxer = {
    .p.name           = "f5vid",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Factor 5 DivX .vid (Carmen Sandiego GC)"),
    .p.extensions     = "vid",
    .p.audio_codec    = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec    = AV_CODEC_ID_MPEG4,
    .priv_data_size   = sizeof(F5VIDMuxContext),
    .write_header     = f5vid_write_header,
    .write_packet     = f5vid_write_packet,
    .write_trailer    = f5vid_write_trailer,
    .deinit           = f5vid_write_deinit,
};

#endif /* CONFIG_F5VID_MUXER */
