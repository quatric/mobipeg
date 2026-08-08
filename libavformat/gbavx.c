/*
 * ActImagine "VX++" GBA Video cartridge demuxer
 * Copyright (c) 2026 the FFmpeg developers
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

/**
 * @file
 * The 64 MB GBA Video cartridges (Shrek + Shark Tale and friends) hold movies
 * in an ActImagine container tagged 'VX++'. It is not the DS '.vx' container
 * and not either Majesco stack; see doc/gba_video_ads.md.
 *
 * A stream is a 0x38-byte header, then the video bitstream, then the audio,
 * then a chapter table. Offsets in the header are stream-relative:
 *
 *     +00 'VX++'          +1c audio offset
 *     +04 frame count     +20 chapter table offset
 *     +08 width           +24 first frame of the last chapter
 *     +0c height          +28 seek table offset
 *     +10 frame rate      +2c seek table entries
 *     +14 quantizer (0)   +30 (same as +24)
 *     +18 sample rate     +34 unknown
 *
 * The seek table is {frame, video bit offset, audio byte offset, 0}, and that
 * middle column is the whole story: the video is one continuous bitstream with
 * no per-frame sizes. The seek entries do provide independently decodable
 * segment boundaries, however, so each interval is exposed as one packet for
 * the GBA-specific decoder. This bitstream is not the one libavcodec/vx.c
 * decodes - it is an earlier generation of the codec.
 *
 * The audio is the DS codec unchanged. Its region opens with the same
 * 3124-byte codebook block, and every AFrame is a whole number of 16-bit words
 * (32 header bits plus 16 per pulse word), so AFrames can be walked without
 * decoding: the walk over the reference cartridge tiles all 9.7 MB of audio
 * exactly and lands on every one of the 181 audio offsets the seek table
 * states.
 */

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavcodec/defs.h"
#include "libavcodec/gba_vx.h"

#define VX_HEADER_SIZE 0x38

/* 3*64*8 int16 codebooks, 8 uint16 scale modifiers, 8 int32 lpc bases and one
 * uint32 initial scale - AudioExtraData in libavcodec/vx_audio.c. */
#define VX_AUDIO_EXTRADATA_SIZE (3*64*8*2 + 8*2 + 8*4 + 4)

/* 128 AFrames of 128 samples cover 7 video frames, measured against every
 * entry of the reference cartridge's seek table. At the 16384 Hz these carts
 * use that is exactly 7 fps. */
#define VX_FRAMES_PER_SECOND 7
#define VX_AFRAMES_PER_SECOND 128

#define AFRAME_SAMPLES 128
#define AFRAMES_PER_PACKET 128          /* one second of audio */

typedef struct GBAVXStream {
    int64_t off;
    uint32_t nb_frames, width, height;
    uint32_t frame_rate, quantizer, sample_rate;
    uint32_t audio_off, chapter_off, seek_off, nb_seek;
} GBAVXStream;

typedef struct GBAVXSegment {
    uint32_t frame;
    uint32_t video_bit;
    int64_t audio_pos;
    int64_t audio_pts;
} GBAVXSegment;

typedef struct GBAVXDemuxContext {
    const AVClass *class;
    const char *resource;               /* stream index to play */

    GBAVXStream st;
    GBAVXSegment *segments;
    int nb_segments;
    int video_segment;
    int video_idx, audio_idx;
    int64_t audio_pos, audio_end;
    int64_t audio_pts;
} GBAVXDemuxContext;

/* Bytes an AFrame occupies, given its first two 16-bit words. */
static int aframe_size(uint32_t word2)
{
    static const uint8_t pulse_data_len[4] = { 8, 5, 4, 3 };

    return 4 + 2 * pulse_data_len[(word2 >> 12) & 3];
}

/* Count complete AFrames in [start, end). Seek-table boundaries must tile
 * exactly; unlike the end of the whole audio region they carry no zero pad. */
static int count_aframes(AVIOContext *pb, int64_t start, int64_t end,
                         int64_t *count)
{
    int64_t pos = start, n = 0;

    if (start > end || avio_seek(pb, start, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    while (pos < end) {
        int size;

        if (end - pos < 4)
            return AVERROR_INVALIDDATA;
        avio_skip(pb, 2);
        size = aframe_size(avio_rl16(pb));
        if (pos + size > end)
            return AVERROR_INVALIDDATA;
        avio_skip(pb, size - 4);
        pos += size;
        n++;
    }
    *count = n;
    return 0;
}

static int header_ok(const GBAVXStream *st, int64_t filesize)
{
    return st->width  >= 16 && st->width  <= 240 && !(st->width  % 16) &&
           st->height >= 16 && st->height <= 240 && !(st->height % 16) &&
           st->nb_frames > 0 && st->nb_frames < (1 << 20) &&
           st->sample_rate >= 4000 && st->sample_rate <= 48000 &&
           st->nb_seek > 0 && st->nb_seek < (1 << 16) &&
           st->audio_off > VX_HEADER_SIZE &&
           st->audio_off + VX_AUDIO_EXTRADATA_SIZE <= st->chapter_off &&
           st->seek_off > st->audio_off &&
           st->off + st->chapter_off <= filesize;
}

static void parse_header(GBAVXStream *st, int64_t off, const uint8_t *h)
{
    st->off         = off;
    st->nb_frames   = AV_RL32(h + 0x04);
    st->width       = AV_RL32(h + 0x08);
    st->height      = AV_RL32(h + 0x0c);
    st->frame_rate  = AV_RL32(h + 0x10);
    st->quantizer   = AV_RL32(h + 0x14);
    st->sample_rate = AV_RL32(h + 0x18);
    st->audio_off   = AV_RL32(h + 0x1c);
    st->chapter_off = AV_RL32(h + 0x20);
    st->seek_off    = AV_RL32(h + 0x28);
    st->nb_seek     = AV_RL32(h + 0x2c);
}

static int gbavx_probe(const AVProbeData *p)
{
    /* Every GBA cartridge header carries a fixed 0x96 at 0xb2. The first
     * stream of the reference cart sits at 0x20200, inside the probe buffer. */
    if (p->buf_size < 0xc0 || p->buf[0xb2] != 0x96)
        return 0;

    for (int i = 0; i + VX_HEADER_SIZE <= p->buf_size; i += 4) {
        GBAVXStream st;

        if (AV_RL32(p->buf + i) != MKTAG('V', 'X', '+', '+'))
            continue;
        parse_header(&st, i, p->buf + i);
        /* The file is longer than the probe buffer, so bound by the header's
         * own claim rather than by a size we do not know here. */
        if (header_ok(&st, st.off + st.chapter_off))
            return AVPROBE_SCORE_MAX;
    }
    return 0;
}

/** Add the chapter table as AVChapters; failure here is not fatal. */
static void read_chapters(AVFormatContext *avctx, const GBAVXStream *st)
{
    AVIOContext *pb = avctx->pb;
    uint32_t count, *start;

    avio_seek(pb, st->off + st->chapter_off, SEEK_SET);
    count = avio_rl32(pb);
    if (!count || count > 256)
        return;

    start = av_malloc_array(count, sizeof(*start));
    if (!start)
        return;
    for (uint32_t i = 0; i < count; i++)
        start[i] = avio_rl32(pb);

    /* Chapters are frame numbers, which the seek table dates at 7 fps. */
    if (!avio_feof(pb))
        for (uint32_t i = 0; i < count; i++)
            avpriv_new_chapter(avctx, i, (AVRational){ 1, VX_FRAMES_PER_SECOND },
                               start[i],
                               i + 1 < count ? start[i + 1] : st->nb_frames,
                               NULL);
    av_free(start);
}

/* Build exact audio indexes from the container seek table. The table dates
 * video in frames but gives audio in bytes, so count AFrames between byte
 * offsets to avoid accumulating a fractional 128/7 cadence approximation. */
static int read_indexes(AVFormatContext *avctx, AVStream *ast,
                        GBAVXDemuxContext *s, const GBAVXStream *st)
{
    AVIOContext *pb = avctx->pb;
    GBAVXSegment *segments;
    int64_t previous = st->off + st->audio_off + VX_AUDIO_EXTRADATA_SIZE;
    int64_t pts = 0;
    uint32_t previous_frame = 0, previous_bit = 0;
    int nb_segments = 0, ret;

    segments = av_calloc(st->nb_seek, sizeof(*segments));
    if (!segments)
        return AVERROR(ENOMEM);

    if (avio_seek(pb, st->off + st->seek_off, SEEK_SET) < 0)
        goto invalid;
    for (uint32_t i = 0; i < st->nb_seek; i++) {
        uint32_t frame = avio_rl32(pb);
        uint32_t video_bit = avio_rl32(pb);
        uint32_t audio_off = avio_rl32(pb);
        int64_t pos, n;

        avio_skip(pb, 4);
        if (!audio_off) {
            if (frame != st->nb_frames || video_bit)
                goto invalid;
            continue;                       /* terminal sentinel */
        }
        if ((nb_segments && (frame <= previous_frame || video_bit <= previous_bit)) ||
            (!nb_segments && (frame || video_bit)))
            goto invalid;
        pos = st->off + st->audio_off + audio_off;
        if (pos < previous || pos + 2 > st->off + st->chapter_off)
            goto invalid;
        ret = count_aframes(pb, previous, pos, &n);
        if (ret < 0)
            goto fail;
        pts += n * AFRAME_SAMPLES;

        /* A random-access segment always opens with an intra AFrame. */
        if (avio_seek(pb, pos, SEEK_SET) < 0 || ((avio_rl16(pb) >> 9) & 0x7f) != 0x7f)
            goto invalid;
        ret = av_add_index_entry(ast, pos, pts, 0, 0, AVINDEX_KEYFRAME);
        if (ret < 0)
            goto fail;
        segments[nb_segments++] = (GBAVXSegment) {
            .frame     = frame,
            .video_bit = video_bit,
            .audio_pos = pos,
            .audio_pts = pts,
        };
        av_log(avctx, AV_LOG_TRACE,
               "audio seek %u: video frame %u bit %u, byte 0x%"PRIx64", pts %"PRId64"\n",
               i, frame, video_bit, pos, pts);
        previous = pos;
        previous_frame = frame;
        previous_bit = video_bit;

        /* count_aframes and the intra check seek away from the table. */
        if (avio_seek(pb, st->off + st->seek_off + 16 * (i + 1), SEEK_SET) < 0)
            goto invalid;
    }
    if (!nb_segments)
        goto invalid;
    s->segments = segments;
    s->nb_segments = nb_segments;
    return 0;

invalid:
    ret = AVERROR_INVALIDDATA;
fail:
    av_free(segments);
    return ret;
}

static int gbavx_read_header(AVFormatContext *avctx)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t filesize = avio_size(pb), pos = 0, best = -1;
    int64_t best_size = 0;
    int want = s->resource ? atoi(s->resource) : -1, idx = 0, ret;
    GBAVXStream st = { 0 }, best_st = { 0 };
    uint8_t h[VX_HEADER_SIZE];

    s->video_idx = s->audio_idx = -1;
    if (filesize <= 0)
        return AVERROR_INVALIDDATA;

    /* Streams are 0x200-aligned and laid out back to back, but scan on a word
     * boundary so an unusual cart cannot hide one. */
    while (pos + VX_HEADER_SIZE <= filesize) {
        avio_seek(pb, pos, SEEK_SET);
        if (avio_read(pb, h, VX_HEADER_SIZE) != VX_HEADER_SIZE)
            break;
        if (AV_RL32(h) != MKTAG('V', 'X', '+', '+')) {
            pos += 4;
            continue;
        }
        parse_header(&st, pos, h);
        if (!header_ok(&st, filesize)) {
            pos += 4;
            continue;
        }

        av_log(avctx, AV_LOG_VERBOSE,
               "  [%d] VX++ at 0x%"PRIx64", %ux%u, %u frames, %u Hz\n",
               idx, pos, st.width, st.height, st.nb_frames, st.sample_rate);

        if (want >= 0 ? idx == want : (int64_t)st.chapter_off > best_size) {
            best      = pos;
            best_size = st.chapter_off;
            best_st   = st;
        }
        idx++;
        pos = st.off + st.chapter_off;
    }

    if (best < 0) {
        av_log(avctx, AV_LOG_ERROR, "no VX++ stream found in this ROM\n");
        return AVERROR_INVALIDDATA;
    }
    s->st = best_st;
    s->audio_pos = best_st.off + best_st.audio_off + VX_AUDIO_EXTRADATA_SIZE;
    s->audio_end = best_st.off + best_st.chapter_off;

    {
        AVStream *vst = avformat_new_stream(avctx, NULL);

        if (!vst)
            return AVERROR(ENOMEM);
        vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        vst->codecpar->codec_id   = AV_CODEC_ID_GBA_VX;
        vst->codecpar->width      = best_st.width;
        vst->codecpar->height     = best_st.height;
        s->video_idx = vst->index;
        avpriv_set_pts_info(vst, 64, 1, VX_FRAMES_PER_SECOND);
        vst->avg_frame_rate = (AVRational) { VX_FRAMES_PER_SECOND, 1 };
        vst->duration = best_st.nb_frames;
    }

    {
        AVStream *ast = avformat_new_stream(avctx, NULL);

        if (!ast)
            return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = AV_CODEC_ID_VX_AUDIO;
        ast->codecpar->sample_rate = best_st.sample_rate;
        ast->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        s->audio_idx = ast->index;
        avpriv_set_pts_info(ast, 64, 1, best_st.sample_rate);
        ast->duration = av_rescale(best_st.nb_frames, best_st.sample_rate,
                                   VX_FRAMES_PER_SECOND);

        /* [u32 quantizer][u32 width][u32 height][codebooks]; width and height
         * are zero because these packets carry no leading video bits, which is
         * the same shape .mods uses. */
        ret = ff_alloc_extradata(ast->codecpar, 12 + VX_AUDIO_EXTRADATA_SIZE);
        if (ret < 0)
            return ret;
        memset(ast->codecpar->extradata, 0, 12);
        avio_seek(pb, best_st.off + best_st.audio_off, SEEK_SET);
        ret = avio_read(pb, ast->codecpar->extradata + 12,
                        VX_AUDIO_EXTRADATA_SIZE);
        if (ret != VX_AUDIO_EXTRADATA_SIZE)
            return AVERROR_INVALIDDATA;
    }

    ret = read_indexes(avctx, avctx->streams[s->audio_idx], s, &best_st);
    if (ret < 0)
        return ret;

    read_chapters(avctx, &best_st);

    return 0;
}

static int gbavx_read_seek(AVFormatContext *avctx, int stream_index,
                           int64_t timestamp, int flags)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    int64_t frame;
    int index = 0;

    if ((stream_index != s->video_idx && stream_index != s->audio_idx) ||
        flags & AVSEEK_FLAG_BYTE)
        return AVERROR(ENOSYS);
    frame = stream_index == s->video_idx ? timestamp :
            av_rescale_q(timestamp, avctx->streams[s->audio_idx]->time_base,
                         avctx->streams[s->video_idx]->time_base);
    for (int i = 1; i < s->nb_segments && s->segments[i].frame <= frame; i++)
        index = i;
    s->video_segment = index;
    s->audio_pos = s->segments[index].audio_pos;
    s->audio_pts = s->segments[index].audio_pts;
    return 0;
}

static int gbavx_read_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    const GBAVXSegment *seg = &s->segments[s->video_segment];
    int64_t end_bit = s->video_segment + 1 < s->nb_segments ?
                      s->segments[s->video_segment + 1].video_bit :
                      (int64_t)(s->st.audio_off - VX_HEADER_SIZE) * 8;
    int64_t aligned_bit = seg->video_bit & ~15LL;
    int64_t valid_bits = end_bit - aligned_bit;
    int64_t pos = s->st.off + VX_HEADER_SIZE + aligned_bit / 8;
    int payload_size, frames, ret;
    uint8_t *payload;

    if (valid_bits <= 0 || valid_bits > INT_MAX ||
        pos < s->st.off + VX_HEADER_SIZE || pos >= s->st.off + s->st.audio_off)
        return AVERROR_INVALIDDATA;
    payload_size = FFALIGN(valid_bits, 16) / 8;
    if (pos + payload_size > s->st.off + s->st.audio_off)
        payload_size = s->st.off + s->st.audio_off - pos;
    frames = (s->video_segment + 1 < s->nb_segments ?
              s->segments[s->video_segment + 1].frame : s->st.nb_frames) - seg->frame;
    if (payload_size <= 0 || frames <= 0)
        return AVERROR_INVALIDDATA;

    ret = av_new_packet(pkt, GBA_VX_PACKET_HEADER_SIZE + payload_size);
    if (ret < 0)
        return ret;
    AV_WL32(pkt->data,      GBA_VX_PACKET_MAGIC);
    AV_WL32(pkt->data + 4,  seg->video_bit - aligned_bit);
    AV_WL32(pkt->data + 8,  valid_bits);
    AV_WL32(pkt->data + 12, frames);
    payload = pkt->data + GBA_VX_PACKET_HEADER_SIZE;
    if (avio_seek(pb, pos, SEEK_SET) < 0 || avio_read(pb, payload, payload_size) != payload_size)
        return AVERROR_INVALIDDATA;
    for (int i = 0; i + 1 < payload_size; i += 2)
        FFSWAP(uint8_t, payload[i], payload[i + 1]);

    pkt->stream_index = s->video_idx;
    pkt->pts = pkt->dts = seg->frame;
    pkt->duration = frames;
    pkt->pos = pos;
    pkt->flags |= AV_PKT_FLAG_KEY;
    s->video_segment++;
    return 0;
}

static int gbavx_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t pos = s->audio_pos;
    int nb_aframes = 0, packet_is_key = 0, ret;

    if (s->video_segment < s->nb_segments &&
        (pos + 4 > s->audio_end ||
         av_compare_ts(s->segments[s->video_segment].frame,
                       avctx->streams[s->video_idx]->time_base,
                       s->audio_pts,
                       avctx->streams[s->audio_idx]->time_base) <= 0))
        return gbavx_read_video_packet(avctx, pkt);

    if (pos + 4 > s->audio_end)
        return AVERROR_EOF;

    /* Walk whole AFrames so a packet never splits one. */
    avio_seek(pb, pos, SEEK_SET);
    while (nb_aframes < AFRAMES_PER_PACKET) {
        uint16_t word1;
        int size;

        if (pos + 4 > s->audio_end)
            break;
        word1 = avio_rl16(pb);
        if (!nb_aframes)
            packet_is_key = ((word1 >> 9) & 0x7f) == 0x7f;
        size = aframe_size(avio_rl16(pb));
        if (pos + size > s->audio_end)
            break;
        avio_skip(pb, size - 4);
        pos += size;
        nb_aframes++;
    }
    if (!nb_aframes)
        return AVERROR_EOF;

    ret = av_new_packet(pkt, 4 + (int)(pos - s->audio_pos));
    if (ret < 0)
        return ret;
    AV_WL32(pkt->data, nb_aframes);

    avio_seek(pb, s->audio_pos, SEEK_SET);
    ret = avio_read(pb, pkt->data + 4, (int)(pos - s->audio_pos));
    if (ret != (int)(pos - s->audio_pos))
        return AVERROR_INVALIDDATA;

    pkt->stream_index = s->audio_idx;
    pkt->pts          = s->audio_pts;
    pkt->duration     = (int64_t)nb_aframes * AFRAME_SAMPLES;
    if (packet_is_key)
        pkt->flags |= AV_PKT_FLAG_KEY;

    s->audio_pts += pkt->duration;
    s->audio_pos  = pos;

    return 0;
}

static int gbavx_read_close(AVFormatContext *avctx)
{
    GBAVXDemuxContext *s = avctx->priv_data;

    av_freep(&s->segments);
    return 0;
}

#define OFFSET(x) offsetof(GBAVXDemuxContext, x)
static const AVOption gbavx_options[] = {
    { "resource", "index of the movie to demux, default the longest",
      OFFSET(resource), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0,
      AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass gbavx_class = {
    .class_name = "GBA Video (VX++) demuxer",
    .item_name  = av_default_item_name,
    .option     = gbavx_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_gbavx_demuxer = {
    .p.name         = "gbavx",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ActImagine GBA Video ROM (VX++)"),
    .p.extensions   = "gba",
    .p.priv_class   = &gbavx_class,
    .p.flags        = AVFMT_GENERIC_INDEX,
    .priv_data_size = sizeof(GBAVXDemuxContext),
    .read_probe     = gbavx_probe,
    .read_header    = gbavx_read_header,
    .read_packet    = gbavx_read_packet,
    .read_seek      = gbavx_read_seek,
    .read_close     = gbavx_read_close,
};
