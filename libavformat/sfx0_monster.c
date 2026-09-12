/*
 * Monster Games SFX0 audio demuxer
 *
 * This file is part of FFmpeg / mobipeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

/* SFX0 is the internal name used by Monster Games.  Excite Truck's files
 * retain 0x80 bytes of the enclosing SNG allocation before the SFX0 header. */
#define SFX0_SNG_PREFIX 0x80
#define SFX0_HEADER_MIN 0x5c

typedef struct SFX0DemuxContext {
    AVIOContext *right_pb;
    int64_t      bytes_left;
    int          channels;
} SFX0DemuxContext;

static int sfx0_header_offset(const uint8_t *buf, int size)
{
    static const int offsets[] = { 0, SFX0_SNG_PREFIX };

    for (int i = 0; i < FF_ARRAY_ELEMS(offsets); i++) {
        int off = offsets[i];
        uint32_t data_size, header_size, config1;

        if (size < off + SFX0_HEADER_MIN)
            continue;
        data_size   = AV_RL32(buf + off);
        header_size = AV_RL32(buf + off + 4);
        config1     = AV_RL32(buf + off + 0x18);
        if (data_size && header_size >= SFX0_HEADER_MIN &&
            header_size <= 0x10000 && buf[off + 8] <= 1 &&
            buf[off + 9] == 1 && AV_RL16(buf + off + 0x0c) == 0 &&
            AV_RL16(buf + off + 0x0e) == 1 &&
            AV_RL32(buf + off + 0x10) >= 4000 &&
            AV_RL32(buf + off + 0x10) <= 192000 && config1 == 0x00100000)
            return off;
    }
    return -1;
}

static int sfx0_probe(const AVProbeData *p)
{
    int off = sfx0_header_offset(p->buf, p->buf_size);

    if (off < 0)
        return 0;
    if (p->buf_size >= AV_RL32(p->buf + off) + AV_RL32(p->buf + off + 4))
        return AVPROBE_SCORE_MAX;
    return AVPROBE_SCORE_EXTENSION + 1;
}

static int sfx0_read_sibling_header(AVIOContext *pb, int off, uint32_t size,
                                    uint32_t rate, uint8_t *coeffs)
{
    uint8_t header[SFX0_HEADER_MIN];

    if (avio_seek(pb, off, SEEK_SET) < 0 ||
        avio_read(pb, header, sizeof(header)) != sizeof(header))
        return AVERROR_INVALIDDATA;
    if (!AV_RL32(header) || AV_RL32(header + 4) < SFX0_HEADER_MIN ||
        header[8] > 1 || header[9] != 1 || AV_RL16(header + 0x0c) != 0 ||
        AV_RL16(header + 0x0e) != 1 || AV_RL32(header + 0x10) != rate ||
        AV_RL32(header + 0x18) != 0x00100000 || AV_RL32(header) != size)
        return AVERROR_INVALIDDATA;
    memcpy(coeffs, header + 0x3c, 32);
    return 0;
}

static int sfx0_try_open_right(AVFormatContext *s, SFX0DemuxContext *c,
                               int header_off, uint32_t data_size,
                               uint32_t sample_rate, uint8_t *coeffs)
{
    const char *suffix = ".sfx";
    size_t url_len = strlen(s->url), suffix_len = strlen(suffix);
    char *url;
    int ret;

    if (url_len <= suffix_len || strcmp(s->url + url_len - suffix_len, suffix))
        return 0;
    if (url_len > 6 && !strcmp(s->url + url_len - 6, ".2.sfx"))
        return 0;

    url = av_asprintf("%.*s.2.sfx", (int)(url_len - suffix_len), s->url);
    if (!url)
        return AVERROR(ENOMEM);
    ret = avio_open2(&c->right_pb, url, AVIO_FLAG_READ, &s->interrupt_callback,
                     NULL);
    av_free(url);
    if (ret < 0)
        return 0; /* Mono SFX0 files are valid; the companion is optional. */

    ret = sfx0_read_sibling_header(c->right_pb, header_off, data_size,
                                   sample_rate, coeffs);
    if (ret < 0) {
        avio_closep(&c->right_pb);
        return 0;
    }
    return 1;
}

static int sfx0_read_header(AVFormatContext *s)
{
    SFX0DemuxContext *c = s->priv_data;
    uint8_t header[SFX0_SNG_PREFIX + SFX0_HEADER_MIN];
    AVStream *st;
    int header_off, ret;
    uint32_t data_size, header_size, sample_rate;
    int64_t file_size, data_start;

    if ((ret = ffio_read_size(s->pb, header, sizeof(header))) < 0)
        return ret;
    header_off = sfx0_header_offset(header, sizeof(header));
    if (header_off < 0 || avio_seek(s->pb, header_off, SEEK_SET) < 0 ||
        (ret = ffio_read_size(s->pb, header, SFX0_HEADER_MIN)) < 0)
        return AVERROR_INVALIDDATA;

    data_size   = AV_RL32(header);
    header_size = AV_RL32(header + 4);
    sample_rate = AV_RL32(header + 0x10);
    file_size   = avio_size(s->pb);
    data_start  = header_off + header_size;
    if (file_size < 0 || (int64_t)data_size + header_size != file_size ||
        data_start >= file_size)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_ADPCM_THP;
    st->codecpar->sample_rate = sample_rate;
    st->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    c->channels = sfx0_try_open_right(s, c, header_off, data_size, sample_rate,
                                      header + 0x3c);
    if (c->channels < 0)
        return c->channels;
    c->channels++;
    st->codecpar->ch_layout.nb_channels = c->channels;
    if (c->channels == 2)
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;

    if ((ret = ff_alloc_extradata(st->codecpar, 32 * c->channels)) < 0)
        return ret;
    memcpy(st->codecpar->extradata, header + 0x3c, 32);
    if (c->channels == 2) {
        if ((ret = sfx0_read_sibling_header(c->right_pb, header_off, data_size,
                                            sample_rate,
                                            st->codecpar->extradata + 32)) < 0)
            return ret;
    }

    c->bytes_left = file_size - data_start;
    st->duration = c->bytes_left / 8 * 14;
    avpriv_set_pts_info(st, 64, 1, sample_rate);
    return avio_seek(s->pb, data_start, SEEK_SET) < 0 ? AVERROR(EIO) : 0;
}

static int sfx0_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SFX0DemuxContext *c = s->priv_data;
    int bytes = FFMIN(c->bytes_left, 0x1000);
    int ret;

    bytes -= bytes % 8;
    if (!bytes)
        return AVERROR_EOF;
    if ((ret = av_new_packet(pkt, bytes * c->channels)) < 0)
        return ret;
    ret = avio_read(s->pb, pkt->data, bytes);
    if (ret != bytes)
        goto fail;
    if (c->channels == 2) {
        ret = avio_read(c->right_pb, pkt->data + bytes, bytes);
        if (ret != bytes)
            goto fail;
    }
    pkt->stream_index = 0;
    pkt->duration = bytes / 8 * 14;
    c->bytes_left -= bytes;
    return 0;

fail:
    av_packet_unref(pkt);
    return ret < 0 ? ret : AVERROR_EOF;
}

static int sfx0_read_close(AVFormatContext *s)
{
    SFX0DemuxContext *c = s->priv_data;
    avio_closep(&c->right_pb);
    return 0;
}

const FFInputFormat ff_sfx0_monster_demuxer = {
    .p.name         = "sfx0_monster",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Monster Games SFX0 audio"),
    .p.extensions   = "sfx,sf0",
    .priv_data_size = sizeof(SFX0DemuxContext),
    .read_probe     = sfx0_probe,
    .read_header    = sfx0_read_header,
    .read_packet    = sfx0_read_packet,
    .read_close     = sfx0_read_close,
};
