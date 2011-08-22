/*
 * imx dump header bitstream filter
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
 * imx dump header bitstream filter
 * modifies bitstream to fit in mov and be decoded by final cut pro decoder
 */

#include "avcodec.h"
#include "bytestream.h"

typedef struct {
    uint8_t *seq_data;
    unsigned seq_len;
} MPEG2DumpSeqContext;

static int mpeg2_dump_seq(AVBitStreamFilterContext *bsfc, AVCodecContext *avctx, const char *args,
                          uint8_t **poutbuf, int *poutbuf_size,
                          const uint8_t *buf, int buf_size, int keyframe)
{
    MPEG2DumpSeqContext *ctx = bsfc->priv_data;
    int i, c = 0, pict_type = 0;
    int copy_seq = 0;
    uint8_t *poutbufp;

    if (avctx->codec_id != CODEC_ID_MPEG2VIDEO) {
        av_log(avctx, AV_LOG_ERROR, "seq dump bitstream filter only applies to mpeg2video codec\n");
        return 0;
    }

    if ((AV_RB32(buf) & 0xFFFFFF00) != 0x100) {
        av_log(avctx, AV_LOG_ERROR, "mpeg2 bistream malformated\n");
        return -1;
    }

    if (AV_RB32(buf) == 0x1b3)
        copy_seq = 1;

    for (i = 0; i < buf_size - 4; i++) {
        c = (c<<8) + buf[i];
        if (copy_seq && c == 0x1b8) {
            av_freep(&ctx->seq_data);
            ctx->seq_len = i - 3;
            ctx->seq_data = av_malloc(ctx->seq_len);
            if (!ctx->seq_data)
                return AVERROR(ENOMEM);
            memcpy(ctx->seq_data, buf, ctx->seq_len);
            copy_seq = 0;
        }
        if (c == 0x100) {
            pict_type = (buf[i+2]>>3) & 0x07;
            break;
        }
    }

    if (!pict_type) {
        av_log(avctx, AV_LOG_ERROR, "could not get pict type\n");
        return -1;
    }

    if (AV_RB32(buf) == 0x1b3 || pict_type != 1) {
        *poutbuf = buf;
        *poutbuf_size = buf_size;
        return 0;
    }

    if (!ctx->seq_len) {
        av_log(avctx, AV_LOG_ERROR, "could not extract sequence header\n");
        return -1;
    }

    *poutbuf = av_malloc(buf_size + ctx->seq_len + FF_INPUT_BUFFER_PADDING_SIZE);
    poutbufp = *poutbuf;
    bytestream_put_buffer(&poutbufp, ctx->seq_data, ctx->seq_len);
    bytestream_put_buffer(&poutbufp, buf, buf_size);
    *poutbuf_size = poutbufp - *poutbuf;
    return 1;
}

static void close(AVBitStreamFilterContext *bsfc)
{
    MPEG2DumpSeqContext *ctx = bsfc->priv_data;

    av_freep(&ctx->seq_data);
    ctx->seq_len = 0;
}

AVBitStreamFilter ff_mpeg2_dump_seq_bsf = {
    "mpeg2seqdump",
    sizeof(MPEG2DumpSeqContext),
    mpeg2_dump_seq,
    close,
};
