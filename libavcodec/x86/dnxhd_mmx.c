/*
 * VC3/DNxHD SIMD functions
 * Copyright (c) 2007 Baptiste Coudurier <baptiste dot coudurier at smartjog dot com>
 *
 * VC-3 encoder funded by the British Broadcasting Corporation
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
#include "libavcodec/dnxhdenc.h"

extern uint16_t inv_zigzag_direct16[64];

static void get_pixels_8x4_sym_sse2(DCTELEM *block, const uint8_t *pixels, int line_size)
{
    __asm__ volatile(
        "pxor %%xmm5,      %%xmm5       \n\t"
        "movq (%0),        %%xmm0       \n\t"
        "add  %2,          %0           \n\t"
        "movq (%0),        %%xmm1       \n\t"
        "movq (%0, %2),    %%xmm2       \n\t"
        "movq (%0, %2,2),  %%xmm3       \n\t"
        "punpcklbw %%xmm5, %%xmm0       \n\t"
        "punpcklbw %%xmm5, %%xmm1       \n\t"
        "punpcklbw %%xmm5, %%xmm2       \n\t"
        "punpcklbw %%xmm5, %%xmm3       \n\t"
        "movdqa %%xmm0,      (%1)       \n\t"
        "movdqa %%xmm1,    16(%1)       \n\t"
        "movdqa %%xmm2,    32(%1)       \n\t"
        "movdqa %%xmm3,    48(%1)       \n\t"
        "movdqa %%xmm3 ,   64(%1)       \n\t"
        "movdqa %%xmm2 ,   80(%1)       \n\t"
        "movdqa %%xmm1 ,   96(%1)       \n\t"
        "movdqa %%xmm0,   112(%1)       \n\t"
        : "+r" (pixels)
        : "r" (block), "r" ((x86_reg)line_size)
    );
}

#if HAVE_SSSE3
#define HAVE_SSSE3_BAK
#endif
#undef HAVE_SSSE3
#define HAVE_SSSE3 0

#undef HAVE_SSE2
#undef HAVE_MMX2
#define HAVE_SSE2 0
#define HAVE_MMX2 0
#define RENAME(a) a ## _MMX
#define RENAMEl(a) a ## _mmx
#include "dnxhd_mmx_template.c"

#undef HAVE_MMX2
#define HAVE_MMX2 1
#undef RENAME
#undef RENAMEl
#define RENAME(a) a ## _MMX2
#define RENAMEl(a) a ## _mmx2
#include "dnxhd_mmx_template.c"

#undef HAVE_SSE2
#define HAVE_SSE2 1
#undef RENAME
#undef RENAMEl
#define RENAME(a) a ## _SSE2
#define RENAMEl(a) a ## _sse2
#include "dnxhd_mmx_template.c"

#ifdef HAVE_SSSE3_BAK
#undef HAVE_SSSE3
#define HAVE_SSSE3 1
#undef RENAME
#undef RENAMEl
#define RENAME(a) a ## _SSSE3
#define RENAMEl(a) a ## _sse2
#include "dnxhd_mmx_template.c"
#endif

void ff_dnxhd_init_mmx(DNXHDEncContext *ctx)
{
    int mm_flags = av_get_cpu_flags();
    const int dct_algo = ctx->avctx->dct_algo;

    if (mm_flags & AV_CPU_FLAG_SSE2)
        ctx->get_pixels_8x4_sym = get_pixels_8x4_sym_sse2;

    if (dct_algo == FF_DCT_AUTO || dct_algo == FF_DCT_MMX) {
#if HAVE_SSSE3
        if (mm_flags & AV_CPU_FLAG_SSSE3) {
            ctx->dct_quantize = dct_quantize_SSSE3;
        } else
#endif
            if (mm_flags & AV_CPU_FLAG_SSE2) {
                ctx->dct_quantize = dct_quantize_SSE2;
            } else if(mm_flags & AV_CPU_FLAG_MMX2){
                ctx->dct_quantize = dct_quantize_MMX2;
            } else {
                ctx->dct_quantize = dct_quantize_MMX;
            }
    }
}
