/*
 * TiVo ty stream demuxer
 * Copyright (c) 2005 VLC authors and VideoLAN
 * Copyright (c) 2005 by Neal Symms (tivo@freakinzoo.com) - February 2005
 * based on code by Christopher Wingert for tivo-mplayer
 * tivo(at)wingert.org, February 2003
 * Copyright (c) 2017 Paul B Mahol
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

#include "libavutil/attributes.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"
#include "mpeg.h"
#include "mpegts.h"

#define SERIES1_PES_LENGTH  11    /* length of audio PES hdr on S1 */
#define SERIES2_PES_LENGTH  17    /* length of audio PES hdr on S2 */
#define AC3_PES_LENGTH      14    /* length of audio PES hdr for AC3 */
#define VIDEO_PES_LENGTH    16    /* length of video PES header */
#define DTIVO_PTS_OFFSET    6     /* offs into PES for MPEG PTS on DTivo */
#define SA_PTS_OFFSET       9     /* offset into PES for MPEG PTS on SA */
#define AC3_PTS_OFFSET      9     /* offset into PES for AC3 PTS on DTivo */
#define VIDEO_PTS_OFFSET    9     /* offset into PES for video PTS on all */
#define AC3_PKT_LENGTH      1536  /* size of TiVo AC3 pkts (w/o PES hdr) */

static const uint8_t ty_VideoPacket[]     = { 0x00, 0x00, 0x01, 0xe0 };
static const uint8_t ty_MPEGAudioPacket[] = { 0x00, 0x00, 0x01, 0xc0 };
static const uint8_t ty_AC3AudioPacket[]  = { 0x00, 0x00, 0x01, 0xbd };

#define TIVO_PES_FILEID   0xf5467abd
#define CHUNK_SIZE        (128 * 1024)
#define CHUNK_PEEK_COUNT  3      /* number of chunks to probe */

typedef struct TyRecHdr {
    int32_t   rec_size;
    uint8_t   ex[2];
    uint8_t   rec_type;
    uint8_t   subrec_type;
    uint64_t  ty_pts;            /* TY PTS in the record header */
} TyRecHdr;

typedef enum {
    TIVO_TYPE_UNKNOWN,
    TIVO_TYPE_SA,
    TIVO_TYPE_DTIVO
} TiVo_type;

typedef enum {
    TIVO_SERIES_UNKNOWN,
    TIVO_SERIES1,
    TIVO_SERIES2
} TiVo_series;

typedef enum {
    TIVO_AUDIO_UNKNOWN,
    TIVO_AUDIO_AC3,
    TIVO_AUDIO_MPEG
} TiVo_audio;

typedef struct TYDemuxContext {
    unsigned        cur_chunk;
    unsigned        cur_chunk_pos;
    int64_t         cur_pos;
    TiVo_type       tivo_type;        /* TiVo type (SA / DTiVo) */
    TiVo_series     tivo_series;      /* Series1 or Series2 */
    TiVo_audio      audio_type;       /* AC3 or MPEG */
    int             pes_length;       /* Length of Audio PES header */
    int             pts_offset;       /* offset into audio PES of PTS */
    uint8_t         pes_buffer[20];   /* holds incomplete pes headers */
    int             pes_buf_cnt;      /* how many bytes in our buffer */
    size_t          ac3_pkt_size;     /* length of ac3 pkt we've seen so far */
    uint64_t        last_ty_pts;      /* last TY timestamp we've seen */

    int64_t         first_audio_pts;
    int64_t         last_audio_pts;
    int64_t         last_video_pts;

    TyRecHdr       *rec_hdrs;         /* record headers array */
    int             cur_rec;          /* current record in this chunk */
    int             num_recs;         /* number of recs in this chunk */
    int             first_chunk;

    uint8_t        *probe_chunks;      /* replay buffer for pipe inputs */
    int             probe_chunk_count;
    int             probe_chunk_index;
    int             is_tmf;

    MpegTSContext  *mpegts;            /* Series 3 transport stream parser */
    int             is_series3;
    int             series3_have_master;
    int             series3_chunks_left;
    int             series3_payload_pos;
    int             series3_payload_size;
    int             series3_flushed;
    AVPacket       *series3_packet;

    uint8_t         chunk[CHUNK_SIZE];
    uint8_t         series3_payload[CHUNK_SIZE];
} TYDemuxContext;

static int ty_read_series3_packet(AVFormatContext *s, AVPacket *pkt);

static int ty_probe(const AVProbeData *p)
{
    int i;

    if (p->buf_size >= 512 && !memcmp(p->buf + 257, "ustar", 5)) {
        int pos = 0;

        while (pos + 512 <= p->buf_size &&
               !memcmp(p->buf + pos + 257, "ustar", 5)) {
            uint64_t size = 0;
            int valid = 0;

            for (int j = 0; j < 12; j++) {
                const int c = p->buf[pos + 124 + j];

                if (c >= '0' && c <= '7') {
                    size = size * 8 + c - '0';
                    valid = 1;
                } else if (c && c != ' ') {
                    valid = 0;
                    break;
                }
            }
            if (!valid)
                break;

            pos += 512;
            if (strncmp((const char *)p->buf + pos - 512,
                        "showing.xml", 100))
                break;
            pos += FFALIGN(size, 512);
        }

        if (pos + 12 <= p->buf_size &&
            AV_RB32(p->buf + pos) == TIVO_PES_FILEID)
            return AVPROBE_SCORE_MAX;
        if (av_match_ext(p->filename, "tmf"))
            return AVPROBE_SCORE_EXTENSION;
    }

    for (i = 0; i + 12 < p->buf_size; i += CHUNK_SIZE) {
        if (AV_RB32(p->buf + i) == TIVO_PES_FILEID &&
            AV_RB32(p->buf + i + 8) == CHUNK_SIZE) {
            return AVPROBE_SCORE_MAX;
        }

        /* A non-seekable TY stream has no master/index chunks and starts
         * directly with a payload chunk.  Validate the record table rather
         * than letting the MPEG audio probe mistake its embedded MP2 data for
         * a raw elementary stream. */
        if (i + 4 <= p->buf_size) {
            const uint8_t *chunk = p->buf + i;
            const int available = p->buf_size - i;
            const int num_recs = (chunk[3] & 0x80) ? AV_RL16(chunk)
                                                   : chunk[0];
            const int seq_word = AV_RL16(chunk + 2);
            int recognized = 0;
            int64_t payload = 0;

            if (num_recs >= 5 && num_recs < (CHUNK_SIZE - 4) / 16 &&
                4 + 16 * num_recs <= available &&
                (!(seq_word & 0x8000) || (seq_word & 0x7fff) < num_recs)) {
                for (int rec = 0; rec < num_recs; rec++) {
                    const uint8_t *h = chunk + 4 + 16 * rec;
                    const int rec_type = h[3];
                    const int subrec_type = h[2] & 0x0f;
                    int rec_size = 0;

                    if (!(h[0] & 0x80))
                        rec_size = ((h[0] << 8) | h[1]) << 4 | (h[2] >> 4);
                    payload += rec_size;
                    if ((rec_type == VIDEO_ID && subrec_type <= 0x0c) ||
                        (rec_type == AUDIO_ID &&
                         (subrec_type == 0x02 || subrec_type == 0x03 ||
                          subrec_type == 0x04 || subrec_type == 0x09)))
                        recognized++;
                }
                if (recognized >= 5 &&
                    payload <= CHUNK_SIZE - 4 - 16 * num_recs) {
                    if (!(seq_word & 0x8000) ||
                        (chunk[4 + 16 * (seq_word & 0x7fff) + 3] == VIDEO_ID &&
                         (chunk[4 + 16 * (seq_word & 0x7fff) + 2] & 0x0f) == 0x07))
                        return AVPROBE_SCORE_MAX - 1;
                }
            }
        }
    }

    return 0;
}

static TyRecHdr *parse_chunk_headers(const uint8_t *buf,
                                     int num_recs)
{
    TyRecHdr *hdrs, *rec_hdr;
    int i;

    hdrs = av_calloc(num_recs, sizeof(TyRecHdr));
    if (!hdrs)
        return NULL;

    for (i = 0; i < num_recs; i++) {
        const uint8_t *record_header = buf + (i * 16);

        rec_hdr = &hdrs[i];     /* for brevity */
        rec_hdr->rec_type = record_header[3];
        rec_hdr->subrec_type = record_header[2] & 0x0f;
        if ((record_header[0] & 0x80) == 0x80) {
            uint8_t b1, b2;

            /* marker bit 2 set, so read extended data */
            b1 = (((record_header[0] & 0x0f) << 4) |
                  ((record_header[1] & 0xf0) >> 4));
            b2 = (((record_header[1] & 0x0f) << 4) |
                  ((record_header[2] & 0xf0) >> 4));

            rec_hdr->ex[0] = b1;
            rec_hdr->ex[1] = b2;
            rec_hdr->rec_size = 0;
            rec_hdr->ty_pts = 0;
        } else {
            rec_hdr->rec_size = (record_header[0] << 8 |
                                 record_header[1]) << 4 |
                                (record_header[2] >> 4);
            rec_hdr->ty_pts = AV_RB64(&record_header[8]);
        }
    }
    return hdrs;
}

static int find_es_header(const uint8_t *header,
                          const uint8_t *buffer, int search_len)
{
    int count;

    for (count = 0; count < search_len; count++) {
        if (!memcmp(&buffer[count], header, 4))
            return count;
    }
    return -1;
}

static int find_next_start_code(const uint8_t *buffer, int start, int size)
{
    const int end = FFMIN(size, start + 64);

    for (int i = start; i + 3 < end; i++)
        if (!buffer[i] && !buffer[i + 1] && buffer[i + 2] == 1)
            return i;
    return -1;
}

static int analyze_chunk(AVFormatContext *s, const uint8_t *chunk)
{
    TYDemuxContext *ty = s->priv_data;
    int num_recs, i;
    TyRecHdr *hdrs;
    int num_6e0, num_be0, num_9c0, num_3c0;

    /* skip if it's a Part header */
    if (AV_RB32(&chunk[0]) == TIVO_PES_FILEID)
        return 0;

    /* number of records in chunk (we ignore high order byte;
     * rarely are there > 256 chunks & we don't need that many anyway) */
    num_recs = chunk[0];
    if (num_recs < 5) {
        /* try again with the next chunk.  Sometimes there are dead ones */
        return 0;
    }

    chunk += 4;       /* skip past rec count & SEQ bytes */
    ff_dlog(s, "probe: chunk has %d recs\n", num_recs);
    hdrs = parse_chunk_headers(chunk, num_recs);
    if (!hdrs)
        return AVERROR(ENOMEM);

    /* scan headers.
     * 1. check video packets.  Presence of 0x6e0 means S1.
     *    No 6e0 but have be0 means S2.
     * 2. probe for audio 0x9c0 vs 0x3c0 (AC3 vs Mpeg)
     *    If AC-3, then we have DTivo.
     *    If MPEG, search for PTS offset.  This will determine SA vs. DTivo.
     */
    num_6e0 = num_be0 = num_9c0 = num_3c0 = 0;
    for (i = 0; i < num_recs; i++) {
        switch (hdrs[i].subrec_type << 8 | hdrs[i].rec_type) {
        case 0x6e0:
            num_6e0++;
            break;
        case 0xbe0:
            num_be0++;
            break;
        case 0x3c0:
            num_3c0++;
            break;
        case 0x9c0:
            num_9c0++;
            break;
        }
    }
    ff_dlog(s, "probe: chunk has %d 0x6e0 recs, %d 0xbe0 recs.\n",
            num_6e0, num_be0);

    /* set up our variables */
    if (num_6e0 > 0) {
        ff_dlog(s, "detected Series 1 Tivo\n");
        ty->tivo_series = TIVO_SERIES1;
        ty->pes_length = SERIES1_PES_LENGTH;
    } else {
        ff_dlog(s, "detected Series 2/3 Tivo\n");
        ty->tivo_series = TIVO_SERIES2;
        ty->pes_length = SERIES2_PES_LENGTH;
    }
    if (num_9c0 > 0) {
        ff_dlog(s, "detected AC-3 Audio (DTivo)\n");
        ty->audio_type = TIVO_AUDIO_AC3;
        ty->tivo_type = TIVO_TYPE_DTIVO;
        ty->pts_offset = AC3_PTS_OFFSET;
        ty->pes_length = AC3_PES_LENGTH;
    } else if (num_3c0 > 0) {
        ty->audio_type = TIVO_AUDIO_MPEG;
        ff_dlog(s, "detected MPEG Audio\n");
    }

    /* if tivo_type still unknown, we can check PTS location
     * in MPEG packets to determine tivo_type */
    if (ty->tivo_type == TIVO_TYPE_UNKNOWN) {
        uint32_t data_offset = 16 * num_recs;

        for (i = 0; i < num_recs; i++) {
            if (data_offset + hdrs[i].rec_size > CHUNK_SIZE)
                break;

            if ((hdrs[i].subrec_type << 8 | hdrs[i].rec_type) == 0x3c0 && hdrs[i].rec_size > 15) {
                /* first make sure we're aligned */
                int pes_offset = find_es_header(ty_MPEGAudioPacket,
                        &chunk[data_offset], 5);
                if (pes_offset >= 0) {
                    /* pes found. on SA, PES has hdr data at offset 6, not PTS. */
                    if ((chunk[data_offset + 6 + pes_offset] & 0x80) == 0x80) {
                        /* S1SA or S2(any) Mpeg Audio (PES hdr, not a PTS start) */
                        if (ty->tivo_series == TIVO_SERIES1)
                            ff_dlog(s, "detected Stand-Alone Tivo\n");
                        ty->tivo_type = TIVO_TYPE_SA;
                        ty->pts_offset = SA_PTS_OFFSET;
                    } else {
                        if (ty->tivo_series == TIVO_SERIES1)
                            ff_dlog(s, "detected DirecTV Tivo\n");
                        ty->tivo_type = TIVO_TYPE_DTIVO;
                        ty->pts_offset = DTIVO_PTS_OFFSET;
                    }
                    break;
                }
            }
            data_offset += hdrs[i].rec_size;
        }
    }
    av_free(hdrs);

    return 0;
}

static int tmf_skip(const uint8_t *buf)
{
    int skip = 0;

    while (skip + 512 <= CHUNK_SIZE &&
           !memcmp(buf + skip + 257, "ustar", 5)) {
        const uint8_t *header = buf + skip;
        uint64_t size = 0;
        int valid = 0;

        for (int i = 0; i < 12; i++) {
            const int c = header[124 + i];

            if (c >= '0' && c <= '7') {
                size = size * 8 + c - '0';
                valid = 1;
            } else if (c && c != ' ') {
                return AVERROR_INVALIDDATA;
            }
        }
        if (!valid)
            return AVERROR_INVALIDDATA;

        skip += 512;
        if (strncmp((const char *)header, "showing.xml", 100))
            return skip;
        if (size > CHUNK_SIZE)
            return AVERROR_INVALIDDATA;
        if (FFALIGN(size, 512) > CHUNK_SIZE - skip)
            return AVERROR_INVALIDDATA;
        skip += FFALIGN(size, 512);
    }

    return skip;
}

static int read_source_chunk(AVFormatContext *s, uint8_t *buf)
{
    TYDemuxContext *ty = s->priv_data;
    int ret, skip;

    ret = ffio_read_size(s->pb, buf, CHUNK_SIZE);
    if (ret < 0)
        return ret;

    if (!ty->is_tmf && !memcmp(buf + 257, "ustar", 5))
        ty->is_tmf = 1;
    if (!ty->is_tmf)
        return 0;

    skip = tmf_skip(buf);
    if (skip < 0)
        return skip;
    if (!skip)
        return 0;

    memmove(buf, buf + skip, CHUNK_SIZE - skip);
    ret = ffio_read_size(s->pb, buf + CHUNK_SIZE - skip, skip);
    return ret < 0 ? ret : 0;
}

static int ty_read_header(AVFormatContext *s)
{
    TYDemuxContext *ty = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st, *ast;
    int i, ret = 0;

    ty->first_audio_pts = AV_NOPTS_VALUE;
    ty->last_audio_pts = AV_NOPTS_VALUE;
    ty->last_video_pts = AV_NOPTS_VALUE;
    ty->probe_chunks = av_malloc(CHUNK_PEEK_COUNT * CHUNK_SIZE);
    if (!ty->probe_chunks)
        return AVERROR(ENOMEM);

    for (i = 0; i < CHUNK_PEEK_COUNT; i++) {
        int read_size = read_source_chunk(s, ty->chunk);

        if (read_size < 0)
            break;
        memcpy(ty->probe_chunks + i * CHUNK_SIZE, ty->chunk, CHUNK_SIZE);
        ty->probe_chunk_count++;

        if (AV_RB32(ty->chunk) == TIVO_PES_FILEID &&
            AV_RB32(ty->chunk + 4) >= 3) {
            ty->is_series3 = 1;
            break;
        }

        ret = analyze_chunk(s, ty->chunk);
        if (ret < 0)
            return ret;
        if (ty->tivo_series != TIVO_SERIES_UNKNOWN &&
            ty->audio_type  != TIVO_AUDIO_UNKNOWN &&
            ty->tivo_type   != TIVO_TYPE_UNKNOWN)
            break;
    }

    if (ty->is_series3) {
        ty->mpegts = avpriv_mpegts_parse_open(s);
        if (!ty->mpegts)
            return AVERROR(ENOMEM);
        ty->first_chunk = 1;

        ty->series3_packet = av_packet_alloc();
        if (!ty->series3_packet)
            return AVERROR(ENOMEM);
        ret = ty_read_series3_packet(s, ty->series3_packet);
        if (ret < 0)
            return ret;
        return 0;
    }

    if (ty->tivo_series == TIVO_SERIES_UNKNOWN ||
        (ty->audio_type != TIVO_AUDIO_UNKNOWN &&
         ty->tivo_type == TIVO_TYPE_UNKNOWN))
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_MPEG2VIDEO;
    ffstream(st)->need_parsing = AVSTREAM_PARSE_FULL_RAW;
    avpriv_set_pts_info(st, 64, 1, 90000);

    if (ty->audio_type != TIVO_AUDIO_UNKNOWN) {
        ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

        if (ty->audio_type == TIVO_AUDIO_MPEG) {
            ast->codecpar->codec_id = AV_CODEC_ID_MP2;
            ffstream(ast)->need_parsing = AVSTREAM_PARSE_FULL_RAW;
        } else {
            ast->codecpar->codec_id = AV_CODEC_ID_AC3;
        }
        avpriv_set_pts_info(ast, 64, 1, 90000);
    }

    ty->first_chunk = 1;

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        if (avio_seek(pb, 0, SEEK_SET) < 0)
            return AVERROR(EIO);
        av_freep(&ty->probe_chunks);
        ty->probe_chunk_count = 0;
    }

    return 0;
}

static int read_raw_chunk(AVFormatContext *s)
{
    TYDemuxContext *ty = s->priv_data;
    int read_size;

    if (ty->probe_chunk_index < ty->probe_chunk_count) {
        memcpy(ty->chunk,
               ty->probe_chunks + ty->probe_chunk_index * CHUNK_SIZE,
               CHUNK_SIZE);
        ty->probe_chunk_index++;
        read_size = CHUNK_SIZE;
        if (ty->probe_chunk_index == ty->probe_chunk_count)
            av_freep(&ty->probe_chunks);
    } else {
        if (avio_feof(s->pb))
            return AVERROR_EOF;
        read_size = read_source_chunk(s, ty->chunk) < 0 ? 0 : CHUNK_SIZE;
    }
    ty->cur_chunk++;

    if (read_size != CHUNK_SIZE)
        return read_size < 0 ? read_size : AVERROR_EOF;
    return 0;
}

static int get_chunk(AVFormatContext *s)
{
    TYDemuxContext *ty = s->priv_data;
    int read_size, num_recs;

    ff_dlog(s, "parsing ty chunk #%d\n", ty->cur_chunk);

    /* if we have left-over filler space from the last chunk, get that */
    /* read the TY packet header */
    if (read_raw_chunk(s) < 0)
        return AVERROR_EOF;
    read_size = CHUNK_SIZE;

    if ((read_size < 4) || (AV_RB32(ty->chunk) == 0)) {
        return AVERROR_EOF;
    }

    /* check if it's a PART Header */
    if (AV_RB32(ty->chunk) == TIVO_PES_FILEID) {
        /* skip master chunk and read new chunk */
        return get_chunk(s);
    }

    /* number of records in chunk (8- or 16-bit number) */
    if (ty->chunk[3] & 0x80) {
        /* 16 bit rec cnt */
        ty->num_recs = num_recs = (ty->chunk[1] << 8) + ty->chunk[0];
    } else {
        /* 8 bit reclen - TiVo 1.3 format */
        ty->num_recs = num_recs = ty->chunk[0];
    }
    ty->cur_rec = 0;
    ty->first_chunk = 0;

    ff_dlog(s, "chunk has %d records\n", num_recs);
    ty->cur_chunk_pos = 4;

    av_freep(&ty->rec_hdrs);

    if (num_recs * 16 >= CHUNK_SIZE - 4)
        return AVERROR_INVALIDDATA;

    ty->rec_hdrs = parse_chunk_headers(ty->chunk + 4, num_recs);
    if (!ty->rec_hdrs)
        return AVERROR(ENOMEM);
    ty->cur_chunk_pos += 16 * num_recs;

    return 0;
}

static int parse_series3_master(TYDemuxContext *ty)
{
    const unsigned chunk_count = AV_RB32(ty->chunk + 12);
    unsigned segment_length;

    if (AV_RB32(ty->chunk + 4) != 3 ||
        AV_RB32(ty->chunk + 8) != CHUNK_SIZE ||
        chunk_count < 4 || (chunk_count - 4) % 256)
        return AVERROR_INVALIDDATA;

    segment_length = (chunk_count - 4) / 256;
    if (!segment_length)
        return AVERROR_INVALIDDATA;

    ty->series3_have_master = 1;
    ty->series3_chunks_left = segment_length - 1;
    return 0;
}

static int get_series3_payload(AVFormatContext *s)
{
    TYDemuxContext *ty = s->priv_data;

    for (;;) {
        int record_pos = 0;

        if (read_raw_chunk(s) < 0)
            return AVERROR_EOF;

        if (AV_RB32(ty->chunk) == TIVO_PES_FILEID) {
            int ret = parse_series3_master(ty);

            if (ret < 0)
                return ret;
            continue;
        }

        if (ty->series3_have_master) {
            if (ty->series3_chunks_left <= 0)
                continue;
            ty->series3_chunks_left--;
        }

        ty->series3_payload_pos  = 0;
        ty->series3_payload_size = 0;

        while (record_pos + 8 <= CHUNK_SIZE) {
            const unsigned type_size = AV_RB32(ty->chunk + record_pos);
            const unsigned total_size = type_size >> 12;
            const unsigned type = type_size & 0xfff;
            const unsigned fragments = AV_RB32(ty->chunk + record_pos + 4);

            if (!total_size)
                break;
            if (total_size < 8 || total_size > CHUNK_SIZE - record_pos ||
                fragments > (total_size - 8) / 8)
                return AVERROR_INVALIDDATA;

            if (type == 1) {
                for (unsigned i = 0; i < fragments; i++) {
                    const int table = record_pos + 8 + 8 * i;
                    const unsigned offset = AV_RB32(ty->chunk + table);
                    const unsigned size = AV_RB32(ty->chunk + table + 4);

                    if (!offset || size > CHUNK_SIZE ||
                        offset > CHUNK_SIZE - size ||
                        ty->series3_payload_size > CHUNK_SIZE - size)
                        return AVERROR_INVALIDDATA;
                    memcpy(ty->series3_payload + ty->series3_payload_size,
                           ty->chunk + offset, size);
                    ty->series3_payload_size += size;
                }
            } else if (type != 2 && type != 3) {
                break;
            }

            record_pos += total_size;
        }

        if (!ty->series3_payload_size)
            continue;
        if (ty->series3_payload_size % 188 || ty->series3_payload[0] != 0x47)
            return AVERROR_INVALIDDATA;
        return 0;
    }
}

static int ty_read_series3_packet(AVFormatContext *s, AVPacket *pkt)
{
    TYDemuxContext *ty = s->priv_data;

    for (;;) {
        int ret;

        if (ty->series3_payload_size < 188) {
            ret = get_series3_payload(s);
            if (ret < 0 && !ty->series3_flushed) {
                ty->series3_flushed = 1;
                return avpriv_mpegts_parse_flush(ty->mpegts, pkt);
            }
            if (ret < 0)
                return ret;
        }

        ret = avpriv_mpegts_parse_packet(ty->mpegts, pkt,
                                         ty->series3_payload +
                                             ty->series3_payload_pos,
                                         ty->series3_payload_size);
        if (ret >= 0) {
            ty->series3_payload_pos  += ret;
            ty->series3_payload_size -= ret;
            return 0;
        }

        /* No complete PES packet was returned, but the parser consumed the
         * complete transport-stream part. */
        ty->series3_payload_pos  += ty->series3_payload_size;
        ty->series3_payload_size = 0;
    }
}

static int demux_video(AVFormatContext *s, TyRecHdr *rec_hdr, AVPacket *pkt)
{
    TYDemuxContext *ty = s->priv_data;
    const int subrec_type = rec_hdr->subrec_type;
    const int64_t rec_size = rec_hdr->rec_size;
    int es_offset1, ret;
    int got_packet = 0;
    int resource_trailer = 0;

    /* Series 1 resource files end with a type 0xb metadata record.  Its
     * payload contains resource names and table data, but can also contain
     * byte patterns resembling MPEG picture start codes. */
    for (int i = 0; i + 4 <= rec_size; i++)
        if (!memcmp(ty->chunk + ty->cur_chunk_pos + i, ".toc", 4)) {
            resource_trailer = 1;
            break;
        }
    if (subrec_type == 0x0b && resource_trailer) {
        ty->cur_chunk_pos += rec_size;
        return 0;
    }

    if (subrec_type != 0x02 && rec_size > 7) {

        /* get the PTS from this packet if it has one.
         * on S1, only 0x06 has PES.  On S2, however, most all do.
         * Do NOT Pass the PES Header to the MPEG2 codec */
        es_offset1 = find_es_header(ty_VideoPacket, ty->chunk + ty->cur_chunk_pos, 5);
        if (es_offset1 != -1) {
            const uint8_t *data = ty->chunk + ty->cur_chunk_pos;
            const int payload_offset = find_next_start_code(data,
                                                             es_offset1 + 4,
                                                             rec_size);

            /* Some Series 3 MFS resource clips use a 12-byte pseudo-PES
             * header without a PTS, while generated and broadcast TY streams
             * normally use the 16-byte form. */
            if (rec_size >= es_offset1 + VIDEO_PTS_OFFSET + 5 &&
                (data[es_offset1 + 7] & 0x80))
                ty->last_video_pts = ff_parse_pes_pts(
                        data + es_offset1 + VIDEO_PTS_OFFSET);
            if (subrec_type != 0x06) {
                /* if we found a PES, and it's not type 6, then we're S2 */
                /* The packet will have video data (& other headers) so we
                 * chop out the PES header and send the rest */
                if (payload_offset >= 0) {
                    int size = rec_hdr->rec_size - payload_offset;

                    ty->cur_chunk_pos += payload_offset;
                    if ((ret = av_new_packet(pkt, size)) < 0)
                        return ret;
                    memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, size);
                    ty->cur_chunk_pos += size;
                    pkt->stream_index = 0;
                    got_packet = 1;
                } else {
                    ff_dlog(s, "video rec type 0x%02x has short PES"
                        " (%"PRId64" bytes)\n", subrec_type, rec_size);
                    /* nuke this block; it's too short, but has PES marker */
                    ty->cur_chunk_pos += rec_size;
                    return 0;
                }
            }
        }
    }

    if (subrec_type == 0x06) {
        /* type 6 (S1 DTivo) has no data, so we're done */
        ty->cur_chunk_pos += rec_size;
        return 0;
    }

    if (!got_packet) {
        if ((ret = av_new_packet(pkt, rec_size)) < 0)
            return ret;
        memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, rec_size);
        ty->cur_chunk_pos += rec_size;
        pkt->stream_index = 0;
        got_packet = 1;
    }

    /* if it's not a continue blk, then set PTS */
    if (subrec_type != 0x02) {
        if (subrec_type == 0x0c && pkt->size >= 6)
            pkt->data[5] |= 0x08;
        if (subrec_type == 0x07) {
            ty->last_ty_pts = rec_hdr->ty_pts;
        } else {
            /* yes I know this is a cheap hack.  It's the timestamp
               used for display and skipping fwd/back, so it
               doesn't have to be accurate to the millisecond.
               I adjust it here by roughly one 1/30 sec.  Yes it
               will be slightly off for UK streams, but it's OK.
             */
            ty->last_ty_pts += 35000000;
            //ty->last_ty_pts += 33366667;
        }
        /* set PTS for this block before we send */
        if (ty->last_video_pts > AV_NOPTS_VALUE) {
            pkt->pts = ty->last_video_pts;
            /* PTS gets used ONCE.
             * Any subsequent frames we get BEFORE next PES
             * header will have their PTS computed in the codec */
            ty->last_video_pts = AV_NOPTS_VALUE;
        }
    }

    return got_packet;
}

static int check_sync_pes(AVFormatContext *s, AVPacket *pkt,
                          int32_t offset, int32_t rec_len)
{
    TYDemuxContext *ty = s->priv_data;
    int pes_length = ty->pes_length;

    /* MPEG-audio PES header sizes vary between TiVo generations and even
     * between records.  The header-data-length byte is authoritative; leaving
     * any of the header in the elementary stream shifts the MP2 sync word. */
    if (ty->audio_type == TIVO_AUDIO_MPEG &&
        offset >= 0 && offset + 9 <= rec_len)
        pes_length = 9 + pkt->data[offset + 8];

    if (offset < 0 || offset + pes_length > rec_len) {
        /* entire PES header not present */
        ff_dlog(s, "PES header at %"PRId32" not complete in record. storing.\n", offset);
        /* save the partial pes header */
        if (offset < 0) {
            /* no header found, fake some 00's (this works, believe me) */
            memset(ty->pes_buffer, 0, 4);
            ty->pes_buf_cnt = 4;
            if (rec_len > 4)
                ff_dlog(s, "PES header not found in record of %"PRId32" bytes!\n", rec_len);
            return -1;
        }
        /* copy the partial pes header we found */
        memcpy(ty->pes_buffer, pkt->data + offset, rec_len - offset);
        ty->pes_buf_cnt = rec_len - offset;

        if (offset > 0) {
            /* PES Header was found, but not complete, so trim the end of this record */
            pkt->size -= rec_len - offset;
            return 1;
        }
        return -1;    /* partial PES, no audio data */
    }
    /* full PES header present, extract PTS */
    ty->last_audio_pts = ff_parse_pes_pts(&pkt->data[ offset + ty->pts_offset]);
    if (ty->first_audio_pts == AV_NOPTS_VALUE)
        ty->first_audio_pts = ty->last_audio_pts;
    pkt->pts = ty->last_audio_pts;
    memmove(pkt->data + offset, pkt->data + offset + pes_length, rec_len - pes_length);
    pkt->size -= pes_length;
    return 0;
}

static void strip_embedded_mpeg_audio_pes(AVPacket *pkt)
{
    int offset = 0;

    while (offset + 9 <= pkt->size) {
        int pes_length;

        if (memcmp(pkt->data + offset, ty_MPEGAudioPacket, 4)) {
            offset++;
            continue;
        }

        pes_length = 9 + pkt->data[offset + 8];
        if ((pkt->data[offset + 6] & 0xc0) != 0x80 ||
            pes_length < 9 || pes_length > pkt->size - offset) {
            offset++;
            continue;
        }

        memmove(pkt->data + offset, pkt->data + offset + pes_length,
                pkt->size - offset - pes_length);
        pkt->size -= pes_length;
    }
}

static int demux_audio(AVFormatContext *s, TyRecHdr *rec_hdr, AVPacket *pkt)
{
    TYDemuxContext *ty = s->priv_data;
    const int subrec_type = rec_hdr->subrec_type;
    const int64_t rec_size = rec_hdr->rec_size;
    int es_offset1, ret;

    if (subrec_type == 2) {
        int need = 0;
        /* SA or DTiVo Audio Data, no PES (continued block)
         * ================================================
         */

        /* continue PES if previous was incomplete */
        if (ty->pes_buf_cnt > 0) {
            need = ty->pes_length - ty->pes_buf_cnt;

            ff_dlog(s, "continuing PES header\n");
            /* do we have enough data to complete? */
            if (need >= rec_size) {
                /* don't have complete PES hdr; save what we have and return */
                memcpy(ty->pes_buffer + ty->pes_buf_cnt, ty->chunk + ty->cur_chunk_pos, rec_size);
                ty->cur_chunk_pos += rec_size;
                ty->pes_buf_cnt += rec_size;
                return 0;
            }

            /* we have enough; reconstruct this frame with the new hdr */
            memcpy(ty->pes_buffer + ty->pes_buf_cnt, ty->chunk + ty->cur_chunk_pos, need);
            ty->cur_chunk_pos += need;
            /* get the PTS out of this PES header (MPEG or AC3) */
            if (ty->audio_type == TIVO_AUDIO_MPEG) {
                es_offset1 = find_es_header(ty_MPEGAudioPacket,
                        ty->pes_buffer, 5);
            } else {
                es_offset1 = find_es_header(ty_AC3AudioPacket,
                        ty->pes_buffer, 5);
            }
            if (es_offset1 < 0) {
                ff_dlog(s, "Can't find audio PES header in packet.\n");
            } else {
                ty->last_audio_pts = ff_parse_pes_pts(
                    &ty->pes_buffer[es_offset1 + ty->pts_offset]);
                pkt->pts = ty->last_audio_pts;
            }
            ty->pes_buf_cnt = 0;

        }
        if ((ret = av_new_packet(pkt, rec_size - need)) < 0)
            return ret;
        memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, rec_size - need);
        ty->cur_chunk_pos += rec_size - need;
        pkt->stream_index = 1;

        /* S2 DTivo has AC3 packets with 2 padding bytes at end.  This is
         * not allowed in the AC3 spec and will cause problems.  So here
         * we try to trim things. */
        /* Also, S1 DTivo has alternating short / long AC3 packets.  That
         * is, one packet is short (incomplete) and the next packet has
         * the first one's missing data, plus all of its own.  Strange. */
        if (ty->audio_type == TIVO_AUDIO_AC3 &&
                ty->tivo_series == TIVO_SERIES2) {
            if (ty->ac3_pkt_size + pkt->size > AC3_PKT_LENGTH) {
                pkt->size -= FFMIN(pkt->size, 2);
                ty->ac3_pkt_size = 0;
            } else {
                ty->ac3_pkt_size += pkt->size;
            }
        }
    } else if (subrec_type == 0x03) {
        if ((ret = av_new_packet(pkt, rec_size)) < 0)
            return ret;
        memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, rec_size);
        ty->cur_chunk_pos += rec_size;
        pkt->stream_index = 1;
        /* MPEG Audio with PES Header, either SA or DTiVo   */
        /* ================================================ */
        es_offset1 = find_es_header(ty_MPEGAudioPacket, pkt->data, 5);

        /* SA PES Header, No Audio Data                     */
        /* ================================================ */
        if ((es_offset1 == 0) && (rec_size == 16)) {
            ty->last_audio_pts = ff_parse_pes_pts(&pkt->data[SA_PTS_OFFSET]);
            if (ty->first_audio_pts == AV_NOPTS_VALUE)
                ty->first_audio_pts = ty->last_audio_pts;
            av_packet_unref(pkt);
            return 0;
        }
        /* DTiVo Audio with PES Header                      */
        /* ================================================ */

        /* Check for complete PES */
        if (check_sync_pes(s, pkt, es_offset1, rec_size) == -1) {
            /* partial PES header found, nothing else.
             * we're done. */
            av_packet_unref(pkt);
            return 0;
        }
    } else if (subrec_type == 0x04) {
        /* SA Audio with no PES Header                      */
        /* ================================================ */
        if ((ret = av_new_packet(pkt, rec_size)) < 0)
            return ret;
        memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, rec_size);
        ty->cur_chunk_pos += rec_size;
        pkt->stream_index = 1;
        pkt->pts = ty->last_audio_pts;
    } else if (subrec_type == 0x09) {
        if ((ret = av_new_packet(pkt, rec_size)) < 0)
            return ret;
        memcpy(pkt->data, ty->chunk + ty->cur_chunk_pos, rec_size);
        ty->cur_chunk_pos += rec_size ;
        pkt->stream_index = 1;

        /* DTiVo AC3 Audio Data with PES Header             */
        /* ================================================ */
        es_offset1 = find_es_header(ty_AC3AudioPacket, pkt->data, 5);

        /* Check for complete PES */
        if (check_sync_pes(s, pkt, es_offset1, rec_size) == -1) {
            /* partial PES header found, nothing else.  we're done. */
            av_packet_unref(pkt);
            return 0;
        }
        /* S2 DTivo has invalid long AC3 packets */
        if (ty->tivo_series == TIVO_SERIES2) {
            if (pkt->size > AC3_PKT_LENGTH) {
                pkt->size -= 2;
                ty->ac3_pkt_size = 0;
            } else {
                ty->ac3_pkt_size = pkt->size;
            }
        }
    } else {
        /* Unsupported/Unknown */
        ty->cur_chunk_pos += rec_size;
        return 0;
    }

    if (ty->audio_type == TIVO_AUDIO_MPEG && pkt->size > 0)
        strip_embedded_mpeg_audio_pes(pkt);

    return 1;
}

static int ty_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    TYDemuxContext *ty = s->priv_data;
    AVIOContext *pb = s->pb;
    TyRecHdr *rec;
    int64_t rec_size = 0;
    int ret = 0;

    if (ty->series3_packet && ty->series3_packet->size) {
        av_packet_move_ref(pkt, ty->series3_packet);
        return 0;
    }
    if (ty->is_series3)
        return ty_read_series3_packet(s, pkt);

    if (ty->probe_chunk_index >= ty->probe_chunk_count && avio_feof(pb))
        return AVERROR_EOF;

    while (ret <= 0) {
        if (!ty->rec_hdrs || ty->first_chunk || ty->cur_rec >= ty->num_recs) {
            if (get_chunk(s) < 0 || ty->num_recs <= 0)
                return AVERROR_EOF;
        }

        rec = &ty->rec_hdrs[ty->cur_rec];
        rec_size = rec->rec_size;
        ty->cur_rec++;

        if (rec_size <= 0)
            continue;

        if (ty->cur_chunk_pos + rec->rec_size > CHUNK_SIZE)
            return AVERROR_INVALIDDATA;

        switch (rec->rec_type) {
        case VIDEO_ID:
            ret = demux_video(s, rec, pkt);
            break;
        case AUDIO_ID:
            ret = demux_audio(s, rec, pkt);
            break;
        default:
            ff_dlog(s, "Invalid record type 0x%02x\n", rec->rec_type);
            av_fallthrough;
        case 0x01:
        case 0x02:
        case 0x03: /* TiVo data services */
        case 0x05: /* unknown, but seen regularly */
            ty->cur_chunk_pos += rec->rec_size;
            break;
        }
    }

    return 0;
}

static int ty_read_close(AVFormatContext *s)
{
    TYDemuxContext *ty = s->priv_data;

    av_freep(&ty->rec_hdrs);
    av_freep(&ty->probe_chunks);
    if (ty->mpegts)
        avpriv_mpegts_parse_close(ty->mpegts);
    av_packet_free(&ty->series3_packet);

    return 0;
}

const FFInputFormat ff_ty_demuxer = {
    .p.name         = "ty",
    .p.long_name    = NULL_IF_CONFIG_SMALL("TiVo TY Stream"),
    .p.extensions   = "ty,ty+,tmf",
    .p.flags        = AVFMT_TS_DISCONT,
    .priv_data_size = sizeof(TYDemuxContext),
    .read_probe     = ty_probe,
    .read_header    = ty_read_header,
    .read_packet    = ty_read_packet,
    .read_close     = ty_read_close,
};
