/*
 * Swapdoodle SHEET1 decoder.  The container supplies one section per page;
 * this codec turns a section into a 256x256 RGBA drawing frame.
 */
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define SHEET_HEADER 0x40
#define CANVAS 256

static void dot(AVFrame *frame, int x, int y, uint32_t color, int radius)
{
    int xx, yy;
    for (yy = -radius; yy <= radius; yy++)
        for (xx = -radius; xx <= radius; xx++)
            if ((unsigned)(x + xx) < CANVAS && (unsigned)(y + yy) < CANVAS) {
                uint8_t *p = frame->data[0] + (y + yy) * frame->linesize[0] + 4 * (x + xx);
                p[0] = color >> 16;
                p[1] = color >> 8;
                p[2] = color;
                p[3] = 255;
            }
}

static void line(AVFrame *frame, int x0, int y0, int x1, int y1,
                 uint32_t color, int radius)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        dot(frame, x0, y0, color, radius);
        if (x0 == x1 && y0 == y1)
            break;
        if (2 * error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (2 * error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static int swapdoodle_decode(AVCodecContext *avctx, AVFrame *frame,
                              int *got_frame, AVPacket *pkt)
{
    uint32_t palette[] = { 0x333333, 0xee0011, 0xffcc00, 0x2277ff, 0xcc9933 };
    const uint8_t *sheet = pkt->data;
    const uint8_t *root = pkt->data;
    int root_size = pkt->size;
    int sheet_size = pkt->size;
    uint32_t count, i;
    int previous_x = 0, previous_y = 0, previous_color = 0;
    int have_previous = 0;
    int ret;

    if (avctx->extradata_size >= 0x40 && !memcmp(avctx->extradata, "BPK1", 4)) {
        root = avctx->extradata;
        root_size = avctx->extradata_size;
    }
    if (root_size >= 0x40 && !memcmp(root, "BPK1", 4)) {
        uint32_t entries = AV_RL32(root + 4);
        uint32_t total = AV_RL32(root + 12);

        if (entries > (root_size - 0x40) / 20 || total > root_size)
            return AVERROR_INVALIDDATA;
        if (root == pkt->data) {
            for (i = 0; i < entries; i++) {
                const uint8_t *entry = root + 0x40 + 20 * i;
                uint32_t offset = AV_RL32(entry);
                uint32_t size = AV_RL32(entry + 4);
                if (!memcmp(entry + 12, "SHEET1", 6) && offset <= total &&
                    size <= total - offset) {
                    sheet = root + offset;
                    sheet_size = size;
                    break;
                }
            }
        }
        /* COLSLT1 supplies the per-note pen colours as RGBA4 little-endian
         * values.  Its first 16 bytes are a header, followed by equal slots. */
        for (i = 0; i < entries; i++) {
            const uint8_t *entry = root + 0x40 + 20 * i;
            uint32_t offset = AV_RL32(entry);
            uint32_t size = AV_RL32(entry + 4);
            if (!memcmp(entry + 12, "COLSLT1", 7) && size >= 16 &&
                offset <= total && size <= total - offset) {
                const uint8_t *colors = root + offset;
                uint32_t slots = AV_RL32(colors);
                uint32_t stride = (size - 16) / FFMAX(slots, 1);
                unsigned j;
                if (stride < 6)
                    break;
                for (j = 0; j < FFMIN(slots, FF_ARRAY_ELEMS(palette)); j++) {
                    uint16_t rgba = AV_RL16(colors + 16 + j * stride + 4);
                    palette[j] = ((rgba >> 12) * 17 << 16) |
                                 ((rgba >> 8 & 15) * 17 << 8) |
                                 ((rgba >> 4 & 15) * 17);
                }
                break;
            }
        }
    }
    if (sheet_size < SHEET_HEADER + 4)
        return AVERROR_INVALIDDATA;
    count = AV_RL32(sheet + 4);
    if (!count || count > (sheet_size - SHEET_HEADER) / 4)
        return AVERROR_INVALIDDATA;

    avctx->pix_fmt = AV_PIX_FMT_RGBA;
    avctx->width = avctx->coded_width = CANVAS;
    avctx->height = avctx->coded_height = CANVAS;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (i = 0; i < CANVAS; i++)
        memset(frame->data[0] + i * frame->linesize[0], 255, CANVAS * 4);

    for (i = 1; i < count; i++) {
        uint32_t point = AV_RL32(sheet + SHEET_HEADER + 4 * i);
        int x = point >> 12 & 0xff;
        int y = point >> 4 & 0xff;
        int color = point >> 24 & 7;
        int style = (point >> 21 & 1) | ((point >> 27 & 1) << 2);
        int radius = style >> 2 ? 2 : style & 1;

        if ((point & 0xf) || !(point & (1 << 22))) {
            have_previous = 0;
            continue;
        }
        if (have_previous && previous_color == color)
            line(frame, previous_x, previous_y, x, y,
                 palette[color % FF_ARRAY_ELEMS(palette)], radius);
        else
            dot(frame, x, y, palette[color % FF_ARRAY_ELEMS(palette)], radius);
        previous_x = x;
        previous_y = y;
        previous_color = color;
        have_previous = 1;
    }
    *got_frame = 1;
    return pkt->size;
}

const FFCodec ff_swapdoodle_decoder = {
    .p.name         = "swapdoodle",
    CODEC_LONG_NAME("Nintendo Swapdoodle drawing"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_SWAPDOODLE,
    .p.capabilities = AV_CODEC_CAP_DR1,
    FF_CODEC_DECODE_CB(swapdoodle_decode),
};
