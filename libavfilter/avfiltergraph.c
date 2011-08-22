/*
 * filter graphs
 * Copyright (c) 2008 Vitor Sessak
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

#include <ctype.h>
#include <string.h>

#include "avfilter.h"
#include "avfiltergraph.h"
#include "internal.h"

static const char* context_to_name(void *ptr)
{
    return "graph";
}

static const AVClass class = { "AVFilterGraph", context_to_name, NULL,
                               LIBAVFILTER_VERSION_INT,
                               offsetof(AVFilterGraph, log_level_offset) };

AVFilterGraph *avfilter_graph_alloc(void)
{
    AVFilterGraph *graph = av_mallocz(sizeof(AVFilterGraph));
    if (!graph)
        return NULL;
    graph->av_class = &class;
    return graph;
}

void avfilter_graph_free(AVFilterGraph **graph)
{
    if (!*graph)
        return;
    for (; (*graph)->filter_count > 0; (*graph)->filter_count--)
        avfilter_free((*graph)->filters[(*graph)->filter_count - 1]);
    av_freep(&(*graph)->scale_sws_opts);
    av_freep(&(*graph)->filters);
    av_freep(graph);
}

int avfilter_graph_add_filter(AVFilterGraph *graph, AVFilterContext *filter)
{
    AVFilterContext **filters = av_realloc(graph->filters,
                                           sizeof(AVFilterContext*) * (graph->filter_count+1));
    if (!filters)
        return AVERROR(ENOMEM);

    graph->filters = filters;
    graph->filters[graph->filter_count++] = filter;

    return 0;
}

int avfilter_graph_create_filter(AVFilterContext **filt_ctx, AVFilter *filt,
                                 const char *name, const char *args, void *opaque,
                                 AVFilterGraph *graph_ctx)
{
    int ret;

    if ((ret = avfilter_open(filt_ctx, filt, name)) < 0)
        goto fail;
    if ((ret = avfilter_init_filter(*filt_ctx, args, opaque)) < 0)
        goto fail;
    if ((ret = avfilter_graph_add_filter(graph_ctx, *filt_ctx)) < 0)
        goto fail;
    return 0;

fail:
    if (*filt_ctx)
        avfilter_free(*filt_ctx);
    *filt_ctx = NULL;
    return ret;
}

int ff_avfilter_graph_check_validity(AVFilterGraph *graph)
{
    AVFilterContext *filt;
    int i, j;

    for (i = 0; i < graph->filter_count; i++) {
        filt = graph->filters[i];
        if (!filt)
            continue;
        for (j = 0; j < filt->input_count; j++) {
            if (!filt->inputs[j] || !filt->inputs[j]->src) {
                av_log(graph, AV_LOG_ERROR,
                       "Input pad \"%s\" for the filter \"%s\" of type \"%s\" not connected to any source\n",
                       filt->input_pads[j].name, filt->name, filt->filter->name);
                return -1;
            }
        }

        for (j = 0; j < filt->output_count; j++) {
            if (!filt->outputs[j] || !filt->outputs[j]->dst) {
                av_log(graph, AV_LOG_ERROR,
                       "Output pad \"%s\" for the filter \"%s\" of type \"%s\" not connected to any destination\n",
                       filt->output_pads[j].name, filt->name, filt->filter->name);
                return -1;
            }
        }
    }

    return 0;
}

int ff_avfilter_graph_config_links(AVFilterGraph *graph)
{
    AVFilterContext *filt;
    int i, ret;

    for (i=0; i < graph->filter_count; i++) {
        filt = graph->filters[i];
        if (!filt)
            continue;
        if (!filt->output_count) {
            if ((ret = avfilter_config_links(filt)))
                return ret;
        }
    }

    return 0;
}

AVFilterContext *avfilter_graph_get_filter(AVFilterGraph *graph, char *name)
{
    int i;

    for (i = 0; i < graph->filter_count; i++)
        if (!graph->filters[i])
            continue;
        if (graph->filters[i]->name && !strcmp(name, graph->filters[i]->name))
            return graph->filters[i];

    return NULL;
}

static int query_formats(AVFilterGraph *graph)
{
    int i, j, ret;
    int scaler_count = 0;
    char inst_name[30];

    /* ask all the sub-filters for their supported media formats */
    for (i = 0; i < graph->filter_count; i++) {
        if (!graph->filters[i])
            continue;
        if (graph->filters[i]->filter->query_formats)
            graph->filters[i]->filter->query_formats(graph->filters[i]);
        else
            avfilter_default_query_formats(graph->filters[i]);
    }

    /* go through and merge as many format lists as possible */
    for (i = 0; i < graph->filter_count; i++) {
        AVFilterContext *filter = graph->filters[i];
        if (!filter)
            continue;
        for (j = 0; j < filter->input_count; j++) {
            AVFilterLink *link = filter->inputs[j];
            if (link && link->in_formats != link->out_formats) {
                if (!avfilter_merge_formats(link->in_formats,
                                            link->out_formats)) {
                    AVFilterContext *scale;
                    char scale_args[256];
                    /* couldn't merge format lists. auto-insert scale filter */
                    snprintf(inst_name, sizeof(inst_name), "auto-inserted scaler %d",
                             scaler_count++);
                    snprintf(scale_args, sizeof(scale_args), "0:0:%s", graph->scale_sws_opts);
                    if ((ret = avfilter_graph_create_filter(&scale, avfilter_get_by_name("scale"),
                                                            inst_name, scale_args, NULL, graph)) < 0)
                        return ret;
                    if ((ret = avfilter_insert_filter(link, scale, 0, 0)) < 0)
                        return ret;

                    scale->filter->query_formats(scale);
                    if (((link = scale-> inputs[0]) &&
                         !avfilter_merge_formats(link->in_formats, link->out_formats)) ||
                        ((link = scale->outputs[0]) &&
                         !avfilter_merge_formats(link->in_formats, link->out_formats))) {
                        av_log(graph, AV_LOG_ERROR,
                               "Impossible to convert between the formats supported by the filter "
                               "'%s' and the filter '%s'\n", link->src->name, link->dst->name);
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}

static void pick_format(AVFilterLink *link)
{
    if (!link || !link->in_formats)
        return;

    link->in_formats->format_count = 1;
    link->format = link->in_formats->formats[0];

    avfilter_formats_unref(&link->in_formats);
    avfilter_formats_unref(&link->out_formats);
}

static void pick_formats(AVFilterGraph *graph)
{
    int i, j;

    for (i = 0; i < graph->filter_count; i++) {
        AVFilterContext *filter = graph->filters[i];
        if (!filter)
            continue;
        for (j = 0; j < filter->input_count; j++)
            pick_format(filter->inputs[j]);
        for (j = 0; j < filter->output_count; j++)
            pick_format(filter->outputs[j]);
    }
}

int ff_avfilter_graph_config_formats(AVFilterGraph *graph)
{
    /* find supported formats from sub-filters, and merge along links */
    if (query_formats(graph))
        return -1;

    /* Once everything is merged, it's possible that we'll still have
     * multiple valid media format choices. We pick the first one. */
    pick_formats(graph);

    return 0;
}

int avfilter_graph_config(AVFilterGraph *graph)
{
    int ret;

    if ((ret = ff_avfilter_graph_check_validity(graph)))
        return ret;
    if ((ret = ff_avfilter_graph_config_formats(graph)))
        return ret;
    if ((ret = ff_avfilter_graph_config_links(graph)))
        return ret;

    return 0;
}
