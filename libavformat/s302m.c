/*
 * S302M audio demuxer
 * Copyright (c) 2005 Reimar Döffinger
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

static int s302m_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    AVStream *st = av_new_stream(s, 0);
    if (!st)
        return AVERROR(ENOMEM);
    st->codec->codec_type = CODEC_TYPE_AUDIO;
    st->codec->codec_id = CODEC_ID_S302M;

    return 0;
}

static int s302m_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    ByteIOContext *pb = s->pb;
    int ret, size;
    if (url_feof(pb))
        return AVERROR(EIO);
    size = get_be16(pb);
    get_be16(pb);
    ret = av_get_packet(pb, pkt, size);
    pkt->stream_index = 0;
    return ret;
}

AVInputFormat ff_s302m_demuxer = {
    "s302m",
    NULL_IF_CONFIG_SMALL("SMPTE 302M Audio format"),
    0,
    NULL,
    s302m_read_header,
    s302m_read_packet,
    NULL,
    NULL,
    .extensions = "302",
};
