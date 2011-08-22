/*
 * AC-3 DSP utils
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

#include "avcodec.h"
#include "ac3dsp.h"

static void ac3_exponent_min_c(uint8_t *exp, int num_reuse_blocks, int nb_coefs)
{
    int blk, i;

    if (!num_reuse_blocks)
        return;

    for (i = 0; i < nb_coefs; i++) {
        uint8_t min_exp = *exp;
        uint8_t *exp1 = exp + 256;
        for (blk = 0; blk < num_reuse_blocks; blk++) {
            uint8_t next_exp = *exp1;
            if (next_exp < min_exp)
                min_exp = next_exp;
            exp1 += 256;
        }
        *exp++ = min_exp;
    }
}

av_cold void ff_ac3dsp_init(AC3DSPContext *c)
{
    c->ac3_exponent_min = ac3_exponent_min_c;

    if (HAVE_MMX)
        ff_ac3dsp_init_x86(c);
}
