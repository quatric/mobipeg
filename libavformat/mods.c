/*
 * MODS demuxer
 * Copyright (c) 2015-2016 Florian Nouwt
 * Copyright (c) 2017 Adib Surani
 * Copyright (c) 2020 Paul B Mahol
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

#include <stdlib.h>

#include "libavutil/intreadwrite.h"

#define SX_CODEBOOK_SIZE 0xC34

#include "libavutil/mem.h"
#include "libavcodec/avcodec.h"
#include "demux.h"

#include "avformat.h"
#include "internal.h"

/* Consumed video bytes, set by the mobiclip video decoder (mobiclip.c). The
 * demuxer runs its OWN private mobiclip decode of each frame to learn where the
 * SX audio tail begins, so the split is deterministic and independent of the
 * main transcode's (threaded, racy) stream decoders. */

#define SX_CODEBOOK_SIZE 0xC34

#define MODS_AUDIO_FLAG 0x2000
#define MODS_EMBED_SENTINEL 0x41454D4F  /* 'OMEA' -- see modsenc.c */

typedef struct MODSDemuxContext {
    int64_t frame_index;
    int64_t audio_pts;
    int has_audio;
    int audio_channels;
    int embedded_audio;     /* our in-frame [video][audio][u16 size] layout */
    int audio_codec_id;     /* cached for duration/block math */
    int audio_block;        /* bytes per encoded audio block (decoder wants 1/pkt) */
    /* Audio region split off the current chunk, drained one block per packet. */
    uint8_t *aud_buf;
    int      aud_size;
    int      aud_off;
    int64_t  frame_data_end;  /* frames live in [first_frame, frame_data_end) */
    int64_t  kf_table_off;
    int      nb_video_frames;

    /* Retail DS .mods SX audio. The demuxer splits each frame into video/audio
     * using its own private mobiclip decoder (m->vdec), then emits the audio
     * blocks as a self-contained packet: [u32 audio_blocks][audio bytes]. */
    AVCodecContext *vdec;
    AVFrame        *vframe;
    AVPacket       *vpkt;
    int      has_sx_audio;    /* retail SX stream is present */
    int      sx_width, sx_height;
    int      sx_emit_blocks;  /* >0: aud_buf holds a pending SX audio packet */
} MODSDemuxContext;


static int mods_probe(const AVProbeData *p)
{
    if (memcmp(p->buf, "MODSN3\x0a\x00", 8))
        return 0;
    if (AV_RB32(p->buf + 8) == 0)
        return 0;
    if (AV_RB32(p->buf + 12) == 0)
        return 0;
    if (AV_RB32(p->buf + 16) == 0)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int mods_read_header(AVFormatContext *s)
{
    MODSDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    AVRational fps;
    int64_t pos;

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    avio_skip(pb, 8);   /* "MODS" + format id "N3" + video codec u16 (=0x0A) */

    st->nb_frames            = avio_rl32(pb);  /* 0x08 */
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_MOBICLIP;
    st->codecpar->width      = avio_rl32(pb);  /* 0x0C */
    st->codecpar->height     = avio_rl32(pb);  /* 0x10 */
    m->nb_video_frames       = st->nb_frames;

    fps.num = avio_rl32(pb);                    /* 0x14 */
    fps.den = 0x1000000;
    avpriv_set_pts_info(st, 64, fps.den, fps.num);

    /* Header audio descriptor (layout from pleonex/PlayMobic Binary2Mods):
     *   0x18 u16 audio codec   0x1A u16 channels   0x1C u32 sample_rate
     *   0x20 u32 large-frame-idx (our muxer reuses this slot as an embed
     *        sentinel)          0x24 u32 audio-codec-info offset (also the
     *        end of the interleaved video+audio frame data)
     *   0x28 u32 key-frame table offset   0x2C u32 key-frame count
     * Audio codec ids: 1=FastAudio(codebook), 2=FastAudio(enhanced),
     *   3=IMA-ADPCM, 4=PCM16, 0=none. */
    {
        int acodec               = avio_rl16(pb);           /* 0x18: u16 codec type */
        int ach                  = avio_rl16(pb);           /* 0x1A: u16 channel count */
        int arate                = avio_rl32(pb);           /* 0x1C */
        avio_skip(pb, 4);                          /* 0x20 */
        uint32_t acodec_info_off = avio_rl32(pb);          /* 0x24 */
        m->kf_table_off          = avio_rl32(pb);          /* 0x28 */
        /* (key-frame count at 0x2C is not needed here) */

        if (acodec > 1 && acodec != 0xFF)
            m->embedded_audio = 1;

        /* Only use acodec_info_off as an end-of-frames guard when it's
         * plausibly AFTER the keyframe table (i.e. after the header area).
         * kf_table_off is unreliable for retail files where it points into
         * the header itself rather than past all frame data. */
        m->frame_data_end = (acodec_info_off > m->kf_table_off)
                          ? (int64_t)acodec_info_off : 0;

        if (m->embedded_audio && acodec != 0xFF) {
            /* Our own muxer's in-frame audio (see modsenc.c): one block per
             * packet, re-split in mods_read_packet. Codec ids here follow our
             * encoder's mapping, not the retail one. */
            AVStream *ast = avformat_new_stream(s, NULL);
            if (!ast)
                return AVERROR(ENOMEM);
            ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            ast->codecpar->sample_rate = arate;
            ast->codecpar->ch_layout.nb_channels = ach ? ach : 2;
            switch (acodec) {
            case 2:  ast->codecpar->codec_id = AV_CODEC_ID_FASTAUDIO;         break;
            case 3:  ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_NDS;     break;
            case 4:  ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;         break;
            default: ast->codecpar->codec_id = AV_CODEC_ID_NONE;              break;
            }
            avpriv_set_pts_info(ast, 64, 1, arate ? arate : 32000);
            m->has_audio = 1;
            m->audio_channels = ast->codecpar->ch_layout.nb_channels;
            m->audio_codec_id = ast->codecpar->codec_id;
            if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS)
                m->audio_block = m->audio_channels * 128;   /* 256 samples/block, planar nibbles */
            else if (m->audio_codec_id == AV_CODEC_ID_FASTAUDIO) {
                m->audio_block = m->audio_channels * 40;     /* 256 samples/block */
            }
            else if (m->audio_codec_id == AV_CODEC_ID_PCM_S16LE)
                m->audio_block = m->audio_channels * 512;    /* 256 samples/block, s16 */
            else
                m->audio_block = 0;
        } else if (acodec == 1 && ach >= 1 && ach <= 2 && arate > 0 &&
                   acodec_info_off > 0 && (pb->seekable & AVIO_SEEKABLE_NORMAL)) {
            /* DS .mods FastAudio-codebook (SX) audio — the same codec as the
             * ActImagine VX audio codec (vx_audio; verified bit-exact
             * against Nintendo's SxCodec blob). The per-channel 3124-byte
             * codebooks live at the audio-codec-info offset; hand them to the
             * decoder as extradata with a w=h=0 prefix (packets carry no
             * leading video bitstream). The video/audio chunk split uses the
             * 4-byte suffix our muxer writes; retail files (VBR blocks, no
             * suffix) still decode video-only. */
            AVStream *ast = avformat_new_stream(s, NULL);
            int64_t cur;
            if (!ast)
                return AVERROR(ENOMEM);
            ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
            ast->codecpar->codec_id    = AV_CODEC_ID_VX_AUDIO;
            ast->codecpar->sample_rate = arate;
            ast->codecpar->ch_layout.nb_channels = ach;
            ast->codecpar->extradata = av_mallocz(16 + ach * SX_CODEBOOK_SIZE +
                                                  AV_INPUT_BUFFER_PADDING_SIZE);
            if (!ast->codecpar->extradata)
                return AVERROR(ENOMEM);
            ast->codecpar->extradata_size = 16 + ach * SX_CODEBOOK_SIZE;
            AV_WL32(ast->codecpar->extradata + 12, ach);   /* q/w/h stay 0 */
            cur = avio_tell(pb);
            avio_seek(pb, acodec_info_off, SEEK_SET);
            if (avio_read(pb, ast->codecpar->extradata + 16,
                          ach * SX_CODEBOOK_SIZE) != ach * SX_CODEBOOK_SIZE) {
                av_log(s, AV_LOG_WARNING, "MODS: SX codebook truncated; video only.\n");
                ast->codecpar->codec_id = AV_CODEC_ID_NONE;
            } else {
                avpriv_set_pts_info(ast, 64, 1, arate);
                m->has_sx_audio   = 1;
                m->has_audio      = 1;
                m->audio_channels = ach;
                m->audio_codec_id = AV_CODEC_ID_VX_AUDIO;
            }
            avio_seek(pb, cur, SEEK_SET);
        } else if (acodec != 0 && acodec != 0xFF) {
            av_log(s, AV_LOG_WARNING,
                   "MODS: retail in-frame audio (codec %d) not supported; "
                   "decoding video only.\n", acodec);
        }
    }

    /* No private decoder needed anymore, audio_start is computed via math. */
    /* Open a private mobiclip decoder used solely to find the video/audio split
     * for retail SX frames and stateful embedded audio (FastAudio/ADPCM). */
    int needs_vdec = m->has_sx_audio ||
                     (m->embedded_audio && m->has_audio &&
                      m->audio_codec_id != AV_CODEC_ID_PCM_S16LE);
    if (needs_vdec) {
        const AVCodec *vc = avcodec_find_decoder(AV_CODEC_ID_MOBICLIP);
        if (vc) {
            m->vdec = avcodec_alloc_context3(vc);
            m->vframe = av_frame_alloc();
            m->vpkt = av_packet_alloc();
            if (!m->vdec || !m->vframe || !m->vpkt) {
                av_log(s, AV_LOG_WARNING, "MODS: split decoder alloc failed\n");
                m->has_sx_audio = 0;
            } else {
                m->vdec->width  = s->streams[0]->codecpar->width;
                m->vdec->height = s->streams[0]->codecpar->height;
                m->vdec->thread_count = 1;
                if (avcodec_open2(m->vdec, vc, NULL) < 0) {
                    av_log(s, AV_LOG_WARNING, "MODS: split decoder open failed\n");
                    m->has_sx_audio = 0;
                }
            }
        }
    }

    /* First frame = first key-frame table entry's data offset (the entry is
     * [u32 frame_number][u32 data_offset]; +4 skips the frame number). */
    avio_seek(pb, m->kf_table_off + 4, SEEK_SET);
    pos = avio_rl32(pb);
    avio_seek(pb, pos, SEEK_SET);

    {
        int64_t header_end_pos = avio_tell(pb);
        uint32_t size_and_flags = avio_rl32(pb);
        uint32_t size = size_and_flags >> 14;
        if (size >= 4) {
            uint8_t peek[4] = {0};
            int read_bytes = avio_read(pb, peek, 4);
            if (read_bytes == 4) {
                int is_h264 = 0;
                if (peek[0] == 0 && peek[1] == 0) {
                    if (peek[2] == 1) {
                        is_h264 = 1;
                    } else if (peek[2] == 0 && peek[3] == 1) {
                        is_h264 = 1;
                    } else {
                        uint32_t nal_len = ((uint32_t)peek[0] << 24) |
                                           ((uint32_t)peek[1] << 16) |
                                           ((uint32_t)peek[2] << 8)  |
                                           peek[3];
                        if (nal_len + 4 == size) {
                            is_h264 = 1;
                        }
                    }
                }
                /* Our own encoder uses Annex-B start codes with Mobiclip VLC
                 * tables, NOT standard H.264. Retail Nintendo Channel .mods
                 * files actually contain real H.264. Distinguish by the embedded-
                 * audio sentinel: our files set m->embedded_audio, retail don't. */
                if (is_h264 && !m->embedded_audio) {
                    s->streams[0]->codecpar->codec_id = AV_CODEC_ID_H264;
                }
            }
        }
        avio_seek(pb, header_end_pos, SEEK_SET);
    }

    return 0;
}

static int mods_audio_duration(MODSDemuxContext *m, int size)
{
    int ch = m->audio_channels ? m->audio_channels : 1;
    if (m->audio_codec_id == AV_CODEC_ID_PCM_S16LE)
        return size / (ch * 2);
    if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS) {
        int hdr = (size % (128 * ch)) == 0 ? 0 : 4 + 4 * ch;
        return ((size - hdr) / (ch * 128)) * 256;
    }
    /* fastaudio: 256 samples per 40-byte/ch frame */
    return (size / (40 * ch)) * 256;
}

/* Decode one video chunk through the private mobiclip decoder to find where the
 * video bitstream ends and the SX audio tail begins.  Returns the byte offset
 * (== mobiclip_consumed_bytes), or -1 on failure.  Used for SX files that lack
 * our explicit 4-byte split suffix: older muxer output and retail DS .mods.
 * Frames must be fed in order (P-frames reference earlier ones), which the
 * linear demux read satisfies. */
static int mods_sx_video_split(MODSDemuxContext *m, const uint8_t *data, int size)
{
    AVDictionaryEntry *e;
    int consumed = -1, ret;

    if (!m->vdec || !m->vpkt || !m->vframe)
        return -1;
    av_packet_unref(m->vpkt);
    if (av_new_packet(m->vpkt, size) < 0)
        return -1;
    memcpy(m->vpkt->data, data, size);
    ret = avcodec_send_packet(m->vdec, m->vpkt);
    if (ret < 0)
        return -1;
    ret = avcodec_receive_frame(m->vdec, m->vframe);
    if (ret < 0)
        return -1;
    e = av_dict_get(m->vframe->metadata, "mobiclip_consumed_bytes", NULL, 0);
    if (e)
        consumed = atoi(e->value);
    av_frame_unref(m->vframe);
    return (consumed >= 0 && consumed <= size) ? consumed : -1;
}

/* Emit the next audio block buffered from the current chunk, if any. */
static int mods_emit_audio_block(MODSDemuxContext *m, AVPacket *pkt)
{
    int remaining = m->aud_size - m->aud_off;
    int blk;
    if (remaining <= 0)
        return AVERROR(EAGAIN);
    if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS)
        blk = remaining;
    else if (m->audio_block > 0) {
        /* For stateful codecs (FastAudio), the encoder may append a kf_word
         * (4 zero bytes) after the real audio data.  Skip trailing partial
         * blocks so we never feed a sub-block-sized packet to the decoder. */
        if (remaining < m->audio_block) {
            m->aud_off = m->aud_size; /* discard trailing partial block */
            return AVERROR(EAGAIN);
        }
        blk = m->audio_block;
    } else
        blk = remaining;
    if (av_new_packet(pkt, blk) < 0)
        return AVERROR(ENOMEM);
    memcpy(pkt->data, m->aud_buf + m->aud_off, blk);
    /* PCM data in the file is per-channel planar blocks (ch0 512B, ch1 512B,
     * ...).  Deinterleave to the interleaved format FFmpeg's PCM decoder
     * expects.  Mono needs no conversion. */
    if (m->audio_codec_id == AV_CODEC_ID_PCM_S16LE && m->audio_channels > 1) {
        int ch = m->audio_channels;
        int nsamp_per_ch = blk / (ch * (int)sizeof(int16_t));
        int16_t *data = (int16_t *)pkt->data;
        int16_t *tmp = av_malloc(blk);
        if (tmp) {
            memcpy(tmp, data, blk);
            for (int s = 0; s < nsamp_per_ch; s++)
                for (int c = 0; c < ch; c++)
                    data[s * ch + c] = tmp[c * nsamp_per_ch + s];
            av_free(tmp);
        }
    }
    m->aud_off += blk;
    pkt->stream_index = 1;
    pkt->pts = pkt->dts = m->audio_pts;
    pkt->flags |= AV_PKT_FLAG_KEY;
    pkt->duration = mods_audio_duration(m, blk);
    m->audio_pts += pkt->duration;
    return 0;
}

static int mods_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MODSDemuxContext *m = s->priv_data;
    AVIOContext *pb = s->pb;
    unsigned word, size, flags;
    int64_t pos;
    int ret;

    /* Drain audio blocks split from the previous chunk before reading more.
     * (SX has its own single-packet emit path below and its own use of aud_buf.) */
    if (!m->has_sx_audio && mods_emit_audio_block(m, pkt) == 0)
        return 0;

    /* Retail SX audio: emit the audio packet split from the previous frame.
     * Layout: [u32 audio_blocks][audio bytes] (audio starts at offset 4). */
    if (m->sx_emit_blocks > 0) {
        ret = av_new_packet(pkt, m->aud_size);
        if (ret < 0) {
            m->sx_emit_blocks = 0;
            return ret;
        }
        memcpy(pkt->data, m->aud_buf, m->aud_size);
        pkt->stream_index = 1;
        pkt->pts = pkt->dts = m->audio_pts;
        pkt->flags |= AV_PKT_FLAG_KEY;
        pkt->duration = m->sx_emit_blocks * 128;  /* 128 samples per SX block */
        m->audio_pts += pkt->duration;
        m->sx_emit_blocks = 0;
        return 0;
    }

    if (avio_feof(pb))
        return AVERROR_EOF;

    /* Stop at the end of the interleaved frame-data region. Past it lies the
     * audio-codec-info / key-frame tables, which are not chunk-framed - reading
     * them as chunks yields a bogus packet (this is what made retail files emit
     * a corrupt "audio" packet and abort the whole transcode). */
    if (m->frame_data_end > 0 && avio_tell(pb) >= m->frame_data_end)
        return AVERROR_EOF;
    if (m->nb_video_frames > 0 && m->frame_index >= m->nb_video_frames)
        return AVERROR_EOF;

    pos = avio_tell(pb);
    word = avio_rl32(pb);
    size = word >> 14;
    flags = word & 0x3FFF;
    ret = av_get_packet(pb, pkt, size);
    if (ret < 0)
        return ret;
    pkt->pos = pos;

    if (m->has_audio && (flags & MODS_AUDIO_FLAG)) {
        /* Legacy separate audio chunk (older muxer output). */
        pkt->stream_index = 1;
        pkt->pts = pkt->dts = m->audio_pts;
        pkt->flags |= AV_PKT_FLAG_KEY;
        pkt->duration = mods_audio_duration(m, size);
        m->audio_pts += pkt->duration;
        return ret;
    }

    /* Embedded-audio chunk. Buffer the audio region (drained one block per
     * packet on subsequent calls) and shrink the video packet. */
    if (m->embedded_audio && m->has_audio && size >= 4) {
        int audio_blocks = (int)(flags & 0x3FFF);
        int audio_size  = audio_blocks * m->audio_block;
        int audio_start = (int)size - audio_size;

        if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS ||
            m->audio_codec_id == AV_CODEC_ID_FASTAUDIO) {
            /* Find the video/audio split by decoding the video through the
             * private mobiclip decoder (mods_sx_video_split -> exact vfield).
             * The audio size then follows deterministically from the block
             * count in the chunk word, so this is preferred for BOTH our own
             * output and retail DS files. We do NOT trust the [pad][0][asize]
             * suffix as a primary source: retail files carry no suffix, and a
             * retail chunk's last 4 bytes occasionally look like a valid one
             * (bogus audio_size, e.g. 293 -> ADPCM step_index blowup). The
             * suffix and the RBSP-stop-bit heuristic are last resorts for when
             * the private decoder is unavailable. */
            int is_key = (size >= 2 && (pkt->data[1] & 0x80));
            int vfield = mods_sx_video_split(m, pkt->data, size);
            if (vfield >= 0 && vfield <= (int)size) {
                if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS) {
                    /* On keyframes a 4-byte re-prime word precedes the audio,
                     * and the first block of each channel carries a 4-byte IMA
                     * init header (all part of the packet the decoder consumes;
                     * headers are interleaved per block, ffmpeg's adpcm_ima_nds
                     * layout). */
                    audio_start = vfield;
                    audio_size  = audio_blocks * m->audio_channels * 128;
                    if (is_key && audio_blocks > 0)
                        audio_size += 4 + 4 * m->audio_channels;
                } else { /* FASTAUDIO: 4-byte keyframe word precedes blocks */
                    int kf_word = (is_key && audio_blocks > 0) ? 4 : 0;
                    audio_start = vfield + kf_word;
                    audio_size  = audio_blocks * m->audio_channels * 40;
                }
            } else {
                int stored_asize = (size >= 4) ? (int)AV_RL16(pkt->data + size - 2) : 0;
                int spad = (size >= 4) ? pkt->data[size - 4] : 0;
                if (stored_asize > 0 && stored_asize <= size && spad <= 3 && size >= 4) {
                    /* Our muxer's explicit split suffix: [pad][0x00][asize u16le]. */
                    audio_size  = stored_asize;
                    audio_start = size - 4 - spad - stored_asize;
                    if (m->audio_codec_id == AV_CODEC_ID_FASTAUDIO && audio_size >= 4 && is_key) {
                        audio_start += 4;   /* skip the keyframe word */
                        audio_size  -= 4;
                    }
                } else if (m->audio_codec_id == AV_CODEC_ID_ADPCM_IMA_NDS) {
                    audio_size = audio_blocks * m->audio_channels * 128;
                    if (is_key && audio_blocks > 0)
                        audio_size += 4 + 4 * m->audio_channels;
                    audio_start = (int)size - audio_size;
                } else {
                    /* FastAudio heuristic fallback: kf_word is 4 bytes before
                     * the blocks. */
                    int kf_word = (is_key && audio_blocks > 0) ? 4 : 0;
                    audio_size = audio_blocks * m->audio_channels * 40;
                    int S = (int)size - audio_size - kf_word;
                    int e = S, mb_bits = 0;
                    while (e > 0 && pkt->data[e - 1] == 0) e--;
                    if (e > 0) {
                        int lb = pkt->data[e - 1], k = 0;
                        while (!((lb >> k) & 1)) k++;
                        mb_bits = (e - 1) * 8 + (7 - k);
                    }
                    audio_start = ((mb_bits + 15) / 16) * 2 + kf_word;
                }
            }
        } else if (m->audio_codec_id == AV_CODEC_ID_PCM_S16LE) {
            /* Encoder writes a 4-byte suffix: [pad][0x00][asize_u16le].
             * Use it when present to find the video/audio split. */
            int stored_asize = (size >= 4) ? (int)AV_RL16(pkt->data + size - 2) : 0;
            int spad = (size >= 4) ? pkt->data[size - 4] : 0;
            if (stored_asize > 0 && stored_asize <= (int)size && spad <= 3 && size >= 4) {
                audio_size  = stored_asize;
                audio_start = size - 4 - spad - stored_asize;
            } else {
                audio_size  = audio_blocks * (m->audio_channels * 512);
                audio_start = (int)size - audio_size;
            }
        } else {
            audio_size = 0;
            audio_start = size;
        }

        if (audio_start >= 0 && audio_size >= 0 && audio_start + audio_size <= (int)size) {
            if (audio_size > 0) {
                uint8_t *nb = av_realloc(m->aud_buf, audio_size);
                if (nb) {
                    m->aud_buf = nb;
                    memcpy(m->aud_buf, pkt->data + audio_start, audio_size);
                    m->aud_size = audio_size;
                    m->aud_off  = 0;
                }
            }
            pkt->size = audio_start;   /* drop audio + suffix from the video pkt */
        }
    }

    /* SX audio: split the audio tail from this frame and buffer it to emit as
     * the next packet (the full chunk is still emitted as the video packet
     * below; the mobiclip video decoder stops after the video bits). The low
     * 14 bits of the chunk word hold the number of 128-sample PERIODS this
     * frame carries (retail semantics: count == periods, verified — see
     * modsenc.c); each period is one AFrame per channel, so there are
     * periods*channels AFrames. The video/audio boundary comes from the 4-byte
     * [pad][0][asize u16] suffix our muxer writes. */
    if (m->has_sx_audio) {
        int periods   = (int)(word & 0x3FFF);
        int total_afr = periods * m->audio_channels;
        if (total_afr > 0 && size >= 4) {
            int is_kf = (size >= 2 && (pkt->data[1] & 0x80));
            int stored_asize = (int)AV_RL16(pkt->data + size - 2);
            int spad         = pkt->data[size - 4];
            int vfield = -1, audio_start = -1, audio_size = -1;

            if (stored_asize > 0 && spad <= 3 && stored_asize + spad + 4 <= (int)size) {
                /* Our muxer's explicit split suffix: [pad][0x00][asize u16le].
                 * asize includes the 4-byte keyframe reset word on keyframes. */
                vfield      = (int)size - 4 - spad - stored_asize;
                audio_start = vfield;
                audio_size  = stored_asize;
                if (is_kf && audio_size >= 4) {   /* skip the keyframe word */
                    audio_start += 4;
                    audio_size  -= 4;
                }
            } else {
                /* No suffix (older muxer output / retail DS .mods): decode the
                 * video through the private mobiclip decoder to find the split. */
                vfield = mods_sx_video_split(m, pkt->data, size);
                if (vfield >= 0 && vfield <= (int)size) {
                    audio_start = vfield;
                    /* Every video keyframe carries a 4-byte SX re-prime word
                     * before the intra AFrame (retail writes real, non-zero data
                     * there; our muxer writes zeros). Always skip it on keyframes
                     * so the first AFrame the decoder sees is the intra one. */
                    if (is_kf && audio_start + 4 <= (int)size)
                        audio_start += 4;
                    audio_size = (int)size - audio_start;
                }
            }

            if (vfield >= 0 && audio_start >= 0 && audio_size > 0 &&
                audio_start + audio_size <= (int)size) {
                uint8_t *nb = av_realloc(m->aud_buf, 4 + audio_size);
                if (nb) {
                    m->aud_buf = nb;
                    AV_WL32(m->aud_buf, (uint32_t)total_afr);
                    memcpy(m->aud_buf + 4, pkt->data + audio_start, audio_size);
                    m->aud_size = 4 + audio_size;
                    m->sx_emit_blocks = periods;
                }
                pkt->size = vfield; /* video only */
            }
        }
    }

    pkt->stream_index = 0;
    pkt->pts = m->frame_index;
    pkt->dts = m->frame_index;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    m->frame_index++;

    return ret;
}

static int mods_read_close(AVFormatContext *s)
{
    MODSDemuxContext *m = s->priv_data;
    av_freep(&m->aud_buf);
    return 0;
}

const FFInputFormat ff_mods_demuxer = {
    .p.name           = "mods",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Mobiclip MODS"),
    .priv_data_size = sizeof(MODSDemuxContext),
    .read_probe     = mods_probe,
    .read_header    = mods_read_header,
    .read_packet    = mods_read_packet,
    .read_close     = mods_read_close,
    .p.extensions     = "mods",
    .p.flags          = AVFMT_GENERIC_INDEX,
};
