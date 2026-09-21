/*
 * RocketVideo decoder validation and flush regression tests.
 *
 * This file is part of FFmpeg, licensed under the GNU Lesser General Public
 * License, version 2.1 or later.
 */

#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

static int open_decoder(AVCodecContext **ctx, int width, int height,
                        int interlaced, int mode)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_RVID);

    *ctx = avcodec_alloc_context3(codec);
    if (!*ctx)
        return AVERROR(ENOMEM);
    (*ctx)->width = width;
    (*ctx)->height = height;
    (*ctx)->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!(*ctx)->extradata)
        return AVERROR(ENOMEM);
    (*ctx)->extradata_size = 4;
    (*ctx)->extradata[0] = mode;
    (*ctx)->extradata[1] = interlaced;
    return avcodec_open2(*ctx, codec, NULL);
}

static int test_dimensions(void)
{
    static const int cases[][5] = {
        { 240, 1, 0, 1, 1 }, { 256, 2, 1, 2, 1 },
        { 257, 1, 0, 1, 0 }, { 512, 1, 0, 1, 0 },
        { 256, 3, 1, 1, 0 }, { 256, 1, 0, 3, 0 },
    };
    int failed = 0;

    for (int i = 0; i < FF_ARRAY_ELEMS(cases); i++) {
        AVCodecContext *ctx = NULL;
        int ret = open_decoder(&ctx, cases[i][0], cases[i][1],
                               cases[i][2], cases[i][3]);
        if ((ret >= 0) != cases[i][4]) {
            fprintf(stderr, "unexpected init result for case %d: %d\n", i, ret);
            failed = 1;
        }
        avcodec_free_context(&ctx);
    }
    return failed;
}

static int test_flush(void)
{
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int failed = 1;

    if (!pkt || !frame || open_decoder(&ctx, 256, 2, 1, 1) < 0 ||
        av_new_packet(pkt, 512) < 0)
        goto end;
    for (int i = 0; i < 256; i++)
        AV_WL16(pkt->data + 2 * i, 0x801F); /* red */
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0)
        goto end;
    av_frame_unref(frame);
    avcodec_flush_buffers(ctx);
    for (int i = 0; i < 256; i++)
        AV_WL16(pkt->data + 2 * i, 0xFC00); /* blue */
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, frame) < 0)
        goto end;
    for (int x = 0; x < 256; x++) {
        const uint8_t *even = frame->data[0] + 3 * x;
        const uint8_t *odd = even + frame->linesize[0];
        if (even[0] || even[1] || even[2] != 255 || odd[0] || odd[1] || odd[2]) {
            fprintf(stderr, "flush retained old pixels or field parity\n");
            goto end;
        }
    }
    failed = 0;
end:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return failed;
}

int main(void)
{
    int failed = test_dimensions();
    failed |= test_flush();
    return failed;
}
