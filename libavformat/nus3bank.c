/*
 * Namco Universal Sound 3 (NUS3BANK / NUS3AUDIO) demuxer and muxer
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

#include "config_components.h"

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/dict.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

#define NUS3_MAGIC MKTAG('N','U','S','3')
#define BANK_MAGIC MKTAG('B','A','N','K')
#define AUDI_MAGIC MKTAG('A','U','D','I')
#define IDSP_MAGIC MKTAG('I','D','S','P')

typedef struct NUS3Track {
    int      id;
    char     name[128];
    int64_t  pack_offset;
    int64_t  pack_size;
    int64_t  data_offset;
    int64_t  data_size;
    int64_t  bytes_read;
    int64_t  cur_pts;
    int      is_idsp;
    int      channels;
    int      sample_rate;
    int64_t  sample_count;
} NUS3Track;

typedef struct NUS3BankDemuxContext {
    int        nb_tracks;
    NUS3Track *tracks;
    int64_t    pack_start;
    int        cur_stream;
} NUS3BankDemuxContext;

static int nus3bank_probe(const AVProbeData *p)
{
    if (p->buf_size < 16)
        return 0;
    if (AV_RL32(p->buf) != NUS3_MAGIC)
        return 0;
    uint32_t type = AV_RL32(p->buf + 4);
    if (type == BANK_MAGIC || type == AUDI_MAGIC ||
        type == MKTAG('b','a','n','k') || type == MKTAG('a','u','d','i'))
        return AVPROBE_SCORE_MAX - 1;
    return 0;
}

static int nus3bank_read_header(AVFormatContext *s)
{
    NUS3BankDemuxContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t magic, toc_magic, chunk_count;
    int64_t prop_offset = 0, binf_offset = 0, tone_offset = 0, pack_offset = 0;
    uint32_t binf_size = 0;

    magic     = avio_rl32(pb);
    avio_rl32(pb); // bank_type
    avio_rl32(pb); // bank_size

    if (magic != NUS3_MAGIC)
        return AVERROR_INVALIDDATA;

    toc_magic = avio_rl32(pb);
    avio_rl32(pb); // toc_size
    if (toc_magic != MKTAG('T','O','C',' '))
        return AVERROR_INVALIDDATA;

    chunk_count = avio_rl32(pb);
    if (chunk_count > 64)
        return AVERROR_INVALIDDATA;

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t cid = avio_rl32(pb);
        uint32_t cofs = avio_rl32(pb);
        uint32_t csz  = avio_rl32(pb);

        if (cid == MKTAG('P','R','O','P')) {
            prop_offset = cofs;
        } else if (cid == MKTAG('B','I','N','F')) {
            binf_offset = cofs;
            binf_size = csz;
        } else if (cid == MKTAG('T','O','N','E') || cid == MKTAG('T','R','A','C')) {
            tone_offset = cofs;
        } else if (cid == MKTAG('P','A','C','K')) {
            pack_offset = cofs;
        }
    }

    if (!tone_offset || !pack_offset) {
        av_log(s, AV_LOG_ERROR, "NUS3BANK missing TONE/TRAC or PACK chunk\n");
        return AVERROR_INVALIDDATA;
    }

    ctx->pack_start = pack_offset + 8; // skip PACK chunk header (magic + size)

    // Read PROP chunk for track count and metadata
    uint32_t nb_tracks = 0;
    if (prop_offset) {
        avio_seek(pb, prop_offset + 8, SEEK_SET);
        nb_tracks = avio_rl32(pb);
    }

    // Fallback: if PROP not present or count 0, check TONE count
    if (!nb_tracks) {
        avio_seek(pb, tone_offset + 8, SEEK_SET);
        nb_tracks = avio_rl32(pb);
    }

    if (!nb_tracks || nb_tracks > 1024)
        return AVERROR_INVALIDDATA;

    ctx->nb_tracks = nb_tracks;
    ctx->tracks = av_calloc(nb_tracks, sizeof(NUS3Track));
    if (!ctx->tracks)
        return AVERROR(ENOMEM);

    // Read BINF names table if available
    if (binf_offset) {
        avio_seek(pb, binf_offset + 8, SEEK_SET);
        uint32_t binf_count = avio_rl32(pb);
        for (uint32_t i = 0; i < binf_count && i < nb_tracks; i++) {
            uint8_t slen = avio_r8(pb);
            if (slen > 0 && slen < sizeof(ctx->tracks[i].name)) {
                avio_read(pb, ctx->tracks[i].name, slen);
                ctx->tracks[i].name[slen] = 0;
            }
            if (avio_tell(pb) < binf_offset + 8 + binf_size && avio_r8(pb) != 0)
                avio_seek(pb, -1, SEEK_CUR);
        }
    }

    // Read TONE chunk for stream offsets and sizes
    avio_seek(pb, tone_offset + 8, SEEK_SET);
    uint32_t tone_count = avio_rl32(pb);
    for (uint32_t i = 0; i < tone_count && i < nb_tracks; i++) {
        ctx->tracks[i].pack_offset = avio_rl32(pb);
        ctx->tracks[i].pack_size   = avio_rl32(pb);
    }

    // Create AVStreams for each track
    for (uint32_t i = 0; i < nb_tracks; i++) {
        NUS3Track *t = &ctx->tracks[i];
        AVStream *st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        st->id = i;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

        if (t->name[0])
            av_dict_set(&st->metadata, "title", t->name, 0);

        int64_t item_pos = ctx->pack_start + t->pack_offset;
        avio_seek(pb, item_pos, SEEK_SET);

        uint32_t inner_magic = avio_rl32(pb);
        if (inner_magic == IDSP_MAGIC) {
            // Nintendo IDSP stream
            t->is_idsp = 1;
            uint32_t channels     = avio_rb32(pb);
            uint32_t sample_rate  = avio_rb32(pb);
            uint32_t num_samples  = avio_rb32(pb);
            uint32_t loop_start   = avio_rb32(pb);
            uint32_t loop_end     = avio_rb32(pb);
            avio_rb32(pb); // interleave
            uint32_t header_size  = avio_rb32(pb);
            uint32_t ch_info_ofs  = avio_rb32(pb);
            uint32_t ch_info_sz   = avio_rb32(pb);
            uint32_t data_offset  = avio_rb32(pb);
            uint32_t data_size    = avio_rb32(pb);

            t->channels     = channels ? channels : 1;
            t->sample_rate  = sample_rate ? sample_rate : 48000;
            t->sample_count = num_samples;
            t->data_offset  = item_pos + (data_offset ? data_offset : header_size);
            t->data_size    = data_size ? data_size : (t->pack_size - (t->data_offset - item_pos));

            st->codecpar->codec_id = AV_CODEC_ID_ADPCM_THP;
            st->codecpar->sample_rate = t->sample_rate;
            st->codecpar->ch_layout.nb_channels = t->channels;
            if (t->channels == 1)
                st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
            else if (t->channels == 2)
                st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;

            st->duration = num_samples;
            avpriv_set_pts_info(st, 64, 1, t->sample_rate);

            // Read DSP coefficients from channel headers into extradata
            if (ch_info_ofs && ch_info_sz) {
                int extradata_size = 32 * t->channels;
                if (ff_alloc_extradata(st->codecpar, extradata_size) >= 0) {
                    for (int ch = 0; ch < t->channels; ch++) {
                        avio_seek(pb, item_pos + ch_info_ofs + ch * ch_info_sz + 0x1c, SEEK_SET);
                        avio_read(pb, st->codecpar->extradata + ch * 32, 32);
                    }
                }
            }

            if (loop_end > loop_start) {
                av_dict_set_int(&st->metadata, "loop_start", loop_start, 0);
                av_dict_set_int(&st->metadata, "loop_end", loop_end, 0);
            }
        } else if (inner_magic == MKTAG('O','P','U','S') ||
                   inner_magic == MKTAG('l','o','p','u') ||
                   inner_magic == MKTAG('L','O','P','U')) {
            st->codecpar->codec_id = AV_CODEC_ID_OPUS;
            st->codecpar->sample_rate = 48000;
            st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            t->data_offset = item_pos;
            t->data_size   = t->pack_size;
            avpriv_set_pts_info(st, 64, 1, 48000);
        } else if (inner_magic == MKTAG('R','I','F','F')) {
            st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
            st->codecpar->sample_rate = 44100;
            st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            t->data_offset = item_pos + 44;
            t->data_size   = t->pack_size > 44 ? t->pack_size - 44 : t->pack_size;
            avpriv_set_pts_info(st, 64, 1, 44100);
        } else {
            // Default raw ADPCM_THP or audio payload
            st->codecpar->codec_id = AV_CODEC_ID_ADPCM_THP;
            st->codecpar->sample_rate = 48000;
            st->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
            t->channels = 2;
            t->sample_rate = 48000;
            t->is_idsp = 1;
            t->data_offset = item_pos;
            t->data_size   = t->pack_size;
            avpriv_set_pts_info(st, 64, 1, 48000);
        }
    }

    return 0;
}

static int nus3bank_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    NUS3BankDemuxContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;

    int start_stream = ctx->cur_stream;
    int stream_idx = -1;

    for (int i = 0; i < ctx->nb_tracks; i++) {
        int idx = (start_stream + i) % ctx->nb_tracks;
        if (ctx->tracks[idx].bytes_read < ctx->tracks[idx].data_size) {
            stream_idx = idx;
            break;
        }
    }

    if (stream_idx < 0)
        return AVERROR_EOF;

    NUS3Track *t = &ctx->tracks[stream_idx];
    int64_t remaining = t->data_size - t->bytes_read;
    int read_size = FFMIN(remaining, 4096);

    avio_seek(pb, t->data_offset + t->bytes_read, SEEK_SET);

    int ret = av_get_packet(pb, pkt, read_size);
    if (ret < 0)
        return ret;

    pkt->stream_index = stream_idx;
    pkt->pts = t->cur_pts;

    if (t->is_idsp && t->channels > 0) {
        int64_t samples = (ret / t->channels) / FF_DSP_ADPCM_BYTES_PER_FRAME * FF_DSP_ADPCM_SAMPLES_PER_FRAME;
        pkt->duration = samples;
        t->cur_pts += samples;
    } else {
        pkt->duration = read_size / 4;
        t->cur_pts += pkt->duration;
    }

    t->bytes_read += ret;
    ctx->cur_stream = (stream_idx + 1) % ctx->nb_tracks;

    return 0;
}

static int nus3bank_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    NUS3BankDemuxContext *ctx = s->priv_data;
    if (stream_index < 0 || stream_index >= ctx->nb_tracks)
        return AVERROR(EINVAL);

    NUS3Track *t = &ctx->tracks[stream_index];
    if (t->sample_rate <= 0)
        return AVERROR(EINVAL);

    int64_t byte_pos = 0;
    if (t->is_idsp && t->channels > 0) {
        byte_pos = ff_dsp_adpcm_byte_count(timestamp) * t->channels;
    } else {
        byte_pos = timestamp * 4;
    }

    if (byte_pos > t->data_size)
        byte_pos = t->data_size;

    t->bytes_read = byte_pos;
    t->cur_pts = timestamp;
    return 0;
}

static int nus3bank_read_close(AVFormatContext *s)
{
    NUS3BankDemuxContext *ctx = s->priv_data;
    av_freep(&ctx->tracks);
    return 0;
}

const FFInputFormat ff_nus3bank_demuxer = {
    .p.name         = "nus3bank",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Namco Universal Sound 3 (NUS3BANK / NUS3AUDIO)"),
    .p.extensions   = "nus3bank,nus3audio",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .priv_data_size = sizeof(NUS3BankDemuxContext),
    .read_probe     = nus3bank_probe,
    .read_header    = nus3bank_read_header,
    .read_packet    = nus3bank_read_packet,
    .read_seek      = nus3bank_read_seek,
    .read_close     = nus3bank_read_close,
};

typedef struct NUS3MuxTrack {
    AVIOContext *buf;
    int64_t      sample_count;
    uint32_t     pack_offset;
    uint32_t     pack_size;
} NUS3MuxTrack;

typedef struct NUS3BankMuxContext {
    uint32_t     track_count;
    NUS3MuxTrack *tracks;
} NUS3BankMuxContext;

static int nus3bank_write_header(AVFormatContext *s)
{
    NUS3BankMuxContext *ctx = s->priv_data;
    ctx->track_count = s->nb_streams;
    if (!ctx->track_count)
        return AVERROR(EINVAL);

    ctx->tracks = av_calloc(ctx->track_count, sizeof(NUS3MuxTrack));
    if (!ctx->tracks)
        return AVERROR(ENOMEM);

    for (uint32_t i = 0; i < ctx->track_count; i++) {
        int ret = avio_open_dyn_buf(&ctx->tracks[i].buf);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int nus3bank_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    NUS3BankMuxContext *ctx = s->priv_data;
    if (pkt->stream_index < 0 || (uint32_t)pkt->stream_index >= ctx->track_count)
        return AVERROR(EINVAL);

    NUS3MuxTrack *t = &ctx->tracks[pkt->stream_index];
    avio_write(t->buf, pkt->data, pkt->size);
    if (pkt->duration > 0)
        t->sample_count += pkt->duration;
    else
        t->sample_count += pkt->size / 2;
    return 0;
}

static int nus3bank_write_trailer(AVFormatContext *s)
{
    NUS3BankMuxContext *ctx = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t n_tracks = ctx->track_count;

    AVIOContext *pack_buf = NULL;
    int ret = avio_open_dyn_buf(&pack_buf);
    if (ret < 0)
        return ret;

    for (uint32_t i = 0; i < n_tracks; i++) {
        NUS3MuxTrack *t = &ctx->tracks[i];
        AVStream *st = s->streams[i];
        uint8_t *raw_audio = NULL;
        int raw_size = avio_close_dyn_buf(t->buf, &raw_audio);
        t->buf = NULL;

        t->pack_offset = avio_tell(pack_buf);

        if (st->codecpar->codec_id == AV_CODEC_ID_ADPCM_THP) {
            // Write IDSP container
            int channels = st->codecpar->ch_layout.nb_channels > 0 ? st->codecpar->ch_layout.nb_channels : 2;
            int sample_rate = st->codecpar->sample_rate > 0 ? st->codecpar->sample_rate : 48000;
            int64_t num_samples = t->sample_count > 0 ? t->sample_count : (raw_size / channels / 8 * 14);

            int ch_info_size = 0x20;
            int header_size = 0x60;
            int data_offset = header_size + (channels * ch_info_size);

            avio_wl32(pack_buf, IDSP_MAGIC);
            avio_wb32(pack_buf, channels);
            avio_wb32(pack_buf, sample_rate);
            avio_wb32(pack_buf, (uint32_t)num_samples);
            avio_wb32(pack_buf, 0); // loop_start
            avio_wb32(pack_buf, (uint32_t)num_samples); // loop_end
            avio_wb32(pack_buf, 0x10); // interleave
            avio_wb32(pack_buf, header_size);
            avio_wb32(pack_buf, header_size); // ch_info_ofs
            avio_wb32(pack_buf, ch_info_size);
            avio_wb32(pack_buf, data_offset);
            avio_wb32(pack_buf, raw_size);

            // Pad up to 0x60
            ffio_fill(pack_buf, 0, header_size - 0x30);

            // Write DSP coefficients for each channel
            for (int ch = 0; ch < channels; ch++) {
                if (st->codecpar->extradata && st->codecpar->extradata_size >= (ch + 1) * 32) {
                    avio_write(pack_buf, st->codecpar->extradata + ch * 32, 32);
                } else {
                    ffio_fill(pack_buf, 0, 32);
                }
            }

            // Write raw ADPCM samples
            if (raw_audio && raw_size > 0)
                avio_write(pack_buf, raw_audio, raw_size);

            t->pack_size = data_offset + raw_size;
        } else {
            // Raw passthrough
            if (raw_audio && raw_size > 0)
                avio_write(pack_buf, raw_audio, raw_size);
            t->pack_size = raw_size;
        }

        av_free(raw_audio);
    }

    uint8_t *pack_data = NULL;
    int pack_data_size = avio_close_dyn_buf(pack_buf, &pack_data);

    uint32_t toc_size = 4 + (4 * 12); // 4 chunks: PROP, BINF, TONE, PACK
    uint32_t prop_size = 4 + (n_tracks * 8);
    uint32_t binf_size = 4 + (n_tracks * 16);
    uint32_t tone_size = 4 + (n_tracks * 8);
    uint32_t pack_size = (uint32_t)pack_data_size;

    uint32_t prop_ofs = 12 + 8 + toc_size;
    uint32_t binf_ofs = prop_ofs + 8 + prop_size;
    uint32_t tone_ofs = binf_ofs + 8 + binf_size;
    uint32_t pack_ofs = tone_ofs + 8 + tone_size;
    uint32_t total_size = pack_ofs + 8 + pack_size;

    // Write Header
    avio_wl32(pb, NUS3_MAGIC);
    avio_wl32(pb, BANK_MAGIC);
    avio_wl32(pb, total_size - 8);

    // Write TOC chunk
    avio_wl32(pb, MKTAG('T','O','C',' '));
    avio_wl32(pb, toc_size);
    avio_wl32(pb, 4); // 4 chunks

    avio_wl32(pb, MKTAG('P','R','O','P'));
    avio_wl32(pb, prop_ofs);
    avio_wl32(pb, prop_size);

    avio_wl32(pb, MKTAG('B','I','N','F'));
    avio_wl32(pb, binf_ofs);
    avio_wl32(pb, binf_size);

    avio_wl32(pb, MKTAG('T','O','N','E'));
    avio_wl32(pb, tone_ofs);
    avio_wl32(pb, tone_size);

    avio_wl32(pb, MKTAG('P','A','C','K'));
    avio_wl32(pb, pack_ofs);
    avio_wl32(pb, pack_size);

    // Write PROP chunk
    avio_wl32(pb, MKTAG('P','R','O','P'));
    avio_wl32(pb, prop_size);
    avio_wl32(pb, n_tracks);
    for (uint32_t i = 0; i < n_tracks; i++) {
        avio_wl32(pb, i);
        avio_wl32(pb, 0);
    }

    // Write BINF chunk
    avio_wl32(pb, MKTAG('B','I','N','F'));
    avio_wl32(pb, binf_size);
    avio_wl32(pb, n_tracks);
    for (uint32_t i = 0; i < n_tracks; i++) {
        char name[16] = { 0 };
        AVDictionaryEntry *de = av_dict_get(s->streams[i]->metadata, "title", NULL, 0);
        if (de && de->value)
            snprintf(name, sizeof(name), "%s", de->value);
        else
            snprintf(name, sizeof(name), "track_%03u", i);
        uint8_t len = (uint8_t)strlen(name);
        avio_w8(pb, len);
        avio_write(pb, name, 15);
    }

    // Write TONE chunk
    avio_wl32(pb, MKTAG('T','O','N','E'));
    avio_wl32(pb, tone_size);
    avio_wl32(pb, n_tracks);
    for (uint32_t i = 0; i < n_tracks; i++) {
        avio_wl32(pb, ctx->tracks[i].pack_offset);
        avio_wl32(pb, ctx->tracks[i].pack_size);
    }

    // Write PACK chunk
    avio_wl32(pb, MKTAG('P','A','C','K'));
    avio_wl32(pb, pack_size);
    if (pack_data && pack_size > 0)
        avio_write(pb, pack_data, pack_size);

    av_free(pack_data);
    av_freep(&ctx->tracks);
    return 0;
}

const FFOutputFormat ff_nus3bank_muxer = {
    .p.name            = "nus3bank",
    .p.long_name       = NULL_IF_CONFIG_SMALL("Namco Universal Sound 3 (NUS3BANK / NUS3AUDIO)"),
    .p.extensions      = "nus3bank,nus3audio",
    .priv_data_size    = sizeof(NUS3BankMuxContext),
    .p.audio_codec     = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec     = AV_CODEC_ID_NONE,
    .p.subtitle_codec  = AV_CODEC_ID_NONE,
    .flags_internal    = FF_OFMT_FLAG_MAX_ONE_OF_EACH,
    .write_header      = nus3bank_write_header,
    .write_packet      = nus3bank_write_packet,
    .write_trailer     = nus3bank_write_trailer,
};
