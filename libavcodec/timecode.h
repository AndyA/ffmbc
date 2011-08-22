/*
 * Timecode helper functions
 * copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@free.fr>
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

#ifndef AVCODEC_TIMECODE_H
#define AVCODEC_TIMECODE_H

#include "libavutil/rational.h"

/**
 * Adjust frame number for NTSC drop frame time code
 * Adjustment is only valid in NTSC 29.97 and HD 59.94
 * @param frame_num Actual frame number to adjust
 * @return Adjusted frame number
 */
int ff_framenum_to_drop_timecode(int frame_num, int fps);

int ff_framenum_to_timecode(char timecode[32], int frame_num, int drop, int fps);

int ff_timecode_to_framenum(const char *timecode, AVRational tb, int *drop);

#endif /* AVCODEC_TIMECODE_H */
