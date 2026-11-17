/*
 * Copyright (c) 2026 quatric
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
 * Planar RGB to the YCgCo planes MobiClip DS video carries.
 *
 * The planes stay in the GBRP layout, so mergeplanes can relabel them as
 * YUV afterwards: G takes Y = (R + 2G + B) / 4, B takes
 * Cg = (2G - R - B) / 4 + 128 and R takes Co = (R - B) / 2 + 128, each
 * rounded down.  That is what
 *
 *   geq=g='(r(X,Y)+2*g(X,Y)+b(X,Y))/4':b='(2*g(X,Y)-r(X,Y)-b(X,Y))/4+128':r='(r(X,Y)-b(X,Y))/2+128'
 *
 * writes, sample for sample, without evaluating an expression per sample.
 */

#include "libavutil/frame.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

static int ycgco_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    const AVFrame *in = td->in;
    AVFrame *out = td->out;
    const int w = in->width, h = in->height;
    const int start = (h * jobnr) / nb_jobs, end = (h * (jobnr + 1)) / nb_jobs;

    for (int y = start; y < end; y++) {
        const uint8_t *sg = in->data[0] + y * in->linesize[0];
        const uint8_t *sb = in->data[1] + y * in->linesize[1];
        const uint8_t *sr = in->data[2] + y * in->linesize[2];
        uint8_t *dy  = out->data[0] + y * out->linesize[0];
        uint8_t *dcg = out->data[1] + y * out->linesize[1];
        uint8_t *dco = out->data[2] + y * out->linesize[2];

        for (int x = 0; x < w; x++) {
            const int r = sr[x], g = sg[x], b = sb[x];

            dy[x]  = (r + 2 * g + b) >> 2;
            dcg[x] = (2 * g - r - b + 512) >> 2;
            dco[x] = (r - b + 256) >> 1;
        }
    }
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    ThreadData td;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    td.in  = in;
    td.out = out;
    ff_filter_execute(ctx, ycgco_slice, &td, NULL,
                      FFMIN(outlink->h, ff_filter_get_nb_threads(ctx)));

    if (out != in)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad ycgco_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_ycgco = {
    .p.name        = "ycgco",
    .p.description = NULL_IF_CONFIG_SMALL("Convert planar RGB to MobiClip DS YCgCo planes, kept in GBRP order."),
    .p.flags       = AVFILTER_FLAG_SLICE_THREADS,
    FILTER_INPUTS(ycgco_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_GBRP),
};
