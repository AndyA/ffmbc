/*
 * x86-optimized AC-3 DSP utils
 * Copyright (c) 2011 Justin Ruggles
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

#include "libavutil/x86_cpu.h"
#include "dsputil_mmx.h"
#include "libavcodec/ac3dsp.h"

extern void ff_ac3_exponent_min_mmx   (uint8_t *exp, int num_reuse_blocks, int nb_coefs);
extern void ff_ac3_exponent_min_mmxext(uint8_t *exp, int num_reuse_blocks, int nb_coefs);
extern void ff_ac3_exponent_min_sse2  (uint8_t *exp, int num_reuse_blocks, int nb_coefs);

av_cold void ff_ac3dsp_init_x86(AC3DSPContext *c)
{
    int mm_flags = av_get_cpu_flags();

#if HAVE_YASM
    if (mm_flags & AV_CPU_FLAG_MMX) {
        c->ac3_exponent_min = ff_ac3_exponent_min_mmx;
    }
    if (mm_flags & AV_CPU_FLAG_MMX2 && HAVE_MMX2) {
        c->ac3_exponent_min = ff_ac3_exponent_min_mmxext;
    }
    if (mm_flags & AV_CPU_FLAG_SSE2 && HAVE_SSE) {
        c->ac3_exponent_min = ff_ac3_exponent_min_sse2;
    }
#endif
}
