/*
 * DNxHD/VC-3 parser
 * Copyright (c) 2008 Baptiste Coudurier <baptiste.coudurier@free.fr>
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
 * DNxHD/VC-3 parser
 */

#include "parser.h"

#define DNXHD_HEADER_PREFIX     0x000002800100

static int dnxhd_find_frame_end(ParseContext *pc,
                                const uint8_t *buf, int buf_size)
{
    uint64_t state = pc->state64;
    int pic_found = pc->frame_start_found;
    int i = 0;

    if (!pic_found) {
        for (i = 0; i < buf_size; i++) {
            state = (state<<8) | buf[i];
            if ((state & 0xffffffffff00LL) == DNXHD_HEADER_PREFIX &&
                (state & 0x000000000003LL) < 3) { /* coding unit is progressive frame or field 1 */
                i++;
                pic_found = 1;
                break;
            }
        }
    }

    if (pic_found) {
        if (!buf_size) /* EOF considered as end of frame */
            return 0;
        for (; i < buf_size; i++) {
            state = (state<<8) | buf[i];
            if ((state & 0xffffffffff00LL) == DNXHD_HEADER_PREFIX &&
                (state & 0x000000000003LL) < 3) { /* next coding unit is progressive frame or field 1 */
                pc->frame_start_found = 0;
                pc->state64 = -1;
                return i-5;
            }
        }
    }
    pc->frame_start_found = pic_found;
    pc->state64 = state;
    return END_NOT_FOUND;
}

static int dnxhd_parse(AVCodecParserContext *s,
                       AVCodecContext *avctx,
                       const uint8_t **poutbuf, int *poutbuf_size,
                       const uint8_t *buf, int buf_size)
{
    ParseContext *pc = s->priv_data;
    int next;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
    } else {
        next = dnxhd_find_frame_end(pc, buf, buf_size);
        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }
    *poutbuf = buf;
    *poutbuf_size = buf_size;
    return next;
}

AVCodecParser ff_dnxhd_parser = {
    { CODEC_ID_DNXHD },
    sizeof(ParseContext),
    NULL,
    dnxhd_parse,
    ff_parse_close,
};
