/*
 * Em-Flash (Pixter Multimedia .mo fla2/fla3) renderer
 *
 * The payload of an fla2/fla3 movie is an ordinary Flash 3-5 tag stream, so
 * this file is a small software Flash timeline player: it parses the SWF,
 * keeps a display list per timeline, rasterizes vector shapes (solid and
 * gradient fills, strokes), static and dynamic text, buttons (up state),
 * nested sprites and clip masks, and runs ActionScript 1 (AVM1): frame and
 * clip event scripts, functions, timeline variables, movie clip properties,
 * attachMovie / removeMovieClip, Math and Key.  There is no input, so
 * Key.isDown() is always false; buttons can be pressed after a delay.
 * Bitmap fills (drawn grey), morph shapes and sound are not supported.  Device text
 * uses the VGA 8x16 font in place of the player's system font.
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

#include "emflash.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EF_MIN(a, b) ((a) < (b) ? (a) : (b))
#define EF_MAX(a, b) ((a) > (b) ? (a) : (b))

#ifdef EF_STANDALONE
#define EF_MALLOCZ(n)      calloc(1, (n))
#define EF_FREE(p)         free(p)
#define EF_REALLOC(p, n)   realloc((p), (n))
#include "../libavutil/xga_font_data.c"
#else
#include "libavutil/mem.h"
#include "libavutil/xga_font_data.h"
#define EF_MALLOCZ(n)      av_mallocz(n)
#define EF_FREE(p)         av_free(p)
#define EF_REALLOC(p, n)   av_realloc((p), (n))
#endif

static const uint8_t *ef_vga16_font(void) { return avpriv_vga16_font_get(); }

#define EF_MAX_OPS   100000
#define EF_SUB       4                  /* vertical sub-samples per pixel */
#define EF_MAXNEST   12

typedef struct EFMatrix { float a, b, c, d, tx, ty; } EFMatrix;
typedef struct EFCx { int mul[4], add[4]; } EFCx;       /* mul 256 = 1.0, order r g b a */

typedef struct EFGradStop { int ratio; uint8_t c[4]; } EFGradStop;

typedef struct EFFill {
    int type;                           /* 0 solid, 0x10 linear, 0x12 radial, 0x40.. bitmap */
    uint8_t c[4];
    EFMatrix gm;
    int ngrad;
    EFGradStop grad[16];
    int bitmap;
} EFFill;

typedef struct EFLine { int width; uint8_t c[4]; } EFLine;

typedef struct EFEdge {
    int x0, y0, x1, y1;                 /* twips */
    int cx, cy;                         /* control point if curve */
    int curve;
    int fill0, fill1, line;             /* absolute, 1 based, 0 = none */
} EFEdge;

typedef struct EFShape {
    int nfill, nline, nedge;            /* fill / line arrays are 1 based (index 0 unused) */
    EFFill *fill;
    EFLine *line;
    EFEdge *edge;
} EFShape;

typedef struct EFFont {
    int nglyph;
    EFShape *glyph;
    int16_t *adv;                       /* may be NULL */
    uint16_t *code;                     /* char code per glyph, may be NULL */
    int ascent, descent, leading;
    int has_layout;
} EFFont;

typedef struct EFTagRef { int code; const uint8_t *p; int len; } EFTagRef;

typedef struct EFFrame {
    EFTagRef *tags;
    int ntags, atags;
} EFFrame;

typedef struct EFLabel { char *name; int frame; } EFLabel;

typedef struct EFTimeline {
    int nframes;
    EFFrame *frames;
    EFLabel *labels;
    int nlabels;
} EFTimeline;

enum { EF_NONE, EF_SHAPE, EF_SPRITE, EF_FONT, EF_TEXT, EF_EDIT, EF_BUTTON };

typedef struct EFChar {
    int type;
    EFShape *shape;
    EFTimeline *tl;                     /* sprite */
    EFFont *font;
    const uint8_t *tag;                 /* text / edit / button raw data */
    int taglen;
    int tagcode;
} EFChar;

struct EFInst;

/* AVM1 values.  Strings are interned in the movie, clips are referenced by
 * instance id so a removed clip simply stops resolving. */
enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_CLIP, V_FUNC, V_MATH, V_KEY, V_OBJ };

typedef struct EFVal { int t; double n; const char *s; int id; } EFVal;

typedef struct EFVar { const char *name; EFVal v; } EFVar;

typedef struct EFItem {
    int depth;                          /* timeline depths are stored minus EF_TLDEPTH */
    int charid;
    EFMatrix m;
    EFCx cx;
    int clipdepth;
    int isclip;
    int visible;
    const char *name;                   /* instance name, interned */
    const uint8_t *clipev;              /* clip action records of PlaceObject2 */
    int clipevlen;
    struct EFInst *sub;                 /* sprite instance */
    int fresh;                          /* placed during this frame */
    int ticks;                          /* frames on stage, for the button autoclick */
} EFItem;

typedef struct EFInst {
    EFTimeline *tl;
    int id;
    int cur;                            /* frame whose tags were executed last */
    int playing;
    struct EFInst *parent;
    EFItem *item;                       /* NULL for the root */
    EFItem **dl;                        /* sorted by depth */
    int ndl, adl;
    EFVar *vars;
    int nvars, avars;
} EFInst;

typedef struct EFFunc {
    const uint8_t *code;
    int len;
    int nparams;
    const char *param[16];
    uint8_t preg[16];                   /* DefineFunction2 parameter registers */
    int v2, nregs, flags;
    const char **pool;                  /* constant pool at definition time */
    int npool;
    int scope;                          /* id of the defining timeline */
} EFFunc;

typedef struct EFQueued { int id; const uint8_t *p; int len; } EFQueued;

typedef struct EFPool { const uint8_t *key; const char **s; int n; } EFPool;

typedef struct EFExport { const char *name; int id; } EFExport;

struct EFMovie {
    int version;
    int xmin, ymin;                     /* stage origin, twips */
    int w_twips, h_twips;
    int nframes_root;
    float rate;
    uint8_t bg[3];
    EFChar *chars;
    int nchars;
    EFTimeline root_tl;
    EFInst root;
    /* raster state */
    int W, H;
    float scale;
    uint8_t *canvas;                    /* RGBA */
    float *mask;                        /* W*H or NULL */
    float *maskbuild;
    float *cov;
    int nest;
    /* action state */
    int ops;
    EFInst **insts;                     /* live instances, for id lookup */
    int ninsts, ainsts, next_id;
    EFQueued *queue;
    int nq, aq;
    EFFunc *funcs;
    int nfuncs, afuncs;
    struct EFPool *pools;               /* constant pools, by record */
    int npools, apools;
    char **strs;
    int nstrs, astrs;
    EFExport *exports;
    int nexports;
    uint32_t rnd;
    int tick;
    int autoclick;                      /* frames a button waits before it is pressed, 0 off */
    int calldepth;
};

/* ---------------------------------------------------------------- helpers */

static int rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int rd16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct BR { const uint8_t *p, *end; int pos; } BR;     /* MSB-first bit reader */

static int br_eof(const BR *b) { return b->p + (b->pos >> 3) > b->end; }

static uint32_t br_bits(BR *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) {
        int bit = 0;
        if (b->p + (b->pos >> 3) < b->end)
            bit = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
        v = v << 1 | bit;
        b->pos++;
    }
    return v;
}

static int br_sbits(BR *b, int n)
{
    uint32_t v;
    if (n <= 0)
        return 0;
    v = br_bits(b, n);
    if (n < 32 && (v & (1u << (n - 1))))
        v |= ~0u << n;
    return (int)v;
}

static void br_align(BR *b) { b->pos = (b->pos + 7) & ~7; }
static const uint8_t *br_ptr(const BR *b) { return b->p + (b->pos >> 3); }

static void br_init(BR *b, const uint8_t *p, const uint8_t *end)
{
    b->p = p;
    b->end = end;
    b->pos = 0;
}

static void rd_rect(BR *b, int r[4])
{
    int n = br_bits(b, 5);
    r[0] = br_sbits(b, n);
    r[1] = br_sbits(b, n);
    r[2] = br_sbits(b, n);
    r[3] = br_sbits(b, n);
    br_align(b);
}

static void rd_matrix(BR *b, EFMatrix *m)
{
    m->a = m->d = 1.0f;
    m->b = m->c = m->tx = m->ty = 0.0f;
    if (br_bits(b, 1)) {
        int n = br_bits(b, 5);
        m->a = br_sbits(b, n) / 65536.0f;
        m->d = br_sbits(b, n) / 65536.0f;
    }
    if (br_bits(b, 1)) {
        int n = br_bits(b, 5);
        m->b = br_sbits(b, n) / 65536.0f;
        m->c = br_sbits(b, n) / 65536.0f;
    }
    {
        int n = br_bits(b, 5);
        m->tx = br_sbits(b, n);
        m->ty = br_sbits(b, n);
    }
    br_align(b);
}

static void cx_ident(EFCx *c)
{
    int i;
    for (i = 0; i < 4; i++) {
        c->mul[i] = 256;
        c->add[i] = 0;
    }
}

static void rd_cxform(BR *b, EFCx *cx, int alpha)
{
    int has_add = br_bits(b, 1), has_mul = br_bits(b, 1), n = br_bits(b, 4);
    int i, nc = alpha ? 4 : 3;

    cx_ident(cx);
    if (has_mul)
        for (i = 0; i < nc; i++)
            cx->mul[i] = br_sbits(b, n);
    if (has_add)
        for (i = 0; i < nc; i++)
            cx->add[i] = br_sbits(b, n);
    br_align(b);
}

static void mat_ident(EFMatrix *m)
{
    m->a = m->d = 1.0f;
    m->b = m->c = m->tx = m->ty = 0.0f;
}

static EFMatrix mat_mul(const EFMatrix *p, const EFMatrix *m)       /* p * m: m applied first */
{
    EFMatrix r;
    r.a  = p->a * m->a + p->c * m->b;
    r.b  = p->b * m->a + p->d * m->b;
    r.c  = p->a * m->c + p->c * m->d;
    r.d  = p->b * m->c + p->d * m->d;
    r.tx = p->a * m->tx + p->c * m->ty + p->tx;
    r.ty = p->b * m->tx + p->d * m->ty + p->ty;
    return r;
}

static int mat_inv(const EFMatrix *m, EFMatrix *r)
{
    float det = m->a * m->d - m->b * m->c;
    if (fabsf(det) < 1e-12f)
        return -1;
    r->a  =  m->d / det;
    r->b  = -m->b / det;
    r->c  = -m->c / det;
    r->d  =  m->a / det;
    r->tx = -(r->a * m->tx + r->c * m->ty);
    r->ty = -(r->b * m->tx + r->d * m->ty);
    return 0;
}

static EFCx cx_mul(const EFCx *p, const EFCx *c)    /* p applied after c */
{
    EFCx r;
    int i;
    for (i = 0; i < 4; i++) {
        r.mul[i] = p->mul[i] * c->mul[i] / 256;
        r.add[i] = p->mul[i] * c->add[i] / 256 + p->add[i];
    }
    return r;
}

static void cx_apply(const EFCx *cx, const uint8_t in[4], int out[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        int v = in[i] * cx->mul[i] / 256 + cx->add[i];
        out[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

/* ----------------------------------------------------------------- shapes */

static void add_edge(EFShape *s, int *cap, const EFEdge *e)
{
    if (s->nedge == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        s->edge = EF_REALLOC(s->edge, *cap * sizeof(*s->edge));
    }
    s->edge[s->nedge++] = *e;
}

static void rd_color(BR *b, uint8_t c[4], int rgba)
{
    c[0] = br_bits(b, 8);
    c[1] = br_bits(b, 8);
    c[2] = br_bits(b, 8);
    c[3] = rgba ? br_bits(b, 8) : 255;
}

/* fill and line style arrays; appended after the existing nfill / nline */
static void rd_styles(BR *b, EFShape *s, int tagtype)
{
    int n, i, rgba = tagtype >= 3;
    int basef = s->nfill, basel = s->nline;

    br_align(b);
    n = br_bits(b, 8);
    if (n == 0xFF && tagtype >= 2)
        n = br_bits(b, 16);
    s->fill = EF_REALLOC(s->fill, (basef + n + 1) * sizeof(*s->fill));
    if (!basef)
        memset(&s->fill[0], 0, sizeof(s->fill[0]));
    for (i = 1; i <= n; i++) {
        EFFill *f = &s->fill[basef + i];
        memset(f, 0, sizeof(*f));
        f->type = br_bits(b, 8);
        if (f->type == 0) {
            rd_color(b, f->c, rgba);
        } else if (f->type == 0x10 || f->type == 0x12 || f->type == 0x13) {
            int g;
            rd_matrix(b, &f->gm);
            f->ngrad = br_bits(b, 8) & 15;
            if (f->type == 0x13)
                br_bits(b, 16);
            for (g = 0; g < f->ngrad; g++) {
                f->grad[g].ratio = br_bits(b, 8);
                rd_color(b, f->grad[g].c, rgba);
            }
        } else if (f->type >= 0x40 && f->type <= 0x43) {
            f->bitmap = br_bits(b, 16);
            rd_matrix(b, &f->gm);
        }
    }
    s->nfill = basef + n;
    br_align(b);
    n = br_bits(b, 8);
    if (n == 0xFF && tagtype >= 2)
        n = br_bits(b, 16);
    s->line = EF_REALLOC(s->line, (basel + n + 1) * sizeof(*s->line));
    if (!basel)
        memset(&s->line[0], 0, sizeof(s->line[0]));
    for (i = 1; i <= n; i++) {
        EFLine *l = &s->line[basel + i];
        l->width = br_bits(b, 16);
        rd_color(b, l->c, rgba);
    }
    s->nline = basel + n;
}

/* shape records.  glyph = no style tables, a single implicit fill */
static int rd_shape_records(BR *b, EFShape *s, int tagtype, int glyph)
{
    int nfb, nlb, x = 0, y = 0, cap = 0;
    int fill0 = 0, fill1 = 0, line = 0;
    int fbase = 0, lbase = 0;

    nfb = br_bits(b, 4);
    nlb = br_bits(b, 4);
    for (;;) {
        if (br_eof(b))
            return -1;
        if (br_bits(b, 1)) {                        /* edge record */
            EFEdge e;
            memset(&e, 0, sizeof(e));
            e.fill0 = fill0;
            e.fill1 = fill1;
            e.line  = line;
            e.x0 = x;
            e.y0 = y;
            if (br_bits(b, 1)) {                    /* straight */
                int nb = br_bits(b, 4) + 2, dx = 0, dy = 0;
                if (br_bits(b, 1)) {
                    dx = br_sbits(b, nb);
                    dy = br_sbits(b, nb);
                } else if (br_bits(b, 1)) {
                    dy = br_sbits(b, nb);
                } else {
                    dx = br_sbits(b, nb);
                }
                x += dx;
                y += dy;
            } else {
                int nb = br_bits(b, 4) + 2;
                int cdx = br_sbits(b, nb), cdy = br_sbits(b, nb);
                int adx = br_sbits(b, nb), ady = br_sbits(b, nb);
                e.curve = 1;
                e.cx = x + cdx;
                e.cy = y + cdy;
                x += cdx + adx;
                y += cdy + ady;
            }
            e.x1 = x;
            e.y1 = y;
            add_edge(s, &cap, &e);
        } else {
            int flags = br_bits(b, 5);
            if (!flags)
                break;
            if (flags & 1) {
                int nb = br_bits(b, 5);
                x = br_sbits(b, nb);
                y = br_sbits(b, nb);
            }
            if (flags & 2) {
                int v = br_bits(b, nfb);
                fill0 = v ? fbase + v : 0;
            }
            if (flags & 4) {
                int v = br_bits(b, nfb);
                fill1 = v ? fbase + v : 0;
            }
            if (flags & 8) {
                int v = br_bits(b, nlb);
                line = v ? lbase + v : 0;
            }
            if ((flags & 16) && !glyph) {
                fbase = s->nfill;
                lbase = s->nline;
                rd_styles(b, s, tagtype);
                nfb = br_bits(b, 4);
                nlb = br_bits(b, 4);
            }
        }
    }
    return 0;
}

static EFShape *parse_shape(const uint8_t *p, int len, int tagtype, int *idp)
{
    EFShape *s = EF_MALLOCZ(sizeof(*s));
    BR b;
    int r[4];

    if (!s)
        return NULL;
    *idp = rd16(p);
    br_init(&b, p + 2, p + len);
    rd_rect(&b, r);
    rd_styles(&b, s, tagtype);
    if (rd_shape_records(&b, s, tagtype, 0) < 0)
        ;                                           /* keep what we have */
    return s;
}

static void free_shape(EFShape *s)
{
    if (!s)
        return;
    EF_FREE(s->fill);
    EF_FREE(s->line);
    EF_FREE(s->edge);
}

/* ------------------------------------------------------------------ fonts */

static EFFont *font_new(void) { return EF_MALLOCZ(sizeof(EFFont)); }

static void parse_font1(EFChar *c, const uint8_t *p, int len)
{
    EFFont *f = font_new();
    int n, i;
    BR b;

    c->font = f;
    c->type = EF_FONT;
    if (!f || len < 4)
        return;
    n = rd16(p + 2) / 2;
    if (n <= 0 || n > 4096)
        return;
    f->nglyph = n;
    f->glyph = EF_MALLOCZ(n * sizeof(EFShape));
    for (i = 0; i < n; i++) {
        int off = 2 + rd16(p + 2 + 2 * i);
        if (off >= len)
            continue;
        br_init(&b, p + off, p + len);
        f->glyph[i].fill = EF_MALLOCZ(2 * sizeof(EFFill));
        f->glyph[i].nfill = 1;
        rd_shape_records(&b, &f->glyph[i], 1, 1);
    }
}

static void parse_fontinfo(EFChar *c, const uint8_t *p, int len)
{
    EFFont *f = c->font;
    int nl, flags, wide, i, n;

    if (!f || len < 4)
        return;
    nl = p[2];
    if (3 + nl + 1 > len)
        return;
    flags = p[3 + nl];
    wide = flags & 1;
    n = f->nglyph;
    f->code = EF_MALLOCZ(n * sizeof(uint16_t));
    if (!f->code)
        return;
    for (i = 0; i < n; i++) {
        int off = 4 + nl + i * (wide ? 2 : 1);
        if (off + (wide ? 2 : 1) > len)
            break;
        f->code[i] = wide ? rd16(p + off) : p[off];
    }
}

static void parse_font2(EFChar *c, const uint8_t *p, int len)
{
    EFFont *f = font_new();
    int flags, nl, n, i, wideoff, widecodes, pos;
    const uint8_t *offs;
    BR b;

    c->font = f;
    c->type = EF_FONT;
    if (!f || len < 6)
        return;
    flags = p[2];
    nl = p[4];
    pos = 5 + nl;
    n = rd16(p + pos);
    pos += 2;
    wideoff = flags & 8;
    widecodes = flags & 4;
    if (n <= 0 || n > 8192)
        return;
    f->nglyph = n;
    f->glyph = EF_MALLOCZ(n * sizeof(EFShape));
    offs = p + pos;
    if (pos + (wideoff ? 4 : 2) * (n + 1) > len) {
        f->nglyph = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t off = wideoff ? rd32(offs + 4 * i) : rd16(offs + 2 * i);
        if (pos + off >= (uint32_t)len)
            continue;
        br_init(&b, p + pos + off, p + len);
        f->glyph[i].fill = EF_MALLOCZ(2 * sizeof(EFFill));
        f->glyph[i].nfill = 1;
        rd_shape_records(&b, &f->glyph[i], 1, 1);
    }
    pos += (wideoff ? 4 : 2) * n;
    {
        /* the code table offset is relative to the start of the offset table */
        uint32_t codeoff = pos + 4 <= len ? (wideoff ? rd32(p + pos) : rd16(p + pos)) : 0;
        int cp = (offs - p) + codeoff;
        f->code = EF_MALLOCZ(n * sizeof(uint16_t));
        if (f->code && cp + n * (widecodes ? 2 : 1) <= len)
            for (i = 0; i < n; i++)
                f->code[i] = widecodes ? rd16(p + cp + 2 * i) : p[cp + i];
        pos = cp + n * (widecodes ? 2 : 1);
    }
    if ((flags & 0x80) && pos + 6 <= len) {
        f->has_layout = 1;
        f->ascent  = rd16s(p + pos);
        f->descent = rd16s(p + pos + 2);
        f->leading = rd16s(p + pos + 4);
        pos += 6;
        f->adv = EF_MALLOCZ(n * sizeof(int16_t));
        if (f->adv && pos + 2 * n <= len)
            for (i = 0; i < n; i++)
                f->adv[i] = rd16s(p + pos + 2 * i);
    }
}

static int font_glyph_for_code(const EFFont *f, int code)
{
    int i;
    if (f->code)
        for (i = 0; i < f->nglyph; i++)
            if (f->code[i] == code)
                return i;
    if (!f->code && code < f->nglyph)
        return code;
    return -1;
}

/* --------------------------------------------------------------- timeline */

static void tl_add_tag(EFFrame *fr, int code, const uint8_t *p, int len)
{
    if (fr->ntags == fr->atags) {
        fr->atags = fr->atags ? fr->atags * 2 : 8;
        fr->tags = EF_REALLOC(fr->tags, fr->atags * sizeof(*fr->tags));
    }
    fr->tags[fr->ntags].code = code;
    fr->tags[fr->ntags].p = p;
    fr->tags[fr->ntags].len = len;
    fr->ntags++;
}

static int next_tag(const uint8_t **pp, const uint8_t *end, int *code, const uint8_t **data, int *len)
{
    const uint8_t *p = *pp;
    int h, l;

    if (p + 2 > end)
        return 0;
    h = rd16(p);
    p += 2;
    *code = h >> 6;
    l = h & 63;
    if (l == 63) {
        if (p + 4 > end)
            return 0;
        l = rd32(p);
        p += 4;
    }
    if (l < 0 || p + l > end)
        l = end - p;
    *data = p;
    *len = l;
    *pp = p + l;
    return 1;
}

/* collect the control tags of a timeline into frames */
static void tl_build(EFTimeline *tl, const uint8_t *p, const uint8_t *end, int declared)
{
    int code, len, nf = 0;
    const uint8_t *data;
    EFFrame cur;

    memset(&cur, 0, sizeof(cur));
    tl->frames = EF_MALLOCZ((declared + 1) * sizeof(EFFrame) + 4096 * 0);
    while (next_tag(&p, end, &code, &data, &len)) {
        if (code == 0)
            break;
        if (code == 1) {
            if (nf < declared + 1) {
                tl->frames[nf++] = cur;
                memset(&cur, 0, sizeof(cur));
            }
            continue;
        }
        if (code == 43) {
            tl->labels = EF_REALLOC(tl->labels, (tl->nlabels + 1) * sizeof(EFLabel));
            tl->labels[tl->nlabels].name = (char *)data;      /* NUL terminated in stream */
            tl->labels[tl->nlabels].frame = nf;
            tl->nlabels++;
            continue;
        }
        if (code == 4 || code == 5 || code == 9 || code == 12 || code == 26 || code == 28)
            tl_add_tag(&cur, code, data, len);
    }
    if (cur.ntags && nf < declared + 1)
        tl->frames[nf++] = cur;                 /* trailing tags without ShowFrame */
    tl->nframes = nf;
}

static void tl_free(EFTimeline *tl)
{
    int i;
    for (i = 0; i < tl->nframes; i++)
        EF_FREE(tl->frames[i].tags);
    EF_FREE(tl->frames);
    EF_FREE(tl->labels);
}

/* -------------------------------------------------------------- instances */

#define EF_TLDEPTH  16384               /* Flash keeps timeline depths below script depths */
#define EF_NREGS    256
#define EF_STACK    256
#define EF_MAXCALL  48

static int ef_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (uint8_t)*a, y = (uint8_t)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x)
            return x - y;
    }
}

/* interned strings live until ff_emflash_close() */
static const char *intern_n(EFMovie *m, const char *s, int n)
{
    int i;
    char *r;

    for (i = m->nstrs - 1; i >= 0 && i >= m->nstrs - 4096; i--)
        if (!strncmp(m->strs[i], s, n) && !m->strs[i][n])
            return m->strs[i];
    if (m->nstrs >= 65536)
        return "";
    if (m->nstrs == m->astrs) {
        char **t = EF_REALLOC(m->strs, (m->astrs * 2 + 64) * sizeof(*t));
        if (!t)
            return "";
        m->strs = t;
        m->astrs = m->astrs * 2 + 64;
    }
    r = EF_MALLOCZ(n + 1);
    if (!r)
        return "";
    memcpy(r, s, n);
    m->strs[m->nstrs++] = r;
    return r;
}

static const char *intern(EFMovie *m, const char *s) { return intern_n(m, s, strlen(s)); }

static EFChar *get_char(EFMovie *m, int id)
{
    if (id < 0 || id >= m->nchars || m->chars[id].type == EF_NONE)
        return NULL;
    return &m->chars[id];
}

static EFInst *inst_get(EFMovie *m, int id)
{
    int i;
    if (id == m->root.id)
        return &m->root;
    for (i = 0; i < m->ninsts; i++)
        if (m->insts[i]->id == id)
            return m->insts[i];
    return NULL;
}

static void inst_register(EFMovie *m, EFInst *in)
{
    in->id = ++m->next_id;
    if (in == &m->root)
        return;
    if (m->ninsts == m->ainsts) {
        EFInst **t = EF_REALLOC(m->insts, (m->ainsts * 2 + 16) * sizeof(*t));
        if (!t)
            return;
        m->insts = t;
        m->ainsts = m->ainsts * 2 + 16;
    }
    m->insts[m->ninsts++] = in;
}

static void inst_unregister(EFMovie *m, EFInst *in)
{
    int i;
    for (i = 0; i < m->ninsts; i++)
        if (m->insts[i] == in) {
            m->insts[i] = m->insts[--m->ninsts];
            return;
        }
}

static void inst_free(EFMovie *m, EFInst *in);

static void item_free(EFMovie *m, EFItem *it)
{
    if (it->sub) {
        inst_free(m, it->sub);
        EF_FREE(it->sub);
        it->sub = NULL;
    }
    EF_FREE(it);
}

/* frees what the instance owns, not the instance itself */
static void inst_free(EFMovie *m, EFInst *in)
{
    int i;
    for (i = 0; i < in->ndl; i++)
        item_free(m, in->dl[i]);
    EF_FREE(in->dl);
    EF_FREE(in->vars);
    in->dl = NULL;
    in->vars = NULL;
    in->ndl = in->adl = in->nvars = in->avars = 0;
    inst_unregister(m, in);
}

static EFItem *dl_find(EFInst *in, int depth)
{
    int i;
    for (i = 0; i < in->ndl; i++)
        if (in->dl[i]->depth == depth)
            return in->dl[i];
    return NULL;
}

static EFItem *dl_find_name(EFInst *in, const char *name)
{
    int i;
    for (i = 0; i < in->ndl; i++)
        if (in->dl[i]->name && !ef_strcasecmp(in->dl[i]->name, name))
            return in->dl[i];
    return NULL;
}

static void dl_remove(EFMovie *m, EFInst *in, int depth)
{
    int i;
    for (i = 0; i < in->ndl; i++)
        if (in->dl[i]->depth == depth) {
            item_free(m, in->dl[i]);
            memmove(&in->dl[i], &in->dl[i + 1], (in->ndl - i - 1) * sizeof(*in->dl));
            in->ndl--;
            return;
        }
}

static EFItem *dl_insert(EFMovie *m, EFInst *in, int depth)
{
    int i, pos;
    EFItem *it;

    dl_remove(m, in, depth);
    if (in->ndl == in->adl) {
        EFItem **t = EF_REALLOC(in->dl, (in->adl * 2 + 16) * sizeof(*t));
        if (!t)
            return NULL;
        in->dl = t;
        in->adl = in->adl * 2 + 16;
    }
    it = EF_MALLOCZ(sizeof(*it));
    if (!it)
        return NULL;
    it->depth = depth;
    it->visible = 1;
    it->fresh = 1;
    mat_ident(&it->m);
    cx_ident(&it->cx);
    for (pos = 0; pos < in->ndl && in->dl[pos]->depth < depth; pos++)
        ;
    for (i = in->ndl; i > pos; i--)
        in->dl[i] = in->dl[i - 1];
    in->dl[pos] = it;
    in->ndl++;
    return it;
}

/* ------------------------------------------------------------ action queue */

static void queue_code(EFMovie *m, EFInst *in, const uint8_t *p, int len)
{
    if (m->nq == m->aq) {
        EFQueued *t;
        if (m->aq >= 65536)
            return;
        t = EF_REALLOC(m->queue, (m->aq * 2 + 32) * sizeof(*t));
        if (!t)
            return;
        m->queue = t;
        m->aq = m->aq * 2 + 32;
    }
    m->queue[m->nq].id = in->id;
    m->queue[m->nq].p = p;
    m->queue[m->nq].len = len;
    m->nq++;
}

/* clip events: 1 load, 2 enterFrame (SWF 5 bit layout, also used by 6+) */
static void queue_event(EFMovie *m, EFInst *in, int mask)
{
    const uint8_t *p, *end;
    int wide = m->version >= 6;

    if (!in->item || !in->item->clipev)
        return;
    p = in->item->clipev;
    end = p + in->item->clipevlen;
    while (p + (wide ? 4 : 2) <= end) {
        uint32_t flags = wide ? rd32(p) : (uint32_t)rd16(p);
        int size;
        p += wide ? 4 : 2;
        if (!flags || p + 4 > end)
            break;
        size = rd32(p);
        p += 4;
        if (size < 0 || size > end - p)
            break;
        if (flags & mask)
            queue_code(m, in, p + ((flags & 0x20000) ? 1 : 0), size - ((flags & 0x20000) ? 1 : 0));
        p += size;
    }
}

static void run_queue(EFMovie *m);

/* ---------------------------------------------------------------- timeline */

static void inst_goto(EFMovie *m, EFInst *in, int frame);

static EFInst *inst_new_sprite(EFMovie *m, EFInst *parent, EFItem *it, EFTimeline *tl)
{
    EFInst *in = EF_MALLOCZ(sizeof(*in));
    if (!in)
        return NULL;
    in->tl = tl;
    in->cur = -1;
    in->playing = 1;
    in->parent = parent;
    in->item = it;
    it->sub = in;
    inst_register(m, in);
    if (m->nest < EF_MAXNEST && tl && tl->nframes) {
        m->nest++;
        inst_goto(m, in, 0);
        m->nest--;
    }
    queue_event(m, in, 1);
    return in;
}

static void edittext_bind(EFMovie *m, EFInst *in, const EFChar *c);

static void place(EFMovie *m, EFInst *in, int code, const uint8_t *p, int len)
{
    int depth, charid = -1, move = 0, flags;
    EFItem *it;
    EFMatrix mat;
    EFCx cx;
    int hasm = 0, hascx = 0, hasclip = 0, clip = 0;
    const char *name = NULL;
    const uint8_t *clipev = NULL;
    int clipevlen = 0;
    BR b;

    if (code == 4) {
        if (len < 4)
            return;
        charid = rd16(p);
        depth = rd16(p + 2);
        br_init(&b, p + 4, p + len);
        rd_matrix(&b, &mat);
        hasm = 1;
        if (br_ptr(&b) < p + len) {
            rd_cxform(&b, &cx, 0);
            hascx = 1;
        }
    } else {
        int pos = 3;
        if (len < 3)
            return;
        flags = p[0];
        depth = rd16(p + 1);
        move = flags & 1;
        if (flags & 2) {
            if (pos + 2 > len)
                return;
            charid = rd16(p + pos);
            pos += 2;
        }
        if (flags & 4) {
            br_init(&b, p + pos, p + len);
            rd_matrix(&b, &mat);
            pos = br_ptr(&b) - p;
            hasm = 1;
        }
        if (flags & 8) {
            br_init(&b, p + pos, p + len);
            rd_cxform(&b, &cx, 1);
            pos = br_ptr(&b) - p;
            hascx = 1;
        }
        if (flags & 0x10)
            pos += 2;
        if (flags & 0x20) {
            int s = pos;
            while (pos < len && p[pos])
                pos++;
            if (pos < len)
                name = intern_n(m, (const char *)p + s, pos - s);
            pos++;
        }
        if (flags & 0x40) {
            if (pos + 2 <= len) {
                clip = rd16(p + pos) - EF_TLDEPTH;
                hasclip = 1;
            }
            pos += 2;
        }
        if ((flags & 0x80) && pos < len) {
            int skip = 2 + (m->version >= 6 ? 4 : 2);           /* reserved, all event flags */
            if (pos + skip < len) {
                clipev = p + pos + skip;
                clipevlen = len - pos - skip;
            }
        }
    }
    depth -= EF_TLDEPTH;

    it = dl_find(in, depth);
    if (move && charid < 0) {
        if (!it)
            return;
    } else if (charid >= 0) {
        EFMatrix om;
        EFCx ocx;
        int oclip = 0, oisclip = 0;
        EFChar *c;

        mat_ident(&om);
        cx_ident(&ocx);
        if (it && move) {
            om = it->m;
            ocx = it->cx;
            oclip = it->clipdepth;
            oisclip = it->isclip;
            if (!name)
                name = it->name;
        }
        it = dl_insert(m, in, depth);
        if (!it)
            return;
        it->m = om;
        it->cx = ocx;
        it->clipdepth = oclip;
        it->isclip = oisclip;
        it->charid = charid;
        it->name = name;
        it->clipev = clipev;
        it->clipevlen = clipevlen;
        if (hasm)
            it->m = mat;
        if (hascx)
            it->cx = cx;
        if (hasclip) {
            it->clipdepth = clip;
            it->isclip = 1;
        }
        c = get_char(m, charid);
        if (c && c->type == EF_SPRITE)
            inst_new_sprite(m, in, it, c->tl);
        else if (c && c->type == EF_EDIT)
            edittext_bind(m, in, c);
        return;
    }
    if (!it)
        return;
    if (hasm)
        it->m = mat;
    if (hascx)
        it->cx = cx;
    if (hasclip) {
        it->clipdepth = clip;
        it->isclip = 1;
    }
    if (name)
        it->name = name;
}

/* actions: queue DoAction tags too; a goto skips the actions of the frames it passes */
static void exec_frame(EFMovie *m, EFInst *in, int f, int actions)
{
    EFFrame *fr = &in->tl->frames[f];
    int i;

    for (i = 0; i < fr->ntags; i++) {
        EFTagRef *t = &fr->tags[i];
        switch (t->code) {
        case 4:
        case 26:
            place(m, in, t->code, t->p, t->len);
            break;
        case 5:
            if (t->len >= 4)
                dl_remove(m, in, rd16(t->p + 2) - EF_TLDEPTH);
            break;
        case 28:
            if (t->len >= 2)
                dl_remove(m, in, rd16(t->p) - EF_TLDEPTH);
            break;
        case 9:
            if (in == &m->root && t->len >= 3) {
                m->bg[0] = t->p[0];
                m->bg[1] = t->p[1];
                m->bg[2] = t->p[2];
            }
            break;
        case 12:
            if (actions)
                queue_code(m, in, t->p, t->len);
            break;
        }
    }
}

/* run the frames up to the target so the display list is rebuilt; a jump
 * backwards drops the timeline objects but keeps script-created clips */
static void inst_goto(EFMovie *m, EFInst *in, int frame)
{
    int f, i;

    if (!in->tl || !in->tl->nframes)
        return;
    frame = EF_MAX(0, EF_MIN(frame, in->tl->nframes - 1));
    if (frame == in->cur)
        return;
    if (frame < in->cur) {
        for (i = in->ndl - 1; i >= 0; i--)
            if (in->dl[i]->depth < 0)
                dl_remove(m, in, in->dl[i]->depth);
        in->cur = -1;
    }
    for (f = in->cur + 1; f <= frame; f++) {
        in->cur = f;
        exec_frame(m, in, f, f == frame);
    }
}

static void inst_tick(EFMovie *m, EFInst *in)
{
    int i, n, ids[256];

    if (m->nest >= EF_MAXNEST)
        return;
    queue_event(m, in, 2);
    if (in->playing && in->tl && in->tl->nframes > 1)
        inst_goto(m, in, in->cur + 1 >= in->tl->nframes ? 0 : in->cur + 1);
    /* children may be removed or added while ticking, so go by id */
    for (i = n = 0; i < in->ndl && n < 256; i++) {
        in->dl[i]->ticks++;
        if (in->dl[i]->sub && !in->dl[i]->fresh)
            ids[n++] = in->dl[i]->sub->id;
    }
    m->nest++;
    for (i = 0; i < n; i++) {
        EFInst *c = inst_get(m, ids[i]);
        if (c)
            inst_tick(m, c);
    }
    m->nest--;
}

static void inst_clear_fresh(EFInst *in, int lvl)
{
    int i;
    for (i = 0; i < in->ndl; i++) {
        in->dl[i]->fresh = 0;
        if (in->dl[i]->sub && lvl < EF_MAXNEST)
            inst_clear_fresh(in->dl[i]->sub, lvl + 1);
    }
}

static int tl_label(const EFTimeline *tl, const char *name)
{
    int i;
    if (!tl)
        return -1;
    for (i = 0; i < tl->nlabels; i++)
        if (!ef_strcasecmp(tl->labels[i].name, name))
            return tl->labels[i].frame;
    return -1;
}

/* ---------------------------------------------------------------- values */

static EFVal vundef(void) { EFVal v = { .t = V_UNDEF }; return v; }
static EFVal vnum(double n) { EFVal v = { .t = V_NUM }; v.n = n; return v; }
static EFVal vbool(int b) { EFVal v = { .t = V_BOOL }; v.n = !!b; return v; }
static EFVal vstr(const char *s) { EFVal v = { .t = V_STR }; v.s = s ? s : ""; return v; }
static EFVal vclip(const EFInst *in) { EFVal v = { .t = V_CLIP }; if (!in) return vundef(); v.id = in->id; return v; }
static EFVal vkind(int t) { EFVal v = { .t = V_UNDEF }; v.t = t; return v; }

static double to_num(const EFVal *v)
{
    char *e;
    double d;
    switch (v->t) {
    case V_NUM:
    case V_BOOL:
        return v->n;
    case V_STR: {
        const char *s = v->s;
        while (*s == ' ')
            s++;
        d = strtod(s, &e);
        return e == s ? (*s ? NAN : 0) : d;
    }
    default:
        return 0;
    }
}

static int to_bool(const EFVal *v)
{
    switch (v->t) {
    case V_NUM:
    case V_BOOL:
        return v->n != 0 && !isnan(v->n);
    case V_STR: {
        double d = to_num(v);
        return d != 0 && !isnan(d);
    }
    case V_CLIP: case V_FUNC: case V_MATH: case V_KEY: case V_OBJ:
        return 1;
    default:
        return 0;
    }
}

static void clip_path(EFMovie *m, const EFInst *in, char *buf, int size)
{
    const char *names[EF_MAXNEST + 4];
    int n = 0, i;

    while (in && in != &m->root && n < EF_MAXNEST + 4) {
        names[n++] = in->item && in->item->name ? in->item->name : "instance";
        in = in->parent;
    }
    snprintf(buf, size, "_level0");
    for (i = n - 1; i >= 0; i--) {
        int l = strlen(buf);
        snprintf(buf + l, size - l, ".%s", names[i]);
    }
}

static const char *to_str(EFMovie *m, const EFVal *v)
{
    char buf[256];
    switch (v->t) {
    case V_NULL:
        return "null";
    case V_BOOL:
        return v->n ? "true" : "false";
    case V_NUM:
        if (isnan(v->n))
            return "NaN";
        if (isinf(v->n))
            return v->n > 0 ? "Infinity" : "-Infinity";
        if (v->n == floor(v->n) && fabs(v->n) < 1e15)
            snprintf(buf, sizeof(buf), "%.0f", v->n);
        else
            snprintf(buf, sizeof(buf), "%.15g", v->n);
        return intern(m, buf);
    case V_STR:
        return v->s;
    case V_CLIP:
        clip_path(m, inst_get(m, v->id), buf, sizeof(buf));
        return intern(m, buf);
    case V_FUNC:
        return "[type Function]";
    case V_MATH: case V_KEY: case V_OBJ:
        return "[object Object]";
    default:
        return "";
    }
}

static int vars_find(EFVar *vars, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!ef_strcasecmp(vars[i].name, name))
            return i;
    return -1;
}

static void vars_set(EFMovie *m, EFVar **vars, int *n, int *a, const char *name, EFVal v)
{
    int i = vars_find(*vars, *n, name);
    if (i < 0) {
        if (*n == *a) {
            EFVar *t;
            if (*a >= 4096)
                return;
            t = EF_REALLOC(*vars, (*a * 2 + 8) * sizeof(*t));
            if (!t)
                return;
            *vars = t;
            *a = *a * 2 + 8;
        }
        i = (*n)++;
        (*vars)[i].name = intern(m, name);
    }
    (*vars)[i].v = v;
}

/* ------------------------------------------------------------------ AVM1 */

typedef struct EFCtx {
    EFMovie *m;
    int target;                         /* current target clip (SetTarget) */
    int scope;                          /* timeline the code runs on */
    int self;                           /* "this" */
    EFVar *locals;
    int nlocals, alocals;
    int infunc;
    EFVal regs[EF_NREGS];
    const char **pool;
    int npool;
} EFCtx;

static EFVal run_code(EFCtx *c, const uint8_t *p, int len);

static void ctx_init(EFCtx *c, EFMovie *m, EFInst *in)
{
    memset(c, 0, sizeof(*c));
    c->m = m;
    c->target = c->scope = c->self = in->id;
}

static EFInst *ctx_target(EFCtx *c)
{
    EFInst *in = inst_get(c->m, c->target);
    return in ? in : inst_get(c->m, c->scope);
}

static EFInst *child_clip(EFInst *in, const char *name)
{
    EFItem *it = in ? dl_find_name(in, name) : NULL;
    return it ? it->sub : NULL;
}

/* "/a/b", "../x", "_root.a.b", "_parent", "this.a" or a plain child name */
static EFInst *resolve_path(EFCtx *c, const char *path)
{
    EFMovie *m = c->m;
    EFInst *in = ctx_target(c);
    char seg[256];
    const char *s = path;

    if (!*s)
        return in;
    if (*s == '/') {
        in = &m->root;
        s++;
    }
    while (*s && in) {
        int n = 0;
        if (s[0] == '.' && s[1] == '.') {               /* "../" */
            in = in->parent;
            s += 2;
            if (*s == '/' || *s == ':')
                s++;
            continue;
        }
        while (s[n] && s[n] != '/' && s[n] != '.' && s[n] != ':')
            n++;
        if (n >= (int)sizeof(seg))
            return NULL;
        memcpy(seg, s, n);
        seg[n] = 0;
        s += n;
        if (!n) {
        } else if (!ef_strcasecmp(seg, "_root") || !ef_strcasecmp(seg, "_level0")) {
            in = &m->root;
        } else if (!ef_strcasecmp(seg, "_parent")) {
            in = in->parent;
        } else if (!ef_strcasecmp(seg, "this")) {
            in = inst_get(m, c->self);
        } else {
            EFInst *ch = child_clip(in, seg);
            if (!ch) {
                int i = vars_find(in->vars, in->nvars, seg);
                ch = i >= 0 && in->vars[i].v.t == V_CLIP ? inst_get(m, in->vars[i].v.id) : NULL;
            }
            in = ch;
        }
        if (*s == '/' || *s == '.' || *s == ':')
            s++;
    }
    return in;
}

enum { P_X, P_Y, P_XSCALE, P_YSCALE, P_CURFRAME, P_TOTALFRAMES, P_ALPHA, P_VISIBLE, P_WIDTH,
       P_HEIGHT, P_ROTATION, P_TARGET, P_FRAMESLOADED, P_NAME, P_DROPTARGET, P_URL,
       P_HIGHQUALITY, P_FOCUSRECT, P_SOUNDBUFTIME, P_QUALITY, P_XMOUSE, P_YMOUSE, P_NB };

static const char *const prop_names[P_NB] = {
    "_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha", "_visible",
    "_width", "_height", "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url",
    "_highquality", "_focusrect", "_soundbuftime", "_quality", "_xmouse", "_ymouse",
};

static int prop_index(const char *name)
{
    int i;
    for (i = 0; i < P_NB; i++)
        if (!ef_strcasecmp(prop_names[i], name))
            return i;
    return -1;
}

static EFVal get_prop(EFMovie *m, EFInst *in, int idx)
{
    EFItem *it = in->item;
    EFMatrix id;
    char buf[256];
    const EFMatrix *mt = it ? &it->m : &id;

    mat_ident(&id);
    switch (idx) {
    case P_X:           return vnum(mt->tx / 20.0);
    case P_Y:           return vnum(mt->ty / 20.0);
    case P_XSCALE:      return vnum(sqrt(mt->a * mt->a + mt->b * mt->b) * 100.0);
    case P_YSCALE:      return vnum(sqrt(mt->c * mt->c + mt->d * mt->d) * 100.0);
    case P_ROTATION:    return vnum(atan2(mt->b, mt->a) * 180.0 / M_PI);
    case P_CURFRAME:    return vnum(in->cur + 1);
    case P_TOTALFRAMES:
    case P_FRAMESLOADED: return vnum(in->tl ? in->tl->nframes : 1);
    case P_ALPHA:       return vnum(it ? it->cx.mul[3] * 100.0 / 256 : 100);
    case P_VISIBLE:     return vbool(it ? it->visible : 1);
    case P_NAME:        return vstr(it && it->name ? it->name : "");
    case P_TARGET:
        clip_path(m, in, buf, sizeof(buf));
        return vstr(intern(m, buf + 7));                /* without "_level0" */
    case P_HIGHQUALITY: return vnum(1);
    case P_QUALITY:     return vstr("HIGH");
    default:            return vnum(0);
    }
}

static void set_prop(EFMovie *m, EFInst *in, int idx, const EFVal *v)
{
    EFItem *it = in->item;
    double d = to_num(v);
    EFMatrix *mt;

    if (!it)
        return;
    mt = &it->m;
    if (isnan(d) && idx != P_NAME)
        return;
    switch (idx) {
    case P_X: mt->tx = d * 20; break;
    case P_Y: mt->ty = d * 20; break;
    case P_XSCALE:
    case P_YSCALE:
    case P_ROTATION: {
        double sx = sqrt(mt->a * mt->a + mt->b * mt->b), sy = sqrt(mt->c * mt->c + mt->d * mt->d);
        double rot = atan2(mt->b, mt->a), skew = atan2(-mt->c, mt->d) - rot;
        if (mt->a * mt->d - mt->b * mt->c < 0)
            sy = -sy;
        if (idx == P_XSCALE)
            sx = d / 100;
        else if (idx == P_YSCALE)
            sy = d / 100;
        else
            rot = d * M_PI / 180;
        mt->a = sx * cos(rot);
        mt->b = sx * sin(rot);
        mt->c = -sy * sin(rot + skew);
        mt->d = sy * cos(rot + skew);
        break;
    }
    case P_ALPHA: it->cx.mul[3] = (int)(d * 256 / 100); break;
    case P_VISIBLE: it->visible = to_bool(v); break;
    case P_NAME: it->name = intern(m, to_str(m, v)); break;
    }
}

static EFVal get_var(EFCtx *c, const char *name);

static EFVal get_member(EFCtx *c, const EFVal *obj, const char *name)
{
    EFMovie *m = c->m;
    static const struct { const char *n; int code; } keys[] = {
        { "BACKSPACE", 8 }, { "TAB", 9 }, { "ENTER", 13 }, { "SHIFT", 16 }, { "CONTROL", 17 },
        { "CAPSLOCK", 20 }, { "ESCAPE", 27 }, { "SPACE", 32 }, { "PGUP", 33 }, { "PGDN", 34 },
        { "END", 35 }, { "HOME", 36 }, { "LEFT", 37 }, { "UP", 38 }, { "RIGHT", 39 },
        { "DOWN", 40 }, { "INSERT", 45 }, { "DELETEKEY", 46 },
    };
    int i;

    switch (obj->t) {
    case V_CLIP: {
        EFInst *in = inst_get(m, obj->id), *ch;
        if (!in)
            return vundef();
        if ((i = prop_index(name)) >= 0)
            return get_prop(m, in, i);
        if (!ef_strcasecmp(name, "_parent"))
            return in->parent ? vclip(in->parent) : vundef();
        if ((i = vars_find(in->vars, in->nvars, name)) >= 0)
            return in->vars[i].v;
        if ((ch = child_clip(in, name)))
            return vclip(ch);
        return vundef();
    }
    case V_MATH:
        if (!strcmp(name, "PI")) return vnum(M_PI);
        if (!strcmp(name, "E"))  return vnum(M_E);
        if (!strcmp(name, "SQRT2")) return vnum(M_SQRT2);
        return vundef();
    case V_KEY:
        for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
            if (!strcmp(keys[i].n, name))
                return vnum(keys[i].code);
        return vundef();
    case V_STR:
        if (!strcmp(name, "length"))
            return vnum(strlen(obj->s));
        return vundef();
    default:
        return vundef();
    }
}

static void set_member(EFCtx *c, const EFVal *obj, const char *name, EFVal v)
{
    EFInst *in;
    int i;

    if (obj->t != V_CLIP || !(in = inst_get(c->m, obj->id)))
        return;
    if ((i = prop_index(name)) >= 0)
        set_prop(c->m, in, i, &v);
    else
        vars_set(c->m, &in->vars, &in->nvars, &in->avars, name, v);
}

static EFVal get_var(EFCtx *c, const char *name)
{
    EFMovie *m = c->m;
    EFInst *in;
    const char *sep = NULL, *s;
    int i;

    for (s = name; *s; s++)
        if (*s == ':' || *s == '.' || *s == '/')
            sep = s;
    if (sep && sep != name) {
        char path[256];
        int n = EF_MIN(sep - name, (int)sizeof(path) - 1);
        EFVal o;
        memcpy(path, name, n);
        path[n] = 0;
        in = resolve_path(c, path);
        if (!in)
            return vundef();
        o = vclip(in);
        return get_member(c, &o, sep + 1);
    }
    if (c->infunc && (i = vars_find(c->locals, c->nlocals, name)) >= 0)
        return c->locals[i].v;
    if (!ef_strcasecmp(name, "this"))
        return inst_get(m, c->self) ? vclip(inst_get(m, c->self)) : vundef();
    if (!ef_strcasecmp(name, "_root") || !ef_strcasecmp(name, "_level0"))
        return vclip(&m->root);
    if (!strcmp(name, "Math"))
        return vkind(V_MATH);
    if (!strcmp(name, "Key"))
        return vkind(V_KEY);
    if (!strcmp(name, "_global"))
        return vkind(V_OBJ);
    in = ctx_target(c);
    if (in) {
        EFVal o = vclip(in), r;
        if (!ef_strcasecmp(name, "_parent"))
            return in->parent ? vclip(in->parent) : vundef();
        if ((i = prop_index(name)) >= 0)
            return get_prop(m, in, i);
        r = get_member(c, &o, name);
        if (r.t != V_UNDEF)
            return r;
    }
    /* functions also see the timeline they were defined on */
    if (c->scope != c->target && (in = inst_get(m, c->scope))) {
        EFVal o = vclip(in);
        return get_member(c, &o, name);
    }
    return vundef();
}

static void set_var(EFCtx *c, const char *name, EFVal v)
{
    EFInst *in;
    const char *sep = NULL, *s;
    int i;

    for (s = name; *s; s++)
        if (*s == ':' || *s == '.' || *s == '/')
            sep = s;
    if (sep && sep != name) {
        char path[256];
        int n = EF_MIN(sep - name, (int)sizeof(path) - 1);
        EFVal o;
        memcpy(path, name, n);
        path[n] = 0;
        if (!(in = resolve_path(c, path)))
            return;
        o = vclip(in);
        set_member(c, &o, sep + 1, v);
        return;
    }
    if (c->infunc && (i = vars_find(c->locals, c->nlocals, name)) >= 0) {
        c->locals[i].v = v;
        return;
    }
    if ((in = ctx_target(c))) {
        EFVal o = vclip(in);
        set_member(c, &o, name, v);
    }
}

static int func_new(EFMovie *m, EFCtx *c, const uint8_t *a, int alen, int v2, const char **name,
                    int *codesize)
{
    EFFunc f;
    int q = 0, i, np;

    memset(&f, 0, sizeof(f));
    *name = (const char *)a;
    while (q < alen && a[q])
        q++;
    q++;
    if (q + 2 > alen)
        return -1;
    np = rd16(a + q);
    q += 2;
    f.v2 = v2;
    if (v2) {
        if (q + 3 > alen)
            return -1;
        f.nregs = a[q];
        f.flags = rd16(a + q + 1);
        q += 3;
    }
    for (i = 0; i < np && q < alen; i++) {
        int r = 0;
        if (v2)
            r = a[q++];
        if (i < 16) {
            f.param[i] = (const char *)a + q;
            f.preg[i] = r;
            f.nparams = i + 1;
        }
        while (q < alen && a[q])
            q++;
        q++;
    }
    if (q + 2 > alen)
        return -1;
    *codesize = rd16(a + q);
    f.code = a + alen;
    f.len = *codesize;
    f.scope = c->target;
    f.pool = c->pool;                   /* pools are owned by the movie */
    f.npool = c->npool;
    /* the same definition runs again on every frame loop: reuse its slot */
    for (i = 0; i < m->nfuncs; i++)
        if (m->funcs[i].code == f.code && m->funcs[i].scope == f.scope)
            return i;
    if (m->nfuncs == m->afuncs) {
        EFFunc *t;
        if (m->afuncs >= 4096)
            return -1;
        t = EF_REALLOC(m->funcs, (m->afuncs * 2 + 16) * sizeof(*t));
        if (!t)
            return -1;
        m->funcs = t;
        m->afuncs = m->afuncs * 2 + 16;
    }
    m->funcs[m->nfuncs] = f;
    return m->nfuncs++;
}

static EFVal call_func(EFMovie *m, int fi, int self, EFVal *args, int nargs)
{
    EFFunc *f;
    EFCtx *c;
    EFVal r;
    EFInst *scope;
    int i, reg = 1;

    if (fi < 0 || fi >= m->nfuncs || m->calldepth >= EF_MAXCALL)
        return vundef();
    f = &m->funcs[fi];
    scope = inst_get(m, f->scope);
    if (!scope)
        scope = &m->root;
    c = EF_MALLOCZ(sizeof(*c));
    if (!c)
        return vundef();
    ctx_init(c, m, scope);
    if (inst_get(m, self))
        c->self = self;
    c->infunc = 1;
    c->pool = f->pool;
    c->npool = f->npool;
    if (f->v2) {
        EFInst *s = inst_get(m, c->self);
        if (f->flags & 0x0001) c->regs[reg++] = s ? vclip(s) : vundef();   /* this */
        if (f->flags & 0x0004) reg++;                                       /* arguments */
        if (f->flags & 0x0010) reg++;                                       /* super */
        if (f->flags & 0x0040) c->regs[reg++] = vclip(&m->root);
        if (f->flags & 0x0080) c->regs[reg++] = scope->parent ? vclip(scope->parent) : vundef();
        if (f->flags & 0x0100) c->regs[reg++] = vkind(V_OBJ);              /* _global */
    }
    for (i = 0; i < f->nparams; i++) {
        EFVal v = i < nargs ? args[i] : vundef();
        if (f->v2 && f->preg[i])
            c->regs[f->preg[i]] = v;
        else
            vars_set(m, &c->locals, &c->nlocals, &c->alocals, f->param[i], v);
    }
    m->calldepth++;
    r = run_code(c, f->code, f->len);
    m->calldepth--;
    EF_FREE(c->locals);
    EF_FREE(c);
    return r;
}

static EFInst *attach_movie(EFMovie *m, EFInst *in, const char *linkage, const char *name, int depth)
{
    EFChar *ch = NULL;
    EFItem *it;
    int i;

    for (i = 0; i < m->nexports; i++)
        if (!ef_strcasecmp(m->exports[i].name, linkage))
            ch = get_char(m, m->exports[i].id);
    if (!ch || ch->type != EF_SPRITE || !in)
        return NULL;
    it = dl_insert(m, in, depth);
    if (!it)
        return NULL;
    it->charid = ch - m->chars;
    it->name = intern(m, name);
    return inst_new_sprite(m, in, it, ch->tl);
}

static int clip_goto_arg(EFInst *in, const EFVal *v)
{
    if (v->t == V_STR) {
        char *e;
        double d = strtod(v->s, &e);
        if (e != v->s && !*e)
            return (int)d - 1;
        return tl_label(in->tl, v->s);
    }
    return (int)to_num(v) - 1;
}

static uint32_t ef_rand(EFMovie *m)
{
    m->rnd = m->rnd * 1664525u + 1013904223u;
    return m->rnd >> 8;
}

/* returns 1 when name was a built-in movie clip method */
static int clip_method(EFCtx *c, EFInst *in, const char *name, EFVal *args, int nargs, EFVal *ret)
{
    EFMovie *m = c->m;
    EFVal a0 = nargs > 0 ? args[0] : vundef();

    *ret = vundef();
    if (!ef_strcasecmp(name, "play")) {
        in->playing = 1;
    } else if (!ef_strcasecmp(name, "stop")) {
        in->playing = 0;
    } else if (!ef_strcasecmp(name, "gotoAndPlay") || !ef_strcasecmp(name, "gotoAndStop")) {
        int f = clip_goto_arg(in, &a0);
        if (f >= 0)
            inst_goto(m, in, f);
        in->playing = name[9] == 'P' || name[9] == 'p';
    } else if (!ef_strcasecmp(name, "nextFrame") || !ef_strcasecmp(name, "prevFrame")) {
        inst_goto(m, in, EF_MAX(in->cur + (name[0] == 'n' || name[0] == 'N' ? 1 : -1), 0));
        in->playing = 0;
    } else if (!ef_strcasecmp(name, "attachMovie")) {
        EFInst *n = nargs >= 3 ? attach_movie(m, in, to_str(m, &args[0]), to_str(m, &args[1]),
                                              (int)to_num(&args[2])) : NULL;
        if (n)
            *ret = vclip(n);
    } else if (!ef_strcasecmp(name, "createEmptyMovieClip")) {
        if (nargs >= 2) {
            EFItem *it = dl_insert(m, in, (int)to_num(&args[1]));
            if (it) {
                it->charid = -1;
                it->name = intern(m, to_str(m, &args[0]));
                *ret = vclip(inst_new_sprite(m, in, it, NULL));
            }
        }
    } else if (!ef_strcasecmp(name, "duplicateMovieClip")) {
        EFInst *p = in->parent;
        if (p && in->item && nargs >= 2) {
            EFItem src = *in->item, *it = dl_insert(m, p, (int)to_num(&args[1]));
            EFChar *ch = get_char(m, src.charid);
            if (it) {
                it->charid = src.charid;
                it->m = src.m;
                it->cx = src.cx;
                it->clipev = src.clipev;
                it->clipevlen = src.clipevlen;
                it->name = intern(m, to_str(m, &args[0]));
                if (ch && ch->type == EF_SPRITE)
                    *ret = vclip(inst_new_sprite(m, p, it, ch->tl));
            }
        }
    } else if (!ef_strcasecmp(name, "removeMovieClip")) {
        if (in->parent && in->item && in->item->depth >= 0)
            dl_remove(m, in->parent, in->item->depth);
    } else if (!ef_strcasecmp(name, "swapDepths")) {
        EFInst *p = in->parent;
        if (p && in->item && nargs >= 1) {
            int nd, od = in->item->depth, i;
            EFItem *other;
            if (a0.t == V_CLIP) {
                EFInst *o = inst_get(m, a0.id);
                if (!o || o->parent != p || !o->item)
                    return 1;
                nd = o->item->depth;
            } else {
                nd = (int)to_num(&a0);
            }
            other = dl_find(p, nd);
            if (other)
                other->depth = od;
            in->item->depth = nd;
            for (i = 1; i < p->ndl; i++) {             /* re-sort */
                int j = i;
                while (j > 0 && p->dl[j - 1]->depth > p->dl[j]->depth) {
                    EFItem *t = p->dl[j];
                    p->dl[j] = p->dl[j - 1];
                    p->dl[j - 1] = t;
                    j--;
                }
            }
        }
    } else if (!ef_strcasecmp(name, "getDepth")) {
        *ret = vnum(in->item ? in->item->depth : 0);
    } else if (!ef_strcasecmp(name, "getBytesLoaded") || !ef_strcasecmp(name, "getBytesTotal")) {
        *ret = vnum(1);
    } else if (!ef_strcasecmp(name, "hitTest")) {
        *ret = vbool(0);
    } else if (!ef_strcasecmp(name, "getTimer")) {
        *ret = vnum(m->tick * 1000.0 / (m->rate > 0 ? m->rate : 12));
    } else {
        return 0;
    }
    return 1;
}

static EFVal math_method(EFMovie *m, const char *name, EFVal *args, int nargs)
{
    double x = nargs > 0 ? to_num(&args[0]) : NAN, y = nargs > 1 ? to_num(&args[1]) : NAN;
    if (!strcmp(name, "abs"))    return vnum(fabs(x));
    if (!strcmp(name, "floor"))  return vnum(floor(x));
    if (!strcmp(name, "ceil"))   return vnum(ceil(x));
    if (!strcmp(name, "round"))  return vnum(floor(x + 0.5));
    if (!strcmp(name, "sqrt"))   return vnum(sqrt(x));
    if (!strcmp(name, "sin"))    return vnum(sin(x));
    if (!strcmp(name, "cos"))    return vnum(cos(x));
    if (!strcmp(name, "tan"))    return vnum(tan(x));
    if (!strcmp(name, "asin"))   return vnum(asin(x));
    if (!strcmp(name, "acos"))   return vnum(acos(x));
    if (!strcmp(name, "atan"))   return vnum(atan(x));
    if (!strcmp(name, "atan2"))  return vnum(atan2(x, y));
    if (!strcmp(name, "exp"))    return vnum(exp(x));
    if (!strcmp(name, "log"))    return vnum(log(x));
    if (!strcmp(name, "pow"))    return vnum(pow(x, y));
    if (!strcmp(name, "min"))    return vnum(x < y ? x : y);
    if (!strcmp(name, "max"))    return vnum(x > y ? x : y);
    if (!strcmp(name, "random")) return vnum((ef_rand(m) & 0xFFFFFF) / 16777216.0);
    return vundef();
}

static EFVal call_method(EFCtx *c, const EFVal *obj, const char *name, EFVal *args, int nargs)
{
    EFMovie *m = c->m;
    EFVal r;

    if (!name || !*name)
        return obj->t == V_FUNC ? call_func(m, obj->id, c->self, args, nargs) : vundef();
    switch (obj->t) {
    case V_MATH:
        return math_method(m, name, args, nargs);
    case V_KEY:
        return !strcmp(name, "getCode") || !strcmp(name, "getAscii") ? vnum(0) : vbool(0);
    case V_CLIP: {
        EFInst *in = inst_get(m, obj->id);
        EFVal f;
        if (!in)
            return vundef();
        f = get_member(c, obj, name);
        if (f.t == V_FUNC)
            return call_func(m, f.id, in->id, args, nargs);
        if (clip_method(c, in, name, args, nargs, &r))
            return r;
        return vundef();
    }
    case V_STR: {
        const char *s = obj->s;
        int l = strlen(s), a = nargs > 0 ? (int)to_num(&args[0]) : 0, b = nargs > 1 ? (int)to_num(&args[1]) : l;
        if (!strcmp(name, "toUpperCase") || !strcmp(name, "toLowerCase")) {
            char *t = EF_MALLOCZ(l + 1);
            int i;
            if (!t)
                return vstr("");
            for (i = 0; i < l; i++)
                t[i] = name[2] == 'U' ? (s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i])
                                      : (s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
            r = vstr(intern(m, t));
            EF_FREE(t);
            return r;
        }
        if (!strcmp(name, "charAt"))
            return a >= 0 && a < l ? vstr(intern_n(m, s + a, 1)) : vstr("");
        if (!strcmp(name, "charCodeAt"))
            return a >= 0 && a < l ? vnum((uint8_t)s[a]) : vnum(NAN);
        if (!strcmp(name, "substr") || !strcmp(name, "substring")) {
            if (a < 0) a = name[6] ? 0 : EF_MAX(l + a, 0);
            if (a > l) a = l;
            b = name[6] ? EF_MIN(EF_MAX(b, 0), l) - a : EF_MIN(b, l - a);
            if (b < 0) b = 0;
            return vstr(intern_n(m, s + a, b));
        }
        if (!strcmp(name, "indexOf")) {
            const char *f = nargs > 0 ? strstr(s, to_str(m, &args[0])) : NULL;
            return vnum(f ? f - s : -1);
        }
        return vundef();
    }
    default:
        return vundef();
    }
}

static EFVal call_named(EFCtx *c, const char *name, EFVal *args, int nargs)
{
    EFMovie *m = c->m;
    EFVal f = get_var(c, name), r;
    EFInst *in;

    if (f.t == V_FUNC)
        return call_func(m, f.id, c->self, args, nargs);
    if (!ef_strcasecmp(name, "getTimer"))
        return vnum(m->tick * 1000.0 / (m->rate > 0 ? m->rate : 12));
    if (!strcmp(name, "random"))
        return vnum(nargs > 0 && to_num(&args[0]) >= 1 ? (int)(ef_rand(m) % (uint32_t)to_num(&args[0])) : 0);
    if (!strcmp(name, "int"))
        return vnum(nargs > 0 ? trunc(to_num(&args[0])) : 0);
    if (!strcmp(name, "Number") || !strcmp(name, "parseFloat"))
        return vnum(nargs > 0 ? to_num(&args[0]) : 0);
    if (!strcmp(name, "parseInt"))
        return vnum(nargs > 0 ? trunc(to_num(&args[0])) : NAN);
    if (!strcmp(name, "String"))
        return vstr(nargs > 0 ? to_str(m, &args[0]) : "");
    if (!strcmp(name, "Boolean"))
        return vbool(nargs > 0 && to_bool(&args[0]));
    if (!strcmp(name, "isNaN"))
        return vbool(nargs > 0 && isnan(to_num(&args[0])));
    /* the timeline is in the scope chain, so its clip methods are reachable */
    in = ctx_target(c);
    if (in && clip_method(c, in, name, args, nargs, &r))
        return r;
    return vundef();
}

static int vals_equal(EFMovie *m, const EFVal *a, const EFVal *b)
{
    if ((a->t == V_UNDEF || a->t == V_NULL) && (b->t == V_UNDEF || b->t == V_NULL))
        return 1;
    if (a->t == V_UNDEF || a->t == V_NULL || b->t == V_UNDEF || b->t == V_NULL)
        return 0;
    if (a->t == V_STR && b->t == V_STR)
        return !strcmp(a->s, b->s);
    if (a->t >= V_CLIP || b->t >= V_CLIP)
        return a->t == b->t && a->id == b->id;
    return to_num(a) == to_num(b);
}

static int vals_less(EFMovie *m, const EFVal *a, const EFVal *b)
{
    if (a->t == V_STR && b->t == V_STR)
        return strcmp(a->s, b->s) < 0;
    return to_num(a) < to_num(b);
}

static const char *typeof_str(const EFVal *v)
{
    switch (v->t) {
    case V_NUM:  return "number";
    case V_BOOL: return "boolean";
    case V_STR:  return "string";
    case V_CLIP: return "movieclip";
    case V_FUNC: return "function";
    case V_NULL: return "null";
    case V_UNDEF: return "undefined";
    default:     return "object";
    }
}

/* constant pools are parsed once per ConstantPool record and kept until close */
static const char **pool_get(EFMovie *m, const uint8_t *a, int alen, int *count)
{
    const char *s, *end;
    const char **np;
    int i, n;

    *count = 0;
    for (i = 0; i < m->npools; i++)
        if (m->pools[i].key == a) {
            *count = m->pools[i].n;
            return m->pools[i].s;
        }
    if (alen < 2 || m->npools >= 4096)
        return NULL;
    if (m->npools == m->apools) {
        EFPool *t = EF_REALLOC(m->pools, (m->apools * 2 + 16) * sizeof(*t));
        if (!t)
            return NULL;
        m->pools = t;
        m->apools = m->apools * 2 + 16;
    }
    n = rd16(a);
    s = (const char *)a + 2;
    end = (const char *)a + alen;
    np = EF_MALLOCZ((n + 1) * sizeof(*np));
    if (!np)
        return NULL;
    for (i = 0; i < n && s < end; i++) {
        np[i] = s;
        while (s < end && *s)
            s++;
        s++;
    }
    m->pools[m->npools].key = a;
    m->pools[m->npools].s = np;
    m->pools[m->npools].n = i;
    m->npools++;
    *count = i;
    return np;
}

static EFVal run_code(EFCtx *c, const uint8_t *p, int len)
{
    EFMovie *m = c->m;
    EFVal *st, ret = vundef();
    int sp = 0, pc = 0;

    st = EF_MALLOCZ(EF_STACK * sizeof(*st));
    if (!st)
        return ret;
#define POP()   (sp > 0 ? st[--sp] : vundef())
#define PUSH(x) do { EFVal t_ = (x); if (sp < EF_STACK) st[sp++] = t_; } while (0)

    while (pc < len && m->ops++ < EF_MAX_OPS) {
        int op = p[pc++], alen = 0;
        const uint8_t *a = NULL;
        EFInst *tgt;

        if (!op)
            break;
        if (op >= 0x80) {
            if (pc + 2 > len)
                break;
            alen = rd16(p + pc);
            pc += 2;
            if (pc + alen > len)
                break;
            a = p + pc;
            pc += alen;
        }
        tgt = ctx_target(c);
        switch (op) {
        /* timeline control */
        case 0x04: case 0x05:                       /* NextFrame, PrevFrame */
            if (tgt) {
                inst_goto(m, tgt, EF_MAX(tgt->cur + (op == 0x04 ? 1 : -1), 0));
                tgt->playing = 0;
            }
            break;
        case 0x06: if (tgt) tgt->playing = 1; break;
        case 0x07: if (tgt) tgt->playing = 0; break;
        case 0x81:                                  /* GotoFrame */
            if (tgt && alen >= 2) {
                inst_goto(m, tgt, rd16(a));
                tgt->playing = 0;
            }
            break;
        case 0x8C:                                  /* GoToLabel */
            if (tgt && alen >= 1) {
                int f = tl_label(tgt->tl, (const char *)a);
                if (f >= 0)
                    inst_goto(m, tgt, f);
                tgt->playing = 0;
            }
            break;
        case 0x9F: {                                /* GotoFrame2 */
            EFVal v = POP();
            if (tgt && alen >= 1) {
                int f = clip_goto_arg(tgt, &v);
                if ((a[0] & 2) && alen >= 3)
                    f += rd16(a + 1);
                if (f >= 0)
                    inst_goto(m, tgt, f);
                tgt->playing = a[0] & 1;
            }
            break;
        }
        case 0x8B: {                                /* SetTarget */
            EFInst *in = alen && a[0] ? resolve_path(c, (const char *)a) : inst_get(m, c->scope);
            c->target = in ? in->id : -1;
            break;
        }
        case 0x20: {                                /* SetTarget2 */
            EFVal v = POP();
            EFInst *in = v.t == V_CLIP ? inst_get(m, v.id) :
                         *to_str(m, &v) ? resolve_path(c, to_str(m, &v)) : inst_get(m, c->scope);
            c->target = in ? in->id : -1;
            break;
        }
        case 0x8A:                                  /* WaitForFrame: everything is loaded */
        case 0x08: case 0x09:                       /* ToggleQuality, StopSounds */
        case 0x28:                                  /* EndDrag */
            break;
        case 0x8D:                                  /* WaitForFrame2 */
            sp = EF_MAX(sp - 1, 0);
            break;
        case 0x83:                                  /* GetURL */
            break;
        case 0x9A:                                  /* GetURL2 */
            sp = EF_MAX(sp - 2, 0);
            break;
        case 0x27: {                                /* StartDrag */
            EFVal cons;
            sp = EF_MAX(sp - 2, 0);
            cons = POP();
            if (to_bool(&cons))
                sp = EF_MAX(sp - 4, 0);
            break;
        }
        case 0x24: {                                /* CloneSprite */
            EFVal d = POP(), nm = POP(), src = POP(), args[2], r;
            EFInst *in = resolve_path(c, to_str(m, &src));
            args[0] = nm;
            args[1] = vnum(to_num(&d) - EF_TLDEPTH);
            if (in)
                clip_method(c, in, "duplicateMovieClip", args, 2, &r);
            break;
        }
        case 0x25: {                                /* RemoveSprite */
            EFVal t = POP(), r;
            EFInst *in = t.t == V_CLIP ? inst_get(m, t.id) : resolve_path(c, to_str(m, &t));
            if (in)
                clip_method(c, in, "removeMovieClip", NULL, 0, &r);
            break;
        }
        /* stack and constants */
        case 0x96: {                                /* Push */
            int q = 0;
            while (q < alen) {
                int t = a[q++];
                if (t == 0) {
                    int s = q;
                    while (q < alen && a[q])
                        q++;
                    PUSH(vstr(intern_n(m, (const char *)a + s, q - s)));
                    q++;
                } else if (t == 1 && q + 4 <= alen) {
                    union { uint32_t i; float f; } u;
                    u.i = rd32(a + q);
                    PUSH(vnum(u.f));
                    q += 4;
                } else if (t == 2) {
                    PUSH(vkind(V_NULL));
                } else if (t == 3) {
                    PUSH(vundef());
                } else if (t == 4 && q + 1 <= alen) {
                    PUSH(c->regs[a[q]]);
                    q++;
                } else if (t == 5 && q + 1 <= alen) {
                    PUSH(vbool(a[q]));
                    q++;
                } else if (t == 6 && q + 8 <= alen) {
                    union { uint64_t i; double d; } u;
                    u.i = (uint64_t)rd32(a + q + 4) | (uint64_t)rd32(a + q) << 32;
                    PUSH(vnum(u.d));
                    q += 8;
                } else if (t == 7 && q + 4 <= alen) {
                    PUSH(vnum((int32_t)rd32(a + q)));
                    q += 4;
                } else if ((t == 8 || t == 9) && q + (t == 8 ? 1 : 2) <= alen) {
                    int idx = t == 8 ? a[q] : rd16(a + q);
                    q += t == 8 ? 1 : 2;
                    PUSH(idx < c->npool ? vstr(c->pool[idx]) : vundef());
                } else {
                    break;
                }
            }
            break;
        }
        case 0x88:                                  /* ConstantPool */
            c->pool = pool_get(m, a, alen, &c->npool);
            break;
        case 0x17: sp = EF_MAX(sp - 1, 0); break;  /* Pop */
        case 0x4C:                                  /* PushDuplicate */
            if (sp) PUSH(st[sp - 1]);
            break;
        case 0x4D:                                  /* StackSwap */
            if (sp >= 2) { EFVal t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; }
            break;
        case 0x87:                                  /* StoreRegister */
            if (alen >= 1 && sp)
                c->regs[a[0]] = st[sp - 1];
            break;
        /* variables and members */
        case 0x1C: { EFVal n = POP(); PUSH(get_var(c, to_str(m, &n))); break; }
        case 0x1D: { EFVal v = POP(), n = POP(); set_var(c, to_str(m, &n), v); break; }
        case 0x3C: {                                /* DefineLocal */
            EFVal v = POP(), n = POP();
            if (c->infunc)
                vars_set(m, &c->locals, &c->nlocals, &c->alocals, to_str(m, &n), v);
            else
                set_var(c, to_str(m, &n), v);
            break;
        }
        case 0x41: {                                /* DefineLocal2 */
            EFVal n = POP();
            if (c->infunc && vars_find(c->locals, c->nlocals, to_str(m, &n)) < 0)
                vars_set(m, &c->locals, &c->nlocals, &c->alocals, to_str(m, &n), vundef());
            break;
        }
        case 0x4E: { EFVal n = POP(), o = POP(); PUSH(get_member(c, &o, to_str(m, &n))); break; }
        case 0x4F: { EFVal v = POP(), n = POP(), o = POP(); set_member(c, &o, to_str(m, &n), v); break; }
        case 0x22: {                                /* GetProperty */
            EFVal i = POP(), t = POP();
            EFInst *in = t.t == V_CLIP ? inst_get(m, t.id) : resolve_path(c, to_str(m, &t));
            int idx = (int)to_num(&i);
            PUSH(in && idx >= 0 && idx < P_NB ? get_prop(m, in, idx) : vundef());
            break;
        }
        case 0x23: {                                /* SetProperty */
            EFVal v = POP(), i = POP(), t = POP();
            EFInst *in = t.t == V_CLIP ? inst_get(m, t.id) : resolve_path(c, to_str(m, &t));
            int idx = (int)to_num(&i);
            if (in && idx >= 0 && idx < P_NB)
                set_prop(m, in, idx, &v);
            break;
        }
        case 0x3A: sp = EF_MAX(sp - 2, 0); PUSH(vbool(1)); break;  /* Delete */
        case 0x3B: sp = EF_MAX(sp - 1, 0); PUSH(vbool(1)); break;  /* Delete2 */
        case 0x46: case 0x55:                       /* Enumerate(2): no enumerable members */
            sp = EF_MAX(sp - 1, 0);
            PUSH(vkind(V_NULL));
            break;
        case 0x44: { EFVal v = POP(); PUSH(vstr(typeof_str(&v))); break; }
        /* calls */
        case 0x3D: case 0x40: {                     /* CallFunction, NewObject */
            EFVal n = POP(), cnt = POP(), args[16], r;
            int na = EF_MAX((int)to_num(&cnt), 0), i;
            for (i = 0; i < na; i++) {
                EFVal v = POP();
                if (i < 16)
                    args[i] = v;
            }
            r = op == 0x3D ? call_named(c, to_str(m, &n), args, EF_MIN(na, 16)) : vkind(V_OBJ);
            PUSH(r);
            break;
        }
        case 0x52: case 0x53: {                     /* CallMethod, NewMethod */
            EFVal n = POP(), o = POP(), cnt = POP(), args[16], r;
            int na = EF_MAX((int)to_num(&cnt), 0), i;
            for (i = 0; i < na; i++) {
                EFVal v = POP();
                if (i < 16)
                    args[i] = v;
            }
            r = op == 0x52 ? call_method(c, &o, n.t == V_UNDEF ? "" : to_str(m, &n), args, EF_MIN(na, 16))
                           : vkind(V_OBJ);
            PUSH(r);
            break;
        }
        case 0x3E: ret = POP(); pc = len; break;    /* Return */
        case 0x9B: case 0x8E: {                     /* DefineFunction(2) */
            const char *name;
            int size = 0, fi = func_new(m, c, a, alen, op == 0x8E, &name, &size);
            if (size < 0 || size > len - pc)
                size = len - pc;
            if (fi >= 0) {
                EFVal f = { .t = V_FUNC };
                f.id = fi;
                m->funcs[fi].len = size;
                if (*name)
                    set_var(c, name, f);
                else
                    PUSH(f);
            }
            pc += size;
            break;
        }
        case 0x42: {                                /* InitArray */
            EFVal n = POP();
            sp = EF_MAX(sp - EF_MAX((int)to_num(&n), 0), 0);
            PUSH(vkind(V_OBJ));
            break;
        }
        case 0x43: {                                /* InitObject */
            EFVal n = POP();
            sp = EF_MAX(sp - 2 * EF_MAX((int)to_num(&n), 0), 0);
            PUSH(vkind(V_OBJ));
            break;
        }
        case 0x94:                                  /* With: run the block in the current scope */
            sp = EF_MAX(sp - 1, 0);
            break;
        case 0x26: sp = EF_MAX(sp - 1, 0); break;  /* Trace */
        case 0x2A: pc = len; break;                 /* Throw */
        /* control flow */
        case 0x99:
            if (alen >= 2)
                pc += rd16s(a);
            break;
        case 0x9D: {
            EFVal v = POP();
            if (alen >= 2 && to_bool(&v))
                pc += rd16s(a);
            break;
        }
        case 0x9E: sp = EF_MAX(sp - 1, 0); break;  /* Call (frame actions) */
        /* arithmetic, SWF 4 style */
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: {
            EFVal bv = POP(), av = POP();
            double x = to_num(&av), y = to_num(&bv);
            switch (op) {
            case 0x0A: PUSH(vnum(x + y)); break;
            case 0x0B: PUSH(vnum(x - y)); break;
            case 0x0C: PUSH(vnum(x * y)); break;
            case 0x0D: PUSH(vnum(x / y)); break;
            case 0x0E: PUSH(vbool(x == y)); break;
            case 0x0F: PUSH(vbool(x < y)); break;
            case 0x10: PUSH(vbool(to_bool(&av) && to_bool(&bv))); break;
            case 0x11: PUSH(vbool(to_bool(&av) || to_bool(&bv))); break;
            }
            break;
        }
        case 0x12: { EFVal v = POP(); PUSH(vbool(!to_bool(&v))); break; }
        case 0x13: { EFVal bv = POP(), av = POP(); PUSH(vbool(!strcmp(to_str(m, &av), to_str(m, &bv)))); break; }
        case 0x29: { EFVal bv = POP(), av = POP(); PUSH(vbool(strcmp(to_str(m, &av), to_str(m, &bv)) < 0)); break; }
        case 0x68: { EFVal bv = POP(), av = POP(); PUSH(vbool(strcmp(to_str(m, &av), to_str(m, &bv)) > 0)); break; }
        case 0x14: case 0x31: { EFVal v = POP(); PUSH(vnum(strlen(to_str(m, &v)))); break; }
        case 0x21: {                                /* StringAdd */
            EFVal bv = POP(), av = POP();
            const char *x = to_str(m, &av), *y = to_str(m, &bv);
            int lx = strlen(x), ly = strlen(y);
            char *t = EF_MALLOCZ(lx + ly + 1);
            if (t) {
                memcpy(t, x, lx);
                memcpy(t + lx, y, ly);
                PUSH(vstr(intern(m, t)));
                EF_FREE(t);
            } else {
                PUSH(vstr(""));
            }
            break;
        }
        case 0x15: case 0x35: {                     /* StringExtract */
            EFVal cnt = POP(), idx = POP(), s = POP();
            const char *x = to_str(m, &s);
            int l = strlen(x), i = (int)to_num(&idx) - 1, n = (int)to_num(&cnt);
            if (i < 0) i = 0;
            if (i > l) i = l;
            if (n < 0 || n > l - i) n = l - i;
            PUSH(vstr(intern_n(m, x + i, n)));
            break;
        }
        case 0x18: { EFVal v = POP(); PUSH(vnum(trunc(to_num(&v)))); break; }
        case 0x32: case 0x36: { EFVal v = POP(); PUSH(vnum((uint8_t)to_str(m, &v)[0])); break; }
        case 0x33: case 0x37: {
            EFVal v = POP();
            char ch = (char)(int)to_num(&v);
            PUSH(vstr(intern_n(m, &ch, ch ? 1 : 0)));
            break;
        }
        case 0x30: {                                /* RandomNumber */
            EFVal v = POP();
            uint32_t n = (uint32_t)EF_MAX(to_num(&v), 0);
            PUSH(vnum(n ? ef_rand(m) % n : 0));
            break;
        }
        case 0x34: PUSH(vnum(m->tick * 1000.0 / (m->rate > 0 ? m->rate : 12))); break;
        /* SWF 5 operators */
        case 0x47: {                                /* Add2 */
            EFVal bv = POP(), av = POP();
            if (av.t == V_STR || bv.t == V_STR) {
                const char *x = to_str(m, &av), *y = to_str(m, &bv);
                int lx = strlen(x), ly = strlen(y);
                char *t = EF_MALLOCZ(lx + ly + 1);
                if (t) {
                    memcpy(t, x, lx);
                    memcpy(t + lx, y, ly);
                    PUSH(vstr(intern(m, t)));
                    EF_FREE(t);
                } else {
                    PUSH(vstr(""));
                }
            } else {
                PUSH(vnum(to_num(&av) + to_num(&bv)));
            }
            break;
        }
        case 0x48: { EFVal bv = POP(), av = POP(); PUSH(vbool(vals_less(m, &av, &bv))); break; }
        case 0x67: { EFVal bv = POP(), av = POP(); PUSH(vbool(vals_less(m, &bv, &av))); break; }
        case 0x49: { EFVal bv = POP(), av = POP(); PUSH(vbool(vals_equal(m, &av, &bv))); break; }
        case 0x66: {                                /* StrictEquals */
            EFVal bv = POP(), av = POP();
            PUSH(vbool(av.t == bv.t && vals_equal(m, &av, &bv)));
            break;
        }
        case 0x3F: { EFVal bv = POP(), av = POP(); PUSH(vnum(fmod(to_num(&av), to_num(&bv)))); break; }
        case 0x50: { EFVal v = POP(); PUSH(vnum(to_num(&v) + 1)); break; }
        case 0x51: { EFVal v = POP(); PUSH(vnum(to_num(&v) - 1)); break; }
        case 0x4A: { EFVal v = POP(); PUSH(vnum(to_num(&v))); break; }
        case 0x4B: { EFVal v = POP(); PUSH(vstr(to_str(m, &v))); break; }
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: {
            EFVal bv = POP(), av = POP();
            double dx = to_num(&av), dy = to_num(&bv);
            int32_t x = isnan(dx) ? 0 : (int32_t)(int64_t)dx, y = isnan(dy) ? 0 : (int32_t)(int64_t)dy;
            switch (op) {
            case 0x60: PUSH(vnum(x & y)); break;
            case 0x61: PUSH(vnum(x | y)); break;
            case 0x62: PUSH(vnum(x ^ y)); break;
            case 0x63: PUSH(vnum((int32_t)((uint32_t)x << (y & 31)))); break;
            case 0x64: PUSH(vnum(x >> (y & 31))); break;
            case 0x65: PUSH(vnum((uint32_t)x >> (y & 31))); break;
            }
            break;
        }
        case 0x2B: case 0x54: case 0x69: {          /* CastOp, InstanceOf, Extends */
            sp = EF_MAX(sp - 2, 0);
            if (op != 0x69)
                PUSH(op == 0x54 ? vbool(0) : vkind(V_OBJ));
            break;
        }
        default:
            break;
        }
    }
#undef POP
#undef PUSH
    EF_FREE(st);
    return ret;
}

static void run_queue(EFMovie *m)
{
    int i;
    for (i = 0; i < m->nq && m->ops < EF_MAX_OPS; i++) {
        EFInst *in = inst_get(m, m->queue[i].id);
        EFCtx *c;
        if (!in)
            continue;
        c = EF_MALLOCZ(sizeof(*c));
        if (!c)
            break;
        ctx_init(c, m, in);
        run_code(c, m->queue[i].p, m->queue[i].len);
        EF_FREE(c);
    }
    m->nq = 0;
}

/* button actions for a press followed by a release (DefineButton: release only) */
static void button_click(EFMovie *m, EFInst *owner, const EFChar *c)
{
    const uint8_t *p = c->tag, *end = c->tag + c->taglen;

    if (c->tagcode == 34) {
        int off, pass;
        if (c->taglen < 5 || !(off = rd16(p + 3)))
            return;
        for (pass = 0; pass < 2; pass++) {
            const uint8_t *q = p + 3 + off;
            while (q + 4 <= end) {
                int size = rd16(q), cond = rd16(q + 2);
                const uint8_t *e = size ? q + size : end;
                if (e > end)
                    e = end;
                if (cond & (pass ? 0x08 : 0x04))        /* OverUpToOverDown, then OverDownToOverUp */
                    queue_code(m, owner, q + 4, e - q - 4);
                if (!size)
                    break;
                q += size;
            }
        }
    } else {
        p += 2;
        while (p < end && *p) {                     /* skip the character records */
            BR b;
            EFMatrix bm;
            br_init(&b, p + 5, end);
            rd_matrix(&b, &bm);
            p = br_ptr(&b);
        }
        if (p < end)
            queue_code(m, owner, p + 1, end - p - 1);
    }
}

/* press a button that has been on stage for m->autoclick frames */
static int autoclick(EFMovie *m, EFInst *in, int lvl)
{
    int i;
    for (i = 0; i < in->ndl; i++) {
        EFItem *it = in->dl[i];
        EFChar *c = get_char(m, it->charid);
        if (c && c->type == EF_BUTTON && it->visible && it->ticks >= m->autoclick) {
            it->ticks = 0;
            button_click(m, in, c);
            return 1;
        }
        if (it->sub && lvl < EF_MAXNEST && autoclick(m, it->sub, lvl + 1))
            return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- raster */

typedef struct DEdge { float x0, y0, x1, y1; } DEdge;

typedef struct DEdges { DEdge *e; int n, a; } DEdges;

static void de_add(DEdges *d, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;
    if (d->n == d->a) {
        d->a = d->a ? d->a * 2 : 256;
        d->e = EF_REALLOC(d->e, d->a * sizeof(DEdge));
    }
    d->e[d->n].x0 = x0;
    d->e[d->n].y0 = y0;
    d->e[d->n].x1 = x1;
    d->e[d->n].y1 = y1;
    d->n++;
}

typedef struct Paint {
    int type;                           /* 0 solid, 1 gradient */
    int c[4];
    int ramp[256][4];
    EFMatrix inv;                       /* device -> gradient space */
    int radial;
} Paint;

typedef struct SortX { float x; int w; } SortX;

static int cmpsx(const void *a, const void *b)
{
    float x = ((const SortX *)a)->x, y = ((const SortX *)b)->x;
    return x < y ? -1 : x > y;
}

/* evenodd: even-odd rule (shape fills), else nonzero (stroke outlines) */
static void fill_edges(EFMovie *m, DEdges *d, const Paint *pt, int mask_mode, int evenodd)
{
    int W = m->W, H = m->H, i, y, ymin, ymax;
    float fymin = 1e30f, fymax = -1e30f;
    SortX *xs;
    int cap = d->n + 2;

    if (!d->n)
        return;
    for (i = 0; i < d->n; i++) {
        DEdge *e = &d->e[i];
        if (e->y0 < fymin) fymin = e->y0;
        if (e->y1 < fymin) fymin = e->y1;
        if (e->y0 > fymax) fymax = e->y0;
        if (e->y1 > fymax) fymax = e->y1;
    }
    ymin = EF_MAX(0, (int)floorf(fymin));
    ymax = EF_MIN(H - 1, (int)ceilf(fymax));
    xs = EF_MALLOCZ(cap * sizeof(*xs));
    if (!xs)
        return;
    for (y = ymin; y <= ymax; y++) {
        int s, any = 0, xlo = W, xhi = -1;
        float *cov = m->cov;

        for (s = 0; s < EF_SUB; s++) {
            float sy = y + (s + 0.5f) / EF_SUB;
            int n = 0, wnd = 0;

            for (i = 0; i < d->n; i++) {
                DEdge *e = &d->e[i];
                float y0 = e->y0, y1 = e->y1;
                int dir = 1;
                if (y0 > y1) {
                    float t = y0; y0 = y1; y1 = t;
                    dir = -1;
                }
                if (sy < y0 || sy >= y1)
                    continue;
                xs[n].x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                xs[n].w = dir;
                n++;
            }
            if (n < 2)
                continue;
            qsort(xs, n, sizeof(*xs), cmpsx);
            for (i = 0; i < n - 1; i++) {
                float xa, xb;
                int ia, ib, ix;
                wnd += evenodd ? 1 : xs[i].w;
                if (evenodd ? !(wnd & 1) : !wnd)
                    continue;
                xa = xs[i].x;
                xb = xs[i + 1].x;
                if (xb <= 0 || xa >= W)
                    continue;
                if (xa < 0) xa = 0;
                if (xb > W) xb = W;
                if (xb <= xa)
                    continue;
                if (!any) {
                    memset(cov, 0, W * sizeof(float));
                    any = 1;
                }
                ia = (int)xa;
                ib = (int)xb;
                if (ia < xlo) xlo = ia;
                if (ib > xhi) xhi = ib;
                if (ia == ib) {
                    cov[ia] += (xb - xa) / EF_SUB;
                } else {
                    cov[ia] += (ia + 1 - xa) / EF_SUB;
                    for (ix = ia + 1; ix < ib; ix++)
                        cov[ix] += 1.0f / EF_SUB;
                    if (ib < W)
                        cov[ib] += (xb - ib) / EF_SUB;
                }
            }
        }
        if (!any)
            continue;
        if (xhi >= W)
            xhi = W - 1;
        for (i = xlo; i <= xhi; i++) {
            float c = cov[i];
            if (c <= 0.0f)
                continue;
            if (c > 1.0f)
                c = 1.0f;
            if (mask_mode) {
                if (c > m->maskbuild[y * W + i])
                    m->maskbuild[y * W + i] = c;
                continue;
            }
            if (m->mask)
                c *= m->mask[y * W + i];
            {
                uint8_t *px = m->canvas + (y * W + i) * 4;
                int col[4];
                float a;
                if (pt->type == 0) {
                    col[0] = pt->c[0]; col[1] = pt->c[1]; col[2] = pt->c[2]; col[3] = pt->c[3];
                } else {
                    float gx = pt->inv.a * (i + 0.5f) + pt->inv.c * (y + 0.5f) + pt->inv.tx;
                    float gy = pt->inv.b * (i + 0.5f) + pt->inv.d * (y + 0.5f) + pt->inv.ty;
                    float t = pt->radial ? sqrtf(gx * gx + gy * gy) / 16384.0f : (gx + 16384.0f) / 32768.0f;
                    int ti = (int)(t * 255.0f + 0.5f);
                    ti = ti < 0 ? 0 : ti > 255 ? 255 : ti;
                    col[0] = pt->ramp[ti][0]; col[1] = pt->ramp[ti][1];
                    col[2] = pt->ramp[ti][2]; col[3] = pt->ramp[ti][3];
                }
                a = c * col[3] / 255.0f;
                px[0] += (int)lrintf((col[0] - px[0]) * a);
                px[1] += (int)lrintf((col[1] - px[1]) * a);
                px[2] += (int)lrintf((col[2] - px[2]) * a);
            }
        }
    }
    EF_FREE(xs);
}

static void flatten_edge(DEdges *d, const EFEdge *e, const EFMatrix *M, int rev)
{
    float x0 = M->a * e->x0 + M->c * e->y0 + M->tx, y0 = M->b * e->x0 + M->d * e->y0 + M->ty;
    float x1 = M->a * e->x1 + M->c * e->y1 + M->tx, y1 = M->b * e->x1 + M->d * e->y1 + M->ty;

    if (!e->curve) {
        if (rev)
            de_add(d, x1, y1, x0, y0);
        else
            de_add(d, x0, y0, x1, y1);
    } else {
        float cx = M->a * e->cx + M->c * e->cy + M->tx, cy = M->b * e->cx + M->d * e->cy + M->ty;
        float len = fabsf(cx - x0) + fabsf(cy - y0) + fabsf(x1 - cx) + fabsf(y1 - cy);
        int n = 3 + (int)(len / 3.0f), i;
        float px = x0, py = y0;
        if (n > 40) n = 40;
        for (i = 1; i <= n; i++) {
            float t = (float)i / n, u = 1 - t;
            float qx = u * u * x0 + 2 * u * t * cx + t * t * x1;
            float qy = u * u * y0 + 2 * u * t * cy + t * t * y1;
            if (rev)
                de_add(d, qx, qy, px, py);
            else
                de_add(d, px, py, qx, qy);
            px = qx;
            py = qy;
        }
    }
}

/* round join / cap: an octagon wound the same way as the segment quads */
static void add_disc(DEdges *d, float x, float y, float r)
{
    float px = x + r, py = y;
    int k;
    for (k = 1; k <= 8; k++) {
        float ang = -k * (float)M_PI / 4, qx = x + r * cosf(ang), qy = y + r * sinf(ang);
        de_add(d, px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

static void make_ramp(Paint *pt, const EFFill *f, const EFCx *cx)
{
    int i, g;
    for (i = 0; i < 256; i++) {
        int c0[4], c1[4], out[4];
        float t;
        if (!f->ngrad) {
            pt->ramp[i][0] = pt->ramp[i][1] = pt->ramp[i][2] = 0;
            pt->ramp[i][3] = 255;
            continue;
        }
        g = 0;
        while (g + 1 < f->ngrad && f->grad[g + 1].ratio <= i)
            g++;
        if (i <= f->grad[0].ratio || g + 1 >= f->ngrad) {
            int k = (i <= f->grad[0].ratio) ? 0 : f->ngrad - 1;
            cx_apply(cx, f->grad[k].c, out);
            memcpy(pt->ramp[i], out, sizeof(out));
            continue;
        }
        cx_apply(cx, f->grad[g].c, c0);
        cx_apply(cx, f->grad[g + 1].c, c1);
        t = (float)(i - f->grad[g].ratio) / EF_MAX(1, f->grad[g + 1].ratio - f->grad[g].ratio);
        for (int k = 0; k < 4; k++)
            pt->ramp[i][k] = (int)(c0[k] + (c1[k] - c0[k]) * t);
    }
}

/* draw a shape.  override: NULL or an RGBA used for fill 1 (glyphs) */
static void draw_shape(EFMovie *m, const EFShape *s, const EFMatrix *M, const EFCx *cx,
                       const uint8_t *override, int mask_mode)
{
    int fi, li, i;
    DEdges d = { 0 };
    float sc = sqrtf(fabsf(M->a * M->d - M->b * M->c));

    if (!s || !s->nedge)
        return;
    for (fi = 1; fi <= s->nfill; fi++) {
        const EFFill *f = &s->fill[fi];
        Paint pt;
        int used = 0;

        d.n = 0;
        for (i = 0; i < s->nedge; i++) {
            const EFEdge *e = &s->edge[i];
            if (e->fill1 == fi) {
                flatten_edge(&d, e, M, 0);
                used = 1;
            }
            if (e->fill0 == fi) {
                flatten_edge(&d, e, M, 1);
                used = 1;
            }
        }
        if (!used)
            continue;
        memset(&pt, 0, sizeof(pt));
        if (override) {
            int c[4];
            cx_apply(cx, override, c);
            memcpy(pt.c, c, sizeof(c));
        } else if (f->type == 0) {
            int c[4];
            cx_apply(cx, f->c, c);
            memcpy(pt.c, c, sizeof(c));
        } else if (f->type == 0x10 || f->type == 0x12 || f->type == 0x13) {
            EFMatrix gm = mat_mul(M, &f->gm), inv;
            pt.type = 1;
            pt.radial = f->type != 0x10;
            make_ramp(&pt, f, cx);
            if (mat_inv(&gm, &inv) < 0)
                continue;
            pt.inv = inv;
        } else {
            pt.c[0] = pt.c[1] = pt.c[2] = 128;      /* bitmap fills are not supported */
            pt.c[3] = 255;
        }
        fill_edges(m, &d, &pt, mask_mode, 1);
    }
    if (mask_mode)
        goto out;
    for (li = 1; li <= s->nline; li++) {
        const EFLine *l = &s->line[li];
        Paint pt;
        float hw = EF_MAX(l->width * sc, 1.0f) * 0.5f;
        int c[4], used = 0;

        d.n = 0;
        for (i = 0; i < s->nedge; i++) {
            const EFEdge *e = &s->edge[i];
            DEdges tmp = { 0 };
            int k;

            if (e->line != li)
                continue;
            flatten_edge(&tmp, e, M, 0);
            for (k = 0; k < tmp.n; k++) {
                DEdge *t = &tmp.e[k];
                float dx = t->x1 - t->x0, dy = t->y1 - t->y0, ln = sqrtf(dx * dx + dy * dy), nx, ny;
                if (ln < 1e-6f)
                    continue;
                nx = -dy / ln * hw;
                ny = dx / ln * hw;
                de_add(&d, t->x0 + nx, t->y0 + ny, t->x1 + nx, t->y1 + ny);
                de_add(&d, t->x1 + nx, t->y1 + ny, t->x1 - nx, t->y1 - ny);
                de_add(&d, t->x1 - nx, t->y1 - ny, t->x0 - nx, t->y0 - ny);
                de_add(&d, t->x0 - nx, t->y0 - ny, t->x0 + nx, t->y0 + ny);
                if (hw > 0.75f) {
                    add_disc(&d, t->x0, t->y0, hw);
                    add_disc(&d, t->x1, t->y1, hw);
                }
                used = 1;
            }
            EF_FREE(tmp.e);
        }
        if (!used)
            continue;
        memset(&pt, 0, sizeof(pt));
        cx_apply(cx, l->c, c);
        memcpy(pt.c, c, sizeof(c));
        fill_edges(m, &d, &pt, 0, 0);
    }
out:
    EF_FREE(d.e);
}

/* --------------------------------------------------------------- display */

static void draw_item(EFMovie *m, EFInst *owner, EFItem *it, const EFMatrix *P, const EFCx *PC, int mask_mode);

static void draw_text(EFMovie *m, const EFChar *c, const EFMatrix *M, const EFCx *cx, int alpha)
{
    const uint8_t *p = c->tag, *end = c->tag + c->taglen;
    BR b;
    int r[4], gbits, abits;
    EFMatrix tm, TM;
    int fontid = -1, x = 0, y = 0, height = 0;
    uint8_t color[4] = { 0, 0, 0, 255 };

    if (c->taglen < 4)
        return;
    br_init(&b, p + 2, end);
    rd_rect(&b, r);
    rd_matrix(&b, &tm);
    p = br_ptr(&b);
    if (p + 2 > end)
        return;
    gbits = p[0];
    abits = p[1];
    p += 2;
    TM = mat_mul(M, &tm);
    while (p < end && *p) {
        int flags = *p++;
        if (flags & 0x80) {
            if (flags & 8) {
                fontid = rd16(p);
                p += 2;
            }
            if (flags & 4) {
                color[0] = p[0]; color[1] = p[1]; color[2] = p[2];
                color[3] = alpha ? p[3] : 255;
                p += alpha ? 4 : 3;
            }
            if (flags & 1) {
                x = rd16s(p);
                p += 2;
            }
            if (flags & 2) {
                y = rd16s(p);
                p += 2;
            }
            if (flags & 8) {
                height = rd16(p);
                p += 2;
            }
        } else {
            int n = flags & 0x7f, i;
            EFChar *fc = get_char(m, fontid);
            br_init(&b, p, end);
            for (i = 0; i < n; i++) {
                int gi = br_bits(&b, gbits), adv = br_sbits(&b, abits);
                if (fc && fc->font && gi < fc->font->nglyph) {
                    EFMatrix gm, S = { height / 1024.0f, 0, 0, height / 1024.0f, (float)x, (float)y };
                    gm = mat_mul(&TM, &S);
                    draw_shape(m, &fc->font->glyph[gi], &gm, cx, color, 0);
                }
                x += adv;
            }
            br_align(&b);
            p = br_ptr(&b);
        }
    }
}

typedef struct EFEdit {
    int r[4];
    int fontid, height, align, lm, rm, leading;
    int wrap, multi, html, outlines;
    uint8_t color[4];
    const char *var;                    /* bound variable or NULL */
    const char *text;                   /* initial text or NULL */
} EFEdit;

static int edit_parse(const EFChar *c, EFEdit *e)
{
    const uint8_t *p = c->tag + 2, *end = c->tag + c->taglen;
    BR b;
    int flags;

    memset(e, 0, sizeof(*e));
    e->fontid = -1;
    e->height = 240;
    e->color[3] = 255;
    if (c->taglen < 6)
        return -1;
    br_init(&b, p, end);
    rd_rect(&b, e->r);
    p = br_ptr(&b);
    if (p + 2 > end)
        return -1;
    /* first byte: HasText WordWrap Multiline Password ReadOnly HasTextColor HasMaxLength HasFont,
     * second: HasFontClass AutoSize HasLayout NoSelect Border WasStatic HTML UseOutlines */
    flags = p[0] << 8 | p[1];
    p += 2;
    e->wrap     = flags & 0x4000;
    e->multi    = flags & 0x2000;
    e->html     = flags & 0x0002;
    e->outlines = flags & 0x0001;
    if (flags & 0x0100) {                       /* HasFont */
        if (p + 2 > end)
            return -1;
        e->fontid = rd16(p);
        p += 2;
    }
    if (flags & 0x0080) {                       /* HasFontClass */
        while (p < end && *p)
            p++;
        p++;
    }
    if (flags & 0x0100) {
        if (p + 2 > end)
            return -1;
        e->height = rd16(p);
        p += 2;
    }
    if (flags & 0x0400) {                       /* HasTextColor */
        if (p + 4 > end)
            return -1;
        memcpy(e->color, p, 4);
        p += 4;
    }
    if (flags & 0x0200)                         /* HasMaxLength */
        p += 2;
    if (flags & 0x0020) {                       /* HasLayout */
        if (p + 9 > end)
            return -1;
        e->align   = p[0];
        e->lm      = rd16(p + 1);
        e->rm      = rd16(p + 3);
        e->leading = rd16s(p + 7);
        p += 9;
    }
    if (p >= end)
        return -1;
    if (*p)
        e->var = (const char *)p;
    while (p < end && *p)
        p++;
    p++;
    if ((flags & 0x8000) && p < end)
        e->text = (const char *)p;
    return 0;
}

/* a field bound to a variable sets it to its initial text when it is undefined */
static void edittext_bind(EFMovie *m, EFInst *in, const EFChar *c)
{
    EFEdit e;
    EFCtx *x;

    if (edit_parse(c, &e) < 0 || !e.var || !e.text)
        return;
    x = EF_MALLOCZ(sizeof(*x));
    if (!x)
        return;
    ctx_init(x, m, in);
    if (get_var(x, e.var).t == V_UNDEF)
        set_var(x, e.var, vstr(intern(m, e.text)));
    EF_FREE(x);
}

/* device text: the 8x16 VGA font stands in for the player's system font */
#define EF_DEV_ADV    0.5f              /* advance in ems */
#define EF_DEV_ASC    0.75f             /* baseline at row 12 of 16 */

static void draw_dev_glyph(EFMovie *m, int code, const EFMatrix *M, float em, const EFCx *cx,
                           const uint8_t *color)
{
    const uint8_t *font = ef_vga16_font();
    float u = em / 16;                  /* one font pixel in twips */
    DEdges d = { 0 };
    Paint pt;
    int row, c[4];

    if (!font || code < 0 || code > 255)
        return;
    for (row = 0; row < 16; row++) {
        int bits = font[code * 16 + row], x = 0;
        while (x < 8) {
            int x1;
            if (!(bits & (0x80 >> x))) {
                x++;
                continue;
            }
            for (x1 = x; x1 < 8 && (bits & (0x80 >> x1)); x1++)
                ;
            {
                float X0 = x * u, X1 = x1 * u, Y0 = (row - 12) * u, Y1 = (row - 11) * u;
                float px[4] = { X0, X1, X1, X0 }, py[4] = { Y0, Y0, Y1, Y1 }, tx[4], ty[4];
                int k;
                for (k = 0; k < 4; k++) {
                    tx[k] = M->a * px[k] + M->c * py[k] + M->tx;
                    ty[k] = M->b * px[k] + M->d * py[k] + M->ty;
                }
                for (k = 0; k < 4; k++)
                    de_add(&d, tx[k], ty[k], tx[(k + 1) & 3], ty[(k + 1) & 3]);
            }
            x = x1;
        }
    }
    memset(&pt, 0, sizeof(pt));
    cx_apply(cx, color, c);
    memcpy(pt.c, c, sizeof(c));
    fill_edges(m, &d, &pt, 0, 0);
    EF_FREE(d.e);
}

static void draw_edittext(EFMovie *m, EFInst *owner, const EFChar *c, const EFMatrix *M, const EFCx *cx)
{
    EFEdit e;
    const char *text = NULL;
    EFFont *font = NULL;
    EFChar *fc;
    int maxw, y, dev;
    float scale, asc, lineh;
    char *buf, *q;

    if (edit_parse(c, &e) < 0)
        return;
    if (e.var && owner) {
        EFCtx *x = EF_MALLOCZ(sizeof(*x));
        if (x) {
            EFVal v;
            ctx_init(x, m, owner);
            v = get_var(x, e.var);
            if (v.t != V_UNDEF)
                text = to_str(m, &v);
            EF_FREE(x);
        }
    }
    if (!text)
        text = e.text;
    if (!text || !*text)
        return;
    fc = get_char(m, e.fontid);
    if (fc && fc->type == EF_FONT && fc->font && fc->font->nglyph)
        font = fc->font;
    dev = !e.outlines || !font;
    scale = e.height / 1024.0f;
    if (dev) {
        asc = e.height * EF_DEV_ASC;
        lineh = e.height * 1.15f;
    } else {
        asc = font->has_layout ? font->ascent * scale : e.height * 0.9f;
        lineh = font->has_layout ? (font->ascent + font->descent) * scale : e.height * 1.2f;
    }
    lineh += e.leading;
    maxw = e.r[1] - e.r[0] - 80 - e.lm - e.rm;

    buf = EF_MALLOCZ(strlen(text) + 1);
    if (!buf)
        return;
    q = buf;
    if (e.html) {
        int intag = 0;
        const char *s;
        for (s = text; *s; s++) {
            if (*s == '<')
                intag = 1;
            else if (*s == '>')
                intag = 0;
            else if (!intag)
                *q++ = *s;
        }
        *q = 0;
    } else {
        strcpy(buf, text);
    }

#define ADV(ch) (dev ? e.height * EF_DEV_ADV : \
                 (gi = font_glyph_for_code(font, (uint8_t)(ch))) >= 0 && font->adv ? \
                 font->adv[gi] * scale : e.height / 2.0f)
    y = e.r[2] + 40 + (int)asc;
    {
        char *line = buf;
        while (*line) {
            char *nl = strchr(line, '\r'), *nl2 = strchr(line, '\n'), *next;
            int len, start = 0, gi;
            if (nl2 && (!nl || nl2 < nl))
                nl = nl2;
            len = nl ? nl - line : (int)strlen(line);
            next = nl ? nl + 1 : line + len;
            /* split the paragraph into wrapped lines */
            while (start <= len) {
                int pos, lastbreak = -1, endl = len, k;
                float wsum = 0, lw = 0, x0;
                for (pos = start; pos < len; pos++) {
                    float a = ADV(line[pos]);
                    if (line[pos] == ' ')
                        lastbreak = pos;
                    if (e.wrap && e.multi && wsum + a > maxw && pos > start) {
                        endl = lastbreak > start ? lastbreak : pos;
                        break;
                    }
                    wsum += a;
                }
                for (k = start; k < endl; k++)
                    lw += ADV(line[k]);
                x0 = e.r[0] + 40.0f + e.lm;
                if (e.align == 1)
                    x0 = e.r[1] - 40.0f - e.rm - lw;
                else if (e.align == 2)
                    x0 = (e.r[0] + e.lm + e.r[1] - e.rm) / 2.0f - lw / 2.0f;
                for (k = start; k < endl; k++) {
                    int code = (uint8_t)line[k];
                    if (dev) {
                        EFMatrix S = { 1, 0, 0, 1, x0, (float)y }, gm = mat_mul(M, &S);
                        if (code != ' ')
                            draw_dev_glyph(m, code, &gm, e.height, cx, e.color);
                    } else if ((gi = font_glyph_for_code(font, code)) >= 0 && gi < font->nglyph) {
                        EFMatrix S = { scale, 0, 0, scale, x0, (float)y }, gm = mat_mul(M, &S);
                        draw_shape(m, &font->glyph[gi], &gm, cx, e.color, 0);
                    }
                    x0 += ADV(code);
                }
                y += (int)lineh;
                if (endl >= len)
                    break;
                start = endl;
                while (start < len && line[start] == ' ')
                    start++;
                if (!(e.wrap && e.multi))
                    break;
            }
            line = next;
        }
    }
#undef ADV
    EF_FREE(buf);
}

static void draw_inst(EFMovie *m, EFInst *in, const EFMatrix *P, const EFCx *PC);

static void draw_button(EFMovie *m, EFInst *owner, const EFChar *c, const EFMatrix *M, const EFCx *cx)
{
    const uint8_t *p = c->tag, *end = c->tag + c->taglen;
    int v2 = c->tagcode == 34, nq = m->nq;

    if (c->taglen < (v2 ? 5 : 2))
        return;
    p += v2 ? 5 : 2;                            /* id, [flags, action offset] */
    while (p + 5 <= end && *p) {
        int flags = *p++;
        int charid = rd16(p), depth = rd16(p + 2);
        BR b;
        EFItem tmp;
        EFInst sub;
        EFChar *cc;

        p += 4;
        memset(&tmp, 0, sizeof(tmp));
        br_init(&b, p, end);
        rd_matrix(&b, &tmp.m);
        cx_ident(&tmp.cx);
        if (v2)
            rd_cxform(&b, &tmp.cx, 1);
        p = br_ptr(&b);
        if (!(flags & 1))                       /* up state only */
            continue;
        tmp.depth = depth;
        tmp.charid = charid;
        tmp.visible = 1;
        cc = get_char(m, charid);
        if (cc && cc->type == EF_SPRITE && m->nest < EF_MAXNEST) {
            /* a still copy of the sprite's first frame */
            memset(&sub, 0, sizeof(sub));
            sub.tl = cc->tl;
            sub.cur = -1;
            sub.parent = owner;
            sub.item = &tmp;
            tmp.sub = &sub;
            inst_register(m, &sub);
            m->nest++;
            inst_goto(m, &sub, 0);
            draw_inst(m, &sub, M, cx);
            m->nest--;
            inst_free(m, &sub);
            m->nq = nq;                         /* its frame actions are not run */
        } else {
            draw_item(m, owner, &tmp, M, cx, 0);
        }
    }
}

static void draw_inst(EFMovie *m, EFInst *in, const EFMatrix *P, const EFCx *PC)
{
    int i;

    for (i = 0; i < in->ndl; i++) {
        EFItem *it = in->dl[i];
        if (!it->visible)
            continue;
        if (it->isclip) {
            int j, n = m->W * m->H;
            float *saved = m->mask;
            /* build the mask from this item, then draw the items it covers */
            if (!m->maskbuild)
                m->maskbuild = EF_MALLOCZ(n * sizeof(float));
            if (!m->maskbuild)
                continue;
            memset(m->maskbuild, 0, n * sizeof(float));
            draw_item(m, in, it, P, PC, 1);
            m->mask = EF_MALLOCZ(n * sizeof(float));
            if (!m->mask) {
                m->mask = saved;
                continue;
            }
            memcpy(m->mask, m->maskbuild, n * sizeof(float));
            if (saved)
                for (j = 0; j < n; j++)
                    m->mask[j] *= saved[j];
            for (j = i + 1; j < in->ndl && in->dl[j]->depth <= it->clipdepth; j++)
                if (in->dl[j]->visible)
                    draw_item(m, in, in->dl[j], P, PC, 0);
            EF_FREE(m->mask);
            m->mask = saved;
            i = j - 1;
            continue;
        }
        draw_item(m, in, it, P, PC, 0);
    }
}

static void draw_item(EFMovie *m, EFInst *owner, EFItem *it, const EFMatrix *P, const EFCx *PC, int mask_mode)
{
    EFChar *c = get_char(m, it->charid);
    EFMatrix M = mat_mul(P, &it->m);
    EFCx cx = cx_mul(PC, &it->cx);

    if (it->sub) {
        if (m->nest >= EF_MAXNEST)
            return;
        m->nest++;
        if (mask_mode) {
            /* masks are built from shapes only */
            int i;
            for (i = 0; i < it->sub->ndl; i++)
                draw_item(m, it->sub, it->sub->dl[i], &M, &cx, 1);
        } else {
            draw_inst(m, it->sub, &M, &cx);
        }
        m->nest--;
        return;
    }
    if (!c)
        return;
    switch (c->type) {
    case EF_SHAPE:
        draw_shape(m, c->shape, &M, &cx, NULL, mask_mode);
        break;
    case EF_TEXT:
        if (!mask_mode)
            draw_text(m, c, &M, &cx, c->tagcode == 33);
        break;
    case EF_EDIT:
        if (!mask_mode)
            draw_edittext(m, owner, c, &M, &cx);
        break;
    case EF_BUTTON:
        if (!mask_mode && m->nest < EF_MAXNEST) {
            m->nest++;
            draw_button(m, owner, c, &M, &cx);
            m->nest--;
        }
        break;
    }
}

/* -------------------------------------------------------------- top level */

static int chars_ensure(EFMovie *m, int id)
{
    if (id < 0 || id > 65535)
        return -1;
    if (id >= m->nchars) {
        int n = id + 64;
        EFChar *t = EF_REALLOC(m->chars, n * sizeof(EFChar));
        if (!t)
            return -1;
        m->chars = t;
        memset(m->chars + m->nchars, 0, (n - m->nchars) * sizeof(EFChar));
        m->nchars = n;
    }
    return 0;
}

static void parse_tags(EFMovie *m, const uint8_t *p, const uint8_t *end)
{
    int code, len;
    const uint8_t *data;

    while (next_tag(&p, end, &code, &data, &len)) {
        int id = len >= 2 ? rd16(data) : -1;

        if (code == 0)
            break;
        switch (code) {
        case 2: case 22: case 32: {
            EFShape *s = parse_shape(data, len, code == 2 ? 1 : code == 22 ? 2 : 3, &id);
            if (!s || chars_ensure(m, id) < 0 || m->chars[id].type != EF_NONE) {
                free_shape(s);
                EF_FREE(s);
                break;
            }
            m->chars[id].type = EF_SHAPE;
            m->chars[id].shape = s;
            break;
        }
        case 10: case 48:
            if (chars_ensure(m, id) < 0 || m->chars[id].type != EF_NONE)
                break;
            if (code == 10)
                parse_font1(&m->chars[id], data, len);
            else
                parse_font2(&m->chars[id], data, len);
            break;
        case 13:
            if (chars_ensure(m, id) >= 0 && m->chars[id].font && !m->chars[id].font->code)
                parse_fontinfo(&m->chars[id], data, len);
            break;
        case 11: case 33: case 37: case 7: case 34:
            if (chars_ensure(m, id) < 0 || m->chars[id].type != EF_NONE)
                break;
            m->chars[id].type = code == 37 ? EF_EDIT : (code == 7 || code == 34) ? EF_BUTTON : EF_TEXT;
            m->chars[id].tag = data;
            m->chars[id].taglen = len;
            m->chars[id].tagcode = code;
            break;
        case 39: {
            EFTimeline *tl;
            if (len < 4 || chars_ensure(m, id) < 0 || m->chars[id].type != EF_NONE)
                break;
            tl = EF_MALLOCZ(sizeof(*tl));
            if (!tl)
                break;
            tl_build(tl, data + 4, data + len, rd16(data + 2));
            m->chars[id].type = EF_SPRITE;
            m->chars[id].tl = tl;
            break;
        }
        case 56: {                              /* ExportAssets: linkage names for attachMovie */
            int n = len >= 2 ? rd16(data) : 0, i, q = 2;
            EFExport *t;
            if (n <= 0 || n > 65535)
                break;
            t = EF_REALLOC(m->exports, (m->nexports + n) * sizeof(*t));
            if (!t)
                break;
            m->exports = t;
            for (i = 0; i < n && q + 2 < len; i++) {
                int s = q + 2;
                while (s < len && data[s])
                    s++;
                if (s >= len)
                    break;
                m->exports[m->nexports].id = rd16(data + q);
                m->exports[m->nexports].name = intern_n(m, (const char *)data + q + 2, s - q - 2);
                m->nexports++;
                q = s + 1;
            }
            break;
        }
        default:
            break;
        }
    }
}

EFMovie *ff_emflash_open(const uint8_t *swf, int size, int out_w, int out_h)
{
    EFMovie *m;
    BR b;
    int r[4], nfr;
    const uint8_t *p;

    if (size < 12 || memcmp(swf, "FWS", 3))
        return NULL;
    m = EF_MALLOCZ(sizeof(*m));
    if (!m)
        return NULL;
    m->version = swf[3];
    br_init(&b, swf + 8, swf + size);
    rd_rect(&b, r);
    p = br_ptr(&b);
    if (p + 4 > swf + size) {
        EF_FREE(m);
        return NULL;
    }
    m->xmin = r[0];
    m->ymin = r[2];
    m->w_twips = r[1] - r[0];
    m->h_twips = r[3] - r[2];
    m->rate = rd16(p) / 256.0f;
    nfr = rd16(p + 2);
    p += 4;
    m->bg[0] = m->bg[1] = m->bg[2] = 255;
    m->rnd = 1;
    if (m->w_twips <= 0 || m->h_twips <= 0) {
        EF_FREE(m);
        return NULL;
    }
    m->W = out_w > 0 ? out_w : (m->w_twips + 10) / 20;
    m->H = out_h > 0 ? out_h : (m->h_twips + 10) / 20;
    if (m->W <= 0 || m->H <= 0 || m->W > 8192 || m->H > 8192) {
        EF_FREE(m);
        return NULL;
    }
    m->scale = (float)m->W / m->w_twips;
    parse_tags(m, p, swf + size);
    tl_build(&m->root_tl, p, swf + size, nfr);
    m->nframes_root = m->root_tl.nframes;
    m->root.tl = &m->root_tl;
    m->root.cur = -1;
    m->root.playing = 1;
    inst_register(m, &m->root);
    m->cov = EF_MALLOCZ(m->W * sizeof(float));
    m->canvas = EF_MALLOCZ(m->W * m->H * 4);
    if (!m->cov || !m->canvas) {
        ff_emflash_close(m);
        return NULL;
    }
    if (m->root_tl.nframes)
        inst_goto(m, &m->root, 0);
    run_queue(m);
    return m;
}

void ff_emflash_set_autoclick(EFMovie *m, int frames) { m->autoclick = frames > 0 ? frames : 0; }
void ff_emflash_set_seed(EFMovie *m, uint32_t seed) { m->rnd = seed; }

void ff_emflash_get_size(const EFMovie *m, int *w, int *h)
{
    *w = m->W;
    *h = m->H;
}

float ff_emflash_get_rate(const EFMovie *m) { return m->rate; }
int ff_emflash_get_nframes(const EFMovie *m) { return m->nframes_root; }

void ff_emflash_render(EFMovie *m, uint8_t *rgba, int stride)
{
    int i, y;
    EFMatrix P = { m->scale, 0, 0, m->scale, -m->xmin * m->scale, -m->ymin * m->scale };
    EFCx cx;

    for (i = 0; i < m->W * m->H; i++) {
        m->canvas[4 * i + 0] = m->bg[0];
        m->canvas[4 * i + 1] = m->bg[1];
        m->canvas[4 * i + 2] = m->bg[2];
        m->canvas[4 * i + 3] = 255;
    }
    cx_ident(&cx);
    m->nest = 0;
    draw_inst(m, &m->root, &P, &cx);
    for (y = 0; y < m->H; y++)
        memcpy(rgba + y * stride, m->canvas + y * m->W * 4, m->W * 4);
}

void ff_emflash_step(EFMovie *m)
{
    m->ops = 0;
    m->nest = 0;
    m->tick++;
    inst_tick(m, &m->root);
    inst_clear_fresh(&m->root, 0);
    run_queue(m);
    if (m->autoclick && autoclick(m, &m->root, 0))
        run_queue(m);
}

static void free_chars(EFMovie *m)
{
    int i, g;
    for (i = 0; i < m->nchars; i++) {
        EFChar *c = &m->chars[i];
        if (c->shape) {
            free_shape(c->shape);
            EF_FREE(c->shape);
        }
        if (c->tl) {
            tl_free(c->tl);
            EF_FREE(c->tl);
        }
        if (c->font) {
            for (g = 0; g < c->font->nglyph; g++)
                free_shape(&c->font->glyph[g]);
            EF_FREE(c->font->glyph);
            EF_FREE(c->font->adv);
            EF_FREE(c->font->code);
            EF_FREE(c->font);
        }
    }
    EF_FREE(m->chars);
}

void ff_emflash_close(EFMovie *m)
{
    int i;

    if (!m)
        return;
    inst_free(m, &m->root);
    tl_free(&m->root_tl);
    free_chars(m);
    for (i = 0; i < m->npools; i++)
        EF_FREE(m->pools[i].s);
    for (i = 0; i < m->nstrs; i++)
        EF_FREE(m->strs[i]);
    EF_FREE(m->pools);
    EF_FREE(m->strs);
    EF_FREE(m->funcs);
    EF_FREE(m->queue);
    EF_FREE(m->insts);
    EF_FREE(m->exports);
    EF_FREE(m->canvas);
    EF_FREE(m->cov);
    EF_FREE(m->mask);
    EF_FREE(m->maskbuild);
    EF_FREE(m);
}

#ifdef EF_STANDALONE
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    int n = argc > 2 ? atoi(argv[2]) : 1, i;
    long sz;
    uint8_t *buf, *px;
    EFMovie *m;
    int w, h;

    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz)
        return 1;
    fclose(f);
    m = ff_emflash_open(buf, sz, 0, 0);
    if (!m) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    if (argc > 4)
        ff_emflash_set_autoclick(m, atoi(argv[4]));
    ff_emflash_get_size(m, &w, &h);
    fprintf(stderr, "%dx%d, %d frames, %.1f fps\n", w, h, ff_emflash_get_nframes(m), ff_emflash_get_rate(m));
    px = malloc(w * h * 4);
    for (i = 0; i < n; i++) {
        char name[256];
        int y, x;
        FILE *o;
        ff_emflash_render(m, px, w * 4);
        snprintf(name, sizeof(name), "%s_%03d.ppm", argc > 3 ? argv[3] : "out", i);
        o = fopen(name, "wb");
        fprintf(o, "P6 %d %d 255\n", w, h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                fwrite(px + (y * w + x) * 4, 1, 3, o);
        fclose(o);
        ff_emflash_step(m);
    }
    ff_emflash_close(m);
    free(px);
    free(buf);
    return 0;
}
#endif
