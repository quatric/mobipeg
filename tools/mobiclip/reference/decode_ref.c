/* Feed the reference encoder's raw Mobiclip frames to the mobiclip decoder.
 * usage: decode_ref <ref.bin> <w> <h> <out.yuv> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: %s ref.bin w h out.yuv\n", argv[0]); return 2; }
    int W = atoi(argv[2]), H = atoi(argv[3]);
    FILE *fi = fopen(argv[1], "rb");
    FILE *fo = fopen(argv[4], "wb");
    if (!fi || !fo) { perror("open"); return 2; }

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_MOBICLIP);
    if (!dec) { fprintf(stderr, "no mobiclip decoder\n"); return 2; }
    AVCodecContext *dc = avcodec_alloc_context3(dec);
    dc->width = W; dc->height = H;
    if (avcodec_open2(dc, dec, NULL) < 0) { fprintf(stderr, "open failed\n"); return 2; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int n = 0, ok = 0, fail = 0;
    for (;;) {
        uint32_t sz; uint8_t kf;
        if (fread(&sz, 4, 1, fi) != 1) break;
        if (fread(&kf, 1, 1, fi) != 1) break;
        /* The reference encoder prefixes each payload with a u32 bit count
         * that is not part of the Mobiclip frame (see reference/README.md).
         * Strip it — feeding it to the decoder desyncs the very first header
         * and every frame fails with "setup_qtables failed". */
        if (sz < 4) break;
        uint8_t *raw = av_malloc(sz);
        if (!raw) break;
        if (fread(raw, 1, sz, fi) != sz) { av_free(raw); break; }
        if (av_new_packet(pkt, (int)sz - 4) < 0) { av_free(raw); break; }
        memcpy(pkt->data, raw + 4, sz - 4);
        av_free(raw);
        if (kf) pkt->flags |= AV_PKT_FLAG_KEY;
        int r = avcodec_send_packet(dc, pkt);
        if (r < 0) { fail++; fprintf(stderr, "frame %d: send failed\n", n); }
        while (avcodec_receive_frame(dc, fr) >= 0) {
            for (int p = 0; p < 3; p++) {
                int w = p ? W/2 : W, h = p ? H/2 : H;
                for (int y = 0; y < h; y++)
                    fwrite(fr->data[p] + (size_t)y*fr->linesize[p], 1, w, fo);
            }
            ok++;
            av_frame_unref(fr);
        }
        av_packet_unref(pkt);
        n++;
    }
    fclose(fi); fclose(fo);
    printf("packets=%d decoded=%d failed=%d\n", n, ok, fail);
    return fail ? 1 : 0;
}
