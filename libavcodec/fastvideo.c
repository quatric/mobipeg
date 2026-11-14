/*
 * FastVideoDS video decoder
 * Copyright (c) 2026 quatric
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "mathops.h"
#include "mpeg4videodata.h"

#define FV_VLC_BITS 12

typedef struct FVBitReader {
    const uint8_t *data;
    int size_bits;
    int bit_pos;
} FVBitReader;

typedef struct FastVideoContext {
    uint8_t *plane[2][3];
    int16_t *mvx;
    int16_t *mvy;
    uint8_t max_level[2][64];
    uint8_t max_run[2][32];
    /* the code a 12-bit window starts with: its index and length (0: none) */
    uint8_t vlc_index[1 << FV_VLC_BITS];
    uint8_t vlc_len[1 << FV_VLC_BITS];
    int width, height;
    int mb_width, mb_height;
    int ref_index;
    int have_reference;
} FastVideoContext;

/* n bits (at most 24) starting at bit pos; they must lie inside the data,
 * which is followed by the packet padding. */
static av_always_inline unsigned fv_peek_bits(const FVBitReader *br, int pos, int n)
{
    const uint8_t *p = br->data + (pos >> 4) * 2;
    const uint64_t v = (uint64_t)AV_RL16(p) << 32 | (uint64_t)AV_RL16(p + 2) << 16 |
                       AV_RL16(p + 4);

    return (v >> (48 - (pos & 15) - n)) & ((1U << n) - 1);
}

static int fv_get_bits(FVBitReader *br, int n, unsigned *value)
{
    if (n < 0 || n > 24 || br->bit_pos > br->size_bits - n)
        return AVERROR_INVALIDDATA;
    *value = fv_peek_bits(br, br->bit_pos, n);
    br->bit_pos += n;
    return 0;
}

/* The codes are 2 to 12 bits long and none is the start of another, so the
 * next 12 bits (short of that at the end of the data, where the missing ones
 * count as zero) pick out the one code that can match. */
static int fv_get_vlc(const FastVideoContext *s, FVBitReader *br, int allow_escape)
{
    const int left = br->size_bits - br->bit_pos;
    unsigned window;
    int len;

    if (left >= FV_VLC_BITS)
        window = fv_peek_bits(br, br->bit_pos, FV_VLC_BITS);
    else if (left > 0)
        window = fv_peek_bits(br, br->bit_pos, left) << (FV_VLC_BITS - left);
    else
        return AVERROR_INVALIDDATA;
    len = s->vlc_len[window];
    if (!len || len > left || (!allow_escape && s->vlc_index[window] == 102))
        return AVERROR_INVALIDDATA;
    br->bit_pos += len;
    return s->vlc_index[window];
}

static int fv_decode_base(const FastVideoContext *s, FVBitReader *br,
                          int *last, int *run, int *level)
{
    unsigned sign;
    int code = fv_get_vlc(s, br, 0);

    if (code < 0 || fv_get_bits(br, 1, &sign) < 0)
        return AVERROR_INVALIDDATA;
    *last  = code >= 67;
    *run   = ff_mpeg4_intra_run[code];
    *level = sign ? -ff_mpeg4_intra_level[code]
                  :  ff_mpeg4_intra_level[code];
    return 0;
}

static int fv_decode_dct(FastVideoContext *s, FVBitReader *br, int coeff[64])
{
    int pos = 0;

    memset(coeff, 0, 64 * sizeof(*coeff));
    for (;;) {
        unsigned bit;
        int code, last, run, level, sign;

        code = fv_get_vlc(s, br, 1);
        if (code < 0)
            return code;
        if (code != 102) {
            if (fv_get_bits(br, 1, &bit) < 0)
                return AVERROR_INVALIDDATA;
            last  = code >= 67;
            run   = ff_mpeg4_intra_run[code];
            level = bit ? -ff_mpeg4_intra_level[code]
                        :  ff_mpeg4_intra_level[code];
        } else {
            if (fv_get_bits(br, 1, &bit) < 0)
                return AVERROR_INVALIDDATA;
            if (!bit) {
                if (fv_decode_base(s, br, &last, &run, &level) < 0)
                    return AVERROR_INVALIDDATA;
                sign = level < 0;
                level = FFABS(level) + s->max_level[last][run];
                if (sign)
                    level = -level;
            } else {
                if (fv_get_bits(br, 1, &bit) < 0)
                    return AVERROR_INVALIDDATA;
                if (!bit) {
                    if (fv_decode_base(s, br, &last, &run, &level) < 0)
                        return AVERROR_INVALIDDATA;
                    if (FFABS(level) >= 32)
                        return AVERROR_INVALIDDATA;
                    run += s->max_run[last][FFABS(level)] + 1;
                } else {
                    unsigned raw;
                    if (fv_get_bits(br, 1, &bit) < 0 ||
                        fv_get_bits(br, 6, &raw) < 0)
                        return AVERROR_INVALIDDATA;
                    last = bit;
                    run = raw;
                    if (fv_get_bits(br, 12, &raw) < 0)
                        return AVERROR_INVALIDDATA;
                    level = sign_extend(raw, 12);
                }
            }
        }

        if (run > 63 - pos)
            return AVERROR_INVALIDDATA;
        pos += run;
        coeff[pos] = level;
        if (last)
            return 0;
        if (++pos >= 64)
            return AVERROR_INVALIDDATA;
    }
}

static int fv_get_signed_varint(FVBitReader *br, int *value)
{
    unsigned bit, rest = 0, code;
    int zeros = 0;

    do {
        if (fv_get_bits(br, 1, &bit) < 0 || zeros > 15)
            return AVERROR_INVALIDDATA;
        zeros++;
    } while (!bit);
    zeros--;
    if (fv_get_bits(br, zeros, &rest) < 0)
        return AVERROR_INVALIDDATA;
    code = (1U << zeros) + rest;
    *value = (code & 1) ? (1 - (int)code) / 2 : (int)code / 2;
    return 0;
}

static void fv_get_group(const uint8_t *plane, int stride, int x, int y,
                         uint8_t dst[4])
{
    dst[0] = plane[y * stride + x];
    dst[1] = plane[y * stride + x + 2];
    dst[2] = plane[(y + 2) * stride + x];
    dst[3] = plane[(y + 2) * stride + x + 2];
}

static void fv_put_group(uint8_t *plane, int stride, int x, int y,
                         const uint8_t src[4])
{
    plane[y * stride + x] = src[0];
    plane[y * stride + x + 2] = src[1];
    plane[(y + 2) * stride + x] = src[2];
    plane[(y + 2) * stride + x + 2] = src[3];
}

static void fv_idct4(const int dct[4], const uint8_t pred[4], uint8_t dst[4])
{
    const int r0 = dct[0] + 16;
    const int t0 = r0 + dct[1];
    const int t2 = r0 - dct[1];
    const int t1 = dct[2] + dct[3];
    const int t3 = dct[2] - dct[3];

    dst[0] = av_clip((pred[0] >> 3) + ((t0 + t1) >> 5), 0, 31) << 3;
    dst[1] = av_clip((pred[1] >> 3) + ((t0 - t1) >> 5), 0, 31) << 3;
    dst[2] = av_clip((pred[2] >> 3) + ((t2 + t3) >> 5), 0, 31) << 3;
    dst[3] = av_clip((pred[3] >> 3) + ((t2 - t3) >> 5), 0, 31) << 3;
}

static void fv_reconstruct_intra(uint8_t *dst, const uint8_t *green,
                                 int stride, int mbx, int mby,
                                 const int coeff[64], int green_plane)
{
    static const int dequant[4] = { 32, 23, 23, 64 };
    uint8_t pred[4], out[4];
    int group = 0;

    for (int y3 = 0; y3 < 8; y3 += 4)
        for (int x3 = 0; x3 < 8; x3 += 4)
            for (int y2 = 0; y2 < 2; y2++)
                for (int x2 = 0; x2 < 2; x2++, group++) {
                    const int x = mbx + x3 + x2;
                    const int y = mby + y3 + y2;
                    int dct[4];

                    if (!x3 && !x2 && !y3 && !y2) {
                        if (green_plane)
                            memset(pred, 0, sizeof(pred));
                        else
                            fv_get_group(green, stride, x, y, pred);
                    } else if (!x2 && y2) {
                        fv_get_group(dst, stride, mbx + x3, y - 1, pred);
                    } else if (!x2 && x3) {
                        fv_get_group(dst, stride, x - 4, y, pred);
                    } else if (!x2 && !y2 && y3) {
                        fv_get_group(dst, stride, x, y - 4, pred);
                    } else {
                        fv_get_group(dst, stride, x - 1, y, pred);
                    }
                    for (int i = 0; i < 4; i++)
                        dct[i] = coeff[i * 16 + group] * dequant[i];
                    fv_idct4(dct, pred, out);
                    fv_put_group(dst, stride, x, y, out);
                }
}

static uint8_t fv_half_sample(const uint8_t *src, int width, int height,
                              int hx, int hy)
{
    const int x = hx >> 1;
    const int y = hy >> 1;
    const int x1 = av_clip(x, 0, width - 1);
    const int y1 = av_clip(y, 0, height - 1);
    int x2 = x1, y2 = y1;
    int a, b;

    if (!(hx & 1) && !(hy & 1))
        return src[y1 * width + x1];
    if (hx & 1)
        x2 = av_clip(x + 1, 0, width - 1);
    if (hy & 1)
        y2 = av_clip(y + 1, 0, height - 1);
    a = src[y1 * width + x1] >> 3;
    b = src[y2 * width + x2] >> 3;
    a = a ? 2 * a + 1 : 0;
    b = b ? 2 * b + 1 : 0;
    return ((a + b) >> 2) << 3;
}

static void fv_motion_tile(const uint8_t *src, int width, int height,
                           int hx, int hy, uint8_t dst[64])
{
    const int fx = hx & 1, fy = hy & 1;
    const int x0 = hx >> 1, y0 = hy >> 1;

    /* a tile that stays inside the plane needs none of the clipping */
    if (x0 >= 0 && y0 >= 0 && x0 + 7 + fx < width && y0 + 7 + fy < height) {
        const uint8_t *p = src + y0 * width + x0;

        if (!fx && !fy) {
            for (int y = 0; y < 8; y++)
                memcpy(dst + y * 8, p + y * width, 8);
        } else {
            const uint8_t *q = p + fy * width + fx;

            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int a = p[y * width + x] >> 3;
                    int b = q[y * width + x] >> 3;
                    a = a ? 2 * a + 1 : 0;
                    b = b ? 2 * b + 1 : 0;
                    dst[y * 8 + x] = ((a + b) >> 2) << 3;
                }
        }
        return;
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            dst[y * 8 + x] = fv_half_sample(src, width, height,
                                             hx + 2 * x, hy + 2 * y);
}

static void fv_reconstruct_residual(uint8_t *dst, int stride, int mbx, int mby,
                                    const uint8_t pred[64], const int coeff[64])
{
    static const int dequant[4] = { 32, 23, 23, 64 };
    uint8_t p[4], out[4];
    int group = 0;

    for (int y3 = 0; y3 < 8; y3 += 4)
        for (int x3 = 0; x3 < 8; x3 += 4)
            for (int y2 = 0; y2 < 2; y2++)
                for (int x2 = 0; x2 < 2; x2++, group++) {
                    int dct[4];
                    fv_get_group(pred, 8, x3 + x2, y3 + y2, p);
                    for (int i = 0; i < 4; i++)
                        dct[i] = coeff[i * 16 + group] * dequant[i];
                    fv_idct4(dct, p, out);
                    fv_put_group(dst, stride, mbx + x3 + x2,
                                 mby + y3 + y2, out);
                }
}

static int fv_median_prediction(const int16_t *vectors, int mb_width,
                                int x, int y)
{
    int values[3], n = 0;

    if (x)
        values[n++] = vectors[y * mb_width + x - 1];
    if (y) {
        values[n++] = vectors[(y - 1) * mb_width + x];
        if (x + 1 < mb_width)
            values[n++] = vectors[(y - 1) * mb_width + x + 1];
    }
    if (!n)
        return 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (values[j] < values[i])
                FFSWAP(int, values[i], values[j]);
    return values[n / 2];
}

static int fv_decode_i_frame(FastVideoContext *s, FVBitReader *br, int dst_index)
{
    int last_gdc = 0;

    for (int y = 0; y < s->height; y += 8)
        for (int x = 0; x < s->width; x += 8) {
            int coeff[64];

            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            coeff[0] += last_gdc;
            last_gdc = coeff[0];
            fv_reconstruct_intra(s->plane[dst_index][1], NULL,
                                 s->width, x, y, coeff, 1);
            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            fv_reconstruct_intra(s->plane[dst_index][0], s->plane[dst_index][1],
                                 s->width, x, y, coeff, 0);
            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            fv_reconstruct_intra(s->plane[dst_index][2], s->plane[dst_index][1],
                                 s->width, x, y, coeff, 0);
        }
    return 0;
}

static int fv_decode_p_frame(FastVideoContext *s, FVBitReader *br, int dst_index)
{
    const int mb_count = s->mb_width * s->mb_height;
    const int ref = s->ref_index;

    if (!s->have_reference)
        return AVERROR_INVALIDDATA;

    for (int y = 0; y < s->mb_height; y++)
        for (int x = 0; x < s->mb_width; x++) {
            const int mb = y * s->mb_width + x;
            int px = fv_median_prediction(s->mvx, s->mb_width, x, y);
            int py = fv_median_prediction(s->mvy, s->mb_width, x, y);
            unsigned same;

            if (fv_get_bits(br, 1, &same) < 0)
                return AVERROR_INVALIDDATA;
            if (!same) {
                int dx, dy;
                if (fv_get_signed_varint(br, &dx) < 0 ||
                    fv_get_signed_varint(br, &dy) < 0)
                    return AVERROR_INVALIDDATA;
                px += dx;
                py += dy;
            }
            if (px < INT16_MIN || px > INT16_MAX ||
                py < INT16_MIN || py > INT16_MAX)
                return AVERROR_INVALIDDATA;
            s->mvx[mb] = px;
            s->mvy[mb] = py;
        }

    br->bit_pos = FFALIGN(br->bit_pos, 16);
    if (br->bit_pos > br->size_bits)
        return AVERROR_INVALIDDATA;

    for (int mb = 0; mb < mb_count; mb++) {
        const int x = (mb % s->mb_width) * 8;
        const int y = (mb / s->mb_width) * 8;
        unsigned intra;
        int coeff[64];

        if (fv_get_bits(br, 1, &intra) < 0)
            return AVERROR_INVALIDDATA;
        if (intra) {
            uint8_t local[3][64];
            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            fv_reconstruct_intra(local[1], NULL, 8, 0, 0, coeff, 1);
            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            fv_reconstruct_intra(local[0], local[1], 8, 0, 0, coeff, 0);
            if (fv_decode_dct(s, br, coeff) < 0)
                return AVERROR_INVALIDDATA;
            fv_reconstruct_intra(local[2], local[1], 8, 0, 0, coeff, 0);
            for (int p = 0; p < 3; p++)
                for (int row = 0; row < 8; row++)
                    memcpy(s->plane[dst_index][p] + (y + row) * s->width + x,
                           local[p] + row * 8, 8);
        } else {
            static const uint8_t plane_order[3] = { 1, 0, 2 };

            for (int component = 0; component < 3; component++) {
                uint8_t pred[64];
                unsigned coded;
                const int p = plane_order[component];

                fv_motion_tile(s->plane[ref][p], s->width, s->height,
                               2 * x + s->mvx[mb], 2 * y + s->mvy[mb], pred);
                memset(coeff, 0, sizeof(coeff));
                if (fv_get_bits(br, 1, &coded) < 0 ||
                    (coded && fv_decode_dct(s, br, coeff) < 0))
                    return AVERROR_INVALIDDATA;
                fv_reconstruct_residual(s->plane[dst_index][p], s->width,
                                        x, y, pred, coeff);
            }
        }
    }
    return 0;
}

static av_cold int fastvideo_decode_init(AVCodecContext *avctx)
{
    FastVideoContext *s = avctx->priv_data;
    size_t plane_size;

    if (avctx->width <= 0 || avctx->height <= 0 ||
        (avctx->width & 7) || (avctx->height & 7))
        return AVERROR_INVALIDDATA;
    s->width = avctx->width;
    s->height = avctx->height;
    s->mb_width = s->width / 8;
    s->mb_height = s->height / 8;
    plane_size = (size_t)s->width * s->height;

    for (int frame = 0; frame < 2; frame++)
        for (int p = 0; p < 3; p++) {
            s->plane[frame][p] = av_malloc(plane_size);
            if (!s->plane[frame][p])
                return AVERROR(ENOMEM);
        }
    s->mvx = av_calloc(s->mb_width * s->mb_height, sizeof(*s->mvx));
    s->mvy = av_calloc(s->mb_width * s->mb_height, sizeof(*s->mvy));
    if (!s->mvx || !s->mvy)
        return AVERROR(ENOMEM);

    for (int i = 0; i < 102; i++) {
        const int last = i >= 67;
        const int run = ff_mpeg4_intra_run[i];
        const int level = ff_mpeg4_intra_level[i];
        s->max_level[last][run] = FFMAX(s->max_level[last][run], level);
        s->max_run[last][level] = FFMAX(s->max_run[last][level], run);
    }
    for (int i = 0; i < 103; i++) {
        const int len = ff_mpeg4_intra_vlc[i][1];
        const int first = ff_mpeg4_intra_vlc[i][0] << (FV_VLC_BITS - len);

        for (int j = 0; j < 1 << (FV_VLC_BITS - len); j++) {
            s->vlc_index[first + j] = i;
            s->vlc_len[first + j]   = len;
        }
    }
    avctx->pix_fmt = AV_PIX_FMT_BGR555LE;
    return 0;
}

static av_cold int fastvideo_decode_close(AVCodecContext *avctx)
{
    FastVideoContext *s = avctx->priv_data;

    for (int frame = 0; frame < 2; frame++)
        for (int p = 0; p < 3; p++)
            av_freep(&s->plane[frame][p]);
    av_freep(&s->mvx);
    av_freep(&s->mvy);
    return 0;
}

static int fastvideo_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                  int *got_frame, AVPacket *avpkt)
{
    FastVideoContext *s = avctx->priv_data;
    FVBitReader br = { avpkt->data, avpkt->size * 8, 0 };
    unsigned p_frame, q;
    int dst_index = s->have_reference ? s->ref_index ^ 1 : 0;
    int ret;

    if (avpkt->size < 2 || avpkt->size > INT_MAX / 8 || (avpkt->size & 1) ||
        fv_get_bits(&br, 1, &p_frame) < 0)
        return AVERROR_INVALIDDATA;
    if (!p_frame) {
        if (fv_get_bits(&br, 6, &q) < 0)
            return AVERROR_INVALIDDATA;
        (void)q;
        ret = fv_decode_i_frame(s, &br, dst_index);
    } else {
        ret = fv_decode_p_frame(s, &br, dst_index);
    }
    if (ret < 0)
        return ret;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (int y = 0; y < s->height; y++) {
        uint16_t *line = (uint16_t *)(frame->data[0] + y * frame->linesize[0]);
        for (int x = 0; x < s->width; x++) {
            const int off = y * s->width + x;
            line[x] = 0x8000 | (s->plane[dst_index][0][off] >> 3) |
                      ((s->plane[dst_index][1][off] >> 3) << 5) |
                      ((s->plane[dst_index][2][off] >> 3) << 10);
        }
    }

    frame->pict_type = p_frame ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    if (!p_frame)
        frame->flags |= AV_FRAME_FLAG_KEY;
    s->ref_index = dst_index;
    s->have_reference = 1;
    *got_frame = 1;
    return avpkt->size;
}

const FFCodec ff_fastvideo_decoder = {
    .p.name         = "fastvideo",
    CODEC_LONG_NAME("FastVideoDS Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FASTVIDEO,
    .priv_data_size = sizeof(FastVideoContext),
    .init           = fastvideo_decode_init,
    .close          = fastvideo_decode_close,
    FF_CODEC_DECODE_CB(fastvideo_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};

typedef struct FVPutBitContext {
    uint8_t *data;
    int size;
    int pos;
    unsigned word;
    int bits;
} FVPutBitContext;

typedef struct FastVideoEncContext {
    uint8_t *plane[3];
    int width, height;
    /* the code for (last, run, |level|), or -1 where only an escape will do */
    int8_t code[2][64][64];
} FastVideoEncContext;

/* Append the low n bits (at most 16) of value; fewer than 16 are ever left
 * waiting in pb->word. */
static int fv_put_bits(FVPutBitContext *pb, unsigned value, int n)
{
    pb->word  = pb->word << n | (value & ((1U << n) - 1));
    pb->bits += n;
    if (pb->bits >= 16) {
        if (pb->pos > pb->size - 2)
            return AVERROR_BUFFER_TOO_SMALL;
        pb->bits -= 16;
        AV_WL16(pb->data + pb->pos, pb->word >> pb->bits);
        pb->pos += 2;
        pb->word &= (1U << pb->bits) - 1;
    }
    return 0;
}

static int fv_flush_put_bits(FVPutBitContext *pb)
{
    if (pb->bits) {
        if (pb->pos > pb->size - 2)
            return AVERROR_BUFFER_TOO_SMALL;
        pb->word <<= 16 - pb->bits;
        AV_WL16(pb->data + pb->pos, pb->word);
        pb->pos += 2;
        pb->word = pb->bits = 0;
    }
    return 0;
}

static int fv_encode_dct(const FastVideoEncContext *s, FVPutBitContext *pb,
                         const int coeff[64])
{
    int last_nonzero = -1, run = 0;

    for (int i = 0; i < 64; i++)
        if (coeff[i])
            last_nonzero = i;
    if (last_nonzero < 0) {
        if (fv_put_bits(pb, 3, 7) < 0 || fv_put_bits(pb, 3, 2) < 0 ||
            fv_put_bits(pb, 1, 1) < 0 || fv_put_bits(pb, 0, 6) < 0 ||
            fv_put_bits(pb, 0, 12) < 0)
            return AVERROR_BUFFER_TOO_SMALL;
        return 0;
    }

    for (int i = 0; i <= last_nonzero; i++) {
        int code = -1;
        const int last = i == last_nonzero;
        const int level = coeff[i];

        if (!level) {
            run++;
            continue;
        }
        if (FFABS(level) < 64)
            code = s->code[last][run][FFABS(level)];
        if (code >= 0) {
            if (fv_put_bits(pb, ff_mpeg4_intra_vlc[code][0],
                            ff_mpeg4_intra_vlc[code][1]) < 0 ||
                fv_put_bits(pb, level < 0, 1) < 0)
                return AVERROR_BUFFER_TOO_SMALL;
        } else {
            if (run > 63 || level < -2048 || level > 2047)
                return AVERROR(EINVAL);
            if (fv_put_bits(pb, 3, 7) < 0 || fv_put_bits(pb, 3, 2) < 0 ||
                fv_put_bits(pb, last, 1) < 0 || fv_put_bits(pb, run, 6) < 0 ||
                fv_put_bits(pb, level & 0xFFF, 12) < 0)
                return AVERROR_BUFFER_TOO_SMALL;
        }
        run = 0;
    }
    return 0;
}

static av_always_inline int fv_quantize(int value, int index)
{
#define FV_QUANT(dequant) (262144 / (dequant))
#define FV_ROUND(dequant) FFMIN(((1 << 17) + (FV_QUANT(dequant) >> 1)) / FV_QUANT(dequant), \
                                ((21 << 12) + (FV_QUANT(dequant) >> 1)) / FV_QUANT(dequant))
    static const int quants[4] = { FV_QUANT(32), FV_QUANT(23), FV_QUANT(23), FV_QUANT(64) };
    static const int rounds[4] = { FV_ROUND(32), FV_ROUND(23), FV_ROUND(23), FV_ROUND(64) };
    const int quant = quants[index];
    const int f = rounds[index];

    if (value < 0)
        return -(((f - value) * quant) >> 18);
    return ((f + value) * quant) >> 18;
}

static int fv_source_component(const AVFrame *frame, int x, int y, int component)
{
    const uint16_t pixel = AV_RL16(frame->data[0] + y * frame->linesize[0] + 2 * x);

    if (component == 0)
        return (pixel & 31) << 3;
    if (component == 1)
        return ((pixel >> 5) & 31) << 3;
    return ((pixel >> 10) & 31) << 3;
}

static void fv_encode_plane(FastVideoEncContext *s, const AVFrame *frame,
                            int component, int mbx, int mby, int coeff[64])
{
    static const int dequant[4] = { 32, 23, 23, 64 };
    uint8_t *dst = s->plane[component];
    uint8_t pred[4], out[4];
    int group = 0;

    memset(coeff, 0, 64 * sizeof(*coeff));
    for (int y3 = 0; y3 < 8; y3 += 4)
        for (int x3 = 0; x3 < 8; x3 += 4)
            for (int y2 = 0; y2 < 2; y2++)
                for (int x2 = 0; x2 < 2; x2++, group++) {
                    const int x = mbx + x3 + x2;
                    const int y = mby + y3 + y2;
                    int pixels[4], dct[4], quantized[4];
                    const int sx[4] = { x, x + 2, x, x + 2 };
                    const int sy[4] = { y, y, y + 2, y + 2 };

                    if (!x3 && !x2 && !y3 && !y2) {
                        if (component == 1)
                            memset(pred, 0, sizeof(pred));
                        else
                            fv_get_group(s->plane[1], s->width, x, y, pred);
                    } else if (!x2 && y2) {
                        fv_get_group(dst, s->width, mbx + x3, y - 1, pred);
                    } else if (!x2 && x3) {
                        fv_get_group(dst, s->width, x - 4, y, pred);
                    } else if (!x2 && !y2 && y3) {
                        fv_get_group(dst, s->width, x, y - 4, pred);
                    } else {
                        fv_get_group(dst, s->width, x - 1, y, pred);
                    }
                    for (int i = 0; i < 4; i++)
                        pixels[i] = fv_source_component(frame, sx[i], sy[i], component) - pred[i];
                    dct[0] = pixels[0] + pixels[1] + pixels[2] + pixels[3];
                    dct[1] = pixels[0] + pixels[1] - pixels[2] - pixels[3];
                    dct[2] = pixels[0] - pixels[1] + pixels[2] - pixels[3];
                    dct[3] = pixels[0] - pixels[1] - pixels[2] + pixels[3];
                    for (int i = 0; i < 4; i++) {
                        quantized[i] = fv_quantize(dct[i], i);
                        coeff[i * 16 + group] = quantized[i];
                        dct[i] = quantized[i] * dequant[i];
                    }
                    fv_idct4(dct, pred, out);
                    fv_put_group(dst, s->width, x, y, out);
                }
}

static av_cold int fastvideo_encode_init(AVCodecContext *avctx)
{
    FastVideoEncContext *s = avctx->priv_data;
    const size_t plane_size = (size_t)avctx->width * avctx->height;

    if (avctx->width <= 0 || avctx->height <= 0 ||
        (avctx->width & 7) || (avctx->height & 7)) {
        av_log(avctx, AV_LOG_ERROR,
               "Width and height must be positive multiples of eight\n");
        return AVERROR(EINVAL);
    }
    s->width = avctx->width;
    s->height = avctx->height;
    for (int p = 0; p < 3; p++) {
        s->plane[p] = av_malloc(plane_size);
        if (!s->plane[p])
            return AVERROR(ENOMEM);
    }
    memset(s->code, -1, sizeof(s->code));
    for (int i = 101; i >= 0; i--)
        s->code[i >= 67][ff_mpeg4_intra_run[i]][ff_mpeg4_intra_level[i]] = i;
    return 0;
}

static av_cold int fastvideo_encode_close(AVCodecContext *avctx)
{
    FastVideoEncContext *s = avctx->priv_data;

    for (int p = 0; p < 3; p++)
        av_freep(&s->plane[p]);
    return 0;
}

static int fastvideo_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                  const AVFrame *frame, int *got_packet)
{
    FastVideoEncContext *s = avctx->priv_data;
    const int64_t mb_count = (int64_t)s->width * s->height / 64;
    const int64_t max_size = 2 + mb_count * 3 * 64 * 28 / 8;
    FVPutBitContext pb;
    int last_gdc = 0, ret;

    if (max_size > INT_MAX)
        return AVERROR(EINVAL);
    if ((ret = ff_get_encode_buffer(avctx, pkt, max_size, 0)) < 0)
        return ret;
    pb = (FVPutBitContext) { pkt->data, pkt->size, 0, 0, 0 };
    if (fv_put_bits(&pb, 0, 1) < 0 || fv_put_bits(&pb, 30, 6) < 0)
        return AVERROR_BUFFER_TOO_SMALL;

    for (int y = 0; y < s->height; y += 8)
        for (int x = 0; x < s->width; x += 8) {
            int coeff[64], dc;

            fv_encode_plane(s, frame, 1, x, y, coeff);
            dc = coeff[0];
            coeff[0] -= last_gdc;
            last_gdc = dc;
            if ((ret = fv_encode_dct(s, &pb, coeff)) < 0)
                return ret;
            fv_encode_plane(s, frame, 0, x, y, coeff);
            if ((ret = fv_encode_dct(s, &pb, coeff)) < 0)
                return ret;
            fv_encode_plane(s, frame, 2, x, y, coeff);
            if ((ret = fv_encode_dct(s, &pb, coeff)) < 0)
                return ret;
        }
    if ((ret = fv_flush_put_bits(&pb)) < 0)
        return ret;
    av_shrink_packet(pkt, pb.pos);
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

const FFCodec ff_fastvideo_encoder = {
    .p.name         = "fastvideo",
    CODEC_LONG_NAME("FastVideoDS Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FASTVIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(FastVideoEncContext),
    .init           = fastvideo_encode_init,
    FF_CODEC_ENCODE_CB(fastvideo_encode_frame),
    .close          = fastvideo_encode_close,
    CODEC_PIXFMTS(AV_PIX_FMT_BGR555LE),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
