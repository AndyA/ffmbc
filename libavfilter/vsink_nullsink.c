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

#include "avfilter.h"

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
}

static void end_frame(AVFilterLink *link)
{
}

AVFilter avfilter_vsink_nullsink = {
    .name        = "nullsink",
    .description = NULL_IF_CONFIG_SMALL("Do absolutely nothing with the input video."),

    .priv_size = 0,

    .inputs    = (AVFilterPad[]) {
        {
            .name            = "default",
            .type            = AVMEDIA_TYPE_VIDEO,
            .start_frame     = start_frame,
            .end_frame       = end_frame,
        },
        { .name = NULL},
    },
    .outputs   = (AVFilterPad[]) {{ .name = NULL }},
};
