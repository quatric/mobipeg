/*
 * Wii U boot sound (.btsnd) demuxer
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

/*
 * bootSound.btsnd, the jingle a Wii U title plays while it loads. The whole
 * format is an 8-byte big-endian header -- a type word and a loop point --
 * followed by raw 48 kHz stereo 16-bit big-endian PCM. The console's player
 * does not read a sample rate or a channel count from anywhere, so those are
 * fixed and a file that disagrees simply plays wrong.
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "btsnd.h"
#include "demux.h"
#include "internal.h"

#define BTSND_PACKET_SAMPLES 4096

static int btsnd_probe(const AVProbeData *p)
{
    /* There is no magic number: the file starts with a type word that is
     * usually 0 or 2, then a loop point. Claiming anything on content alone
     * would mean claiming most raw data, so this only recognises a file that
     * is already named like one. */
    if (!av_match_ext(p->filename, "btsnd"))
        return 0;
    if (p->buf_size < BTSND_HEADER_SIZE || AV_RB32(p->buf) > 0xFF)
        return 0;

    return AVPROBE_SCORE_EXTENSION;
}

static int btsnd_read_header(AVFormatContext *s)
{
    AVStream *st = avformat_new_stream(s, NULL);
    uint32_t loop_start;

    if (!st)
        return AVERROR(ENOMEM);

    avio_skip(s->pb, 4);            /* type */
    loop_start = avio_rb32(s->pb);

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_PCM_S16BE;
    st->codecpar->sample_rate = BTSND_SAMPLE_RATE;
    st->codecpar->block_align = BTSND_CHANNELS * 2;
    av_channel_layout_default(&st->codecpar->ch_layout, BTSND_CHANNELS);
    st->start_time = 0;
    avpriv_set_pts_info(st, 64, 1, BTSND_SAMPLE_RATE);

    if (loop_start &&
        av_dict_set_int(&s->metadata, "loop_start",
                        av_rescale(loop_start, AV_TIME_BASE,
                                   BTSND_SAMPLE_RATE), 0) < 0)
        return AVERROR(ENOMEM);

    return 0;
}

static int btsnd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVCodecParameters *par = s->streams[0]->codecpar;
    int ret = av_get_packet(s->pb, pkt,
                            BTSND_PACKET_SAMPLES * par->block_align);

    if (ret < 0)
        return ret;
    pkt->stream_index = 0;
    return 0;
}

const FFInputFormat ff_btsnd_demuxer = {
    .p.name         = "btsnd",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Wii U boot sound"),
    .p.extensions   = "btsnd",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .read_probe     = btsnd_probe,
    .read_header    = btsnd_read_header,
    .read_packet    = btsnd_read_packet,
};
