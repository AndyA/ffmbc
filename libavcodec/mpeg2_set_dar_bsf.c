/*
 * mpeg2 display aspect ratio bitstream filter
 * Copyright (c) 2007 Baptiste Coudurier
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

/**
 * @file
 * mpeg2 set display aspect ratio
 * modifies bitstream to rewrite display aspect ratio
 */

#include "avcodec.h"
#include "bytestream.h"
#include "mpeg12data.h"

typedef struct {
    int aspect;
} MPEG2SetDarContext;

static int init(AVBitStreamFilterContext *bsfc, const char *args)
{
    MPEG2SetDarContext *ctx = bsfc->priv_data;
    const char *p;
    char *end;
    int i, num = 0, den = 0;

    ctx->aspect = -1;

    p = strpbrk(args, ":x/");
    if (p) {
        num = strtol(args, &end, 10);
        if (end == p)
            den = strtol(end+1, &end, 10);
    }
    if (!p) {
        av_log(NULL, AV_LOG_ERROR, "Incorrect aspect ratio, usage: <num>/<den>\n");
        return -1;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(ff_mpeg2_aspect); i++) {
        if (num == ff_mpeg2_aspect[i].num && den == ff_mpeg2_aspect[i].den) {
            ctx->aspect = i;
            break;
        }
    }

    av_log(NULL, AV_LOG_DEBUG, "aspect %d/%d val %d\n", num, den, ctx->aspect);

    if (ctx->aspect == -1) {
        av_log(NULL, AV_LOG_ERROR, "Unsupported aspect ratio: %d/%d\n", num, den);
        return -1;
    }

    return 0;
}

static int mpeg2_set_dar(AVBitStreamFilterContext *bsfc, AVCodecContext *avctx, const char *args,
                         uint8_t **poutbuf, int *poutbuf_size,
                         const uint8_t *buf, int buf_size, int keyframe)
{
    MPEG2SetDarContext *ctx = bsfc->priv_data;
    char *p;

    if (avctx->codec_id != CODEC_ID_MPEG2VIDEO) {
        av_log(avctx, AV_LOG_ERROR, "seq dump bitstream filter only applies to mpeg2video codec\n");
        return 0;
    }

    if ((AV_RB32(buf) & 0xFFFFFF00) != 0x100) {
        av_log(avctx, AV_LOG_ERROR, "mpeg2 bistream malformated\n");
        return -1;
    }

    if (AV_RB32(buf) != 0x1b3) {
        *poutbuf = buf;
        *poutbuf_size = buf_size;
        return 0;
    }

    p = *poutbuf = av_malloc(buf_size);
    *poutbuf_size = buf_size;
    memcpy(p, buf, buf_size);

    p[7] = (ctx->aspect << 4) | (p[7] & 0xf);

    return 1;
}

AVBitStreamFilter ff_mpeg2_set_dar_bsf = {
    "mpeg2setdar",
    sizeof(MPEG2SetDarContext),
    mpeg2_set_dar,
    .init = init,
};
