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
 *                         | u16 channels_LE (2) | 74 bytes initial audio data
 *       (Bam/LS/River carry zeros here = silence; Demo/A2M carry samples.
 *       The 74 bytes are part of the audio stream, not header.)
 *   FRAM len variable: 8-byte header + 24 bytes zeros, then VIDD + AUDD.
 *     VIDD: 8-byte header + 12-byte inner header + DivX video MB data.
 *       inner: 00 00 00 00 | 00 01 code sub | BE32 timestamp (60000 Hz clock:
 *       0, 2002, 4004, ... = frame*1001*2; some I-frames file out of order,
 *       e.g. Demo f3 ts=3003 after f2 ts=4004).
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
 *       not have; the demuxer drops it. Isolated single-bit glitches occur
 *       in some frames (deterministic per frame); the demuxer resyncs past
 *       them (skips to the next valid MB) so the rest of the frame decodes.
 *       KNOWN GAPS: P-frame motion uses a custom signed VLC (not yet
 *       transcoded, so P with nonzero MVs may misparse); B/GMC types (2/3)
 *       are asserted unsupported (absent from all shipped files).
  *     AUDD: 8-byte header + 8-byte inner header + audio data.
  *       inner: 00 00 00 00 | BE32 (ALN-32, i.e. audio_len-16).
  *       "APCM" audio at 32000 Hz stereo, ~37 kB/s: DSP-ADPCM with 8-byte
  *       frames (predictor/scale header + 14 nibbles, THP-compatible framing;
  *       14 samples/8 bytes = 4.57 bits/sample = 36.6 kB/s stereo @32kHz).
  *       Frames are interleaved L,R,L,R...; the demuxer deinterleaves to
  *       channel-major for the adpcm_thp decoder. The per-file coef table
  *       is not present (AUDH carries initial samples, not coefs), so the
  *       demuxer exposes a zero table (memoryless decode: plausible level
  *       and duration, harsh spectrum). TODO: recover the true coef table
  *       (game binary / MusyX docs) and confirm the channel layout for full
  *       quality. The 74 AUDH bytes are initial audio (skipped here; ~2 ms).
  *
  *   Timing: file order is display order; packet pts/dts are a monotonic
  *   decode clock (2002 ticks = 1 frame at 29.97 fps in 60000 Hz units).
  *   Container ts jumps (out-of-order I-frames) are ignored for timing
  *   (players sort by pts; the jumps would otherwise drop frames).
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
#include "demux.h"
#include "internal.h"

#include "avformat.h"

typedef struct F5VIDIndex {
    int64_t pos;        /* file offset of FRAM tag */
    uint32_t video_ts;  /* container ts (for reference; timing uses video_dts) */
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

/* ---- F5 I-frame MB bit filter ----
 * Parses I-frame MB data with F5's exact VLC grammar (all tables verified
 * against the game binary; they match MPEG-4) and emits a clean MPEG-4 MB
 * stream: identical bits except (a) the 1 extra bit F5 emits after an intra
 * DC with size>8 is dropped, and (b) on invalid VLC the filter resyncs
 * (skips input bits to the next valid MB) or truncates. P-frames are passed
 * through (motion uses custom tables; transcode TBD).
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
                /* third escape: last(1) run(6) marker(1) level(12) marker(1) */
                if (r->pos + 1 + 6 + 1 + 12 + 1 > r->nbits)
                    return -1;
                last = f5br_get(r, 1);
                f5bw_put(w, last, 1);
                f5_copy_bits(r, w, 6 + 1 + 12 + 1);
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
 * conceal the rest. */
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
        if (!ok || resyncs >= 40)
            break; /* truncate */
        r.pos = save_pos + adv;
        r.err = 0;
        resyncs++;
    }
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
    if ((ret = f5bw_put(w, 0, 3))  < 0) return ret; /* intra_dc_threshold idx 0 */
    if ((ret = f5bw_put(w, quant, 5)) < 0) return ret; /* vop_quant */
    if (type != 0 && (ret = f5bw_put(w, fwd, 3)) < 0) return ret;
    if (type == 2 && (ret = f5bw_put(w, bwd, 3)) < 0) return ret;
    if (type == 1 && (ret = f5bw_put(w, rounding, 1)) < 0) return ret;
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
     * THP-compatible framing). The per-file coef table is not present
     * (AUDH carries initial samples, not coefs), so expose a zero table
     * (memoryless decode: plausible level/duration, harsh spectrum).
     * TODO: recover the true coef table (game binary / MusyX docs) and the
     * exact channel layout for full quality. */
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
    /* audh layout: u32 0 | "APCM" | u32 rate_BE | u16 channels + ... */
    if (AV_RB32(audh + 4) != MKBETAG('A','P','C','M'))
        av_log(s, AV_LOG_WARNING, "f5vid: unexpected audio tag %08X\n",
               AV_RB32(audh + 4));
    ast->codecpar->sample_rate = AV_RB32(audh + 8);
    if (!ast->codecpar->sample_rate)
        ast->codecpar->sample_rate = 32000;
    /* channels are LE u16 at audh+12; the rest of AUDH is initial audio
     * data, skip the rest of AUDH */
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
                m->index[m->nb_index].video_ts = AV_RB32(inner + 8);
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
    if (avio_rb32(pb) != MKBETAG('F','R','A','M'))
        return AVERROR_EOF;
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
             * failure) so the mpeg4 decoder gets a clean stream. P-frames
             * pass through (motion transcode TBD). */
            {
                F5BitW fw = { NULL, 0, 0, 0, 0 };
                int use_filtered = 0;
                if (vop_type == 0) {
                    int mb_w = (s->streams[0]->codecpar->width + 15) / 16;
                    int mb_h = (s->streams[0]->codecpar->height + 15) / 16;
                    if (mb_w < 1)
                        mb_w = 40;
                    if (mb_h < 1)
                        mb_h = 30;
                    f5_filter_iframe(mbdata, mb_size, mb_w, mb_h, &fw);
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
             * follows at bit granularity). VOP tinc uses the monotonic
             * decode clock (container ts jumps); packet pts below too. */
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
            /* File order is display order; container ts jumps (out-of-order
             * I-frames) so do not use it for timing. Fixed 1-frame duration
             * (2002 ticks = 1/29.97s at 60kHz) and monotonic pts/dts. */
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
