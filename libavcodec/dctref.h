/*
 * reference discrete cosine transform (double precision)
 * Copyright (C) 2009 Dylan Yudaken
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

#ifndef AVCODEC_DCTREF_H
#define AVCODEC_DCTREF_H

#include "dsputil.h"

void ff_ref_fdct(DCTELEM *block);
void ff_ref_idct(DCTELEM *block);
void ff_ref_dct_init(void);

#endif
