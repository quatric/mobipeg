/*
 * Caimans 2.2 GBA Video decoders
 * Copyright (c) 2026 the FFmpeg developers
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"

#include "adpcm_data.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define WIDTH       240
#define HEIGHT      160
#define STRIDE      240
#define DISPLAY_Y   28
#define CELL_SIZE   36
#define CACHE_SIZE  (256 * CELL_SIZE)

typedef struct Caimans22VideoContext {
    uint8_t simple[CACHE_SIZE];
    uint8_t complex[CACHE_SIZE];
    uint8_t table[2][512];
    uint16_t framebuffer[WIDTH * HEIGHT];
} Caimans22VideoContext;

static int video_init(AVCodecContext *avctx)
{
    Caimans22VideoContext *s = avctx->priv_data;
    static const int threshold[2] = { 2, 5 };

    avctx->pix_fmt = AV_PIX_FMT_RGB555LE;
    for (int t = 0; t < 2; t++) {
        for (int value = -128; value < 384; value++) {
            int q;
            if (value <= 0)
                q = 0;
            else if (value > 247)
                q = 31;
            else {
                int base = value & ~7;
                q = value - base <= threshold[t] ? base >> 3 : (base + 8) >> 3;
            }
            s->table[t][value + 128] = q;
        }
    }
    return 0;
}

static uint16_t make_pixel(const uint8_t *table, int idx, int d1, int d2)
{
    int dr = 2 * d2;
    int dg = -d2 - ((d1 + 1) >> 1);
    int db = 2 * d1;
    return table[128 + idx + dr] |
           table[128 + idx + dg] << 5 |
           table[128 + idx + db] << 10;
}

static int build_cell(Caimans22VideoContext *s, uint8_t *cell,
                      const uint8_t *src, int complex)
{
    uint16_t pair[4][2];
    int d1 = (int8_t)src[4], d2 = (int8_t)src[5];

    for (int i = 0; i < 4; i++) {
        pair[i][0] = make_pixel(s->table[0], src[i], d1, d2);
        pair[i][1] = make_pixel(s->table[1], src[i], d1, d2);
    }
    memset(cell, 0, CELL_SIZE);
    memcpy(cell + 16, src, 6);
    if (complex) {
        AV_WL16(cell +  0, pair[0][0]);
        AV_WL16(cell +  6, pair[1][1]);
        AV_WL16(cell + 10, pair[2][1]);
        AV_WL16(cell + 12, pair[3][0]);
    } else {
        for (int i = 0; i < 4; i++) {
            AV_WL16(cell + i * 4,     pair[i][0]);
            AV_WL16(cell + i * 4 + 2, pair[i][1]);
        }
    }
    return 0;
}

static int decode_codebook(Caimans22VideoContext *s, unsigned tag,
                           const uint8_t *buf, int size)
{
    uint8_t *cache = tag < 0x2200 ? s->complex : s->simple;
    int complex = tag < 0x2200, pos = 0, slot = 0;

    if (tag == 0x2000 || tag == 0x2200) {
        if (size % 6 || size / 6 > 256)
            return AVERROR_INVALIDDATA;
        while (pos < size) {
            build_cell(s, cache + slot++ * CELL_SIZE, buf + pos, complex);
            pos += 6;
        }
        return 0;
    }
    while (pos < size) {
        uint32_t mask;
        if (pos + 4 > size || slot + 32 > 256)
            return AVERROR_INVALIDDATA;
        mask = AV_RL32(buf + pos);
        pos += 4;
        for (int bit = 0; bit < 32; bit++, slot++) {
            if (mask & (0x80000000U >> bit)) {
                if (pos + 6 > size)
                    return AVERROR_INVALIDDATA;
                build_cell(s, cache + slot * CELL_SIZE, buf + pos, complex);
                pos += 6;
            }
        }
    }
    return 0;
}

static void blit_simple(Caimans22VideoContext *s, int x, int y, int idx)
{
    const uint8_t *c = s->simple + idx * CELL_SIZE;
    uint16_t *d = s->framebuffer + y * STRIDE + x;
    for (int half = 0; half < 2; half++) {
        uint16_t a = AV_RL16(c + half * 8), b = AV_RL16(c + half * 8 + 2);
        uint16_t e = AV_RL16(c + half * 8 + 4), f = AV_RL16(c + half * 8 + 6);
        d[0] = a; d[1] = b; d[2] = e; d[3] = f;
        d += STRIDE;
        d[0] = b; d[1] = a; d[2] = f; d[3] = e;
        d += STRIDE;
    }
}

static void blit_complex(Caimans22VideoContext *s, int x, int y, int ia, int ib)
{
    const uint8_t *a = s->complex + ia * CELL_SIZE;
    const uint8_t *b = s->complex + ib * CELL_SIZE;
    uint16_t *d = s->framebuffer + y * STRIDE + x;
    d[0] = AV_RL16(a);      d[1] = AV_RL16(a + 6);
    d[2] = AV_RL16(b);      d[3] = AV_RL16(b + 6);
    d += STRIDE;
    d[0] = AV_RL16(a + 10); d[1] = AV_RL16(a + 12);
    d[2] = AV_RL16(b + 10); d[3] = AV_RL16(b + 12);
}

static int decode_raster_3000(Caimans22VideoContext *s, const uint8_t *buf,
                              int size, int coded_height)
{
    int pos = 0, x = 0, y = DISPLAY_Y;
    while (pos + 4 <= size && y < DISPLAY_Y + coded_height) {
        uint32_t mask = AV_RL32(buf + pos);
        pos += 4;
        for (int bit = 0; bit < 32 && y < DISPLAY_Y + coded_height; bit++) {
            if (mask & (0x80000000U >> bit)) {
                if (pos + 4 > size) return AVERROR_INVALIDDATA;
                blit_complex(s, x, y, buf[pos], buf[pos + 1]);
                blit_complex(s, x, y + 2, buf[pos + 2], buf[pos + 3]);
                pos += 4;
            } else {
                if (pos >= size) return AVERROR_INVALIDDATA;
                blit_simple(s, x, y, buf[pos++]);
            }
            if ((x += 4) >= WIDTH) { x = 0; y += 4; }
        }
    }
    return 0;
}

static int decode_raster_3100(Caimans22VideoContext *s, const uint8_t *buf,
                              int size, int coded_height)
{
    int pos = 0, x = 0, y = DISPLAY_Y;
    while (pos + 4 <= size && y < DISPLAY_Y + coded_height) {
        uint32_t mask = AV_RL32(buf + pos), sel = 0x80000000U;
        pos += 4;
        while (sel && y < DISPLAY_Y + coded_height) {
            if (mask & sel) {
                if (sel == 1) {
                    if (pos + 4 > size) return AVERROR_INVALIDDATA;
                    mask = AV_RL32(buf + pos); pos += 4; sel = 0x80000000U;
                } else {
                    sel >>= 1;
                }
                if (!(mask & sel)) {
                    if (pos >= size) return AVERROR_INVALIDDATA;
                    blit_simple(s, x, y, buf[pos++]);
                } else {
                    if (pos + 4 > size) return AVERROR_INVALIDDATA;
                    blit_complex(s, x, y, buf[pos], buf[pos + 1]);
                    blit_complex(s, x, y + 2, buf[pos + 2], buf[pos + 3]);
                    pos += 4;
                }
            }
            sel >>= 1;
            if ((x += 4) >= WIDTH) { x = 0; y += 4; }
        }
    }
    return 0;
}

static int decode_raster_3200(Caimans22VideoContext *s, const uint8_t *buf,
                              int size, int coded_height)
{
    int pos = 0, x = 0, y = DISPLAY_Y;
    while (pos < size && y < DISPLAY_Y + coded_height) {
        blit_simple(s, x, y, buf[pos++]);
        if ((x += 4) >= WIDTH) { x = 0; y += 4; }
    }
    return 0;
}

static int video_decode(AVCodecContext *avctx, AVFrame *frame,
                        int *got_frame, AVPacket *avpkt)
{
    Caimans22VideoContext *s = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int size = avpkt->size, pos = 22, end, coded_height, ret;

    if (size < 22 || AV_RB24(buf + 1) != size)
        return AVERROR_INVALIDDATA;
    coded_height = AV_RL16(buf + 18);
    end = pos + AV_RL16(buf + 12) - 12;
    if (coded_height > HEIGHT || end > size)
        return AVERROR_INVALIDDATA;
    if (avpkt->flags & AV_PKT_FLAG_KEY) {
        memset(s->simple, 0, sizeof(s->simple));
        memset(s->complex, 0, sizeof(s->complex));
        memset(s->framebuffer, 0, sizeof(s->framebuffer));
    }
    while (pos < end) {
        unsigned tag;
        int len;
        if (pos + 4 > end) return AVERROR_INVALIDDATA;
        tag = AV_RL16(buf + pos); len = AV_RL16(buf + pos + 2);
        if (len < 4 || pos + len > end) return AVERROR_INVALIDDATA;
        if (tag >= 0x2000 && tag <= 0x2300 && !(tag & 0xff))
            ret = decode_codebook(s, tag, buf + pos + 4, len - 4);
        else if (tag == 0x3000 || tag == 0x8000)
            ret = decode_raster_3000(s, buf + pos + 4, len - 4, coded_height);
        else if (tag == 0x3100 || tag == 0x8100)
            ret = decode_raster_3100(s, buf + pos + 4, len - 4, coded_height);
        else if (tag == 0x3200 || tag == 0x8200)
            ret = decode_raster_3200(s, buf + pos + 4, len - 4, coded_height);
        else
            ret = 0;
        if (ret < 0) return ret;
        pos += len;
    }
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int y = 0; y < HEIGHT; y++)
        memcpy(frame->data[0] + y * frame->linesize[0],
               s->framebuffer + y * WIDTH, WIDTH * 2);
    frame->pict_type = avpkt->flags & AV_PKT_FLAG_KEY ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    frame->flags |= avpkt->flags & AV_PKT_FLAG_KEY ? AV_FRAME_FLAG_KEY : 0;
    *got_frame = 1;
    return size;
}

typedef struct Caimans22AudioContext {
    int predictor, step_index;
} Caimans22AudioContext;

static int audio_decode(AVCodecContext *avctx, AVFrame *frame,
                        int *got_frame, AVPacket *avpkt)
{
    Caimans22AudioContext *s = avctx->priv_data;
    static const int8_t index_table[8] = { -1, -1, -1, -1, 2, 4, 7, 12 };
    int samples = avpkt->size * 2, ret;

    if (avpkt->duration > 0)
        samples = FFMIN(samples, avpkt->duration);
    frame->nb_samples = samples;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int i = 0; i < samples; i++) {
        int nibble = i & 1 ? avpkt->data[i >> 1] >> 4 : avpkt->data[i >> 1] & 15;
        int mag = nibble & 7, step, diff;
        s->step_index = av_clip(s->step_index, 0, 88);
        step = ff_adpcm_step_table[s->step_index];
        diff = step >> 3;
        if (mag & 1) diff += step >> 2;
        if (mag & 2) diff += step >> 1;
        if (mag & 4) diff += step;
        if (mag == 7) diff += step >> 1;
        if (nibble & 8) diff = -diff;
        s->predictor = av_clip(s->predictor + diff, -32768, 32767);
        s->step_index += index_table[mag];
        frame->data[0][i] = (s->predictor >> 8) + 128;
    }
    *got_frame = 1;
    return avpkt->size;
}

static int audio_init(AVCodecContext *avctx)
{
    avctx->sample_fmt = AV_SAMPLE_FMT_U8;
    return 0;
}

const FFCodec ff_caimans22_decoder = {
    .p.name         = "caimans22",
    CODEC_LONG_NAME("Caimans 2.2 GBA Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_CAIMANS22,
    .priv_data_size = sizeof(Caimans22VideoContext),
    .init           = video_init,
    FF_CODEC_DECODE_CB(video_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    CODEC_PIXFMTS(AV_PIX_FMT_RGB555LE),
};

const FFCodec ff_caimans22_audio_decoder = {
    .p.name         = "caimans22_audio",
    CODEC_LONG_NAME("Caimans 2.2 modified IMA ADPCM"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_CAIMANS22_AUDIO,
    .priv_data_size = sizeof(Caimans22AudioContext),
    .init           = audio_init,
    FF_CODEC_DECODE_CB(audio_decode),
    .p.capabilities = AV_CODEC_CAP_DR1,
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_U8),
};
