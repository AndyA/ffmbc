/*
 * id RoQ (.roq) File muxer
 * Copyright (c) 2007 Vitor Sessak
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

#include "avformat.h"
#include "rawenc.h"


static int roq_write_header(struct AVFormatContext *s)
{
    static const uint8_t header[] = {
        0x84, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x1E, 0x00
    };

    put_buffer(s->pb, header, 8);
    put_flush_packet(s->pb);

    return 0;
}

AVOutputFormat ff_roq_muxer =
{
    "RoQ",
    NULL_IF_CONFIG_SMALL("raw id RoQ format"),
    NULL,
    "roq",
    0,
    CODEC_ID_ROQ_DPCM,
    CODEC_ID_ROQ,
    roq_write_header,
    ff_raw_write_packet,
};
