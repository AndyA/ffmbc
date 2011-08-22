/*
 * ID3v2 header parser
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

#include "id3v2.h"
#include "id3v1.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "metadata.h"

int ff_id3v2_match(const uint8_t *buf, const char * magic)
{
    return  buf[0]         == magic[0] &&
            buf[1]         == magic[1] &&
            buf[2]         == magic[2] &&
            buf[3]         != 0xff &&
            buf[4]         != 0xff &&
           (buf[6] & 0x80) ==    0 &&
           (buf[7] & 0x80) ==    0 &&
           (buf[8] & 0x80) ==    0 &&
           (buf[9] & 0x80) ==    0;
}

int ff_id3v2_tag_len(const uint8_t * buf)
{
    int len = ((buf[6] & 0x7f) << 21) +
              ((buf[7] & 0x7f) << 14) +
              ((buf[8] & 0x7f) << 7) +
               (buf[9] & 0x7f) +
              ID3v2_HEADER_SIZE;
    if (buf[5] & 0x10)
        len += ID3v2_HEADER_SIZE;
    return len;
}

static unsigned int get_size(ByteIOContext *s, int len)
{
    int v = 0;
    while (len--)
        v = (v << 7) + (get_byte(s) & 0x7F);
    return v;
}

static int read_id3v2_string(AVFormatContext *s, ByteIOContext *pb,
                             const char *key, int taglen,
                             int encoding, char *dst, int dstlen)
{
    unsigned int (*get)(ByteIOContext*) = get_be16;
    char *q = dst;
    uint8_t tmp;
    uint32_t ch;
    int bom;

    switch (encoding) {
    case ID3v2_ENCODING_ISO8859:
        while (taglen) {
            uint32_t val = get_byte(pb); taglen--;
            if (!val)
                break;
            PUT_UTF8(val, tmp, if (q - dst < dstlen) *q++ = tmp;);
        }
        *q = 0;
        break;
    case ID3v2_ENCODING_UTF16BOM:
        bom = get_be16(pb); taglen -= 2;
        switch (bom) {
        case 0xfffe:
            get = get_le16;
        case 0xfeff:
            break;
        default:
            av_log(s, AV_LOG_ERROR, "Incorrect BOM value: %x in tag %s\n", bom, key);
            return taglen;
        }
        // fall-through
    case ID3v2_ENCODING_UTF16BE:
        while (taglen > 1) {
            uint16_t val = get(pb); taglen -= 2;
            if (!val)
                break;
            GET_UTF16(ch, val, break;);
            PUT_UTF8(ch, tmp, if (q - dst < dstlen) *q++ = tmp;);
        }
        *q = 0;
        break;
    case ID3v2_ENCODING_UTF8:
        while (taglen) {
            uint32_t val = get_byte(pb); taglen--;
            if (!val)
                break;
            GET_UTF8(ch, val, break;);
            PUT_UTF8(ch, tmp, if (q - dst < dstlen) *q++ = tmp;);
        }
        *q = 0;
        break;
    default:
        av_log(s, AV_LOG_WARNING, "Unknown encoding in tag %s\n", key);
    }
    return taglen;
}

static void read_ttag(AVFormatContext *s, ByteIOContext *pb, int taglen, const char *key)
{
    char dst[512];
    const char *val = NULL;
    int len, dstlen = sizeof(dst) - 1;
    unsigned genre, encoding;

    if (taglen < 1)
        return;

    taglen--; /* account for encoding type byte */
    encoding = get_byte(pb);
    taglen = read_id3v2_string(s, pb, key, taglen, encoding, dst, dstlen);

    if (!(strcmp(key, "TCON") && strcmp(key, "TCO"))
        && (sscanf(dst, "(%d)", &genre) == 1 || sscanf(dst, "%d", &genre) == 1)
        && genre <= ID3v1_GENRE_MAX)
        val = ff_id3v1_genre_str[genre];
    else if (!(strcmp(key, "TXXX") && strcmp(key, "TXX"))) {
        /* dst now contains two 0-terminated strings */
        len = strlen(dst);
        read_id3v2_string(s, pb, key, taglen, encoding, dst+len, dstlen-len);
        val = dst+len;
    } else if (!strcmp(key, "TDAT")) {
        /* date in the form DDMM, change to DD/MM */
        dst[5] = 0;
        dst[4] = dst[3];
        dst[3] = dst[2];
        dst[2] = '/';
        val = dst;
    } else if (*dst)
        val = dst;

    if (val)
        av_metadata_set2(&s->metadata, key, val, AV_METADATA_DONT_OVERWRITE);
}

static int read_uslt(AVFormatContext *s, int taglen, const char *key)
{
    AVMetadataTag *tag;
    uint8_t *data;
    char lang[4];
    int encoding;

    encoding = get_byte(s->pb); // encoding
    get_buffer(s->pb, lang, 3);
    taglen -= 4;
    taglen = read_id3v2_string(s, s->pb, key, taglen, encoding, lang+3, 0); // description
    data = av_malloc(taglen);
    if (!data)
        return AVERROR_NOMEM;
    read_id3v2_string(s, s->pb, key, taglen, encoding, data, taglen-1);

    if (av_metadata_set_custom(&s->metadata, &tag, METADATA_STRING, key, data,
                               strlen(data), AV_METADATA_DONT_STRDUP_VAL) < 0)
        return -1;
    av_metadata_set_attribute(tag, "language", lang);

    return 0;
}

static int read_apic(AVFormatContext *s, int taglen, const char *key)
{
    AVMetadataTag *tag;
    char mime[64];
    int64_t pos = url_ftell(s->pb);
    int len;
    uint8_t *data;

    get_byte(s->pb); // encoding
    get_strz(s->pb, mime, sizeof(mime));
    get_byte(s->pb); // type
    while (get_byte(s->pb)); // description

    len = taglen - (url_ftell(s->pb) - pos);
    if (len <= 0)
        return -1;
    data = av_malloc(len);
    if (!data)
        return AVERROR_NOMEM;
    get_buffer(s->pb, data, len);

    if (av_metadata_set_custom(&s->metadata, &tag, METADATA_BYTEARRAY, "APIC",
                               data, len, AV_METADATA_DONT_STRDUP_VAL) < 0)
        return -1;
    av_metadata_set_attribute(tag, "mime", mime);

    return 0;
}

static void ff_id3v2_parse(AVFormatContext *s, int len, uint8_t version, uint8_t flags)
{
    int isv34, tlen, unsync;
    char tag[5];
    int64_t next;
    int taghdrlen;
    const char *reason;
    ByteIOContext pb;
    unsigned char *buffer = NULL;
    int buffer_size = 0;

    switch (version) {
    case 2:
        if (flags & 0x40) {
            reason = "compression";
            goto error;
        }
        isv34 = 0;
        taghdrlen = 6;
        break;

    case 3:
    case 4:
        isv34 = 1;
        taghdrlen = 10;
        break;

    default:
        reason = "version";
        goto error;
    }

    unsync = flags & 0x80;

    if (isv34 && flags & 0x40) /* Extended header present, just skip over it */
        url_fskip(s->pb, get_size(s->pb, 4));

    while (len >= taghdrlen) {
        unsigned int tflags = 0;
        int tunsync = 0;

        if (isv34) {
            get_buffer(s->pb, tag, 4);
            tag[4] = 0;
            if(version==3){
                tlen = get_be32(s->pb);
            }else
                tlen = get_size(s->pb, 4);
            tflags = get_be16(s->pb);
            tunsync = tflags & ID3v2_FLAG_UNSYNCH;
        } else {
            get_buffer(s->pb, tag, 3);
            tag[3] = 0;
            tlen = get_be24(s->pb);
        }
        len -= taghdrlen + tlen;

        if (len < 0)
            break;

        next = url_ftell(s->pb) + tlen;

        if (tflags & ID3v2_FLAG_DATALEN) {
            get_be32(s->pb);
            tlen -= 4;
        }

        if (tflags & (ID3v2_FLAG_ENCRYPTION | ID3v2_FLAG_COMPRESSION)) {
            av_log(s, AV_LOG_WARNING, "Skipping encrypted/compressed ID3v2 frame %s.\n", tag);
            url_fskip(s->pb, tlen);
        } else if (tag[0] == 'T') {
            if (unsync || tunsync) {
                int i, j;
                av_fast_malloc(&buffer, &buffer_size, tlen);
                for (i = 0, j = 0; i < tlen; i++, j++) {
                    buffer[j] = get_byte(s->pb);
                    if (j > 0 && !buffer[j] && buffer[j - 1] == 0xff) {
                        /* Unsynchronised byte, skip it */
                        j--;
                    }
                }
                init_put_byte(&pb, buffer, j, 0, NULL, NULL, NULL, NULL);
                read_ttag(s, &pb, j, tag);
            } else {
                read_ttag(s, s->pb, tlen, tag);
            }
        } else if (!strcmp(tag, "APIC")) {
            read_apic(s, tlen, tag);
        } else if (!strcmp(tag, "USLT") || !strcmp(tag, "ULT")) {
            read_uslt(s, tlen, tag);
        } else if (!tag[0]) {
            if (tag[1])
                av_log(s, AV_LOG_WARNING, "invalid frame id, assuming padding");
            url_fskip(s->pb, tlen);
            break;
        }
        /* Skip to end of tag */
        url_fseek(s->pb, next, SEEK_SET);
    }

    if (len > 0) {
        /* Skip padding */
        url_fskip(s->pb, len);
    }
    if (version == 4 && flags & 0x10) /* Footer preset, always 10 bytes, skip over it */
        url_fskip(s->pb, 10);

    av_free(buffer);
    return;

  error:
    av_log(s, AV_LOG_INFO, "ID3v2.%d tag skipped, cannot handle %s\n", version, reason);
    url_fskip(s->pb, len);
    av_free(buffer);
}

void ff_id3v2_read(AVFormatContext *s, const char *magic)
{
    int len, ret;
    uint8_t buf[ID3v2_HEADER_SIZE];
    int     found_header;
    int64_t off;

    do {
        /* save the current offset in case there's nothing to read/skip */
        off = url_ftell(s->pb);
        ret = get_buffer(s->pb, buf, ID3v2_HEADER_SIZE);
        if (ret != ID3v2_HEADER_SIZE)
            break;
            found_header = ff_id3v2_match(buf, magic);
            if (found_header) {
            /* parse ID3v2 header */
            len = ((buf[6] & 0x7f) << 21) |
                  ((buf[7] & 0x7f) << 14) |
                  ((buf[8] & 0x7f) << 7) |
                   (buf[9] & 0x7f);
            ff_id3v2_parse(s, len, buf[3], buf[5]);
        } else {
            url_fseek(s->pb, off, SEEK_SET);
        }
    } while (found_header);
    ff_metadata_conv(&s->metadata, NULL, ff_id3v2_34_metadata_conv);
    ff_metadata_conv(&s->metadata, NULL, ff_id3v2_2_metadata_conv);
    ff_metadata_conv(&s->metadata, NULL, ff_id3v2_4_metadata_conv);
}

const AVMetadataConv ff_id3v2_34_metadata_conv[] = {
    { "APIC", "cover"},
    { "TALB", "album"},
    { "TCOM", "composer"},
    { "TCON", "genre"},
    { "TCOP", "copyright"},
    { "TENC", "encoder"},
    { "TIT2", "title"},
    { "TLAN", "language"},
    { "TPE1", "artist"},
    { "TPE2", "album_artist"},
    { "TPE3", "performer"},
    { "TPOS", "disc"},
    { "TPUB", "publisher"},
    { "TRCK", "track"},
    { "TSSE", "encoder"},
    { "TYER", "year"},
    { "USLT", "lyrics"},
    { 0 }
};

const AVMetadataConv ff_id3v2_4_metadata_conv[] = {
    { "TDRC", "date"},
    { "TDRL", "release_date"},
    { "TDEN", "creation_time"},
    { "TSOA", "album-sort"},
    { "TSOP", "artist-sort"},
    { "TSOT", "title-sort"},
    { 0 }
};

const AVMetadataConv ff_id3v2_2_metadata_conv[] = {
    { "TAL",  "album"},
    { "TCM",  "composer"},
    { "TCO",  "genre"},
    { "TT2",  "title"},
    { "TEN",  "encoder"},
    { "TP1",  "artist"},
    { "TP2",  "album_artist"},
    { "TP3",  "performer"},
    { "TRK",  "track"},
    { "ULT",  "lyrics"},
    { "TYE",  "year"},
    { 0 }
};


const char ff_id3v2_tags[][4] = {
   "TALB", "TBPM", "TCOM", "TCON", "TCOP", "TDLY", "TENC", "TEXT",
   "TFLT", "TIT1", "TIT2", "TIT3", "TKEY", "TLAN", "TLEN", "TMED",
   "TOAL", "TOFN", "TOLY", "TOPE", "TOWN", "TPE1", "TPE2", "TPE3",
   "TPE4", "TPOS", "TPUB", "TRCK", "TRSN", "TRSO", "TSRC", "TSSE",
   "APIC", "USLT",
   { 0 },
};

const char ff_id3v2_4_tags[][4] = {
   "TDEN", "TDOR", "TDRC", "TDRL", "TDTG", "TIPL", "TMCL", "TMOO",
   "TPRO", "TSOA", "TSOP", "TSOT", "TSST",
   { 0 },
};

const char ff_id3v2_3_tags[][4] = {
   "TDAT", "TIME", "TORY", "TRDA", "TSIZ", "TYER",
   { 0 },
};
