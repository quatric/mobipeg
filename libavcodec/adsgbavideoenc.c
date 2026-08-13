/*
 * ADS-era (Majesco) GBA Video encoder
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

/**
 * @file
 * Writes what adsgbavideo.c reads: a vector-quantised, all-intra stream whose
 * frames are nothing but 8-bit indices into a 256-entry codebook.
 *
 * One packet is one chunk, and a chunk is a codebook plus the index planes of
 * however many frames share it (-chunk_frames). Since frames carry no temporal
 * prediction at all, the only thing that makes the format compress is that
 * consecutive frames tend to reuse the same entries, so the index planes are
 * highly repetitive - which is exactly what the entropy coder behind them is
 * good at.
 *
 * The colour transform is the inverse of the decoder's, which is itself the
 * IWRAM converter at 0x030004f8:
 *     R = clip(Y + 2*Cr)   G = clip(Y - Cr - Cb/2)   B = clip(Y + 2*Cb)
 * Solving those three for Y gives 7Y = 2R + 4G + B, and the chroma pair falls
 * out of the R and B rows:
 *     Y = (2R + 4G + B) / 7   Cb = (B - Y) / 2   Cr = (R - Y) / 2
 *
 * Chroma is subsampled to one sample per block column. How many of those an
 * entry stores is per-mode: the even modes keep a single sample for the whole
 * block, the odd ones keep one per block row. Mode 5 is the exception and is
 * rejected - it declares two samples for a three-row block, so the decoder's
 * row index runs off the end of the entry into its neighbour, faithfully
 * reproducing a ROM quirk that there is no sane way to encode for.
 *
 * The codebook is built by k-means over every block in the chunk. Cost is
 * measured in approximate squared RGB error rather than raw sample distance,
 * which is why chroma carries a weight: a chroma sample is amplified about
 * twofold on its way back to RGB and covers blk_w pixels of a row, so its
 * squared error counts for 4 * blk_w * (blk_h / chroma_rows) times a luma
 * sample's.
 *
 * Both blobs in a chunk (codebook, index plane) go through whichever
 * compressor -compression selects: LZMA (the ADS-era / Dragon Ball GT
 * lineage, and the default here) or Inflate (the Hydrogen-era cartridges).
 * Either way the blob keeps the format's 8-byte
 * [uint32 uncompressed_size][uint32 params] prefix.
 */

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "adslzma.h"
#include "majesco.h"

#define FRAME_W    240
#define NB_ENTRIES 256

/* Block geometry and chroma count per mode, transcribed from the decoder. */
static const uint8_t block_dims[8][2] = {
    { 4, 4 }, { 4, 4 }, { 4, 3 }, { 4, 3 },
    { 3, 3 }, { 3, 3 }, { 4, 2 }, { 4, 2 },
};
static const uint8_t chroma_dims[8] = { 1, 4, 1, 3, 1, 2, 1, 2 };

typedef struct ADSEncContext {
    const AVClass *class;

    int mode;
    int chunk_frames;
    int kmeans_iters;
    int compression;            /* 0 = LZMA (ADS-era), 1 = Inflate (Hydrogen) */

    int blk_w, blk_h, chroma_rows;
    int grid_w, grid_h;
    int blocks_per_frame;
    int vec_dim;                /* blk_w*blk_h luma + 2*chroma_rows chroma */
    int w_chroma;               /* chroma weight in the distance metric */

    int16_t *vec;               /* every block of the buffered frames */
    int nb_vec;
    int nb_buffered;
    int64_t chunk_pts;

    int32_t *centroid;          /* NB_ENTRIES * vec_dim, in fixed point */
    int16_t *book;              /* NB_ENTRIES * vec_dim, rounded */
    uint8_t *index;             /* chunk_frames * blocks_per_frame */
    int64_t *sum;
    int     *count;
} ADSEncContext;

static av_cold int ads_enc_init(AVCodecContext *avctx)
{
    ADSEncContext *s = avctx->priv_data;

    if (s->mode == 5) {
        av_log(avctx, AV_LOG_ERROR,
               "mode 5 stores two chroma samples for a three-row block, which "
               "the decoder reads past the end of; pick another mode\n");
        return AVERROR(EINVAL);
    }

    s->blk_w       = block_dims[s->mode][0];
    s->blk_h       = block_dims[s->mode][1];
    s->chroma_rows = chroma_dims[s->mode];
    s->grid_w      = FRAME_W / s->blk_w;

    if (avctx->width != s->grid_w * s->blk_w) {
        av_log(avctx, AV_LOG_ERROR,
               "mode %d needs a width of %d, not %d\n",
               s->mode, s->grid_w * s->blk_w, avctx->width);
        return AVERROR(EINVAL);
    }
    if (avctx->height % s->blk_h) {
        av_log(avctx, AV_LOG_ERROR,
               "mode %d needs a height that is a multiple of %d, not %d\n",
               s->mode, s->blk_h, avctx->height);
        return AVERROR(EINVAL);
    }

    s->grid_h           = avctx->height / s->blk_h;
    s->blocks_per_frame = s->grid_w * s->grid_h;
    s->vec_dim          = s->blk_w * s->blk_h + 2 * s->chroma_rows;
    s->w_chroma         = 4 * s->blk_w * (s->blk_h / s->chroma_rows);

    /* The index plane's compressed size is a 19-bit word count and its
     * uncompressed size has to stay inside the decoder's 8 MB guard. */
    if ((int64_t)s->chunk_frames * s->blocks_per_frame > (1 << 23)) {
        av_log(avctx, AV_LOG_ERROR,
               "chunk_frames %d exceeds what one chunk can carry at %dx%d\n",
               s->chunk_frames, avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }

    s->vec      = av_malloc_array((size_t)s->chunk_frames * s->blocks_per_frame,
                                  s->vec_dim * sizeof(*s->vec));
    s->centroid = av_malloc_array(NB_ENTRIES, s->vec_dim * sizeof(*s->centroid));
    s->book     = av_malloc_array(NB_ENTRIES, s->vec_dim * sizeof(*s->book));
    s->index    = av_malloc_array(s->chunk_frames, s->blocks_per_frame);
    s->sum      = av_malloc_array(NB_ENTRIES, s->vec_dim * sizeof(*s->sum));
    s->count    = av_malloc_array(NB_ENTRIES, sizeof(*s->count));
    if (!s->vec || !s->centroid || !s->book || !s->index || !s->sum ||
        !s->count)
        return AVERROR(ENOMEM);

    av_log(avctx, AV_LOG_VERBOSE,
           "mode %d: %dx%d blocks, %dx%d grid, %d chroma row(s) per entry, "
           "%d frames per codebook\n",
           s->mode, s->blk_w, s->blk_h, s->grid_w, s->grid_h, s->chroma_rows,
           s->chunk_frames);

    return 0;
}

static av_cold int ads_enc_close(AVCodecContext *avctx)
{
    ADSEncContext *s = avctx->priv_data;

    av_freep(&s->vec);
    av_freep(&s->centroid);
    av_freep(&s->book);
    av_freep(&s->index);
    av_freep(&s->sum);
    av_freep(&s->count);
    return 0;
}

/**
 * Turn one RGB frame into block vectors, appending them to the chunk.
 *
 * A vector is the block's luma in raster order, then its Cb samples, then its
 * Cr samples, which is the order an entry stores them in.
 */
static void gather_blocks(ADSEncContext *s, const AVFrame *frame)
{
    const int bw = s->blk_w, bh = s->blk_h, cr = s->chroma_rows;
    const int rows_per_chroma = bh / cr;
    int16_t *v = s->vec + (size_t)s->nb_vec * s->vec_dim;

    for (int by = 0; by < s->grid_h; by++) {
        for (int bx = 0; bx < s->grid_w; bx++, v += s->vec_dim) {
            int cb_acc[4] = { 0 }, cr_acc[4] = { 0 };

            for (int j = 0; j < bh; j++) {
                const uint8_t *src = frame->data[0] +
                                     (by * bh + j) * frame->linesize[0] +
                                     bx * bw * 3;

                for (int i = 0; i < bw; i++) {
                    int r = src[3 * i], g = src[3 * i + 1], b = src[3 * i + 2];
                    int y = (2 * r + 4 * g + b + 3) / 7;

                    y = av_clip_uint8(y);
                    v[j * bw + i] = y;

                    /* One chroma sample covers blk_w pixels of this row, and
                     * rows_per_chroma rows of the block. */
                    cb_acc[j / rows_per_chroma] += b - y;
                    cr_acc[j / rows_per_chroma] += r - y;
                }
            }

            for (int k = 0; k < cr; k++) {
                int n = 2 * bw * rows_per_chroma;

                /* Halved on the way in, doubled on the way out. */
                v[bw * bh + k]      = av_clip_int8((cb_acc[k] +
                                                    (cb_acc[k] > 0 ? n / 2 : -n / 2)) / n);
                v[bw * bh + cr + k] = av_clip_int8((cr_acc[k] +
                                                    (cr_acc[k] > 0 ? n / 2 : -n / 2)) / n);
            }
        }
    }
    s->nb_vec += s->blocks_per_frame;
}

static av_always_inline int64_t vec_cost(const ADSEncContext *s,
                                         const int16_t *a, const int16_t *b,
                                         int luma_dim)
{
    int64_t cost = 0;

    for (int i = 0; i < luma_dim; i++) {
        int d = a[i] - b[i];

        cost += (int64_t)d * d;
    }
    for (int i = luma_dim; i < s->vec_dim; i++) {
        int d = a[i] - b[i];

        cost += (int64_t)s->w_chroma * d * d;
    }
    return cost;
}

/** Nearest entry, by the weighted metric. */
static int nearest(const ADSEncContext *s, const int16_t *v, int64_t *out_cost)
{
    const int luma_dim = s->blk_w * s->blk_h;
    int64_t best = INT64_MAX;
    int best_e = 0;

    for (int e = 0; e < NB_ENTRIES; e++) {
        int64_t c = vec_cost(s, v, s->book + (size_t)e * s->vec_dim, luma_dim);

        if (c < best) {
            best   = c;
            best_e = e;
            if (!c)
                break;
        }
    }
    if (out_cost)
        *out_cost = best;
    return best_e;
}

/**
 * Seed the codebook with blocks spread across the luma range.
 *
 * k-means only refines, so the seed decides which minimum it lands in. Sorting
 * by block brightness and sampling evenly costs one pass and reliably beats
 * picking the first 256 blocks, which on a fade-in would all be the same
 * colour.
 */
static void seed_codebook(ADSEncContext *s)
{
    const int luma_dim = s->blk_w * s->blk_h;
    int *order = av_malloc_array(s->nb_vec, sizeof(*order));
    int32_t *key = av_malloc_array(s->nb_vec, sizeof(*key));

    if (!order || !key) {
        /* Seeding is only a heuristic: fall back to an even stride. */
        for (int e = 0; e < NB_ENTRIES; e++) {
            int src = (int)((int64_t)e * s->nb_vec / NB_ENTRIES);

            memcpy(s->book + (size_t)e * s->vec_dim,
                   s->vec + (size_t)src * s->vec_dim,
                   s->vec_dim * sizeof(*s->book));
        }
        av_freep(&order);
        av_freep(&key);
        return;
    }

    for (int i = 0; i < s->nb_vec; i++) {
        const int16_t *v = s->vec + (size_t)i * s->vec_dim;
        int32_t acc = 0;

        for (int k = 0; k < luma_dim; k++)
            acc += v[k];
        /* Fold a little chroma in so two equally bright but differently
         * coloured blocks do not collapse onto one seed. */
        for (int k = luma_dim; k < s->vec_dim; k++)
            acc += 4 * v[k];
        key[i]   = acc;
        order[i] = i;
    }

    /* Counting sort over the key range, which is small and bounded. */
    {
        int32_t lo = key[0], hi = key[0];
        int nb_bucket, *bucket;

        for (int i = 1; i < s->nb_vec; i++) {
            lo = FFMIN(lo, key[i]);
            hi = FFMAX(hi, key[i]);
        }
        nb_bucket = hi - lo + 2;
        bucket = av_calloc(nb_bucket, sizeof(*bucket));
        if (bucket) {
            for (int i = 0; i < s->nb_vec; i++)
                bucket[key[i] - lo + 1]++;
            for (int i = 1; i < nb_bucket; i++)
                bucket[i] += bucket[i - 1];
            for (int i = 0; i < s->nb_vec; i++)
                order[bucket[key[i] - lo]++] = i;
            av_freep(&bucket);
        }
    }

    for (int e = 0; e < NB_ENTRIES; e++) {
        int src = order[(int)((int64_t)e * s->nb_vec / NB_ENTRIES)];

        memcpy(s->book + (size_t)e * s->vec_dim,
               s->vec + (size_t)src * s->vec_dim,
               s->vec_dim * sizeof(*s->book));
    }

    av_freep(&order);
    av_freep(&key);
}

/** Lloyd iterations: assign every block, then move each entry to its mean. */
static void build_codebook(ADSEncContext *s)
{
    const int luma_dim = s->blk_w * s->blk_h;

    seed_codebook(s);

    for (int it = 0; it < s->kmeans_iters; it++) {
        int moved = 0;

        memset(s->sum, 0, NB_ENTRIES * s->vec_dim * sizeof(*s->sum));
        memset(s->count, 0, NB_ENTRIES * sizeof(*s->count));

        for (int i = 0; i < s->nb_vec; i++) {
            const int16_t *v = s->vec + (size_t)i * s->vec_dim;
            int e = nearest(s, v, NULL);

            s->index[i] = e;
            s->count[e]++;
            for (int k = 0; k < s->vec_dim; k++)
                s->sum[(size_t)e * s->vec_dim + k] += v[k];
        }

        for (int e = 0; e < NB_ENTRIES; e++) {
            int16_t *b = s->book + (size_t)e * s->vec_dim;

            if (!s->count[e])
                continue;
            for (int k = 0; k < s->vec_dim; k++) {
                int64_t acc = s->sum[(size_t)e * s->vec_dim + k];
                int n = s->count[e];
                int val = (int)((acc + (acc > 0 ? n / 2 : -n / 2)) / n);

                val = k < luma_dim ? av_clip_uint8(val) : av_clip_int8(val);
                if (b[k] != val)
                    moved = 1;
                b[k] = val;
            }
        }

        if (!moved)
            break;
    }

    /* One last assignment so the indices match the codebook that ships. */
    for (int i = 0; i < s->nb_vec; i++)
        s->index[i] = nearest(s, s->vec + (size_t)i * s->vec_dim, NULL);
}

/** Byte-wise difference, the inverse of the decoder's running sum. */
static void delta(uint8_t *p, int len)
{
    for (int i = len - 1; i > 0; i--)
        p[i] = (uint8_t)(p[i] - p[i - 1]);
}

/** Lay the codebook out as the decoder indexes it, then delta code it. */
static void serialise_codebook(ADSEncContext *s, uint8_t *dst)
{
    const int luma_dim = s->blk_w * s->blk_h;
    const int cr = s->chroma_rows;
    const int luma_region = NB_ENTRIES * luma_dim;
    uint8_t *cb_base = dst + luma_region;
    uint8_t *cr_base = cb_base + NB_ENTRIES * cr;

    for (int e = 0; e < NB_ENTRIES; e++) {
        const int16_t *v = s->book + (size_t)e * s->vec_dim;

        for (int k = 0; k < luma_dim; k++)
            dst[e * luma_dim + k] = (uint8_t)v[k];
        for (int k = 0; k < cr; k++) {
            cb_base[e * cr + k] = (uint8_t)(int8_t)v[luma_dim + k];
            cr_base[e * cr + k] = (uint8_t)(int8_t)v[luma_dim + cr + k];
        }
    }

    /* Luma is one run; Cb and Cr are a second, Cb flowing into Cr. */
    delta(dst, luma_region);
    delta(dst + luma_region, 2 * NB_ENTRIES * cr);
}

/**
 * Compress a blob and pad it out to a whole number of words, because the chunk
 * header states both sizes in words. Trailing padding is invisible to the
 * decompressor, which stops on the uncompressed size in the blob's own prefix.
 *
 * Which compressor to use is the same choice the demuxer's -compression
 * option gives a reader: 0 for the LZMA that the Dragon Ball GT / ADS-era
 * lineage runs, 1 for the DEFLATE-shaped Inflate the Hydrogen-era carts
 * (Dora the Explorer) use instead. Both write the same 8-byte
 * [uint32 uncompressed_size][uint32 params] prefix.
 */
static int compress_padded(ADSEncContext *s, const uint8_t *src, int src_size,
                           uint8_t **dst, int *dst_size)
{
    uint8_t *buf;
    int size, ret;

    ret = s->compression ? ff_majesco_deflate(src, src_size, &buf, &size)
                         : ff_ads_lzma_encode_blob(src, src_size, &buf, &size);
    if (ret < 0)
        return ret;

    if (size & 3) {
        int padded = (size + 3) & ~3;
        uint8_t *grown = av_realloc(buf, padded);

        if (!grown) {
            av_freep(&buf);
            return AVERROR(ENOMEM);
        }
        memset(grown + size, 0, padded - size);
        buf  = grown;
        size = padded;
    }

    *dst      = buf;
    *dst_size = size;
    return size;
}

/** Emit the buffered frames as one chunk. */
static int flush_chunk(AVCodecContext *avctx, AVPacket *pkt, int *got_packet)
{
    ADSEncContext *s = avctx->priv_data;
    const int cbook_size = NB_ENTRIES * (s->blk_w * s->blk_h +
                                         2 * s->chroma_rows);
    uint8_t *raw = NULL, *blob_a = NULL, *blob_b = NULL;
    int size_a = 0, size_b = 0, ret;

    if (!s->nb_buffered)
        return 0;

    build_codebook(s);

    raw = av_malloc(cbook_size);
    if (!raw)
        return AVERROR(ENOMEM);
    serialise_codebook(s, raw);

    ret = compress_padded(s, raw, cbook_size, &blob_a, &size_a);
    av_freep(&raw);
    if (ret < 0)
        return ret;

    ret = compress_padded(s, s->index, s->nb_vec, &blob_b, &size_b);
    if (ret < 0) {
        av_freep(&blob_a);
        return ret;
    }

    /* The header packs the codebook's word count into 13 bits and the index
     * plane's into 19. */
    if (size_a / 4 > 0x1FFF || size_b / 4 > 0x7FFFF) {
        av_log(avctx, AV_LOG_ERROR,
               "chunk does not fit the header's size fields (%d/%d bytes)\n",
               size_a, size_b);
        av_freep(&blob_a);
        av_freep(&blob_b);
        return AVERROR(EINVAL);
    }

    ret = ff_get_encode_buffer(avctx, pkt, 8 + size_a + size_b, 0);
    if (ret < 0) {
        av_freep(&blob_a);
        av_freep(&blob_b);
        return ret;
    }

    AV_WL32(pkt->data,     (uint32_t)s->mode | ((uint32_t)s->nb_buffered << 16));
    AV_WL32(pkt->data + 4, (uint32_t)(size_a / 4) |
                           ((uint32_t)(size_b / 4) << 13));
    memcpy(pkt->data + 8, blob_a, size_a);
    memcpy(pkt->data + 8 + size_a, blob_b, size_b);

    av_freep(&blob_a);
    av_freep(&blob_b);

    pkt->pts      = pkt->dts = s->chunk_pts;
    pkt->duration = s->nb_buffered;
    pkt->flags   |= AV_PKT_FLAG_KEY;
    *got_packet   = 1;

    av_log(avctx, AV_LOG_DEBUG,
           "chunk: %d frames, codebook %d B, index %d B (%d raw)\n",
           s->nb_buffered, size_a, size_b, s->nb_vec);

    s->nb_buffered = 0;
    s->nb_vec      = 0;

    return 0;
}

static int ads_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    ADSEncContext *s = avctx->priv_data;

    if (!frame)
        return flush_chunk(avctx, pkt, got_packet);

    if (!s->nb_buffered)
        s->chunk_pts = frame->pts;

    gather_blocks(s, frame);
    s->nb_buffered++;

    if (s->nb_buffered >= s->chunk_frames)
        return flush_chunk(avctx, pkt, got_packet);

    return 0;
}

#define OFFSET(x) offsetof(ADSEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption ads_options[] = {
    { "mode", "block geometry: even modes keep one chroma sample per block, "
              "odd ones one per row (5 is unencodable)",
      OFFSET(mode), AV_OPT_TYPE_INT, { .i64 = 7 }, 0, 7, VE },
    { "chunk_frames", "frames sharing one codebook; fewer is sharper, more is smaller",
      OFFSET(chunk_frames), AV_OPT_TYPE_INT, { .i64 = 8 }, 1, 1100, VE },
    { "kmeans_iters", "codebook refinement passes",
      OFFSET(kmeans_iters), AV_OPT_TYPE_INT, { .i64 = 6 }, 1, 64, VE },
    { "compression", "blob compressor: 0 LZMA (ADS-era), 1 Inflate (Hydrogen)",
      OFFSET(compression), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { NULL },
};

static const AVClass ads_encoder_class = {
    .class_name = "ads_gba encoder",
    .item_name  = av_default_item_name,
    .option     = ads_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_ads_gba_encoder = {
    .p.name         = "ads_gba",
    CODEC_LONG_NAME("ADS-era GBA Video (Majesco)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ADS_GBA,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .priv_data_size = sizeof(ADSEncContext),
    .p.priv_class   = &ads_encoder_class,
    .init           = ads_enc_init,
    .close          = ads_enc_close,
    FF_CODEC_ENCODE_CB(ads_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_RGB24),
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
