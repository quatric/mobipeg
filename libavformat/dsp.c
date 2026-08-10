/*
 * Nintendo DSP-ADPCM (.dsp) demuxer
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

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "dsp_adpcm.h"
#include "internal.h"

typedef struct DSPDemuxContext {
    int      channels;
    int      block;         /* bytes per channel per interleave block */
    int64_t  data_start;
    int64_t  samples_left;
} DSPDemuxContext;

/*
 * .dsp has no magic number, so probing is a consistency check on the header:
 * a plausible sample rate, the one defined format value, a loop flag that is
 * a flag, and a nibble count that agrees with the sample count. Getting all
 * of those right by accident is unlikely but not impossible, so this never
 * claims a score that would beat a format with a real signature.
 */
static int dsp_probe(const AVProbeData *p)
{
    uint32_t samples, nibbles, rate;

    if (p->buf_size < FF_DSP_ADPCM_HEADER_SIZE)
        return 0;

    samples = AV_RB32(p->buf);
    nibbles = AV_RB32(p->buf + 4);
    rate    = AV_RB32(p->buf + 8);

    if (!samples || samples > INT32_MAX / 2)
        return 0;
    if (rate < 4000 || rate > 192000)
        return 0;
    if (AV_RB16(p->buf + 12) > 1)           /* loop flag */
        return 0;
    if (AV_RB16(p->buf + 14) != 0)          /* format: 0 is ADPCM, the only one */
        return 0;
    if (nibbles != ff_dsp_adpcm_nibble_count(samples))
        return 0;

    return AVPROBE_SCORE_MAX / 2;
}

/* True if the 0x60 bytes at buf describe the same stream as the first header,
 * which is how a multi-channel file announces its extra channels. */
static int dsp_is_sibling_header(const uint8_t *buf, uint32_t samples,
                                 uint32_t rate)
{
    return AV_RB32(buf) == samples && AV_RB32(buf + 8) == rate &&
           AV_RB16(buf + 14) == 0;
}

static int dsp_read_header(AVFormatContext *s)
{
    DSPDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t headers[FF_DSP_ADPCM_MAX_CHANNELS][FF_DSP_ADPCM_HEADER_SIZE];
    AVStream *st;
    uint32_t samples, rate;
    int declared, frames_per_block, ret;

    if ((ret = ffio_read_size(pb, headers[0], FF_DSP_ADPCM_HEADER_SIZE)) < 0)
        return ret;

    samples  = AV_RB32(headers[0]);
    rate     = AV_RB32(headers[0] + 8);
    declared = AV_RB16(headers[0] + 0x4A);
    frames_per_block = AV_RB16(headers[0] + 0x4C);

    if (!samples || rate < 4000 || rate > 192000)
        return AVERROR_INVALIDDATA;

    /* The channel-count field is 0 in most files, including every mono one,
     * so fall back to counting the headers that actually follow. */
    c->channels = 1;
    if (declared > 1 && declared <= FF_DSP_ADPCM_MAX_CHANNELS) {
        for (int ch = 1; ch < declared; ch++) {
            if ((ret = ffio_read_size(pb, headers[ch],
                                      FF_DSP_ADPCM_HEADER_SIZE)) < 0)
                return ret;
        }
        c->channels = declared;
    } else if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        while (c->channels < FF_DSP_ADPCM_MAX_CHANNELS) {
            int64_t pos = avio_tell(pb);

            if (avio_read(pb, headers[c->channels],
                          FF_DSP_ADPCM_HEADER_SIZE) != FF_DSP_ADPCM_HEADER_SIZE ||
                !dsp_is_sibling_header(headers[c->channels], samples, rate)) {
                if (avio_seek(pb, pos, SEEK_SET) < 0)
                    return AVERROR_INVALIDDATA;
                break;
            }
            c->channels++;
        }
        if (c->channels > 1)
            frames_per_block = AV_RB16(headers[0] + 0x4C);
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_ADPCM_THP;
    st->codecpar->sample_rate = rate;
    st->codecpar->ch_layout.nb_channels = c->channels;
    st->start_time            = 0;
    st->duration              = samples;
    avpriv_set_pts_info(st, 64, 1, rate);

    /* The adpcm_thp decoder takes the coefficient tables out of extradata and
     * then expects packets to be nothing but ADPCM frames, channel-major. */
    if ((ret = ff_alloc_extradata(st->codecpar, 32 * c->channels)) < 0)
        return ret;
    for (int ch = 0; ch < c->channels; ch++)
        memcpy(st->codecpar->extradata + ch * 32, headers[ch] + 0x1C, 32);

    /* A single-channel file has nothing to interleave; a multi-channel one
     * without a stated block size stores each channel end to end. */
    if (c->channels == 1 || frames_per_block <= 0)
        c->block = ff_dsp_adpcm_byte_count(samples);
    else
        c->block = frames_per_block * FF_DSP_ADPCM_BYTES_PER_FRAME;

    c->data_start   = avio_tell(pb);
    c->samples_left = samples;

    return 0;
}

/* The decoder derives its sample count from the packet size, and a packet
 * always holds whole 14-sample frames, so the final one runs up to 13 samples
 * past the end of the stream. Tell the decoder to discard the overshoot
 * rather than letting it reach the output. */
static int dsp_trim_packet(AVPacket *pkt, int64_t produced, int64_t wanted)
{
    uint8_t *side;

    if (produced <= wanted)
        return 0;
    side = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
    if (!side)
        return AVERROR(ENOMEM);
    AV_WL32(side,     0);                       /* nothing to skip at the start */
    AV_WL32(side + 4, produced - wanted);
    side[8] = side[9] = 0;
    return 0;
}

static int dsp_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DSPDemuxContext *c = s->priv_data;
    int64_t want = FFMIN(c->samples_left,
                         (int64_t)c->block / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME);
    int per_ch, ret;

    if (c->samples_left <= 0 || avio_feof(s->pb))
        return AVERROR_EOF;

    /* Read whole frames only: the decoder derives its sample count from the
     * packet size, so a short read would be reported as fewer samples rather
     * than as an error. */
    per_ch = ff_dsp_adpcm_byte_count(want);

    ret = av_new_packet(pkt, per_ch * c->channels);
    if (ret < 0)
        return ret;

    for (int ch = 0; ch < c->channels; ch++) {
        ret = avio_read(s->pb, pkt->data + (size_t)ch * per_ch, per_ch);
        if (ret != per_ch) {
            av_packet_unref(pkt);
            return ret < 0 ? ret : AVERROR_EOF;
        }
        /* Skip this channel's share of the interleave block that the sample
         * count does not reach into (the muxer pads the last block). */
        if (c->block > per_ch)
            avio_skip(s->pb, c->block - per_ch);
    }

    pkt->stream_index = 0;
    pkt->duration     = want;
    c->samples_left  -= want;

    return dsp_trim_packet(pkt,
                           (int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME *
                           FF_DSP_ADPCM_SAMPLES_PER_FRAME, want);
}

const FFInputFormat ff_dsp_demuxer = {
    .p.name         = "dsp",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Nintendo DSP-ADPCM"),
    .p.extensions   = "dsp",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .priv_data_size = sizeof(DSPDemuxContext),
    .read_probe     = dsp_probe,
    .read_header    = dsp_read_header,
    .read_packet    = dsp_read_packet,
};
