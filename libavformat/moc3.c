/*
 * MOC2/MOC3 demuxer (fla2 / gvi3 / vid2)
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

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "demux.h"
#include "internal.h"

#include "avformat.h"
#include "mo.h"

typedef struct MOC3DemuxContext {
    const AVClass *class;       /* MUST be first: demuxer has a priv_class */
    int        is_vid2;         /* vid2 uses MOC5-style chunks, fla2 is raw bitstream */
    int64_t    data_start;      /* offset where video data begins */
    int        current_frame;
    int        frame_count;
    int64_t    audio_pts;
    int        has_audio;
    int        audio_stream_index;
    /* For vid2: chunk parsing state */
    int64_t    next_chunk_pos;
    int        audio_size;
    int        audio_padding;
    int        handle_audio_packet;
    AVStream  *ast;
    /* For fla2: frame offsets */
    int64_t   *frame_offsets;
    int        nb_frames;
    /* Experimental: emit the whole fla2 payload from payload offset 2 as one
     * pre-byteswapped packet.  Probing all 8 fla2 samples shows the video
     * bitstream starts at payload+2 in NATURAL byte order (unlike MOC5/MOFLEX,
     * which are stored 16-bit byteswapped); the decoder always bswap16s its
     * input, so we pre-swap here to cancel that out.  With this, every sample
     * parses as an I-frame with quantizer 48-51. */
    int        fla2_whole;
} MOC3DemuxContext;

/* Container layout, validated against a 70-file corpus — every MOC2/fla2,
 * MOC3/fla2 and MOC3/gvi3 sample satisfies it exactly:
 *
 *   0   magic      "MOC2" or "MOC3"
 *   4   hdr_size   LE32  (varies: 64 on MOC2, 240/284/288/292 on MOC3 — do not
 *                        assume a fixed value, the old code hardcoded 240/256)
 *   8   tag        "fla2" | "gvi3" | "vid2"
 *   12  LE32       small count (8/10/14)
 *   16  header region, hdr_size bytes; its last 8 bytes are
 *          payload_size LE32 @ hdr_size+8, then a licence-date-looking LE32
 *   16+hdr_size  payload
 *
 * giving 16 + hdr_size + payload_size == filesize. */
static int moc3_probe(const AVProbeData *p)
{
    uint32_t magic, tag, hdr_size;

    if (p->buf_size < 16)
        return 0;

    magic = AV_RL32(p->buf);
    if (magic != MKTAG('M', 'O', 'C', '3') && magic != MKTAG('M', 'O', 'C', '2'))
        return 0;

    tag = AV_RL32(p->buf + 8);
    if (tag != MKTAG('f', 'l', 'a', '2') && tag != MKTAG('v', 'i', 'd', '2') &&
        tag != MKTAG('g', 'v', 'i', '3'))
        return 0;

    hdr_size = AV_RL32(p->buf + 4);
    if (hdr_size < 16 || hdr_size > (1 << 20))
        return 0;

    return AVPROBE_SCORE_MAX;
}

/* Parse frame boundaries in the fla2 payload.
 *
 * WARNING: this heuristic is KNOWN WRONG and is kept only so the demuxer
 * produces something until the real framing is recovered.  It scans for
 * 0xd8 0x02 / 0xd9 0x02 believing those delimit frames; they do not.  Across
 * the corpus those byte pairs occur only 1-3 times per file (Christmas.mo has
 * exactly one in an 11.5 KB video), so this yields 1-3 "frames" for an entire
 * clip.  The 0xd8/0xd9 byte is simply what the first byte of a frame bitstream
 * encodes to (I-frame + flags + quantizer); the following 0x02 is incidental,
 * and the old player compares against no such constant anywhere.
 *
 * Real framing is still unknown — the leading hypothesis is
 * [variable-length size field][frame bitstream] per frame. */
static int moc3_parse_fla2_frames(AVFormatContext *s)
{
    MOC3DemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t payload_start = m->data_start;
    int64_t payload_size = avio_size(pb) - payload_start;
    int64_t pos;
    int capacity = 32;
    int ret;

    m->frame_offsets = av_malloc_array(capacity, sizeof(int64_t));
    if (!m->frame_offsets)
        return AVERROR(ENOMEM);

    /* Seek to payload start */
    ret = avio_seek(pb, payload_start, SEEK_SET);
    if (ret < 0)
        return ret;

    /* Read entire payload */
    uint8_t *payload = av_malloc(payload_size);
    if (!payload)
        return AVERROR(ENOMEM);

    ret = avio_read(pb, payload, payload_size);
    if (ret < payload_size) {
        av_free(payload);
        return ret < 0 ? ret : AVERROR_EOF;
    }

    /* Find frame boundaries */
    pos = 0;
    m->nb_frames = 0;

    while (pos < payload_size - 2) {
        /* Check for frame delimiter: 0xd8 0x02 or 0xd9 0x02 at pos+2 */
        if (pos + 3 < payload_size &&
            payload[pos + 2] == 0xd8 && payload[pos + 3] == 0x02) {
            /* Found frame start at pos */
            if (m->nb_frames >= capacity) {
                capacity *= 2;
                int64_t *new_offsets = av_realloc_array(m->frame_offsets, capacity, sizeof(int64_t));
                if (!new_offsets) {
                    av_free(payload);
                    return AVERROR(ENOMEM);
                }
                m->frame_offsets = new_offsets;
            }
            m->frame_offsets[m->nb_frames++] = payload_start + pos;
            /* Next frame starts at this delimiter */
            pos += 2;
        } else if (pos + 3 < payload_size &&
                   payload[pos + 2] == 0xd9 && payload[pos + 3] == 0x02) {
            if (m->nb_frames >= capacity) {
                capacity *= 2;
                int64_t *new_offsets = av_realloc_array(m->frame_offsets, capacity, sizeof(int64_t));
                if (!new_offsets) {
                    av_free(payload);
                    return AVERROR(ENOMEM);
                }
                m->frame_offsets = new_offsets;
            }
            m->frame_offsets[m->nb_frames++] = payload_start + pos;
            pos += 2;
        } else {
            pos++;
        }
    }

    /* If we found frames, add the end as a sentinel for the last frame's size */
    if (m->nb_frames > 0) {
        if (m->nb_frames >= capacity) {
            capacity++;
            int64_t *new_offsets = av_realloc_array(m->frame_offsets, capacity, sizeof(int64_t));
            if (!new_offsets) {
                av_free(payload);
                return AVERROR(ENOMEM);
            }
            m->frame_offsets = new_offsets;
        }
        m->frame_offsets[m->nb_frames++] = payload_start + payload_size;
    }

    av_free(payload);

    av_log(s, AV_LOG_DEBUG, "fla2: found %d frames\n", m->nb_frames - 1);
    return 0;
}

static int moc3_read_header(AVFormatContext *s)
{
    MOC3DemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t tag;
    uint32_t hdr_size;
    int64_t video_data_start;
    int ret;

    /* Video stream */
    AVStream *vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);

    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_MOBICLIP;

    /* Read magic (MOC2 and MOC3 share this container) */
    avio_skip(pb, 4); /* "MOC2" / "MOC3" */
    hdr_size = avio_rl32(pb);
    tag = avio_rl32(pb);
    if (hdr_size < 16 || hdr_size > (1 << 20))
        return AVERROR_INVALIDDATA;

    m->is_vid2 = (tag == MKTAG('v', 'i', 'd', '2'));
    m->current_frame = 0;
    m->frame_count = 0;
    m->has_audio = 0;
    m->audio_stream_index = -1;
    m->next_chunk_pos = 0;
    m->handle_audio_packet = 0;
    m->frame_offsets = NULL;
    m->nb_frames = 0;

    /* field1 (8/10/14), then the hdr_size-byte header region.  Most of that
     * region is a constant licence blob (byte-identical across files); only its
     * final 8 bytes are per-file: payload_size LE32 then a date-looking LE32.
     * hdr_size is NOT fixed (64 on MOC2, 240/284/288/292 on MOC3), so seek by
     * hdr_size rather than by hardcoded region sizes. */
    avio_skip(pb, 4);

    /* Resolution/fps have not been located in the header yet; the values below
     * are placeholders (they match the a/ sample set). */
    vst->codecpar->width  = 400;
    vst->codecpar->height = 240;
    avpriv_set_pts_info(vst, 1, 1, 25); /* default 25fps */

    /* Skip padding to align to header size */
    int64_t cur = avio_tell(pb);
    /* Fixed prologue is 16 bytes — magic(4) + hdr_size(4) + tag(4) + LE32(4) —
     * followed by hdr_size bytes of header region.  For fla2 hdr_size is 240,
     * so the payload starts at 0x100 (the payload-size LE32 sits at 0xf8, the
     * last 8 bytes of the header region). */
    video_data_start = 16 + hdr_size;
    if (cur < video_data_start)
        avio_skip(pb, video_data_start - cur);

    m->data_start = video_data_start;

    {   /* Validate the container identity: 16 + hdr_size + payload_size == filesize.
         * Holds for every MOC2/fla2, MOC3/fla2 and MOC3/gvi3 sample in the corpus. */
        int64_t fsize = avio_size(pb);
        int64_t save  = avio_tell(pb);
        if (fsize > 0 && avio_seek(pb, 8 + (int64_t)hdr_size, SEEK_SET) >= 0) {
            uint32_t payload_size = avio_rl32(pb);
            if ((int64_t)video_data_start + payload_size != fsize)
                av_log(s, AV_LOG_WARNING,
                       "moc3: size mismatch — 16+%u+%u = %"PRId64" but file is %"PRId64"\n",
                       hdr_size, payload_size,
                       (int64_t)video_data_start + payload_size, fsize);
            avio_seek(pb, save, SEEK_SET);
        }
    }

    if (m->is_vid2) {
        /* vid2 uses MOC5-style chunk headers: chunk_size, video_size, then video+audio */
        m->next_chunk_pos = video_data_start;
    } else {
        /* fla2: raw bitstream, parse frame boundaries */
        ret = moc3_parse_fla2_frames(s);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int moc3_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MOC3DemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    if (m->is_vid2) {
        /* vid2: MOC5-style chunks with chunk_size/video_size */
        if (m->handle_audio_packet) {
            /* TODO: handle audio packet */
            return AVERROR_PATCHWELCOME;
        }

        if (avio_feof(pb))
            return AVERROR_EOF;

        /* Read chunk header */
        int64_t chunk_pos = avio_tell(pb);
        uint32_t chunk_size = avio_rl32(pb);
        uint32_t video_size = avio_rl32(pb);

        if (chunk_size < 8 || video_size > chunk_size - 4)
            return AVERROR_EOF;

        /* Calculate next chunk position (4-byte aligned) */
        int64_t raw_end = chunk_pos + chunk_size;
        int pad = (int)(4 - (raw_end % 4));
        int64_t next_chunk_pos = raw_end + pad;
        m->next_chunk_pos = next_chunk_pos;

        int32_t audio_size = chunk_size - video_size - 8;
        if (audio_size < 0)
            audio_size = 0;

        m->audio_size = audio_size;
        m->audio_padding = pad;

        /* Read video data */
        ret = av_get_packet(pb, pkt, video_size);
        if (ret < 0)
            return ret;

        pkt->stream_index = 0;
        pkt->pts = m->current_frame;
        pkt->dts = m->current_frame;
        pkt->duration = 1;

        /* Keyframe detection: bit 7 of byte 1 (after bswap16 decode) */
        if (pkt->size >= 2 && (pkt->data[1] & 0x80))
            pkt->flags |= AV_PKT_FLAG_KEY;

        m->current_frame++;

        if (m->audio_size > 0) {
            m->handle_audio_packet = 1;
            /* TODO: create audio stream if needed */
        } else {
            avio_seek(pb, m->next_chunk_pos, SEEK_SET);
        }

        return ret;
    } else {
        /* fla2: raw bitstream with frame boundaries */
        if (m->fla2_whole) {
            int64_t end;
            int size, ret2;
            if (m->current_frame)
                return AVERROR_EOF;
            avio_seek(pb, 0, SEEK_END);
            end = avio_tell(pb);
            size = (int)(end - (m->data_start + 2));
            if (size <= 0)
                return AVERROR_EOF;
            avio_seek(pb, m->data_start + 2, SEEK_SET);
            ret2 = av_get_packet(pb, pkt, size);
            if (ret2 < 0)
                return ret2;
            /* pre-swap so the decoder's unconditional bswap16 restores natural order */
            for (int i = 0; i + 1 < pkt->size; i += 2)
                FFSWAP(uint8_t, pkt->data[i], pkt->data[i + 1]);
            pkt->stream_index = 0;
            pkt->pts = pkt->dts = 0;
            pkt->duration = 1;
            pkt->flags |= AV_PKT_FLAG_KEY;
            m->current_frame++;
            return ret2;
        }
        if (m->current_frame >= m->nb_frames - 1)
            return AVERROR_EOF;

        int64_t frame_start = m->frame_offsets[m->current_frame];
        int64_t frame_end   = m->frame_offsets[m->current_frame + 1];
        int frame_size = (int)(frame_end - frame_start);

        ret = avio_seek(pb, frame_start, SEEK_SET);
        if (ret < 0)
            return ret;

        ret = av_get_packet(pb, pkt, frame_size);
        if (ret < 0)
            return ret;

        pkt->stream_index = 0;
        pkt->pts = m->current_frame;
        pkt->dts = m->current_frame;
        pkt->duration = 1;

        /* Keyframe detection: after bswap16, first bit of frame is I-frame flag.
         * In raw bytes, this is bit 7 of byte 1. */
        if (pkt->size >= 2 && (pkt->data[1] & 0x80))
            pkt->flags |= AV_PKT_FLAG_KEY;

        m->current_frame++;
        return ret;
    }
}

static int moc3_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    MOC3DemuxContext *m = s->priv_data;

    if (m->is_vid2) {
        /* vid2 seek not yet implemented */
        return AVERROR(ENOSYS);
    } else {
        /* fla2: seek to frame */
        int frame = (int)timestamp;
        if (frame < 0 || frame >= m->nb_frames - 1)
            return AVERROR(EINVAL);

        int64_t frame_start = m->frame_offsets[frame];
        int64_t ret = avio_seek(s->pb, frame_start, SEEK_SET);
        if (ret < 0)
            return ret;

        m->current_frame = frame;
        return 0;
    }
}

static int moc3_read_close(AVFormatContext *s)
{
    MOC3DemuxContext *m = s->priv_data;
    av_free(m->frame_offsets);
    return 0;
}

#define OFFSET(x) offsetof(MOC3DemuxContext, x)
static const AVOption moc3_options[] = {
    { "fla2_whole", "emit whole fla2 payload from offset 2 as one pre-swapped packet (experimental)",
      OFFSET(fla2_whole), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass moc3_demuxer_class = {
    .class_name = "moc3",
    .item_name  = av_default_item_name,
    .option     = moc3_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_moc3_demuxer = {
    .p.name           = "moc3",
    .p.long_name      = "MobiClip MOC2/MOC3 (fla2/gvi3/vid2)",
    .read_probe       = moc3_probe,
    .read_header      = moc3_read_header,
    .read_packet      = moc3_read_packet,
    .read_seek        = moc3_read_seek,
    .read_close       = moc3_read_close,
    .priv_data_size   = sizeof(MOC3DemuxContext),
    .p.extensions     = "mo",
    .p.flags          = AVFMT_GENERIC_INDEX,
    .p.priv_class     = &moc3_demuxer_class,
};