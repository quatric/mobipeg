/*
 * Flipnote Studio 3D (KWZ) demuxer
 * Copyright (c) 2026
 *
 * Decodes the video of Nintendo Flipnote Studio 3D `.kwz` animation files to
 * RGB24 rawvideo. Ported from flipnote.js (James Daniel / Flipnote Collective),
 * KWZ format docs:
 *   https://github.com/Flipnote-Collective/flipnote-studio-3d-docs/wiki/KWZ-Format
 *
 * Decode only. Audio (ADPCM sound tracks) is not exposed here.
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

#define KWZ_W 320
#define KWZ_H 240
#define KWZ_PIXELS (KWZ_W * KWZ_H)

#define KWZ_RAW_SAMPLE_RATE 16364
#define KWZ_SAMPLE_RATE     32768

static const int8_t ADPCM_INDEX_TABLE_2BIT[4] = { -1, 2, -1, 2 };
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

/* framerates indexed by in-app frame speed */
static const float KWZ_FRAMERATES[11] = {
    .2f, .5f, 1, 2, 4, 6, 8, 12, 20, 24, 30
};

/* palette: WHITE, BLACK, RED, YELLOW, GREEN, BLUE, NONE(transparent) */
static const uint8_t KWZ_RGB[7][3] = {
    { 0xff, 0xff, 0xff }, /* WHITE  */
    { 0x10, 0x10, 0x10 }, /* BLACK  */
    { 0xff, 0x10, 0x10 }, /* RED    */
    { 0xff, 0xe7, 0x00 }, /* YELLOW */
    { 0x00, 0x86, 0x31 }, /* GREEN  */
    { 0x00, 0x38, 0xce }, /* BLUE   */
    { 0xff, 0xff, 0xff }, /* NONE (never composited) */
};

/* indices into KWZ_LINE_TABLE for the 32 common lines */
static const uint16_t KWZ_COMMON_IDX[32] = {
    0x0000, 0x0CD0, 0x19A0, 0x02D9, 0x088B, 0x0051, 0x00F3, 0x0009,
    0x001B, 0x0001, 0x0003, 0x05B2, 0x1116, 0x00A2, 0x01E6, 0x0012,
    0x0036, 0x0002, 0x0006, 0x0B64, 0x08DC, 0x0144, 0x00FC, 0x0024,
    0x001C, 0x0004, 0x0334, 0x099C, 0x0668, 0x1338, 0x1004, 0x166C
};

typedef struct KwzDemuxContext {
    uint8_t *buf;
    int64_t  size;

    int      frame_count;
    int      frame;
    AVRational fps;

    uint32_t *meta_off;   /* per frame: offset of KMI entry (flags u32) */
    uint32_t *data_off;   /* per frame: offset of layer data in KMC */
    uint16_t (*layer_sz)[3];

    /* line tables */
    uint8_t *line;        /* 6561 * 8 */
    uint8_t *line_shift;  /* 6561 * 8 */
    uint8_t common[32 * 8];
    uint8_t common_shift[32 * 8];

    /* persistent layer buffers (values 0/1/2) */
    uint8_t *layer[3];

    /* bit reader state */
    const uint8_t *bp;
    const uint8_t *bend;
    uint32_t bit_value;
    int      bit_index;

    /* audio */
    int      has_audio;
    int      audio_stream_index;
    int16_t *audio;        /* mixed mono master PCM at KWZ_SAMPLE_RATE */
    int      audio_len;    /* samples */
    double   samples_per_frame;
    int      pending_audio;
} KwzDemuxContext;

static int kwz_probe(const AVProbeData *p)
{
    if (p->buf_size < 8)
        return 0;
    /* "KFH" (kwz) or "KIC" (folder icon) */
    if ((p->buf[0] == 'K' && p->buf[1] == 'F' && p->buf[2] == 'H') ||
        (p->buf[0] == 'K' && p->buf[1] == 'I' && p->buf[2] == 'C'))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static void build_line_tables(KwzDemuxContext *c)
{
    int a, b, d, e, f, g, h, i, off = 0;
    for (a = 0; a < 3; a++)
    for (b = 0; b < 3; b++)
    for (d = 0; d < 3; d++)
    for (e = 0; e < 3; e++)
    for (f = 0; f < 3; f++)
    for (g = 0; g < 3; g++)
    for (h = 0; h < 3; h++)
    for (i = 0; i < 3; i++) {
        /* variables named a..h in flipnote.js; loop order abcdefgh -> here a,b,d,e,f,g,h,i */
        uint8_t A = a, B = b, C = d, D = e, E = f, F = g, G = h, H = i;
        uint8_t *l  = c->line       + off;
        uint8_t *ls = c->line_shift + off;
        l[0]=B; l[1]=A; l[2]=D; l[3]=C; l[4]=F; l[5]=E; l[6]=H; l[7]=G;
        ls[0]=A; ls[1]=D; ls[2]=C; ls[3]=F; ls[4]=E; ls[5]=H; ls[6]=G; ls[7]=B;
        off += 8;
    }
    for (i = 0; i < 32; i++) {
        const uint8_t *sp = c->line       + KWZ_COMMON_IDX[i] * 8;
        const uint8_t *ss = c->line_shift + KWZ_COMMON_IDX[i] * 8;
        memcpy(c->common       + i * 8, sp, 8);
        memcpy(c->common_shift + i * 8, ss, 8);
    }
}

/* find a section's data pointer (points just past the 8-byte header) and length */
static int find_section(KwzDemuxContext *c, const char *magic,
                        int64_t *data_ptr, uint32_t *length)
{
    int64_t ptr = 0;
    int count = 0;
    while (ptr + 8 <= c->size && count < 6) {
        uint32_t len = AV_RL32(c->buf + ptr + 4);
        if (c->buf[ptr] == magic[0] && c->buf[ptr+1] == magic[1] &&
            c->buf[ptr+2] == magic[2]) {
            if (data_ptr) *data_ptr = ptr + 8;
            if (length)   *length   = len;
            return 0;
        }
        ptr += (int64_t)len + 8;
        count++;
    }
    return AVERROR_INVALIDDATA;
}

static int kwz_build_audio(KwzDemuxContext *c, int frame_speed);

static av_cold int kwz_read_header(AVFormatContext *s)
{
    KwzDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st;
    int64_t kfh, kmi, kmc;
    uint32_t kmi_len, kmc_len;
    int64_t sz = avio_size(pb);
    int i, frame_speed, ret;
    int64_t meta_ptr, data_ptr;

    if (sz <= 0 || sz > INT_MAX)
        return AVERROR_INVALIDDATA;
    c->size = sz;
    c->buf = av_malloc(sz);
    if (!c->buf)
        return AVERROR(ENOMEM);
    if ((ret = avio_read(pb, c->buf, sz)) < 0)
        return ret;
    if (ret != sz)
        return AVERROR_INVALIDDATA;

    if (find_section(c, "KFH", &kfh, NULL) < 0)
        return AVERROR_INVALIDDATA;
    if (find_section(c, "KMI", &kmi, &kmi_len) < 0 ||
        find_section(c, "KMC", &kmc, &kmc_len) < 0)
        return AVERROR_INVALIDDATA;

    /* KFH data starts at kfh; frameCount u16 at data+0xC4, frameSpeed u8 at +0xCA */
    if (kfh + 0xCB > c->size)
        return AVERROR_INVALIDDATA;
    c->frame_count = AV_RL16(c->buf + kfh + 0xC4);
    frame_speed    = c->buf[kfh + 0xCA];
    if (frame_speed > 10)
        frame_speed = 10;
    if (c->frame_count <= 0)
        return AVERROR_INVALIDDATA;

    if ((int64_t)c->frame_count * 28 > kmi_len)
        return AVERROR_INVALIDDATA;

    c->meta_off  = av_malloc_array(c->frame_count, sizeof(*c->meta_off));
    c->data_off  = av_malloc_array(c->frame_count, sizeof(*c->data_off));
    c->layer_sz  = av_malloc_array(c->frame_count, sizeof(*c->layer_sz));
    if (!c->meta_off || !c->data_off || !c->layer_sz)
        return AVERROR(ENOMEM);

    meta_ptr = kmi;       /* KMI entries begin right after the 8-byte header */
    data_ptr = kmc + 4;   /* KMC frame data begins at section header +12 = data +4 */
    for (i = 0; i < c->frame_count; i++) {
        int64_t a;
        if (meta_ptr + 10 > c->size)
            return AVERROR_INVALIDDATA;
        c->layer_sz[i][0] = AV_RL16(c->buf + meta_ptr + 4);
        c->layer_sz[i][1] = AV_RL16(c->buf + meta_ptr + 6);
        c->layer_sz[i][2] = AV_RL16(c->buf + meta_ptr + 8);
        c->meta_off[i] = meta_ptr;
        c->data_off[i] = data_ptr;
        a = (int64_t)c->layer_sz[i][0] + c->layer_sz[i][1] + c->layer_sz[i][2];
        meta_ptr += 28;
        data_ptr += a;
        if (data_ptr > c->size)
            return AVERROR_INVALIDDATA;
    }

    c->line       = av_malloc(6561 * 8);
    c->line_shift = av_malloc(6561 * 8);
    for (i = 0; i < 3; i++)
        c->layer[i] = av_mallocz(KWZ_PIXELS);
    if (!c->line || !c->line_shift || !c->layer[0] || !c->layer[1] || !c->layer[2])
        return AVERROR(ENOMEM);
    build_line_tables(c);

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    c->fps = av_d2q(KWZ_FRAMERATES[frame_speed], INT_MAX);
    avpriv_set_pts_info(st, 64, c->fps.den, c->fps.num);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format     = AV_PIX_FMT_RGB24;
    st->codecpar->width      = KWZ_W;
    st->codecpar->height     = KWZ_H;
    st->nb_frames = st->duration = c->frame_count;

    /* audio: decode + mix all tracks to one mono master stream */
    if ((ret = kwz_build_audio(c, frame_speed)) < 0)
        return ret;
    if (c->audio && c->audio_len > 0) {
        AVStream *ast = avformat_new_stream(s, NULL);
        if (!ast)
            return AVERROR(ENOMEM);
        c->has_audio = 1;
        c->audio_stream_index = ast->index;
        avpriv_set_pts_info(ast, 64, 1, KWZ_SAMPLE_RATE);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
        ast->codecpar->format      = AV_SAMPLE_FMT_S16;
        ast->codecpar->sample_rate = KWZ_SAMPLE_RATE;
        ast->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        ast->duration              = c->audio_len;
    }
    return 0;
}

static av_always_inline unsigned read_bits(KwzDemuxContext *c, int num)
{
    unsigned res;
    if (c->bit_index + num > 16) {
        uint16_t nb = (c->bp + 1 < c->bend) ? AV_RL16(c->bp) : 0;
        c->bp += 2;
        c->bit_value |= (uint32_t)nb << (16 - c->bit_index);
        c->bit_index -= 16;
    }
    res = c->bit_value & ((1u << num) - 1);
    c->bit_value >>= num;
    c->bit_index += num;
    return res;
}

static void put_line(uint8_t *dst, const uint8_t *pix)
{
    memcpy(dst, pix, 8);
}

static void decode_layer(KwzDemuxContext *c, int layer_index, int frame_index)
{
    uint8_t *pixel_buffer = c->layer[layer_index];
    const uint8_t *base = c->buf + c->data_off[frame_index];
    uint16_t layer_size;
    int skip = 0;
    int tox, toy, sx, sy, x, y;
    int off = 0, li;

    /* seek to start of this layer's data */
    for (li = 0; li < layer_index; li++)
        off += c->layer_sz[frame_index][li];
    layer_size = c->layer_sz[frame_index][layer_index];

    /* 38 bytes => layer unchanged since previous frame, keep buffer */
    if (layer_size == 38)
        return;

    c->bp        = base + off;
    c->bend      = base + off + layer_size;
    c->bit_index = 16;
    c->bit_value = 0;

    for (toy = 0; toy < 240; toy += 128) {
        for (tox = 0; tox < 320; tox += 128) {
            for (sy = 0; sy < 128; sy += 8) {
                y = toy + sy;
                if (y >= 240) break;
                for (sx = 0; sx < 128; sx += 8) {
                    int ptr;
                    int tile_type;
                    x = tox + sx;
                    if (x >= 320) break;
                    if (skip > 0) { skip--; continue; }
                    ptr = y * KWZ_W + x;
                    tile_type = read_bits(c, 3);

                    if (tile_type == 0) {
                        const uint8_t *px = c->common + read_bits(c, 5) * 8;
                        int k;
                        for (k = 0; k < 8; k++, ptr += KWZ_W)
                            put_line(pixel_buffer + ptr, px);
                    } else if (tile_type == 1) {
                        const uint8_t *px = c->line + read_bits(c, 13) * 8;
                        int k;
                        for (k = 0; k < 8; k++, ptr += KWZ_W)
                            put_line(pixel_buffer + ptr, px);
                    } else if (tile_type == 2) {
                        int lp = read_bits(c, 5) * 8;
                        const uint8_t *pa = c->common + lp;
                        const uint8_t *pb = c->common_shift + lp;
                        int k;
                        for (k = 0; k < 8; k++, ptr += KWZ_W)
                            put_line(pixel_buffer + ptr, (k & 1) ? pb : pa);
                    } else if (tile_type == 3) {
                        int lp = read_bits(c, 13) * 8;
                        const uint8_t *pa = c->line + lp;
                        const uint8_t *pb = c->line_shift + lp;
                        int k;
                        for (k = 0; k < 8; k++, ptr += KWZ_W)
                            put_line(pixel_buffer + ptr, (k & 1) ? pb : pa);
                    } else if (tile_type == 4) {
                        int flags = read_bits(c, 8);
                        int mask;
                        for (mask = 1; mask < 0xFF; mask <<= 1) {
                            const uint8_t *px;
                            if (flags & mask)
                                px = c->common + read_bits(c, 5) * 8;
                            else
                                px = c->line + read_bits(c, 13) * 8;
                            put_line(pixel_buffer + ptr, px);
                            ptr += KWZ_W;
                        }
                    } else if (tile_type == 5) {
                        skip = read_bits(c, 5);
                        continue;
                    } else if (tile_type == 7) {
                        int pattern = read_bits(c, 2);
                        int use_common = read_bits(c, 1);
                        const uint8_t *pa, *pb;
                        static const uint8_t seq[4][8] = {
                            { 0, 1, 0, 1, 0, 1, 0, 1 },
                            { 0, 0, 1, 0, 0, 1, 0, 0 },
                            { 0, 1, 0, 0, 1, 0, 0, 1 },
                            { 0, 1, 1, 0, 1, 1, 0, 1 },
                        };
                        const uint8_t *row;
                        int k;
                        if (use_common) {
                            int la = read_bits(c, 5) * 8;
                            int lb = read_bits(c, 5) * 8;
                            pa = c->common + la;
                            pb = c->common + lb;
                            pattern += 1;
                        } else {
                            int la = read_bits(c, 13) * 8;
                            int lb = read_bits(c, 13) * 8;
                            pa = c->line + la;
                            pb = c->line + lb;
                        }
                        row = seq[pattern & 3];
                        for (k = 0; k < 8; k++, ptr += KWZ_W)
                            put_line(pixel_buffer + ptr, row[k] ? pb : pa);
                    }
                    /* tile_type 6 does not exist */
                }
            }
        }
    }
}

/* Decode a KWZ ADPCM track (variable 2-bit / 4-bit IMA). Returns malloc'd
 * Int16 PCM, *out_len samples. */
static int16_t *kwz_decode_track(const uint8_t *src, int src_size, int *out_len)
{
    int16_t *dst;
    int predictor = 0, step_index = 40, dst_ptr = 0, i;

    *out_len = 0;
    if (src_size <= 0)
        return NULL;
    dst = av_malloc_array((size_t)src_size * 8, sizeof(*dst));
    if (!dst)
        return NULL;

    for (i = 0; i < src_size; i++) {
        int cur_byte = src[i];
        int cur_bit = 0;
        while (cur_bit < 8) {
            int sample, step, diff;
            if (step_index < 18 || cur_bit > 4) {
                /* 2-bit sample */
                sample = cur_byte & 0x3;
                step = ADPCM_STEP_TABLE[step_index];
                diff = step >> 3;
                if (sample & 1) diff += step;
                if (sample & 2) diff = -diff;
                predictor += diff;
                step_index += ADPCM_INDEX_TABLE_2BIT[sample];
                cur_byte >>= 2;
                cur_bit  += 2;
            } else {
                /* 4-bit sample */
                sample = cur_byte & 0xf;
                step = ADPCM_STEP_TABLE[step_index];
                diff = step >> 3;
                if (sample & 1) diff += step >> 2;
                if (sample & 2) diff += step >> 1;
                if (sample & 4) diff += step;
                if (sample & 8) diff = -diff;
                predictor += diff;
                step_index += ADPCM_INDEX_TABLE_4BIT[sample];
                cur_byte >>= 4;
                cur_bit  += 4;
            }
            step_index = av_clip(step_index, 0, 79);
            predictor  = av_clip(predictor, -2048, 2047);
            dst[dst_ptr++] = predictor * 16;
        }
    }
    *out_len = dst_ptr;
    return dst;
}

/* simple linear resample */
static int16_t *kwz_resample_linear(const int16_t *src, int src_len,
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
        double a = i * adj;
        int sp = (int)a;
        double w = a - sp;
        int s0 = (sp     >= 0 && sp     < src_len) ? src[sp]     : 0;
        int s1 = (sp + 1 >= 0 && sp + 1 < src_len) ? src[sp + 1] : 0;
        dst[i] = (int16_t)lrint(s0 + w * (s1 - s0));
    }
    *out_len = dst_len;
    return dst;
}

static void kwz_mix_full(int16_t *dst, int dst_len,
                        const int16_t *src, int src_len, int off)
{
    int n;
    for (n = 0; n < src_len; n++) {
        int idx = off + n;
        if (idx >= dst_len)
            break;
        dst[idx] = av_clip_int16(dst[idx] + src[n]);
    }
}

static int kwz_build_audio(KwzDemuxContext *c, int frame_speed)
{
    const int dst_freq = KWZ_SAMPLE_RATE;
    double framerate = KWZ_FRAMERATES[frame_speed];
    double duration  = c->frame_count / framerate;
    int master_len   = (int)ceil(duration * dst_freq);
    int64_t ksn, base;
    uint32_t bgm_speed, track_len[5];
    int64_t track_ptr[5];
    int16_t *master;
    double bgmrate;
    int t, i;

    if (master_len <= 0)
        return 0;
    if (find_section(c, "KSN", &ksn, NULL) < 0)
        return 0;                       /* no sound section (comment/icon) */
    if (ksn + 28 > c->size)
        return 0;

    bgm_speed = AV_RL32(c->buf + ksn);
    if (bgm_speed > 10)
        bgm_speed = frame_speed;
    bgmrate = KWZ_FRAMERATES[bgm_speed];

    for (t = 0; t < 5; t++)
        track_len[t] = AV_RL32(c->buf + ksn + 4 + t * 4);
    base = ksn + 28;                    /* BGM data begins 28 bytes into KSN data */
    track_ptr[0] = base;
    for (t = 1; t < 5; t++)
        track_ptr[t] = track_ptr[t-1] + track_len[t-1];

    for (i = 0, t = 0; t < 5; t++) i += track_len[t];
    if (i == 0)
        return 0;

    master = av_calloc(master_len, sizeof(*master));
    if (!master)
        return AVERROR(ENOMEM);

    /* BGM (track 0) at offset 0 */
    if (track_len[0] > 0 && track_ptr[0] + track_len[0] <= c->size) {
        int raw_len, res_len;
        int16_t *raw = kwz_decode_track(c->buf + track_ptr[0], track_len[0], &raw_len);
        if (raw) {
            double src_freq = KWZ_RAW_SAMPLE_RATE * (framerate / bgmrate);
            int16_t *r = kwz_resample_linear(raw, raw_len, src_freq, dst_freq, &res_len);
            av_free(raw);
            if (r) {
                kwz_mix_full(master, master_len, r, res_len, 0);
                av_free(r);
            }
        }
    }

    /* SE1..SE4 (tracks 1..4) at per-frame offsets when flagged */
    {
        int16_t *se[4] = { NULL, NULL, NULL, NULL };
        int se_len[4]  = { 0, 0, 0, 0 };
        double spf = (double)dst_freq / framerate;
        for (t = 1; t < 5; t++) {
            if (track_len[t] > 0 && track_ptr[t] + track_len[t] <= c->size) {
                int raw_len, res_len;
                int16_t *raw = kwz_decode_track(c->buf + track_ptr[t], track_len[t], &raw_len);
                if (raw) {
                    se[t-1] = kwz_resample_linear(raw, raw_len, KWZ_RAW_SAMPLE_RATE,
                                                  dst_freq, &res_len);
                    se_len[t-1] = res_len;
                    av_free(raw);
                }
            }
        }
        if (se[0] || se[1] || se[2] || se[3]) {
            for (i = 0; i < c->frame_count; i++) {
                int off = (int)ceil(i * spf);
                uint8_t flag = c->buf[c->meta_off[i] + 0x17];
                int k;
                for (k = 0; k < 4; k++)
                    if (se[k] && (flag & (1 << k)))
                        kwz_mix_full(master, master_len, se[k], se_len[k], off);
            }
        }
        for (t = 0; t < 4; t++)
            av_freep(&se[t]);
    }

    c->audio = master;
    c->audio_len = master_len;
    c->samples_per_frame = (double)dst_freq / framerate;
    return 0;
}

static int kwz_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    KwzDemuxContext *c = s->priv_data;
    uint32_t flags;
    int pal[7];
    int layer, i, ret;
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
    }

    if (c->frame >= c->frame_count)
        return AVERROR_EOF;

    for (layer = 0; layer < 3; layer++)
        decode_layer(c, layer, c->frame);

    /* per-frame palette indices from KMI flags */
    flags = AV_RL32(c->buf + c->meta_off[c->frame]);
    pal[0] =  flags        & 0xF;
    pal[1] = (flags >> 8)  & 0xF;
    pal[2] = (flags >> 12) & 0xF;
    pal[3] = (flags >> 16) & 0xF;
    pal[4] = (flags >> 20) & 0xF;
    pal[5] = (flags >> 24) & 0xF;
    pal[6] = (flags >> 28) & 0xF;
    for (i = 0; i < 7; i++)
        if (pal[i] > 6) pal[i] = 6;

    if ((ret = av_new_packet(pkt, KWZ_PIXELS * 3)) < 0)
        return ret;
    dst = pkt->data;

    /* fill with paper color */
    {
        const uint8_t *paper = KWZ_RGB[pal[0]];
        for (i = 0; i < KWZ_PIXELS; i++) {
            dst[i*3+0] = paper[0];
            dst[i*3+1] = paper[1];
            dst[i*3+2] = paper[2];
        }
    }
    /* composite back-to-front: layer 2, 1, 0 */
    {
        static const int order[3] = { 2, 1, 0 };
        int o;
        for (o = 0; o < 3; o++) {
            int l = order[o];
            const uint8_t *buf = c->layer[l];
            for (i = 0; i < KWZ_PIXELS; i++) {
                int v = buf[i];
                int ci;
                if (!v) continue;
                ci = pal[1 + l*2 + (v - 1)];
                if (ci == 6) continue;   /* NONE = transparent */
                dst[i*3+0] = KWZ_RGB[ci][0];
                dst[i*3+1] = KWZ_RGB[ci][1];
                dst[i*3+2] = KWZ_RGB[ci][2];
            }
        }
    }

    pkt->stream_index = 0;
    pkt->pts = c->frame;
    pkt->dts = c->frame;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    if (c->has_audio)
        c->pending_audio = 1;
    else
        c->frame++;
    return 0;
}

static av_cold int kwz_read_close(AVFormatContext *s)
{
    KwzDemuxContext *c = s->priv_data;
    int i;
    av_freep(&c->buf);
    av_freep(&c->meta_off);
    av_freep(&c->data_off);
    av_freep(&c->layer_sz);
    av_freep(&c->line);
    av_freep(&c->line_shift);
    for (i = 0; i < 3; i++)
        av_freep(&c->layer[i]);
    av_freep(&c->audio);
    return 0;
}

const FFInputFormat ff_kwz_demuxer = {
    .p.name         = "kwz",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Flipnote Studio 3D (KWZ)"),
    .p.extensions   = "kwz",
    .priv_data_size = sizeof(KwzDemuxContext),
    .read_probe     = kwz_probe,
    .read_header    = kwz_read_header,
    .read_packet    = kwz_read_packet,
    .read_close     = kwz_read_close,
};
