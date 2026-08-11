/* Caimans Pro GBA Video decoder -- syntax primitives. */
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"

#define IWRAM_BASE 0x03000000
#define IWRAM_SIZE (0xbfbc - 0x4f6c)
#define START_CODE 0x20
#define SIZE_TABLE 0x030070b4

typedef struct CaimansProContext {
    const uint8_t *iwram;
    int iwram_size;
    const uint8_t *rom;
    int rom_size;
    int width, height;
    uint8_t reference[0xae00], current[0xae00];
} CaimansProContext;

static const uint8_t *image_ptr(CaimansProContext *s, unsigned addr, int size)
{
    if (addr >= IWRAM_BASE && addr - IWRAM_BASE <= (unsigned)s->iwram_size - size)
        return s->iwram + addr - IWRAM_BASE;
    if (addr >= 0x08000000 && addr - 0x08000000 <= (unsigned)s->rom_size - size)
        return s->rom + addr - 0x08000000;
    return NULL;
}

static int iwram_offset(unsigned addr, unsigned size)
{
    unsigned off;
    if (addr < IWRAM_BASE)
        return -1;
    off = addr - IWRAM_BASE;
    return off <= IWRAM_SIZE - size ? off : -1;
}

static int iwram_u8(CaimansProContext *s, unsigned addr, uint8_t *v)
{
    int off = iwram_offset(addr, 1);
    if (off < 0 || off >= s->iwram_size)
        return AVERROR_INVALIDDATA;
    *v = s->iwram[off];
    return 0;
}

static int iwram_le16(CaimansProContext *s, unsigned addr, unsigned *v)
{
    int off = iwram_offset(addr, 2);
    if (off < 0 || off + 2 > s->iwram_size)
        return AVERROR_INVALIDDATA;
    *v = AV_RL16(s->iwram + off);
    return 0;
}

static int iwram_le32(CaimansProContext *s, unsigned addr, unsigned *v)
{
    int off = iwram_offset(addr, 4);
    if (off < 0 || off + 4 > s->iwram_size) return AVERROR_INVALIDDATA;
    *v = AV_RL32(s->iwram + off);
    return 0;
}

/* Direct transcription of FUN_03005a00's header branch. */
static int parse_header(CaimansProContext *s, GetBitContext *gb,
                        int *intra, int *width, int *height)
{
    unsigned start, ptype, fmt, v, n;
    int ret;

    if (get_bits_left(gb) < 32)
        return AVERROR_INVALIDDATA;
    start = show_bits_long(gb, 22);
    skip_bits_long(gb, 22);
    if ((start & ~0x70) || !(start & 0x60) || get_bits_left(gb) < 10)
        return AVERROR_INVALIDDATA;
    skip_bits(gb, 8);
    ptype = get_bits(gb, 2);
    if (ptype > 1)
        return AVERROR_INVALIDDATA;
    *intra = !ptype;
    if (!ptype) {
        if (start == 0x50 || start == 0x60) {
            if (get_bits_left(gb) < 16) return AVERROR_INVALIDDATA;
            skip_bits(gb, 16);
        }
        if ((start ^ 0x10) > 0x4f) {
            if (get_bits_left(gb) < 8) return AVERROR_INVALIDDATA;
            n = get_bits(gb, 8);
            if (get_bits_left(gb) < 8 * n) return AVERROR_INVALIDDATA;
            skip_bits_long(gb, 8 * n);
        }
        if (get_bits_left(gb) < 8) return AVERROR_INVALIDDATA;
        skip_bits(gb, 5);
        fmt = get_bits(gb, 3);
        if (fmt == 7) {
            if (get_bits_left(gb) < 24) return AVERROR_INVALIDDATA;
            *width = get_bits(gb, 12);
            *height = get_bits(gb, 12);
        } else {
            if ((ret = iwram_le16(s, SIZE_TABLE + fmt * 4, &v)) < 0) return ret;
            *width = v;
            if ((ret = iwram_le16(s, SIZE_TABLE + fmt * 4 + 2, &v)) < 0) return ret;
            *height = v;
        }
    } else if (!s->width || !s->height) {
        return AVERROR_INVALIDDATA;
    } else {
        *width = s->width;
        *height = s->height;
    }
    /* optional picture-header fields */
    if (get_bits_left(gb) < 1) return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        if (get_bits_left(gb) < 4) return AVERROR_INVALIDDATA;
        skip_bits(gb, 2);
        if (show_bits(gb, 2)) return AVERROR_PATCHWELCOME;
        skip_bits(gb, 2);
    }
    if (get_bits_left(gb) < 1) return AVERROR_INVALIDDATA;
    if (get_bits1(gb)) {
        if (get_bits_left(gb) < 6) return AVERROR_INVALIDDATA;
        skip_bits(gb, 6);
        do {
            n = get_bits_left(gb) < 8 ? 0 : get_bits(gb, 8);
        } while (n & 1);
    }
    return 0;
}

static int table_u8(CaimansProContext *s, unsigned addr)
{
    uint8_t v;
    return iwram_u8(s, addr, &v) < 0 ? -1 : v;
}

static int table_s8(CaimansProContext *s, unsigned addr)
{
    int v = table_u8(s, addr);
    return v < 0 ? v : (int8_t)v;
}

static int table_s16(CaimansProContext *s, unsigned addr)
{
    unsigned v;
    return iwram_le16(s, addr, &v) < 0 ? -1 : (int16_t)v;
}

static int read_mode(CaimansProContext *s, GetBitContext *gb, int intra, int level)
{
    unsigned base = intra ? 0x030015c0 : 0x03001a40;
    unsigned lbase = intra ? 0x030012c0 : 0x030018c0;
    int peek = intra ? 7 : 6, stride = intra ? 0x80 : 0x40;
    int idx, value, len;
    if (get_bits_left(gb) < peek) return AVERROR_INVALIDDATA;
    idx = show_bits(gb, peek);
    value = table_s8(s, base + level * stride + idx);
    len = table_u8(s, lbase + level * stride + idx);
    if (value < -1 || len < 1 || len > peek) return AVERROR_INVALIDDATA;
    skip_bits(gb, len);
    return value;
}

static int read_value(CaimansProContext *s, GetBitContext *gb, int intra, int *out)
{
    uint32_t v;
    int idx, n, value;
    if (get_bits_left(gb) < 24) return AVERROR_INVALIDDATA;
    v = show_bits_long(gb, 32);
    if (intra) {
        if (v > 0x24ffffff) {
            idx = v >> 24; value = table_u8(s, 0x030011e0 + idx - 0x25);
            n = table_u8(s, 0x03001100 + idx - 0x25);
        } else if (v < 0x03400000) {
            if (v >= 0x00040000) {
                idx = v >> 18; value = table_u8(s, 0x03000f80 + idx - 1);
                n = table_u8(s, 0x03000ea0 + idx - 1);
            } else {
                idx = v >> 12; value = table_u8(s, 0x03000e60 + idx);
                n = table_u8(s, 0x03000e20 + idx);
            }
        } else { idx = v >> 22; value = table_u8(s, 0x03001060 + idx - 0x0d); n = idx < 0x2c ? 10 : 9; }
    } else if (v < 0x0b000000) {
        if (v < 0x01200000) {
            if (v < 0x002e0000) {
                if (v < 0x00094000) {
                    if (v < 0x00049000) { idx = v >> 10; value = table_s16(s, 0x030005a0 + idx * 2); n = idx < 0x106 ? 22 : 21; }
                    else { idx = v >> 12; value = table_s16(s, 0x03000800 + idx * 2 - 0x92); n = idx < 0x5a ? 20 : 19; }
                } else { idx = v >> 14; value = table_s8(s, 0x030008a0 + idx - 0x25); n = table_u8(s, 0x03000940 + idx - 0x25); }
            } else { idx = v >> 17; value = table_s8(s, 0x03000a60 + idx - 0x17); n = table_u8(s, 0x030009e0 + idx - 0x17); }
        } else { idx = v >> 20; value = table_s8(s, 0x03000b80 + idx - 0x12); n = table_u8(s, 0x03000ae0 + idx - 0x12); }
    } else { idx = v >> 24; value = table_s8(s, 0x03000d20 + idx - 0x0b); n = table_u8(s, 0x03000c20 + idx - 0x0b); }
    if (value < -256 || n < 1 || n > 24 || get_bits_left(gb) < n) return AVERROR_INVALIDDATA;
    skip_bits(gb, n); *out = value; return 0;
}

static void fill(uint8_t *dst, int off, int stride, int w, int h, int value)
{
    for (int y = 0; y < h; y++) memset(dst + off + y * stride, value, w);
}

static int decode_leaf(CaimansProContext *s, GetBitContext *gb, uint8_t *dst,
                       int off, int stride, int level, int intra)
{
    int w = table_u8(s, 0x030000bc + level), h = table_u8(s, 0x030000b4 + level);
    int mode, value, nibbles[6], ret;
    unsigned base;
    const uint8_t *book;
    if (w < 0 || h < 0) return AVERROR_INVALIDDATA;
    if (!w) w = h = 16;
    else if (h != w / 2) h = w;
    mode = read_mode(s, gb, intra, level);
    if (mode < -1 || mode > 6) return AVERROR_INVALIDDATA;
    if (mode == -1) { if (intra) fill(dst, off, stride, w, h, 0); return 0; }
    if ((ret = read_value(s, gb, intra, &value)) < 0) return ret;
    if (!mode && intra) { fill(dst, off, stride, w, h, value); return 0; }
    if (!mode) return 0;
    if (level > 3 || iwram_le32(s, (intra ? 0x03001d20 : 0x03002640) + level * 4, &base) < 0)
        return AVERROR_INVALIDDATA;
    book = image_ptr(s, base, 96 * w * h);
    if (!book || get_bits_left(gb) < 4 * mode) return AVERROR_INVALIDDATA;
    for (int i = 0; i < mode; i++) nibbles[i] = get_bits(gb, 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int acc = value + (intra ? 0 : dst[off + y * stride + x]);
        for (int i = 0; i < mode; i++) acc += (int8_t)book[(i * 16 + nibbles[i]) * w * h + y * w + x];
        dst[off + y * stride + x] = av_clip_uint8(acc);
    }
    return 0;
}

static int decode_block(CaimansProContext *s, GetBitContext *gb, uint8_t *dst,
                        int base, int stride, int intra)
{
    int nodes[64] = { base }, head = 0, tail = 1, level_end = 1, level = 5;
    while (head < tail) {
        int split = 0;
        if (level > 0) {
            if (head != level_end) split = 1;
            else { level--; level_end = tail; split = level != 0; }
        }
        if (split && get_bits_left(gb) && get_bits1(gb)) {
            int delta = level & 1 ? stride << ((level >> 1) + 1) : 1 << ((level >> 1) + 1);
            if (tail + 2 > FF_ARRAY_ELEMS(nodes)) return AVERROR_INVALIDDATA;
            nodes[tail++] = nodes[head]; nodes[tail++] = nodes[head] + delta; head++;
        } else {
            int ret = decode_leaf(s, gb, dst, nodes[head++], stride, level, intra);
            if (ret < 0) return ret;
        }
    }
    return 0;
}

static int median3(int a, int b, int c)
{
    int q = c <= b;
    if (a < b) q = !q;
    if (q) return b;
    q = b < c;
    if (a < c) q = !q;
    return q ? c : a;
}

static int read_mv_component(CaimansProContext *s, GetBitContext *gb, int *out)
{
    uint32_t v = show_bits_long(gb, 32); int idx, mag, len, sign;
    if (v & 0x80000000) { skip_bits(gb, 1); *out = 0; return 0; }
    if (v < 0x06000000) {
        idx = (v >> 20) - 2; mag = table_s8(s, 0x03001c20 + idx); len = table_s8(s, 0x03001bc0 + idx);
    } else {
        idx = (v >> 25) - 3; mag = table_s8(s, 0x03001cc0 + idx); len = table_s8(s, 0x03001c80 + idx);
    }
    if (mag < 0 || len < 1 || len > 20 || get_bits_left(gb) < len) return AVERROR_INVALIDDATA;
    sign = !!(v & (0x80000000U >> (len - 1))); skip_bits(gb, len);
    *out = sign ? -mag : mag; return 0;
}

static int read_mv(CaimansProContext *s, GetBitContext *gb, const int a[2],
                   const int b[2], const int c[2], int out[2])
{
    for (int i = 0; i < 2; i++) {
        int d, ret = read_mv_component(s, gb, &d);
        if (ret < 0) return ret;
        out[i] = (int8_t)((median3(a[i], b[i], c[i]) + d) << 2) >> 2;
    }
    return 0;
}

static void mc(uint8_t *dst, const uint8_t *ref, int off, int stride,
               int dx, int dy, int mx, int my, int w, int h)
{
    if (dx + (mx >> 1) < 0) mx = 0;
    if (dy + (my >> 1) < 0) my = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int sx = dx + (mx >> 1) + x, sy = dy + (my >> 1) + y;
        int a = ref[off + sy * stride + sx], b = ref[off + sy * stride + sx + 1];
        int c = ref[off + (sy + 1) * stride + sx], d = ref[off + (sy + 1) * stride + sx + 1];
        if (!(mx & 1) && !(my & 1)) {
            /* FUN_030041b8's unaligned 8-byte path reads byte +5 twice. */
            if (w == 8 && ((off + sy * stride + sx) & 3) && x == 6) a = ref[off + sy * stride + sx - 1];
            dst[off + (dy + y) * stride + dx + x] = a;
        } else if (mx & 1 && my & 1) dst[off + (dy + y) * stride + dx + x] = (a + b + c + d + 2) >> 2;
        else if (mx & 1) dst[off + (dy + y) * stride + dx + x] = (a + b + 1) >> 1;
        else dst[off + (dy + y) * stride + dx + x] = (a + c + 1) >> 1;
    }
}

static int read_mb_type(CaimansProContext *s, GetBitContext *gb)
{
    int idx, len, type;
    if (get_bits_left(gb) < 3) return AVERROR_INVALIDDATA;
    idx = show_bits(gb, 3);
    type = table_s16(s, 0x03001d00 + idx * 4);
    len = table_u8(s, 0x03001d00 + idx * 4 + 2);
    if (type < 0 || type > 3 || len < 1 || len > 3) return AVERROR_INVALIDDATA;
    skip_bits(gb, len); return type;
}

static int decode_inter_plane(CaimansProContext *s, GetBitContext *gb, uint8_t *dst,
                              const uint8_t *ref, int off, int stride, int w, int h)
{
    int top[32][2] = {{0}};
    for (int y = 0; y < h; y += 16) {
        int left[2] = {0, 0};
        for (int x = 0; x < w; x += 16) {
            int c = x >> 3, type = read_mb_type(s, gb), ret;
            int zero[2] = {0,0}, a[2], b[2], d[2], e[2];
            if (type < 0) return type;
            if (type == 0 || type == 3) memset(top[c], 0, 4), memset(top[c + 1], 0, 4), memset(left, 0, 8);
            if (type == 0) { mc(dst, ref, off, stride, x, y, 0, 0, 16, 16); continue; }
            if (type == 3) { if ((ret = decode_block(s, gb, dst, off + y * stride + x, stride, 1)) < 0) return ret; continue; }
            if (type == 1) {
                ret = read_mv(s, gb, left, y ? top[c] : left, y ? top[c + 2] : left, a);
                if (ret < 0) return ret;
                memcpy(left, a, 8); memcpy(top[c], a, 8); memcpy(top[c + 1], a, 8);
                mc(dst, ref, off, stride, x, y, a[0], a[1], 16, 16);
            } else {
                ret = read_mv(s, gb, left, y ? top[c] : left, y ? top[c + 2] : left, a);
                if (ret < 0) return ret;
                ret = read_mv(s, gb, a, y ? top[c + 1] : a, y ? top[c + 2] : a, b);
                if (ret < 0) return ret;
                memcpy(left, b, 8);
                ret = read_mv(s, gb, a, left, c ? top[c - 1] : zero, d);
                if (ret < 0) return ret;
                memcpy(top[c], d, 8);
                ret = read_mv(s, gb, a, left, top[c], e);
                if (ret < 0) return ret;
                memcpy(top[c + 1], e, 8);
                mc(dst, ref, off, stride, x, y, a[0], a[1], 8, 8);
                mc(dst, ref, off, stride, x + 8, y, b[0], b[1], 8, 8);
                mc(dst, ref, off, stride, x, y + 8, d[0], d[1], 8, 8);
                mc(dst, ref, off, stride, x + 8, y + 8, e[0], e[1], 8, 8);
            }
            if ((ret = decode_block(s, gb, dst, off + y * stride + x, stride, 0)) < 0) return ret;
        }
    }
    return 0;
}

static int pro_init(AVCodecContext *avctx)
{
    CaimansProContext *s = avctx->priv_data;
    if (avctx->extradata_size <= IWRAM_SIZE) return AVERROR_INVALIDDATA;
    s->iwram = avctx->extradata; s->iwram_size = IWRAM_SIZE;
    s->rom = avctx->extradata + IWRAM_SIZE; s->rom_size = avctx->extradata_size - IWRAM_SIZE;
    avctx->pix_fmt = AV_PIX_FMT_YUV410P;
    return 0;
}

static int pro_decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    CaimansProContext *s = avctx->priv_data;
    GetBitContext gb;
    int intra, w, h, lw, lh, cw, ch, ret, used;
    if (pkt->size < 8) return AVERROR_INVALIDDATA;
    if ((ret = init_get_bits8(&gb, pkt->data, pkt->size)) < 0 ||
        (ret = parse_header(s, &gb, &intra, &w, &h)) < 0) return ret;
    if (w <= 0 || w > 240 || h <= 0 || h > 160) return AVERROR_INVALIDDATA;
    lw = FFALIGN(w, 16); lh = FFALIGN(h, 16); cw = FFALIGN((w + 3) >> 2, 16); ch = FFALIGN((h + 3) >> 2, 16);
    if (0xa200 + cw * ch > (int)sizeof(s->current)) return AVERROR_INVALIDDATA;
    if (intra) {
        memset(s->current, 0, sizeof(s->current));
        for (int off = 0, p = 0; p < 3; p++, off = p ? (p == 1 ? 0x9600 : 0xa200) : 0) {
            int pw = p ? cw : lw, ph = p ? ch : lh;
            for (int y = 0; y < ph; y += 16) for (int x = 0; x < pw; x += 16)
                if ((ret = decode_block(s, &gb, s->current, off + y * pw + x, pw, 1)) < 0) return ret;
        }
    } else {
        for (int p = 0, off = 0; p < 3; p++, off = p ? (p == 1 ? 0x9600 : 0xa200) : 0) {
            int pw = p ? cw : lw, ph = p ? ch : lh;
            if ((ret = decode_inter_plane(s, &gb, s->current, s->reference, off, pw, pw, ph)) < 0) return ret;
        }
    }
    s->width = w; s->height = h;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) return ret;
    for (int y = 0; y < h; y++) memcpy(frame->data[0] + y * frame->linesize[0], s->current + y * lw, w);
    for (int y = 0; y < (h + 3) >> 2; y++) {
        memcpy(frame->data[1] + y * frame->linesize[1], s->current + 0x9600 + y * cw, (w + 3) >> 2);
        memcpy(frame->data[2] + y * frame->linesize[2], s->current + 0xa200 + y * cw, (w + 3) >> 2);
    }
    memcpy(s->reference, s->current, sizeof(s->reference));
    frame->pict_type = intra ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    frame->flags |= intra ? AV_FRAME_FLAG_KEY : 0; *got_frame = 1;
    used = (get_bits_count(&gb) + 7) >> 3;
    while (used < pkt->size && used < ((get_bits_count(&gb) + 7) >> 3) + 4 && pkt->data[used] == 0) used++;
    return used;
}

const FFCodec ff_caimanspro_decoder = {
    .p.name = "caimanspro", CODEC_LONG_NAME("Caimans Pro GBA Video"),
    .p.type = AVMEDIA_TYPE_VIDEO, .p.id = AV_CODEC_ID_CAIMANSPRO,
    .priv_data_size = sizeof(CaimansProContext), .init = pro_init,
    FF_CODEC_DECODE_CB(pro_decode), .p.capabilities = AV_CODEC_CAP_DR1,
    CODEC_PIXFMTS(AV_PIX_FMT_YUV410P),
};
