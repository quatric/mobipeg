/*
 * Nintendo BWAV demuxer (Switch Binary Wave, AAL library)
 *
 * BWAV is the streamed-wave container of Nintendo's AAL audio library,
 * used since Super Mario Maker 2 (e.g. Tomodachi Life: Living the Dream).
 * Layout: 0x10 file header ("BWAV", BOM, version, CRC32, prefetch flag,
 * channel count), then one 0x4C channel info per channel, then each
 * channel's sample data as a separate block padded to 0x40.
 *
 * Codec 0 is PCM16, codec 1 is DSP-ADPCM (adpcm_thp). Codec 2 is Opus
 * (Zelda: Tears of the Kingdom) and is not demuxed here.
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
#include "libavutil/mem.h"
#include "libavutil/dict.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "dsp_adpcm.h"
#include "internal.h"

#define BWAV_CHANNEL_INFO_SIZE 0x4C

typedef struct BWAVDemuxContext {
    int      little_endian;
    int      channels;
    int      is_adpcm;
    int      bytes_per_sample;
    int64_t  samples_left;
    int64_t  total_samples;
    int64_t  ch_data_offsets[FF_DSP_ADPCM_MAX_CHANNELS];
    int64_t  ch_bytes_read;
    int64_t  ch_total_bytes;
} BWAVDemuxContext;

static int bwav_probe(const AVProbeData *p)
{
    if (AV_RL32(p->buf) == MKTAG('B','W','A','V') &&
        (AV_RB16(p->buf + 4) == 0xFEFF || AV_RB16(p->buf + 4) == 0xFFFE))
        return AVPROBE_SCORE_MAX / 3 * 2;
    return 0;
}

static av_always_inline unsigned int bwav_read16(AVFormatContext *s)
{
    BWAVDemuxContext *b = s->priv_data;
    if (b->little_endian)
        return avio_rl16(s->pb);
    else
        return avio_rb16(s->pb);
}

static av_always_inline unsigned int bwav_read32(AVFormatContext *s)
{
    BWAVDemuxContext *b = s->priv_data;
    if (b->little_endian)
        return avio_rl32(s->pb);
    else
        return avio_rb32(s->pb);
}

static int bwav_read_header(AVFormatContext *s)
{
    BWAVDemuxContext *b = s->priv_data;
    AVStream *st;
    uint32_t magic;
    int bom, channels, codec = -1, sample_rate = 0;
    int64_t n_samples = 0, loop_start = 0, loop_end = -1;
    uint8_t coefs[FF_DSP_ADPCM_MAX_CHANNELS][32];

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

    magic = avio_rl32(s->pb);
    if (magic != MKTAG('B','W','A','V'))
        return AVERROR_INVALIDDATA;

    bom = avio_rb16(s->pb);
    if (bom != 0xFEFF && bom != 0xFFFE) {
        av_log(s, AV_LOG_ERROR, "invalid byte order BOM: %04X\n", bom);
        return AVERROR_INVALIDDATA;
    }
    b->little_endian = (bom == 0xFFFE);

    bwav_read16(s); /* version (1) */
    bwav_read32(s); /* CRC32 over the concatenated channel data */
    bwav_read16(s); /* prefetch flag: 1 for the short prefetch companion */
    channels = bwav_read16(s);
    if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS) {
        av_log(s, AV_LOG_ERROR, "invalid channel count %d\n", channels);
        return AVERROR_INVALIDDATA;
    }

    for (int ch = 0; ch < channels; ch++) {
        int ch_codec, ch_rate;
        int64_t ch_count;
        uint32_t ch_off;
        int32_t ch_loop_end, ch_loop_start;

        ch_codec = bwav_read16(s);
        bwav_read16(s); /* pan: 0 left, 1 right, 2 middle */
        ch_rate  = bwav_read32(s);
        bwav_read32(s); /* sample count of the full (non-prefetch) file */
        ch_count = bwav_read32(s); /* sample count of this file */
        if (avio_read(s->pb, coefs[ch], 32) != 32)
            return AVERROR_INVALIDDATA;
        bwav_read32(s); /* data offset in the full file */
        ch_off = bwav_read32(s); /* data offset in this file */
        bwav_read32(s); /* loop flag, always 1 even when not looping */
        ch_loop_end   = (int32_t)bwav_read32(s); /* -1 when not looping */
        ch_loop_start = (int32_t)bwav_read32(s);
        bwav_read16(s); /* predictor scale (start context) */
        bwav_read16(s); /* history sample 1 */
        bwav_read16(s); /* history sample 2 */
        bwav_read16(s); /* reserved */

        if (ch_rate <= 0 || ch_count <= 0 || ch_count > INT32_MAX) {
            av_log(s, AV_LOG_ERROR, "invalid channel %d header\n", ch);
            return AVERROR_INVALIDDATA;
        }
        if (ch == 0) {
            codec       = ch_codec;
            sample_rate = ch_rate;
            n_samples   = ch_count;
            loop_start  = ch_loop_start;
            loop_end    = ch_loop_end;
        } else if (ch_codec != codec || ch_rate != sample_rate ||
                   ch_count != n_samples) {
            av_log(s, AV_LOG_ERROR,
                   "channel %d header does not match channel 0\n", ch);
            return AVERROR_INVALIDDATA;
        }
        /* The start context (predictor scale + histories) is only a seek
         * aid; like the other DSP-ADPCM demuxers the decoder starts from
         * silence. */
        b->ch_data_offsets[ch] = ch_off;
        if (ch > 0 && b->ch_data_offsets[ch] <= b->ch_data_offsets[ch - 1]) {
            av_log(s, AV_LOG_ERROR, "channel data offsets not increasing\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (codec == 2) {
        avpriv_request_sample(s, "BWAV Opus (codec 2)");
        return AVERROR_PATCHWELCOME;
    }

    b->channels      = channels;
    b->total_samples = n_samples;
    b->samples_left  = n_samples;

    st->codecpar->sample_rate            = sample_rate;
    st->codecpar->ch_layout.nb_channels = channels;
    st->start_time                       = 0;
    st->duration                         = n_samples;
    avpriv_set_pts_info(st, 64, 1, sample_rate);

    /* loop_end == -1 (0xFFFFFFFF) means "does not loop". */
    if (loop_end != -1 && loop_end > 0) {
        av_dict_set_int(&s->metadata, "loop_start",
                        av_rescale(loop_start, AV_TIME_BASE, sample_rate), 0);
        av_dict_set_int(&s->metadata, "loop_end",
                        av_rescale(loop_end, AV_TIME_BASE, sample_rate), 0);
    }

    switch (codec) {
    case 0:
        st->codecpar->codec_id = b->little_endian ?
            AV_CODEC_ID_PCM_S16LE_PLANAR : AV_CODEC_ID_PCM_S16BE_PLANAR;
        b->bytes_per_sample = 2;
        break;
    case 1:
        st->codecpar->codec_id = b->little_endian ?
            AV_CODEC_ID_ADPCM_THP_LE : AV_CODEC_ID_ADPCM_THP;
        b->is_adpcm = 1;
        break;
    default:
        avpriv_request_sample(s, "BWAV codec %d", codec);
        return AVERROR_PATCHWELCOME;
    }

    if (b->is_adpcm) {
        int ret = ff_alloc_extradata(st->codecpar, 32 * channels);
        if (ret < 0)
            return ret;
        for (int ch = 0; ch < channels; ch++)
            memcpy(st->codecpar->extradata + ch * 32, coefs[ch], 32);
    }

    /* The last ADPCM frame may be truncated to its remaining samples (as
     * in BRSTM's last block) while other channels are padded to 0x40, so
     * the exact stored size varies; the full-frame count is the safe
     * upper bound and short final reads are zero-padded below. */
    b->ch_total_bytes = b->is_adpcm ? ff_dsp_adpcm_byte_count(n_samples)
                                    : n_samples * b->bytes_per_sample;

    return 0;
}

static int bwav_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BWAVDemuxContext *b = s->priv_data;
    int64_t want;
    int per_ch, ret;

    if (b->samples_left <= 0 || avio_feof(s->pb))
        return AVERROR_EOF;

    want = FFMIN(b->samples_left, 14 * 1024);
    per_ch = b->is_adpcm ? (int)ff_dsp_adpcm_byte_count(want)
                         : (int)(want * b->bytes_per_sample);
    if (b->ch_bytes_read + per_ch > b->ch_total_bytes)
        per_ch = (int)(b->ch_total_bytes - b->ch_bytes_read);

    if (per_ch <= 0)
        return AVERROR_EOF;

    ret = av_new_packet(pkt, per_ch * b->channels);
    if (ret < 0)
        return ret;

    for (int ch = 0; ch < b->channels; ch++) {
        if (avio_seek(s->pb, b->ch_data_offsets[ch] + b->ch_bytes_read,
                      SEEK_SET) < 0) {
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        }
        ret = avio_read(s->pb, pkt->data + (size_t)ch * per_ch, per_ch);
        if (ret != per_ch) {
            if (ret >= 0) {
                /* Truncated final ADPCM frame: pad with silence. The
                 * decoder only consumes want samples, so the padding is
                 * never heard. */
                memset(pkt->data + (size_t)ch * per_ch + ret, 0,
                       per_ch - ret);
                for (int r = ch + 1; r < b->channels; r++) {
                    int got = 0;
                    if (avio_seek(s->pb,
                                  b->ch_data_offsets[r] + b->ch_bytes_read,
                                  SEEK_SET) >= 0)
                        got = avio_read(s->pb,
                                        pkt->data + (size_t)r * per_ch,
                                        per_ch);
                    if (got < 0)
                        got = 0;
                    if (got != per_ch)
                        memset(pkt->data + (size_t)r * per_ch + got, 0,
                               per_ch - got);
                }
                break;
            }
            av_packet_unref(pkt);
            return ret;
        }
    }

    b->ch_bytes_read += per_ch;
    pkt->stream_index = 0;
    pkt->duration     = want;
    b->samples_left  -= want;

    if (b->is_adpcm) {
        int64_t produced = ((int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME) *
                           FF_DSP_ADPCM_SAMPLES_PER_FRAME;
        if (produced > want) {
            uint8_t *side = av_packet_new_side_data(pkt,
                                                    AV_PKT_DATA_SKIP_SAMPLES,
                                                    10);
            if (!side)
                return AVERROR(ENOMEM);
            AV_WL32(side, 0);
            AV_WL32(side + 4, produced - want);
            side[8] = side[9] = 0;
        }
    }

    return 0;
}

const FFInputFormat ff_bwav_demuxer = {
    .p.name         = "bwav",
    .p.long_name    = NULL_IF_CONFIG_SMALL("BWAV (Binary Wave)"),
    .p.extensions   = "bwav",
    .priv_data_size = sizeof(BWAVDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = bwav_probe,
    .read_header    = bwav_read_header,
    .read_packet    = bwav_read_packet,
};
