/*
 * TiVo TY stream muxer
 * Copyright (c) 2026 quatric
 *
 * Container/record layout ported from the "tyffmpeg" ty-enc.c / ty.c /
 * tymaster.c / tyutils.c encoding library, Copyright (C) 2004-2006
 * B.C. <b24cc@yahoo.com>, written against FFmpeg SVN r19344. That code
 * was never merged upstream and is GPL-2.0-or-later licensed, which this
 * file inherits.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#if !CONFIG_GPL
#error "The TiVo TY muxer is GPL-2.0-or-later licensed and requires --enable-gpl"
#endif

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/avassert.h"

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "mpeg.h"

/* ---- TiVo chunk/record container constants (see tydefs.h in tyffmpeg) --- */

#define TY_CHUNK_SIZE           (128 * 1024)
#define TY_CHUNK_STD_PER_SEG    4096
#define TY_CHUNK_VALID_PER_SEG  (TY_CHUNK_STD_PER_SEG - 2)
#define TY_RECHDR_LEN           4
#define TY_RECLEN                16
#define TY_REC_MAX               (TY_CHUNK_SIZE / TY_RECLEN)
#define TY_MASTER_MAGIC           0xf5467abdU
#define TY_MASTER_HDR_LEN         32
#define TY_TIMESCALE              1000000000ULL
#define TY_BITMAPSIZE             2
#define TY_GOP_REC_LEN            (TY_BITMAPSIZE + 8)
#define TY_BITSPERREC             (TY_BITMAPSIZE * 8)
#define TY_MIN_DATA_SPACE         4

/* Record major (subrec) codes */
#define VID_CONT       0x2
#define VID_SEQ        0x7
#define VID_I_FRAME    0x8
#define VID_P_FRAME    0xa
#define VID_B_FRAME    0xb
#define VID_GOP_FRAME  0xc

#define AUD_CONT   0x2
#define AUD_MPEG   0x3
#define AUD_AC3    0x9

/* ISO 13818-2 start codes */
#define MPEG_SEQ_START_CODE   0xb3
#define MPEG_GOP_START_CODE   0xb8
#define MPEG_PIC_START_CODE   0x00
#define MPEG_SEQ_END_CODE     0xb7
#define MPEG_PIC_TYPE_P       2

#define TY_PRIVATE_STREAM_1_ID   0xbd  /* low byte of mpeg.h's PRIVATE_STREAM_1 */
#define PES_LEN_OFFSET     6

typedef struct TYGop {
    uint64_t stamp;
    uint8_t  have;
} TYGop;

typedef struct TYMuxContext {
    const AVClass *class;

    int video_index;
    int audio_index;
    int audio_is_ac3;

    int seekable;
    uint8_t *zero_chunk;   /* CHUNK_SIZE scratch buffer of zero bytes  */
    uint8_t *blank_chunk;  /* CHUNK_SIZE scratch buffer of 0xff bytes  */

    /* record accumulator for the chunk currently being built */
    uint8_t paybuf[TY_CHUNK_SIZE];
    uint8_t headbuf[TY_REC_MAX * TY_RECLEN + TY_RECHDR_LEN];
    int headidx;
    int payidx;
    int have_seq;
    int seqidx;

    int64_t chnk;          /* total chunks written so far (incl. master/blank) */
    int segchunks;         /* valid payload chunks written in the current segment */
    int64_t master_offset; /* byte offset of current segment's master chunk */

    TYGop gop[TY_CHUNK_VALID_PER_SEG];

    /* shared TY-PTS clock, derived from whichever stream (audio or video)
     * is muxed first; see ty_compute_ts() */
    uint64_t tivo_timestamp;
    uint64_t last_ts;
    int64_t  first_pes_ts;
} TYMuxContext;

static void ty_put_pts(uint8_t *buf, int marker, int64_t ts)
{
    buf[0] = (marker << 4) | (((ts >> 30) & 0x07) << 1) | 1;
    AV_WB16(buf + 1, (uint16_t)((((ts >> 15) & 0x7fff) << 1) | 1));
    AV_WB16(buf + 3, (uint16_t)((((ts)       & 0x7fff) << 1) | 1));
}

static uint64_t ty_compute_ts(TYMuxContext *ty, int64_t pts)
{
    int64_t cur_ts;

    if (pts == AV_NOPTS_VALUE)
        pts = 0;
    cur_ts = pts / 90; /* 90kHz PTS -> ms */

    if (ty->first_pes_ts == AV_NOPTS_VALUE) {
        ty->first_pes_ts = cur_ts;
        ty->tivo_timestamp = 0;
    } else {
        if (cur_ts < ty->first_pes_ts)
            cur_ts = ty->first_pes_ts;
        ty->tivo_timestamp = (uint64_t)(cur_ts - ty->first_pes_ts) * (TY_TIMESCALE / 1000);
        if (ty->tivo_timestamp < ty->last_ts)
            ty->tivo_timestamp = ty->last_ts;
        else
            ty->last_ts = ty->tivo_timestamp;
    }
    return ty->tivo_timestamp;
}

/*
 * Build a "Stand-Alone TiVo"-style PES header: start code + stream id,
 * always exactly one PTS field (no DTS, no P-STD extension) at a fixed
 * offset, followed by 0xff stuffing.
 *
 * The companion ty.c demuxer strips a *fixed* number of header bytes per
 * stream type (VIDEO_PES_LENGTH/SERIES2_PES_LENGTH = 16, AC3_PES_LENGTH =
 * 14) and always reads the PTS from a fixed offset (VIDEO_PTS_OFFSET/
 * SA_PTS_OFFSET = 9), so the header size here must be constant per stream
 * type rather than varying with which fields happen to be present -
 * matching what real TiVo hardware and other TY-aware tools expect.
 */
static int ty_build_pes(int stream_id, int stuffing, int64_t pts, uint8_t *out)
{
    int p = 4;

    if (pts == AV_NOPTS_VALUE)
        pts = 0;

    AV_WB24(out, 0x000001);
    out[3] = stream_id;

    AV_WB16(out + p, 0); /* packet length: 0 for video, patched by caller for audio */
    p += 2;
    out[p++] = 0x80; /* mpeg2 marker */
    out[p++] = 0x80; /* pes_flags: PTS present, no DTS */
    out[p++] = 5;    /* pes_header_data_length: PTS only */

    ty_put_pts(out + p, 0x02, pts);
    p += 5;

    while (stuffing-- > 0)
        out[p++] = 0xff;

    return p;
}

static void ty_rec_header(uint8_t *h, int rec_type, int subrec_type, uint32_t size, uint64_t ts)
{
    memset(h, 0, TY_RECLEN);
    AV_WB16(h, (size >> 4) & 0xffff);
    h[2] = ((size & 0xf) << 4) | (subrec_type & 0xf);
    h[3] = rec_type;
    AV_WB64(h + 8, ts);
}

static void ty_rec_setlen(uint8_t *h, uint32_t size)
{
    AV_WB16(h, (size >> 4) & 0xffff);
    h[2] = ((size & 0xf) << 4) | (h[2] & 0xf);
}

static int ty_write_master(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;
    int chunks = ty->segchunks;
    int bitmaps, gopsize, remain, i;
    int64_t cur_pos;
    uint8_t hdr[TY_MASTER_HDR_LEN];
    uint64_t last_stamp = 0;

    if (chunks <= 0)
        return 0;

    bitmaps = (chunks + (TY_BITSPERREC - 1)) / TY_BITSPERREC;
    gopsize = bitmaps * TY_GOP_REC_LEN + 8;
    remain  = TY_CHUNK_SIZE - gopsize - TY_MASTER_HDR_LEN;
    av_assert0(remain >= 0 && remain <= TY_CHUNK_SIZE);

    cur_pos = avio_tell(s->pb);
    avio_seek(s->pb, ty->master_offset, SEEK_SET);

    AV_WB32(hdr +  0, TY_MASTER_MAGIC);
    AV_WB32(hdr +  4, 2);              /* MFS_FILE_STREAM */
    AV_WB32(hdr +  8, TY_CHUNK_SIZE);
    AV_WB32(hdr + 12, chunks * 256 + 4);
    AV_WB32(hdr + 16, 8);
    AV_WB32(hdr + 20, TY_BITMAPSIZE);
    AV_WB32(hdr + 24, TY_TIMESCALE);
    AV_WB32(hdr + 28, gopsize);
    avio_write(s->pb, hdr, sizeof(hdr));

    for (i = 0; i < bitmaps; i++) {
        uint64_t stamp = 0;
        uint8_t bitmap[TY_BITMAPSIZE] = { 0 };
        int bit;

        for (bit = 0; bit < TY_BITSPERREC; bit++) {
            int idx = i * TY_BITSPERREC + bit;
            if (idx >= chunks || !ty->gop[idx].have)
                continue;
            if (!stamp)
                stamp = ty->gop[idx].stamp;
            bitmap[bit / 8] |= 1 << (bit % 8);
        }
        avio_wb64(s->pb, stamp);
        avio_write(s->pb, bitmap, TY_BITMAPSIZE);
    }

    for (i = chunks - 1; i >= 0; i--) {
        if (ty->gop[i].have) {
            last_stamp = ty->gop[i].stamp;
            break;
        }
    }
    avio_wb64(s->pb, last_stamp);

    if (remain > 0)
        avio_write(s->pb, ty->zero_chunk, remain);

    avio_seek(s->pb, cur_pos, SEEK_SET);
    return 0;
}

static int ty_close_segment(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;
    int ret;

    if ((ret = ty_write_master(s)) < 0)
        return ret;

    avio_write(s->pb, ty->blank_chunk, TY_CHUNK_SIZE);
    ty->chnk++;
    ty->segchunks = 0;
    memset(ty->gop, 0, sizeof(ty->gop));

    return 0;
}

static int ty_flush_chunk(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;
    uint32_t headlen;
    int ret;

    if (!ty->headidx)
        return 0;

    if (ty->seekable && !ty->segchunks) {
        ty->master_offset = avio_tell(s->pb);
        avio_write(s->pb, ty->zero_chunk, TY_CHUNK_SIZE);
        ty->chnk++;
    }

    ty->headbuf[0] = ty->headidx & 0xff;
    ty->headbuf[1] = (ty->headidx >> 8) & 0xff;
    if (!ty->have_seq)
        ty->seqidx = 0xffff;
    else
        ty->seqidx |= 0x8000;
    ty->headbuf[2] = ty->seqidx & 0xff;
    ty->headbuf[3] = (ty->seqidx >> 8) & 0xff;

    if (ty->seekable && ty->have_seq && ty->segchunks < TY_CHUNK_VALID_PER_SEG) {
        int real_seqidx = ty->seqidx & 0x7fff;
        uint64_t ts = AV_RB64(&ty->headbuf[TY_RECHDR_LEN + real_seqidx * TY_RECLEN + 8]);
        ty->gop[ty->segchunks].have  = 1;
        ty->gop[ty->segchunks].stamp = ts ? ts : 1;
    }

    headlen = ty->headidx * TY_RECLEN + TY_RECHDR_LEN;
    avio_write(s->pb, ty->headbuf, headlen);
    memset(ty->paybuf + ty->payidx, 0, TY_CHUNK_SIZE - headlen - ty->payidx);
    avio_write(s->pb, ty->paybuf, TY_CHUNK_SIZE - headlen);

    ty->payidx = ty->headidx = 0;
    ty->have_seq = 0;
    ty->chnk++;
    ty->segchunks++;

    if (ty->seekable && ty->segchunks == TY_CHUNK_VALID_PER_SEG) {
        if ((ret = ty_close_segment(s)) < 0)
            return ret;
    }

    return 0;
}

/*
 * Fragment one TY record (header + payload) into the chunk accumulator,
 * splitting across TY_CHUNK_SIZE chunk boundaries as needed.
 *
 * Video records may be split freely: the demuxer's continuation handling
 * for video (subrec VID_CONT) just concatenates raw ES bytes, and only the
 * very first fragment of a video packet ever carries a PES header. Audio
 * records must stay atomic (never split): the demuxer's continuation path
 * for audio (subrec AUD_CONT) is reserved for reassembling a PES header
 * that itself got split across records, not for continuing already-parsed
 * frame payload, so a split audio record would be misparsed on decode.
 */
static int ty_copy_rec(AVFormatContext *s, uint8_t *rechdr,
                        const uint8_t *payload, uint32_t size, int is_seq, int atomic)
{
    TYMuxContext *ty = s->priv_data;
    int ret;

    do {
        uint32_t headlen = (ty->headidx + 1) * TY_RECLEN + TY_RECHDR_LEN;
        int avail = TY_CHUNK_SIZE - (int)headlen - ty->payidx;
        uint32_t remain = 0, cur;

        if (ty->headidx >= TY_REC_MAX - 1 || avail < 0 ||
            (size && avail < TY_MIN_DATA_SPACE) ||
            (atomic && size > (uint32_t)avail)) {
            if ((ret = ty_flush_chunk(s)) < 0)
                return ret;
            headlen = (ty->headidx + 1) * TY_RECLEN + TY_RECHDR_LEN;
            avail = TY_CHUNK_SIZE - (int)headlen - ty->payidx;
            if (atomic && size > (uint32_t)avail) {
                av_log(s, AV_LOG_ERROR,
                       "ty: audio record of %u bytes is too large to fit in one chunk\n", size);
                return AVERROR(EINVAL);
            }
        }

        if (!ty->have_seq && is_seq) {
            ty->have_seq = 1;
            ty->seqidx = ty->headidx;
        }
        is_seq = 0;

        cur = size;
        if (cur > (uint32_t)avail) {
            remain = cur - avail;
            cur = avail;
            ty_rec_setlen(rechdr, cur);
        }

        if (!cur) {
            memcpy(&ty->headbuf[ty->headidx * TY_RECLEN + TY_RECHDR_LEN], rechdr, TY_RECLEN);
            ty->headidx++;
            size = 0;
            continue;
        }

        memcpy(&ty->paybuf[ty->payidx], payload, cur);
        ty->payidx += cur;

        if (!remain) {
            uint32_t postpad = cur % 4;
            postpad = postpad ? 4 - postpad : 0;
            if (postpad) {
                ty_rec_setlen(rechdr, cur + postpad);
                memset(&ty->paybuf[ty->payidx], 0, postpad);
                ty->payidx += postpad;
            }
            memcpy(&ty->headbuf[ty->headidx * TY_RECLEN + TY_RECHDR_LEN], rechdr, TY_RECLEN);
            ty->headidx++;
            size = 0;
        } else {
            memcpy(&ty->headbuf[ty->headidx * TY_RECLEN + TY_RECHDR_LEN], rechdr, TY_RECLEN);
            ty->headidx++;
            if ((ret = ty_flush_chunk(s)) < 0)
                return ret;
            payload += cur;
            size = remain;
            ty_rec_setlen(rechdr, size);
            rechdr[2] = (rechdr[2] & 0xf0) | VID_CONT; /* == AUD_CONT */
        }
    } while (size);

    return 0;
}

static int ty_write_audio(AVFormatContext *s, AVPacket *pkt)
{
    TYMuxContext *ty = s->priv_data;
    uint8_t hdr[16];
    uint8_t rechdr[TY_RECLEN];
    uint8_t *tmp;
    int hlen, total, ret;
    int64_t pts = pkt->pts;
    uint64_t ts;

    if (pts == AV_NOPTS_VALUE)
        pts = pkt->dts;

    hlen = ty_build_pes(ty->audio_is_ac3 ? TY_PRIVATE_STREAM_1_ID : AUDIO_ID,
                         ty->audio_is_ac3 ? 0 : 2, pts, hdr);

    total = hlen + pkt->size;
    AV_WB16(hdr + 4, total - PES_LEN_OFFSET);

    ts = ty_compute_ts(ty, pts);

    tmp = av_malloc(total);
    if (!tmp)
        return AVERROR(ENOMEM);
    memcpy(tmp, hdr, hlen);
    memcpy(tmp + hlen, pkt->data, pkt->size);

    ty_rec_header(rechdr, AUDIO_ID, ty->audio_is_ac3 ? AUD_AC3 : AUD_MPEG, total, ts);
    ret = ty_copy_rec(s, rechdr, tmp, total, 0, 1);
    av_free(tmp);
    if (ret < 0)
        return ret;

    return 0;
}

static int ty_find_start(const uint8_t *buf, int start, int size)
{
    int i;
    for (i = start; i + 3 < size; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            return i;
    return -1;
}

static int ty_search_code(const uint8_t *buf, int code, int start, int size)
{
    int i;
    for (i = start; i + 3 < size; i++)
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && buf[i + 3] == code)
            return i;
    return -1;
}

static int ty_write_video(AVFormatContext *s, AVPacket *pkt)
{
    TYMuxContext *ty = s->priv_data;
    uint8_t rechdr[TY_RECLEN];
    uint8_t hdr[16];
    const uint8_t *buf = pkt->data;
    int len = pkt->size, off = 0;
    int64_t pts = pkt->pts;
    uint64_t ts;
    int hlen, pos, code, ret;

    if (pts == AV_NOPTS_VALUE)
        pts = pkt->dts;
    ts = ty_compute_ts(ty, pts);

    hlen = ty_build_pes(VIDEO_ID, 2, pts, hdr);

again:
    pos = ty_find_start(buf, off, len);
    if (pos < 0) {
        return 0;
    }
    code = buf[pos + 3];

    if (code == MPEG_SEQ_START_CODE || code == MPEG_GOP_START_CODE) {
        int seq_pos = (code == MPEG_SEQ_START_CODE) ? pos : -1;
        int gop_pos = (code == MPEG_GOP_START_CODE) ? pos :
                      ty_search_code(buf, MPEG_GOP_START_CODE, pos, len);
        int pic_pos = (gop_pos >= 0) ?
                      ty_search_code(buf, MPEG_PIC_START_CODE, gop_pos, len) : -1;

        if (gop_pos < 0 || pic_pos < 0) {
            av_log(s, AV_LOG_ERROR,
                   "ty: malformed MPEG-2 GOP (seq=%d gop=%d pic=%d); "
                   "the video encoder must emit a sequence+GOP header before every I-frame\n",
                   seq_pos, gop_pos, pic_pos);
            return AVERROR_INVALIDDATA;
        }

        if (seq_pos != -1) {
            int seqlen = gop_pos - seq_pos;
            int total = hlen + seqlen;
            uint8_t *tmp = av_malloc(total);
            if (!tmp)
                return AVERROR(ENOMEM);
            memcpy(tmp, hdr, hlen);
            memcpy(tmp + hlen, buf + seq_pos, seqlen);
            ty_rec_header(rechdr, VIDEO_ID, VID_SEQ, total, ts);
            ret = ty_copy_rec(s, rechdr, tmp, total, 1, 0);
            av_free(tmp);
            if (ret < 0)
                return ret;
        }

        ty_rec_header(rechdr, VIDEO_ID, VID_GOP_FRAME, pic_pos - gop_pos, ts);
        if ((ret = ty_copy_rec(s, rechdr, buf + gop_pos, pic_pos - gop_pos, 0, 0)) < 0)
            return ret;

        ty_rec_header(rechdr, VIDEO_ID, VID_I_FRAME, len - pic_pos, ts);
        if ((ret = ty_copy_rec(s, rechdr, buf + pic_pos, len - pic_pos, 0, 0)) < 0)
            return ret;

        return 0;
    }

    if (code == MPEG_SEQ_END_CODE || code != MPEG_PIC_START_CODE) {
        /* not a start code we handle (sequence-end, extension, user data);
         * skip it and keep looking within this packet */
        off = pos + 4;
        if (off >= len) {
            return 0;
        }
        goto again;
    }

    /* P or B picture continuing an existing GOP */
    {
        int coding_type, major, total;
        uint8_t *tmp;

        if (pos + 5 >= len)
            return AVERROR_INVALIDDATA;
        coding_type = (AV_RB16(buf + pos + 4) >> 3) & 3;
        major = (coding_type == MPEG_PIC_TYPE_P) ? VID_P_FRAME : VID_B_FRAME;

        total = hlen + (len - pos);
        tmp = av_malloc(total);
        if (!tmp)
            return AVERROR(ENOMEM);
        memcpy(tmp, hdr, hlen);
        memcpy(tmp + hlen, buf + pos, len - pos);

        ty_rec_header(rechdr, VIDEO_ID, major, total, ts);
        ret = ty_copy_rec(s, rechdr, tmp, total, 0, 0);
        av_free(tmp);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int ty_write_header(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;
    int i;

    ty->video_index = -1;
    ty->audio_index = -1;
    ty->first_pes_ts = AV_NOPTS_VALUE;

    if (s->nb_streams < 1 || s->nb_streams > 2) {
        av_log(s, AV_LOG_ERROR,
               "ty: needs exactly one MPEG-2 video stream and at most one MP2/AC-3 audio stream\n");
        return AVERROR(EINVAL);
    }

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (ty->video_index >= 0 || st->codecpar->codec_id != AV_CODEC_ID_MPEG2VIDEO) {
                av_log(s, AV_LOG_ERROR, "ty: video stream must be a single MPEG-2 video stream\n");
                return AVERROR(EINVAL);
            }
            ty->video_index = i;
            avpriv_set_pts_info(st, 64, 1, 90000);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (ty->audio_index >= 0 ||
                (st->codecpar->codec_id != AV_CODEC_ID_MP2 &&
                 st->codecpar->codec_id != AV_CODEC_ID_AC3)) {
                av_log(s, AV_LOG_ERROR, "ty: audio stream must be a single MP2 or AC-3 stream\n");
                return AVERROR(EINVAL);
            }
            ty->audio_index = i;
            ty->audio_is_ac3 = st->codecpar->codec_id == AV_CODEC_ID_AC3;
            avpriv_set_pts_info(st, 64, 1, 90000);
        } else {
            av_log(s, AV_LOG_ERROR, "ty: unsupported stream type\n");
            return AVERROR(EINVAL);
        }
    }

    if (ty->video_index < 0) {
        av_log(s, AV_LOG_ERROR, "ty: a video stream is required\n");
        return AVERROR(EINVAL);
    }

    ty->seqidx = 0xffff;

    ty->seekable = !!(s->pb->seekable & AVIO_SEEKABLE_NORMAL);
    if (ty->seekable) {
        ty->zero_chunk  = av_mallocz(TY_CHUNK_SIZE);
        ty->blank_chunk = av_malloc(TY_CHUNK_SIZE);
        if (!ty->zero_chunk || !ty->blank_chunk)
            return AVERROR(ENOMEM);
        memset(ty->blank_chunk, 0xff, TY_CHUNK_SIZE);
    } else {
        av_log(s, AV_LOG_WARNING,
               "ty: output is not seekable; writing a raw payload stream without "
               "TiVo master/GOP index chunks (still readable by this demuxer)\n");
    }

    return 0;
}

static int ty_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    TYMuxContext *ty = s->priv_data;

    if (pkt->stream_index == ty->video_index)
        return ty_write_video(s, pkt);
    if (pkt->stream_index == ty->audio_index)
        return ty_write_audio(s, pkt);
    return 0;
}

static int ty_write_trailer(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;
    int ret;

    if ((ret = ty_flush_chunk(s)) < 0)
        return ret;

    if (ty->seekable && ty->segchunks > 0) {
        if ((ret = ty_close_segment(s)) < 0)
            return ret;
    }

    return 0;
}

static void ty_deinit(AVFormatContext *s)
{
    TYMuxContext *ty = s->priv_data;

    av_freep(&ty->zero_chunk);
    av_freep(&ty->blank_chunk);
}

const FFOutputFormat ff_ty_muxer = {
    .p.name         = "ty",
    .p.long_name    = NULL_IF_CONFIG_SMALL("TiVo TY Stream"),
    .p.mime_type    = "video/x-tivo-mpeg",
    .p.extensions   = "ty,ty+",
    .priv_data_size = sizeof(TYMuxContext),
    .p.audio_codec  = AV_CODEC_ID_MP2,
    .p.video_codec  = AV_CODEC_ID_MPEG2VIDEO,
    .write_header   = ty_write_header,
    .write_packet   = ty_write_packet,
    .write_trailer  = ty_write_trailer,
    .deinit         = ty_deinit,
};
