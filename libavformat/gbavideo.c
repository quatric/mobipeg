/*
 * ADS-era (Majesco) GBA Video demuxer
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
 * Reads the .mmstr resource files that ADS-era GBA Video carts carry inside
 * their SFCD archive (tools/gba_video/ads_extract.py pulls them out of a ROM).
 *
 * A .mmstr is [uint32 total][uint32 count] then a run of resources, each a
 * single uint32 of size_in_words:28 | type:4 followed by its payload. Type 1
 * is video, 2 audio, 4 still images and 5 text.
 *
 * A video resource opens with a header naming the movie and listing its
 * chapters, then a run of chunks; each chunk is an 8-byte header, an
 * LZMA-compressed codebook and an LZMA-compressed plane of block indices
 * covering several frames. One chunk becomes one packet.
 *
 * An audio resource is [uint32][uint32][uint32][uint16 0x000c]
 * [uint16 block_count][uint16 block_size[]] with the sizes in words, padded
 * to four bytes, then the blocks back to back. One block becomes one packet.
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define T_VIDEO 1
#define T_AUDIO 2
#define T_IMAGE 4
#define T_TEXT  5

typedef struct GBAVideoDemuxContext {
    const AVClass *class;

    int frame_rate;
    int sample_rate;
    const char *resource;       /* SFCD member to play, NULL = the largest */
    int64_t best_size;

    int video_idx, audio_idx;

    int64_t chunk_pos;          /* next video chunk */
    int64_t chunk_end;
    int64_t video_pts;

    int64_t audio_pos;          /* next audio block */
    int nb_blocks, block;
    uint16_t *block_size;       /* in words */
    int64_t audio_pts;
    int samples_per_block;      /* estimated, see read_header */
    int64_t nb_video_frames;
} GBAVideoDemuxContext;

/** Skip a NUL-terminated obfuscated string, padded to four bytes. */
static int64_t skip_name(AVIOContext *pb, int64_t pos)
{
    int len = 0;

    avio_seek(pb, pos, SEEK_SET);
    while (avio_r8(pb) && !avio_feof(pb))
        len++;

    return pos + ((len + 4) & ~3);
}

static int gbavideo_probe(const AVProbeData *p)
{
    uint32_t total, count, pos = 8;
    int audio_ok = 0;

    if (p->buf_size < 16)
        return 0;

    total = AV_RL32(p->buf);
    count = AV_RL32(p->buf + 4);

    if (!count || (count & 0xFF) != count || total < 8)
        return 0;

    /* Walk as many resource headers as the probe buffer holds and insist that
     * every one of them carries a known type and a sane size. */
    for (unsigned i = 0; i < (count & 0xFF); i++) {
        uint32_t w, nbytes, type;

        if (pos + 4 > p->buf_size) {
            /* The probe buffer rarely spans a whole .mmstr; a run of
             * well-formed resource headers is evidence enough. */
            if (audio_ok)
                return AVPROBE_SCORE_MAX / 2;
            return i >= 3 ? AVPROBE_SCORE_MAX / 2 :
                   i >= 1 ? AVPROBE_SCORE_MAX / 4 : 0;
        }

        w      = AV_RL32(p->buf + pos);
        nbytes = (w & 0x0FFFFFFF) * 4;
        type   = w >> 28;

        if (type != T_VIDEO && type != T_AUDIO &&
            type != T_IMAGE && type != T_TEXT)
            return 0;
        if (!nbytes)
            return 0;

        /* An audio resource carries a 0x000c marker at a fixed offset, which
         * is the closest thing this container has to a magic number. */
        if (type == T_AUDIO && pos + 16 <= p->buf_size) {
            if (AV_RL32(p->buf + pos + 12) || AV_RL16(p->buf + pos + 16) != 0x0c)
                return 0;
            audio_ok = 1;
        }
        pos += 4 + nbytes;
    }

    /* The walk consumed every resource; insist it lands where the header
     * said it would. */
    return pos == total + 4 ? AVPROBE_SCORE_MAX * 3 / 4 : AVPROBE_SCORE_MAX / 2;
}

static int parse_video_resource(AVFormatContext *avctx, int64_t off,
                                uint32_t nbytes)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    AVStream *st;
    uint32_t w0;
    int nb_chapters;
    int64_t pos;

    avio_seek(pb, off, SEEK_SET);
    w0 = avio_rl32(pb);
    nb_chapters = w0 & 0xFF;
    avio_rl32(pb);                      /* magic */
    avio_rl32(pb);                      /* chunk count */

    pos = skip_name(pb, off + 12);      /* title */

    for (int i = 0; i < nb_chapters; i++) {
        pos = skip_name(pb, pos + 2);   /* uint16 first chunk, then a name */
        if (pos > off + nbytes)
            return AVERROR_INVALIDDATA;
    }

    s->chunk_pos = (pos + 3) & ~3;
    s->chunk_end = off + nbytes - 4;    /* trailing uint32 terminator */

    st = avformat_new_stream(avctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_ADS_GBA;
    st->codecpar->width      = 240;
    st->codecpar->height     = 160;
    st->nb_frames            = (w0 >> 8) & 0xFFFF;
    s->nb_video_frames       = st->nb_frames;
    s->video_idx             = st->index;
    avpriv_set_pts_info(st, 64, 1, s->frame_rate);

    return 0;
}

static int parse_audio_resource(AVFormatContext *avctx, int64_t off,
                                uint32_t nbytes)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    AVStream *st;
    int nb_blocks;

    avio_seek(pb, off + 14, SEEK_SET);
    nb_blocks = avio_rl16(pb);
    if (nb_blocks <= 0 || 16 + 2 * (int64_t)nb_blocks > nbytes)
        return AVERROR_INVALIDDATA;

    s->block_size = av_malloc_array(nb_blocks, sizeof(*s->block_size));
    if (!s->block_size)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_blocks; i++)
        s->block_size[i] = avio_rl16(pb);
    if (avio_feof(pb))
        return AVERROR_INVALIDDATA;

    s->nb_blocks = nb_blocks;
    s->audio_pos = off + ((16 + 2 * nb_blocks + 3) & ~3);

    st = avformat_new_stream(avctx, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_ADS_GBA_AUDIO;
    st->codecpar->sample_rate = s->sample_rate;
    st->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    s->audio_idx              = st->index;
    avpriv_set_pts_info(st, 64, 1, s->sample_rate);

    return 0;
}

/** Walk the resource table of the .mmstr that starts at @p base. */
static int read_mmstr(AVFormatContext *avctx, int64_t base)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t pos = base + 8;
    int count, ret;

    s->video_idx = s->audio_idx = -1;

    avio_seek(pb, base, SEEK_SET);
    avio_rl32(pb);                      /* total size */
    count = avio_rl32(pb) & 0xFF;

    for (int i = 0; i < count; i++) {
        uint32_t w, nbytes, type;

        avio_seek(pb, pos, SEEK_SET);
        w = avio_rl32(pb);
        if (avio_feof(pb))
            break;

        nbytes = (w & 0x0FFFFFFF) * 4;
        type   = w >> 28;

        if (type == T_VIDEO && s->video_idx < 0) {
            ret = parse_video_resource(avctx, pos + 4, nbytes);
            if (ret < 0)
                return ret;
        } else if (type == T_AUDIO && s->audio_idx < 0) {
            ret = parse_audio_resource(avctx, pos + 4, nbytes);
            if (ret < 0)
                return ret;
        }
        pos += 4 + nbytes;
    }

    if (s->video_idx < 0 && s->audio_idx < 0) {
        av_log(avctx, AV_LOG_ERROR, "no video or audio resource found\n");
        return AVERROR_INVALIDDATA;
    }

    /* Neither the block table nor the block itself records how many samples a
     * block carries, so pace the audio off the movie's own length: the two
     * streams cover the same wall clock. Without video there is nothing to
     * pace against and blocks are timed by their coded size instead. */
    if (s->audio_idx >= 0) {
        if (s->nb_video_frames > 0)
            s->samples_per_block = av_rescale(s->nb_video_frames, s->sample_rate,
                                              (int64_t)s->frame_rate * s->nb_blocks);
        if (s->samples_per_block <= 0)
            s->samples_per_block = 0;
    }

    return 0;
}

static int gbavideo_read_header(AVFormatContext *avctx)
{
    return read_mmstr(avctx, 0);
}

/*
 * An ADS-era cartridge keeps its .mmstr resources in an SFCD archive:
 *
 *     'SFCD' | uint16 data_off | .. | uint32 count
 *     count * { uint32 size, uint32 offset, NUL-terminated name, pad to 4 }
 *     file data at sfcd + data_off + 8 + offset
 *
 * so a ROM can be demuxed directly, without unpacking it first.
 */
#define SFCD_SEARCH_LEN (1 << 20)

static int64_t find_sfcd(AVIOContext *pb)
{
    uint32_t window = 0;

    avio_seek(pb, 0, SEEK_SET);
    for (int64_t i = 0; i < SFCD_SEARCH_LEN && !avio_feof(pb); i++) {
        window = (window << 8) | avio_r8(pb);
        if (window == MKBETAG('S', 'F', 'C', 'D'))
            return i - 3;
    }
    return -1;
}

static int gbavideo_rom_read_header(AVFormatContext *avctx)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    int64_t sfcd, data_base, pos, want = -1;
    uint32_t count;

    sfcd = find_sfcd(pb);
    if (sfcd < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "no SFCD archive found; not an ADS-era GBA Video ROM\n");
        return AVERROR_INVALIDDATA;
    }
    av_log(avctx, AV_LOG_VERBOSE, "SFCD archive at 0x%"PRIx64"\n", sfcd);

    avio_seek(pb, sfcd + 4, SEEK_SET);
    data_base = sfcd + avio_rl16(pb) + 8;
    avio_seek(pb, sfcd + 8, SEEK_SET);
    count = avio_rl32(pb);
    if (!count || count > 4096)
        return AVERROR_INVALIDDATA;

    /* The archive holds menu art and text as well as the movies, so pick the
     * requested member, else the largest - which is always a movie. */
    pos = sfcd + 12;
    for (uint32_t i = 0; i < count; i++) {
        char name[256];
        uint32_t size, off;
        int len = 0, c;

        avio_seek(pb, pos, SEEK_SET);
        size = avio_rl32(pb);
        off  = avio_rl32(pb);
        while ((c = avio_r8(pb)) && !avio_feof(pb))
            if (len < sizeof(name) - 1)
                name[len++] = c;
        name[len] = 0;
        if (avio_feof(pb))
            return AVERROR_INVALIDDATA;

        av_log(avctx, AV_LOG_VERBOSE, "  [%u] %-24s %u bytes\n", i, name, size);

        /* Match with or without the .mmstr suffix, so -resource "jingle"
         * finds "jingle.mmstr". */
        if (s->resource ? !av_strcasecmp(name, s->resource) ||
                          (av_strstart(name, s->resource, NULL) &&
                           !av_strcasecmp(name + strlen(s->resource), ".mmstr"))
                        : size > (uint32_t)s->best_size) {
            want         = data_base + off;
            s->best_size = size;
        }
        pos += 8 + ((len + 4) & ~3);
    }

    if (want < 0) {
        av_log(avctx, AV_LOG_ERROR, "no resource named '%s' in the archive\n",
               s->resource);
        return AVERROR(EINVAL);
    }

    return read_mmstr(avctx, want);
}

static int gbavideo_rom_probe(const AVProbeData *p)
{
    /* Every GBA cartridge header carries a fixed 0x96 at 0xb2; requiring the
     * SFCD magic as well keeps this off non-ADS carts. */
    if (p->buf_size < 0xc0 || p->buf[0xb2] != 0x96)
        return 0;

    for (int i = 0; i + 4 <= p->buf_size; i++)
        if (AV_RB32(p->buf + i) == MKBETAG('S', 'F', 'C', 'D'))
            return AVPROBE_SCORE_MAX;

    return 0;
}

static int read_video_chunk(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    AVIOContext *pb = avctx->pb;
    uint32_t a, b;
    int size_a, size_b, nb_frames, ret;

    avio_seek(pb, s->chunk_pos, SEEK_SET);
    a = avio_rl32(pb);
    b = avio_rl32(pb);
    if (avio_feof(pb))
        return AVERROR_EOF;

    nb_frames = a >> 16;
    size_a    = (b & 0x1FFF) * 4;
    size_b    = (b >> 13)    * 4;
    if (!size_a || !size_b)
        return AVERROR_INVALIDDATA;

    avio_seek(pb, s->chunk_pos, SEEK_SET);
    ret = av_get_packet(pb, pkt, 8 + size_a + size_b);
    if (ret < 0)
        return ret;

    pkt->stream_index = s->video_idx;
    pkt->pts          = s->video_pts;
    pkt->duration     = nb_frames;
    pkt->flags       |= AV_PKT_FLAG_KEY;

    s->video_pts  += nb_frames;
    s->chunk_pos  += 8 + size_a + size_b;

    return 0;
}

static int read_audio_block(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    int size = s->block_size[s->block] * 4;
    int ret;

    avio_seek(avctx->pb, s->audio_pos, SEEK_SET);
    ret = av_get_packet(avctx->pb, pkt, size);
    if (ret < 0)
        return ret;

    pkt->stream_index = s->audio_idx;
    pkt->pts          = s->audio_pts;
    pkt->flags       |= AV_PKT_FLAG_KEY;

    s->audio_pos += size;
    s->block++;

    return 0;
}

static int gbavideo_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    GBAVideoDemuxContext *s = avctx->priv_data;
    int have_video = s->video_idx >= 0 && s->chunk_pos + 8 <= s->chunk_end;
    int have_audio = s->audio_idx >= 0 && s->block < s->nb_blocks;
    int ret;

    if (!have_video && !have_audio)
        return AVERROR_EOF;

    /* Interleave by time so players do not have to buffer a whole stream. */
    if (have_audio && (!have_video ||
                       av_compare_ts(s->audio_pts, (AVRational){ 1, s->sample_rate },
                                     s->video_pts, (AVRational){ 1, s->frame_rate }) <= 0)) {
        ret = read_audio_block(avctx, pkt);
        if (ret >= 0) {
            pkt->duration = s->samples_per_block ? s->samples_per_block
                                                 : pkt->size * 8 / 2;
            s->audio_pts += pkt->duration;
        }
        return ret;
    }

    return read_video_chunk(avctx, pkt);
}

static int gbavideo_read_close(AVFormatContext *avctx)
{
    GBAVideoDemuxContext *s = avctx->priv_data;

    av_freep(&s->block_size);
    return 0;
}

#define OFFSET(x) offsetof(GBAVideoDemuxContext, x)
static const AVOption gbavideo_options[] = {
    { "frame_rate", "video frame rate (the container does not record one)",
      OFFSET(frame_rate), AV_OPT_TYPE_INT, { .i64 = 30 }, 1, 240,
      AV_OPT_FLAG_DECODING_PARAM },
    { "sample_rate", "audio sample rate (the container does not record one)",
      OFFSET(sample_rate), AV_OPT_TYPE_INT, { .i64 = 16384 }, 1000, 48000,
      AV_OPT_FLAG_DECODING_PARAM },
    { "resource", "name of the SFCD member to demux (ROM input only)",
      OFFSET(resource), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0,
      AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass gbavideo_class = {
    .class_name = "GBA Video (ADS) demuxer",
    .item_name  = av_default_item_name,
    .option     = gbavideo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_gbavideo_demuxer = {
    .p.name         = "gbavideo",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ADS-era GBA Video (Majesco)"),
    .p.extensions   = "mmstr",
    .p.priv_class   = &gbavideo_class,
    .p.flags        = AVFMT_GENERIC_INDEX,
    .priv_data_size = sizeof(GBAVideoDemuxContext),
    .read_probe     = gbavideo_probe,
    .read_header    = gbavideo_read_header,
    .read_packet    = gbavideo_read_packet,
    .read_close     = gbavideo_read_close,
};

static const AVClass gbavideo_rom_class = {
    .class_name = "GBA Video (ADS) ROM demuxer",
    .item_name  = av_default_item_name,
    .option     = gbavideo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_gbavideo_rom_demuxer = {
    .p.name         = "gbavideo_rom",
    .p.long_name    = NULL_IF_CONFIG_SMALL("ADS-era GBA Video ROM (Majesco)"),
    .p.extensions   = "gba",
    .p.priv_class   = &gbavideo_rom_class,
    .p.flags        = AVFMT_GENERIC_INDEX,
    .priv_data_size = sizeof(GBAVideoDemuxContext),
    .read_probe     = gbavideo_rom_probe,
    .read_header    = gbavideo_rom_read_header,
    .read_packet    = gbavideo_read_packet,
    .read_close     = gbavideo_read_close,
};
