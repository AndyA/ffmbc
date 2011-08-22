/*
 * IMX remove klv bitstream filter
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
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
 * @file libavcodec/imx_remove_klv_bsf.c
 * imx remove klv bitstream filter
 * remove klv cruft from d-10/imx mpeg-2 bitstream stored in mov
 */

#include "avcodec.h"
#include "bytestream.h"

static int64_t klv_decode_ber_length(const uint8_t **buf)
{
    uint64_t size = bytestream_get_byte(buf);
    if (size & 0x80) { /* long form */
        int bytes_num = size & 0x7f;
        /* SMPTE 379M 5.3.4 guarantee that bytes_num must not exceed 8 bytes */
        if (bytes_num > 8)
            return -1;
        size = 0;
        while (bytes_num--)
            size = size << 8 | bytestream_get_byte(buf);
    }
    return size;
}

static int imx_remove_klv(AVBitStreamFilterContext *bsfc, AVCodecContext *avctx, const char *args,
                          uint8_t **poutbuf, int *poutbuf_size,
                          const uint8_t *buf, int buf_size, int keyframe)
{
    /* MXF essence element key */
    static const uint8_t d10_klv_header[15] = { 0x06,0x0e,0x2b,0x34,0x01,0x02,0x01,0x01,0x0d,0x01,0x03,0x01,0x05,0x01,0x01 };
    const uint8_t *bufp = buf;

    if (avctx->codec_id != CODEC_ID_MPEG2VIDEO) {
        av_log(avctx, AV_LOG_ERROR, "imx bitstream filter only applies to mpeg2video codec\n");
        return 0;
    }

    if (buf_size < 17) {
        av_log(avctx, AV_LOG_ERROR, "wrong packet size\n");
        return -1;
    }

    if (!memcmp(buf, d10_klv_header, 15)) {
        int64_t frame_size;
        bufp += 16;
        if ((frame_size = klv_decode_ber_length(&bufp)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "error decoding klv length\n");
            return -1;
        }
        if (bufp - buf + frame_size > buf_size) {
            av_log(avctx, AV_LOG_ERROR, "wrong frame size\n");
            return -1;
        }
        if (frame_size > INT_MAX)
            return -1;
        *poutbuf = av_malloc(frame_size);
        if (!*poutbuf)
            return AVERROR(ENOMEM);
        memcpy(*poutbuf, bufp, frame_size);
        *poutbuf_size = frame_size;
        return 1;
    } else {
        av_log(avctx, AV_LOG_ERROR, "bitstream does not contain klv packet header\n");
        *poutbuf = NULL;
        *poutbuf_size = 0;
        return 0;
    }
}

AVBitStreamFilter ff_imx_remove_klv_bsf = {
    "imxremoveklv",
    0,
    imx_remove_klv,
};
