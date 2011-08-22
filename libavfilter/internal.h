/*
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

#ifndef AVFILTER_INTERNAL_H
#define AVFILTER_INTERNAL_H

/**
 * @file
 * internal API functions
 */

#include "avfilter.h"
#include "avfiltergraph.h"

/**
 * Check for the validity of graph.
 *
 * A graph is considered valid if all its input and output pads are
 * connected.
 *
 * @return 0 in case of success, a negative value otherwise
 */
int ff_avfilter_graph_check_validity(AVFilterGraph *graph);

/**
 * Configure all the links of graphctx.
 *
 * @return 0 in case of success, a negative value otherwise
 */
int ff_avfilter_graph_config_links(AVFilterGraph *graph);

/**
 * Configure the formats of all the links in the graph.
 */
int ff_avfilter_graph_config_formats(AVFilterGraph *graph);

/** default handler for freeing audio/video buffer when there are no references left */
void ff_avfilter_default_free_buffer(AVFilterBuffer *buf);

#endif  /* AVFILTER_INTERNAL_H */
