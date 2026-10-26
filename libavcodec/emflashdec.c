/*
 * Em-Flash (Pixter Multimedia .mo fla2/fla3) video decoder
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
 * Renders an Em-Flash movie frame by frame.
 *
 * The movie is the extradata, an uncompressed SWF file (the moc3 demuxer
 * rebuilds it around the descrambled payload), and each packet is the LE32
 * number of the frame to show.  A packet can also be a whole SWF file, then
 * its pts picks the frame.  The player state only moves forward, so asking
 * for an earlier frame restarts the movie and steps up to it.
 */

#include <math.h>
#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "emflash.h"

typedef struct EmFlashContext {
    const AVClass *class;
    EFMovie *movie;
    uint8_t *swf;                       /* private copy, the movie points into it */
    int      swf_size;
    int64_t  cur;                       /* frame the movie state shows */
    double   autoclick;
    int      seed;
} EmFlashContext;

static void emflash_close_movie(EmFlashContext *s)
{
    ff_emflash_close(s->movie);
    s->movie = NULL;
}

static int emflash_open_movie(AVCodecContext *avctx)
{
    EmFlashContext *s = avctx->priv_data;
    int w, h, ret;

    emflash_close_movie(s);
    s->movie = ff_emflash_open(s->swf, s->swf_size, avctx->width, avctx->height);
    if (!s->movie) {
        av_log(avctx, AV_LOG_ERROR, "invalid or unsupported SWF data\n");
        return AVERROR_INVALIDDATA;
    }
    ff_emflash_set_seed(s->movie, s->seed);
    if (s->autoclick > 0)
        ff_emflash_set_autoclick(s->movie, FFMAX(1, lrint(s->autoclick * ff_emflash_get_rate(s->movie))));
    ff_emflash_get_size(s->movie, &w, &h);
    if (w != avctx->width || h != avctx->height) {
        ret = ff_set_dimensions(avctx, w, h);
        if (ret < 0)
            return ret;
    }
    s->cur = 0;
    return 0;
}

static int emflash_set_movie(AVCodecContext *avctx, const uint8_t *data, int size)
{
    EmFlashContext *s = avctx->priv_data;

    if (s->swf && size == s->swf_size && !memcmp(data, s->swf, size))
        return 0;
    av_freep(&s->swf);
    s->swf = av_memdup(data, size);
    if (!s->swf)
        return AVERROR(ENOMEM);
    s->swf_size = size;
    return emflash_open_movie(avctx);
}

static av_cold int emflash_init(AVCodecContext *avctx)
{
    avctx->pix_fmt = AV_PIX_FMT_RGB0;
    if (avctx->extradata_size >= 12)
        return emflash_set_movie(avctx, avctx->extradata, avctx->extradata_size);
    return 0;
}

static int emflash_decode(AVCodecContext *avctx, AVFrame *frame,
                          int *got_frame, AVPacket *pkt)
{
    EmFlashContext *s = avctx->priv_data;
    int64_t target;
    int ret;

    if (pkt->size >= 12 && !memcmp(pkt->data, "FWS", 3)) {
        ret = emflash_set_movie(avctx, pkt->data, pkt->size);
        if (ret < 0)
            return ret;
        target = pkt->pts != AV_NOPTS_VALUE && pkt->pts >= 0 ? pkt->pts : s->cur;
    } else if (pkt->size >= 4) {
        target = AV_RL32(pkt->data);
    } else {
        return AVERROR_INVALIDDATA;
    }
    if (!s->movie)
        return AVERROR_INVALIDDATA;
    if (target < s->cur) {
        ret = emflash_open_movie(avctx);
        if (ret < 0)
            return ret;
    }
    for (; s->cur < target; s->cur++)
        ff_emflash_step(s->movie);

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;
    ff_emflash_render(s->movie, frame->data[0], frame->linesize[0]);
    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags    |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;

    return pkt->size;
}

static av_cold int emflash_close(AVCodecContext *avctx)
{
    EmFlashContext *s = avctx->priv_data;

    emflash_close_movie(s);
    av_freep(&s->swf);
    return 0;
}

#define OFFSET(x) offsetof(EmFlashContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption emflash_options[] = {
    { "autoclick", "press a button after it has been on stage this many seconds (0 = never)",
      OFFSET(autoclick), AV_OPT_TYPE_DOUBLE, { .dbl = 0 }, 0, 3600, VD },
    { "seed", "seed of the movie's random numbers",
      OFFSET(seed), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, INT_MAX, VD },
    { NULL },
};

static const AVClass emflash_class = {
    .class_name = "emflash",
    .item_name  = av_default_item_name,
    .option     = emflash_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_emflash_decoder = {
    .p.name         = "emflash",
    CODEC_LONG_NAME("Em-Flash (Pixter Multimedia Flash movie)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_EMFLASH,
    .p.priv_class   = &emflash_class,
    .priv_data_size = sizeof(EmFlashContext),
    .init           = emflash_init,
    FF_CODEC_DECODE_CB(emflash_decode),
    .close          = emflash_close,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
