/*
 * AST demuxer
 * Copyright (c) 2012 Paul B Mahol
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

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "ast.h"

typedef struct ASTDemuxContext {
    int64_t samples_left;
} ASTDemuxContext;

static int ast_probe(const AVProbeData *p)
{
    if (AV_RL32(p->buf) != MKTAG('S','T','R','M'))
        return 0;

    if (!AV_RB16(p->buf + 10) ||
        !AV_RB16(p->buf + 12) || AV_RB16(p->buf + 12) > 256 ||
        !AV_RB32(p->buf + 16) || AV_RB32(p->buf + 16) > 8*48000)
        return AVPROBE_SCORE_MAX / 8;

    return AVPROBE_SCORE_MAX / 3 * 2;
}

static int ast_read_header(AVFormatContext *s)
{
    int depth;
    AVStream *st;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    avio_skip(s->pb, 8);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = ff_codec_get_id(ff_codec_ast_tags, avio_rb16(s->pb));

    depth = avio_rb16(s->pb);
    if (depth != 16) {
        avpriv_request_sample(s, "depth %d", depth);
        return AVERROR_INVALIDDATA;
    }

    st->codecpar->ch_layout.nb_channels = avio_rb16(s->pb);
    if (!st->codecpar->ch_layout.nb_channels)
        return AVERROR_INVALIDDATA;

    if (st->codecpar->ch_layout.nb_channels == 2)
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    else if (st->codecpar->ch_layout.nb_channels == 4)
        st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_4POINT0;

    avio_skip(s->pb, 2);
    st->codecpar->sample_rate = avio_rb32(s->pb);
    if (st->codecpar->sample_rate <= 0)
        return AVERROR_INVALIDDATA;
    st->start_time         = 0;
    st->duration           = avio_rb32(s->pb);
    avio_skip(s->pb, 40);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    ((ASTDemuxContext *)s->priv_data)->samples_left = st->duration;

    return 0;
}

/* Samples one channel's worth of @p size bytes decodes to. ADPCM AFC packs 16
 * samples into 9 bytes; PCM is 16-bit, which read_header has already
 * checked is the only depth this demuxer accepts. */
static int64_t ast_block_samples(const AVCodecParameters *par, uint32_t size)
{
    if (par->codec_id == AV_CODEC_ID_ADPCM_AFC)
        return (int64_t)size / 9 * 16;
    return (int64_t)size / 2;
}

static int ast_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    uint32_t type, size;
    int64_t pos;
    int ret;

    if (avio_feof(s->pb))
        return AVERROR_EOF;

    pos  = avio_tell(s->pb);
    type = avio_rl32(s->pb);
    size = avio_rb32(s->pb);
    if (!s->streams[0]->codecpar->ch_layout.nb_channels ||
        size > INT_MAX / s->streams[0]->codecpar->ch_layout.nb_channels)
        return AVERROR_INVALIDDATA;

    size *= s->streams[0]->codecpar->ch_layout.nb_channels;
    if ((ret = avio_skip(s->pb, 24)) < 0) // padding
        return ret;

    if (type == MKTAG('B','L','C','K')) {
        ASTDemuxContext *c = s->priv_data;
        AVCodecParameters *par = s->streams[0]->codecpar;
        int64_t produced = ast_block_samples(par, size / par->ch_layout.nb_channels);

        ret = av_get_packet(s->pb, pkt, size);
        pkt->stream_index = 0;
        pkt->pos = pos;

        /* A block holds whole frames, so the last one runs past the end of
         * the stream by up to 15 samples of padding. The header's sample
         * count is what bounds the audio, so discard the overshoot rather
         * than letting it reach the output. */
        if (ret >= 0 && c->samples_left > 0 && produced > c->samples_left) {
            uint8_t *side = av_packet_new_side_data(pkt,
                                                    AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (!side)
                return AVERROR(ENOMEM);
            AV_WL32(side,     0);
            AV_WL32(side + 4, produced - c->samples_left);
            side[8] = side[9] = 0;
        }
        c->samples_left -= produced;
    } else {
        av_log(s, AV_LOG_ERROR, "unknown chunk %"PRIx32"\n", type);
        avio_skip(s->pb, size);
        ret = AVERROR_INVALIDDATA;
    }

    return ret;
}

const FFInputFormat ff_ast_demuxer = {
    .p.name         = "ast",
    .p.long_name    = NULL_IF_CONFIG_SMALL("AST (Audio Stream)"),
    .p.extensions   = "ast",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.codec_tag    = ff_ast_codec_tags_list,
    .priv_data_size = sizeof(ASTDemuxContext),
    .read_probe     = ast_probe,
    .read_header    = ast_read_header,
    .read_packet    = ast_read_packet,
};
