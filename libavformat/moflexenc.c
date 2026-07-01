/*
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 * MOFLEX muxer
 *
 * Retail-faithful 3DS MOFLEX writer.  The byte grammar is reverse-engineered
 * from retail moflex (e.g. ESJ_MD4 boss) by instrumenting the demuxer:
 *
 *   - The stream is a sequence of 4096-byte blocks.  A block fills to exactly
 *     4096 bytes, then the trailing frame is split (endframe=0, no terminator)
 *     and continues in the next block.  A block that runs out of data ends
 *     early with a single 0x00 terminator (a short, variable-length block).
 *
 *   - Each block begins (after an optional 4C32 sync header) with one flags
 *     byte = (counter<<2)|1.  bit0=1 (VariablePacketSize), bit1=0.  The 6-bit
 *     counter increments only on blocks that carry a sync header; intervening
 *     blocks reuse the same flags byte.
 *
 *   - A 4C32 sync header (magic + checksum + LE-unused / BE ts(µs) + blocksize,
 *     followed by the full descriptor list) is written at a block start about
 *     once per second of media time.  ts is microseconds, base = 1.
 *
 *   - Chunks within a block are concatenated directly (no per-chunk flags
 *     byte): [EP header + payload][EP header + payload]...  Video (stream 0)
 *     and audio (stream 1) are interleaved by timestamp.
 *
 *   - The seek table is stream_index 2 (type=4 data descriptor) and lives at
 *     the FRONT of the file, with one 24-byte entry per sync point (~1/sec):
 *       16-byte header: LE32 n, LE32 total_frames, LE32 dur_us, LE32 0
 *       entry: LE64 frame_index, LE64 ts_us, LE64 file_offset(of a 4C32 block)
 *
 * Because the front table's entries reference absolute offsets of media sync
 * blocks, media is muxed into an in-memory dyn buffer first (offsets relative
 * to media start).  Once muxing is done the entry count n — and therefore the
 * exact front size — is known, media offsets are biased by the front size, and
 * the header + table + media are written out in order.  No seek-back needed.
 *
 * This file is part of FFmpeg.
 */

#include "avformat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "mux.h"
#include "avio_internal.h"
#include "internal.h"
#include "mo.h"
#include "mo_audioenc.h"

#define MOFLEX_BLOCK   4096      /* block_size from the 4C32 sync header (retail uses 4096) */
#define SEEK_ENTRY_SZ  24
#define SYNC_INTERVAL  1000000   /* µs between sync points (~1/sec)        */

typedef struct QEntry {
    uint8_t *data;
    int      size;
    int64_t  pts;       /* µs */
    int      si;        /* stream index (0 video, 1 audio) */
} QEntry;

typedef struct MOFLEXMuxContext {
    const AVClass *class;   /* MUST be first */
    int audio_codec;        /* -1=auto, 0=fastaudio, 1=adpcm, 2=pcm */
    int gop_size;           /* unused placeholder kept for option compat */
    int audio_stream_index;
    int video_stream_index;
    int aenc_inited;
    MobiAudioEnc aenc;

    /* Multi-video (stereoscopic 3D) support.  MOFLEX can carry more than one
     * video chunk; the 3DS pairs two video streams (left/right eye) into a 3D
     * image.  The descriptors are written in order video(s) -> data -> audio,
     * and the EP stream_index of each chunk is its position in that list.  With
     * a second video the data and audio EP indices shift up by one. */
    int nb_video;               /* 1 or 2 */
    int video_in[2];            /* input stream indices of the video streams   */
    int64_t vframes[2];         /* per-video frame counter (drives pts)        */
    int data_ep;                /* EP stream index of the seek-table data desc */
    int audio_ep;               /* EP stream index of the audio descriptor     */
    int mo_layout;              /* ImageLayout byte for video 0 (Simple2D = 6) */
    int mo_layout2;             /* ImageLayout byte for video 1                */

    AVRational vfr;             /* video frame rate */

    /* Pending interleave queue (all frames buffered, sorted at trailer). */
    QEntry  *q;
    int      nq, qcap;

    int      total_frames;      /* video frames */
    int64_t  audio_samples;     /* cumulative audio sample-frames */
    int      audio_rate;

    /* --- media block-writer state (operates on m->dyn) --- */
    AVIOContext *dyn;
    int64_t  block_start;       /* dyn offset of open block, or -1 */
    int      counter;           /* 6-bit flags counter */
    int64_t  next_sync_us;
    int64_t  cur_sync_ts;       /* ts (µs) of the most recent 4C32 sync block; per-frame
                                 * endframe timestamps are encoded relative to this */

    /* Recorded media sync points (one per 4C32 block), relative offsets. */
    int64_t *sp_frame, *sp_ts, *sp_off;
    int      n_sp, sp_cap;
} MOFLEXMuxContext;

/* ----------------------------------------------------------------
 * Variable-byte encoder (for descriptors)
 * ---------------------------------------------------------------- */
static void put_var(AVIOContext *pb, unsigned v)
{
    if (v < 0x80) {
        avio_w8(pb, v);
    } else if (v < 0x4000) {
        avio_w8(pb, 0x80 | (v >> 7));
        avio_w8(pb, v & 0x7F);
    } else if (v < 0x200000) {
        avio_w8(pb, 0x80 | (v >> 14));
        avio_w8(pb, 0x80 | ((v >> 7) & 0x7F));
        avio_w8(pb, v & 0x7F);
    } else {
        avio_w8(pb, 0x80 | (v >> 21));
        avio_w8(pb, 0x80 | ((v >> 14) & 0x7F));
        avio_w8(pb, 0x80 | ((v >> 7) & 0x7F));
        avio_w8(pb, v & 0x7F);
    }
}

/* ----------------------------------------------------------------
 * EP writer
 *
 * write_ep() writes the bit-packed EP header followed by the payload.
 * If data==NULL writes only the 0x00 terminator byte.
 *
 * Bit layout (MSB first):
 *   pop_length  : (nrbits-1) zero bits then 1 bit
 *   stream_index: nrbits bits
 *   endframe    : 1 bit
 *   if endframe : 1, 0, 0, 1, 28×0  (endframe extra fields)
 *   pkt_size-1  : 13 bits
 * ---------------------------------------------------------------- */
static int ep_nrbits(int stream_index)
{
    return (stream_index <= 1) ? 1 : (av_log2(stream_index) + 1);
}

/* Number of bytes the EP header occupies for a given stream/endframe. */
static int ep_header_bytes(int stream_index, int endframe)
{
    int nrbits = ep_nrbits(stream_index);
    int bits = (nrbits - 1) + 1 + nrbits + 1 + (endframe ? (1 + 1 + 1 + 1 + 28) : 0) + 13;
    return (bits + 7) / 8;
}

static void write_ep(AVIOContext *pb, int stream_index,
                     const uint8_t *data, int size, int endframe,
                     int frametype, int64_t rel_ts)
{
    if (!data) {
        avio_w8(pb, 0x00);
        return;
    }

    uint64_t val = 0;
    int bits = 0;
    int nrbits = ep_nrbits(stream_index);

    bits += nrbits - 1;
    val |= (uint64_t)1 << (63 - bits);
    bits++;

    val |= (uint64_t)stream_index << (64 - bits - nrbits);
    bits += nrbits;

    if (endframe)
        val |= (uint64_t)1 << (63 - bits);
    bits++;

    if (endframe) {
        /* FrameType: exp-Golomb-style — a 1 stop bit then `nrbits` value bits
         * (here nrbits=1, so one value bit). 0 = keyframe/intra, 1 = inter. */
        val |= (uint64_t)1 << (63 - bits); bits++;          /* FrameType stop bit */
        if (frametype) val |= (uint64_t)1 << (63 - bits);
        bits++;                                              /* FrameType value bit */
        bits++;                                              /* sign = 0 (rel_ts >= 0) */
        val |= (uint64_t)1 << (63 - bits); bits++;          /* v23-extension stop -> 28-bit field */
        /* 28-bit per-frame timestamp, RELATIVE to the current sync block's GTS,
         * in microseconds (the 3DS player paces playback on this; all-zero here
         * stalls it to ~1 FPS).  Fits in 28 bits within the 1s sync interval. */
        val |= (uint64_t)((uint32_t)rel_ts & 0x0FFFFFFFu) << (64 - bits - 28);
        bits += 28;
    }

    val |= (uint64_t)(size - 1) << (64 - bits - 13);
    bits += 13;

    int nbytes = (bits + 7) / 8;
    for (int i = 0; i < nbytes; i++) {
        avio_w8(pb, (uint8_t)(val >> 56));
        val <<= 8;
    }

    avio_write(pb, data, size);
}

/* ----------------------------------------------------------------
 * Sync header + descriptor list (written at file start and at every
 * re-sync block; retail repeats the full descriptor list each time).
 * ---------------------------------------------------------------- */
static void write_descriptors(AVFormatContext *s, AVIOContext *pb)
{
    MOFLEXMuxContext *m = s->priv_data;

    /* Video descriptor(s).  Retail BOSS moflex uses type 3
     * (MoLiveStreamVideoWithLayout, 13 bytes = the 12-byte video descriptor +
     * 1 layout byte).  The 3DS player appears to only fast-path (hardware-
     * decode) the layout descriptor; a plain type-1 descriptor falls back to a
     * slow software path that crawls (~1-3 FPS) and eventually crashes.  The
     * layout byte = (ImageLayout & 0xF) | (ImageRotation << 4).  ImageLayout:
     * 0/1 Interleave3D L/R-first, 2/3 TopToBottom3D, 4/5 SideBySide3D, 6 Simple2D.
     * 3DS stereoscopic 3D is a SINGLE video stream whose frames pack both eyes
     * per this layout (NOT two video chunks — the player renders only the first
     * chunk).  Set -mo_layout 4 with a side-by-side packed frame for L/R 3D. */
    for (int v = 0; v < m->nb_video; v++) {
        AVStream *st = s->streams[m->video_in[v]];
        put_var(pb, 3);
        put_var(pb, 13);
        avio_w8(pb, v);                  /* EP stream index = order in list */
        avio_w8(pb, 0); /* 0 = MobiClip codec */
        avio_wb16(pb, m->vfr.num);   /* fps numerator */
        avio_wb16(pb, m->vfr.den);   /* fps denominator */
        avio_wb16(pb, st->codecpar->width);
        avio_wb16(pb, st->codecpar->height);
        avio_w8(pb, 1); /* PelRatioRate  */
        avio_w8(pb, 1); /* PelRatioScale */
        avio_w8(pb, v == 0 ? m->mo_layout : m->mo_layout2); /* ImageLayout */
    }
    /* Data/seek-table descriptor — retail order: data before audio */
    put_var(pb, 4);
    put_var(pb, 2);
    avio_w8(pb, m->data_ep);
    avio_w8(pb, 0);
    /* Audio descriptor — retail order: audio after data */
    for (int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            put_var(pb, 2);
            put_var(pb, 6);
            avio_w8(pb, m->audio_ep); /* audio is last in retail order */
            int fc = m->audio_codec;
            if (fc < 0) {
                if      (st->codecpar->codec_id == AV_CODEC_ID_FASTAUDIO)         fc = 0;
                else if (st->codecpar->codec_id == AV_CODEC_ID_ADPCM_IMA_MOFLEX) fc = 1;
                else                                                                fc = 2;
            }
            avio_w8(pb, fc);
            avio_wb24(pb, st->codecpar->sample_rate - 1);
            avio_w8(pb, st->codecpar->ch_layout.nb_channels - 1);
        }
    }
    /* Descriptor-list terminator */
    put_var(pb, 0);
    put_var(pb, 0);
}

static void write_sync_header(AVFormatContext *s, AVIOContext *pb, int64_t ts_us)
{
    uint64_t ts = (uint64_t)ts_us;
    uint32_t v19 = (uint32_t)(ts >> 32);
    uint16_t chk = (uint16_t)(((ts >> 16) & 0xFFFF) ^ (v19 >> 16) ^ 0xAAAAu
                              ^ (v19 & 0xFFFF) ^ (ts & 0xFFFF));
    avio_wb16(pb, 0x4C32);
    avio_wb16(pb, chk);
    avio_wb64(pb, ts);
    avio_wb16(pb, MOFLEX_BLOCK - 1);
    write_descriptors(s, pb);
}

/* ----------------------------------------------------------------
 * Media block writer (writes into m->dyn)
 * ---------------------------------------------------------------- */
static int record_sync(MOFLEXMuxContext *m, int64_t frame, int64_t ts, int64_t off)
{
    if (m->n_sp >= m->sp_cap) {
        int ncap = m->sp_cap ? m->sp_cap * 2 : 256;
        int64_t *nf = av_realloc_array(m->sp_frame, ncap, sizeof(int64_t));
        int64_t *nt = av_realloc_array(m->sp_ts,    ncap, sizeof(int64_t));
        int64_t *no = av_realloc_array(m->sp_off,   ncap, sizeof(int64_t));
        if (!nf || !nt || !no) {
            av_free(nf); av_free(nt); av_free(no);
            return AVERROR(ENOMEM);
        }
        m->sp_frame = nf; m->sp_ts = nt; m->sp_off = no; m->sp_cap = ncap;
    }
    m->sp_frame[m->n_sp] = frame;
    m->sp_ts   [m->n_sp] = ts;
    m->sp_off  [m->n_sp] = off;
    m->n_sp++;
    return 0;
}

/* Open a new block at the current dyn position, emitting a sync header +
 * incrementing the counter when a sync point is due. */
static int open_block(AVFormatContext *s, int64_t pts, int frame_start)
{
    MOFLEXMuxContext *m = s->priv_data;
    int64_t off = avio_tell(m->dyn);
    /* Only start a sync block at a frame boundary.  A sync block bumps the
     * 6-bit flags counter, and the reference demuxer (MoLiveDemux.ReadDataBlock)
     * calls ClearData() on every stream when that counter changes — which would
     * discard the already-accumulated first half of any frame that spans into
     * the sync block.  Deferring the sync to the next frame start (at most one
     * frame ~= 33ms late vs the 1s interval) keeps frames contiguous. */
    int do_sync = frame_start && ((off == 0) || (pts >= m->next_sync_us));

    if (do_sync) {
        int64_t frame = m->vfr.num > 0
            ? pts * m->vfr.num / (1000000LL * m->vfr.den) : 0;
        int ret = record_sync(m, frame, pts, off);
        if (ret < 0)
            return ret;
        m->counter = (m->counter + 1) & 0x3F;
        write_sync_header(s, m->dyn, pts);
        m->cur_sync_ts = pts;   /* per-frame endframe ts are relative to this */
        if (off == 0)
            m->next_sync_us = pts + SYNC_INTERVAL;
        else {
            m->next_sync_us += SYNC_INTERVAL;
            while (pts >= m->next_sync_us)
                m->next_sync_us += SYNC_INTERVAL;
        }
    }
    avio_w8(m->dyn, m->counter << 2);   /* flags byte; bit0=0 = fixed-size (retail) */
    m->block_start = off;
    return 0;
}

/* Emit one logical frame (one EP packet) as one or more chunks, splitting at
 * 4096-byte block boundaries with endframe=0 on partial chunks.
 *
 * final_ef controls the endframe bit on the truly last chunk. */

/* Write 0x00 terminator and zero-pad the open block to MOFLEX_BLOCK bytes.
 * With fixed-size blocks (bit0=0) the demuxer seeks to pos+size after every
 * block, so the padding bytes are simply skipped. */
static void close_block_m(MOFLEXMuxContext *m)
{
    if (m->block_start < 0)
        return;
    avio_w8(m->dyn, 0x00);   /* EP terminator */
    int64_t boundary = m->block_start + MOFLEX_BLOCK;
    while (avio_tell(m->dyn) < boundary)
        avio_w8(m->dyn, 0x00);
    m->block_start = -1;
}

static int emit_frame(AVFormatContext *s, int si, const uint8_t *data, int size,
                      int64_t pts, int final_ef)
{
    MOFLEXMuxContext *m = s->priv_data;
    int off = 0;

    /* FrameType for the endframe header: for video (si 0), the MobiClip frame's
     * I/P flag is the MSB of byte[1] (decoder reads it via ReadU16LE bit 15) —
     * keyframe => FrameType 0, inter => 1. Non-video streams use 0. */
    int frametype = (si < m->nb_video && size > 1 && !(data[1] & 0x80)) ? 1 : 0;

    /* At this frame boundary, if a sync is due, close any block left open by
     * the previous (complete) frame so this frame begins a fresh sync block.
     * This guarantees no frame ever spans a sync-counter change. */
    if (m->block_start >= 0 && pts >= m->next_sync_us)
        close_block_m(m);

    while (off < size) {
        if (m->block_start < 0) {
            int ret = open_block(s, pts, off == 0);
            if (ret < 0)
                return ret;
        }

        int64_t cur   = avio_tell(m->dyn);
        int64_t avail = m->block_start + MOFLEX_BLOCK - cur;
        int remaining = size - off;
        int hdr1 = ep_header_bytes(si, final_ef);
        int hdr0 = ep_header_bytes(si, 0);

        if (hdr1 + remaining <= avail) {
            /* Whole rest of the frame fits in this block. */
            write_ep(m->dyn, si, data + off, remaining, final_ef,
                     frametype, pts - m->cur_sync_ts);
            off = size;
            /* If this exactly fills the block, close it now. */
            if (avio_tell(m->dyn) >= m->block_start + MOFLEX_BLOCK)
                m->block_start = -1;
            /* Otherwise the block stays open for subsequent frames. */
        } else if (avail > hdr0) {
            /* Split: write with endframe=0. */
            int payload = (int)(avail - hdr0);
            int exactly_filled = (payload < remaining);
            if (!exactly_filled)
                payload = remaining - 1;
            if (payload > 0) {
                write_ep(m->dyn, si, data + off, payload, 0, frametype, 0);
                off += payload;
            }
            if (exactly_filled && payload > 0)
                m->block_start = -1;   /* block exactly full, no terminator */
            else
                close_block_m(m);      /* terminator + zero-pad to MOFLEX_BLOCK */
        } else {
            /* Not enough room for even a minimal chunk. */
            close_block_m(m);
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Header: stream resolution + audio encoder init.  No bytes are written
 * to s->pb here — media is buffered to m->dyn and flushed in the trailer.
 * ---------------------------------------------------------------- */
static int moflex_write_header(AVFormatContext *s)
{
    MOFLEXMuxContext *m = s->priv_data;
    int has_video = 0;

    for (int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            has_video = 1;
            if (st->codecpar->width > 1024 || st->codecpar->height > 768)
                av_log(s, AV_LOG_WARNING,
                       "MOFLEX video is %dx%d; 3DS top screen is 400×240.\n",
                       st->codecpar->width, st->codecpar->height);
        }
    }
    if (!has_video)
        return AVERROR(EINVAL);

    m->video_stream_index = -1;
    m->audio_stream_index = -1;
    m->nb_video = 0;
    for (int i = 0; i < s->nb_streams; i++) {
        enum AVMediaType t = s->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO) {
            if (m->nb_video < 2)
                m->video_in[m->nb_video++] = i;
            else
                av_log(s, AV_LOG_WARNING,
                       "MOFLEX supports at most 2 video streams; ignoring stream %d.\n", i);
            if (m->video_stream_index < 0)
                m->video_stream_index = i;
        } else if (t == AVMEDIA_TYPE_AUDIO && m->audio_stream_index < 0)
            m->audio_stream_index = i;
    }

    /* EP stream indices follow descriptor order: video(s), data, audio. */
    m->data_ep  = m->nb_video;
    m->audio_ep = m->nb_video + 1;

    AVStream *vst = s->streams[m->video_stream_index];
    AVRational fr = vst->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = vst->r_frame_rate;
    if ((fr.num <= 0 || fr.den <= 0) &&
        vst->time_base.num > 0 && vst->time_base.den > 0)
        fr = av_inv_q(vst->time_base);
    if (fr.num <= 0 || fr.den <= 0) fr = (AVRational){30, 1};
    m->vfr = fr;

    if (m->audio_stream_index >= 0) {
        AVCodecParameters *apar = s->streams[m->audio_stream_index]->codecpar;
        if (m->audio_codec < 0)
            m->audio_codec = MOBI_AUDIO_ADPCM;
        int ret = mobi_aenc_init(&m->aenc, m->audio_codec,
                                 AV_CODEC_ID_ADPCM_IMA_MOFLEX,
                                 &apar->ch_layout, apar->sample_rate,
                                 /* fastaudio_ds_mods = */ 0);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to init MOFLEX audio encoder\n");
            return ret;
        }
        m->aenc_inited = 1;
        m->audio_rate  = apar->sample_rate;
    }

    m->block_start  = -1;
    m->counter      = 2;            /* retail starts the front table at 2 */
    m->next_sync_us = 1 + SYNC_INTERVAL;  /* first sync at ts=1, next at 1+1s */

    int ret = avio_open_dyn_buf(&m->dyn);
    if (ret < 0)
        return ret;
    return 0;
}

/* ----------------------------------------------------------------
 * Packet write: buffer encoded video frames and (in-muxer) encoded audio
 * packets into the interleave queue with microsecond timestamps.
 * ---------------------------------------------------------------- */
static int q_push(MOFLEXMuxContext *m, int si, const uint8_t *data, int size,
                  int64_t pts)
{
    if (m->nq >= m->qcap) {
        int ncap = m->qcap ? m->qcap * 2 : 1024;
        QEntry *nq = av_realloc_array(m->q, ncap, sizeof(QEntry));
        if (!nq)
            return AVERROR(ENOMEM);
        m->q = nq; m->qcap = ncap;
    }
    uint8_t *copy = av_malloc(size);
    if (!copy)
        return AVERROR(ENOMEM);
    memcpy(copy, data, size);
    m->q[m->nq].data = copy;
    m->q[m->nq].size = size;
    m->q[m->nq].pts  = pts;
    m->q[m->nq].si   = si;
    m->nq++;
    return 0;
}

static int64_t audio_pkt_samples(MOFLEXMuxContext *m, int size)
{
    int ch = m->aenc.channels > 0 ? m->aenc.channels : 1;
    switch (m->aenc.target) {
    case MOBI_AUDIO_PCM:  return size / (ch * 2);
    /* moflex IMA-ADPCM: each ch*0x80-byte block decodes to 256 samples/channel
     * (128 data bytes = 256 4-bit nibbles). Returning 128 here ran the audio
     * clock at half speed, so blocks clumped 2x into oversized chunks (~12.5
     * blocks each) that overran the 3DS audio buffer and crashed playback. */
    case MOBI_AUDIO_ADPCM: { int blk = ch * 0x80; return blk ? (size / blk) * 256 : 0; }
    /* FastAudio: 40 bytes/channel per 256-sample frame. Derive from size so a
     * multi-frame packet doesn't run the audio clock slow (same clumping
     * hazard the ADPCM half-count caused). */
    default: { int blk = ch * 40; return blk ? (size / blk) * 256 : 256; }
    }
}

static int drain_audio(AVFormatContext *s)
{
    MOFLEXMuxContext *m = s->priv_data;
    for (;;) {
        AVPacket apkt = {0};
        int ret = mobi_aenc_recv(&m->aenc, &apkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        int64_t pts = m->audio_rate
            ? 1 + m->audio_samples * 1000000LL / m->audio_rate : 1;
        ret = q_push(m, m->audio_ep, apkt.data, apkt.size, pts); /* audio is last in retail descriptor order */
        m->audio_samples += audio_pkt_samples(m, apkt.size);
        av_packet_unref(&apkt);
        if (ret < 0)
            return ret;
    }
}

static int moflex_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MOFLEXMuxContext *m = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];

    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        int nb = pkt->size / (m->aenc.channels * (int)sizeof(int16_t));
        if (nb <= 0)
            return 0;
        int ret = mobi_aenc_send_pcm(&m->aenc, (const int16_t *)pkt->data, nb);
        if (ret < 0)
            return ret;
        return drain_audio(s);
    }

    /* Video: strip Annex-B framing to get raw MobiClip payload. */
    uint8_t *stripped = NULL;
    int stripped_size = 0;
    if (ff_has_annexb_startcode(pkt->data, pkt->size)) {
        int ret = ff_extract_mobiclip_payload(pkt->data, pkt->size,
                                              &stripped, &stripped_size);
        if (ret < 0)
            return ret;
    }
    const uint8_t *vdata = stripped ? stripped : pkt->data;
    int            vsize = stripped ? stripped_size : pkt->size;

    /* Which video (0 = first/left, 1 = second/right eye)?  The EP stream index
     * is that ordinal; each video has its own frame counter driving pts so the
     * two eyes share a common timeline. */
    int v = (m->nb_video > 1 && pkt->stream_index == m->video_in[1]) ? 1 : 0;
    int64_t pts = 1 + m->vframes[v] * 1000000LL * m->vfr.den / m->vfr.num;
    int ret = q_push(m, v, vdata, vsize, pts);
    m->vframes[v]++;
    if (v == 0)
        m->total_frames++;   /* duration / seek table track the primary video */
    av_free(stripped);
    return ret;
}

/* ----------------------------------------------------------------
 * Trailer: flush audio, interleave, emit media blocks, then write the
 * front (header + seek table) followed by the media buffer.
 * ---------------------------------------------------------------- */
static int q_cmp(const void *a, const void *b)
{
    const QEntry *x = a, *y = b;
    if (x->pts != y->pts) return x->pts < y->pts ? -1 : 1;
    return x->si - y->si;   /* video (0), data (1), audio (2) at equal ts */
}

    /* Real front emitter, used both to measure the front size and to write it. */
static int emit_front(AVFormatContext *s, AVIOContext *pb, const uint8_t *blob,
                      int blob_size)
{
    /* Mirror of emit_frame() for a single stream-2 frame (seek table), starting at pos 0,
     * with the first block carrying the sync header + descriptors. */
    MOFLEXMuxContext *m = s->priv_data;
    int counter = 2;
    int64_t block_start = -1;
    int off = 0;
    int si = m->data_ep;   /* seek table lives in the DATA descriptor stream */

    while (off < blob_size) {
        if (block_start < 0) {
            int64_t bs = avio_tell(pb);   /* block starts at the sync header */
            if (bs == 0)
                write_sync_header(s, pb, 1);
            avio_w8(pb, counter << 2);   /* bit0=0 = fixed-size (retail) */
            block_start = bs;
        }
        int64_t cur   = avio_tell(pb);
        int64_t avail = block_start + MOFLEX_BLOCK - cur;
        int remaining = blob_size - off;
        int hdr1 = ep_header_bytes(si, 1);
        int hdr0 = ep_header_bytes(si, 0);

        if (hdr1 + remaining <= avail) {
            write_ep(pb, si, blob + off, remaining, 1, 0, 0);
            off = blob_size;
            if (avio_tell(pb) >= block_start + MOFLEX_BLOCK)
                block_start = -1;
        } else if (avail > hdr0) {
            int payload = (int)(avail - hdr0);
            int exactly_filled = (payload < remaining);
            if (!exactly_filled)
                payload = remaining - 1;
            if (payload > 0) {
                write_ep(pb, si, blob + off, payload, 0, 0, 0);
                off += payload;
            }
            if (exactly_filled && payload > 0) {
                block_start = -1;
            } else {
                avio_w8(pb, 0x00);
                while (avio_tell(pb) < block_start + MOFLEX_BLOCK) avio_w8(pb, 0x00);
                block_start = -1;
            }
        } else {
            avio_w8(pb, 0x00);
            while (avio_tell(pb) < block_start + MOFLEX_BLOCK) avio_w8(pb, 0x00);
            block_start = -1;
        }
    }
    /* Terminate and pad the final block. */
    if (block_start >= 0) {
        avio_w8(pb, 0x00);
        while (avio_tell(pb) < block_start + MOFLEX_BLOCK) avio_w8(pb, 0x00);
    }
    return 0;
}

static int moflex_write_trailer(AVFormatContext *s)
{
    MOFLEXMuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret = 0;
    uint8_t *media = NULL, *front = NULL, *blob = NULL;

    /* 1. Flush the in-muxer audio encoder. */
    if (m->aenc_inited) {
        mobi_aenc_flush(&m->aenc);
        ret = drain_audio(s);
        if (ret < 0)
            goto end;
    }

    /* 2. Interleave by timestamp and emit media blocks into m->dyn.
     *    Consecutive audio (si=2) entries form one combined EP chunk per run:
     *    shared header (ch*4 bytes from block[0]) + nibbles from all blocks
     *    concatenated (ch*128 bytes each).  This matches the retail format where
     *    one audio chunk per video frame carries ~6 ADPCM subframes at 48kHz/30fps. */
    if (m->nq > 0)
        qsort(m->q, m->nq, sizeof(QEntry), q_cmp);
    for (int i = 0; i < m->nq; ) {
        if (m->q[i].si == m->audio_ep) {
            /* Collect consecutive audio blocks for this video-frame window. */
            int j = i + 1;
            while (j < m->nq && m->q[j].si == m->audio_ep)
                j++;
            int n_blks = j - i;
            int ch       = m->aenc.channels > 0 ? m->aenc.channels : 1;

            uint8_t *combined;
            int combined_sz;
            if (m->aenc.target == MOBI_AUDIO_ADPCM) {
                /* IMA-ADPCM block = [ch*4 header][ch*128 nibbles]; share one
                 * header per chunk and concatenate the nibble payloads. */
                int hdr_sz    = ch * 4;
                int nibble_sz = ch * 128;
                combined_sz = hdr_sz + n_blks * nibble_sz;
                combined = av_malloc(combined_sz);
                if (!combined) { ret = AVERROR(ENOMEM); goto end; }
                memcpy(combined, m->q[i].data, hdr_sz);
                for (int k = 0; k < n_blks; k++)
                    memcpy(combined + hdr_sz + k * nibble_sz,
                           m->q[i + k].data + hdr_sz, nibble_sz);
            } else {
                /* PCM (raw int16) / FastAudio (40 B/ch frames): no per-block
                 * header — just concatenate the raw packet bytes. */
                combined_sz = 0;
                for (int k = 0; k < n_blks; k++)
                    combined_sz += m->q[i + k].size;
                combined = av_malloc(combined_sz);
                if (!combined) { ret = AVERROR(ENOMEM); goto end; }
                int off = 0;
                for (int k = 0; k < n_blks; k++) {
                    memcpy(combined + off, m->q[i + k].data, m->q[i + k].size);
                    off += m->q[i + k].size;
                }
            }

            ret = emit_frame(s, m->audio_ep, combined, combined_sz, m->q[i].pts, 1);
            av_free(combined);
            if (ret < 0) goto end;
            i = j;
        } else {
            ret = emit_frame(s, m->q[i].si, m->q[i].data, m->q[i].size,
                             m->q[i].pts, 1);
            if (ret < 0) goto end;
            i++;
        }
    }
    if (m->block_start >= 0)   /* terminate and pad the final block */
        close_block_m(m);

    int media_size = avio_close_dyn_buf(m->dyn, &media);
    m->dyn = NULL;

    /* 3. Build the seek-table blob (with placeholder offsets) to learn n and
     *    the resulting front size, then bias media offsets and fill it in. */
    AVRational fr = m->vfr;
    int64_t dur_us = m->vfr.num > 0
        ? (int64_t)m->total_frames * 1000000LL * fr.den / fr.num : 0;
    int n = m->n_sp;
    int blob_size = 16 + n * SEEK_ENTRY_SZ;
    blob = av_mallocz(blob_size);
    if (!blob) { ret = AVERROR(ENOMEM); goto end; }
    AV_WL32(blob + 0, n);
    AV_WL32(blob + 4, m->total_frames);
    AV_WL32(blob + 8, (uint32_t)dur_us);
    AV_WL32(blob + 12, 0);

    /* Measure front size by emitting the blob to a throwaway buffer. */
    int64_t front_size;
    {
        AVIOContext *tmp;
        ret = avio_open_dyn_buf(&tmp);
        if (ret < 0) goto end;
        emit_front(s, tmp, blob, blob_size);
        uint8_t *t; front_size = avio_close_dyn_buf(tmp, &t); av_free(t);
    }

    /* Fill entries: file_offset = front_size + relative media sync offset. */
    for (int i = 0; i < n; i++) {
        uint8_t *e = blob + 16 + i * SEEK_ENTRY_SZ;
        AV_WL64(e + 0,  (uint64_t)m->sp_frame[i]);
        AV_WL64(e + 8,  (uint64_t)m->sp_ts[i]);
        AV_WL64(e + 16, (uint64_t)(front_size + m->sp_off[i]));
    }

    av_log(s, AV_LOG_VERBOSE,
           "MOFLEX: %d sync points / %d frames / dur=%"PRId64"us / front=%"PRId64" media=%d\n",
           n, m->total_frames, dur_us, front_size, media_size);

    /* 4. Write front (header + table) then media. */
    {
        AVIOContext *fb;
        ret = avio_open_dyn_buf(&fb);
        if (ret < 0) goto end;
        emit_front(s, fb, blob, blob_size);
        int fsz = avio_close_dyn_buf(fb, &front);
        if (fsz != front_size)
            av_log(s, AV_LOG_WARNING,
                   "MOFLEX front size mismatch: predicted %"PRId64" wrote %d\n",
                   front_size, fsz);
        avio_write(pb, front, fsz);
    }
    avio_write(pb, media, media_size);
    ret = 0;

end:
    av_free(media);
    av_free(front);
    av_free(blob);
    return ret;
}

static void moflex_deinit(AVFormatContext *s)
{
    MOFLEXMuxContext *m = s->priv_data;
    if (m->aenc_inited)
        mobi_aenc_close(&m->aenc);
    for (int i = 0; i < m->nq; i++)
        av_free(m->q[i].data);
    av_freep(&m->q);
    av_freep(&m->sp_frame);
    av_freep(&m->sp_ts);
    av_freep(&m->sp_off);
    if (m->dyn) {
        uint8_t *b;
        avio_close_dyn_buf(m->dyn, &b);
        av_free(b);
        m->dyn = NULL;
    }
}

/* ----------------------------------------------------------------
 * Muxer registration
 * ---------------------------------------------------------------- */
#include "libavutil/opt.h"
#define OFFSET(x) offsetof(MOFLEXMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption moflex_options[] = {
    { "mo_audio",   "MobiClip audio codec",      OFFSET(audio_codec), AV_OPT_TYPE_INT,
      {.i64 = -1}, -1, 2, ENC, "mo_audio" },
    { "fastaudio",  "FastAudio", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, ENC, "mo_audio" },
    { "adpcm",      "ADPCM",     0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, ENC, "mo_audio" },
    { "pcm",        "PCM",       0, AV_OPT_TYPE_CONST, {.i64 = 2}, 0, 0, ENC, "mo_audio" },
    /* VideoWithLayout ImageLayout (low nibble): 0=Interleave3DLeftFirst,
     * 1=Interleave3DRightFirst, 2=TopToBottom3DLeftFirst, 3=TopToBottom3DRightFirst,
     * 4=SideBySide3DLeftFirst, 5=SideBySide3DRightFirst, 6=Simple2D. 3DS 3D uses a
     * single video stream whose frames pack both eyes per this layout. */
    { "mo_layout",  "video 0 ImageLayout: 0-1 interleave3D,2-3 topbottom3D,4-5 sidebyside3D,6 simple2D",
      OFFSET(mo_layout), AV_OPT_TYPE_INT, {.i64 = 6}, 0, 255, ENC },
    { "mo_layout2", "video 1 ImageLayout (only used with a 2nd video stream)", OFFSET(mo_layout2),
      AV_OPT_TYPE_INT, {.i64 = 6}, 0, 255, ENC },
    { "gop_size",   "(ignored) keyframe interval", OFFSET(gop_size), AV_OPT_TYPE_INT,
      {.i64 = 0}, 0, INT_MAX, ENC },
    { "keyint",     "(ignored) keyframe interval", OFFSET(gop_size), AV_OPT_TYPE_INT,
      {.i64 = 0}, 0, INT_MAX, ENC },
    { NULL },
};

static const AVClass moflex_muxer_class = {
    .class_name = "moflex",
    .item_name  = av_default_item_name,
    .option     = moflex_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_moflex_muxer = {
    .p.name         = "moflex",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MobiClip MOFLEX"),
    .p.extensions   = "moflex",
    .priv_data_size = sizeof(MOFLEXMuxContext),
    .p.audio_codec  = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec  = AV_CODEC_ID_MOBICLIP,
    .write_header   = moflex_write_header,
    .write_packet   = moflex_write_packet,
    .write_trailer  = moflex_write_trailer,
    .deinit         = moflex_deinit,
    .p.flags        = AVFMT_NOTIMESTAMPS,
    .p.priv_class   = &moflex_muxer_class,
};
