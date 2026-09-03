/*
 * MOFLEX demuxer
 * Copyright (c) 2015-2016 Florian Nouwt
 * Copyright (c) 2017 Adib Surani
 * Copyright (c) 2020 Paul B Mahol
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
 */

#include "libavcodec/bytestream.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/stereo3d.h"
#include "demux.h"
#include "subtitles.h"

#include "avformat.h"
#include "internal.h"

typedef struct BitReader {
    unsigned last;
    unsigned pos;
} BitReader;

typedef struct MOFLEXTrailerSub {
    int stream_index;
    FFDemuxSubtitlesQueue q;
} MOFLEXTrailerSub;

typedef struct MOFLEXDemuxContext {
    unsigned size;
    int64_t pos;
    int64_t ts;
    int flags;
    int in_block;

    BitReader br;

    int64_t stream_pts[16]; /* cumulative pts per stream index */

    /* Super MOFLEX (CSXTRA01 trailer) */
    int has_trailer;
    int64_t poff;
    char inband_lang[5];

    /* Trailer audio (AUD1) */
    int tr_audio_stream;
    int64_t tr_audio_off;
    int64_t tr_audio_size;
    int tr_audio_rate;
    int tr_audio_channels;
    int tr_audio_pkt_samples;
    int tr_audio_pkt_size;
    int tr_audio_total_pkts;
    int tr_audio_next_pkt;

    /* Trailer subtitles (SUB0/SUB1) */
    MOFLEXTrailerSub tr_subs[16];
    int nb_tr_subs;
} MOFLEXDemuxContext;

static int pop(BitReader *br, AVIOContext *pb)
{
    if (avio_feof(pb))
        return AVERROR_EOF;

    if ((br->pos & 7) == 0)
        br->last = (unsigned)avio_r8(pb) << 24U;
    else
        br->last <<= 1;

    br->pos++;
    return !!(br->last & 0x80000000);
}

static int pop_int(BitReader *br, AVIOContext *pb, int n)
{
    int value = 0;

    for (int i = 0; i < n; i++) {
        int ret = pop(br, pb);

        if (ret < 0)
            return ret;
        if (ret > INT_MAX - value - value)
            return AVERROR_INVALIDDATA;
        value = 2 * value + ret;
    }

    return value;
}

static int pop_length(BitReader *br, AVIOContext *pb)
{
    int ret, n = 1;

    while ((ret = pop(br, pb)) == 0)
        n++;

    if (ret < 0)
        return ret;
    return n;
}

static int read_var_byte(AVFormatContext *s, unsigned *out)
{
    AVIOContext *pb = s->pb;
    unsigned value = 0, data;

    data = avio_r8(pb);
    if (!(data & 0x80)) {
        *out = data;
        return 0;
    }

    value = (data & 0x7F) << 7;
    data = avio_r8(pb);
    if (!(data & 0x80)) {
        value |= data;
        *out = value;
        return 0;
    }

    value = ((data & 0x7F) | value) << 7;
    data = avio_r8(pb);
    if (!(data & 0x80)) {
        value |= data;
        *out = value;
        return 0;
    }

    value = (((data & 0x7F) | value) << 7) | avio_r8(pb);
    *out = value;

    return 0;
}

static int moflex_probe(const AVProbeData *p)
{
    GetByteContext gb;
    int score = 0;

    bytestream2_init(&gb, p->buf, p->buf_size);

    if (bytestream2_get_be16(&gb) != 0x4C32)
        return 0;
    score += 10;

    bytestream2_skip(&gb, 10);
    if (bytestream2_get_be16(&gb) == 0)
        return 0;
    score += 5;

    while (bytestream2_get_bytes_left(&gb) > 0) {
        int type = bytestream2_get_byte(&gb);
        int size = bytestream2_get_byte(&gb);

        if (type == 0) {
            score += 5 * (size == 0);
            break;
        }
        if ((type == 1 && size == 12) ||
            (type == 2 && size ==  6) ||
            (type == 3 && size == 13) ||
            (type == 4 && size ==  2))
            score += 20;
        bytestream2_skip(&gb, size);
    }

    return FFMIN(AVPROBE_SCORE_MAX, score);
}

static int moflex_read_sync(AVFormatContext *s)
{
    MOFLEXDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;

    if (avio_rb16(pb) != 0x4C32) {
        if (avio_feof(pb))
            return AVERROR_EOF;
        avio_seek(pb, -2, SEEK_CUR);
        return 1;
    }

    avio_skip(pb, 2);
    m->ts = avio_rb64(pb);
    m->size = avio_rb16(pb) + 1;

    while (!avio_feof(pb)) {
        unsigned type, ssize, codec_id = 0;
        unsigned codec_type, width = 0, height = 0, sample_rate = 0, channels = 0;
        int image_layout = -1;
        int stream_index = -1;
        AVRational tb = av_make_q(0, 1);

        read_var_byte(s, &type);
        read_var_byte(s, &ssize);

        switch (type) {
        case 0:
            if (ssize > 0)
                avio_skip(pb, ssize);
            return 0;
        case 2:
            codec_type = AVMEDIA_TYPE_AUDIO;
            stream_index = avio_r8(pb);
            codec_id = avio_r8(pb);
            switch (codec_id) {
            case 0: codec_id = AV_CODEC_ID_FASTAUDIO; break;
            case 1: codec_id = AV_CODEC_ID_ADPCM_IMA_MOFLEX; break;
            case 2: codec_id = AV_CODEC_ID_PCM_S16LE; break;
            default:
                /* An audio codec we don't know must not cost the caller the
                 * whole file: the rest of the descriptor is fixed-size, so
                 * parsing stays in sync and every other stream still works.
                 * Expose the stream as data so its packets remain available
                 * (and demuxing keeps its timestamps) while the video decodes
                 * normally.  Returning AVERROR_PATCHWELCOME here meant a
                 * single unrecognised audio track made the file undecodable. */
                av_log(s, AV_LOG_WARNING,
                       "Unsupported audio codec %d in stream %d; "
                       "keeping it as a data stream.\n", codec_id, stream_index);
                codec_type = AVMEDIA_TYPE_DATA;
                codec_id = AV_CODEC_ID_NONE;
                break;
            }
            sample_rate = avio_rb24(pb) + 1;
            tb = av_make_q(1, sample_rate);
            channels = avio_r8(pb) + 1;
            break;
        case 1:
        case 3:
            codec_type = AVMEDIA_TYPE_VIDEO;
            stream_index = avio_r8(pb);
            codec_id = avio_r8(pb);
            switch (codec_id) {
            case 0: codec_id = AV_CODEC_ID_MOBICLIP; break;
            case 1: codec_id = AV_CODEC_ID_H264; break;
            default:
                av_log(s, AV_LOG_ERROR, "Unsupported video codec: %d\n", codec_id);
                return AVERROR_PATCHWELCOME;
            }
            tb.den = avio_rb16(pb);
            tb.num = avio_rb16(pb);
            width = avio_rb16(pb);
            height = avio_rb16(pb);
            avio_skip(pb, 2); /* PelRatioRate, PelRatioScale */
            if (type == 3)
                image_layout = avio_r8(pb) & 0x0f;
            break;
        case 4:
            codec_type = AVMEDIA_TYPE_DATA;
            stream_index = avio_r8(pb);
            avio_skip(pb, 1);
            break;
        }

        if (stream_index == s->nb_streams) {
            AVStream *st = avformat_new_stream(s, NULL);

            if (!st)
                return AVERROR(ENOMEM);

            st->codecpar->codec_type = codec_type;
            st->codecpar->codec_id   = codec_id;
            st->codecpar->width      = width;
            st->codecpar->height     = height;
            st->codecpar->sample_rate= sample_rate;
            st->codecpar->ch_layout.nb_channels = channels;
            st->priv_data            = av_packet_alloc();
            if (!st->priv_data)
                return AVERROR(ENOMEM);

            if (tb.num)
                avpriv_set_pts_info(st, 63, tb.num, tb.den);

            if (codec_type == AVMEDIA_TYPE_VIDEO && image_layout >= 0) {
                AVStereo3D *stereo;
                size_t stereo_size;

                stereo = av_stereo3d_alloc_size(&stereo_size);
                if (!stereo)
                    return AVERROR(ENOMEM);

                switch (image_layout) {
                case 0:
                case 1:
                    stereo->type = AV_STEREO3D_FRAMESEQUENCE;
                    break;
                case 2:
                case 3:
                    stereo->type = AV_STEREO3D_TOPBOTTOM;
                    break;
                case 4:
                case 5:
                    stereo->type = AV_STEREO3D_SIDEBYSIDE;
                    break;
                case 6:
                    stereo->type = AV_STEREO3D_2D;
                    break;
                default:
                    stereo->type = AV_STEREO3D_UNSPEC;
                    break;
                }
                if (image_layout & 1)
                    stereo->flags |= AV_STEREO3D_FLAG_INVERT;
                stereo->view = AV_STEREO3D_VIEW_PACKED;

                if (!av_packet_side_data_add(&st->codecpar->coded_side_data,
                                             &st->codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_STEREO3D,
                                             stereo, stereo_size, 0)) {
                    av_free(stereo);
                    return AVERROR(ENOMEM);
                }
            }
        }
    }

    return 0;
}

static void moflex_parse_srt(FFDemuxSubtitlesQueue *q, const uint8_t *srt_data, int srt_len, int stream_index)
{
    FFTextReader tr;
    char line[4096];
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    ff_text_init_buf(&tr, srt_data, srt_len);

    while (!ff_text_eof(&tr)) {
        if (ff_subtitles_read_line(&tr, line, sizeof(line)) < 0)
            break;
        int hh1, mm1, ss1, ms1, hh2, mm2, ss2, ms2;
        if (sscanf(line, "%d:%d:%d%*1[,.]%d --> %d:%d:%d%*1[,.]%d",
                   &hh1, &mm1, &ss1, &ms1, &hh2, &mm2, &ss2, &ms2) >= 8) {
            int64_t start = (hh1 * 3600LL + mm1 * 60LL + ss1) * 1000LL + ms1;
            int64_t end   = (hh2 * 3600LL + mm2 * 60LL + ss2) * 1000LL + ms2;
            av_bprint_clear(&buf);
            while (!ff_text_eof(&tr)) {
                if (ff_subtitles_read_line(&tr, line, sizeof(line)) < 0)
                    break;
                if (!line[0] || line[0] == '\r')
                    break;
                av_bprintf(&buf, "%s\n", line);
            }
            if (buf.len > 0) {
                while (buf.len > 0 && buf.str[buf.len - 1] == '\n')
                    buf.str[--buf.len] = 0;
                AVPacket *sub = ff_subtitles_queue_insert_bprint(q, &buf, 0);
                if (sub) {
                    sub->pts = start;
                    sub->dts = start;
                    sub->duration = end > start ? (end - start) : 0;
                    sub->stream_index = stream_index;
                }
            }
        }
    }
    av_bprint_finalize(&buf, NULL);
    ff_subtitles_queue_finalize(NULL, q);
}

static int moflex_parse_trailer(AVFormatContext *s)
{
    MOFLEXDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t fsz;
    uint8_t tail[16];

    m->tr_audio_stream = -1;
    m->has_trailer = 0;
    m->poff = 0;
    m->nb_tr_subs = 0;
    memset(m->inband_lang, 0, sizeof(m->inband_lang));

    if (!(pb->seekable & AVIO_SEEKABLE_NORMAL))
        return 0;

    fsz = avio_size(pb);
    if (fsz < 32)
        return 0;

    if (avio_seek(pb, fsz - 16, SEEK_SET) < 0)
        return 0;

    if (avio_read(pb, tail, 16) != 16) {
        avio_seek(pb, 0, SEEK_SET);
        return 0;
    }

    if (memcmp(tail + 8, "CSXTRA01", 8) != 0) {
        avio_seek(pb, 0, SEEK_SET);
        return 0;
    }

    uint64_t poff = AV_RL64(tail);
    if (poff == 0 || poff >= fsz - 16) {
        avio_seek(pb, 0, SEEK_SET);
        return 0;
    }

    m->has_trailer = 1;
    m->poff = (int64_t)poff;

    if (avio_seek(pb, m->poff, SEEK_SET) < 0) {
        avio_seek(pb, 0, SEEK_SET);
        return 0;
    }

    while (avio_tell(pb) + 8 <= fsz - 16) {
        uint8_t cc[4];
        uint32_t len;
        if (avio_read(pb, cc, 4) != 4)
            break;
        len = avio_rl32(pb);
        if (len == 0 || avio_tell(pb) + len > fsz - 16)
            break;

        int64_t sec_start = avio_tell(pb);

        if (!memcmp(cc, "LNG0", 4) && len >= 4) {
            char lang[5] = {0};
            avio_read(pb, lang, 4);
            lang[4] = 0;
            for (int k = 0; k < 4; k++)
                if (lang[k] >= 'A' && lang[k] <= 'Z')
                    lang[k] += 'a' - 'A';
            av_strlcpy(m->inband_lang, lang, sizeof(m->inband_lang));
        } else if (!memcmp(cc, "NFO0", 4) && len > 0) {
            uint8_t *nfo = av_malloc(len + 1);
            if (nfo) {
                if (avio_read(pb, nfo, len) == len) {
                    nfo[len] = 0;
                    char *saveptr = NULL;
                    char *line = av_strtok((char *)nfo, "\r\n", &saveptr);
                    while (line) {
                        char *eq = strchr(line, '=');
                        if (eq) {
                            *eq = 0;
                            char *key = line;
                            char *val = eq + 1;
                            while (*val == ' ') val++;
                            if (*val) {
                                if (!strcmp(key, "title"))
                                    av_dict_set(&s->metadata, "title", val, 0);
                                else if (!strcmp(key, "year") || !strcmp(key, "date"))
                                    av_dict_set(&s->metadata, "date", val, 0);
                                else if (!strcmp(key, "genres"))
                                    av_dict_set(&s->metadata, "genre", val, 0);
                                else if (!strcmp(key, "desc"))
                                    av_dict_set(&s->metadata, "comment", val, 0);
                                else if (!strcmp(key, "showdesc"))
                                    av_dict_set(&s->metadata, "description", val, 0);
                                else if (!strcmp(key, "category"))
                                    av_dict_set(&s->metadata, "category", val, 0);
                            }
                        }
                        line = av_strtok(NULL, "\r\n", &saveptr);
                    }
                }
                av_free(nfo);
            }
        } else if (!memcmp(cc, "ART5", 4) && len >= 4) {
            int w = avio_rl16(pb);
            int h = avio_rl16(pb);
            int data_sz = len - 4;
            if (w > 0 && h > 0 && data_sz == w * h * 2) {
                AVStream *st = avformat_new_stream(s, NULL);
                if (st) {
                    st->disposition |= AV_DISPOSITION_ATTACHED_PIC;
                    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
                    st->codecpar->format = AV_PIX_FMT_RGB565LE;
                    st->codecpar->width = w;
                    st->codecpar->height = h;
                    if (av_new_packet(&st->attached_pic, data_sz) >= 0) {
                        if (avio_read(pb, st->attached_pic.data, data_sz) == data_sz) {
                            st->attached_pic.stream_index = st->index;
                            st->attached_pic.flags |= AV_PKT_FLAG_KEY;
                        } else {
                            av_packet_unref(&st->attached_pic);
                        }
                    }
                }
            }
        } else if (!memcmp(cc, "SUB1", 4) && len >= 4) {
            char lang[5] = {0};
            avio_read(pb, lang, 4);
            lang[4] = 0;
            for (int k = 0; k < 4; k++)
                if (lang[k] >= 'A' && lang[k] <= 'Z')
                    lang[k] += 'a' - 'A';
            int srt_len = len - 4;
            if (srt_len > 0 && m->nb_tr_subs < 16) {
                uint8_t *srt_data = av_malloc(srt_len + 1);
                if (srt_data) {
                    if (avio_read(pb, srt_data, srt_len) == srt_len) {
                        srt_data[srt_len] = 0;
                        AVStream *st = avformat_new_stream(s, NULL);
                        if (st) {
                            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
                            st->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
                            avpriv_set_pts_info(st, 64, 1, 1000);
                            av_dict_set(&st->metadata, "language", lang, 0);
                            m->tr_subs[m->nb_tr_subs].stream_index = st->index;
                            moflex_parse_srt(&m->tr_subs[m->nb_tr_subs].q, srt_data, srt_len, st->index);
                            m->nb_tr_subs++;
                        }
                    }
                    av_free(srt_data);
                }
            }
        } else if (!memcmp(cc, "SUB0", 4) && len > 0) {
            int srt_len = len;
            if (m->nb_tr_subs < 16) {
                uint8_t *srt_data = av_malloc(srt_len + 1);
                if (srt_data) {
                    if (avio_read(pb, srt_data, srt_len) == srt_len) {
                        srt_data[srt_len] = 0;
                        AVStream *st = avformat_new_stream(s, NULL);
                        if (st) {
                            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
                            st->codecpar->codec_id = AV_CODEC_ID_SUBRIP;
                            avpriv_set_pts_info(st, 64, 1, 1000);
                            m->tr_subs[m->nb_tr_subs].stream_index = st->index;
                            moflex_parse_srt(&m->tr_subs[m->nb_tr_subs].q, srt_data, srt_len, st->index);
                            m->nb_tr_subs++;
                        }
                    }
                    av_free(srt_data);
                }
            }
        } else if (!memcmp(cc, "AUD1", 4) && len >= 12) {
            uint32_t rate = avio_rl32(pb);
            uint16_t ch = avio_rl16(pb);
            uint16_t samp_pkt = avio_rl16(pb);
            char lang[5] = {0};
            avio_read(pb, lang, 4);
            lang[4] = 0;
            for (int k = 0; k < 4; k++)
                if (lang[k] >= 'A' && lang[k] <= 'Z')
                    lang[k] += 'a' - 'A';
            int aud_len = len - 12;
            if (rate > 0 && ch > 0 && aud_len > 0) {
                AVStream *st = avformat_new_stream(s, NULL);
                if (st) {
                    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    st->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_MOFLEX;
                    st->codecpar->sample_rate = rate;
                    st->codecpar->ch_layout.nb_channels = ch;
                    avpriv_set_pts_info(st, 64, 1, rate);
                    av_dict_set(&st->metadata, "language", lang, 0);
                    m->tr_audio_stream = st->index;
                    m->tr_audio_off = avio_tell(pb);
                    m->tr_audio_size = aud_len;
                    m->tr_audio_rate = rate;
                    m->tr_audio_channels = ch;
                    m->tr_audio_pkt_samples = samp_pkt ? samp_pkt : 1024;
                    m->tr_audio_pkt_size = ch * 4 + (m->tr_audio_pkt_samples / 2) * ch;
                    m->tr_audio_total_pkts = m->tr_audio_pkt_size > 0 ? (aud_len / m->tr_audio_pkt_size) : 0;
                    m->tr_audio_next_pkt = 0;
                }
            }
        }

        avio_seek(pb, sec_start + len, SEEK_SET);
    }

    avio_seek(pb, 0, SEEK_SET);
    return 0;
}

static int moflex_read_header(AVFormatContext *s)
{
    MOFLEXDemuxContext *m = s->priv_data;
    int ret;

    ret = moflex_read_sync(s);
    if (ret < 0)
        return ret;

    moflex_parse_trailer(s);

    if (m->inband_lang[0]) {
        for (int i = 0; i < s->nb_streams; i++) {
            if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                i != m->tr_audio_stream) {
                av_dict_set(&s->streams[i]->metadata, "language", m->inband_lang, 0);
                break;
            }
        }
    }

    s->ctx_flags |= AVFMTCTX_NOHEADER;
    avio_seek(s->pb, 0, SEEK_SET);

    return 0;
}

static int moflex_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MOFLEXDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    BitReader *br = &m->br;
    int ret;

    while (!avio_feof(pb)) {
        if (!m->in_block) {
            if (m->has_trailer && m->poff > 0 && avio_tell(pb) >= m->poff) {
                /* When we reach the trailer:
                 * 1. Drain any remaining trailer audio packets.
                 * 2. Drain any remaining subtitle packets.
                 * 3. Return EOF. */
                if (m->tr_audio_stream >= 0 && m->tr_audio_next_pkt < m->tr_audio_total_pkts &&
                    s->streams[m->tr_audio_stream]->discard < AVDISCARD_ALL) {
                    int64_t target_off = m->tr_audio_off + (int64_t)m->tr_audio_next_pkt * m->tr_audio_pkt_size;
                    avio_seek(pb, target_off, SEEK_SET);
                    ret = av_get_packet(pb, pkt, m->tr_audio_pkt_size);
                    if (ret >= 0) {
                        pkt->stream_index = m->tr_audio_stream;
                        pkt->pts = (int64_t)m->tr_audio_next_pkt * m->tr_audio_pkt_samples;
                        pkt->dts = pkt->pts;
                        pkt->duration = m->tr_audio_pkt_samples;
                        pkt->flags |= AV_PKT_FLAG_KEY;
                        m->tr_audio_next_pkt++;
                        return 0;
                    }
                }
                for (int i = 0; i < m->nb_tr_subs; i++) {
                    if (m->tr_subs[i].q.current_sub_idx < m->tr_subs[i].q.nb_subs) {
                        ret = ff_subtitles_queue_read_packet(&m->tr_subs[i].q, pkt);
                        if (ret >= 0) {
                            pkt->stream_index = m->tr_subs[i].stream_index;
                            return 0;
                        }
                    }
                }
                return AVERROR_EOF;
            }

            /* Deliver pending subtitle packets due at current sync timestamp */
            for (int i = 0; i < m->nb_tr_subs; i++) {
                FFDemuxSubtitlesQueue *q = &m->tr_subs[i].q;
                if (q->current_sub_idx < q->nb_subs &&
                    q->subs[q->current_sub_idx]->pts <= (m->ts / 1000)) {
                    ret = ff_subtitles_queue_read_packet(q, pkt);
                    if (ret >= 0) {
                        pkt->stream_index = m->tr_subs[i].stream_index;
                        return 0;
                    }
                }
            }

            /* Deliver trailer audio packets due at current sync timestamp */
            if (m->tr_audio_stream >= 0 && m->tr_audio_total_pkts > 0 &&
                s->streams[m->tr_audio_stream]->discard < AVDISCARD_ALL) {
                int64_t target_samples = (m->ts * m->tr_audio_rate) / 1000000LL;
                int target_pkt = target_samples / m->tr_audio_pkt_samples;
                if (m->tr_audio_next_pkt < target_pkt && m->tr_audio_next_pkt < m->tr_audio_total_pkts) {
                    int64_t cur_pos = avio_tell(pb);
                    int64_t target_off = m->tr_audio_off + (int64_t)m->tr_audio_next_pkt * m->tr_audio_pkt_size;
                    avio_seek(pb, target_off, SEEK_SET);
                    ret = av_get_packet(pb, pkt, m->tr_audio_pkt_size);
                    avio_seek(pb, cur_pos, SEEK_SET);
                    if (ret >= 0) {
                        pkt->stream_index = m->tr_audio_stream;
                        pkt->pts = (int64_t)m->tr_audio_next_pkt * m->tr_audio_pkt_samples;
                        pkt->dts = pkt->pts;
                        pkt->duration = m->tr_audio_pkt_samples;
                        pkt->flags |= AV_PKT_FLAG_KEY;
                        m->tr_audio_next_pkt++;
                        return 0;
                    }
                }
            }

            m->pos = avio_tell(pb);

            ret = moflex_read_sync(s);
            if (ret < 0)
                return ret;

            m->flags = avio_r8(pb);
            if (m->flags & 2)
                avio_skip(pb, 2);

            if (getenv("MOFLEX_DEBUG"))
                av_log(s, AV_LOG_INFO,
                       "BLOCK pos=0x%"PRIx64" sync=%d flags=0x%02x size=%d\n",
                       (int64_t)m->pos, ret == 0, m->flags, m->size);
        }

        while ((avio_tell(pb) < m->pos + m->size) && !avio_feof(pb) && avio_r8(pb)) {
            int stream_index, bits, pkt_size, endframe;
            AVPacket *packet;

            m->in_block = 1;

            avio_seek(pb, -1, SEEK_CUR);
            br->pos = br->last = 0;

            bits = pop_length(br, pb);
            if (bits < 0)
                return bits;
            stream_index = pop_int(br, pb, bits);
            if (stream_index < 0)
                return stream_index;
            if (stream_index >= s->nb_streams)
                return AVERROR_INVALIDDATA;

            endframe = pop(br, pb);
            if (endframe < 0)
                return endframe;
            if (endframe) {
                bits = pop_length(br, pb);
                if (bits < 0)
                    return bits;
                pop_int(br, pb, bits);
                pop(br, pb);
                bits = pop_length(br, pb);
                if (bits < 0)
                    return bits;
                pop_int(br, pb, bits * 2 + 26);
            }

            pkt_size = pop_int(br, pb, 13) + 1;
            if (pkt_size > m->size)
                return AVERROR_INVALIDDATA;
            if (getenv("MOFLEX_DEBUG"))
                av_log(s, AV_LOG_INFO,
                       "  CHUNK si=%d endframe=%d pkt_size=%d payload@0x%"PRIx64"\n",
                       stream_index, endframe, pkt_size, (int64_t)avio_tell(pb));
            packet   = s->streams[stream_index]->priv_data;
            if (!packet) {
                avio_skip(pb, pkt_size);
                continue;
            }

            ret = av_append_packet(pb, packet, pkt_size);
            if (ret < 0)
                return ret;

            /* Helper lambda-equivalent: fill audio packet fields and advance PTS. */
#define FILL_AUDIO_PKT(p, si_idx) do {                                          \
            (p)->pos          = m->pos;                                          \
            (p)->stream_index = (si_idx);                                        \
            (p)->flags       |= AV_PKT_FLAG_KEY;                                 \
            if ((si_idx) < FF_ARRAY_ELEMS(m->stream_pts)) {                      \
                (p)->pts = m->stream_pts[(si_idx)];                               \
                (p)->dts = (p)->pts;                                              \
            }                                                                    \
            {                                                                    \
                AVCodecParameters *_par = s->streams[(si_idx)]->codecpar;        \
                int64_t _dur = 0;                                                 \
                int _ch = _par->ch_layout.nb_channels;                           \
                if (_ch <= 0) _ch = 1;                                           \
                if (_par->codec_id == AV_CODEC_ID_PCM_S16LE) {                  \
                    _dur = (p)->size / (_ch * 2);                                 \
                } else if (_par->codec_id == AV_CODEC_ID_ADPCM_IMA_MOFLEX) {    \
                    /* one shared header (ch*4), then ch*128 nibbles per subframe */ \
                    _dur = _ch > 0 ? ((p)->size - 4 * _ch) * 2 / _ch : 0;     \
                } else if (_par->codec_id == AV_CODEC_ID_FASTAUDIO) {           \
                    /* 40 bytes per channel decode to 256 samples, and a packet \
                     * can hold several of those blocks -- assuming exactly one \
                     * stamps every packet 256 long however much audio it       \
                     * carries, so the timestamps advance slower than the sound \
                     * and the stream reports a fraction of its real length. */ \
                    _dur = (p)->size / (40 * _ch) * 256;                         \
                }                                                                \
                (p)->duration = _dur;                                             \
                if ((si_idx) < FF_ARRAY_ELEMS(m->stream_pts))                    \
                    m->stream_pts[(si_idx)] += _dur;                              \
            }                                                                    \
        } while (0)

            if (endframe && packet->size > 0) {
                av_packet_move_ref(pkt, packet);
                pkt->pos = m->pos;
                pkt->stream_index = stream_index;
                if (stream_index < FF_ARRAY_ELEMS(m->stream_pts)) {
                    pkt->pts = m->stream_pts[stream_index];
                    pkt->dts = pkt->pts;
                }
                if (s->streams[stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    pkt->duration = 1;
                    /* Key-frame detection for MobiClip: the decoder applies
                     * bswap16 before parsing, so the first decoded bit is
                     * bit7 of pkt->data[1] (not data[0]).  A 1 there = I-frame.
                     * For H.264 Annex-B streams scan for IDR/SPS NAL type. */
                    if (s->streams[stream_index]->codecpar->codec_id == AV_CODEC_ID_MOBICLIP) {
                        if (pkt->size >= 2 && (pkt->data[1] & 0x80))
                            pkt->flags |= AV_PKT_FLAG_KEY;
                    } else {
                        /* Annex-B: find first non-zero byte past start codes */
                        int si = 0;
                        while (si < pkt->size - 1 && pkt->data[si] == 0) si++;
                        if (si < pkt->size && pkt->data[si] == 0x01) si++;
                        if (si < pkt->size) {
                            int nal_type = pkt->data[si] & 0x1F;
                            if (nal_type == 5 || nal_type == 7)
                                pkt->flags |= AV_PKT_FLAG_KEY;
                        }
                    }
                    if (stream_index < FF_ARRAY_ELEMS(m->stream_pts))
                        m->stream_pts[stream_index] += 1;
                } else {
                    FILL_AUDIO_PKT(pkt, stream_index);
                }
                return 0;
            }
        }

        m->in_block = 0;

        if (m->flags % 2 == 0) {
            if (m->size <= 0)
                return AVERROR_INVALIDDATA;
            avio_seek(pb, m->pos + m->size, SEEK_SET);
        }
    }

    return AVERROR_EOF;
}

static int moflex_read_seek(AVFormatContext *s, int stream_index,
                            int64_t pts, int flags)
{
    MOFLEXDemuxContext *m = s->priv_data;

    m->in_block = 0;
    if (m->has_trailer) {
        int64_t target_ts_us = av_rescale_q(pts, s->streams[stream_index]->time_base, (AVRational){1, 1000000});
        if (m->tr_audio_stream >= 0 && m->tr_audio_rate > 0 && m->tr_audio_pkt_samples > 0) {
            int64_t target_samples = (target_ts_us * m->tr_audio_rate) / 1000000LL;
            m->tr_audio_next_pkt = av_clip(target_samples / m->tr_audio_pkt_samples, 0, m->tr_audio_total_pkts);
        }
        for (int i = 0; i < m->nb_tr_subs; i++) {
            ff_subtitles_queue_seek(&m->tr_subs[i].q, s, stream_index, target_ts_us / 1000, target_ts_us / 1000, target_ts_us / 1000, flags);
        }
    }

    return -1;
}

static int moflex_read_close(AVFormatContext *s)
{
    MOFLEXDemuxContext *m = s->priv_data;

    for (int i = 0; i < s->nb_streams; i++) {
        av_packet_free((AVPacket **)&s->streams[i]->priv_data);
    }
    for (int i = 0; i < m->nb_tr_subs; i++) {
        ff_subtitles_queue_clean(&m->tr_subs[i].q);
    }

    return 0;
}

const FFInputFormat ff_moflex_demuxer = {
    .p.name           = "moflex",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MobiClip MOFLEX"),
    .priv_data_size = sizeof(MOFLEXDemuxContext),
    .read_probe     = moflex_probe,
    .read_header    = moflex_read_header,
    .read_packet    = moflex_read_packet,
    .read_seek      = moflex_read_seek,
    .read_close     = moflex_read_close,
    .p.extensions     = "moflex",
    .p.flags          = AVFMT_GENERIC_INDEX,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
};
