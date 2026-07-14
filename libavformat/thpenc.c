/*
 * THP muxer
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
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
 * Writes the standard (vanilla-compatible) THP v0x11000 layout that stock
 * FFmpeg and real THP tools decode. In THP the audio is chunked to the video
 * frame rate: every frame carries a slice of the audio. The incoming
 * adpcm_thp packets do NOT line up with the video frame boundaries, so this
 * muxer buffers the encoded ADPCM as a continuous per-channel stream of
 * 8-byte / 14-sample blocks and re-slices it, giving each video frame its
 * share of blocks (a fractional accumulator keeps the audio clock in sync).
 * The DSP-ADPCM state is carried across frames by the decoder, so cutting the
 * stream at any block boundary is lossless.
 *
 * Each frame's audio block is
 *   [channelSize][numSamples]
 *   [per ch: 32-byte coef table][per ch: 4-byte history]
 *   [per ch: ADPCM data]
 * with the coefficients taken from the encoder's AV_PKT_DATA_NEW_EXTRADATA.
 */

#include "avformat.h"
#include "mux.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "avio_internal.h"

#define THP_MAX_CHANNELS 2
#define THP_BLOCK_BYTES  8   /* bytes per ADPCM block, per channel */
#define THP_BLOCK_SAMPLES 14 /* samples per ADPCM block */
#define THP_COMPACT_LIMIT (1 << 20)

typedef struct ThpVideoFrame {
    uint8_t *data;
    int      size;
} ThpVideoFrame;

typedef struct ThpMuxContext {
    int64_t max_buffer_size_pos;
    int64_t max_samples_pos;
    int64_t frame_count_pos;
    int64_t first_frame_size_pos;
    int64_t max_frame_size_pos;
    int64_t first_frame_pos;
    int64_t last_frame_pos;
    int64_t audio_nsamples_pos;

    uint32_t frame_count;
    uint32_t max_frame_size;
    uint32_t max_buffer_size;
    uint32_t max_samples;
    uint32_t total_samples;

    int64_t  movie_data_start;
    int64_t  last_frame_offset;
    uint32_t last_frame_size;

    /* Pending video frames (FIFO). */
    ThpVideoFrame *vq;
    int      vq_len;
    int      vq_cap;

    /* Continuous per-channel ADPCM block stream. */
    uint8_t *ach[THP_MAX_CHANNELS];
    int      ach_size[THP_MAX_CHANNELS]; /* valid bytes in ach[c] */
    int      acur;                       /* consumed byte offset (both channels) */
    int64_t  blocks_avail;               /* blocks appended (per channel) */
    int64_t  blocks_emitted;             /* blocks written out (per channel) */

    /* Running DSP-ADPCM decode history (last two output samples per channel).
     * Each THP audio frame must carry the yn1/yn2 that seed its own decode; the
     * console re-seeds every frame from these, so leaving them 0 clicks ~30x/s. */
    int      dec_s1[THP_MAX_CHANNELS];
    int      dec_s2[THP_MAX_CHANNELS];

    /* Fractional audio-clock accumulator (samples * fps_den, mod fps_num). */
    int64_t  samp_rem;

    int      has_audio;
    int      channels;
    int      sample_rate;
    int      fps_num;
    int      fps_den;
    uint8_t  coefs[THP_MAX_CHANNELS * 32];
    int      coefs_ready;
} ThpMuxContext;

static int thp_write_header(AVFormatContext *s)
{
    ThpMuxContext *thp = s->priv_data;
    AVIOContext *pb = s->pb;
    AVCodecParameters *vpar = NULL;
    AVCodecParameters *apar = NULL;
    AVRational fps = { 30, 1 };
    int compcount = 0;

    for (int i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (vpar) {
                av_log(s, AV_LOG_ERROR, "THP supports a single video stream\n");
                return AVERROR(EINVAL);
            }
            if (par->codec_id != AV_CODEC_ID_THP && par->codec_id != AV_CODEC_ID_MJPEG) {
                av_log(s, AV_LOG_ERROR, "Video codec must be THP or MJPEG\n");
                return AVERROR(EINVAL);
            }
            vpar = par;
            if (s->streams[i]->avg_frame_rate.num > 0 &&
                s->streams[i]->avg_frame_rate.den > 0)
                fps = s->streams[i]->avg_frame_rate;
            compcount++;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (apar) {
                av_log(s, AV_LOG_ERROR, "THP supports a single audio stream\n");
                return AVERROR(EINVAL);
            }
            if (par->codec_id != AV_CODEC_ID_ADPCM_THP) {
                av_log(s, AV_LOG_ERROR, "Audio codec must be adpcm_thp\n");
                return AVERROR(EINVAL);
            }
            if (par->ch_layout.nb_channels < 1 ||
                par->ch_layout.nb_channels > THP_MAX_CHANNELS) {
                av_log(s, AV_LOG_ERROR, "THP audio must be mono or stereo\n");
                return AVERROR(EINVAL);
            }
            apar = par;
            thp->has_audio   = 1;
            thp->channels    = par->ch_layout.nb_channels;
            thp->sample_rate = par->sample_rate;
            compcount++;
        } else {
            av_log(s, AV_LOG_ERROR, "Unsupported stream type for THP\n");
            return AVERROR(EINVAL);
        }
    }

    if (!vpar) {
        av_log(s, AV_LOG_ERROR, "A video stream is required\n");
        return AVERROR(EINVAL);
    }

    thp->fps_num = fps.num;
    thp->fps_den = fps.den;

    /* File header. */
    ffio_wfourcc(pb, "THP\0");
    avio_wb32(pb, 0x00011000); /* Version. */

    thp->max_buffer_size_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* Max buffer size. */
    thp->max_samples_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* Max audio samples per frame. */

    avio_wb32(pb, av_float2int(av_q2d(fps)));

    thp->frame_count_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* Frame count. */
    thp->first_frame_size_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* First frame size. */
    thp->max_frame_size_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* Max frame size. */

    avio_wb32(pb, 0x30);       /* Components offset. */
    avio_wb32(pb, 0);          /* offsetDataOffset (unused). */

    thp->first_frame_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* First frame offset. */
    thp->last_frame_pos = avio_tell(pb);
    avio_wb32(pb, 0);          /* Last frame offset. */

    /* Component block at 0x30. */
    avio_wb32(pb, compcount);
    avio_w8(pb, 0);            /* Video component. */
    if (thp->has_audio)
        avio_w8(pb, 1);        /* Audio component. */
    ffio_fill(pb, 0xFF, thp->has_audio ? 14 : 15);

    /* Video component properties. */
    avio_wb32(pb, vpar->width);
    avio_wb32(pb, vpar->height);
    avio_wb32(pb, 0);          /* Unknown (required for version 0x11000). */

    /* Audio component properties (no coef table; coefs are per-frame). */
    if (thp->has_audio) {
        avio_wb32(pb, apar->ch_layout.nb_channels);
        avio_wb32(pb, apar->sample_rate);
        thp->audio_nsamples_pos = avio_tell(pb);
        avio_wb32(pb, 0);      /* Total audio samples (filled in trailer). */
        avio_wb32(pb, 1);      /* flag. */
    }

    /* Frame data must start 32-byte aligned (console DMA); pad the header out.
     * With audio the block already ends at 0x60; video-only ends at 0x50. */
    ffio_fill(pb, 0, (int)((-avio_tell(pb)) & 31));

    /* Frame data begins here; remember it so the trailer can fill in dataSize. */
    thp->movie_data_start = avio_tell(pb);

    return 0;
}

/* Decode nblocks of DSP-ADPCM for channel c (8 bytes / 14 samples per block)
 * to advance the running yn1/yn2 history, using the same arithmetic as the
 * THP decoder.  Only the two trailing samples are kept; no PCM is emitted. */
static void thp_adpcm_advance(ThpMuxContext *thp, int c, const uint8_t *blk, int nblocks)
{
    const uint8_t *coefs = thp->coefs + c * 32;
    int s1 = thp->dec_s1[c], s2 = thp->dec_s2[c];

    for (int b = 0; b < nblocks; b++) {
        const uint8_t *p = blk + b * THP_BLOCK_BYTES;
        int header = p[0];
        int index  = (header >> 4) & 7;
        int expo   = header & 0x0F;
        int f1 = (int16_t)AV_RB16(coefs + index * 4);
        int f2 = (int16_t)AV_RB16(coefs + index * 4 + 2);

        for (int n = 0; n < THP_BLOCK_SAMPLES; n++) {
            int byte = p[1 + n / 2];
            int raw  = (n & 1) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            int nib  = raw >= 8 ? raw - 16 : raw;   /* sign-extend 4-bit */
            int pred = ((s1 * f1 + s2 * f2) >> 11) + nib * (1 << expo);
            pred = av_clip_int16(pred);
            s2 = s1;
            s1 = pred;
        }
    }
    thp->dec_s1[c] = s1;
    thp->dec_s2[c] = s2;
}

/* Write one THP frame: the head video frame plus nblocks of audio (0 blocks
 * allowed only for a video-only file). */
static int thp_emit_frame(AVFormatContext *s, ThpVideoFrame *vf, int nblocks)
{
    ThpMuxContext *thp = s->priv_data;
    AVIOContext *pb = s->pb;
    int ch = thp->channels;
    int64_t current_offset = avio_tell(pb);
    uint32_t audio_size = 0, frame_size, frame_pad, channel_size = 0, num_samples = 0;

    if (thp->has_audio) {
        channel_size = (uint32_t)nblocks * THP_BLOCK_BYTES;
        num_samples  = (uint32_t)nblocks * THP_BLOCK_SAMPLES;
        audio_size   = 8 + 36 * ch + channel_size * ch;
    }

    frame_size = 12 + (thp->has_audio ? 4 : 0) + vf->size + audio_size;
    /* Whole frames are padded to a 32-byte boundary: the console DMAs frame
     * data and requires 32-byte alignment, so retail THP frame totals (and the
     * first-frame offset) are always multiples of 32.  frame_size is the padded
     * size that goes in the next/prev/first-frame-size fields and by which the
     * demuxer advances; the per-component size fields stay the real byte counts. */
    frame_pad  = (uint32_t)(-(int32_t)frame_size) & 31u;
    frame_size += frame_pad;

    if (thp->last_frame_offset > 0) {
        int64_t saved = avio_tell(pb);
        avio_seek(pb, thp->last_frame_offset, SEEK_SET);
        avio_wb32(pb, frame_size);
        avio_seek(pb, saved, SEEK_SET);
    } else {
        int64_t saved = avio_tell(pb);
        avio_seek(pb, thp->first_frame_pos, SEEK_SET);
        avio_wb32(pb, (uint32_t)current_offset);
        avio_seek(pb, thp->first_frame_size_pos, SEEK_SET);
        avio_wb32(pb, frame_size);
        avio_seek(pb, saved, SEEK_SET);
    }

    /* Frame header. */
    avio_wb32(pb, 0);                    /* Next frame size (patched later). */
    avio_wb32(pb, thp->last_frame_size); /* Previous frame size. */
    avio_wb32(pb, vf->size);
    if (thp->has_audio)
        avio_wb32(pb, audio_size);

    /* Video payload. */
    avio_write(pb, vf->data, vf->size);

    /* Audio payload. */
    if (thp->has_audio) {
        int off = thp->acur;
        avio_wb32(pb, channel_size);
        avio_wb32(pb, num_samples);
        for (int c = 0; c < ch; c++)
            avio_write(pb, thp->coefs + c * 32, 32);
        for (int c = 0; c < ch; c++) {
            avio_wb16(pb, thp->dec_s1[c]); /* history sample1 (yn1). */
            avio_wb16(pb, thp->dec_s2[c]); /* history sample2 (yn2). */
        }
        for (int c = 0; c < ch; c++) {
            avio_write(pb, thp->ach[c] + off, channel_size);
            /* Advance the running history past this frame for the next one. */
            thp_adpcm_advance(thp, c, thp->ach[c] + off, nblocks);
        }

        thp->acur           += channel_size;
        thp->blocks_emitted += nblocks;
        if (num_samples > thp->max_samples)
            thp->max_samples = num_samples;
        thp->total_samples += num_samples;

        /* Reclaim consumed bytes so the buffer stays bounded. */
        if (thp->acur >= THP_COMPACT_LIMIT) {
            for (int c = 0; c < ch; c++) {
                memmove(thp->ach[c], thp->ach[c] + thp->acur,
                        thp->ach_size[c] - thp->acur);
                thp->ach_size[c] -= thp->acur;
            }
            thp->acur = 0;
        }
    }

    /* Pad the whole frame up to its 32-byte-aligned size. */
    ffio_fill(pb, 0, frame_pad);

    if (frame_size > thp->max_frame_size)
        thp->max_frame_size = frame_size;
    if ((uint32_t)vf->size > thp->max_buffer_size)
        thp->max_buffer_size = vf->size;

    thp->last_frame_offset = current_offset;
    thp->last_frame_size   = frame_size;
    thp->frame_count++;

    av_freep(&vf->data);
    vf->size = 0;
    return 0;
}

/* Number of audio blocks the next frame should carry to stay on the audio
 * clock (sample_rate / fps), rounded to whole 14-sample blocks, min 1. */
static int thp_blocks_for_next_frame(ThpMuxContext *thp)
{
    int64_t samples;
    int blocks;

    if (!thp->has_audio)
        return 0;

    /* samples this frame = sample_rate * fps_den / fps_num, with carry. */
    thp->samp_rem += (int64_t)thp->sample_rate * thp->fps_den;
    samples        = thp->samp_rem / thp->fps_num;
    thp->samp_rem -= samples * thp->fps_num;

    blocks = (int)((samples + THP_BLOCK_SAMPLES / 2) / THP_BLOCK_SAMPLES);
    if (blocks < 1)
        blocks = 1;
    return blocks;
}

/* Emit frames while a buffered video frame has its share of audio available. */
static int thp_drain(AVFormatContext *s, int flushing)
{
    ThpMuxContext *thp = s->priv_data;
    int ret;

    while (thp->vq_len > 0) {
        int64_t avail     = thp->blocks_avail - thp->blocks_emitted;
        int64_t saved_rem = thp->samp_rem;
        int nblocks       = thp->has_audio ? thp_blocks_for_next_frame(thp) : 0;

        if (thp->has_audio && !flushing && avail < nblocks) {
            /* Not enough audio yet; restore the accumulator and wait. */
            thp->samp_rem = saved_rem;
            return 0;
        }

        if (thp->has_audio) {
            if (nblocks > avail)
                nblocks = (int)avail;      /* trailer: whatever is left */
            if (flushing && thp->vq_len == 1)
                nblocks = (int)avail;      /* last frame takes the remainder */
        }

        ret = thp_emit_frame(s, &thp->vq[0], nblocks);
        if (ret < 0)
            return ret;

        memmove(thp->vq, thp->vq + 1, (thp->vq_len - 1) * sizeof(*thp->vq));
        thp->vq_len--;
    }

    return 0;
}

/* Rewrite ffmpeg's standard JPEG frame into the "quasi-JPEG" a THP carries:
 *   - drop the APP0/JFIF and comment (COM) markers (retail frames are bare, SOI
 *     straight into the DQT/SOF/DHT/SOS tables);
 *   - un-escape the entropy-coded scan.  A conforming JPEG byte-stuffs every
 *     0xFF in the scan as 0xFF 0x00; THP stores the raw 0xFF with no stuffing,
 *     and the console's decoder treats a stuffing 0x00 as real coded data, so
 *     leaving them in desyncs the entropy decode and garbles the whole frame.
 * Marker segments carry a 2-byte length; the scan runs from SOS to the end.
 * Returns the number of bytes written to out (<= size). */
static int thp_strip_jpeg(uint8_t *out, const uint8_t *in, int size)
{
    int i, o = 0;

    if (size < 2 || in[0] != 0xFF || in[1] != 0xD8) {   /* not JPEG: copy as-is */
        memcpy(out, in, size);
        return size;
    }
    out[o++] = 0xFF; out[o++] = 0xD8;                    /* SOI */
    i = 2;
    while (i + 1 < size) {
        int m;
        if (in[i] != 0xFF) { out[o++] = in[i++]; continue; }
        m = in[i + 1];
        if (m == 0xDA) {                                 /* SOS: copy header, then scan */
            int len = (in[i + 2] << 8) | in[i + 3];
            memcpy(out + o, in + i, 2 + len);            /* SOS marker + scan params */
            o += 2 + len; i += 2 + len;
            /* Un-stuff the entropy-coded scan: 0xFF 0x00 -> 0xFF, but keep a
             * 0xFF followed by a real marker (EOI/RSTn) intact. */
            while (i < size) {
                out[o++] = in[i];
                if (in[i] == 0xFF && i + 1 < size && in[i + 1] == 0x00)
                    i += 2;                              /* drop the stuffing byte */
                else
                    i += 1;
            }
            return o;
        }
        if (m == 0xD9) {                                 /* EOI before any scan: rest as-is */
            memcpy(out + o, in + i, size - i);
            o += size - i;
            return o;
        }
        if (i + 3 >= size) break;
        int len = (in[i + 2] << 8) | in[i + 3];          /* incl. the 2 length bytes */
        if (m == 0xE0 || m == 0xFE) {                    /* APP0/JFIF or COM: drop */
            i += 2 + len;
        } else {
            memcpy(out + o, in + i, 2 + len);
            o += 2 + len; i += 2 + len;
        }
    }
    memcpy(out + o, in + i, size - i);
    return o + (size - i);
}

static int thp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ThpMuxContext *thp = s->priv_data;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        ThpVideoFrame vf;
        vf.data = av_malloc(pkt->size);
        if (!vf.data)
            return AVERROR(ENOMEM);
        vf.size = thp_strip_jpeg(vf.data, pkt->data, pkt->size);

        if (thp->vq_len >= thp->vq_cap) {
            int ncap = thp->vq_cap ? thp->vq_cap * 2 : 16;
            ThpVideoFrame *nq = av_realloc_array(thp->vq, ncap, sizeof(*nq));
            if (!nq) {
                av_free(vf.data);
                return AVERROR(ENOMEM);
            }
            thp->vq     = nq;
            thp->vq_cap = ncap;
        }
        thp->vq[thp->vq_len++] = vf;
    } else {
        int ch  = thp->channels;
        int csz = pkt->size / ch;

        if (pkt->size % ch || csz % THP_BLOCK_BYTES) {
            av_log(s, AV_LOG_ERROR, "Malformed THP audio packet\n");
            return AVERROR_INVALIDDATA;
        }

        if (!thp->coefs_ready) {
            size_t side_size;
            const uint8_t *side = av_packet_get_side_data(pkt,
                                      AV_PKT_DATA_NEW_EXTRADATA, &side_size);
            if (side && side_size >= (size_t)(32 * ch))
                memcpy(thp->coefs, side, 32 * ch);
            thp->coefs_ready = 1;
        }

        for (int c = 0; c < ch; c++) {
            uint8_t *nb = av_realloc(thp->ach[c], thp->ach_size[c] + csz);
            if (!nb)
                return AVERROR(ENOMEM);
            memcpy(nb + thp->ach_size[c], pkt->data + c * csz, csz);
            thp->ach[c]       = nb;
            thp->ach_size[c] += csz;
        }
        thp->blocks_avail += csz / THP_BLOCK_BYTES;
    }

    return thp_drain(s, 0);
}

static int thp_write_trailer(AVFormatContext *s)
{
    ThpMuxContext *thp = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t file_end;
    int ret;

    ret = thp_drain(s, 1);
    if (ret < 0)
        return ret;

    file_end = avio_tell(pb);

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        /* 0x08 "max buffer size" is the largest *whole* frame (header + video
         * + audio); the player allocates its per-frame read buffer from it, so
         * it must not undercount or the buffer overflows and the title crashes. */
        avio_seek(pb, thp->max_buffer_size_pos, SEEK_SET);
        avio_wb32(pb, thp->max_frame_size);
        avio_seek(pb, thp->max_samples_pos, SEEK_SET);
        avio_wb32(pb, thp->max_samples);
        avio_seek(pb, thp->frame_count_pos, SEEK_SET);
        avio_wb32(pb, thp->frame_count);
        /* 0x1C is dataSize: total bytes of frame data (whole file minus the
         * fixed header/component block), not a per-frame maximum. */
        avio_seek(pb, thp->max_frame_size_pos, SEEK_SET);
        avio_wb32(pb, (uint32_t)(file_end - thp->movie_data_start));
        avio_seek(pb, thp->last_frame_pos, SEEK_SET);
        avio_wb32(pb, (uint32_t)thp->last_frame_offset);
        if (thp->has_audio) {
            avio_seek(pb, thp->audio_nsamples_pos, SEEK_SET);
            avio_wb32(pb, thp->total_samples);
        }
        avio_seek(pb, file_end, SEEK_SET);
    }

    return 0;
}

static void thp_deinit(AVFormatContext *s)
{
    ThpMuxContext *thp = s->priv_data;
    for (int i = 0; i < thp->vq_len; i++)
        av_freep(&thp->vq[i].data);
    av_freep(&thp->vq);
    for (int c = 0; c < THP_MAX_CHANNELS; c++)
        av_freep(&thp->ach[c]);
}

const FFOutputFormat ff_thp_muxer = {
    .p.name         = "thp",
    .p.long_name    = NULL_IF_CONFIG_SMALL("THP"),
    .p.extensions   = "thp",
    .priv_data_size = sizeof(ThpMuxContext),
    .p.audio_codec  = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec  = AV_CODEC_ID_MJPEG,
    .write_header   = thp_write_header,
    .write_packet   = thp_write_packet,
    .write_trailer  = thp_write_trailer,
    .deinit         = thp_deinit,
    .p.flags        = AVFMT_NOTIMESTAMPS,
};
