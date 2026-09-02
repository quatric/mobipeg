/*
 * HAL Laboratory HPS demuxer
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
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

typedef struct HPSDemuxContext {
    uint32_t block_size;
} HPSDemuxContext;

static int hps_probe(const AVProbeData *p)
{
    if (p->buf_size < 16)
        return 0;
    if (!memcmp(p->buf, "HALPST17", 8) ||
        !memcmp(p->buf, "HALPST16", 8) ||
        !memcmp(p->buf, "HALPST15", 8))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int hps_read_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    AVStream *st;
    uint32_t sample_rate, channels, max_block;

    avio_skip(pb, 8); // Skip HALPSTxx
    sample_rate = avio_rb32(pb);
    channels    = avio_rb32(pb);
    max_block   = avio_rb32(pb);
    (void)max_block;

    if (channels != 1 && channels != 2)
        return AVERROR_INVALIDDATA;
    if (sample_rate == 0 || sample_rate > 96000)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = AV_CODEC_ID_ADPCM_THP;
    st->codecpar->sample_rate = sample_rate;
    st->codecpar->ch_layout.nb_channels = channels;
    if (channels == 2)
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    else
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;

    // Read DSP coefficient table (32 bytes per channel) into extradata
    if (ff_alloc_extradata(st->codecpar, 32 * channels))
        return AVERROR(ENOMEM);

    for (uint32_t ch = 0; ch < channels; ch++) {
        avio_seek(pb, 0x20 + ch * 0x38, SEEK_SET);
        if (avio_read(pb, st->codecpar->extradata + ch * 32, 32) != 32)
            return AVERROR_INVALIDDATA;
    }

    avio_seek(pb, 0x80, SEEK_SET);
    avpriv_set_pts_info(st, 64, 1, sample_rate);

    return 0;
}

static int hps_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    AVStream *st = s->streams[0];
    int channels = st->codecpar->ch_layout.nb_channels;
    uint32_t block_size, next_block;
    int ret;

    if (avio_feof(pb))
        return AVERROR_EOF;

    block_size = avio_rb32(pb);
    if (block_size == 0 || block_size > 0x100000 || avio_feof(pb))
        return AVERROR_EOF;

    avio_skip(pb, 4); // initial hist
    next_block = avio_rb32(pb);
    avio_skip(pb, 4);

    ret = av_get_packet(pb, pkt, block_size);
    if (ret < 0)
        return ret;

    pkt->stream_index = 0;
    pkt->duration = (block_size / (8 * channels)) * 14;

    if (next_block != 0 && next_block > avio_tell(pb))
        avio_seek(pb, next_block, SEEK_SET);

    return ret;
}

const FFInputFormat ff_hps_demuxer = {
    .p.name         = "hps",
    .p.long_name    = NULL_IF_CONFIG_SMALL("HAL Laboratory HPS Audio Stream"),
    .p.extensions   = "hps",
    .priv_data_size = sizeof(HPSDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = hps_probe,
    .read_header    = hps_read_header,
    .read_packet    = hps_read_packet,
};
