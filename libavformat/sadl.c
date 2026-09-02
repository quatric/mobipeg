/*
 * Level-5 SADL audio demuxer
 *
 * This file is part of FFmpeg / mobipeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

typedef struct SADLDemuxContext {
    int64_t data_end;
} SADLDemuxContext;

static int sadl_probe(const AVProbeData *p)
{
    if (p->buf_size < 0x44)
        return 0;
    if (!memcmp(p->buf, "SADL", 4))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int sadl_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    SADLDemuxContext *ctx = s->priv_data;
    AVStream *st;
    uint8_t hdr[0x100];
    uint32_t sample_rate, channels, file_sz;
    uint8_t coding;

    if (avio_read(pb, hdr, 0x100) != 0x100)
        return AVERROR_INVALIDDATA;

    channels = hdr[0x32] ? hdr[0x32] : 1;
    coding = hdr[0x33];
    sample_rate = (coding & 6) == 4 ? 32728 : (coding & 6) == 2 ? 22050 : 16364;
    file_sz = AV_RL32(hdr + 0x40);

    if (channels != 1 && channels != 2)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_ADPCM_IMA_DK4;
    st->codecpar->sample_rate = sample_rate;
    st->codecpar->ch_layout.nb_channels = channels;
    if (channels == 2)
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    else
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

    ctx->data_end = file_sz > 0x100 ? file_sz : INT64_MAX;
    avpriv_set_pts_info(st, 64, 1, sample_rate);

    return 0;
}

static int sadl_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    SADLDemuxContext *ctx = s->priv_data;
    int ret, size = 1024;
    int64_t cur = avio_tell(pb);

    if (cur >= ctx->data_end || avio_feof(pb))
        return AVERROR_EOF;

    if (cur + size > ctx->data_end)
        size = (int)(ctx->data_end - cur);

    ret = av_get_packet(pb, pkt, size);
    if (ret < 0)
        return ret;

    pkt->stream_index = 0;
    return ret;
}

const FFInputFormat ff_sadl_demuxer = {
    .p.name         = "sadl",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Level-5 SADL audio"),
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.extensions   = "sad,sadl",
    .priv_data_size = sizeof(SADLDemuxContext),
    .read_probe     = sadl_probe,
    .read_header    = sadl_read_header,
    .read_packet    = sadl_read_packet,
};
