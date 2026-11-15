/*
 * FastVideoDS video decoder and encoder
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 *
 * This file is part of FFmpeg.
 */

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/intmath.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "mathops.h"
#include "mpeg4videodata.h"
#include "thread.h"
#include "threadprogress.h"

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

/* The average the player takes of two five-bit samples, for the eight
 * samples in each of a and b at once.  No byte reaches its neighbour: a
 * sample is at most 31, twice that plus one 63, and two of those 126. */
static av_always_inline uint64_t fv_half_average(uint64_t a, uint64_t b)
{
    const uint64_t low5 = UINT64_C(0x1F1F1F1F1F1F1F1F);
    const uint64_t ones = UINT64_C(0x0101010101010101);

    a = (a >> 3) & low5;
    b = (b >> 3) & low5;
    /* 2 * a + 1 where a is not zero */
    a = 2 * a + (((a + (ones * 0x7F)) >> 7) & ones);
    b = 2 * b + (((b + (ones * 0x7F)) >> 7) & ones);
    return (((a + b) >> 2) & low5) << 3;
}

static void fv_motion_tile(const uint8_t *src, int width, int height,
                           int hx, int hy, uint8_t dst[64])
{
    const int fx = hx & 1, fy = hy & 1;
    const int x0 = hx >> 1, y0 = hy >> 1;
    int xa[8], xb[8];

    /* a tile that stays inside the plane needs none of the clipping */
    if (x0 >= 0 && y0 >= 0 && x0 + 7 + fx < width && y0 + 7 + fy < height) {
        const uint8_t *p = src + y0 * width + x0;

        if (!fx && !fy) {
            for (int y = 0; y < 8; y++)
                memcpy(dst + y * 8, p + y * width, 8);
        } else {
            const uint8_t *q = p + fy * width + fx;

            for (int y = 0; y < 8; y++)
                AV_WN64(dst + y * 8, fv_half_average(AV_RN64(p + y * width),
                                                     AV_RN64(q + y * width)));
        }
        return;
    }
    /* past an edge the nearest sample of the plane stands in; the second
     * sample of a pair is the next one along, clipped the same way */
    for (int x = 0; x < 8; x++) {
        xa[x] = av_clip(x0 + x, 0, width - 1);
        xb[x] = fx ? av_clip(x0 + x + 1, 0, width - 1) : xa[x];
    }
    for (int y = 0; y < 8; y++) {
        const int ya = av_clip(y0 + y, 0, height - 1);
        const int yb = fy ? av_clip(y0 + y + 1, 0, height - 1) : ya;
        const uint8_t *const row_a = src + ya * width;
        const uint8_t *const row_b = src + yb * width;
        uint8_t a[8], b[8];

        for (int x = 0; x < 8; x++) {
            a[x] = row_a[xa[x]];
            b[x] = row_b[xb[x]];
        }
        if (!fx && !fy)
            memcpy(dst + y * 8, a, 8);
        else
            AV_WN64(dst + y * 8, fv_half_average(AV_RN64(a), AV_RN64(b)));
    }
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

typedef struct FVMotionVector {
    int x, y;
} FVMotionVector;

/* What was decided for one block, kept until the frame is written. */
typedef struct FVBlock {
    int coeff[3][64];           /* the planes in coding order: green, red, blue */
    FVMotionVector vec;         /* the vector later blocks predict theirs from */
    FVMotionVector delta;       /* its own vector less its prediction */
    int intra;
} FVBlock;

/* The encoder follows Gericom's FastVideoDSEncoder decision for decision, so
 * that the same pictures give the same stream: the fixed intra quantizer, a
 * trellis for the residual of predicted blocks, the diamond motion search and
 * the choice between an intra and a predicted block or frame. */
typedef struct FastVideoEncContext {
    int gop;                    /* most predicted frames after a key frame */
    uint8_t *src[3];            /* the picture being coded, R G B, five bits << 3 */
    uint8_t *plane[2][3];       /* what the decoder has: the reference and the new frame */
    int cur;
    int have_reference;
    int gop_length;
    int width, height;
    /* Blocks are worked out a row per job.  In a key frame no block needs
     * another; in a predicted frame a block needs the vectors of the one to
     * its left and of two in the row above, so a row follows the one above
     * it two blocks behind. */
    FVBlock *blocks;
    ThreadProgress *progress;   /* blocks done, by row */
    int *intra_blocks;          /* by row */
    int nb_progress;
    uint8_t *vector_data, *dct_data;
    int vector_size, dct_size;
    int warned_size;
    /* the code for (last, run, |level|), or -1 where only an escape will do */
    int8_t code[2][64][64];
    uint8_t max_level[2][64];   /* largest level with a code, by run */
    uint8_t max_run[2][64];     /* largest run with a code, by level */
    /* bits of (last, run, level + 64), sign and escapes included */
    uint8_t bit_length[2][64][128];
    /* The same weighed for the trellis, by level + 64 and then run: the
     * code when more coefficients follow in the low half, the code for the
     * last one in the high half.  Row 128 is all zero. */
    uint32_t cost[129][64];
    int min_block_bits;         /* the fewest bits a block of coefficients takes */
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

/* Bits of the Elias gamma code of a signed value, and the code itself. */
static int fv_signed_varint_bits(int value)
{
    const unsigned val = value <= 0 ? 1 - 2 * value : 2 * value;

    return 2 * av_log2(val) + 1;
}

static int fv_put_signed_varint(FVPutBitContext *pb, int value)
{
    const unsigned val = value <= 0 ? 1 - 2 * value : 2 * value;
    const int n = av_log2(val);

    if (fv_put_bits(pb, 0, n) < 0 || fv_put_bits(pb, 1, 1) < 0 ||
        fv_put_bits(pb, val - (1U << n), n) < 0)
        return AVERROR_BUFFER_TOO_SMALL;
    return 0;
}

/* How (last, run, level) is coded: 0 with its own code, 1 with the code of
 * the run shortened by the longest one this level has (9 more bits), 2 with
 * the code of the level lowered by the largest one this run has (8 more
 * bits), 3 as 28 raw bits.  Returns the code to use for the first three. */
static av_always_inline int fv_code_form(const FastVideoEncContext *s, int last,
                                         int run, int level, int *form)
{
    int code, alt;

    if (level <= 31) {
        code = s->code[last][run][level];
        if (code >= 0) {
            *form = 0;
            return code;
        }
        alt = run - s->max_run[last][level] - 1;
        if (alt >= 0 && (code = s->code[last][alt][level]) >= 0) {
            *form = 1;
            return code;
        }
    }
    alt = level - s->max_level[last][run];
    if (alt > 0 && alt <= 31 && (code = s->code[last][run][alt]) >= 0) {
        *form = 2;
        return code;
    }
    *form = 3;
    return -1;
}

/* 64 flags, each a byte that is zero or one, as the bits of one word. */
static av_always_inline uint64_t fv_pack_flags(const uint8_t flags[64])
{
    uint64_t mask = 0;

    /* the low bit of each of eight bytes, gathered into one byte */
    for (int i = 0; i < 64; i += 8)
        mask |= (AV_RL64(flags + i) * UINT64_C(0x0102040810204080) >> 56) << i;
    return mask;
}

/* Which of the 64 coefficients are not zero, coefficient i in bit i. */
static av_always_inline uint64_t fv_nonzero_mask(const int coeff[64])
{
    uint8_t nonzero[64];

    for (int i = 0; i < 64; i++)
        nonzero[i] = coeff[i] != 0;
    return fv_pack_flags(nonzero);
}

/* Bits of a block with these coefficients; an empty one is its first
 * coefficient as a raw zero. */
static int fv_dct_bits(const FastVideoEncContext *s, const int coeff[64])
{
    uint64_t mask = fv_nonzero_mask(coeff);
    int bits = 0, prev = -1;

    if (!mask)
        mask = 1;
    while (mask) {
        const int i = ff_ctzll(mask);
        const int last = !(mask &= mask - 1);

        bits += coeff[i] >= -64 && coeff[i] < 64 ? s->bit_length[last][i - prev - 1][coeff[i] + 64] : 28;
        prev  = i;
    }
    return bits;
}

static int fv_encode_dct(const FastVideoEncContext *s, FVPutBitContext *pb,
                         const int coeff[64])
{
    /* an empty block is its first coefficient as a raw zero */
    uint64_t mask = fv_nonzero_mask(coeff);
    int prev = -1;

    if (!mask)
        mask = 1;
    while (mask) {
        const int i = ff_ctzll(mask);
        const int last = !(mask &= mask - 1);
        const int run = i - prev - 1;
        const int level = coeff[i];
        int form, code, ret = 0;

        code = fv_code_form(s, last, run, FFABS(level), &form);
        if (form) {
            ret |= fv_put_bits(pb, 3, 7);
            if (form == 1)
                ret |= fv_put_bits(pb, 2, 2);
            else if (form == 2)
                ret |= fv_put_bits(pb, 0, 1);
            else
                ret |= fv_put_bits(pb, 3, 2);
        }
        if (form < 3) {
            ret |= fv_put_bits(pb, ff_mpeg4_intra_vlc[code][0] << 1 | (level < 0),
                               ff_mpeg4_intra_vlc[code][1] + 1);
        } else {
            if (level < -2048 || level > 2047)
                return AVERROR(EINVAL);
            ret |= fv_put_bits(pb, last, 1);
            ret |= fv_put_bits(pb, run, 6);
            ret |= fv_put_bits(pb, level & 0xFFF, 12);
        }
        if (ret < 0)
            return AVERROR_BUFFER_TOO_SMALL;
        prev = i;
    }
    return 0;
}

#define FV_QUANT(dequant) (262144 / (dequant))
static const int fv_dequant[4] = { 32, 23, 23, 64 };
static const int fv_quant[4] = { FV_QUANT(32), FV_QUANT(23), FV_QUANT(23), FV_QUANT(64) };

static av_always_inline int fv_quantize(int value, int index)
{
#define FV_ROUND(dequant) FFMIN(((1 << 17) + (FV_QUANT(dequant) >> 1)) / FV_QUANT(dequant), \
                                ((21 << 12) + (FV_QUANT(dequant) >> 1)) / FV_QUANT(dequant))
    static const int rounds[4] = { FV_ROUND(32), FV_ROUND(23), FV_ROUND(23), FV_ROUND(64) };
    const int quant = fv_quant[index];
    const int f = rounds[index];

    /* the magnitude is rounded, then the sign put back: which sign a
     * coefficient has is not something to branch on */
    const int sign = value >> 31;
    const int level = ((f + ((value ^ sign) - sign)) * quant) >> 18;

    return (level ^ sign) - sign;
}

static av_always_inline void fv_dct4(const uint8_t pixels[4], const uint8_t pred[4],
                                     int dct[4])
{
    const int d0 = pixels[0] - pred[0], d1 = pixels[1] - pred[1];
    const int d2 = pixels[2] - pred[2], d3 = pixels[3] - pred[3];
    const int t0 = d0 + d1, t1 = d0 - d1, t2 = d2 + d3, t3 = d2 - d3;

    dct[0] = t0 + t2;
    dct[1] = t0 - t2;
    dct[2] = t1 + t3;
    dct[3] = t1 - t3;
}

/* The sixteen groups of four samples of a block in coding order: where a
 * group is, and where the group it is predicted from is next to it.  The
 * first one is predicted from the green plane, or from nothing. */
static const struct {
    int8_t x, y, pred_x, pred_y;
} fv_groups[16] = {
    { 0, 0,  0,  0 }, { 1, 0, -1, 0 }, { 0, 1, 0, -1 }, { 1, 1, -1, 0 },
    { 4, 0, -4,  0 }, { 5, 0, -1, 0 }, { 4, 1, 0, -1 }, { 5, 1, -1, 0 },
    { 0, 4,  0, -4 }, { 1, 4, -1, 0 }, { 0, 5, 0, -1 }, { 1, 5, -1, 0 },
    { 4, 4, -4,  0 }, { 5, 4, -1, 0 }, { 4, 5, 0, -1 }, { 5, 5, -1, 0 },
};

/* Code one plane of an intra macroblock: src is the macroblock in the
 * picture, dst the plane it is rebuilt in at (mbx, mby), green the rebuilt
 * green plane (NULL when this is it). */
static av_always_inline void fv_encode_intra(const uint8_t *src, int src_stride,
                                             uint8_t *dst, const uint8_t *green, int stride,
                                             int mbx, int mby, int coeff[64])
{
    uint8_t pred[4], pixels[4], out[4];

    for (int group = 0; group < 16; group++) {
        const int x = mbx + fv_groups[group].x;
        const int y = mby + fv_groups[group].y;
        int dct[4];

        if (group)
            fv_get_group(dst, stride, x + fv_groups[group].pred_x,
                         y + fv_groups[group].pred_y, pred);
        else if (green)
            fv_get_group(green, stride, x, y, pred);
        else
            memset(pred, 0, sizeof(pred));
        fv_get_group(src, src_stride, fv_groups[group].x, fv_groups[group].y, pixels);
        fv_dct4(pixels, pred, dct);
        for (int i = 0; i < 4; i++) {
            const int quantized = fv_quantize(dct[i], i);
            coeff[i * 16 + group] = quantized;
            dct[i] = quantized * fv_dequant[i];
        }
        fv_idct4(dct, pred, out);
        fv_put_group(dst, stride, x, y, out);
    }
}

/* Quantize the 64 coefficients of a predicted block's plane, weighing the
 * error of each choice against the bits of its code (after the trellis of
 * FFmpeg's MPEG encoders).  Returns the last coded position, -1 for none. */
typedef struct FVSurvivor {
    int pos;                    /* the run of zeros before the next coefficient starts here */
    int score;                  /* of the best way to code what comes before */
} FVSurvivor;

static int fv_quantize_trellis(const FastVideoEncContext *s, int dst[64])
{
    enum { ESC_LENGTH = 28, LAMBDA = 200 };
    const unsigned threshold1 = (1U << 18) - 1, threshold2 = threshold1 << 1;
    int run_tab[65], level_tab[65];
    /* The list is gone through four at a time, so that how often the loop
     * goes round changes rarely; the places past its end hold starts that
     * can never win. */
    enum { NEVER = 256 * 256 * 256 * 120 + (1 << 20) };
    FVSurvivor survivor[65 + 3];
    int first[2] = { 0 }, first_count = 0;      /* the choices for coefficient 0 */
    int last_run = 0, last_level = 0, last_score = 0, last_i = 0;
    int last_nonzero = -1, survivor_count = 1, dc, i2;

    for (int i = 63; i >= 0; i--)
        if ((unsigned)(dst[i] * fv_quant[i >> 4]) + threshold1 > threshold2) {
            last_nonzero = i;
            break;
        }
    if (last_nonzero < 0) {
        memset(dst, 0, 64 * sizeof(*dst));
        return -1;
    }

    survivor[0] = (FVSurvivor){ 0, 0 };
    survivor[1] = survivor[2] = survivor[3] = (FVSurvivor){ 0, NEVER };
    for (int i = 0; i <= last_nonzero; i++) {
        const int dct_coeff = FFABS(dst[i]);
        const int zero_distortion = dct_coeff * dct_coeff;
        const int dequant = fv_dequant[i >> 4];
        int best_score = 256 * 256 * 256 * 120;
        int level = dst[i] * fv_quant[i >> 4];
        int values[2], count = 1;

        /* the level it rounds down to and the one below, or one for a
         * coefficient that rounds to nothing */
        {
            const int sign = level >> 31;
            const int rounds = (unsigned)level + threshold1 > threshold2;

            level     = rounds ? FFABS(level) >> 18 : 1;
            values[0] = (level ^ sign) - sign;
            values[1] = ((level - 1) ^ sign) - sign;
            count     = rounds ? FFMIN(level, 2) : 1;
        }
        if (!i) {
            first[0] = values[0];
            first[1] = values[1];
            first_count = count;
        }

        for (int k = 0; k < count; k++) {
            const int value = values[k];
            const int unquant = FFABS(value) * dequant;
            const int coded = value >= -64 && value < 64;
            /* an escape costs the same wherever the run starts */
            const uint32_t *const cost = s->cost[coded ? value + 64 : 128];
            const int distortion = (unquant - dct_coeff) * (unquant - dct_coeff) - zero_distortion +
                                   (coded ? 0 : ESC_LENGTH * LAMBDA);
            int best = best_score, best_run = 0;
            int last = last_score, run_last = 0;

            /* The best way to code this value when more coefficients follow
             * (best) and when none do (last), over the places the run before
             * it can start.  The comparisons are kept out of the way of the
             * branch predictor: which start wins is close to random. */
#define FV_TRY(j) do {                                                  \
                const int run   = i - survivor[j].pos;                  \
                const int base  = distortion + survivor[j].score;       \
                const int score = base + (cost[run] & 0xFFFF);          \
                const int score_last = base + (cost[run] >> 16);        \
                                                                        \
                best_run = score < best ? run : best_run;               \
                best     = score < best ? score : best;                 \
                run_last = score_last < last ? run : run_last;          \
                last     = score_last < last ? score_last : last;       \
            } while (0)
            for (int j = (survivor_count + 3) & ~3; j > 0; j -= 4) {
                FV_TRY(j - 1);
                FV_TRY(j - 2);
                FV_TRY(j - 3);
                FV_TRY(j - 4);
            }
#undef FV_TRY
            if (best < best_score) {
                best_score = best;
                run_tab[i + 1] = best_run;
                level_tab[i + 1] = value;
            }
            if (last < last_score) {
                last_score = last;
                last_run = run_last;
                last_level = value;
                last_i = i + 1;
            }
        }

        for (; survivor_count; survivor_count--)
            if (survivor[survivor_count - 1].score <=
                best_score + (last_nonzero <= 27 ? 0 : LAMBDA))
                break;
        survivor[survivor_count++] = (FVSurvivor){ i + 1, best_score };
        for (int j = 0; j < 3; j++)
            survivor[survivor_count + j] = (FVSurvivor){ i + 1, NEVER };
    }

    dc = FFABS(dst[0]);
    last_nonzero = last_i - 1;
    memset(dst, 0, 64 * sizeof(*dst));
    if (last_nonzero < 0)
        return -1;

    if (!last_nonzero) {
        int best_level = 0, best_score = dc * dc;

        for (int k = 0; k < first_count; k++) {
            const int value = first[k];
            const int unquant = ((FFABS(value) * fv_dequant[0] >> 3) + 4) >> 3 << 6;
            const int distortion = (unquant - dc) * (unquant - dc);
            const int score = distortion + (value >= -64 && value < 64
                ? s->bit_length[1][0][value + 64] * LAMBDA : ESC_LENGTH * LAMBDA);

            if (score < best_score) {
                best_score = score;
                best_level = value;
            }
        }
        dst[0] = best_level;
        return best_level ? 0 : -1;
    }

    i2 = last_i;
    dst[last_nonzero] = last_level;
    i2 -= last_run + 1;
    for (; i2 > 0; i2 -= run_tab[i2] + 1)
        dst[i2 - 1] = level_tab[i2];
    return last_nonzero;
}

/* Code one plane of a predicted macroblock against pred, rebuild it in out
 * and return the bits it takes. */
static int fv_encode_residual(const FastVideoEncContext *s, const uint8_t *src,
                              int src_stride, const uint8_t pred[64],
                              uint8_t out[64], int coeff[64])
{
    uint8_t p[4], pixels[4], rebuilt[4];
    int group = 0, last;

    for (int y3 = 0; y3 < 8; y3 += 4)
        for (int x3 = 0; x3 < 8; x3 += 4)
            for (int y2 = 0; y2 < 2; y2++)
                for (int x2 = 0; x2 < 2; x2++, group++) {
                    int dct[4];

                    fv_get_group(src, src_stride, x3 + x2, y3 + y2, pixels);
                    fv_get_group(pred, 8, x3 + x2, y3 + y2, p);
                    fv_dct4(pixels, p, dct);
                    for (int i = 0; i < 4; i++)
                        coeff[i * 16 + group] = dct[i];
                }
    last = fv_quantize_trellis(s, coeff);
    if (last < 0) {
        /* nothing coded: the block is its prediction */
        memcpy(out, pred, 64);
        return 1;
    }

    group = 0;
    for (int y3 = 0; y3 < 8; y3 += 4)
        for (int x3 = 0; x3 < 8; x3 += 4)
            for (int y2 = 0; y2 < 2; y2++)
                for (int x2 = 0; x2 < 2; x2++, group++) {
                    int dct[4];

                    fv_get_group(pred, 8, x3 + x2, y3 + y2, p);
                    for (int i = 0; i < 4; i++)
                        dct[i] = coeff[i * 16 + group] * fv_dequant[i];
                    fv_idct4(dct, p, rebuilt);
                    fv_put_group(out, 8, x3 + x2, y3 + y2, rebuilt);
                }
    return 1 + fv_dct_bits(s, coeff);
}

static av_always_inline int fv_sad64(const uint8_t *a, int a_stride, const uint8_t *b)
{
    int sum = 0;

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            sum += FFABS(a[y * a_stride + x] - b[y * 8 + x]);
    return sum;
}

/* What a vector leaves of one plane of a block: the same number as the
 * prediction built with fv_motion_tile() and compared with the picture
 * gives, without building it where the block lies inside the plane. */
static av_always_inline int fv_vector_error(const uint8_t *src, const uint8_t *ref,
                                            int width, int height, int hx, int hy)
{
    const int fx = hx & 1, fy = hy & 1;
    const int x0 = hx >> 1, y0 = hy >> 1;
    uint8_t block[64];

    if (x0 >= 0 && y0 >= 0 && x0 + 7 + fx < width && y0 + 7 + fy < height) {
        const uint8_t *p = ref + y0 * width + x0;

        if (!fx && !fy) {
            int sum = 0;

            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    sum += FFABS(src[y * width + x] - p[y * width + x]);
            return sum;
        }
    }
    fv_motion_tile(ref, width, height, hx, hy, block);
    return fv_sad64(src, width, block);
}

typedef struct FVSearch {
    const FastVideoEncContext *s;
    const uint8_t *src[3];      /* the macroblock in the picture */
    uint8_t *const *ref;
    FVMotionVector center, cheap;
    /* The diamonds overlap from one step to the next: what the vectors
     * already tried scored, by the low bits of the vector. */
    struct {
        FVMotionVector vec;
        int score;
    } tried[64];
} FVSearch;

static av_always_inline void fv_forget_vectors(FVSearch *c)
{
    /* a slot holds vectors with its own low bits: no vector has these */
    for (int i = 0; i < FF_ARRAY_ELEMS(c->tried); i++)
        c->tried[i].vec.x = i + 1;
}

/* The error of a vector, or some smaller number above limit once it is
 * known to be above limit. */
static int fv_distortion(FVSearch *c, FVMotionVector vec, int limit)
{
    const FastVideoEncContext *s = c->s;
    const int slot = (vec.x & 7) | (vec.y & 7) << 3;
    int score = 0;

    /* The limit only ever comes down, so a score that was cut short above
     * the limit of its time is above this one as well. */
    if (c->tried[slot].vec.x == vec.x && c->tried[slot].vec.y == vec.y)
        return c->tried[slot].score;

    /* The player draws the picture as two halves of a texture, and a block
     * may not reach from one into what lies past the other. */
    if ((c->center.y < (s->height >> 1) * 2 && vec.y > (s->height - 8) * 2) ||
        (c->center.y >= (s->height >> 1) * 2 && vec.y < 0))
        return 999999;
    for (int p = 0; p < 3; p++) {
        score += fv_vector_error(c->src[p], c->ref[p], s->width, s->height, vec.x, vec.y);
        if (score > limit)
            break;
    }
    c->tried[slot].vec   = vec;
    c->tried[slot].score = score;
    return score;
}

static int fv_vector_bits(const FVSearch *c, FVMotionVector vec)
{
    if (vec.x == c->cheap.x && vec.y == c->cheap.y)
        return 1;
    return 1 + fv_signed_varint_bits(vec.x - c->cheap.x) +
               fv_signed_varint_bits(vec.y - c->cheap.y);
}

/* Large then small diamond search from the predicted vector, in half
 * samples; the vector kept is the one with the best error plus four times
 * its bits, while the search itself walks towards the least error. */
static FVMotionVector fv_find_motion_vector(FVSearch *c)
{
    static const FVMotionVector diamond[2][8] = {
        { { -2, 0 }, { -1, -1 }, { 0, -2 }, { 1, -1 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { -1, 1 } },
        { { -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 } },
    };
    enum { LAMBDA = 4 };
    /* A vector changes nothing below unless its error is at most the least
     * so far, or at most what the best total leaves once its bits are paid
     * for; the planes after the one that takes it past both are not looked
     * at. */
#define FV_LIMIT(bits) FFMAX(best_score, best_rd - (bits) * LAMBDA)
    int best_score = (fv_forget_vectors(c), fv_distortion(c, c->center, INT_MAX));
    int best_bits  = fv_vector_bits(c, c->center);
    int best_rd    = best_bits * LAMBDA + best_score;
    FVMotionVector best_rd_vec = c->center, best_vec = c->center, search_center;
    int bits  = fv_vector_bits(c, c->cheap);
    int score = fv_distortion(c, c->cheap, FV_LIMIT(bits));
    int rd    = bits * LAMBDA + score;

    if (score < best_score || (score == best_score && bits < best_bits)) {
        best_score = score;
        best_bits  = bits;
        best_vec   = c->cheap;
    }
    if (rd < best_rd || (rd == best_rd && score < best_score)) {
        best_rd     = rd;
        best_rd_vec = c->cheap;
    }

    search_center = c->cheap;
    for (int pass = 0; pass < 2; pass++) {
        const int directions = pass ? 4 : 8;
        int count = 0, best_idx;

        do {
            best_idx = -1;
            for (int i = 0; i < directions; i++) {
                const FVMotionVector vec = { search_center.x + diamond[pass][i].x,
                                             search_center.y + diamond[pass][i].y };

                bits  = fv_vector_bits(c, vec);
                score = fv_distortion(c, vec, FV_LIMIT(bits));
                rd    = bits * LAMBDA + score;
                if (score < best_score || (score == best_score && bits < best_bits)) {
                    best_score = score;
                    best_bits  = bits;
                    best_vec   = vec;
                    best_idx   = i;
                }
                if (rd < best_rd || (rd == best_rd && score < best_score)) {
                    best_rd     = rd;
                    best_rd_vec = vec;
                }
                count++;
            }
            search_center = best_vec;
        } while (best_idx != -1 && count < 128);
    }
#undef FV_LIMIT
    return best_rd_vec;
}

static int fv_median(const FVMotionVector *left, const FVMotionVector *above,
                     const FVMotionVector *above_right, int y_component)
{
    int values[3], n = 0;

    if (left)
        values[n++] = y_component ? left->y : left->x;
    if (above)
        values[n++] = y_component ? above->y : above->x;
    if (above_right)
        values[n++] = y_component ? above_right->y : above_right->x;
    if (!n)
        return 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (values[j] < values[i])
                FFSWAP(int, values[i], values[j]);
    return values[n / 2];
}

static void fv_store_tile(uint8_t *plane, int stride, int x, int y, const uint8_t tile[64])
{
    for (int row = 0; row < 8; row++)
        memcpy(plane + (y + row) * stride + x, tile + row * 8, 8);
}

/* What a block costs: its error and its bits.  Single precision, one
 * operation at a time, as the reference computes it; it does not decrease
 * when either number grows. */
static av_always_inline float fv_block_cost(int diff, int bits)
{
    const float lambda = 0.35f;
    float cost = diff * lambda;

    cost += bits;
    return cost;
}

/* Work out one block of a predicted frame at (x, y): l, a and ar are the
 * blocks to its left, above and above right, where there are any. */
static void fv_encode_p_block(const FastVideoEncContext *s, int x, int y,
                              const FVBlock *l, const FVBlock *a,
                              const FVBlock *ar, FVBlock *blk)
{
    static const uint8_t plane_order[3] = { 1, 0, 2 };   /* green, red, blue */
    uint8_t *const *ref = s->plane[s->cur ^ 1];
    uint8_t *const *dec = s->plane[s->cur];
    const FVMotionVector *lv = l ? &l->vec : NULL, *av = a ? &a->vec : NULL;
    const FVMotionVector *arv = ar ? &ar->vec : NULL;
    const FVMotionVector last_vec = { fv_median(lv, av, arv, 0), fv_median(lv, av, arv, 1) };
    FVSearch search = { s, { 0 }, ref, { x << 1, y << 1 },
                        { last_vec.x + (x << 1), last_vec.y + (y << 1) } };
    uint8_t pred[3][64], i_tile[3][64], p_tile[3][64];
    int i_coeff[3][64], p_coeff[3][64];
    int i_bits = 1, p_bits = 1, diff_i = 0, diff_p = 0, same;
    float i_cost, p_cost;
    FVMotionVector vec;

    for (int p = 0; p < 3; p++)
        search.src[p] = s->src[p] + y * s->width + x;
    vec = fv_find_motion_vector(&search);
    for (int p = 0; p < 3; p++)
        fv_motion_tile(ref[p], s->width, s->height, vec.x, vec.y, pred[p]);
    vec.x -= x << 1;
    vec.y -= y << 1;
    same = vec.x == last_vec.x && vec.y == last_vec.y;

    if (!same)
        p_bits += fv_signed_varint_bits(vec.x - last_vec.x) +
                  fv_signed_varint_bits(vec.y - last_vec.y);
    for (int k = 0; k < 3; k++) {
        const int p = plane_order[k];

        p_bits += fv_encode_residual(s, search.src[p], s->width, pred[p],
                                     p_tile[p], p_coeff[k]);
        diff_p += fv_sad64(search.src[p], s->width, p_tile[p]);
    }
    p_cost = fv_block_cost(diff_p, p_bits);

    /* The intra block costs no less than the planes coded so far plus the
     * fewest bits the others can take; once that is not below the cost of
     * the predicted block the rest of it need not be worked out, and where
     * the predicted block costs next to nothing none of it. */
    i_cost = fv_block_cost(0, i_bits + 3 * s->min_block_bits);
    for (int k = 0; k < 3 && i_cost < p_cost; k++) {
        const int p = plane_order[k];

        fv_encode_intra(search.src[p], s->width, i_tile[p],
                        k ? i_tile[1] : NULL, 8, 0, 0, i_coeff[k]);
        i_bits += fv_dct_bits(s, i_coeff[k]);
        diff_i += fv_sad64(search.src[p], s->width, i_tile[p]);
        i_cost  = fv_block_cost(diff_i, i_bits + (2 - k) * s->min_block_bits);
    }

    blk->intra = i_cost < p_cost;
    if (blk->intra) {
        memcpy(blk->coeff, i_coeff, sizeof(blk->coeff));
        for (int p = 0; p < 3; p++)
            fv_store_tile(dec[p], s->width, x, y, i_tile[p]);
        blk->vec = last_vec;
    } else {
        memcpy(blk->coeff, p_coeff, sizeof(blk->coeff));
        for (int p = 0; p < 3; p++)
            fv_store_tile(dec[p], s->width, x, y, p_tile[p]);
        blk->vec     = vec;
        blk->delta.x = vec.x - last_vec.x;
        blk->delta.y = vec.y - last_vec.y;
    }
}

static int fv_encode_p_row(AVCodecContext *avctx, void *arg, int row, int threadnr)
{
    FastVideoEncContext *s = avctx->priv_data;
    const int mb_width = s->width / 8;
    FVBlock *blocks = s->blocks + row * mb_width;
    const FVBlock *above = row ? blocks - mb_width : NULL;
    int intra_blocks = 0;

    for (int mb = 0; mb < mb_width; mb++) {
        if (row)
            ff_thread_progress_await(&s->progress[row - 1], FFMIN(mb + 2, mb_width));
        fv_encode_p_block(s, mb * 8, row * 8, mb ? &blocks[mb - 1] : NULL,
                          above ? &above[mb] : NULL,
                          above && mb + 1 < mb_width ? &above[mb + 1] : NULL,
                          &blocks[mb]);
        intra_blocks += blocks[mb].intra;
        ff_thread_progress_report(&s->progress[row], mb + 1);
    }
    s->intra_blocks[row] = intra_blocks;
    return 0;
}

/* Code the picture against the reference; returns the size of the frame in
 * out, with the number of intra blocks in *intra_blocks. */
static int fv_encode_p_frame(AVCodecContext *avctx, uint8_t *out, int out_size,
                             int *intra_blocks)
{
    FastVideoEncContext *s = avctx->priv_data;
    FVPutBitContext vpb = { s->vector_data, s->vector_size, 0, 0, 0 };
    FVPutBitContext dpb = { s->dct_data, s->dct_size, 0, 0, 0 };
    const int mb_count = s->width * s->height / 64;
    int ret = 0;

    for (int i = 0; i < s->nb_progress; i++)
        ff_thread_progress_reset(&s->progress[i]);
    avctx->execute2(avctx, fv_encode_p_row, NULL, NULL, s->height / 8);

    *intra_blocks = 0;
    for (int i = 0; i < s->height / 8; i++)
        *intra_blocks += s->intra_blocks[i];

    ret |= fv_put_bits(&vpb, 1, 1);
    for (int mb = 0; mb < mb_count; mb++) {
        const FVBlock *blk = &s->blocks[mb];

        if (blk->intra) {
            ret |= fv_put_bits(&dpb, 1, 1);
            ret |= fv_put_bits(&vpb, 1, 1);
            for (int k = 0; k < 3; k++)
                ret |= fv_encode_dct(s, &dpb, blk->coeff[k]);
        } else {
            ret |= fv_put_bits(&dpb, 0, 1);
            if (!blk->delta.x && !blk->delta.y) {
                ret |= fv_put_bits(&vpb, 1, 1);
            } else {
                ret |= fv_put_bits(&vpb, 0, 1);
                ret |= fv_put_signed_varint(&vpb, blk->delta.x);
                ret |= fv_put_signed_varint(&vpb, blk->delta.y);
            }
            for (int k = 0; k < 3; k++) {
                int coded = 0;

                for (int i = 0; i < 64; i++)
                    coded |= blk->coeff[k][i];
                ret |= fv_put_bits(&dpb, !!coded, 1);
                if (coded)
                    ret |= fv_encode_dct(s, &dpb, blk->coeff[k]);
            }
        }
        if (ret < 0)
            return AVERROR_BUFFER_TOO_SMALL;
    }
    if (fv_flush_put_bits(&vpb) < 0 || fv_flush_put_bits(&dpb) < 0 ||
        vpb.pos + dpb.pos > out_size)
        return AVERROR_BUFFER_TOO_SMALL;
    memcpy(out, s->vector_data, vpb.pos);
    memcpy(out + vpb.pos, s->dct_data, dpb.pos);
    return vpb.pos + dpb.pos;
}

static int fv_encode_i_row(AVCodecContext *avctx, void *arg, int row, int threadnr)
{
    static const uint8_t plane_order[3] = { 1, 0, 2 };
    FastVideoEncContext *s = avctx->priv_data;
    uint8_t *const *dec = s->plane[s->cur];
    FVBlock *blocks = s->blocks + row * (s->width / 8);
    const int y = row * 8;

    for (int x = 0; x < s->width; x += 8)
        for (int k = 0; k < 3; k++) {
            const int p = plane_order[k];

            fv_encode_intra(s->src[p] + y * s->width + x, s->width, dec[p],
                            k ? dec[1] : NULL, s->width, x, y,
                            blocks[x >> 3].coeff[k]);
        }
    return 0;
}

static int fv_encode_i_frame(AVCodecContext *avctx, uint8_t *out, int out_size)
{
    FastVideoEncContext *s = avctx->priv_data;
    FVPutBitContext pb = { out, out_size, 0, 0, 0 };
    const int mb_count = s->width * s->height / 64;
    int last_gdc = 0, ret;

    avctx->execute2(avctx, fv_encode_i_row, NULL, NULL, s->height / 8);

    if (fv_put_bits(&pb, 0, 1) < 0 || fv_put_bits(&pb, 30, 6) < 0)
        return AVERROR_BUFFER_TOO_SMALL;
    for (int mb = 0; mb < mb_count; mb++)
        for (int k = 0; k < 3; k++) {
            int *coeff = s->blocks[mb].coeff[k];

            if (!k) {
                const int dc = coeff[0];
                coeff[0] -= last_gdc;
                last_gdc  = dc;
            }
            if ((ret = fv_encode_dct(s, &pb, coeff)) < 0)
                return ret;
        }
    if ((ret = fv_flush_put_bits(&pb)) < 0)
        return ret;
    return pb.pos;
}

static av_cold int fastvideo_encode_init(AVCodecContext *avctx)
{
    FastVideoEncContext *s = avctx->priv_data;
    const size_t plane_size = (size_t)avctx->width * avctx->height;
    const int64_t mb_count = (int64_t)avctx->width * avctx->height / 64;

    if (avctx->width <= 0 || avctx->height <= 0 ||
        (avctx->width & 7) || (avctx->height & 7)) {
        av_log(avctx, AV_LOG_ERROR,
               "Width and height must be positive multiples of eight\n");
        return AVERROR(EINVAL);
    }
    if (avctx->width != 256 || avctx->height > 192)
        av_log(avctx, AV_LOG_WARNING,
               "FastVideoDSPlayer shows pictures 256 wide and at most 192 high; "
               "%dx%d will not play there\n", avctx->width, avctx->height);
    if (mb_count > (INT_MAX - 64) / (3 * 64 * 28 / 8 + 16))
        return AVERROR(EINVAL);
    s->width = avctx->width;
    s->height = avctx->height;
    s->vector_size = mb_count * 10 + 8;
    s->dct_size    = mb_count * (3 * 64 * 28 / 8 + 2) + 8;
    s->vector_data = av_malloc(s->vector_size);
    s->dct_data    = av_malloc(s->dct_size);
    if (!s->vector_data || !s->dct_data)
        return AVERROR(ENOMEM);
    s->blocks       = av_calloc(mb_count, sizeof(*s->blocks));
    s->intra_blocks = av_calloc(s->height / 8, sizeof(*s->intra_blocks));
    s->progress     = av_calloc(s->height / 8, sizeof(*s->progress));
    if (!s->blocks || !s->intra_blocks || !s->progress)
        return AVERROR(ENOMEM);
    for (int i = 0; i < s->height / 8; i++) {
        /* with one thread the rows come in order and nothing has to wait */
        int ret = ff_thread_progress_init(&s->progress[i],
                                          (avctx->active_thread_type & FF_THREAD_SLICE) &&
                                          avctx->thread_count > 1);
        if (ret < 0)
            return ret;
        s->nb_progress++;
    }
    for (int p = 0; p < 3; p++) {
        s->src[p] = av_malloc(plane_size);
        s->plane[0][p] = av_malloc(plane_size);
        s->plane[1][p] = av_malloc(plane_size);
        if (!s->src[p] || !s->plane[0][p] || !s->plane[1][p])
            return AVERROR(ENOMEM);
    }

    memset(s->code, -1, sizeof(s->code));
    for (int i = 101; i >= 0; i--) {
        const int last = i >= 67;
        const int run = ff_mpeg4_intra_run[i];
        const int level = ff_mpeg4_intra_level[i];

        s->code[last][run][level] = i;
        s->max_level[last][run] = FFMAX(s->max_level[last][run], level);
        s->max_run[last][level] = FFMAX(s->max_run[last][level], run);
    }
    for (int last = 0; last < 2; last++)
        for (int run = 0; run < 64; run++)
            for (int value = -64; value < 64; value++) {
                int form;
                const int code = fv_code_form(s, last, run, FFABS(value), &form);
                static const uint8_t extra[4] = { 1, 10, 9, 28 };

                s->bit_length[last][run][value + 64] =
                    extra[form] + (form < 3 ? ff_mpeg4_intra_vlc[code][1] : 0);
            }
    for (int value = 0; value < 128; value++)
        for (int run = 0; run < 64; run++)
            s->cost[value][run] = s->bit_length[0][run][value] * 200 |
                                  s->bit_length[1][run][value] * 200 << 16;
    /* every block ends in one code for a last coefficient */
    s->min_block_bits = 28;
    for (int run = 0; run < 64; run++)
        for (int value = 0; value < 128; value++)
            s->min_block_bits = FFMIN(s->min_block_bits, s->bit_length[1][run][value]);
    s->gop = avctx->gop_size >= 0 ? avctx->gop_size : 250;
    return 0;
}

static av_cold int fastvideo_encode_close(AVCodecContext *avctx)
{
    FastVideoEncContext *s = avctx->priv_data;

    for (int p = 0; p < 3; p++) {
        av_freep(&s->src[p]);
        av_freep(&s->plane[0][p]);
        av_freep(&s->plane[1][p]);
    }
    for (int i = 0; i < s->nb_progress; i++)
        ff_thread_progress_destroy(&s->progress[i]);
    av_freep(&s->progress);
    av_freep(&s->intra_blocks);
    av_freep(&s->blocks);
    av_freep(&s->vector_data);
    av_freep(&s->dct_data);
    return 0;
}

static int fastvideo_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                  const AVFrame *frame, int *got_packet)
{
    FastVideoEncContext *s = avctx->priv_data;
    const int mb_count = s->width * s->height / 64;
    const int max_size = s->vector_size + s->dct_size;
    /* a frame with more intra blocks than this becomes a key frame */
    const int intra_limit = (int)(mb_count * 0.6f);
    int size = -1, key = 1, ret;

    if ((ret = ff_get_encode_buffer(avctx, pkt, max_size, 0)) < 0)
        return ret;
    for (int y = 0; y < s->height; y++) {
        const uint8_t *line = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < s->width; x++) {
            const unsigned pixel = AV_RL16(line + 2 * x);
            s->src[0][y * s->width + x] = (pixel & 31) << 3;
            s->src[1][y * s->width + x] = ((pixel >> 5) & 31) << 3;
            s->src[2][y * s->width + x] = ((pixel >> 10) & 31) << 3;
        }
    }

    s->cur ^= 1;
    if (s->have_reference && frame->pict_type != AV_PICTURE_TYPE_I &&
        s->gop_length < s->gop) {
        int intra_blocks;

        size = fv_encode_p_frame(avctx, pkt->data, pkt->size, &intra_blocks);
        if (size < 0)
            return size;
        if (intra_blocks > intra_limit)
            size = -1;
        else
            key = 0;
    }
    if (size < 0) {
        size = fv_encode_i_frame(avctx, pkt->data, pkt->size);
        if (size < 0)
            return size;
    }
    s->gop_length = key ? 0 : s->gop_length + 1;
    s->have_reference = 1;

    /* the player reads a frame into a 64 KiB buffer */
    if (size > 0x10000 && !s->warned_size) {
        av_log(avctx, AV_LOG_WARNING,
               "A frame of %d bytes is more than the 65536 FastVideoDSPlayer has room for\n", size);
        s->warned_size = 1;
    }
    av_shrink_packet(pkt, size);
    if (key)
        pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

/* the reference encoder starts a new key frame after 250 predicted ones */
static const FFCodecDefault fastvideo_defaults[] = {
    { "g", "250" },
    { NULL },
};

const FFCodec ff_fastvideo_encoder = {
    .p.name         = "fastvideo",
    CODEC_LONG_NAME("FastVideoDS Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_FASTVIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE |
                      AV_CODEC_CAP_SLICE_THREADS,
    .priv_data_size = sizeof(FastVideoEncContext),
    .init           = fastvideo_encode_init,
    FF_CODEC_ENCODE_CB(fastvideo_encode_frame),
    .close          = fastvideo_encode_close,
    .defaults       = fastvideo_defaults,
    CODEC_PIXFMTS(AV_PIX_FMT_BGR555LE),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
