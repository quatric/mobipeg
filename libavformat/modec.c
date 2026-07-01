/*
 * MO demuxer
 * Copyright (c) 2022 Spotlight Deveaux
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

#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "demux.h"
#include "libavutil/mem.h"
#include "libavutil/avassert.h"

#include "avformat.h"
#include "internal.h"
#include "mo.h"

typedef struct MoDemuxContext {
    int handle_audio_packet;
    uint32_t audio_size;
    uint32_t audio_padding;
    int current_frame;
    int last_video_frame;
    int64_t audio_sample_pos;
    unsigned int frame_count;
    int *keyframes;
    int64_t next_chunk_pos;
    /* Multi-packet Vorbis: remaining sub-packets in current audio section */
    int vorbis_pkt_remaining;
    /* Audio-start alignment: many retail clips have leading (and interspersed)
     * video frames with no audio payload. The audio belongs to the frame whose
     * chunk carries it, so the first real audio packet must be time-shifted to
     * that frame instead of starting at t=0 (otherwise audio plays early).
     * fps_fixed is the FORMAT_LENGTH framerate field (= fps * 256); samples per
     * frame = frame * 256 * sample_rate / fps_fixed. */
    int audio_started;
    uint32_t fps_fixed;
    /* Vorbis timing state for our own [0xFFFF] size-split sections: anchor the
     * first packet to its video frame, then let ffmpeg interpolate. Retail
     * sections use the per-section soff field below instead. */
    int64_t vorbis_sample_pos;
    int vorbis_pts_started;
    /* Retail Vorbis sections carry a per-section sample offset in the 4-byte
     * header: [LE16 seq][LE16 soff]. soff is the intra-frame sample offset, so
     * the absolute sample position of a section = frame * sr / fps + soff. This
     * is the authoritative timing (consecutive sections differ by exactly their
     * decoded sample counts), and using it keeps audio video-locked instead of
     * drifting. vorbis_soff_valid marks that the current section is a retail
     * (soff-carrying) section, vs our own [0xFFFF] size-split sections. */
    uint16_t vorbis_soff;
    int vorbis_soff_valid;
} MoDemuxContext;

static int mo_probe(const AVProbeData *p)
{
    /* Check for MOC5 magic bytes */
    if (AV_RL32(p->buf) == MO_TAG) {
        if (AV_RL32(p->buf + 4) < 0x28) // Rough minimum size
            return 0;
        if (AV_RL16(p->buf + 8) != FORMAT_LENGTH) // Typically first
            return 0;
        return AVPROBE_SCORE_MAX;
    }
    
    /* Also detect .mo files by extension */
    if (p->filename) {
        const char *ext = strrchr(p->filename, '.');
        if (ext && !av_strcasecmp(ext, ".mo"))
            return AVPROBE_SCORE_MAX / 2;  /* Lower than magic byte match but still high */
    }
    
    return 0;
}

static int mo_handle_audio(AVStream *ast, uint16_t marker, AVIOContext* pb) {
    ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    ast->codecpar->codec_tag = 0;
    
    uint32_t sample_rate = avio_rl32(pb);
    ast->codecpar->sample_rate = sample_rate;
    avpriv_set_pts_info(ast, 1, 1, ast->codecpar->sample_rate);

    // The container also stores a channel count. The FastAudio/ADPCM markers
    // are explicitly mono/stereo, so we trust the marker for those. But PCM has
    // only ONE marker ('AP') for both layouts, so we MUST honour the stored
    // channel count here — otherwise mono PCM is mis-read as stereo and decodes
    // at half rate (the frames-per-channel halve).
    uint32_t stored_channels = avio_rl32(pb);

    switch(marker) {
    case FORMAT_FASTAUDIO:
        ast->codecpar->codec_id = AV_CODEC_ID_FASTAUDIO;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        break;
    case FORMAT_FASTAUDIO_STEREO:
        ast->codecpar->codec_id = AV_CODEC_ID_FASTAUDIO;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    case FORMAT_PCM:
        ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        ast->codecpar->ch_layout = (stored_channels == 1)
            ? (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO
            : (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    case FORMAT_ADPCM:
        ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_MOBICLIP_WII;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        break;
    case FORMAT_ADPCM_STEREO:
        ast->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_MOBICLIP_WII;
        ast->codecpar->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        break;
    default:
        // Unknown audio type.
        return -1;
    }

    return 0;
};

static int mo_read_header(AVFormatContext *s)
{
    MoDemuxContext *mo = s->priv_data;
    AVIOContext *pb = s->pb;
    AVRational fps;

    // Wii MobiClips must have audio and video.
    // Though the format appears to support an audioless type
    // on some platforms, the library for the Wii does not.
    AVStream *vst = avformat_new_stream(s, NULL);
    if (!vst)
        return AVERROR(ENOMEM);
    
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id   = AV_CODEC_ID_MOBICLIP;

    AVStream *ast = avformat_new_stream(s, NULL);
    if (!ast)
        return AVERROR(ENOMEM);

    avio_skip(pb, 4);
    // Add 8 to account for magic and header length
    uint32_t header_length = avio_rl32(pb) + 8;

    mo->handle_audio_packet = 0;
    mo->audio_size = 0;
    mo->audio_padding = 0;
    mo->current_frame = 0;
    mo->last_video_frame = -1;
    mo->audio_sample_pos = 0;
    mo->frame_count = 0;
    mo->next_chunk_pos = 0;
    mo->keyframes = NULL;
    mo->audio_started = 0;
    mo->fps_fixed = 0;

    int has_read_header = 0;
    while (has_read_header == 0) {
        if (avio_tell(pb) > header_length) {
            // Exhausted header
            break;
        }

        uint16_t format_marker = avio_rl16(pb);
        av_log(s, AV_LOG_TRACE, "Handling '%c%c'...\n",
            (uint8_t)format_marker, (uint8_t)(format_marker >> 8));

        // Length in file is amount of u32s available within format segment.
        uint16_t format_length = avio_rl16(pb) * 4;
        if ((avio_tell(pb) + format_length) > header_length) {
            // Will exhaust header length
            break;
        }

        // Used for stream helper functions.
        int result;

        switch (format_marker) {
        case FORMAT_LENGTH:
            // 256.0 / fps for our time base
            // exact fps is that flipped, fps / 256.0
            fps.num = 256;
            fps.den = avio_rl32(pb);
            mo->fps_fixed = fps.den;
            avpriv_set_pts_info(vst, 1, fps.num, fps.den);

            // TODO: can we use chunk count?
            mo->frame_count = avio_rl32(pb);
            vst->duration = mo->frame_count;
            /* Don't set audio duration here - it will be determined from actual
             * audio chunks read during demuxing, not from frame count */
            /* ast->duration = mo->frame_count; */

            // TODO: what is this?
            avio_skip(pb, 4);
            break;
        case FORMAT_VIDEO:
            vst->codecpar->width  = avio_rl32(pb);
            vst->codecpar->height = avio_rl32(pb);
            break;
        case FORMAT_RSA:
            // We can not - and will not - handle validating RSA signatures.
            avio_skip(pb, format_length);
            break;
        case FORMAT_UNKNOWN_AUDIO:
            // TODO: Should we rightfully ignore this chunk?
            // This existing may imply a stereo track.
            avio_skip(pb, format_length);
            break;
        case FORMAT_FASTAUDIO:
        case FORMAT_FASTAUDIO_STEREO:
        case FORMAT_PCM:
        case FORMAT_ADPCM:
        case FORMAT_ADPCM_STEREO:
            result = mo_handle_audio(ast, format_marker, pb);
            if (result == -1) { // Unknown audio type
                return AVERROR_PATCHWELCOME;
            }
            break;
        case FORMAT_MULTITRACK:
            return AVERROR_PATCHWELCOME;
        case FORMAT_VORBIS:
        {
            ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            ast->codecpar->codec_id = AV_CODEC_ID_VORBIS;
            /* Tag this as MobiClip vorbis so the (patched) vorbis decoder
             * self-delimits multiple concatenated packets per AVPacket. */
            ast->codecpar->codec_tag = MKTAG('M', 'o', 'V', 'o');

            uint32_t p1_size = avio_rl32(pb);
            if (p1_size > 4096) return AVERROR_INVALIDDATA;
            uint8_t *p1 = av_malloc(p1_size);
            avio_read(pb, p1, p1_size);
            
            uint32_t p2_size = avio_rl32(pb);
            if (p2_size > 4096) return AVERROR_INVALIDDATA;
            uint8_t *p2 = av_malloc(p2_size);
            avio_read(pb, p2, p2_size);
            
            uint32_t p3_size = avio_rl32(pb);
            if (p3_size > 65536) return AVERROR_INVALIDDATA;
            uint8_t *p3 = av_malloc(p3_size);
            avio_read(pb, p3, p3_size);
            
            int extradata_size = 1 + 1 + 1 + p1_size + p2_size + p3_size;
            ast->codecpar->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            ast->codecpar->extradata_size = extradata_size;
            
            uint8_t *ed = ast->codecpar->extradata;
            ed[0] = 2; // 3 headers
            
            // Xiph lacing encoding for lengths (max 255 per byte).
            // But Vorbis headers are small enough except maybe p3, which is implicit.
            // p1 and p2 must be xiph laced. Since they are small, we just use 1 byte if they are < 255.
            // But if they are >= 255, we need proper lacing. Let's do it properly just in case.
            int offset = 1;
            int len = p1_size;
            while (len >= 255) { ed[offset++] = 255; len -= 255; }
            ed[offset++] = len;
            
            len = p2_size;
            while (len >= 255) { ed[offset++] = 255; len -= 255; }
            ed[offset++] = len;
            
            memcpy(ed + offset, p1, p1_size);
            offset += p1_size;
            memcpy(ed + offset, p2, p2_size);
            offset += p2_size;
            memcpy(ed + offset, p3, p3_size);
            offset += p3_size;
            
            ast->codecpar->extradata_size = offset;
            
            if (p1[0] == 1 && !strncmp((const char*)p1 + 1, "vorbis", 6)) {
                ast->codecpar->ch_layout.nb_channels = p1[11];
                ast->codecpar->sample_rate = AV_RL32(p1 + 12);
            } else {
                ast->codecpar->ch_layout.nb_channels = 2;
                ast->codecpar->sample_rate = 48000;
            }
            avpriv_set_pts_info(ast, 1, 1, ast->codecpar->sample_rate);

            /* We supply exact per-packet timestamps (via the vorbis parser in
             * mo_read_packet); don't let a stream parser re-packetize and
             * discard them. */
            ffstream(ast)->need_parsing = AVSTREAM_PARSE_NONE;

            av_free(p1);
            av_free(p2);
            av_free(p3);
            
            int bytes_read = 4 + p1_size + 4 + p2_size + 4 + p3_size;
            if (format_length > bytes_read) {
                avio_skip(pb, format_length - bytes_read);
            }
            
            break;
        }
        case FORMAT_KEYINDEX:
            if (mo->frame_count == 0) {
                av_log(s, AV_LOG_WARNING, "Ignoring keyframe index before frame count metadata.\n");
                avio_skip(pb, format_length);
                break;
            }

            mo->keyframes = av_calloc(mo->frame_count + 1, sizeof(*mo->keyframes));
            if (!mo->keyframes)
                return AVERROR(ENOMEM);

            for (int i = 0; i < format_length / 8; ++i) {
                uint32_t frame;

                avio_skip(pb, 4);
                frame = avio_rl32(pb);

                if (frame <= mo->frame_count)
                    mo->keyframes[frame] = 1;
            }
            break;
        case FORMAT_HEADER_DONE:
            // We should be finished!
            has_read_header = 1;
            break;
        case FORMAT_VIDEO_CODEC:
            /* Video codec identifier (MBCL or H264) — codec is determined from
             * payload sniffing later, so just skip this header entry. */
            avio_skip(pb, format_length);
            break;
        case FORMAT_POSSIBLY_CAPTIONS:
            /* Optional subtitle/caption track — not yet supported, skip. */
            avio_skip(pb, format_length);
            break;
        default:
            av_log(s, AV_LOG_DEBUG, "Encountered unknown chunk '%c%c' - ignoring.\n",
                (uint8_t)format_marker, (uint8_t)(format_marker >> 8));
            avio_skip(pb, format_length);
            break;
        }
    }

    if (!has_read_header) {
        return AVERROR_EOF;
    }

    // Sniff first video packet to check if payload is H264
    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        int64_t start_pos = avio_tell(pb);
        uint32_t chunk_size = avio_rl32(pb);
        uint32_t video_size = avio_rl32(pb);
        if (video_size > 0 && chunk_size >= video_size + 8) {
            uint8_t peek[4] = {0};
            int read_bytes = avio_read(pb, peek, 4);
            if (read_bytes == 4) {
                /* .mo container format is always MobiClip video.
                 * Do not switch to H.264 codec even if the payload contains
                 * Annex-B markers - those are just the x264 output format. */
            }
        }
        avio_seek(pb, start_pos, SEEK_SET);
    }

    return 0;
}

static int mo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MoDemuxContext *mo = s->priv_data;
    AVIOContext *pb = s->pb;
    int ret;

    // Determine whether audio or video.
    if (mo->handle_audio_packet) {
        // We now need to read the audio packet within this chunk.
        if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_VORBIS) {
            /* Vorbis audio section formats:
             *
             * Retail (single-packet): [LE16 seq][LE16 sample_offset][vorbis_data]
             *   seq is always < 0xFFFF for reasonable stream lengths.
             *
             * Encoder multi-packet: [LE16 0xFFFF][LE16 num_packets]
             *   followed by num_packets × ([LE32 size][data]).
             *
             * The 4-byte [seq][offset] header is followed by the real payload;
             * an "empty" section is the header plus at most a couple of 4-byte
             * alignment-pad bytes (observed audio_size 4 and 5 in retail clips,
             * vs >=48 for real packets). Treat <=7 payload bytes as no audio so
             * these spurious markers don't anchor the audio timeline to frame 0
             * (which made audio start early on clips that open silent). */
            if (mo->audio_size <= 7) {
                avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
                mo->handle_audio_packet = 0;
                mo->vorbis_pkt_remaining = 0;
                goto read_video;
            }

            if (mo->vorbis_pkt_remaining > 0) {
                /* We're mid-way through a multi-packet section */
                uint32_t pkt_size = avio_rl32(pb);
                if (pkt_size > (uint32_t)(mo->next_chunk_pos - avio_tell(pb)))
                    pkt_size = 0;
                ret = av_get_packet(pb, pkt, pkt_size);
                mo->vorbis_pkt_remaining--;
                if (mo->vorbis_pkt_remaining == 0) {
                    avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
                    mo->handle_audio_packet = 0;
                }
            } else {
                /* Read the 4-byte prefix and dispatch */
                uint16_t seq = avio_rl16(pb);
                uint16_t sample_offset = avio_rl16(pb);
                if (seq == 0xFFFF) {
                    /* Multi-packet: num_packets was in sample_offset position */
                    /* Re-read properly: we read seq=0xFFFF and discarded next u16.
                     * Back up 2 bytes and read num_packets correctly. */
                    avio_seek(pb, -2, SEEK_CUR);
                    uint16_t num_pkts = avio_rl16(pb);
                    if (num_pkts == 0) {
                        avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
                        mo->handle_audio_packet = 0;
                        goto read_video;
                    }
                    /* Read first packet */
                    uint32_t pkt_size = avio_rl32(pb);
                    if (pkt_size > (uint32_t)(mo->next_chunk_pos - avio_tell(pb)))
                        pkt_size = 0;
                    ret = av_get_packet(pb, pkt, pkt_size);
                    mo->vorbis_pkt_remaining = num_pkts - 1;
                    mo->vorbis_soff_valid = 0; /* our own size-split format */
                    if (mo->vorbis_pkt_remaining == 0) {
                        avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
                        mo->handle_audio_packet = 0;
                    }
                } else {
                    /* Retail section: the rest of the audio region is one or more
                     * vorbis packets concatenated (no size table); the patched
                     * vorbis decoder self-delimits them. Include the trailing 4-
                     * byte chunk padding so the final packet is byte-complete
                     * (omitting it leaves the bit reader short -> "Overread").
                     * Stash soff for video-locked timing. */
                    int psize = mo->audio_size - 4 + mo->audio_padding;
                    ret = av_get_packet(pb, pkt, psize);
                    mo->vorbis_soff = sample_offset;
                    mo->vorbis_soff_valid = 1;
                }
            }
        } else {
            int read_size = mo->audio_size;
            int block_size = 0;
            if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_ADPCM_IMA_MOBICLIP_WII) {
                int ch = s->streams[1]->codecpar->ch_layout.nb_channels;
                block_size = ch * 132;
            } else if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE) {
                /* The muxer folds the chunk's trailing alignment pad into the
                 * audio tail, so a folded PCM chunk reports an audio_size that
                 * is not a whole number of sample frames (channels * 2 bytes);
                 * round it back up using the pad bytes so pcm_s16le gets a
                 * complete frame.  Unfolded (legacy) chunks are already aligned
                 * and pass through unchanged. */
                int ch = s->streams[1]->codecpar->ch_layout.nb_channels;
                if (ch <= 0) ch = 1;
                block_size = ch * 2;
            } else if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_FASTAUDIO) {
                /* FastAudio frames are exactly 40 bytes/channel (256 samples).
                 * The fastaudio decoder floor-divides pkt->size/(40*ch), so a
                 * chunk whose reported audio_size isn't a whole frame (chunk
                 * alignment padding) loses its last frame. Round up via the pad
                 * bytes so every full frame is delivered. */
                int ch = s->streams[1]->codecpar->ch_layout.nb_channels;
                if (ch <= 0) ch = 1;
                block_size = ch * 40;
            }
            if (block_size > 0) {
                int total_available = mo->audio_size + mo->audio_padding;
                int remainder = read_size % block_size;
                if (remainder != 0) {
                    int padding_needed = block_size - remainder;
                    if (padding_needed <= (int)mo->audio_padding)
                        read_size += padding_needed;
                    else
                        read_size = total_available;
                }
            }
            ret = av_get_packet(pb, pkt, read_size);
        }
        
        if (ret < 0) {
            return ret;
        }

        // Stream 1 is always audio.
        pkt->stream_index = 1;

        /* Anchor the audio timeline to the first frame that actually carries
         * audio. No-audio Vorbis frames are skipped before reaching here, so
         * the first packet we emit may belong to a much later frame; without
         * this the audio would play from t=0 (too early). For PCM/ADPCM (audio
         * on every frame) this anchors to frame 0 and is a no-op. */
        int anchored_now = 0;
        if (!mo->audio_started && mo->fps_fixed) {
            int sr = s->streams[1]->codecpar->sample_rate;
            int frame = mo->current_frame > 0 ? mo->current_frame - 1 : 0;
            mo->audio_sample_pos = (int64_t)frame * 256 * sr / mo->fps_fixed;
            mo->audio_started = 1;
            anchored_now = 1;
        }

        if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE) {
            int channels = s->streams[1]->codecpar->ch_layout.nb_channels;
            if (channels <= 0)
                channels = 1;
            int samples = ret / (channels * 2);
            pkt->pts = mo->audio_sample_pos;
            pkt->dts = mo->audio_sample_pos;
            pkt->duration = samples;
            mo->audio_sample_pos += samples;
        } else if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_ADPCM_IMA_MOBICLIP_WII) {
            int channels = s->streams[1]->codecpar->ch_layout.nb_channels;
            if (channels <= 0)
                channels = 1;
            int block_size = channels * 132;
            int blocks = ret / block_size;
            int samples = blocks * 256;
            pkt->pts = mo->audio_sample_pos;
            pkt->dts = mo->audio_sample_pos;
            pkt->duration = samples;
            mo->audio_sample_pos += samples;
        } else if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_FASTAUDIO) {
            /* FastAudio: 40 bytes/channel per 256-sample frame. */
            int channels = s->streams[1]->codecpar->ch_layout.nb_channels;
            if (channels <= 0)
                channels = 1;
            int frames = ret / (channels * 40);
            int samples = frames * 256;
            pkt->pts = mo->audio_sample_pos;
            pkt->dts = mo->audio_sample_pos;
            pkt->duration = samples;
            mo->audio_sample_pos += samples;
        } else if (s->streams[1]->codecpar->codec_id == AV_CODEC_ID_VORBIS) {
            /* Vorbis: a single audio section can hold MANY vorbis packets
             * concatenated with NO size table (retail Nintendo Channel/Wii no
             * Ma format). The packet count is recoverable from the per-section
             * sequence number delta, but the byte boundaries are not stored -
             * the Wii player relies on Tremor self-delimiting each packet by
             * consuming exactly its bits. We mirror that: the whole audio
             * section is emitted as ONE AVPacket and our patched vorbis decoder
             * (gated on codec_tag == 'MoVo') returns the bytes it actually
             * consumed, so ffmpeg's decode loop re-feeds the remainder and
             * decodes every packet in the section. (Our own encoder's explicit
             * [LE32 size] multi-packet sections are already split above, so each
             * of those AVPackets is exactly one vorbis packet.)
             *
             * Because one AVPacket can yield several frames, we anchor ONLY the
             * first emitted packet to its video-frame sample position and leave
             * every later packet's timestamp unset; ffmpeg then interpolates a
             * continuous, gap-free audio timeline from each frame's nb_samples.
             * This avoids both the old duplicate-pts stutter and the drift that
             * came from undercounting the dropped concatenated packets. */
            (void)anchored_now;
            {
                AVCodecParameters *apar = s->streams[1]->codecpar;
                int sr = apar->sample_rate;
                if (mo->vorbis_soff_valid && mo->fps_fixed) {
                    /* Retail: absolute position = frame * sr/fps + soff. This is
                     * video-locked and gap-exact (consecutive sections differ by
                     * their decoded sample count), so audio neither drifts nor
                     * collapses real silence gaps. The decoder splits this
                     * section's concatenated packets and ffmpeg interpolates the
                     * intermediate frame timestamps from the anchor. */
                    int frame = mo->current_frame > 0 ? mo->current_frame - 1 : 0;
                    pkt->pts = pkt->dts =
                        (int64_t)frame * 256 * sr / mo->fps_fixed + mo->vorbis_soff;
                } else if (!mo->vorbis_pts_started) {
                    /* Our own [0xFFFF] size-split sections: anchor the first
                     * packet to its frame, interpolate the rest. */
                    int frame = mo->current_frame > 0 ? mo->current_frame - 1 : 0;
                    mo->vorbis_sample_pos = mo->fps_fixed
                            ? (int64_t)frame * 256 * sr / mo->fps_fixed
                            : 0;
                    mo->vorbis_pts_started = 1;
                    pkt->pts = pkt->dts = mo->vorbis_sample_pos;
                } else {
                    pkt->pts = pkt->dts = AV_NOPTS_VALUE;
                }
                pkt->duration = 0;
            }
        }

        /* For non-Vorbis and retail Vorbis, seek to next chunk now.
         * For multi-packet Vorbis mid-section, skip: more packets to serve
         * from this chunk before advancing. Seek/clear already happens when
         * vorbis_pkt_remaining reaches 0 in the Vorbis branch above. */
        if (mo->handle_audio_packet && mo->vorbis_pkt_remaining == 0) {
            avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
            mo->handle_audio_packet = 0;
        }
    } else {
read_video:
        ;
        int64_t chunk_pos = avio_tell(pb);
        // Dissect the current packet's header.
        uint32_t chunk_size = avio_rl32(pb);
        uint32_t video_size = avio_rl32(pb);
        /* Audio-less "skip frame" chunks (all-0xFF P-frames in retail files)
         * have chunk_size = video_size + 4: the video payload's last 4 bytes
         * occupy the trailing pad region. Only video_size > chunk_size - 4
         * indicates real corruption / end of stream. */
        if (chunk_size < 8 || video_size > chunk_size - 4)
            return AVERROR_EOF;

        // Calculate next chunk pos: MO format always pads 1-4 bytes
        // between chunks (even when chunk_end is already 4-aligned).
        int64_t raw_end = chunk_pos + chunk_size;
        int pad = (int)(4 - (raw_end % 4));
        int64_t next_chunk_pos = raw_end + pad;
        mo->next_chunk_pos = next_chunk_pos;
        
        int32_t audio_size = chunk_size - video_size - 8;
        if (audio_size < 0) {
            audio_size = 0;
        }

        mo->audio_size = audio_size;
        mo->audio_padding = pad;

        ret = av_get_packet(pb, pkt, video_size);
        if (ret < 0) {
            return ret;
        }

        // Stream 0 is always video.
        pkt->stream_index = 0;
        pkt->pts = mo->current_frame;
        pkt->dts = mo->current_frame;
        pkt->duration = 1;

        // If H264, prepend start code and check for AVCC length prefixes
        if (s->streams[0]->codecpar->codec_id == AV_CODEC_ID_H264) {
            // Check if packet already starts with a start code
            int has_start_code = (pkt->size >= 4 && pkt->data[0] == 0 && pkt->data[1] == 0 && 
                                  (pkt->data[2] == 1 || (pkt->data[2] == 0 && pkt->data[3] == 1))) ||
                                 (pkt->size >= 3 && pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1);
             
            // If no start code, prepend one using av_grow_packet
            if (!has_start_code) {
                int ret_grow = av_grow_packet(pkt, 4);
                if (ret_grow < 0) return ret_grow;
                 
                // Shift data 4 bytes to the right
                memmove(pkt->data + 4, pkt->data, pkt->size - 4);
                 
                // Add 4-byte start code at the beginning
                pkt->data[0] = 0;
                pkt->data[1] = 0;
                pkt->data[2] = 0;
                pkt->data[3] = 1;
            }
             
            // Now check for internal AVCC length prefixes and convert them
            if (pkt->size >= 8) {
                uint32_t offset = 4;  // Start after the initial start code
                while (offset + 4 <= pkt->size) {
                    uint32_t nal_len = ((uint32_t)pkt->data[offset] << 24) |
                                       ((uint32_t)pkt->data[offset + 1] << 16) |
                                       ((uint32_t)pkt->data[offset + 2] << 8)  |
                                       pkt->data[offset + 3];
                    // Check if this looks like a valid AVCC length
                    if (nal_len != 0 && nal_len < 100000 && offset + 4 + nal_len <= pkt->size) {
                        // Convert AVCC length prefix to Annex B start code
                        pkt->data[offset] = 0;
                        pkt->data[offset + 1] = 0;
                        pkt->data[offset + 2] = 0;
                        pkt->data[offset + 3] = 1;
                        offset += 4 + nal_len;
                    } else {
                        break;
                    }
                }
            }
        }

        if (mo->current_frame == 0 ||
            (mo->keyframes && mo->current_frame <= mo->frame_count &&
             mo->keyframes[mo->current_frame]))
          pkt->flags |= AV_PKT_FLAG_KEY;

        mo->last_video_frame = mo->current_frame;

        if (mo->audio_size > 0) {
            mo->handle_audio_packet = 1;
        } else {
            // Seek to the next chunk's aligned position
            avio_seek(pb, mo->next_chunk_pos, SEEK_SET);
            mo->handle_audio_packet = 0;
        }
        mo->current_frame += 1;
    }

    return ret;
}

static int mo_read_close(AVFormatContext *s)
{
    MoDemuxContext *mo = s->priv_data;

    av_freep(&mo->keyframes);
    return 0;
}

const FFInputFormat ff_mo_demuxer = {
    .p.name           = "mobiclip_mo",
    .p.long_name      = NULL_IF_CONFIG_SMALL("MobiClip MO"),
    .read_probe     = mo_probe,
    .read_header    = mo_read_header,
    .read_packet    = mo_read_packet,
    .read_close     = mo_read_close,
    .priv_data_size = sizeof(MoDemuxContext),
    .p.extensions     = "mo",
    .p.flags          = AVFMT_GENERIC_INDEX,
};
