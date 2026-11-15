/*
 * RocketVideo (.rvid) video codec (encoder + decoder)
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
 * RocketVideo frames as used by RocketRobz's Vid2RVID / RocketVideoPlayer.
 * A frame is a DS bitmap: bmpMode 1 = 16bpp RGB555, bmpMode 2 = 16bpp RGB565
 * (packed the DS way, 6-bit green split across bit15 and bits5..9), bmpMode 0 =
 * 8bpp with a per-frame 256-entry 565-packed palette.  Frames may be Nintendo
 * LZ10 compressed and/or interlaced (each stored frame is one field; the player
 * keeps a persistent image and updates alternate scanlines).
 *
 * Container framing lives in the (de)muxer; here a packet is exactly one stored
 * frame, or for a dual screen video the top screen's size as a 32 bit word
 * and then the two screens' stored frames.  Mode/interlace/compression flags
 * travel in a 4-byte extradata blob shared with the (de)muxer:
 * [bmpMode, interlaced, compressed, dualScreen].  In a compressed file a frame
 * whose stored size is that of a raw frame is a raw frame.
 */
#include "libavutil/intreadwrite.h"
#include "libavutil/imgutils.h"
#include "libavutil/lfg.h"
#include "libavutil/mem.h"
#include "libavutil/nintendo_lz.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "elbg.h"

#define RVID_BMP_8BPP  0
#define RVID_BMP_555   1
#define RVID_BMP_565   2
#define RVID_PAL_BYTES 0x200      /* 256 * u16 palette that precedes 8bpp data */

/* --------------------------------------------------------------------------
 * Pixel (un)packing
 * ------------------------------------------------------------------------ */
static av_always_inline void unpack_word(unsigned w, int bmp_mode, uint8_t *rgb)
{
    unsigned r5 = w & 0x1F, b5 = (w >> 10) & 0x1F, g;
    if (bmp_mode == RVID_BMP_555) {
        g = (w >> 5) & 0x1F;
        rgb[0] = (r5 << 3) | (r5 >> 2);
        rgb[1] = (g  << 3) | (g  >> 2);
        rgb[2] = (b5 << 3) | (b5 >> 2);
    } else {  /* 565-in-1555 (bmpMode 0 palette entries and bmpMode 2) */
        unsigned g6 = ((w >> 15) & 1) | (((w >> 5) & 0x1F) << 1);
        rgb[0] = (r5 << 3) | (r5 >> 2);
        rgb[1] = (g6 << 2) | (g6 >> 4);
        rgb[2] = (b5 << 3) | (b5 >> 2);
    }
}

static av_always_inline unsigned pack_word(int R, int G, int B, int bmp_mode)
{
    if (bmp_mode == RVID_BMP_555)
        return (R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | 0x8000;
    else {
        unsigned g6 = (G >> 2) & 0x3F;
        return (R >> 3) | ((B >> 3) << 10) |
               ((g6 & 1) << 15) | (((g6 >> 1) & 0x1F) << 5);
    }
}

/* --------------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------------ */
typedef struct RVIDDecCtx {
    int bmp_mode, interlaced, compressed;
    int vres, width, disp_h, screens;
    uint8_t *persist;         /* RGB24 display buffers for interlacing */
    uint8_t *scratch;         /* LZ10 decompress target */
    int frame_idx;
} RVIDDecCtx;

static av_cold int rvid_decode_init(AVCodecContext *avctx)
{
    RVIDDecCtx *c = avctx->priv_data;
    if (avctx->extradata_size < 3) {
        av_log(avctx, AV_LOG_ERROR, "rvid: missing extradata\n");
        return AVERROR_INVALIDDATA;
    }
    c->bmp_mode   = avctx->extradata[0];
    c->interlaced = avctx->extradata[1];
    c->compressed = avctx->extradata[2];
    c->screens    = avctx->extradata_size > 3 && avctx->extradata[3] == 1 ? 2 : 1;
    c->width  = avctx->width;
    if (avctx->height % c->screens)
        return AVERROR_INVALIDDATA;
    c->disp_h = avctx->height / c->screens;
    c->vres   = c->interlaced ? c->disp_h / 2 : c->disp_h;
    if ((c->width != 240 && c->width != 256) ||
        c->vres <= 0 || c->vres > 255 ||
        (c->interlaced && (c->disp_h & 1)) ||
        c->bmp_mode > RVID_BMP_565)
        return AVERROR_INVALIDDATA;
    avctx->pix_fmt = AV_PIX_FMT_RGB24;

    c->scratch = av_malloc((size_t)c->width * c->vres * 2);
    if (!c->scratch)
        return AVERROR(ENOMEM);
    if (c->interlaced) {
        c->persist = av_mallocz((size_t)c->width * c->disp_h * 3 * c->screens);
        if (!c->persist)
            return AVERROR(ENOMEM);
    }
    return 0;
}

/* One stored frame (a whole screen, or one field of it) into dst, a
 * width x disp_h RGB24 picture. */
static int rvid_decode_screen(RVIDDecCtx *c, const uint8_t *data, int size,
                              uint8_t *dst, ptrdiff_t linesize, uint8_t *persist)
{
    const int W = c->width, VR = c->vres;
    const int raw_size = W * VR * (c->bmp_mode == RVID_BMP_8BPP ? 1 : 2);
    const int parity = c->interlaced ? (c->frame_idx & 1) : 0;
    const int field_stride = W * 3;
    const uint8_t *px;         /* palette-index bytes or 16bpp words */
    const uint8_t *pal = NULL; /* 8bpp palette (raw, never compressed) */
    uint8_t fieldrow[256 * 3];
    int ret;

    if (c->bmp_mode == RVID_BMP_8BPP) {
        if (size < RVID_PAL_BYTES)
            return AVERROR_INVALIDDATA;
        pal   = data;
        data += RVID_PAL_BYTES;
        size -= RVID_PAL_BYTES;
    }
    /* A compressed file keeps a frame that LZ10 would not shrink as it is;
     * the player tells the two apart by the stored size alone. */
    if (c->compressed && size != raw_size) {
        ret = avpriv_nintendo_lz10_decompress(data, size, c->scratch, raw_size);
        if (ret != raw_size)
            return ret < 0 ? ret : AVERROR_INVALIDDATA;
        px = c->scratch;
    } else {
        if (size < raw_size)
            return AVERROR_INVALIDDATA;
        px = data;
    }

    for (int y = 0; y < VR; y++) {
        if (c->bmp_mode == RVID_BMP_8BPP) {
            const uint8_t *row = px + (size_t)y * W;
            for (int x = 0; x < W; x++)
                unpack_word(AV_RL16(pal + row[x] * 2), RVID_BMP_565,
                            fieldrow + x * 3);
        } else {
            const uint8_t *row = px + (size_t)y * W * 2;
            for (int x = 0; x < W; x++)
                unpack_word(AV_RL16(row + x * 2), c->bmp_mode, fieldrow + x * 3);
        }
        if (c->interlaced)
            memcpy(persist + (size_t)(parity + 2 * y) * field_stride,
                   fieldrow, field_stride);
        else
            memcpy(dst + y * linesize, fieldrow, field_stride);
    }

    if (c->interlaced)
        for (int y = 0; y < c->disp_h; y++)
            memcpy(dst + y * linesize, persist + (size_t)y * field_stride,
                   field_stride);
    return 0;
}

static int rvid_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    RVIDDecCtx *c = avctx->priv_data;
    const uint8_t *data = avpkt->data;
    int size = avpkt->size, first = size, ret;

    /* A dual screen packet is the top screen's size, then both screens. */
    if (c->screens == 2) {
        if (size < 4 || AV_RL32(data) > size - 4)
            return AVERROR_INVALIDDATA;
        first = AV_RL32(data);
        data += 4;
        size -= 4;
    }

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int i = 0; i < c->screens; i++) {
        const size_t screen_size = (size_t)c->width * c->disp_h * 3;
        ret = rvid_decode_screen(c, i ? data + first : data,
                                 i ? size - first : first,
                                 frame->data[0] + i * c->disp_h * frame->linesize[0],
                                 frame->linesize[0],
                                 c->persist ? c->persist + i * screen_size : NULL);
        if (ret < 0)
            return ret;
    }

    c->frame_idx++;
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;
    return avpkt->size;
}

static av_cold int rvid_decode_close(AVCodecContext *avctx)
{
    RVIDDecCtx *c = avctx->priv_data;
    av_freep(&c->persist);
    av_freep(&c->scratch);
    return 0;
}

static void rvid_decode_flush(AVCodecContext *avctx)
{
    RVIDDecCtx *c = avctx->priv_data;

    c->frame_idx = 0;
    if (c->persist)
        memset(c->persist, 0, (size_t)c->width * c->disp_h * 3 * c->screens);
}

const FFCodec ff_rvid_decoder = {
    .p.name         = "rvid",
    CODEC_LONG_NAME("RocketVideo (RVID)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_RVID,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(RVIDDecCtx),
    .init           = rvid_decode_init,
    .close          = rvid_decode_close,
    .flush          = rvid_decode_flush,
    FF_CODEC_DECODE_CB(rvid_decode_frame),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};

/* --------------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------------ */
typedef struct RVIDEncCtx {
    const AVClass *class;
    int mode;         /* AVOption: 0=256(8bpp), 1=555, 2=565 */
    int interlaced;   /* AVOption */
    int compress;     /* AVOption */
    int dither;       /* AVOption */
    int dual_screen;  /* AVOption */
    int lazy;         /* AVOption */
    int vres, width, disp_h, screens;
    int fast_dither;  /* second dither pattern, used from 48 fps up */
    int frame_idx;
    struct ELBGContext *elbg;
    AVLFG lfg;
    uint8_t *payload;   /* packed field payload (indices or 16bpp words)   */
    uint8_t *lzbuf;     /* LZ10 scratch                                    */
    int     *points;    /* ELBG input (npix*3)                             */
    int     *closest;   /* ELBG output index per pixel                     */
    int      codebook[256 * 3];
    uint8_t  *pal_index;  /* palette index of a 16 bit colour ...           */
    uint32_t *pal_stamp;  /* ... valid when stamped with the current screen */
    uint32_t  stamp;
} RVIDEncCtx;

/* What Vid2RVID settles from the frame rate, because the player cannot keep
 * up otherwise: above a limit a video is stored as fields, and above another
 * its frames are left uncompressed.  fps is the whole rate (24 for 23.976),
 * height that of one screen. */
static void rvid_player_limits(void *log, int fps, int height, int dual,
                               int deep, int gba, int *interlaced, int *compress)
{
    const int low_height = gba ? 100 : 108;
    int progressive_limit = gba ? 30 : 72;
    int compress_limit    = gba ? 15 : 50;

    if (dual) progressive_limit /= 2;
    if (deep) progressive_limit /= 2;
    if (*interlaced < 0)
        *interlaced = fps > progressive_limit && !(height & 1);

    if (dual) compress_limit /= 2;
    if (deep) compress_limit /= 2;
    if (height <= low_height) {
        compress_limit *= 2;
    } else if (!gba && height > 144) {
        if (dual) compress_limit /= 2;
        if (deep) compress_limit /= 2;
    }
    if (*interlaced)
        compress_limit *= 2;
    if (height > low_height && compress_limit > 48)
        compress_limit = 48;
    if (*compress < 0)
        *compress = fps <= compress_limit;
    else if (*compress && fps > compress_limit)
        av_log(log, AV_LOG_WARNING,
               "rvid: Vid2RVID leaves frames uncompressed above %d fps at this size; "
               "the player may not keep up\n", compress_limit);
}

static av_cold int rvid_encode_init(AVCodecContext *avctx)
{
    RVIDEncCtx *c = avctx->priv_data;
    int bmp_mode = c->mode;
    int fps = 0;
    size_t npix;

    if (avctx->width != 256 && avctx->width != 240) {
        av_log(avctx, AV_LOG_ERROR,
               "rvid: width must be 256 (DS) or 240 (GBA); scale first.\n");
        return AVERROR(EINVAL);
    }
    c->screens = c->dual_screen ? 2 : 1;
    if (c->dual_screen && (avctx->width != 256 || (avctx->height & 1))) {
        av_log(avctx, AV_LOG_ERROR,
               "rvid: a dual screen video is 256 wide, the top screen above the bottom one\n");
        return AVERROR(EINVAL);
    }
    c->width  = avctx->width;
    c->disp_h = avctx->height / c->screens;
    if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
        fps = lrint(av_q2d(avctx->framerate));
    rvid_player_limits(avctx, fps, c->disp_h, c->dual_screen, bmp_mode != RVID_BMP_8BPP,
                       c->width == 240, &c->interlaced, &c->compress);
    if (c->interlaced && (c->disp_h & 1)) {
        av_log(avctx, AV_LOG_ERROR, "rvid: interlaced needs even height\n");
        return AVERROR(EINVAL);
    }
    c->vres   = c->interlaced ? c->disp_h / 2 : c->disp_h;
    c->fast_dither = !c->interlaced && fps >= 48;
    if (c->vres < 1 || c->vres > 255) {
        av_log(avctx, AV_LOG_ERROR, "rvid: vRes %d out of range 1..255\n", c->vres);
        return AVERROR(EINVAL);
    }
    npix = (size_t)c->width * c->vres;

    c->payload = av_malloc(npix * 2);
    c->lzbuf   = av_malloc(npix * 2 + npix / 4 + 64);
    if (!c->payload || !c->lzbuf)
        return AVERROR(ENOMEM);
    if (bmp_mode == RVID_BMP_8BPP) {
        c->points  = av_malloc(npix * 3 * sizeof(int));
        c->closest = av_malloc(npix * sizeof(int));
        c->pal_index = av_malloc(0x10000);
        c->pal_stamp = av_calloc(0x10000, sizeof(*c->pal_stamp));
        c->stamp = 1;
        if (!c->points || !c->closest || !c->pal_index || !c->pal_stamp)
            return AVERROR(ENOMEM);
        av_lfg_init(&c->lfg, 1);
    }

    avctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = 4;
    avctx->extradata[0] = bmp_mode;
    avctx->extradata[1] = c->interlaced ? 1 : 0;
    avctx->extradata[2] = c->compress ? 1 : 0;
    avctx->extradata[3] = c->dual_screen ? 1 : 0;
    return 0;
}

/* Vid2RVID's ordered dither.  A checkerboard that swaps with every frame adds
 * half a step of the 16 bit colour (4, or 2 for a six bit green) to a channel
 * unless that would leave the range; from 48 fps up a second, fixed
 * checkerboard adds a quarter step on top.  Interlaced videos take their rows
 * from the full picture, so the pattern follows the picture row there. */
static av_always_inline int dither_ch(int v, int a1, int a2, int delta)
{
    const int old = v;

    if (a1 && old >= delta && old < 0x100 - delta)
        v += delta;
    if (a2 && old >= delta / 2 && v < 0x100 - delta / 2)
        v += delta / 2;
    return v;
}

/* One screen of the frame (or one field of it) into out; returns its size. */
static int rvid_encode_screen(RVIDEncCtx *c, const AVFrame *frame, int screen,
                              uint8_t *out)
{
    const int W = c->width, VR = c->vres, bmp = c->mode;
    const int gd = (bmp == RVID_BMP_555) ? 4 : 2;
    const int field0 = c->interlaced ? (c->frame_idx & 1) : 0;
    const int step   = c->interlaced ? 2 : 1;
    const int phase  = c->interlaced ? 0 : (c->frame_idx & 1);
    const int raw_payload = W * VR * (bmp == RVID_BMP_8BPP ? 1 : 2);
    const uint8_t *top = frame->data[0] + (size_t)screen * c->disp_h * frame->linesize[0];
    int ret, pal_bytes = 0;

    if (bmp == RVID_BMP_8BPP) {
        const int npix = W * VR;
        int colors = 0;

        /* Gather dithered, 565-snapped pixels as ELBG points.  While the
         * picture has no more than 256 colours, also number them in the
         * order they appear: that is Vid2RVID's palette, and it is exact. */
        memset(out, 0, RVID_PAL_BYTES);
        for (int y = 0; y < VR; y++) {
            const int row = field0 + step * y;
            const uint8_t *src = top + (size_t)row * frame->linesize[0];
            for (int x = 0; x < W; x++) {
                int R = src[x * 3 + 0], G = src[x * 3 + 1], B = src[x * 3 + 2];
                const int a1 = c->dither && ((x + row + phase) & 1);
                const int a2 = c->dither && c->fast_dither && ((x + row) & 1);
                const int i = (y * W + x) * 3;

                R = dither_ch(R, a1, a2, 4);
                G = dither_ch(G, a1, a2, gd);
                B = dither_ch(B, a1, a2, 4);
                if (colors <= 256) {
                    const unsigned word = pack_word(R, G, B, RVID_BMP_565);
                    int k = c->pal_index[word];

                    if (c->pal_stamp[word] != c->stamp) {
                        c->pal_stamp[word] = c->stamp;
                        k = c->pal_index[word] = colors++;
                        if (k < 256)
                            AV_WL16(out + k * 2, word);
                    }
                    c->payload[y * W + x] = k;
                }
                R &= 0xF8; R |= R >> 5;
                G &= 0xFC; G |= G >> 6;
                B &= 0xF8; B |= B >> 5;
                c->points[i] = R; c->points[i + 1] = G; c->points[i + 2] = B;
            }
        }
        c->stamp++;
        if (colors > 256) {
            memset(c->codebook, 0, sizeof(c->codebook));
            ret = avpriv_elbg_do(&c->elbg, c->points, 3, npix, c->codebook,
                                 256, 1, c->closest, &c->lfg, 0);
            if (ret < 0)
                return ret;
            for (int k = 0; k < 256; k++) {
                int R = av_clip_uint8(c->codebook[k * 3 + 0]);
                int G = av_clip_uint8(c->codebook[k * 3 + 1]);
                int B = av_clip_uint8(c->codebook[k * 3 + 2]);
                AV_WL16(out + k * 2, pack_word(R, G, B, RVID_BMP_565));
            }
            for (int i = 0; i < npix; i++)
                c->payload[i] = c->closest[i];
        }
        pal_bytes = RVID_PAL_BYTES;
    } else {
        for (int y = 0; y < VR; y++) {
            const int row = field0 + step * y;
            const uint8_t *src = top + (size_t)row * frame->linesize[0];
            uint8_t *dst = c->payload + (size_t)y * W * 2;
            for (int x = 0; x < W; x++) {
                int R = src[x * 3 + 0], G = src[x * 3 + 1], B = src[x * 3 + 2];
                const int a1 = c->dither && ((x + row + phase) & 1);
                const int a2 = c->dither && c->fast_dither && ((x + row) & 1);

                R = dither_ch(R, a1, a2, 4);
                G = dither_ch(G, a1, a2, gd);
                B = dither_ch(B, a1, a2, 4);
                AV_WL16(dst + x * 2, pack_word(R, G, B, bmp));
            }
        }
    }

    if (c->compress) {
        /* The player takes a frame whose stored size is that of a raw frame
         * for a raw one, which is how a frame LZ10 cannot shrink is kept. */
        const int cap = avpriv_nintendo_lz10_bound(raw_payload);
        int lz = c->lazy ? avpriv_nintendo_lz10_compress(c->payload, raw_payload, c->lzbuf, cap)
                         : avpriv_nintendo_lz10_compress_greedy(c->payload, raw_payload, c->lzbuf, cap);
        if (lz <= 0)
            return lz < 0 ? lz : AVERROR_BUG;
        if (FFALIGN(lz, 4) < raw_payload) {
            memcpy(out + pal_bytes, c->lzbuf, lz);
            memset(out + pal_bytes + lz, 0, FFALIGN(lz, 4) - lz);
            return pal_bytes + FFALIGN(lz, 4);
        }
    }
    memcpy(out + pal_bytes, c->payload, raw_payload);
    return pal_bytes + raw_payload;
}

static int rvid_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet)
{
    RVIDEncCtx *c = avctx->priv_data;
    const int max_screen = RVID_PAL_BYTES + c->width * c->vres * 2;
    int ret, size = c->screens == 2 ? 4 : 0;

    if (!frame)                 /* no delay: nothing to flush */
        return 0;

    if ((ret = ff_get_encode_buffer(avctx, pkt, 4 + c->screens * max_screen, 0)) < 0)
        return ret;
    for (int i = 0; i < c->screens; i++) {
        ret = rvid_encode_screen(c, frame, i, pkt->data + size);
        if (ret < 0)
            return ret;
        if (c->screens == 2 && !i)
            AV_WL32(pkt->data, ret);
        size += ret;
    }
    av_shrink_packet(pkt, size);
    pkt->flags |= AV_PKT_FLAG_KEY;

    c->frame_idx++;
    *got_packet = 1;
    return 0;
}

static av_cold int rvid_encode_close(AVCodecContext *avctx)
{
    RVIDEncCtx *c = avctx->priv_data;
    avpriv_elbg_free(&c->elbg);
    av_freep(&c->payload);
    av_freep(&c->lzbuf);
    av_freep(&c->points);
    av_freep(&c->closest);
    av_freep(&c->pal_index);
    av_freep(&c->pal_stamp);
    return 0;
}

#define OFF(x) offsetof(RVIDEncCtx, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption rvid_options[] = {
    { "mode", "pixel mode", OFF(mode), AV_OPT_TYPE_INT, {.i64 = RVID_BMP_555}, 0, 2, VE, .unit = "mode" },
    { "rgb555", "16bpp RGB555 (unlimited color)", 0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_555}, 0, 0, VE, .unit = "mode" },
    { "rgb565", "16bpp RGB565 (max color)",       0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_565}, 0, 0, VE, .unit = "mode" },
    { "256",    "8bpp 256-color palette",         0, AV_OPT_TYPE_CONST, {.i64 = RVID_BMP_8BPP}, 0, 0, VE, .unit = "mode" },
    { "interlaced", "store one field per frame (default: as Vid2RVID does, above the frame rate the player can show whole frames at)",  OFF(interlaced), AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VE },
    { "compress",   "Nintendo LZ10 compression (default: as Vid2RVID does, up to the frame rate the player can decompress at)",  OFF(compress),   AV_OPT_TYPE_BOOL, {.i64 = -1}, -1, 1, VE },
    { "dither",     "checkerboard ordered dither", OFF(dither),    AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, VE },
    { "lazy", "lazy LZ10 matching: slightly smaller frames than Vid2RVID writes", OFF(lazy), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VE },
    { "dual_screen", "the frame is the top screen above the bottom screen", OFF(dual_screen), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VE },
    { NULL }
};

static const AVClass rvid_encoder_class = {
    .class_name = "rvid encoder",
    .item_name  = av_default_item_name,
    .option     = rvid_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_rvid_encoder = {
    .p.name         = "rvid",
    CODEC_LONG_NAME("RocketVideo (RVID)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_RVID,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(RVIDEncCtx),
    .p.priv_class   = &rvid_encoder_class,
    .init           = rvid_encode_init,
    .close          = rvid_encode_close,
    FF_CODEC_ENCODE_CB(rvid_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_RGB24),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
