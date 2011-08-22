/*
 * RAW video demuxer
 * Copyright (c) 2001 Fabrice Bellard
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
#include "rawdec.h"

static int rawvideo_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int packet_size, ret, width, height;
    AVStream *st = s->streams[0];

    width = st->codec->width;
    height = st->codec->height;

    packet_size = avpicture_get_size(st->codec->pix_fmt, width, height);
    if (packet_size < 0)
        return -1;

    ret= av_get_packet(s->pb, pkt, packet_size);
    pkt->pts=
    pkt->dts= pkt->pos / packet_size;

    pkt->stream_index = 0;
    if (ret < 0)
        return ret;
    return 0;
}

static int rawvideo_read_seek(AVFormatContext *s, int stream_index,
                              int64_t ts, int flags)
{
    AVStream *st = s->streams[0];
    unsigned frame_size;

    if (url_is_streamed(s->pb))
        return -1;

    frame_size = avpicture_get_size(st->codec->pix_fmt,
                                    st->codec->width, st->codec->height);
    if ((ts+1)*frame_size > url_fsize(s->pb))
        return -1;

    url_fseek(s->pb, ts*frame_size, SEEK_SET);
    return 0;
}

AVInputFormat ff_rawvideo_demuxer = {
    "rawvideo",
    NULL_IF_CONFIG_SMALL("raw video format"),
    0,
    NULL,
    ff_raw_read_header,
    rawvideo_read_packet,
    .extensions = "yuv,cif,qcif,rgb",
    .value = CODEC_ID_RAWVIDEO,
    .read_seek = rawvideo_read_seek,
};
