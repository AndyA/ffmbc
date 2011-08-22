/*
 * MP3 muxer
 * Copyright (c) 2003 Fabrice Bellard
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

#include <strings.h>
#include "avformat.h"
#include "id3v1.h"
#include "id3v2.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"

#if 0
static int id3v1_set_string(AVFormatContext *s, const char *key,
                            uint8_t *buf, int buf_size)
{
    AVMetadataTag *tag;
    if ((tag = av_metadata_get(s->metadata, key, NULL, 0)))
        strncpy(buf, tag->value, buf_size);
    return !!tag;
}

static int id3v1_create_tag(AVFormatContext *s, uint8_t *buf)
{
    AVMetadataTag *tag;
    int i, count = 0;

    memset(buf, 0, ID3v1_TAG_SIZE); /* fail safe */
    buf[0] = 'T';
    buf[1] = 'A';
    buf[2] = 'G';
    count += id3v1_set_string(s, "TIT2",    buf +  3, 30);       //title
    count += id3v1_set_string(s, "TPE1",    buf + 33, 30);       //author|artist
    count += id3v1_set_string(s, "TALB",    buf + 63, 30);       //album
    count += id3v1_set_string(s, "TDRL",    buf + 93,  4);       //date
    count += id3v1_set_string(s, "comment", buf + 97, 30);
    if ((tag = av_metadata_get(s->metadata, "TRCK", NULL, 0))) { //track
        buf[125] = 0;
        buf[126] = atoi(tag->value);
        count++;
    }
    buf[127] = 0xFF; /* default to unknown genre */
    if ((tag = av_metadata_get(s->metadata, "TCON", NULL, 0))) { //genre
        for(i = 0; i <= ID3v1_GENRE_MAX; i++) {
            if (!strcasecmp(tag->value, ff_id3v1_genre_str[i])) {
                buf[127] = i;
                count++;
                break;
            }
        }
    }
    return count;
}
#endif

/* simple formats */

static int is_ascii(const uint8_t *str)
{
    while (*str && *str < 128) str++;
    return !*str;
}

static int len_put_str16(const char *str)
{
    const uint8_t *q = str;
    int ret = 0;
    uint32_t ch;

    do {
        uint16_t tmp;
        GET_UTF8(ch, *q++, break;);
        PUT_UTF16(ch, tmp, ret += 2;);
    } while (ch);
    return ret;
}

static void id3v2_put_size(AVFormatContext *s, int size)
{
    put_byte(s->pb, size >> 21 & 0x7f);
    put_byte(s->pb, size >> 14 & 0x7f);
    put_byte(s->pb, size >> 7  & 0x7f);
    put_byte(s->pb, size       & 0x7f);
}

static int id3v2_put_tag(AVFormatContext *s, const char *key, const char *value, int version)
{
    int len, encoding;

    if (version == 4)
        encoding = 3; // utf-8
    else if (is_ascii(value))
        encoding = 0; // iso-8859-1
    else
        encoding = 1; // utf-16be

    if (encoding == 1)
        len = 2+len_put_str16(value);
    else
        len = strlen(value)+1;

    put_tag(s->pb, key);
    if (version == 4)
        id3v2_put_size(s, 1+len);
    else
        put_be32(s->pb, 1+len);
    put_be16(s->pb, 0);
    put_byte(s->pb, encoding);
    if (encoding == 1) {
        put_be16(s->pb, 0xfffe); // BOM
        avio_put_str16le(s->pb, value);
    } else {
        put_buffer(s->pb, value, len);
    }
    return 4+4+2+1+len;
}

static int id3v2_put_apic(AVFormatContext *s, AVMetadataTag *tag, int version)
{
    const char *mime = av_metadata_get_attribute(tag, "mime");
    int len;
    if (!mime) {
        av_log(s, AV_LOG_ERROR, "error, no mime type set for cover\n");
        return 0;
    }
    put_tag(s->pb, "APIC");
    len = 1+strlen(mime)+1+1+1+tag->len;
    if (version == 4)
        id3v2_put_size(s, len);
    else
        put_be32(s->pb, len);
    put_be16(s->pb, 0); // flags
    put_byte(s->pb, 0); // encoding
    avio_put_str(s->pb, mime);
    put_byte(s->pb, 3); // type, cover front
    put_byte(s->pb, 0); // description
    put_buffer(s->pb, tag->value, tag->len);
    return 4+4+2+len;
}

static int id3v2_put_uslt(AVFormatContext *s, AVMetadataTag *tag, int version)
{
    const char *lang = av_metadata_get_attribute(tag, "language");
    int data_len, len, encoding;

    if (version == 4)
        encoding = 3; // utf-8
    else if (is_ascii(tag->value))
        encoding = 0; // iso-8859-1
    else
        encoding = 1; // utf-16be

    if (encoding == 1)
        data_len = 2+2 + 2+len_put_str16(tag->value);
    else
        data_len = 1 + strlen(tag->value)+1;
    len = 1+3+data_len;

    put_tag(s->pb, "USLT");
    if (version == 4)
        id3v2_put_size(s, len);
    else
        put_be32(s->pb, len);
    put_be16(s->pb, 0); // flags
    put_byte(s->pb, encoding);
    if (!lang)
        lang = "eng";
    put_byte(s->pb, lang[0]);
    put_byte(s->pb, lang[1]);
    put_byte(s->pb, lang[2]);
    if (encoding != 1) {
        put_byte(s->pb, 0); // description
        avio_put_str(s->pb, tag->value);
    } else {
        put_be16(s->pb, 0xfffe); // BOM
        put_be16(s->pb, 0); // description
        put_be16(s->pb, 0xfffe);
        avio_put_str16le(s->pb, tag->value);
    }
    return 4+4+2+len;
}

static int mp3_write_packet(struct AVFormatContext *s, AVPacket *pkt)
{
    put_buffer(s->pb, pkt->data, pkt->size);
    put_flush_packet(s->pb);
    return 0;
}

#if 0
static int mp3_write_trailer(struct AVFormatContext *s)
{
    uint8_t buf[ID3v1_TAG_SIZE];

    /* write the id3v1 tag */
    if (id3v1_create_tag(s, buf) > 0) {
        put_buffer(s->pb, buf, ID3v1_TAG_SIZE);
        put_flush_packet(s->pb);
    }
    return 0;
}
#endif

#if CONFIG_MP2_MUXER
AVOutputFormat ff_mp2_muxer = {
    "mp2",
    NULL_IF_CONFIG_SMALL("MPEG audio layer 2"),
    "audio/x-mpeg",
    "mp2,m2a",
    0,
    CODEC_ID_MP2,
    CODEC_ID_NONE,
    NULL,
    mp3_write_packet,
};
#endif

#if CONFIG_MP3_MUXER
typedef struct MP3Context {
    const AVClass *class;
    int id3v2_version;
} MP3Context;

static const AVOption options[] = {
    { "id3v2_version", "Select ID3v2 version to write. Currently 3 and 4 are supported.",
      offsetof(MP3Context, id3v2_version), FF_OPT_TYPE_INT, 3, 3, 4, AV_OPT_FLAG_ENCODING_PARAM},
    { NULL },
};

static const AVClass mp3_muxer_class = {
    "MP3 muxer",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

static int id3v2_check_write_tag(AVFormatContext *s, AVMetadataTag *t,
                                 const char table[][4], int version)
{
    MP3Context *mp3 = s->priv_data;
    int i;

    if (!strcmp(t->key, "APIC"))
        return id3v2_put_apic(s, t, version);
    else if (!strcmp(t->key, "USLT"))
        return id3v2_put_uslt(s, t, version);
    else if (t->key[0] != 'T' || strlen(t->key) != 4)
        return 0;

    if (!table) {
        table = mp3->id3v2_version == 3 ?
            ff_id3v2_3_tags : ff_id3v2_4_tags;
    }

    for (i = 0; *table[i]; i++) {
        if (AV_RB32(t->key) == AV_RB32(table[i]))
            return id3v2_put_tag(s, t->key, t->value, version);
    }
    return 0;
}

/**
 * Write an ID3v2 header at beginning of stream
 */

static int mp3_write_header(struct AVFormatContext *s)
{
    MP3Context *mp3 = s->priv_data;
    AVMetadataTag *t = NULL;
    int totlen = 0;
    int64_t size_pos, cur_pos;
    AVMetadata *metadata = NULL;

    put_be32(s->pb, MKBETAG('I', 'D', '3', mp3->id3v2_version));
    put_byte(s->pb, 0);
    put_byte(s->pb, 0); /* flags */

    /* reserve space for size */
    size_pos = url_ftell(s->pb);
    put_be32(s->pb, 0);

    ff_metadata_conv2(&metadata, &s->metadata, ff_id3v2_34_metadata_conv, NULL);
    if (mp3->id3v2_version == 4)
        ff_metadata_conv(&metadata, ff_id3v2_4_metadata_conv, NULL);

    while ((t = av_metadata_get(metadata, "", t, AV_METADATA_IGNORE_SUFFIX))) {
        int ret = id3v2_check_write_tag(s, t, ff_id3v2_tags, mp3->id3v2_version);
        if (ret <= 0)
            ret = id3v2_check_write_tag(s, t, NULL, mp3->id3v2_version);
        totlen += ret;
    }

    av_metadata_free(&metadata);

    cur_pos = url_ftell(s->pb);
    url_fseek(s->pb, size_pos, SEEK_SET);
    id3v2_put_size(s, totlen);
    url_fseek(s->pb, cur_pos, SEEK_SET);

    return 0;
}

AVOutputFormat ff_mp3_muxer = {
    "mp3",
    NULL_IF_CONFIG_SMALL("MPEG audio layer 3"),
    "audio/x-mpeg",
    "mp3",
    sizeof(MP3Context),
    CODEC_ID_MP3,
    CODEC_ID_NONE,
    mp3_write_header,
    mp3_write_packet,
    .flags = AVFMT_NOTIMESTAMPS,
    .priv_class = &mp3_muxer_class,
};
#endif
