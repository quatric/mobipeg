/*
 * Nintendo RWAV / FWAV / CWAV demuxer
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

enum RWAVVariant {
    RWAV_VARIANT_RWAV,
    RWAV_VARIANT_FWAV,
    RWAV_VARIANT_CWAV,
};

typedef struct RWAVDemuxContext {
    int      variant;
    int      little_endian;
    int      channels;
    int      is_adpcm;
    int      bytes_per_sample;
    int64_t  samples_left;
    int64_t  total_samples;
    int64_t  ch_data_offsets[FF_DSP_ADPCM_MAX_CHANNELS];
    int64_t  ch_bytes_read;
    int64_t  ch_total_bytes;
} RWAVDemuxContext;

static int probe_rwav(const AVProbeData *p)
{
    if (AV_RL32(p->buf) == MKTAG('R','W','A','V') &&
        (AV_RB16(p->buf + 4) == 0xFEFF || AV_RB16(p->buf + 4) == 0xFFFE))
        return AVPROBE_SCORE_MAX / 3 * 2;
    return 0;
}

static int probe_fwav(const AVProbeData *p)
{
    if (AV_RL32(p->buf) == MKTAG('F','W','A','V') &&
        (AV_RB16(p->buf + 4) == 0xFEFF || AV_RB16(p->buf + 4) == 0xFFFE))
        return AVPROBE_SCORE_MAX / 3 * 2;
    return 0;
}

static int probe_cwav(const AVProbeData *p)
{
    if (AV_RL32(p->buf) == MKTAG('C','W','A','V') &&
        (AV_RB16(p->buf + 4) == 0xFEFF || AV_RB16(p->buf + 4) == 0xFFFE))
        return AVPROBE_SCORE_MAX / 3 * 2;
    return 0;
}

static av_always_inline unsigned int read16(AVFormatContext *s)
{
    RWAVDemuxContext *b = s->priv_data;
    if (b->little_endian)
        return avio_rl16(s->pb);
    else
        return avio_rb16(s->pb);
}

static av_always_inline unsigned int read32(AVFormatContext *s)
{
    RWAVDemuxContext *b = s->priv_data;
    if (b->little_endian)
        return avio_rl32(s->pb);
    else
        return avio_rb32(s->pb);
}

static int rwav_read_header(AVFormatContext *s)
{
    RWAVDemuxContext *b = s->priv_data;
    AVStream *st;
    uint32_t magic;
    int bom;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

    magic = avio_rl32(s->pb);
    bom   = avio_rb16(s->pb);
    if (bom != 0xFEFF && bom != 0xFFFE) {
        av_log(s, AV_LOG_ERROR, "invalid byte order BOM: %04X\n", bom);
        return AVERROR_INVALIDDATA;
    }

    if (magic == MKTAG('F','W','A','V'))
        b->variant = RWAV_VARIANT_FWAV;
    else if (magic == MKTAG('C','W','A','V'))
        b->variant = RWAV_VARIANT_CWAV;
    else if (magic == MKTAG('R','W','A','V'))
        b->variant = RWAV_VARIANT_RWAV;
    else
        return AVERROR_INVALIDDATA;

    b->little_endian = (bom == 0xFFFE);

    if (b->variant == RWAV_VARIANT_RWAV) {
        uint32_t file_size, info_offset, info_length, data_offset, data_length;
        uint32_t info_chunk_size, loop_start_val, data_val, ch_table_off;
        int64_t wave_info_pos;
        int encoding, loop, channels, sample_rate;
        int64_t n_samples, loop_start;

        avio_skip(s->pb, 2); /* version 0x0100 */
        file_size   = read32(s);
        read16(s);           /* header_size (0x20) */
        read16(s);           /* chunk_count (2) */
        info_offset = read32(s);
        info_length = read32(s);
        data_offset = read32(s);
        data_length = read32(s);

        if (avio_seek(s->pb, info_offset, SEEK_SET) < 0)
            return AVERROR_INVALIDDATA;
        if (avio_rl32(s->pb) != MKTAG('I','N','F','O'))
            return AVERROR_INVALIDDATA;
        info_chunk_size = read32(s);
        wave_info_pos   = avio_tell(s->pb);

        encoding = avio_r8(s->pb);
        loop     = avio_r8(s->pb);
        channels = avio_r8(s->pb);
        avio_skip(s->pb, 1);
        sample_rate    = read16(s);
        avio_skip(s->pb, 2);
        loop_start_val = read32(s);
        data_val       = read32(s);
        ch_table_off   = read32(s);

        if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS || sample_rate <= 0)
            return AVERROR_INVALIDDATA;

        n_samples = (encoding == 2)
            ? ff_dsp_adpcm_nibbles_to_samples(data_val)
            : (int64_t)data_val;
        loop_start = (encoding == 2)
            ? ff_dsp_adpcm_nibbles_to_samples(loop_start_val)
            : (int64_t)loop_start_val;

        b->channels         = channels;
        b->total_samples    = n_samples;
        b->samples_left     = n_samples;
        b->is_adpcm         = (encoding == 2);
        b->bytes_per_sample = (encoding == 1) ? 2 : 1;

        st->codecpar->sample_rate           = sample_rate;
        st->codecpar->ch_layout.nb_channels = channels;
        st->start_time                      = 0;
        st->duration                        = n_samples;
        avpriv_set_pts_info(st, 64, 1, sample_rate);

        if (loop) {
            av_dict_set_int(&s->metadata, "loop_start",
                            av_rescale(loop_start, AV_TIME_BASE, sample_rate), 0);
        }

        switch (encoding) {
        case 0: st->codecpar->codec_id = AV_CODEC_ID_PCM_S8_PLANAR; break;
        case 1: st->codecpar->codec_id = b->little_endian ? AV_CODEC_ID_PCM_S16LE_PLANAR : AV_CODEC_ID_PCM_S16BE_PLANAR; break;
        case 2: st->codecpar->codec_id = b->little_endian ? AV_CODEC_ID_ADPCM_THP_LE : AV_CODEC_ID_ADPCM_THP; break;
        default: return AVERROR_INVALIDDATA;
        }

        if (b->is_adpcm) {
            int ret = ff_alloc_extradata(st->codecpar, 32 * channels);
            if (ret < 0)
                return ret;
        }

        for (int ch = 0; ch < channels; ch++) {
            uint32_t ci_off, ch_data_rel;
            if (avio_seek(s->pb, wave_info_pos + ch_table_off + 4 * ch, SEEK_SET) < 0)
                return AVERROR_INVALIDDATA;
            ci_off = read32(s);
            if (avio_seek(s->pb, wave_info_pos + ci_off, SEEK_SET) < 0)
                return AVERROR_INVALIDDATA;
            ch_data_rel = read32(s);
            b->ch_data_offsets[ch] = data_offset + 8 + ch_data_rel;
            if (b->is_adpcm) {
                uint32_t ai_off = read32(s);
                if (avio_seek(s->pb, wave_info_pos + ai_off, SEEK_SET) < 0)
                    return AVERROR_INVALIDDATA;
                if (avio_read(s->pb, st->codecpar->extradata + ch * 32, 32) != 32)
                    return AVERROR_INVALIDDATA;
            }
        }
        b->ch_total_bytes = b->is_adpcm ? ff_dsp_adpcm_byte_count(n_samples) : n_samples * b->bytes_per_sample;
    } else {
        uint32_t info_offset = 0, info_size = 0;
        uint32_t data_offset = 0, data_size = 0;
        uint32_t file_size, loop_start, n_samples, channels;
        int64_t wave_info_pos, ch_anchor_pos;
        int encoding, loop, sample_rate;
        uint16_t block_count;

        read16(s);           /* version 0x0040 */
        avio_skip(s->pb, 4); /* constant 0x00010100 */
        file_size   = read32(s);
        block_count = read16(s);
        avio_skip(s->pb, 2); /* pad */

        for (int i = 0; i < block_count; i++) {
            uint32_t marker = read32(s);
            uint32_t off    = read32(s);
            uint32_t sz     = read32(s);
            if (marker == 0x70000000) {
                info_offset = off;
                info_size   = sz;
            } else if (marker == 0x70010000) {
                data_offset = off;
                data_size   = sz;
            }
        }
        if (!info_offset || !data_offset)
            return AVERROR_INVALIDDATA;

        if (avio_seek(s->pb, info_offset, SEEK_SET) < 0)
            return AVERROR_INVALIDDATA;
        if (avio_rl32(s->pb) != MKTAG('I','N','F','O'))
            return AVERROR_INVALIDDATA;
        read32(s); /* info_chunk_size */
        wave_info_pos = avio_tell(s->pb);

        encoding    = avio_r8(s->pb);
        loop        = avio_r8(s->pb);
        avio_skip(s->pb, 2);
        sample_rate = read32(s);
        loop_start  = read32(s);
        n_samples   = read32(s);
        avio_skip(s->pb, 4);
        channels    = read32(s);
        ch_anchor_pos = wave_info_pos + 0x14;

        if (channels < 1 || channels > FF_DSP_ADPCM_MAX_CHANNELS || sample_rate <= 0)
            return AVERROR_INVALIDDATA;

        b->channels         = channels;
        b->total_samples    = n_samples;
        b->samples_left     = n_samples;
        b->is_adpcm         = (encoding == 2);
        b->bytes_per_sample = (encoding == 1) ? 2 : 1;

        st->codecpar->sample_rate           = sample_rate;
        st->codecpar->ch_layout.nb_channels = channels;
        st->start_time                      = 0;
        st->duration                        = n_samples;
        avpriv_set_pts_info(st, 64, 1, sample_rate);

        if (loop) {
            av_dict_set_int(&s->metadata, "loop_start",
                            av_rescale(loop_start, AV_TIME_BASE, sample_rate), 0);
        }

        switch (encoding) {
        case 0: st->codecpar->codec_id = AV_CODEC_ID_PCM_S8_PLANAR; break;
        case 1: st->codecpar->codec_id = b->little_endian ? AV_CODEC_ID_PCM_S16LE_PLANAR : AV_CODEC_ID_PCM_S16BE_PLANAR; break;
        case 2: st->codecpar->codec_id = b->little_endian ? AV_CODEC_ID_ADPCM_THP_LE : AV_CODEC_ID_ADPCM_THP; break;
        default: return AVERROR_INVALIDDATA;
        }

        if (b->is_adpcm) {
            int ret = ff_alloc_extradata(st->codecpar, 32 * channels);
            if (ret < 0)
                return ret;
        }

        for (int ch = 0; ch < channels; ch++) {
            uint32_t ref_marker, ci_rel, ch_data_rel, ai_rel;
            int64_t ci_pos;
            if (avio_seek(s->pb, ch_anchor_pos + 4 + 8 * ch, SEEK_SET) < 0)
                return AVERROR_INVALIDDATA;
            ref_marker = read32(s);
            ci_rel     = read32(s);
            ci_pos     = ch_anchor_pos + ci_rel;
            if (avio_seek(s->pb, ci_pos, SEEK_SET) < 0)
                return AVERROR_INVALIDDATA;
            avio_skip(s->pb, 4);
            ch_data_rel = read32(s);
            b->ch_data_offsets[ch] = data_offset + 8 + ch_data_rel + 24;
            avio_skip(s->pb, 4);
            ai_rel = read32(s);
            if (b->is_adpcm && ai_rel != 0xFFFFFFFF) {
                if (avio_seek(s->pb, ci_pos + ai_rel, SEEK_SET) < 0)
                    return AVERROR_INVALIDDATA;
                if (avio_read(s->pb, st->codecpar->extradata + ch * 32, 32) != 32)
                    return AVERROR_INVALIDDATA;
            }
        }
        b->ch_total_bytes = b->is_adpcm ? ff_dsp_adpcm_byte_count(n_samples) : n_samples * b->bytes_per_sample;
    }

    return 0;
}

static int rwav_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    RWAVDemuxContext *b = s->priv_data;
    int64_t want;
    int per_ch, ret;

    if (b->samples_left <= 0 || avio_feof(s->pb))
        return AVERROR_EOF;

    want = FFMIN(b->samples_left, 14 * 1024);
    per_ch = b->is_adpcm ? ff_dsp_adpcm_byte_count(want) : (int)(want * b->bytes_per_sample);
    if (b->ch_bytes_read + per_ch > b->ch_total_bytes)
        per_ch = b->ch_total_bytes - b->ch_bytes_read;

    if (per_ch <= 0)
        return AVERROR_EOF;

    ret = av_new_packet(pkt, per_ch * b->channels);
    if (ret < 0)
        return ret;

    for (int ch = 0; ch < b->channels; ch++) {
        if (avio_seek(s->pb, b->ch_data_offsets[ch] + b->ch_bytes_read, SEEK_SET) < 0) {
            av_packet_unref(pkt);
            return AVERROR_INVALIDDATA;
        }
        ret = avio_read(s->pb, pkt->data + ch * per_ch, per_ch);
        if (ret != per_ch) {
            av_packet_unref(pkt);
            return ret < 0 ? ret : AVERROR_EOF;
        }
    }

    b->ch_bytes_read += per_ch;
    pkt->stream_index = 0;
    pkt->duration     = want;
    b->samples_left  -= want;

    if (b->is_adpcm) {
        int64_t produced = ((int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME) * FF_DSP_ADPCM_SAMPLES_PER_FRAME;
        if (produced > want) {
            uint8_t *side = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (!side)
                return AVERROR(ENOMEM);
            AV_WL32(side, 0);
            AV_WL32(side + 4, produced - want);
            side[8] = side[9] = 0;
        }
    }

    return 0;
}

const FFInputFormat ff_rwav_demuxer = {
    .p.name         = "rwav",
    .p.long_name    = NULL_IF_CONFIG_SMALL("RWAV (Binary Revolution Wave)"),
    .p.extensions   = "rwav,brwav",
    .priv_data_size = sizeof(RWAVDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = probe_rwav,
    .read_header    = rwav_read_header,
    .read_packet    = rwav_read_packet,
};

const FFInputFormat ff_fwav_demuxer = {
    .p.name         = "fwav",
    .p.long_name    = NULL_IF_CONFIG_SMALL("FWAV (Binary Cafe Wave)"),
    .p.extensions   = "fwav,bfwav",
    .priv_data_size = sizeof(RWAVDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = probe_fwav,
    .read_header    = rwav_read_header,
    .read_packet    = rwav_read_packet,
};

const FFInputFormat ff_cwav_demuxer = {
    .p.name         = "cwav",
    .p.long_name    = NULL_IF_CONFIG_SMALL("CWAV (Binary CTR Wave)"),
    .p.extensions   = "cwav,bcwav",
    .priv_data_size = sizeof(RWAVDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = probe_cwav,
    .read_header    = rwav_read_header,
    .read_packet    = rwav_read_packet,
};
