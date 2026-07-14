/*
 * Flipnote Studio (PPM) demuxer
 * Copyright (c) 2026
 *
 * Decodes the video of Nintendo (DSiWare) Flipnote Studio `.ppm` animation
 * files to RGB24 rawvideo. Ported from flipnote.js (James Daniel / Flipnote
 * Collective), PPM format docs:
 *   https://github.com/Flipnote-Collective/flipnote-studio-docs/wiki/PPM-format
 *
 * Decode only. Audio (ADPCM sound tracks) is not exposed here.
 *
 * Note: named "flipnote_ppm" to avoid a clash with the PNM/Portable-PixMap
 * image demuxer, which also uses the .ppm extension for a different format.
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

#include <math.h>

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define PPM_W 256
#define PPM_H 192
#define PPM_PIXELS (PPM_W * PPM_H)

/* framerates indexed by in-app frame speed (speed 0 never normally used) */
static const float PPM_FRAMERATES[9] = {
    0.5f, 0.5f, 1, 2, 4, 6, 12, 20, 30
};

/* palette: WHITE, BLACK, RED, BLUE */
static const uint8_t PPM_RGB[4][3] = {
    { 0xff, 0xff, 0xff }, /* WHITE */
    { 0x0e, 0x0e, 0x0e }, /* BLACK */
    { 0xff, 0x2a, 0x2a }, /* RED   */
    { 0x0a, 0x39, 0xff }, /* BLUE  */
};

#define PPM_RAW_SAMPLE_RATE 8192
#define PPM_SAMPLE_RATE     32768

static const int8_t ADPCM_INDEX_TABLE_4BIT[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t ADPCM_STEP_TABLE[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767, 0
};

typedef struct PpmDemuxContext {
    uint8_t *buf;
    int64_t  size;

    int      frame_count;
    int      frame;
    AVRational fps;

    uint32_t frame_data_length;
    uint32_t *frame_offsets;   /* per frame absolute byte offset */
    int      num_offsets;

    /* layer + diffing buffers (values 0/1) */
    uint8_t *layer[2];
    uint8_t *prev[2];
    uint8_t  line_enc[2][PPM_H];

    /* audio */
    int      has_audio;
    int      audio_stream_index;
    int16_t *audio;        /* mixed mono master PCM at PPM_SAMPLE_RATE */
    int      audio_len;    /* samples */
    double   samples_per_frame;
    int      pending_audio;
} PpmDemuxContext;

static int ppm_probe(const AVProbeData *p)
{
    if (p->buf_size < 4)
        return 0;
    if (AV_RL32(p->buf) == MKTAG('P', 'A', 'R', 'A'))
        return AVPROBE_SCORE_MAX;
    return 0;
}

/* Decode a 4-bit IMA-style ADPCM track (flipnote PPM variant, low nibble first).
 * PPM audio tracks carry no per-track state header: decoding starts at byte 0
 * with predictor = 0 and step index = 0 (see flipnote.js PpmParser).
 * Returns malloc'd Int16 PCM, *out_len samples. */
static int16_t *ppm_decode_track(const uint8_t *p, int len, int *out_len)
{
    int16_t *dst;
    int src_size, i, dst_ptr = 0;
    int predictor = 0, step_index = 0, low_nibble = 1;

    *out_len = 0;
    if (len <= 0)
        return NULL;
    src_size = len;

    dst = av_malloc_array(src_size * 2, sizeof(*dst));
    if (!dst)
        return NULL;

    for (i = 0; i < src_size; ) {
        int sample, step, diff;
        if (low_nibble)
            sample = p[i] & 0xF;
        else
            sample = p[i++] >> 4;
        low_nibble = !low_nibble;

        step = ADPCM_STEP_TABLE[step_index];
        diff = step >> 3;
        if (sample & 1) diff += step >> 2;
        if (sample & 2) diff += step >> 1;
        if (sample & 4) diff += step;
        if (sample & 8) diff = -diff;
        predictor  = av_clip_int16(predictor + diff);
        step_index = av_clip(step_index + ADPCM_INDEX_TABLE_4BIT[sample], 0, 88);
        dst[dst_ptr++] = predictor;
    }
    *out_len = dst_ptr;
    return dst;
}

/* zero-order-hold (nearest neighbour) resample */
static int16_t *pcm_resample_nn(const int16_t *src, int src_len,
                               double src_freq, double dst_freq, int *out_len)
{
    int dst_len = (int)((double)src_len / src_freq * dst_freq);
    double adj = src_freq / dst_freq;
    int16_t *dst;
    int i;
    *out_len = 0;
    if (dst_len <= 0)
        return NULL;
    dst = av_malloc_array(dst_len, sizeof(*dst));
    if (!dst)
        return NULL;
    for (i = 0; i < dst_len; i++) {
        int sp = (int)(i * adj);
        dst[i] = (sp >= 0 && sp < src_len) ? src[sp] : 0;
    }
    *out_len = dst_len;
    return dst;
}

static void pcm_mix_half(int16_t *dst, int dst_len,
                        const int16_t *src, int src_len, int off)
{
    int n;
    for (n = 0; n < src_len; n++) {
        int idx = off + n;
        if (idx >= dst_len)
            break;
        dst[idx] = av_clip_int16(dst[idx] + src[n] / 2);
    }
}

/* Build the mixed mono master audio track. Returns 0 on success (c->audio may
 * remain NULL if the note has no audio). */
static int ppm_build_audio(PpmDemuxContext *c, int frame_speed, int bgm_speed)
{
    const int dst_freq = PPM_SAMPLE_RATE;
    double framerate = PPM_FRAMERATES[frame_speed];
    double bgmrate   = PPM_FRAMERATES[bgm_speed];
    double duration  = c->frame_count / framerate;
    int master_len   = (int)ceil(duration * dst_freq);
    int64_t sound_off;
    uint32_t track_len[4];
    int64_t track_ptr[4];
    int16_t *master, *bgm = NULL;
    int bgm_len = 0, t, i;

    if (master_len <= 0)
        return 0;

    sound_off = 0x6A0 + (int64_t)c->frame_data_length + c->frame_count;
    if (sound_off % 4)
        sound_off += 4 - (sound_off % 4);
    if (sound_off + 32 > c->size)
        return 0;
    for (t = 0; t < 4; t++)
        track_len[t] = AV_RL32(c->buf + sound_off + t * 4);
    /* tracks begin 32 bytes after the sound header start */
    track_ptr[0] = sound_off + 32;
    for (t = 1; t < 4; t++)
        track_ptr[t] = track_ptr[t-1] + track_len[t-1];

    /* nothing to do if every track is empty */
    for (i = 0, t = 0; t < 4; t++) i += track_len[t];
    if (i == 0)
        return 0;

    master = av_calloc(master_len, sizeof(*master));
    if (!master)
        return AVERROR(ENOMEM);

    /* BGM (track 0) mixed at offset 0 */
    if (track_len[0] > 0 && track_ptr[0] + track_len[0] <= c->size) {
        int raw_len, res_len;
        int16_t *raw = ppm_decode_track(c->buf + track_ptr[0], track_len[0], &raw_len);
        if (raw) {
            double src_freq = PPM_RAW_SAMPLE_RATE * (framerate / bgmrate);
            bgm = pcm_resample_nn(raw, raw_len, src_freq, dst_freq, &res_len);
            av_free(raw);
            if (bgm) {
                pcm_mix_half(master, master_len, bgm, res_len, 0);
                av_freep(&bgm);
            }
            (void)bgm_len;
        }
    }

    /* Sound effects (tracks 1..3) mixed at per-frame offsets when flagged */
    {
        int16_t *se[3] = { NULL, NULL, NULL };
        int se_len[3]  = { 0, 0, 0 };
        double spf = (double)dst_freq / framerate;
        for (t = 1; t < 4; t++) {
            if (track_len[t] > 0 && track_ptr[t] + track_len[t] <= c->size) {
                int raw_len, res_len;
                int16_t *raw = ppm_decode_track(c->buf + track_ptr[t], track_len[t], &raw_len);
                if (raw) {
                    se[t-1] = pcm_resample_nn(raw, raw_len, PPM_RAW_SAMPLE_RATE,
                                              dst_freq, &res_len);
                    se_len[t-1] = res_len;
                    av_free(raw);
                }
            }
        }
        if (se[0] || se[1] || se[2]) {
            int64_t flags_ptr = 0x6A0 + (int64_t)c->frame_data_length;
            for (i = 0; i < c->frame_count; i++) {
                int off = (int)ceil(i * spf);
                uint8_t flag;
                if (flags_ptr + i >= c->size)
                    break;
                flag = c->buf[flags_ptr + i];
                if (se[0] && (flag & 0x1)) pcm_mix_half(master, master_len, se[0], se_len[0], off);
                if (se[1] && (flag & 0x2)) pcm_mix_half(master, master_len, se[1], se_len[1], off);
                if (se[2] && (flag & 0x4)) pcm_mix_half(master, master_len, se[2], se_len[2], off);
            }
        }
        for (t = 0; t < 3; t++)
            av_freep(&se[t]);
    }

    c->audio = master;
    c->audio_len = master_len;
    c->samples_per_frame = (double)dst_freq / framerate;
    return 0;
}

static av_cold int ppm_read_header(AVFormatContext *s)
{
    PpmDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st;
    int64_t sz = avio_size(pb);
    int64_t sound_off;
    uint16_t offset_table_length;
    int n, frame_speed, bgm_speed, ret;

    if (sz <= 0x6A8 || sz > INT_MAX)
        return AVERROR_INVALIDDATA;
    c->size = sz;
    c->buf = av_malloc(sz);
    if (!c->buf)
        return AVERROR(ENOMEM);
    if ((ret = avio_read(pb, c->buf, sz)) < 0)
        return ret;
    if (ret != sz)
        return AVERROR_INVALIDDATA;

    c->frame_data_length = AV_RL32(c->buf + 4);
    c->frame_count = AV_RL16(c->buf + 0xC) + 1;

    /* sound header holds the frame speed */
    sound_off = 0x6A0 + (int64_t)c->frame_data_length + c->frame_count;
    if (sound_off % 4)
        sound_off += 4 - (sound_off % 4);
    if (sound_off + 17 > c->size)
        return AVERROR_INVALIDDATA;
    frame_speed = 8 - c->buf[sound_off + 16];
    bgm_speed   = 8 - c->buf[sound_off + 17];
    if (frame_speed < 0 || frame_speed > 8)
        return AVERROR_INVALIDDATA;
    if (bgm_speed < 0 || bgm_speed > 8)
        bgm_speed = frame_speed;

    /* animation header: offset table at 0x6A0 */
    offset_table_length = AV_RL16(c->buf + 0x6A0);
    c->num_offsets = offset_table_length / 4;
    if (c->num_offsets <= 0 || c->num_offsets > c->frame_count)
        return AVERROR_INVALIDDATA;

    c->frame_offsets = av_malloc_array(c->num_offsets, sizeof(*c->frame_offsets));
    if (!c->frame_offsets)
        return AVERROR(ENOMEM);
    for (n = 0; n < c->num_offsets; n++) {
        int64_t entry = 0x6A8 + (int64_t)n * 4;
        int64_t ptr;
        if (entry + 4 > c->size)
            return AVERROR_INVALIDDATA;
        ptr = 0x6A8 + (int64_t)offset_table_length + AV_RL32(c->buf + entry);
        if (ptr >= c->size)
            return AVERROR_INVALIDDATA;
        c->frame_offsets[n] = ptr;
    }
    /* only frames present in the offset table can be decoded */
    if (c->frame_count > c->num_offsets)
        c->frame_count = c->num_offsets;

    c->layer[0] = av_mallocz(PPM_PIXELS);
    c->layer[1] = av_mallocz(PPM_PIXELS);
    c->prev[0]  = av_mallocz(PPM_PIXELS);
    c->prev[1]  = av_mallocz(PPM_PIXELS);
    if (!c->layer[0] || !c->layer[1] || !c->prev[0] || !c->prev[1])
        return AVERROR(ENOMEM);

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    c->fps = av_d2q(PPM_FRAMERATES[frame_speed], INT_MAX);
    avpriv_set_pts_info(st, 64, c->fps.den, c->fps.num);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format     = AV_PIX_FMT_RGB24;
    st->codecpar->width      = PPM_W;
    st->codecpar->height     = PPM_H;
    st->nb_frames = st->duration = c->frame_count;

    /* audio: decode + mix all tracks to one mono master stream */
    if ((ret = ppm_build_audio(c, frame_speed, bgm_speed)) < 0)
        return ret;
    if (c->audio && c->audio_len > 0) {
        AVStream *ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        c->has_audio = 1;
        c->audio_stream_index = ast->index;
        avpriv_set_pts_info(ast, 64, 1, PPM_SAMPLE_RATE);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
        ast->codecpar->format      = AV_SAMPLE_FMT_S16;
        ast->codecpar->sample_rate = PPM_SAMPLE_RATE;
        ast->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        ast->duration              = c->audio_len;
    }
    return 0;
}

/* per-frame palette indices: [paper, layer1 color, layer2 color] */
static void frame_palette(PpmDemuxContext *c, int frame, int pal[3])
{
    uint8_t header = c->buf[c->frame_offsets[frame]];
    int is_inverted = (header & 0x1) != 1;
    int pen_map[4];
    pen_map[0] = is_inverted ? 0 : 1;
    pen_map[1] = is_inverted ? 0 : 1;
    pen_map[2] = 2;
    pen_map[3] = 3;
    pal[0] = is_inverted ? 1 : 0;
    pal[1] = pen_map[(header >> 1) & 0x3];
    pal[2] = pen_map[(header >> 3) & 0x3];
}

static int ppm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    PpmDemuxContext *c = s->priv_data;
    const uint8_t *p, *end;
    uint8_t header;
    int is_key, is_translated;
    int translate_x = 0, translate_y = 0;
    int layer, y, i, ret, pal[3];
    uint8_t *dst;

    /* emit the audio chunk for the frame we just output as video */
    if (c->pending_audio) {
        int start = (int)(c->frame * c->samples_per_frame);
        int stop  = (c->frame + 1 >= c->frame_count) ? c->audio_len
                                                      : (int)((c->frame + 1) * c->samples_per_frame);
        c->pending_audio = 0;
        if (stop > c->audio_len) stop = c->audio_len;
        if (start < 0) start = 0;
        c->frame++;
        if (stop > start) {
            int nb = stop - start;
            if ((ret = av_new_packet(pkt, nb * 2)) < 0)
                return ret;
            memcpy(pkt->data, c->audio + start, nb * 2);
            pkt->stream_index = c->audio_stream_index;
            pkt->pts = pkt->dts = start;
            pkt->duration = nb;
            pkt->flags |= AV_PKT_FLAG_KEY;
            return 0;
        }
        /* empty chunk: fall through to next video frame */
    }

    if (c->frame >= c->frame_count)
        return AVERROR_EOF;

    p   = c->buf + c->frame_offsets[c->frame];
    end = c->buf + c->size;

    header        = *p++;
    is_key        = (header >> 7) & 0x1;
    is_translated = (header >> 5) & 0x3;

    memset(c->layer[0], 0, PPM_PIXELS);
    memset(c->layer[1], 0, PPM_PIXELS);

    if (is_translated && p + 2 <= end) {
        translate_x = (int8_t)*p++;
        translate_y = (int8_t)*p++;
    }

    /* unpack line encodings for each layer */
    for (layer = 0; layer < 2; layer++) {
        int ptr = 0;
        memset(c->line_enc[layer], 0, PPM_H);
        while (ptr < PPM_H) {
            uint8_t byte = (p < end) ? *p++ : 0;
            if (byte == 0) { ptr += 4; continue; }
            c->line_enc[layer][ptr++] =  byte       & 0x03;
            if (ptr < PPM_H) c->line_enc[layer][ptr++] = (byte >> 2) & 0x03;
            if (ptr < PPM_H) c->line_enc[layer][ptr++] = (byte >> 4) & 0x03;
            if (ptr < PPM_H) c->line_enc[layer][ptr++] = (byte >> 6) & 0x03;
        }
    }

    /* unpack layer bitmaps */
    for (layer = 0; layer < 2; layer++) {
        uint8_t *pixel_buffer = c->layer[layer];
        for (y = 0; y < PPM_H; y++) {
            int base = y * PPM_W;
            int line_type = c->line_enc[layer][y];
            switch (line_type) {
            case 0:
                break;
            case 1:
            case 2: {
                uint32_t line_header;
                int ptr = base;
                if (line_type == 2)
                    memset(pixel_buffer + base, 1, PPM_W);
                if (p + 4 > end) break;
                line_header = AV_RB32(p); p += 4;
                for (; line_header != 0; line_header <<= 1, ptr += 8) {
                    if (line_header & 0x80000000u) {
                        uint8_t chunk = (p < end) ? *p++ : 0;
                        if (line_type == 1) {
                            int pixel;
                            for (pixel = 0; chunk != 0; pixel++, chunk >>= 1)
                                pixel_buffer[ptr + pixel] = chunk & 0x1;
                        } else {
                            int pixel;
                            for (pixel = 0; pixel < 8; pixel++, chunk >>= 1)
                                pixel_buffer[ptr + pixel] = chunk & 0x1;
                        }
                    }
                }
                break;
            }
            case 3: {
                int ptr = base, i2;
                uint8_t chunk = 0;
                for (i2 = 0; i2 < PPM_W; i2++) {
                    if (i2 % 8 == 0)
                        chunk = (p < end) ? *p++ : 0;
                    pixel_buffer[ptr++] = chunk & 0x1;
                    chunk >>= 1;
                }
                break;
            }
            }
        }
    }

    /* inter-frame diffing (XOR with previous frame) */
    if (!is_key && translate_x == 0 && translate_y == 0) {
        for (i = 0; i < PPM_PIXELS; i++) {
            c->layer[0][i] ^= c->prev[0][i];
            c->layer[1][i] ^= c->prev[1][i];
        }
    } else if (!is_key) {
        int start_x = FFMAX(translate_x, 0);
        int start_y = FFMAX(translate_y, 0);
        int end_x   = FFMIN(PPM_W + translate_x, PPM_W);
        int end_y   = FFMIN(PPM_H + translate_y, PPM_H);
        int shift   = translate_y * PPM_W + translate_x;
        int xx, yy;
        for (yy = start_y; yy < end_y; yy++) {
            for (xx = start_x; xx < end_x; xx++) {
                int d = yy * PPM_W + xx;
                int sidx = d - shift;
                c->layer[0][d] ^= c->prev[0][sidx];
                c->layer[1][d] ^= c->prev[1][sidx];
            }
        }
    }
    memcpy(c->prev[0], c->layer[0], PPM_PIXELS);
    memcpy(c->prev[1], c->layer[1], PPM_PIXELS);

    /* composite to RGB24 */
    frame_palette(c, c->frame, pal);
    if ((ret = av_new_packet(pkt, PPM_PIXELS * 3)) < 0)
        return ret;
    dst = pkt->data;
    {
        const uint8_t *paper = PPM_RGB[pal[0]];
        for (i = 0; i < PPM_PIXELS; i++) {
            dst[i*3+0] = paper[0];
            dst[i*3+1] = paper[1];
            dst[i*3+2] = paper[2];
        }
    }
    /* draw order: layer 2 (buffer 1) first, then layer 1 (buffer 0) on top */
    {
        static const int order[2] = { 1, 0 };
        int o;
        for (o = 0; o < 2; o++) {
            int l = order[o];
            const uint8_t *buf = c->layer[l];
            const uint8_t *col = PPM_RGB[pal[l + 1]];
            for (i = 0; i < PPM_PIXELS; i++) {
                if (!buf[i]) continue;
                dst[i*3+0] = col[0];
                dst[i*3+1] = col[1];
                dst[i*3+2] = col[2];
            }
        }
    }

    pkt->stream_index = 0;
    pkt->pts = c->frame;
    pkt->dts = c->frame;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    if (c->has_audio)
        c->pending_audio = 1;   /* audio chunk emitted on next call, then frame++ */
    else
        c->frame++;
    return 0;
}

static av_cold int ppm_read_close(AVFormatContext *s)
{
    PpmDemuxContext *c = s->priv_data;
    av_freep(&c->buf);
    av_freep(&c->frame_offsets);
    av_freep(&c->layer[0]);
    av_freep(&c->layer[1]);
    av_freep(&c->prev[0]);
    av_freep(&c->prev[1]);
    av_freep(&c->audio);
    return 0;
}

const FFInputFormat ff_flipnote_ppm_demuxer = {
    .p.name         = "flipnote_ppm",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Flipnote Studio (PPM)"),
    .p.extensions   = "ppm",
    .priv_data_size = sizeof(PpmDemuxContext),
    .read_probe     = ppm_probe,
    .read_header    = ppm_read_header,
    .read_packet    = ppm_read_packet,
    .read_close     = ppm_read_close,
};
