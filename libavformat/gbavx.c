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
 * no per-frame sizes, so nothing can cut it into packets without decoding it.
 * That bitstream is also not the one libavcodec/vx.c decodes - it is an earlier
 * generation of the codec - so only the audio is exposed here for now.
 *
 * The audio is the DS codec unchanged. Its region opens with the same
 * 3124-byte codebook block, and every AFrame is a whole number of 16-bit words
 * (32 header bits plus 16 per pulse word), so AFrames can be walked without
 * decoding: the walk over the reference cartridge tiles all 9.7 MB of audio
 * exactly and lands on every one of the 181 audio offsets the seek table
 * states.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavcodec/defs.h"

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

typedef struct GBAVXDemuxContext {
    const AVClass *class;
    const char *resource;               /* stream index to play */

    GBAVXStream st;
    int audio_idx;
    int64_t audio_pos, audio_end;
    int64_t audio_pts;
} GBAVXDemuxContext;

/* Bytes an AFrame occupies, given its first two 16-bit words. */
static int aframe_size(uint32_t word2)
{
    static const uint8_t pulse_data_len[4] = { 8, 5, 4, 3 };

    return 4 + 2 * pulse_data_len[(word2 >> 12) & 3];
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

static int gbavx_read_header(AVFormatContext *avctx)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t filesize = avio_size(pb), pos = 0, best = -1;
    int64_t best_size = 0;
    int want = s->resource ? atoi(s->resource) : -1, idx = 0, ret;
    GBAVXStream st = { 0 }, best_st = { 0 };
    uint8_t h[VX_HEADER_SIZE];

    s->audio_idx = -1;
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

    av_log(avctx, AV_LOG_WARNING,
           "video is a %ux%u %u-frame VX bitstream with no frame boundaries "
           "and no decoder yet; demuxing audio only\n",
           best_st.width, best_st.height, best_st.nb_frames);

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

    s->audio_pos = best_st.off + best_st.audio_off + VX_AUDIO_EXTRADATA_SIZE;
    s->audio_end = best_st.off + best_st.chapter_off;

    read_chapters(avctx, &best_st);

    return 0;
}

static int gbavx_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVXDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t pos = s->audio_pos;
    int nb_aframes = 0, ret;

    if (pos + 4 > s->audio_end)
        return AVERROR_EOF;

    /* Walk whole AFrames so a packet never splits one. */
    avio_seek(pb, pos, SEEK_SET);
    while (nb_aframes < AFRAMES_PER_PACKET) {
        int size;

        if (pos + 4 > s->audio_end)
            break;
        avio_skip(pb, 2);
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
    pkt->flags       |= AV_PKT_FLAG_KEY;

    s->audio_pts += pkt->duration;
    s->audio_pos  = pos;

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
};
