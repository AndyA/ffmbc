/*
 * Copyright (C) 2003 Michael Zucchi <notzed@ximian.com>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "avfilter.h"

typedef struct {
    int mode;
    int frames;
    int hsub, vsub;
    uint8_t *black[3];
    AVFilterBufferRef *cur;
    AVFilterBufferRef *next;
} TInterlaceContext ;

static void end_frame(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    TInterlaceContext *tinterlace = ctx->priv;
    AVFilterBufferRef *cur = tinterlace->cur;
    AVFilterBufferRef *next = tinterlace->next;
    AVFilterBufferRef *out = NULL;

    if (!tinterlace->cur)
        return;

    switch (tinterlace->mode) {
    case 0:
        out = avfilter_get_video_buffer(ctx->outputs[0], AV_PERM_WRITE | AV_PERM_PRESERVE |
                                        AV_PERM_REUSE, link->w, link->h*2);
        avfilter_copy_buffer_ref_props(out, cur);
        out->video->interlaced = 1;

        av_image_copy_plane(out->data[0], out->linesize[0]*2,
                            cur->data[0], cur->linesize[0],
                            link->w, link->h);
        av_image_copy_plane(out->data[1], out->linesize[1]*2,
                            cur->data[1], cur->linesize[1],
                            link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
        av_image_copy_plane(out->data[2], out->linesize[2]*2,
                            cur->data[2], cur->linesize[2],
                            link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);

        av_image_copy_plane(out->data[0]+out->linesize[0],
                            out->linesize[0]*2,
                            next->data[0], next->linesize[0],
                            link->w, link->h);
        av_image_copy_plane(out->data[1]+out->linesize[1],
                            out->linesize[1]*2,
                            next->data[1], next->linesize[1],
                            link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
        av_image_copy_plane(out->data[2]+out->linesize[2],
                            out->linesize[2]*2,
                            next->data[2], next->linesize[2],
                            link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);

        avfilter_unref_buffer(tinterlace->next);
        tinterlace->next = NULL;
        break;
    case 1:
        out = avfilter_ref_buffer(cur, AV_PERM_READ);
        avfilter_unref_buffer(tinterlace->next);
        tinterlace->next = NULL;
        break;
    case 2:
        out = avfilter_ref_buffer(next, AV_PERM_READ);
        avfilter_unref_buffer(tinterlace->next);
        tinterlace->next = NULL;
        break;
    case 3:
        out = avfilter_get_video_buffer(ctx->outputs[0], AV_PERM_WRITE | AV_PERM_PRESERVE |
                                                    AV_PERM_REUSE, link->w, link->h*2);
        avfilter_copy_buffer_ref_props(out, cur);
        out->video->interlaced = 1;

        if (tinterlace->frames & 1) {
            av_image_copy_plane(out->data[0], out->linesize[0]*2,
                                tinterlace->black[0], 0,
                                link->w, link->h);
            av_image_copy_plane(out->data[1], out->linesize[1]*2,
                                tinterlace->black[1], 0,
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
            av_image_copy_plane(out->data[2], out->linesize[2]*2,
                                tinterlace->black[2], 0,
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);

            av_image_copy_plane(out->data[0]+out->linesize[0], out->linesize[0]*2,
                                cur->data[0], next->linesize[0],
                                link->w, link->h);
            av_image_copy_plane(out->data[1]+out->linesize[1], out->linesize[1]*2,
                                cur->data[1], next->linesize[1],
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
            av_image_copy_plane(out->data[2]+out->linesize[2],
                                out->linesize[2]*2,
                                cur->data[2], next->linesize[2],
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
        } else {
            av_image_copy_plane(out->data[0]+out->linesize[0], out->linesize[0]*2,
                                tinterlace->black[0], 0,
                                link->w, link->h);
            av_image_copy_plane(out->data[1]+out->linesize[1], out->linesize[1]*2,
                                tinterlace->black[1], 0,
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
            av_image_copy_plane(out->data[2]+out->linesize[2], out->linesize[2]*2,
                                tinterlace->black[2], 0,
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);

            av_image_copy_plane(out->data[0], out->linesize[0]*2,
                                cur->data[0], cur->linesize[0],
                                link->w, link->h);
            av_image_copy_plane(out->data[1], out->linesize[1]*2,
                                cur->data[1], cur->linesize[1],
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
            av_image_copy_plane(out->data[2], out->linesize[2]*2,
                                cur->data[2], cur->linesize[2],
                                link->w>>tinterlace->hsub, link->h>>tinterlace->vsub);
        }
        break;
    case 4:
        // Interleave even lines (only) from Frame 'i' with odd
        // lines (only) from Frame 'i+1', halving the Frame
        // rate and preserving image height.

        out = avfilter_get_video_buffer(ctx->outputs[0], AV_PERM_WRITE | AV_PERM_PRESERVE |
                                                    AV_PERM_REUSE, link->w, link->h);
        avfilter_copy_buffer_ref_props(out, cur);
        out->video->interlaced = 1;

        av_image_copy_plane(out->data[0], out->linesize[0]*2,
                            cur->data[0], cur->linesize[0]*2,
                            link->w, link->h>>1);
        av_image_copy_plane(out->data[1], out->linesize[1]*2,
                            cur->data[1], cur->linesize[1]*2,
                            link->w>>tinterlace->hsub, (link->h>>1)>>tinterlace->vsub);
        av_image_copy_plane(out->data[2], out->linesize[2]*2,
                            cur->data[2], cur->linesize[2]*2,
                            link->w>>tinterlace->hsub, (link->h>>1)>>tinterlace->vsub);

        av_image_copy_plane(out->data[0]+out->linesize[0], out->linesize[0]*2,
                            next->data[0]+next->linesize[0], next->linesize[0]*2,
                            link->w, link->h>>1);
        av_image_copy_plane(out->data[1]+out->linesize[1], out->linesize[1]*2,
                            next->data[1]+next->linesize[1], next->linesize[1]*2,
                            link->w>>tinterlace->hsub, (link->h>>1)>>tinterlace->vsub);
        av_image_copy_plane(out->data[2]+out->linesize[2], out->linesize[2]*2,
                            next->data[2]+next->linesize[2], next->linesize[2]*2,
                            link->w>>tinterlace->hsub, (link->h>>1)>>tinterlace->vsub);

        avfilter_unref_buffer(tinterlace->next);
        tinterlace->next = NULL;
        break;
    }

    avfilter_start_frame(ctx->outputs[0], out);
    avfilter_draw_slice(ctx->outputs[0], 0, link->dst->outputs[0]->h, 1);
    avfilter_end_frame(ctx->outputs[0]);

    tinterlace->frames++;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    TInterlaceContext *tinterlace = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[inlink->format];

    tinterlace->hsub = pix_desc->log2_chroma_w;
    tinterlace->vsub = pix_desc->log2_chroma_h;

    return 0;
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    AVFilterContext *ctx = link->dst;
    TInterlaceContext *tinterlace = ctx->priv;

    if (tinterlace->cur)
        avfilter_unref_buffer(tinterlace->cur);
    tinterlace->cur  = tinterlace->next;
    tinterlace->next = picref;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV420P,
        PIX_FMT_YUV422P,
        PIX_FMT_YUV444P,
        PIX_FMT_YUVJ420P,
        PIX_FMT_YUVJ422P,
        PIX_FMT_YUVJ444P,
        PIX_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    TInterlaceContext *tinterlace = outlink->src->priv;

    switch (tinterlace->mode) {
    case 3:
        tinterlace->black[0] = av_mallocz(outlink->src->inputs[0]->w);
        tinterlace->black[1] = av_malloc(outlink->src->inputs[0]->w);
        tinterlace->black[2] = av_malloc(outlink->src->inputs[0]->w);
        memset(tinterlace->black[1], 128, outlink->src->inputs[0]->w);
        memset(tinterlace->black[2], 128, outlink->src->inputs[0]->w);
        // fall
    case 0:
        outlink->h = outlink->src->inputs[0]->h*2;
        break;
    case 1:            /* odd frames */
    case 2:            /* even frames */
    case 4:            /* alternate frame (height-preserving) interlacing */
        outlink->h = outlink->src->inputs[0]->h;
        break;
    }

    outlink->w = outlink->src->inputs[0]->w;

    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    TInterlaceContext *tinterlace = ctx->priv;

    tinterlace->mode = 0;

    if (args) sscanf(args, "%d", &tinterlace->mode);

    if (tinterlace->mode > 5) {
        av_log(ctx, AV_LOG_ERROR, "invalid mode\n");
        return -1;
    }

    av_log(ctx, AV_LOG_INFO, "mode:%d\n", tinterlace->mode);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TInterlaceContext *tinterlace = ctx->priv;

    if (tinterlace->cur ) avfilter_unref_buffer(tinterlace->cur );
    if (tinterlace->next) avfilter_unref_buffer(tinterlace->next);
}

static int poll_frame(AVFilterLink *link, int flush)
{
    TInterlaceContext *tinterlace = link->src->priv;
    int ret, val;

    val = avfilter_poll_frame(link->src->inputs[0], flush);

    if (val==1 && !tinterlace->next) { //FIXME change API to not requre this red tape
        if ((ret = avfilter_request_frame(link->src->inputs[0])) < 0)
            return ret;
        val = avfilter_poll_frame(link->src->inputs[0], flush);
    }
    assert(tinterlace->next);

    return val;
}

static void null_draw_slice(AVFilterLink *link, int y, int h, int slice_dir) { }

AVFilter avfilter_vf_tinterlace = {
    .name          = "tinterlace",
    .description   = NULL_IF_CONFIG_SMALL("Temporal field interlacing"),

    .priv_size     = sizeof(TInterlaceContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .config_props     = config_input,
                                    .start_frame      = start_frame,
                                    .draw_slice       = null_draw_slice,
                                    .end_frame        = end_frame, },
                                  { .name = NULL}},

    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .config_props     = config_output,
                                    .poll_frame       = poll_frame, },
                                  { .name = NULL}},
};
