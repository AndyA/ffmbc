/*
 * Copyright (c) 2007 Bobby Bingham
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation;
 * version 2 of the License.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * scale video filter
 */

#include "avfilter.h"
#include "libavutil/eval.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"

static const char *var_names[] = {
    "w",    ///< width of the main video
    "h",    ///< height of the main video
    NULL
};

enum var_name {
    W,
    H,
    VARS_NB,
};

typedef struct {
    SwsContext *sws;     ///< software scaler context
    SwsContext *sws2;    ///< software scaler context 2 for interlaced

    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     */
    int w, h;
    unsigned int flags;         ///sws flags

    int hsub, vsub;             ///< chroma subsampling
    int slice_y;                ///< top of current output slice
    int input_is_pal;           ///< set to 1 if the input format is paletted
    char w_expr[255], h_expr[255];
    int interlaced;
    char *colorspace;
} ScaleContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ScaleContext *scale = ctx->priv;
    const char *p;

    scale->flags = SWS_BILINEAR;
    if (args) {
        sscanf(args, "%255[^:]:%255[^:]:%d", scale->w_expr, scale->h_expr, &scale->interlaced);
        p= strstr(args,"flags=");
        if(p) scale->flags= strtoul(p+6, NULL, 0);
        p = strstr(args,"cs=");
        if (p) {
            const char *e = strchr(p+3, ':');
            if (!e) {
                scale->colorspace = av_strdup(p);
            } else {
                scale->colorspace = av_malloc(e - (p+3) + 1);
                av_strlcpy(scale->colorspace, p+3, e - (p+3) + 1);
            }
        }
    }

    scale->interlaced = !!scale->interlaced;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ScaleContext *scale = ctx->priv;
    sws_freeContext(scale->sws);
    sws_freeContext(scale->sws2);
    scale->sws = NULL;
    scale->sws2 = NULL;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    enum PixelFormat pix_fmt;
    int ret;

    if (ctx->inputs[0]) {
        formats = NULL;
        for (pix_fmt = 0; pix_fmt < PIX_FMT_NB; pix_fmt++)
            if (   sws_isSupportedInput(pix_fmt)
                && (ret = avfilter_add_format(&formats, pix_fmt)) < 0) {
                avfilter_formats_unref(&formats);
                return ret;
            }
        avfilter_formats_ref(formats, &ctx->inputs[0]->out_formats);
    }
    if (ctx->outputs[0]) {
        formats = NULL;
        for (pix_fmt = 0; pix_fmt < PIX_FMT_NB; pix_fmt++)
            if (    sws_isSupportedOutput(pix_fmt)
                && (ret = avfilter_add_format(&formats, pix_fmt)) < 0) {
                avfilter_formats_unref(&formats);
                return ret;
            }
        avfilter_formats_ref(formats, &ctx->outputs[0]->in_formats);
    }

    return 0;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    ScaleContext *scale = ctx->priv;
    int64_t w, h;
    const char *expr;
    double var_values[VARS_NB], ret;

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    var_values[W]    = ctx->inputs[0]->w;
    var_values[H]    = ctx->inputs[0]->h;

    if (av_expr_parse_and_eval(&ret, (expr = scale->w_expr),
                               var_names, var_values,
                               NULL, NULL, NULL, NULL,
                               NULL, 0, ctx) < 0)
        return -1;
    scale->w = ret;

    if (av_expr_parse_and_eval(&ret, (expr = scale->h_expr),
                               var_names, var_values,
                               NULL, NULL, NULL, NULL,
                               NULL, 0, ctx) < 0)
        return -1;
    scale->h = ret;

    /* sanity check params */
    if (scale->w < -1 || scale->h < -1) {
        av_log(ctx, AV_LOG_ERROR, "Size values less than -1 are not acceptable.\n");
        return -1;
    }
    if (scale->w == -1 && scale->h == -1)
        scale->w = scale->h = 0;

    if (!(w = scale->w))
        w = inlink->w;
    if (!(h = scale->h))
        h = inlink->h;
    if (w == -1)
        w = av_rescale(h, inlink->w, inlink->h);
    if (h == -1)
        h = av_rescale(w, inlink->h, inlink->w);

    if (w > INT_MAX || h > INT_MAX ||
        (h * inlink->w) > INT_MAX  ||
        (w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = w;
    outlink->h = h;

    av_reduce(&outlink->sample_aspect_ratio.num,
              &outlink->sample_aspect_ratio.den,
              (int64_t)inlink->sample_aspect_ratio.num * outlink->h * inlink->w,
              (int64_t)inlink->sample_aspect_ratio.den * outlink->w * inlink->h,
              INT_MAX);

    /* TODO: make algorithm configurable */
    av_log(ctx, AV_LOG_INFO, "w:%d h:%d fmt:%s -> w:%d h:%d fmt:%s flags:0x%0x interlaced:%d\n",
           inlink ->w, inlink ->h, av_pix_fmt_descriptors[ inlink->format].name,
           outlink->w, outlink->h, av_pix_fmt_descriptors[outlink->format].name,
           scale->flags, scale->interlaced);

    scale->input_is_pal = av_pix_fmt_descriptors[inlink->format].flags & PIX_FMT_PAL;

    scale->sws = sws_alloc_context();
    if (scale->sws == NULL) {
        av_log(ctx, AV_LOG_INFO, "Cannot get resampling context\n");
        return -1;
    }
    av_set_int(scale->sws, "srcw", inlink->w);
    av_set_int(scale->sws, "srch", inlink->h >> scale->interlaced);
    av_set_int(scale->sws, "src_format", inlink->format);
    av_set_int(scale->sws, "dstw", outlink->w);
    av_set_int(scale->sws, "dsth", outlink->h >> scale->interlaced);
    av_set_int(scale->sws, "dst_format", outlink->format);
    av_set_int(scale->sws, "sws_flags", scale->flags);
    if (scale->colorspace)
        av_set_string3(scale->sws, "colorspace", scale->colorspace, 0, NULL);
    if (sws_init_context(scale->sws, NULL, NULL) < 0)
        return -1;

    if (scale->interlaced) {
        scale->sws2 = sws_alloc_context();
        if (scale->sws2 == NULL) {
            av_log(ctx, AV_LOG_INFO, "Cannot get resampling context\n");
            return -1;
        }
        av_set_int(scale->sws2, "srcw", inlink->w);
        av_set_int(scale->sws2, "srch", inlink->h >> scale->interlaced);
        av_set_int(scale->sws2, "src_format", inlink->format);
        av_set_int(scale->sws2, "dstw", outlink->w);
        av_set_int(scale->sws2, "dsth", outlink->h >> scale->interlaced);
        av_set_int(scale->sws2, "dst_format", outlink->format);
        av_set_int(scale->sws2, "sws_flags", scale->flags);
        if (scale->colorspace)
            av_set_string3(scale->sws2, "colorspace", scale->colorspace, 0, NULL);
        if (sws_init_context(scale->sws2, NULL, NULL) < 0)
            return -1;
    }

    return 0;
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    ScaleContext *scale = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFilterBufferRef *outpicref;

    scale->hsub = av_pix_fmt_descriptors[link->format].log2_chroma_w;
    scale->vsub = av_pix_fmt_descriptors[link->format].log2_chroma_h;

    outpicref = avfilter_get_video_buffer(outlink, AV_PERM_WRITE, outlink->w, outlink->h);
    avfilter_copy_buffer_ref_props(outpicref, picref);
    outpicref->video->w = outlink->w;
    outpicref->video->h = outlink->h;

    outlink->out_buf = outpicref;

    scale->slice_y = 0;
    avfilter_start_frame(outlink, avfilter_ref_buffer(outpicref, ~0));
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    ScaleContext *scale = link->dst->priv;
    int out_h;
    AVFilterBufferRef *cur_pic = link->cur_buf;
    const uint8_t *data[4];

    if (scale->slice_y == 0 && slice_dir == -1)
        scale->slice_y = link->dst->outputs[0]->h;

    if (scale->interlaced) {
        int src_stride[4];
        int dst_stride[4];
        uint8_t *dst[4];
        int i;

        data[0] = cur_pic->data[0] +  y               * cur_pic->linesize[0];
        data[1] = scale->input_is_pal ?
            cur_pic->data[1] :
            cur_pic->data[1] + (y>>scale->vsub) * cur_pic->linesize[1];
        data[2] = cur_pic->data[2] + (y>>scale->vsub) * cur_pic->linesize[2];
        data[3] = cur_pic->data[3] +  y               * cur_pic->linesize[3];

        for (i = 0; i < 4; i++) {
            src_stride[i] = cur_pic->linesize[i] << 1;
            dst_stride[i] = link->dst->outputs[0]->out_buf->linesize[i] << 1;
            dst       [i] = link->dst->outputs[0]->out_buf->data[i];
        }

        sws_scale(scale->sws, data, src_stride, y>>1, h>>1, dst, dst_stride);
        for (i = 0; i < 4; i++) {
            data[i] += cur_pic->linesize[i];
            dst [i] += link->dst->outputs[0]->out_buf->linesize[i];
        }
        out_h = sws_scale(scale->sws2, data, src_stride, y>>1, h>>1, dst, dst_stride);
        out_h <<= 1;
    } else {
        data[0] = cur_pic->data[0] +  y               * cur_pic->linesize[0];
        data[1] = scale->input_is_pal ?
            cur_pic->data[1] :
            cur_pic->data[1] + (y>>scale->vsub) * cur_pic->linesize[1];
        data[2] = cur_pic->data[2] + (y>>scale->vsub) * cur_pic->linesize[2];
        data[3] = cur_pic->data[3] +  y               * cur_pic->linesize[3];

        out_h = sws_scale(scale->sws, data, cur_pic->linesize, y, h,
                          link->dst->outputs[0]->out_buf->data,
                          link->dst->outputs[0]->out_buf->linesize);
    }

    if (slice_dir == -1)
        scale->slice_y -= out_h;
    avfilter_draw_slice(link->dst->outputs[0], scale->slice_y, out_h, slice_dir);
    if (slice_dir == 1)
        scale->slice_y += out_h;
}

AVFilter avfilter_vf_scale = {
    .name      = "scale",
    .description = NULL_IF_CONFIG_SMALL("Scale the input video to width:height size and/or convert the image format."),

    .init      = init,
    .uninit    = uninit,

    .query_formats = query_formats,

    .priv_size = sizeof(ScaleContext),

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .start_frame      = start_frame,
                                    .draw_slice       = draw_slice,
                                    .min_perms        = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .config_props     = config_props, },
                                  { .name = NULL}},
};
