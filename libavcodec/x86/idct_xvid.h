/*
 * XVID MPEG-4 VIDEO CODEC
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

/*!
 * @file
 * header for Xvid IDCT functions
 */

#ifndef AVCODEC_X86_IDCT_XVID_H
#define AVCODEC_X86_IDCT_XVID_H

#include <stdint.h>

void ff_idct_xvid_mmx(short *block);
void ff_idct_xvid_mmx2(short *block);
void ff_idct_xvid_sse2(short *block);
void ff_idct_xvid_sse2_put(uint8_t *dest, int line_size, short *block);
void ff_idct_xvid_sse2_add(uint8_t *dest, int line_size, short *block);

void ff_idct10_xvid_sse2(short *block);

#endif /* AVCODEC_X86_IDCT_XVID_H */
