/*
 * ActImagine original GBA VXGB video decoder
 * Copyright (c) 2026 the FFmpeg developers
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
 * Decoder wrapper for the original VXGB revision used by early ActImagine
 * GBA Video cartridges. A demuxer packet is one independently decodable seek
 * segment containing several consecutive frames in one unaligned bitstream.
 */

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "gba_vx.h"
#include "golomb.h"
#include "vx.h"

typedef struct GBAVXContext {
    AVPacket *packet;
    VXPic pic[4];
    uint8_t *arena;
    GetBitContext gb;
    uint16_t qtab[3];
    int cur;
    int nb_frames;
    int next_frame;
    int valid_bits;
    int vxpp;
    const uint8_t *vxpp_vlc;
    const uint8_t *vxpp_value;
    const uint8_t *vxpp_run;
    int64_t segment_pts;
} GBAVXContext;

#define GBA_STRIDE       0x100
#define GBA_LUMA_ALLOC   0xA000
#define GBA_CHROMA_ALLOC 0x5000
#define GBA_FRAME_SLOT   (GBA_LUMA_ALLOC + GBA_CHROMA_ALLOC)
#define GBA_ARENA_MARGIN (160 * GBA_STRIDE)
#define GBA_ARENA_SIZE   (2 * GBA_ARENA_MARGIN + 4 * GBA_FRAME_SLOT)

static int ensure_arena(GBAVXContext *s)
{
    if (!s->arena) {
        s->arena = av_malloc(GBA_ARENA_SIZE);
        if (!s->arena)
            return AVERROR(ENOMEM);

        for (int i = 0; i < 4; i++) {
            uint8_t *base = s->arena + GBA_ARENA_MARGIN + i * GBA_FRAME_SLOT;

            s->pic[i].data[0] = base;
            s->pic[i].data[1] = base + GBA_LUMA_ALLOC;
            s->pic[i].data[2] = base + GBA_LUMA_ALLOC + 1;
            for (int p = 0; p < 3; p++)
                s->pic[i].linesize[p] = GBA_STRIDE;
        }
    }

    /* The hardware permits motion reads to cross plane and frame-slot
     * boundaries, so preserve its one contiguous allocation and fixed pitch. */
    memset(s->arena, 0x80, GBA_ARENA_SIZE);
    return 0;
}

static void discard_segment(GBAVXContext *s)
{
    av_packet_unref(s->packet);
    s->nb_frames = s->next_frame = 0;
    s->valid_bits = 0;
}

static int consume_tail_padding(GBAVXContext *s)
{
    int remaining = s->valid_bits - get_bits_count(&s->gb);

    /* The final video region is rounded up to the cartridge's 0x200-byte
     * alignment. It has no following seek offset to identify the exact end. */
    if (remaining <= 0 || remaining > 4095)
        return AVERROR_INVALIDDATA;
    while (remaining) {
        int bits = FFMIN(remaining, 32);

        if (get_bits_long(&s->gb, bits))
            return AVERROR_INVALIDDATA;
        remaining -= bits;
    }
    return 0;
}

static int load_segment(AVCodecContext *avctx, GBAVXContext *s)
{
    const uint8_t *buf;
    int payload_size, leading_bits, ret;

    discard_segment(s);
    ret = ff_decode_get_packet(avctx, s->packet);
    if (ret < 0)
        return ret;

    buf = s->packet->data;
    if (s->packet->size < GBA_VX_PACKET_HEADER_SIZE ||
        AV_RL32(buf) != GBA_VX_PACKET_MAGIC)
        goto invalid;

    leading_bits = AV_RL32(buf + 4);
    s->valid_bits = AV_RL32(buf + 8);
    s->nb_frames  = AV_RL32(buf + 12);
    payload_size  = s->packet->size - GBA_VX_PACKET_HEADER_SIZE;
    if (leading_bits >= 16 || s->valid_bits <= leading_bits ||
        s->valid_bits > (int64_t)payload_size * 8 ||
        s->nb_frames <= 0 || s->nb_frames > (1 << 20))
        goto invalid;

    ret = init_get_bits8(&s->gb, buf + GBA_VX_PACKET_HEADER_SIZE, payload_size);
    if (ret < 0)
        goto fail;
    skip_bits(&s->gb, leading_bits);

    if (s->vxpp) {
        int quantizer = get_ue_golomb_31(&s->gb);

        if (ff_vx_calc_qtab(quantizer, s->qtab) < 0)
            goto invalid;
    }

    if ((ret = ensure_arena(s)) < 0)
        goto fail;
    s->cur         = 0;
    s->next_frame  = 0;
    s->segment_pts = s->packet->pts;
    return 0;

invalid:
    ret = AVERROR_INVALIDDATA;
fail:
    discard_segment(s);
    return ret;
}

/* The cartridge uses this integer approximation of YCbCr-to-RGB. */
static void yuv_to_rgb24(AVCodecContext *avctx, const VXPic *yuv, AVFrame *rgb)
{
    for (int y = 0; y < avctx->height; y++) {
        const uint8_t *ly = yuv->data[0] + (ptrdiff_t)y * yuv->linesize[0];
        const uint8_t *lu = yuv->data[1] + (ptrdiff_t)(y >> 1) * yuv->linesize[1];
        const uint8_t *lv = yuv->data[2] + (ptrdiff_t)(y >> 1) * yuv->linesize[2];
        uint8_t *out = rgb->data[0] + (ptrdiff_t)y * rgb->linesize[0];

        for (int x = 0; x < avctx->width; x++) {
            int luma = ly[x];
            int u = lu[x & ~1] - 128;
            int v = lv[x & ~1] - 128;

            out[3 * x    ] = av_clip_uint8(luma + 2 * v);
            out[3 * x + 1] = av_clip_uint8(luma - (u >> 1) - v);
            out[3 * x + 2] = av_clip_uint8(luma + 2 * u);
        }
    }
}

static av_cold int gbavx_init(AVCodecContext *avctx)
{
    GBAVXContext *s = avctx->priv_data;
    uint32_t magic, quantizer;

    avctx->pix_fmt = AV_PIX_FMT_RGB24;
    if (avctx->width < 16 || avctx->height < 16 ||
        avctx->width % 16 || avctx->height % 16 ||
        avctx->extradata_size < GBA_VX_EXTRADATA_SIZE)
        return AVERROR_INVALIDDATA;

    magic     = AV_RL32(avctx->extradata);
    quantizer = AV_RL32(avctx->extradata + 4);
    if (magic == GBA_VX_MAGIC_VXGB) {
        if (ff_vx_calc_qtab(quantizer, s->qtab) < 0)
            return AVERROR_INVALIDDATA;
    } else if (magic == GBA_VX_MAGIC_VXPP &&
               avctx->extradata_size >= GBA_VX_VXPP_EXTRADATA_SIZE) {
        s->vxpp       = 1;
        s->vxpp_vlc   = avctx->extradata + GBA_VX_EXTRADATA_SIZE;
        s->vxpp_value = s->vxpp_vlc + GBA_VX_VLC_TABLE_SIZE;
        s->vxpp_run   = s->vxpp_value + GBA_VX_VALUE_TABLE_SIZE;
    } else {
        return AVERROR_INVALIDDATA;
    }

    s->packet = av_packet_alloc();
    if (!s->packet)
        return AVERROR(ENOMEM);
    return 0;
}

static int gbavx_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    GBAVXContext *s = avctx->priv_data;
    VXPic *dst_frame;
    VXPic dst = { { 0 } }, refs[3] = { { { 0 } } };
    int ret;

    if (s->next_frame >= s->nb_frames && (ret = load_segment(avctx, s)) < 0)
        return ret;

    dst_frame = &s->pic[s->cur];
    dst = *dst_frame;
    for (int i = 0; i < 3; i++) {
        VXPic *ref = &s->pic[((s->cur - 1 - i) % 4 + 4) % 4];

        refs[i] = *ref;
    }

    /* Allocate the public frame before consuming the bitstream. If allocation
     * fails, receive_frame can be retried without advancing decoder state. */
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    if (s->vxpp)
        ret = ff_vx_decode_gba_vxpp_vframe(avctx, &s->gb, avctx->width,
                                           avctx->height, s->qtab, &dst, refs,
                                           s->vxpp_vlc, s->vxpp_value,
                                           s->vxpp_run);
    else
        ret = ff_vx_decode_gba_vframe(avctx, &s->gb, avctx->width,
                                      avctx->height, s->qtab, &dst, refs);
    if (ret < 0) {
        discard_segment(s);
        return ret;
    }
    av_log(avctx, AV_LOG_TRACE, "frame %d ends at segment bit %d\n",
           s->next_frame, get_bits_count(&s->gb));
    yuv_to_rgb24(avctx, dst_frame, frame);

    if (s->segment_pts != AV_NOPTS_VALUE)
        frame->pts = s->segment_pts + s->next_frame;
    frame->duration  = 1;
    frame->pict_type = s->next_frame ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    if (!s->next_frame)
        frame->flags |= AV_FRAME_FLAG_KEY;

    s->cur = (s->cur + 1) % 4;
    s->next_frame++;
    if (s->vxpp && s->next_frame < s->nb_frames)
        skip_bits1(&s->gb); /* inter-frame marker */
    if (s->next_frame == s->nb_frames) {
        if (get_bits_count(&s->gb) != s->valid_bits &&
            consume_tail_padding(s) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "seek segment consumed %d bits, expected %d\n",
                   get_bits_count(&s->gb), s->valid_bits);
            discard_segment(s);
            return AVERROR_INVALIDDATA;
        }
        discard_segment(s);
    }
    return 0;
}

static void gbavx_flush(AVCodecContext *avctx)
{
    GBAVXContext *s = avctx->priv_data;

    discard_segment(s);
}

static av_cold int gbavx_close(AVCodecContext *avctx)
{
    GBAVXContext *s = avctx->priv_data;

    av_packet_free(&s->packet);
    av_freep(&s->arena);
    return 0;
}

const FFCodec ff_gba_vx_decoder = {
    .p.name         = "gba_vx",
    CODEC_LONG_NAME("ActImagine GBA VX Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_GBA_VX,
    .priv_data_size = sizeof(GBAVXContext),
    .init           = gbavx_init,
    .close          = gbavx_close,
    FF_CODEC_RECEIVE_FRAME_CB(gbavx_receive_frame),
    .flush          = gbavx_flush,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
