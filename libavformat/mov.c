/*
 * MOV demuxer
 * Copyright (c) 2001 Fabrice Bellard
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

#include <limits.h>

//#define DEBUG
//#define MOV_EXPORT_ALL_METADATA

#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "avformat.h"
#include "riff.h"
#include "isom.h"
#include "id3v1.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/timecode.h"

#if CONFIG_ZLIB
#include <zlib.h>
#endif

/*
 * First version by Francois Revol revol@free.fr
 * Seek function by Gael Chardon gael.dev@4now.net
 *
 * Features and limitations:
 * - reads most of the QT files I have (at least the structure),
 *   Sample QuickTime files with mp3 audio can be found at: http://www.3ivx.com/showcase.html
 * - the code is quite ugly... maybe I won't do it recursive next time :-)
 *
 * Funny I didn't know about http://sourceforge.net/projects/qt-ffmpeg/
 * when coding this :) (it's a writer anyway)
 *
 * Reference documents:
 * http://www.geocities.com/xhelmboyx/quicktime/formats/qtm-layout.txt
 * Apple:
 *  http://developer.apple.com/documentation/QuickTime/QTFF/
 *  http://developer.apple.com/documentation/QuickTime/QTFF/qtff.pdf
 * QuickTime is a trademark of Apple (AFAIK :))
 */

#include "qtpalette.h"


#undef NDEBUG
#include <assert.h>

/* XXX: it's the first time I make a recursive parser I think... sorry if it's ugly :P */

/* those functions parse an atom */
/* return code:
  0: continue to parse next atom
 <0: error occurred, exit
*/
/* links atom IDs to parse functions */
typedef struct MOVParseTableEntry {
    uint32_t type;
    int (*parse)(MOVContext *ctx, ByteIOContext *pb, MOVAtom atom);
} MOVParseTableEntry;

static const MOVParseTableEntry mov_default_parse_table[];

static int mov_metadata_gnre(MOVContext *c, ByteIOContext *pb, unsigned len)
{
    uint16_t genre = get_be16(pb);
    if (genre-1 < ID3v1_GENRE_MAX)
        av_metadata_set2(c->metadata, "genre", ff_id3v1_genre_str[genre-1], 0);

    return 0;
}

static int mov_metadata_covr(MOVContext *c, ByteIOContext *pb, unsigned len, unsigned type)
{
    AVMetadataTag *tag;
    uint8_t *data = av_malloc(len);
    if (!data)
        return AVERROR_NOMEM;
    get_buffer(pb, data, len);
    if (av_metadata_set_custom(c->metadata, &tag, METADATA_BYTEARRAY, "cover",
                               data, len, AV_METADATA_DONT_STRDUP_VAL) < 0)
        return -1;
    if (type == 14 || type == AV_RB32("PNGf"))
        av_metadata_set_attribute(tag, "mime", "image/png");
    else if (type == 13)
        av_metadata_set_attribute(tag, "mime", "image/jpeg");
    else if (type == 27)
        av_metadata_set_attribute(tag, "mime", "image/bmp");

    return 0;
}

static int mov_metadata_trkn(MOVContext *c, ByteIOContext *pb, unsigned len)
{
    char buf[16];
    int track, track_count;
    get_be16(pb); // unknown
    track = get_be16(pb);
    track_count = get_be16(pb);
    if (track_count)
        snprintf(buf, sizeof(buf), "%d/%d", track, track_count);
    else
        snprintf(buf, sizeof(buf), "%d", track);
    av_metadata_set2(c->metadata, "track", buf, 0);

    return 0;
}

static int mov_read_keys(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    unsigned int i, entries;

    get_be32(pb); // version+flags

    entries = get_be32(pb);
    if (entries >= UINT_MAX/sizeof(*c->keys_data))
        return -1;

    for (i = 0; i < c->keys_count; i++)
        av_freep(&c->keys_data[i]);
    av_freep(&c->keys_data);

    c->keys_data = av_mallocz(entries*sizeof(*c->keys_data));
    if (!c->keys_data)
        return AVERROR_NOMEM;
    c->keys_count = entries;

    for (i = 0; i < entries; i++) {
        uint32_t size = get_be32(pb);
        url_fskip(pb, 4); // 'mdta'
        if (size > atom.size || size < 8)
            return 0;
        size -= 8;
        c->keys_data[i] = av_malloc(size+1);
        if (!c->keys_data[i])
            return AVERROR_NOMEM;
        get_buffer(pb, c->keys_data[i], size);
        c->keys_data[i][size] = 0;
    }

    return 0;
}

static const uint32_t mac_to_unicode[128] = {
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,
    0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,
    0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,
    0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,
    0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,
    0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,
    0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,
    0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,
    0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7,
};

static int mov_read_mac_string(MOVContext *c, ByteIOContext *pb, int len,
                               char *dst, int dstlen)
{
    char *p = dst;
    char *end = dst+dstlen-1;
    int i;

    for (i = 0; i < len; i++) {
        uint8_t t, c = get_byte(pb);
        if (c < 0x80 && p < end)
            *p++ = c;
        else
            PUT_UTF8(mac_to_unicode[c-0x80], t, if (p < end) *p++ = t;);
    }
    *p = 0;
    return p - dst;
}

static const struct {
    uint32_t tag;
    const char *key;
    int (*parse)();
} udta_parse_table[] = {
    { MKTAG( 'a','A','R','T'), "album_artist" },
    { MKTAG( 'c','p','r','t'), "copyright" },
    { MKTAG( 'd','e','s','c'), "description" },
    { MKTAG( 'l','d','e','s'), "synopsis" },
    { MKTAG( 't','v','s','h'), "show" },
    { MKTAG( 't','v','e','n'), "episode_id" },
    { MKTAG( 't','v','n','n'), "network" },
    { MKTAG( 'c','a','t','g'), "category" },
    { MKTAG( 'c','o','v','r'), "cover", mov_metadata_covr },
    { MKTAG( 'g','n','r','e'), "genre", mov_metadata_gnre },
    { MKTAG( 't','r','k','n'), "track", mov_metadata_trkn },
    { MKTAG( 'n','a','m','e'), "reel_name" },
    { MKTAG(0xa9,'A','R','T'), "artist" },
    { MKTAG(0xa9,'P','R','D'), "product" },
    { MKTAG(0xa9,'a','l','b'), "album" },
    { MKTAG(0xa9,'a','u','t'), "author" },
    { MKTAG(0xa9,'c','m','t'), "comment" },
    { MKTAG(0xa9,'c','p','y'), "copyright" },
    { MKTAG(0xa9,'d','a','y'), "date" },
    { MKTAG(0xa9,'e','n','c'), "encoder" },
    { MKTAG(0xa9,'s','w','r'), "encoder" },
    { MKTAG(0xa9,'f','m','t'), "original_format" },
    { MKTAG(0xa9,'g','e','n'), "genre" },
    { MKTAG(0xa9,'i','n','f'), "comment" },
    { MKTAG(0xa9,'n','a','m'), "title" },
    { MKTAG(0xa9,'t','o','o'), "encoder" },
    { MKTAG(0xa9,'w','r','t'), "composer" },
    { MKTAG(0xa9,'d','e','s'), "description" },
    { MKTAG(0xa9,'l','y','r'), "lyrics" },
    { 0 },
};

static int mov_read_udta(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    enum AVMetadataType type;
    char language[4] = {0};
    const char *key = NULL;
    uint16_t langcode = 0;
    uint32_t data_type = 0;
    unsigned i, size;
    int (*parse)() = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(udta_parse_table); i++) {
        if (udta_parse_table[i].tag == atom.type) {
            key = udta_parse_table[i].key;
            parse = udta_parse_table[i].parse;
            break;
        }
    }

    if (c->itunes_metadata && atom.size > 8) {
        int data_size = get_be32(pb);
        int tag = get_le32(pb);
        if (tag == MKTAG('d','a','t','a')) {
            data_type = get_be32(pb); // type
            av_dlog(c->fc, "data type: %d\n", data_type);
            switch (data_type) {
            case  1:  type = METADATA_STRING; break; // UTF-8
          //case  2:  type = METADATA_STRING; break; // UTF-16BE
            case  3:  type = METADATA_STRING; break; // MAC Encoded
            case 21:  type = METADATA_INT;    break; // signed
            case 22:  type = METADATA_INT;    break; // unsigned
            case 23:  type = METADATA_FLOAT;  break; // 32BE
            case 24:  type = METADATA_FLOAT;  break; // 64BE
            default:
                av_dlog(c->fc, "unsupported data type\n");
                type = METADATA_BYTEARRAY;
                break;
            }
            get_be32(pb); // unknown
            size = data_size - 16;
            atom.size -= 16;
        } else return 0;
    } else if (!c->itunes_metadata && atom.size > 4) {
        size = get_be16(pb); // string length
        langcode = get_be16(pb);
        if (!ff_mov_lang_to_iso639(langcode, language) ||
            size > atom.size) {
            language[0] = 0;
            langcode = 0;
            url_fseek(pb, -4, SEEK_CUR);
            goto unrecognized;
        }
        atom.size -= 4;
        type = METADATA_STRING;
    } else {
    unrecognized:
        size = atom.size;
        type = METADATA_BYTEARRAY;
    }

    if (!key) {
        unsigned tagbe = AV_RB32(&atom.type)-1;
        if (tagbe < c->keys_count) {
            key = c->keys_data[tagbe];
        } else {
#ifdef MOV_EXPORT_ALL_METADATA
            char keybuf[5];
            AV_WL32(keybuf, atom.type);
            keybuf[4] = 0;
            key = keybuf;
#endif
        }
    }

    if (!key)
        return 0;
    if (atom.size < 0)
        return -1;

    size = FFMIN(size, atom.size);

    //av_log(c->fc, AV_LOG_DEBUG, "name %.4s data type %d size %d\n",
    //       (char*)&atom.type, data_type, size);

    if (parse) {
        parse(c, pb, size, data_type);
    } else if (type == METADATA_FLOAT) {
        double value;
        switch (size) {
        case 8: value = av_int2dbl(get_be64(pb)); break;
        case 4: value = av_int2dbl(get_be32(pb)); break;
        default:
            av_dlog(c->fc, "unsupported int size: %d\n", size);
            url_fskip(pb, size);
            value = 0;
            break;
        }
        av_metadata_set_float(c->metadata, key, value);
    } else if (type == METADATA_INT) {
        int value;
        switch (size) {
        case 4: value = get_be32(pb); break;
        case 3: value = get_be24(pb); break;
        case 2: value = get_be16(pb); break;
        case 1: value = get_byte(pb); break;
        default:
            av_dlog(c->fc, "unsupported int size: %d\n", size);
            url_fskip(pb, size);
            value = 0;
            break;
        }
        av_metadata_set_int(c->metadata, key, value);
    } else {
        AVMetadataTag *tag = NULL;
        uint8_t *buf;
        unsigned flags = AV_METADATA_DONT_STRDUP_VAL;
        if (type == METADATA_STRING) {
            if (data_type == 3 || (data_type == 0 && langcode < 0x800)) { // MAC Encoded
                char tmp[1024];
                size = mov_read_mac_string(c, pb, size, tmp, sizeof(tmp));
                flags = 0;
                buf = tmp;
            } else { // UTF-8
                if (size >= UINT_MAX)
                    return 0;
                buf = av_malloc(size+1);
                if (!buf)
                    return AVERROR_NOMEM;
                get_buffer(pb, buf, size);
                buf[size] = 0;
            }
        } else {
            buf = av_malloc(size);
            if (!buf)
                return AVERROR_NOMEM;
            get_buffer(pb, buf, size);
        }
        if (av_metadata_set_custom(c->metadata, &tag, type, key, buf, size, flags) < 0)
            return 0;

        if (*language && strcmp(language, "und"))
            av_metadata_set_attribute(tag, "language", language);
    }

    return 0;
}

static int mov_read_chpl(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int64_t start;
    int i, nb_chapters, str_len, version;
    char str[256+1];

    if ((atom.size -= 5) < 0)
        return 0;

    version = get_byte(pb);
    get_be24(pb);
    if (version)
        get_be32(pb); // ???
    nb_chapters = get_byte(pb);

    for (i = 0; i < nb_chapters; i++) {
        if (atom.size < 9)
            return 0;

        start = get_be64(pb);
        str_len = get_byte(pb);

        if ((atom.size -= 9+str_len) < 0)
            return 0;

        get_buffer(pb, str, str_len);
        str[str_len] = 0;
        ff_new_chapter(c->fc, i, (AVRational){1,10000000}, start, AV_NOPTS_VALUE, str);
    }
    return 0;
}

static int mov_read_default(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int64_t total_size = 0;
    MOVAtom a;
    int i;

    if (atom.size < 0)
        atom.size = INT64_MAX;
    while (total_size + 8 < atom.size && !url_feof(pb)) {
        int (*parse)(MOVContext*, ByteIOContext*, MOVAtom) = NULL;
        a.size = atom.size;
        a.type=0;
        if(atom.size >= 8) {
            a.size = get_be32(pb);
            a.type = get_le32(pb);
        }
        av_dlog(c->fc, "type: %08x '%.4s' parent:'%.4s' sz: %"PRId64" %"PRId64" %"PRId64"\n",
                a.type, (char*)&a.type, (char*)&atom.type, a.size, total_size, atom.size);
        total_size += 8;
        if (a.size == 1) { /* 64 bit extended size */
            a.size = get_be64(pb) - 8;
            total_size += 8;
        }
        if (a.size == 0) {
            a.size = atom.size - total_size;
            if (a.size <= 8)
                break;
        }
        a.size -= 8;
        if(a.size < 0)
            break;
        a.size = FFMIN(a.size, atom.size - total_size);

        for (i = 0; mov_default_parse_table[i].type; i++)
            if (mov_default_parse_table[i].type == a.type) {
                parse = mov_default_parse_table[i].parse;
                break;
            }

        // container is user data
        if (!parse && (atom.type == MKTAG('u','d','t','a') ||
                       atom.type == MKTAG('i','l','s','t')))
            parse = mov_read_udta;

        if (!parse) { /* skip leaf atoms data */
            url_fskip(pb, a.size);
        } else {
            int64_t left, start_pos = url_ftell(pb);
            int err = parse(c, pb, a);
            if (err < 0)
                return err;
            if (c->found_moov && c->found_mdat &&
                (url_is_streamed(pb) || start_pos + a.size == url_fsize(pb)))
                return 0;
            left = a.size - url_ftell(pb) + start_pos;
            if (left < 0) {
                av_log(c->fc, AV_LOG_WARNING, "atom '%.4s' left %"PRId64"\n",
                       (char*)&a.type, left);
            } else if (left > 0) { /* skip garbage at atom end */
                av_dlog(c->fc, "atom '%.4s' left %"PRId64"\n",
                        (char*)&a.type, left);
                url_fskip(pb, left);
            }
        }
        total_size += a.size;
    }

    return 0;
}

static int mov_read_dref(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    int entries, i, j;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_be32(pb); // version + flags
    entries = get_be32(pb);
    if (entries >= UINT_MAX / sizeof(*sc->drefs))
        return -1;
    sc->drefs = av_mallocz(entries * sizeof(*sc->drefs));
    if (!sc->drefs)
        return AVERROR(ENOMEM);
    sc->drefs_count = entries;

    for (i = 0; i < sc->drefs_count; i++) {
        MOVDref *dref = &sc->drefs[i];
        uint32_t size = get_be32(pb);
        int64_t next = url_ftell(pb) + size - 4;

        if (size < 12)
            return -1;

        dref->type = get_le32(pb);
        get_be32(pb); // version + flags
        av_dlog(c->fc, "type %.4s size %d\n", (char*)&dref->type, size);

        if (dref->type == MKTAG('a','l','i','s') && size > 150) {
            /* macintosh alias record */
            uint16_t volume_len, len;
            int16_t type;

            url_fskip(pb, 10);

            volume_len = get_byte(pb);
            volume_len = FFMIN(volume_len, 27);
            get_buffer(pb, dref->volume, 27);
            dref->volume[volume_len] = 0;
            av_log(c->fc, AV_LOG_DEBUG, "volume %s, len %d\n", dref->volume, volume_len);

            url_fskip(pb, 12);

            len = get_byte(pb);
            len = FFMIN(len, 63);
            get_buffer(pb, dref->filename, 63);
            dref->filename[len] = 0;
            av_log(c->fc, AV_LOG_DEBUG, "filename %s, len %d\n", dref->filename, len);

            url_fskip(pb, 16);

            /* read next level up_from_alias/down_to_target */
            dref->nlvl_from = get_be16(pb);
            dref->nlvl_to   = get_be16(pb);
            av_log(c->fc, AV_LOG_DEBUG, "nlvl from %d, nlvl to %d\n",
                   dref->nlvl_from, dref->nlvl_to);

            url_fskip(pb, 16);

            for (type = 0; type != -1 && url_ftell(pb) < next; ) {
                type = get_be16(pb);
                len = get_be16(pb);
                av_log(c->fc, AV_LOG_DEBUG, "type %d, len %d\n", type, len);
                if (len&1)
                    len += 1;
                if (type == 2) { // absolute path
                    av_free(dref->path);
                    dref->path = av_mallocz(len+1);
                    if (!dref->path)
                        return AVERROR(ENOMEM);
                    get_buffer(pb, dref->path, len);
                    if (len > volume_len && !strncmp(dref->path, dref->volume, volume_len)) {
                        len -= volume_len;
                        memmove(dref->path, dref->path+volume_len, len);
                        dref->path[len] = 0;
                    }
                    for (j = 0; j < len; j++)
                        if (dref->path[j] == ':')
                            dref->path[j] = '/';
                    av_log(c->fc, AV_LOG_DEBUG, "path %s\n", dref->path);
                } else if (type == 0) { // directory name
                    av_free(dref->dir);
                    dref->dir = av_malloc(len+1);
                    if (!dref->dir)
                        return AVERROR(ENOMEM);
                    get_buffer(pb, dref->dir, len);
                    dref->dir[len] = 0;
                    for (j = 0; j < len; j++)
                        if (dref->dir[j] == ':')
                            dref->dir[j] = '/';
                    av_log(c->fc, AV_LOG_DEBUG, "dir %s\n", dref->dir);
                } else
                    url_fskip(pb, len);
            }
        }
        url_fseek(pb, next, SEEK_SET);
    }
    return 0;
}

static int mov_read_hdlr(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    uint32_t type;
    uint32_t ctype;

    if (c->fc->nb_streams < 1) // meta before first trak
        return 0;

    st = c->fc->streams[c->fc->nb_streams-1];

    get_byte(pb); /* version */
    get_be24(pb); /* flags */

    /* component type */
    ctype = get_le32(pb);
    type = get_le32(pb); /* component subtype */

    av_dlog(c->fc, "ctype= %.4s (0x%08x)\n", (char*)&ctype, ctype);
    av_dlog(c->fc, "stype= %.4s\n", (char*)&type);

    if     (type == MKTAG('v','i','d','e'))
        st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    else if(type == MKTAG('s','o','u','n'))
        st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
    else if(type == MKTAG('m','1','a',' '))
        st->codec->codec_id = CODEC_ID_MP2;
    else if(type == MKTAG('s','u','b','p'))
        st->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;

    get_be32(pb); /* component  manufacture */
    get_be32(pb); /* component flags */
    get_be32(pb); /* component flags mask */

    return 0;
}

int ff_mov_read_esds(AVFormatContext *fc, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    int tag, len;

    if (fc->nb_streams < 1)
        return 0;
    st = fc->streams[fc->nb_streams-1];

    get_be32(pb); /* version + flags */
    len = ff_mp4_read_descr(fc, pb, &tag);
    if (tag == MP4ESDescrTag) {
        get_be16(pb); /* ID */
        get_byte(pb); /* priority */
    } else
        get_be16(pb); /* ID */

    len = ff_mp4_read_descr(fc, pb, &tag);
    if (tag == MP4DecConfigDescrTag)
        ff_mp4_read_dec_config_descr(fc, st, pb);
    return 0;
}

static int mov_read_esds(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    return ff_mov_read_esds(c->fc, pb, atom);
}

static int mov_read_dac3(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    int ac3info, acmod, lfeon;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    ac3info = get_be24(pb);
    acmod = (ac3info >> 11) & 0x7;
    lfeon = (ac3info >> 10) & 0x1;
    st->codec->channels = ((int[]){2,1,2,3,3,4,4,5})[acmod] + lfeon;

    return 0;
}

static int mov_read_pasp(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    const int num = get_be32(pb);
    const int den = get_be32(pb);
    AVStream *st;
    MOVStreamContext *sc;

    if (c->fc->nb_streams < 1 || num < 0 || den < 0)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    av_reduce(&sc->pixel_aspect.num, &sc->pixel_aspect.den,
              num, den, INT_MAX);

    return 0;
}

/* this atom contains actual media data */
static int mov_read_mdat(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    if(atom.size == 0) /* wrong one (MP4) */
        return 0;
    c->found_mdat=1;
    return 0; /* now go for moov */
}

/* read major brand, minor version and compatible brands and store them as metadata */
static int mov_read_ftyp(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    uint32_t minor_ver;
    int comp_brand_size;
    char minor_ver_str[11]; /* 32 bit integer -> 10 digits + null */
    char* comp_brands_str;
    uint8_t type[5] = {0};

    get_buffer(pb, type, 4);
    if (strcmp(type, "qt  "))
        c->isom = 1;
    av_log(c->fc, AV_LOG_DEBUG, "ISO: File Type Major Brand: %.4s\n",(char *)&type);
    av_metadata_set2(&c->fc->metadata, "major_brand", type, 0);
    minor_ver = get_be32(pb); /* minor version */
    snprintf(minor_ver_str, sizeof(minor_ver_str), "%d", minor_ver);
    av_metadata_set2(&c->fc->metadata, "minor_version", minor_ver_str, 0);

    comp_brand_size = atom.size - 8;
    if (comp_brand_size < 0)
        return -1;
    comp_brands_str = av_malloc(comp_brand_size + 1); /* Add null terminator */
    if (!comp_brands_str)
        return AVERROR(ENOMEM);
    get_buffer(pb, comp_brands_str, comp_brand_size);
    comp_brands_str[comp_brand_size] = 0;
    av_metadata_set2(&c->fc->metadata, "compatible_brands", comp_brands_str, 0);
    av_freep(&comp_brands_str);

    return 0;
}

/* this atom should contain all header atoms */
static int mov_read_moov(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    if (c->found_moov) {
        av_log(c->fc, AV_LOG_WARNING, "warning, found double moov atom\n");
        return 0;
    }
    if (mov_read_default(c, pb, atom) < 0)
        return -1;
    /* we parsed the 'moov' atom, we can terminate the parsing as soon as we find the 'mdat' */
    /* so we don't parse the whole file if over a network */
    c->found_moov=1;
    return 0; /* now go for mdat */
}

static int mov_read_moof(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    c->fragment.moof_offset = url_ftell(pb) - 8;
    av_dlog(c->fc, "moof offset %llx\n", c->fragment.moof_offset);
    return mov_read_default(c, pb, atom);
}

static void mov_metadata_creation_time(AVMetadata **metadata, time_t time)
{
    char buffer[32];
    if (time) {
        struct tm *ptm;
        time -= 2082844800;  /* seconds between 1904-01-01 and Epoch */
        ptm = gmtime(&time);
        if (!ptm) return;
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", ptm);
        av_metadata_set2(metadata, "creation_time", buffer, 0);
    }
}

static int mov_read_mdhd(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    int version;
    char language[4] = {0};
    unsigned lang;
    time_t creation_time;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    version = get_byte(pb);
    if (version > 1)
        return -1; /* unsupported */

    get_be24(pb); /* flags */
    if (version == 1) {
        creation_time = get_be64(pb);
        get_be64(pb);
    } else {
        creation_time = get_be32(pb);
        get_be32(pb); /* modification time */
    }
    sc->time_scale = get_be32(pb);
    st->duration = (version == 1) ? get_be64(pb) : get_be32(pb); /* duration */

    lang = get_be16(pb); /* language */
    if (ff_mov_lang_to_iso639(lang, language))
        av_metadata_set2(&st->metadata, "language", language, 0);
    get_be16(pb); /* quality */

    return 0;
}

static int mov_read_mvhd(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    time_t creation_time;
    int version = get_byte(pb); /* version */
    get_be24(pb); /* flags */

    if (version == 1) {
        creation_time = get_be64(pb);
        get_be64(pb);
    } else {
        creation_time = get_be32(pb);
        get_be32(pb); /* modification time */
    }
    mov_metadata_creation_time(&c->fc->metadata, creation_time);
    c->time_scale = get_be32(pb); /* time scale */

    av_dlog(c->fc, "time scale = %i\n", c->time_scale);

    c->duration = (version == 1) ? get_be64(pb) : get_be32(pb); /* duration */
    get_be32(pb); /* preferred scale */

    get_be16(pb); /* preferred volume */

    url_fskip(pb, 10); /* reserved */

    url_fskip(pb, 36); /* display matrix */

    get_be32(pb); /* preview time */
    get_be32(pb); /* preview duration */
    get_be32(pb); /* poster time */
    get_be32(pb); /* selection time */
    get_be32(pb); /* selection duration */
    get_be32(pb); /* current time */
    get_be32(pb); /* next track ID */

    return 0;
}

static int mov_read_enda(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    int little_endian;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    little_endian = get_be16(pb);
    av_dlog(c->fc, "enda %d\n", little_endian);
    if (little_endian == 1) {
        switch (st->codec->codec_id) {
        case CODEC_ID_PCM_S24BE:
            st->codec->codec_id = CODEC_ID_PCM_S24LE;
            break;
        case CODEC_ID_PCM_S32BE:
            st->codec->codec_id = CODEC_ID_PCM_S32LE;
            break;
        case CODEC_ID_PCM_F32BE:
            st->codec->codec_id = CODEC_ID_PCM_F32LE;
            break;
        case CODEC_ID_PCM_F64BE:
            st->codec->codec_id = CODEC_ID_PCM_F64LE;
            break;
        default:
            break;
        }
    }
    return 0;
}

/* FIXME modify qdm2/svq3/h264 decoders to take full atom as extradata */
static int mov_read_extradata(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    uint64_t size;
    uint8_t *buf;

    if (c->fc->nb_streams < 1) // will happen with jp2 files
        return 0;
    st= c->fc->streams[c->fc->nb_streams-1];
    size= (uint64_t)st->codec->extradata_size + atom.size + 8 + FF_INPUT_BUFFER_PADDING_SIZE;
    if(size > INT_MAX || (uint64_t)atom.size > INT_MAX)
        return -1;
    buf= av_realloc(st->codec->extradata, size);
    if(!buf)
        return -1;
    st->codec->extradata= buf;
    buf+= st->codec->extradata_size;
    st->codec->extradata_size= size - FF_INPUT_BUFFER_PADDING_SIZE;
    AV_WB32(       buf    , atom.size + 8);
    AV_WL32(       buf + 4, atom.type);
    get_buffer(pb, buf + 8, atom.size);
    return 0;
}

static int mov_read_fiel(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    int fields;
    int detail;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    fields = get_byte(pb);
    detail = get_byte(pb);
    if (fields == 1) {
        st->codec->interlaced = -1; // forced progressive because of dv
    } else if (fields == 2) {
        // quicktime icefloe 019
        st->codec->interlaced = (detail == 9 || detail == 1) + 1;
    }
    return 0;
}

static int mov_read_colr(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    if (get_le32(pb) != AV_RL32("nclc"))
        return 0;

    st->codec->color_primaries = get_be16(pb);
    st->codec->color_transfer = get_be16(pb);
    st->codec->color_matrix = get_be16(pb);
    if (st->codec->color_primaries >= AVCOL_PRI_NB)
        st->codec->color_primaries  = AVCOL_PRI_UNSPECIFIED;
    if (st->codec->color_transfer >= AVCOL_TRC_NB)
        st->codec->color_transfer  = AVCOL_TRC_UNSPECIFIED;
    if (st->codec->color_matrix >= AVCOL_MTX_NB)
        st->codec->color_matrix  = AVCOL_MTX_UNSPECIFIED;

    return 0;
}

/**
 * This function reads atom content and puts data in extradata without tag
 * nor size unlike mov_read_extradata.
 */
static int mov_read_glbl(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    if((uint64_t)atom.size > (1<<30))
        return -1;

    av_free(st->codec->extradata);
    st->codec->extradata = av_mallocz(atom.size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!st->codec->extradata)
        return AVERROR(ENOMEM);
    st->codec->extradata_size = atom.size;
    get_buffer(pb, st->codec->extradata, atom.size);
    return 0;
}

/**
 * An strf atom is a BITMAPINFOHEADER struct. This struct is 40 bytes itself,
 * but can have extradata appended at the end after the 40 bytes belonging
 * to the struct.
 */
static int mov_read_strf(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;

    if (c->fc->nb_streams < 1)
        return 0;
    if (atom.size <= 40)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];

    if((uint64_t)atom.size > (1<<30))
        return -1;

    av_free(st->codec->extradata);
    st->codec->extradata = av_mallocz(atom.size - 40 + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!st->codec->extradata)
        return AVERROR(ENOMEM);
    st->codec->extradata_size = atom.size - 40;
    url_fskip(pb, 40);
    get_buffer(pb, st->codec->extradata, atom.size - 40);
    return 0;
}

static int mov_read_stco(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */

    entries = get_be32(pb);

    if(entries >= UINT_MAX/sizeof(int64_t))
        return -1;

    sc->chunk_offsets = av_malloc(entries * sizeof(int64_t));
    if (!sc->chunk_offsets)
        return AVERROR(ENOMEM);
    sc->chunk_count = entries;

    if      (atom.type == MKTAG('s','t','c','o'))
        for(i=0; i<entries; i++)
            sc->chunk_offsets[i] = get_be32(pb);
    else if (atom.type == MKTAG('c','o','6','4'))
        for(i=0; i<entries; i++)
            sc->chunk_offsets[i] = get_be64(pb);
    else
        return -1;

    return 0;
}

/**
 * Compute codec id for 'lpcm' tag.
 * See CoreAudioTypes and AudioStreamBasicDescription at Apple.
 */
enum CodecID ff_mov_get_lpcm_codec_id(int bps, int flags)
{
    if (flags & 1) { // floating point
        if (flags & 2) { // big endian
            if      (bps == 32) return CODEC_ID_PCM_F32BE;
            else if (bps == 64) return CODEC_ID_PCM_F64BE;
        } else {
            if      (bps == 32) return CODEC_ID_PCM_F32LE;
            else if (bps == 64) return CODEC_ID_PCM_F64LE;
        }
    } else {
        if (flags & 2) {
            if      (bps == 8)
                // signed integer
                if (flags & 4)  return CODEC_ID_PCM_S8;
                else            return CODEC_ID_PCM_U8;
            else if (bps == 16) return CODEC_ID_PCM_S16BE;
            else if (bps == 24) return CODEC_ID_PCM_S24BE;
            else if (bps == 32) return CODEC_ID_PCM_S32BE;
        } else {
            if      (bps == 8)
                if (flags & 4)  return CODEC_ID_PCM_S8;
                else            return CODEC_ID_PCM_U8;
            else if (bps == 16) return CODEC_ID_PCM_S16LE;
            else if (bps == 24) return CODEC_ID_PCM_S24LE;
            else if (bps == 32) return CODEC_ID_PCM_S32LE;
        }
    }
    return CODEC_ID_NONE;
}

int ff_mov_read_stsd_entries(MOVContext *c, ByteIOContext *pb, int entries)
{
    AVStream *st;
    MOVStreamContext *sc;
    int i, j;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    if (entries >= UINT_MAX/sizeof(*sc->dref_ids))
        return -1;
    sc->dref_ids = av_mallocz(entries*sizeof(*sc->dref_ids));
    if (!sc->dref_ids)
        return AVERROR(ENOMEM);
    sc->dref_ids_count = entries;

    for (i = 0; i < entries; i++) {
        //Parsing Sample description table
        enum CodecID id;
        int dref_id = 1;
        int64_t left, start_pos = url_ftell(pb);
        int size = get_be32(pb); /* size */
        uint32_t format = get_le32(pb); /* data format */

        if (size >= 16) {
            get_be32(pb); /* reserved */
            get_be16(pb); /* reserved */
            dref_id = get_be16(pb);
            if (dref_id <= 0) {
                av_log(c->fc, AV_LOG_INFO, "invalid dref id in stsd\n");
                dref_id = 1;
            }
        }

        if (st->codec->codec_tag &&
            st->codec->codec_tag != format &&
            (c->fc->video_codec_id ? ff_codec_get_id(codec_movvideo_tags, format) != c->fc->video_codec_id
                                   : st->codec->codec_tag != MKTAG('j','p','e','g'))
           ){
            /* Multiple fourcc, we skip JPEG. This is not correct, we should
             * export it as a separate AVStream but this needs a few changes
             * in the MOV demuxer, patch welcome. */
        multiple_stsd:
            av_log(c->fc, AV_LOG_WARNING, "multiple fourcc not supported\n");
            goto skip;
        }
        /* we cannot demux concatenated h264 streams because of different extradata */
        if (st->codec->codec_tag && st->codec->codec_tag == AV_RL32("avc1"))
            goto multiple_stsd;
        sc->dref_ids[i] = dref_id;

        st->codec->codec_tag = format;
        id = ff_codec_get_id(codec_movaudio_tags, format);
        if (id<=0 && ((format&0xFFFF) == 'm'+('s'<<8) || (format&0xFFFF) == 'T'+('S'<<8)))
            id = ff_codec_get_id(ff_codec_wav_tags, av_bswap32(format)&0xFFFF);

        if (st->codec->codec_type != AVMEDIA_TYPE_VIDEO && id > 0) {
            st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
        } else if (st->codec->codec_type != AVMEDIA_TYPE_AUDIO && /* do not overwrite codec type */
                   format && format != MKTAG('m','p','4','s')) { /* skip old asf mpeg4 tag */
            id = ff_codec_get_id(codec_movvideo_tags, format);
            if (id <= 0)
                id = ff_codec_get_id(ff_codec_bmp_tags, format);
            if (id > 0)
                st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
            else if(st->codec->codec_type == AVMEDIA_TYPE_DATA){
                id = ff_codec_get_id(ff_codec_movsubtitle_tags, format);
                if(id > 0)
                    st->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;
            }
        }

        av_dlog(c->fc, "size=%d 4CC= %c%c%c%c codec_type=%d\n", size,
                (format >> 0) & 0xff, (format >> 8) & 0xff, (format >> 16) & 0xff,
                (format >> 24) & 0xff, st->codec->codec_type);

        if(st->codec->codec_type==AVMEDIA_TYPE_VIDEO) {
            unsigned int color_depth, len;
            int color_greyscale;

            st->codec->codec_id = id;
            get_be16(pb); /* version */
            get_be16(pb); /* revision level */
            get_be32(pb); /* vendor */
            get_be32(pb); /* temporal quality */
            get_be32(pb); /* spatial quality */

            st->codec->width = get_be16(pb); /* width */
            st->codec->height = get_be16(pb); /* height */

            get_be32(pb); /* horiz resolution */
            get_be32(pb); /* vert resolution */
            get_be32(pb); /* data size, always 0 */
            get_be16(pb); /* frames per samples */

            len = get_byte(pb); /* codec name, pascal string */
            if (len > 31)
                len = 31;
            mov_read_mac_string(c, pb, len, st->codec->codec_name, 32);
            if (len < 31)
                url_fskip(pb, 31 - len);
            /* codec_tag YV12 triggers an UV swap in rawdec.c */
            if (!memcmp(st->codec->codec_name, "Planar Y'CbCr 8-bit 4:2:0", 25))
                st->codec->codec_tag=MKTAG('I', '4', '2', '0');
            av_metadata_set(&st->metadata, "codec_name", st->codec->codec_name);

            st->codec->bits_per_coded_sample = get_be16(pb); /* depth */
            st->codec->color_table_id = get_be16(pb); /* colortable id */
            av_dlog(c->fc, "depth %d, ctab id %d\n",
                   st->codec->bits_per_coded_sample, st->codec->color_table_id);
            /* figure out the palette situation */
            color_depth = st->codec->bits_per_coded_sample & 0x1F;
            color_greyscale = st->codec->bits_per_coded_sample & 0x20;

            /* if the depth is 2, 4, or 8 bpp, file is palettized */
            if ((color_depth == 2) || (color_depth == 4) ||
                (color_depth == 8)) {
                /* for palette traversal */
                unsigned int color_start, color_count, color_end;
                unsigned char r, g, b;

                st->codec->palctrl = av_malloc(sizeof(*st->codec->palctrl));
                if (color_greyscale) {
                    int color_index, color_dec;
                    /* compute the greyscale palette */
                    st->codec->bits_per_coded_sample = color_depth;
                    color_count = 1 << color_depth;
                    color_index = 255;
                    color_dec = 256 / (color_count - 1);
                    for (j = 0; j < color_count; j++) {
                        r = g = b = color_index;
                        st->codec->palctrl->palette[j] =
                            (r << 16) | (g << 8) | (b);
                        color_index -= color_dec;
                        if (color_index < 0)
                            color_index = 0;
                    }
                } else if (st->codec->color_table_id) {
                    const uint8_t *color_table;
                    /* if flag bit 3 is set, use the default palette */
                    color_count = 1 << color_depth;
                    if (color_depth == 2)
                        color_table = ff_qt_default_palette_4;
                    else if (color_depth == 4)
                        color_table = ff_qt_default_palette_16;
                    else
                        color_table = ff_qt_default_palette_256;

                    for (j = 0; j < color_count; j++) {
                        r = color_table[j * 3 + 0];
                        g = color_table[j * 3 + 1];
                        b = color_table[j * 3 + 2];
                        st->codec->palctrl->palette[j] =
                            (r << 16) | (g << 8) | (b);
                    }
                } else {
                    /* load the palette from the file */
                    color_start = get_be32(pb);
                    color_count = get_be16(pb);
                    color_end = get_be16(pb);
                    if ((color_start <= 255) &&
                        (color_end <= 255)) {
                        for (j = color_start; j <= color_end; j++) {
                            /* each R, G, or B component is 16 bits;
                             * only use the top 8 bits; skip alpha bytes
                             * up front */
                            get_byte(pb);
                            get_byte(pb);
                            r = get_byte(pb);
                            get_byte(pb);
                            g = get_byte(pb);
                            get_byte(pb);
                            b = get_byte(pb);
                            get_byte(pb);
                            st->codec->palctrl->palette[j] =
                                (r << 16) | (g << 8) | (b);
                        }
                    }
                }
                st->codec->palctrl->palette_changed = 1;
            }
        } else if(st->codec->codec_type==AVMEDIA_TYPE_AUDIO) {
            int bits_per_sample, flags;
            uint16_t version = get_be16(pb);

            st->codec->codec_id = id;
            get_be16(pb); /* revision level */
            get_be32(pb); /* vendor */

            st->codec->channels = get_be16(pb);             /* channel count */
            av_dlog(c->fc, "audio channels %d\n", st->codec->channels);
            st->codec->bits_per_coded_sample = get_be16(pb);      /* sample size */

            sc->audio_cid = get_be16(pb);
            get_be16(pb); /* packet size = 0 */

            st->codec->sample_rate = ((get_be32(pb) >> 16));

            //Read QT version 1 fields. In version 0 these do not exist.
            av_dlog(c->fc, "version =%d, isom =%d\n",version,c->isom);
            if(!c->isom) {
                if(version==1) {
                    sc->samples_per_frame = get_be32(pb);
                    get_be32(pb); /* bytes per packet */
                    sc->bytes_per_frame = get_be32(pb);
                    get_be32(pb); /* bytes per sample */
                } else if(version==2) {
                    get_be32(pb); /* sizeof struct only */
                    st->codec->sample_rate = av_int2dbl(get_be64(pb)); /* float 64 */
                    st->codec->channels = get_be32(pb);
                    get_be32(pb); /* always 0x7F000000 */
                    st->codec->bits_per_coded_sample = get_be32(pb); /* bits per channel if sound is uncompressed */
                    flags = get_be32(pb); /* lpcm format specific flag */
                    sc->bytes_per_frame = get_be32(pb); /* bytes per audio packet if constant */
                    sc->samples_per_frame = get_be32(pb); /* lpcm frames per audio packet if constant */
                    if (format == MKTAG('l','p','c','m'))
                        st->codec->codec_id = ff_mov_get_lpcm_codec_id(st->codec->bits_per_coded_sample, flags);
                }
            }

            switch (st->codec->codec_id) {
            case CODEC_ID_PCM_S8:
            case CODEC_ID_PCM_U8:
                if (st->codec->bits_per_coded_sample == 16)
                    st->codec->codec_id = CODEC_ID_PCM_S16BE;
                break;
            case CODEC_ID_PCM_S16LE:
            case CODEC_ID_PCM_S16BE:
                if (st->codec->bits_per_coded_sample == 8)
                    st->codec->codec_id = CODEC_ID_PCM_S8;
                else if (st->codec->bits_per_coded_sample == 24)
                    st->codec->codec_id =
                        st->codec->codec_id == CODEC_ID_PCM_S16BE ?
                        CODEC_ID_PCM_S24BE : CODEC_ID_PCM_S24LE;
                break;
            /* set values for old format before stsd version 1 appeared */
            case CODEC_ID_MACE3:
                sc->samples_per_frame = 6;
                sc->bytes_per_frame = 2*st->codec->channels;
                break;
            case CODEC_ID_MACE6:
                sc->samples_per_frame = 6;
                sc->bytes_per_frame = 1*st->codec->channels;
                break;
            case CODEC_ID_ADPCM_IMA_QT:
                sc->samples_per_frame = 64;
                sc->bytes_per_frame = 34*st->codec->channels;
                break;
            case CODEC_ID_GSM:
                sc->samples_per_frame = 160;
                sc->bytes_per_frame = 33;
                break;
            default:
                break;
            }

            bits_per_sample = av_get_bits_per_sample(st->codec->codec_id);
            if (bits_per_sample) {
                st->codec->bits_per_coded_sample = bits_per_sample;
                sc->sample_size = (bits_per_sample >> 3) * st->codec->channels;
            }
        } else if (st->codec->codec_tag == MKTAG('t','m','c','d')) {
            int val, left;
            get_be32(pb); /* reserved */
            val = get_be32(pb); /* flags */
            if (val & 1)
                st->codec->flags2 |= CODEC_FLAG2_DROP_FRAME_TIMECODE;
            val = get_be32(pb);
            av_dlog(c->fc, "val %d\n", val);
            val = get_be32(pb);
            av_dlog(c->fc, "val %d\n", val);
            st->codec->time_base.den = get_byte(pb);
            st->codec->time_base.num = 1;
            av_dlog(c->fc, "tbc %d/%d\n", st->codec->time_base.num,
                    st->codec->time_base.den);
            get_byte(pb);
            left = size - (url_ftell(pb) - start_pos);
            if (left > 8)
                mov_read_default(c, pb, (MOVAtom){ AV_RL32("udta"), left });
        } else if(st->codec->codec_type==AVMEDIA_TYPE_SUBTITLE){
            // ttxt stsd contains display flags, justification, background
            // color, fonts, and default styles, so fake an atom to read it
            MOVAtom fake_atom = { .size = size - (url_ftell(pb) - start_pos) };
            if (format != AV_RL32("mp4s")) // mp4s contains a regular esds atom
                mov_read_glbl(c, pb, fake_atom);
            st->codec->codec_id= id;
            st->codec->width = sc->width;
            st->codec->height = sc->height;
        } else {
            goto skip;
        }
        /* this will read extra atoms at the end (wave, alac, damr, avcC, SMI ...) */
        left = size - (url_ftell(pb) - start_pos);
        if (left > 8)
            if (mov_read_default(c, pb, (MOVAtom){ AV_RL32("stsd"), left }) < 0)
                return -1;
    skip:
        left = size - (url_ftell(pb) - start_pos);
        if (left < 0) {
            av_log(c->fc, AV_LOG_WARNING, "stsd entry '%.4s' left %"PRId64"\n",
                   (char*)&format, left);
        } else if (left > 0) {
            av_dlog(c->fc, "stsd entry '%.4s' left %"PRId64"\n",
                    (char*)&format, left);
            url_fskip(pb, left);
        }
    }

    if(st->codec->codec_type==AVMEDIA_TYPE_AUDIO && st->codec->sample_rate==0 && sc->time_scale>1)
        st->codec->sample_rate= sc->time_scale;

    /* special codec parameters handling */
    switch (st->codec->codec_id) {
#if CONFIG_DV_DEMUXER
    case CODEC_ID_DVAUDIO:
        c->dv_fctx = avformat_alloc_context();
        c->dv_demux = dv_init_demux(c->dv_fctx);
        if (!c->dv_demux) {
            av_log(c->fc, AV_LOG_ERROR, "dv demux context init error\n");
            return -1;
        }
        sc->dv_audio_container = 1;
        st->codec->codec_id = CODEC_ID_PCM_S16LE;
        break;
#endif
    /* no ifdef since parameters are always those */
    case CODEC_ID_QCELP:
        // force sample rate for qcelp when not stored in mov
        if (st->codec->codec_tag != MKTAG('Q','c','l','p'))
            st->codec->sample_rate = 8000;
        st->codec->frame_size= 160;
        st->codec->channels= 1; /* really needed */
        break;
    case CODEC_ID_AMR_NB:
        st->codec->frame_size = 160;
        st->codec->channels = 1; /* really needed */
        /* force sample rate for amr, stsd in 3gp does not store sample rate */
        st->codec->sample_rate = 8000;
        break;
    case CODEC_ID_AMR_WB:
        st->codec->frame_size = 320;
        st->codec->channels= 1; /* really needed */
        /* force sample rate for amr, stsd in 3gp does not store sample rate */
        st->codec->sample_rate = 16000;
        break;
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
        st->codec->codec_type = AVMEDIA_TYPE_AUDIO; /* force type after stsd for m1a hdlr */
        st->need_parsing = AVSTREAM_PARSE_FULL;
        break;
    case CODEC_ID_QDM2:
    case CODEC_ID_GSM:
    case CODEC_ID_ADPCM_MS:
    case CODEC_ID_ADPCM_IMA_WAV:
        st->codec->frame_size = sc->samples_per_frame;
        st->codec->block_align = sc->bytes_per_frame;
        break;
    case CODEC_ID_ALAC:
        if (st->codec->extradata_size == 36) {
            st->codec->frame_size = AV_RB32(st->codec->extradata+12);
            st->codec->channels   = AV_RB8 (st->codec->extradata+21);
            st->codec->sample_rate = AV_RB32(st->codec->extradata+32);
        }
        break;
    default:
        break;
    }

    return 0;
}

static int mov_read_stsd(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int entries;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */
    entries = get_be32(pb);

    return ff_mov_read_stsd_entries(c, pb, entries);
}

static int mov_read_stsc(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */

    entries = get_be32(pb);

    av_dlog(c->fc, "track[%i].stsc.entries = %i\n", c->fc->nb_streams-1, entries);

    if(entries >= UINT_MAX / sizeof(*sc->stsc_data))
        return -1;
    sc->stsc_data = av_malloc(entries * sizeof(*sc->stsc_data));
    if (!sc->stsc_data)
        return AVERROR(ENOMEM);
    sc->stsc_count = entries;

    for(i=0; i<entries; i++) {
        sc->stsc_data[i].first = get_be32(pb);
        sc->stsc_data[i].count = get_be32(pb);
        sc->stsc_data[i].id = get_be32(pb);
    }
    return 0;
}

static int mov_read_stps(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned i, entries;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_be32(pb); // version + flags

    entries = get_be32(pb);
    if (entries >= UINT_MAX / sizeof(*sc->stps_data))
        return -1;
    sc->stps_data = av_malloc(entries * sizeof(*sc->stps_data));
    if (!sc->stps_data)
        return AVERROR(ENOMEM);
    sc->stps_count = entries;

    for (i = 0; i < entries; i++) {
        sc->stps_data[i] = get_be32(pb);
        //av_dlog(c->fc, "stps %d\n", sc->stps_data[i]);
    }

    return 0;
}

static int mov_read_stss(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */

    entries = get_be32(pb);

    av_dlog(c->fc, "keyframe_count = %d\n", entries);

    if(entries >= UINT_MAX / sizeof(int))
        return -1;
    sc->keyframes = av_malloc(entries * sizeof(int));
    if (!sc->keyframes)
        return AVERROR(ENOMEM);
    sc->keyframe_count = entries;

    for(i=0; i<entries; i++) {
        sc->keyframes[i] = get_be32(pb);
        //av_dlog(c->fc, "keyframes[]=%d\n", sc->keyframes[i]);
    }
    return 0;
}

static int mov_read_stsz(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries, sample_size, field_size, num_bytes;
    GetBitContext gb;
    unsigned char* buf;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */

    if (atom.type == MKTAG('s','t','s','z')) {
        sample_size = get_be32(pb);
        if (!sc->sample_size) /* do not overwrite value computed in stsd */
            sc->sample_size = sample_size;
        field_size = 32;
    } else {
        sample_size = 0;
        get_be24(pb); /* reserved */
        field_size = get_byte(pb);
    }
    entries = get_be32(pb);

    av_dlog(c->fc, "sample_size = %d sample_count = %d\n", sc->sample_size, entries);

    sc->sample_count = entries;
    if (sample_size)
        return 0;

    if (field_size != 4 && field_size != 8 && field_size != 16 && field_size != 32) {
        av_log(c->fc, AV_LOG_ERROR, "Invalid sample field size %d\n", field_size);
        return -1;
    }

    if (entries >= UINT_MAX / sizeof(int) || entries >= (UINT_MAX - 4) / field_size)
        return -1;
    sc->sample_sizes = av_malloc(entries * sizeof(int));
    if (!sc->sample_sizes)
        return AVERROR(ENOMEM);

    num_bytes = (entries*field_size+4)>>3;

    buf = av_malloc(num_bytes+FF_INPUT_BUFFER_PADDING_SIZE);
    if (!buf) {
        av_freep(&sc->sample_sizes);
        return AVERROR(ENOMEM);
    }

    if (get_buffer(pb, buf, num_bytes) < num_bytes) {
        av_freep(&sc->sample_sizes);
        av_free(buf);
        return -1;
    }

    init_get_bits(&gb, buf, 8*num_bytes);

    for(i=0; i<entries; i++)
        sc->sample_sizes[i] = get_bits_long(&gb, field_size);

    av_free(buf);
    return 0;
}

static int mov_read_stts(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries;
    int64_t duration=0;
    int64_t total_sample_count=0;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */
    entries = get_be32(pb);

    av_dlog(c->fc, "track[%i].stts.entries = %i\n", c->fc->nb_streams-1, entries);

    if(entries >= UINT_MAX / sizeof(*sc->stts_data))
        return -1;
    sc->stts_data = av_malloc(entries * sizeof(*sc->stts_data));
    if (!sc->stts_data)
        return AVERROR(ENOMEM);
    sc->stts_count = entries;

    for(i=0; i<entries; i++) {
        int sample_duration;
        int sample_count;

        sample_count=get_be32(pb);
        sample_duration = get_be32(pb);
        sc->stts_data[i].count= sample_count;
        sc->stts_data[i].duration= sample_duration;

        av_dlog(c->fc, "sample_count=%d, sample_duration=%d\n",sample_count,sample_duration);

        duration+=(int64_t)sample_duration*sample_count;
        total_sample_count+=sample_count;
    }

    st->nb_frames= total_sample_count;
    if(duration)
        st->duration= duration;
    return 0;
}

static int mov_read_cslg(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    av_unused int tmp;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_be32(pb); // version + flags

    tmp = get_be32(pb);
    av_dlog(c->fc, "dts shift %d\n", tmp);
    tmp = get_be32(pb); // least dts to pts delta
    av_dlog(c->fc, "least cts %d\n", tmp);
    tmp = get_be32(pb); // greatest dts to pts delta
    av_dlog(c->fc, "greatest cts %d\n", tmp);
    tmp = get_be32(pb); // pts start
    av_dlog(c->fc, "pts start %d\n", tmp);
    tmp = get_be32(pb); // pts end
    av_dlog(c->fc, "pts end %d\n", tmp);

    return 0;
}

static int mov_read_ctts(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    unsigned int i, entries;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    get_byte(pb); /* version */
    get_be24(pb); /* flags */
    entries = get_be32(pb);

    av_dlog(c->fc, "track[%i].ctts.entries = %i\n", c->fc->nb_streams-1, entries);

    if(entries >= UINT_MAX / sizeof(*sc->ctts_data))
        return -1;
    sc->ctts_data = av_malloc(entries * sizeof(*sc->ctts_data));
    if (!sc->ctts_data)
        return AVERROR(ENOMEM);
    sc->ctts_count = entries;

    for(i=0; i<entries; i++) {
        int count    =get_be32(pb);
        int duration =get_be32(pb);

        sc->ctts_data[i].count   = count;
        sc->ctts_data[i].duration= duration;
        if (duration < 0)
            sc->dts_shift = FFMAX(sc->dts_shift, -duration);
    }

    av_dlog(c->fc, "dts shift %d\n", sc->dts_shift);

    return 0;
}

static void mov_compute_stream_time_offset(MOVContext *mov, AVStream *st)
{
    MOVStreamContext *sc = st->priv_data;
    int i;

    // adjust first dts according to edit list
    for (i = 0; i < sc->elst_count; i++) {
        if (sc->elst_data[i].time == -1) {
            if (i > 0)
                goto warning;
            sc->time_offset -= av_rescale(sc->elst_data[i].duration,
                                          sc->time_scale, mov->time_scale);
        } else if (i <= 1) {
            sc->time_offset += sc->elst_data[i].time;
        } else {
            goto warning;
        }
    }

    // shorten the duration of the first video or timecode sample
    if (st->codec->codec_type != CODEC_TYPE_AUDIO &&
        sc->time_offset > 0 && sc->stts_count > 0 &&
        sc->stts_data[0].count == 1 &&
        sc->stts_data[0].duration > sc->time_offset) {
        sc->stts_data[0].duration -= sc->time_offset;
        sc->time_offset = 0;
    }

    return;

 warning:
    av_log(mov, AV_LOG_WARNING, "multiple edit list entries, "
           "a/v desync might occur, patch welcome\n");
}

static void mov_build_index(MOVContext *mov, AVStream *st)
{
    MOVStreamContext *sc = st->priv_data;
    int64_t current_offset;
    int64_t current_dts = 0;
    unsigned int stts_index = 0;
    unsigned int stsc_index = 0;
    unsigned int stss_index = 0;
    unsigned int stps_index = 0;
    unsigned int i, j;
    uint64_t stream_size = 0;

    mov_compute_stream_time_offset(mov, st);
    current_dts = -sc->time_offset;

    /* only use old uncompressed audio chunk demuxing when stts specifies it */
    if (!(st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
          sc->stts_count == 1 && sc->stts_data[0].duration == 1)) {
        unsigned int current_sample = 0;
        unsigned int stts_sample = 0;
        unsigned int sample_size;
        unsigned int distance = 0;
        int key_off = sc->keyframes && sc->keyframes[0] == 1;

        current_dts -= sc->dts_shift;

        if (sc->sample_count >= UINT_MAX / sizeof(*st->index_entries))
            return;
        st->index_entries = av_malloc(sc->sample_count*sizeof(*st->index_entries));
        if (!st->index_entries)
            return;
        st->index_entries_allocated_size = sc->sample_count*sizeof(*st->index_entries);
        sc->sample_dref = av_mallocz(sc->sample_count*sizeof(*sc->sample_dref));
        if (!sc->sample_dref)
            return;

        for (i = 0; i < sc->chunk_count; i++) {
            current_offset = sc->chunk_offsets[i];
            if (stsc_index + 1 < sc->stsc_count &&
                i + 1 == sc->stsc_data[stsc_index + 1].first)
                stsc_index++;
            for (j = 0; j < sc->stsc_data[stsc_index].count; j++) {
                int keyframe = 0;
                if (current_sample >= sc->sample_count) {
                    av_log(mov->fc, AV_LOG_ERROR, "wrong sample count\n");
                    return;
                }

                if (!sc->keyframe_count || current_sample+key_off == sc->keyframes[stss_index]) {
                    keyframe = 1;
                    if (stss_index + 1 < sc->keyframe_count)
                        stss_index++;
                } else if (sc->stps_count && current_sample+key_off == sc->stps_data[stps_index]) {
                    keyframe = 1;
                    if (stps_index + 1 < sc->stps_count)
                        stps_index++;
                }
                if (keyframe)
                    distance = 0;
                sample_size = sc->sample_size > 0 ? sc->sample_size : sc->sample_sizes[current_sample];
                if (sc->stsc_data[stsc_index].id - 1 < sc->dref_ids_count &&
                    sc->dref_ids[sc->stsc_data[stsc_index].id - 1] - 1 < sc->drefs_count) {
                    AVIndexEntry *e = &st->index_entries[st->nb_index_entries++];
                    e->pos = current_offset;
                    e->timestamp = current_dts;
                    e->size = sample_size;
                    e->min_distance = distance;
                    e->flags = keyframe ? AVINDEX_KEYFRAME : 0;
                    //av_dlog(mov->fc, "AVIndex stream %d, sample %d, offset %"PRIx64", dts %"PRId64", "
                    //        "size %d, distance %d, keyframe %d\n", st->index, current_sample,
                    //        current_offset, current_dts, sample_size, distance, keyframe);
                    sc->sample_dref[current_sample] =
                        sc->drefs[sc->dref_ids[sc->stsc_data[stsc_index].id - 1] - 1].pb;
                }

                current_offset += sample_size;
                stream_size += sample_size;
                current_dts += sc->stts_data[stts_index].duration;
                distance++;
                stts_sample++;
                current_sample++;
                if (stts_index + 1 < sc->stts_count && stts_sample == sc->stts_data[stts_index].count) {
                    stts_sample = 0;
                    stts_index++;
                }
            }
        }
        if (st->duration > 0)
            st->codec->bit_rate = stream_size*8*sc->time_scale/st->duration;
    } else {
        unsigned chunk_samples, total = 0;

        // compute total chunk count
        for (i = 0; i < sc->stsc_count; i++) {
            unsigned count, chunk_count;

            chunk_samples = sc->stsc_data[i].count;
            if (sc->samples_per_frame && chunk_samples % sc->samples_per_frame) {
                av_log(mov->fc, AV_LOG_ERROR, "error unaligned chunk\n");
                return;
            }

            if (sc->samples_per_frame >= 160) { // gsm
                count = chunk_samples / sc->samples_per_frame;
            } else if (sc->samples_per_frame > 1) {
                unsigned samples = (1024/sc->samples_per_frame)*sc->samples_per_frame;
                count = (chunk_samples+samples-1) / samples;
            } else {
                count = (chunk_samples+1023) / 1024;
            }

            if (i < sc->stsc_count - 1)
                chunk_count = sc->stsc_data[i+1].first - sc->stsc_data[i].first;
            else
                chunk_count = sc->chunk_count - (sc->stsc_data[i].first - 1);
            total += chunk_count * count;
        }

        av_dlog(mov->fc, "chunk count %d\n", total);
        if (total >= UINT_MAX / sizeof(*st->index_entries))
            return;
        st->index_entries = av_malloc(total*sizeof(*st->index_entries));
        if (!st->index_entries)
            return;
        st->index_entries_allocated_size = total*sizeof(*st->index_entries);
        sc->sample_dref = av_malloc(total*sizeof(*sc->sample_dref));
        if (!sc->sample_dref)
            return;

        // populate index
        for (i = 0; i < sc->chunk_count; i++) {
            current_offset = sc->chunk_offsets[i];
            if (stsc_index + 1 < sc->stsc_count &&
                i + 1 == sc->stsc_data[stsc_index + 1].first)
                stsc_index++;
            chunk_samples = sc->stsc_data[stsc_index].count;

            while (chunk_samples > 0) {
                AVIndexEntry *e;
                unsigned size, samples;

                if (sc->samples_per_frame >= 160) { // gsm
                    samples = sc->samples_per_frame;
                    size = sc->bytes_per_frame;
                } else {
                    if (sc->samples_per_frame > 1) {
                        samples = FFMIN((1920 / sc->samples_per_frame)*
                                        sc->samples_per_frame, chunk_samples);
                        size = (samples / sc->samples_per_frame) * sc->bytes_per_frame;
                    } else {
                        samples = FFMIN(1920, chunk_samples);
                        size = samples * sc->sample_size;
                    }
                }

                if (st->nb_index_entries >= total) {
                    av_log(mov->fc, AV_LOG_ERROR, "wrong chunk count %d\n", total);
                    return;
                }

                if (sc->stsc_data[stsc_index].id - 1 >= sc->dref_ids_count ||
                    sc->dref_ids[sc->stsc_data[stsc_index].id - 1] - 1 >= sc->drefs_count) {
                    av_log(mov->fc, AV_LOG_ERROR, "wrong stsc id\n");
                    return;
                }

                sc->sample_dref[st->nb_index_entries] =
                    sc->drefs[sc->dref_ids[sc->stsc_data[stsc_index].id - 1] - 1].pb;

                e = &st->index_entries[st->nb_index_entries++];
                e->pos = current_offset;
                e->timestamp = current_dts;
                e->size = size;
                e->min_distance = 0;
                e->flags = AVINDEX_KEYFRAME;
                //av_dlog(mov->fc, "AVIndex stream %d, chunk %d, offset %"PRIx64", dts %"PRId64", "
                //        "size %d, duration %d\n", st->index, i, current_offset, current_dts,
                //        size, samples);

                current_offset += size;
                current_dts += samples;
                chunk_samples -= samples;
            }
        }
    }
}

static int mov_open_dref(MOVContext *mov, MOVDref *dref, char *src)
{
    /* try relative path, we do not try the absolute because it can leak information about our
       system to an attacker */
    char filename[1024];
    char *src_path;
    int i, l;

    /* find a source dir */
    src_path = strrchr(src, '/');
    if (src_path)
        src_path++;
    else
        src_path = src;

    /* find a next level down to target */
    for (i = 0, l = strlen(dref->path) - 1; l >= 0; l--)
        if (dref->path[l] == '/') {
            if (i == dref->nlvl_to - 1)
                break;
            else
                i++;
        }

    /* compose filename if next level down to target was found */
    if (i == dref->nlvl_to - 1 && src_path - src  < sizeof(filename)) {
        memcpy(filename, src, src_path - src);
        filename[src_path - src] = 0;

        for (i = 1; i < dref->nlvl_from; i++)
            av_strlcat(filename, "../", 1024);

        av_strlcat(filename, dref->path + l + 1, 1024);

        av_log(mov->fc, AV_LOG_DEBUG, "trying dref %s\n", filename);

        if (!url_fopen(&dref->pb, filename, URL_RDONLY))
            return 0;
    }

    if (dref->filename) {
        av_log(mov->fc, AV_LOG_DEBUG, "trying dref %s\n", dref->filename);
        if (!url_fopen(&dref->pb, dref->filename, URL_RDONLY))
            return 0;
    }

    return AVERROR(ENOENT);
}

static int mov_read_trak(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    MOVStreamContext *sc;
    int ret, i;

    st = av_new_stream(c->fc, c->fc->nb_streams);
    if (!st) return AVERROR(ENOMEM);
    sc = av_mallocz(sizeof(MOVStreamContext));
    if (!sc) return AVERROR(ENOMEM);

    st->priv_data = sc;
    st->codec->codec_type = AVMEDIA_TYPE_DATA;
    sc->ffindex = st->index;

    c->metadata = &st->metadata;
    if ((ret = mov_read_default(c, pb, atom)) < 0)
        return ret;
    c->metadata = &c->fc->metadata;

    /* sanity checks */
    if (sc->chunk_count && (!sc->stts_count || !sc->stsc_count ||
                            (!sc->sample_size && !sc->sample_count))) {
        av_log(c->fc, AV_LOG_ERROR, "stream %d, missing mandatory atoms, broken header\n",
               st->index);
        return 0;
    }

    if (sc->time_scale <= 0) {
        av_log(c->fc, AV_LOG_WARNING, "stream %d, timescale not set\n", st->index);
        sc->time_scale = c->time_scale;
        if (sc->time_scale <= 0)
            sc->time_scale = 1;
    }

    av_set_pts_info(st, 64, 1, sc->time_scale);

    if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
        !st->codec->frame_size && sc->stts_count == 1) {
        st->codec->frame_size = av_rescale(sc->stts_data[0].duration,
                                           st->codec->sample_rate, sc->time_scale);
        av_dlog(c->fc, "frame size %d\n", st->codec->frame_size);
    }

    for (i = 0; i < sc->drefs_count; i++) {
        MOVDref *dref = &sc->drefs[i];
        if (sc->drefs[i].path) {
            if (mov_open_dref(c, dref, c->fc->filename) < 0) {
                av_log(c->fc, AV_LOG_ERROR,
                       "stream %d, error opening alias: path='%s', dir='%s', "
                       "filename='%s', volume='%s', nlvl_from=%d, nlvl_to=%d\n",
                       st->index, dref->path, dref->dir, dref->filename,
                       dref->volume, dref->nlvl_from, dref->nlvl_to);
                return AVERROR(EIO);
            }
        } else {
            dref->pb = c->fc->pb;
        }
    }

    mov_build_index(c, st);

    if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        av_reduce(&st->avg_frame_rate.num, &st->avg_frame_rate.den,
                  sc->time_scale*st->nb_frames, st->duration, INT_MAX);

        if (sc->stts_count == 1 || (sc->stts_count == 2 && sc->stts_data[1].count == 1))
            av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den,
                      sc->time_scale, sc->stts_data[0].duration, INT_MAX);

        // tkhd with matrix will set it
        if (!st->sample_aspect_ratio.num) {
            if (sc->width != st->codec->width || sc->height != st->codec->height) {
                // tkhd width/height is different than stsd
                st->sample_aspect_ratio =
                    av_div_q((AVRational){sc->width, sc->height},
                             (AVRational){st->codec->width, st->codec->height});
            } else if (sc->pixel_aspect.den && sc->pixel_aspect.num) {
                // pasp
                st->sample_aspect_ratio = sc->pixel_aspect;
            }
        }
    }

    /* Do not need those anymore. */
    av_freep(&sc->chunk_offsets);
    av_freep(&sc->stsc_data);
    av_freep(&sc->sample_sizes);
    av_freep(&sc->keyframes);
    av_freep(&sc->stts_data);
    av_freep(&sc->stps_data);
    av_freep(&sc->elst_data);

    return 0;
}

static int mov_read_ilst(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int ret;
    c->itunes_metadata = 1;
    ret = mov_read_default(c, pb, atom);
    c->itunes_metadata = 0;
    return ret;
}

static int mov_read_meta(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    while (atom.size > 8) {
        uint32_t tag = get_le32(pb);
        atom.size -= 4;
        if (tag == MKTAG('h','d','l','r')) {
            url_fseek(pb, -8, SEEK_CUR);
            atom.size += 8;
            return mov_read_default(c, pb, atom);
        }
    }
    return 0;
}

static int mov_read_tkhd(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int i;
    int width;
    int height;
    int64_t disp_transform[2];
    int display_matrix[3][3];
    AVStream *st;
    MOVStreamContext *sc;
    int version;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams-1];
    sc = st->priv_data;

    version = get_byte(pb);
    get_be24(pb); /* flags */
    /*
    MOV_TRACK_ENABLED 0x0001
    MOV_TRACK_IN_MOVIE 0x0002
    MOV_TRACK_IN_PREVIEW 0x0004
    MOV_TRACK_IN_POSTER 0x0008
    */

    if (version == 1) {
        get_be64(pb);
        get_be64(pb);
    } else {
        get_be32(pb); /* creation time */
        get_be32(pb); /* modification time */
    }
    st->id = (int)get_be32(pb); /* track id (NOT 0 !)*/
    get_be32(pb); /* reserved */

    /* highlevel (considering edits) duration in movie timebase */
    (version == 1) ? get_be64(pb) : get_be32(pb);
    get_be32(pb); /* reserved */
    get_be32(pb); /* reserved */

    get_be16(pb); /* layer */
    get_be16(pb); /* alternate group */
    get_be16(pb); /* volume */
    get_be16(pb); /* reserved */

    //read in the display matrix (outlined in ISO 14496-12, Section 6.2.2)
    // they're kept in fixed point format through all calculations
    for (i = 0; i < 3; i++) {
        display_matrix[i][0] = get_be32(pb); // 16.16 fixed point
        display_matrix[i][1] = get_be32(pb); // 16.16 fixed point
        display_matrix[i][2] = get_be32(pb); // 2.30 fixed point
        //av_log(c->fc, AV_LOG_INFO, "%d %d %d %d\n", st->index,
        //       display_matrix[i][0], display_matrix[i][1], display_matrix[i][2]);
    }

    width = get_be32(pb);       // 16.16 fixed point track width
    height = get_be32(pb);      // 16.16 fixed point track height
    sc->width = width >> 16;
    sc->height = height >> 16;

    // transform the display width/height according to the matrix
    // skip this if the display matrix is the default identity matrix
    // or if it is rotating the picture, ex iPhone 3GS
    // to keep the same scale, use [width height 1<<16]
    if (width && height) {
        if ((display_matrix[0][0] != 65536  ||
             display_matrix[1][1] != 65536) &&
            !display_matrix[0][1] &&
            !display_matrix[1][0] &&
            !display_matrix[2][0] && !display_matrix[2][1]) {
            for (i = 0; i < 2; i++)
                disp_transform[i] =
                    (int64_t) width  * display_matrix[0][i] +
                    (int64_t) height * display_matrix[1][i] +
                    ((int64_t) display_matrix[2][i] << 16);

            //sample aspect ratio is new width/height divided by old width/height
            st->sample_aspect_ratio = av_d2q(
            ((double) disp_transform[0] * height) /
            ((double) disp_transform[1] * width), INT_MAX);
        }
        if (display_matrix[0][0] ==  0     && display_matrix[0][1] == 1<<16 && display_matrix[0][2] == 0 &&
            display_matrix[1][0] == -1<<16 && display_matrix[1][1] == 0     && display_matrix[1][2] == 0 &&
            (display_matrix[2][0] == width || display_matrix[2][0] == height) && display_matrix[2][1] == 0 &&
            display_matrix[2][2] == 1<<30)
            av_metadata_set_int(&st->metadata, "rotate", 90);
    }

    return 0;
}

static int mov_read_tfhd(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    MOVFragment *frag = &c->fragment;
    MOVTrackExt *trex = NULL;
    int flags, track_id, i;

    get_byte(pb); /* version */
    flags = get_be24(pb);

    track_id = get_be32(pb);
    if (!track_id)
        return -1;
    frag->track_id = track_id;
    for (i = 0; i < c->trex_count; i++)
        if (c->trex_data[i].track_id == frag->track_id) {
            trex = &c->trex_data[i];
            break;
        }
    if (!trex) {
        av_log(c->fc, AV_LOG_ERROR, "could not find corresponding trex\n");
        return -1;
    }

    if (flags & 0x01) frag->base_data_offset = get_be64(pb);
    else              frag->base_data_offset = frag->moof_offset;
    if (flags & 0x02) frag->stsd_id          = get_be32(pb);
    else              frag->stsd_id          = trex->stsd_id;

    frag->duration = flags & 0x08 ? get_be32(pb) : trex->duration;
    frag->size     = flags & 0x10 ? get_be32(pb) : trex->size;
    frag->flags    = flags & 0x20 ? get_be32(pb) : trex->flags;
    av_dlog(c->fc, "frag flags 0x%x\n", frag->flags);
    return 0;
}

static int mov_read_chap(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    c->chapter_track = get_be32(pb);
    return 0;
}

static int mov_read_trex(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    MOVTrackExt *trex;

    if ((uint64_t)c->trex_count+1 >= UINT_MAX / sizeof(*c->trex_data))
        return -1;
    trex = av_realloc(c->trex_data, (c->trex_count+1)*sizeof(*c->trex_data));
    if (!trex)
        return AVERROR(ENOMEM);
    c->trex_data = trex;
    trex = &c->trex_data[c->trex_count++];
    get_byte(pb); /* version */
    get_be24(pb); /* flags */
    trex->track_id = get_be32(pb);
    trex->stsd_id  = get_be32(pb);
    trex->duration = get_be32(pb);
    trex->size     = get_be32(pb);
    trex->flags    = get_be32(pb);
    return 0;
}

static int mov_read_trun(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    MOVFragment *frag = &c->fragment;
    AVStream *st = NULL;
    MOVStreamContext *sc;
    uint64_t offset;
    int64_t dts;
    int data_offset = 0;
    unsigned entries, first_sample_flags = frag->flags;
    int flags, distance, i;

    for (i = 0; i < c->fc->nb_streams; i++) {
        if (c->fc->streams[i]->id == frag->track_id) {
            st = c->fc->streams[i];
            break;
        }
    }
    if (!st) {
        av_log(c->fc, AV_LOG_ERROR, "could not find corresponding track id %d\n", frag->track_id);
        return -1;
    }
    sc = st->priv_data;

    if (frag->stsd_id - 1 >= sc->dref_ids_count || !sc->dref_ids[frag->stsd_id-1])
        return 0;
    get_byte(pb); /* version */
    flags = get_be24(pb);
    entries = get_be32(pb);
    av_dlog(c->fc, "flags 0x%x entries %d\n", flags, entries);
    if (flags & 0x001) data_offset        = get_be32(pb);
    if (flags & 0x004) first_sample_flags = get_be32(pb);
    if (flags & 0x800) {
        MOVStts *ctts_data;
        if ((uint64_t)entries+sc->ctts_count >= UINT_MAX/sizeof(*sc->ctts_data))
            return -1;
        ctts_data = av_realloc(sc->ctts_data,
                               (entries+sc->ctts_count)*sizeof(*sc->ctts_data));
        if (!ctts_data)
            return AVERROR(ENOMEM);
        sc->ctts_data = ctts_data;
    }
    dts = st->duration;
    offset = frag->base_data_offset + data_offset;
    distance = 0;
    av_dlog(c->fc, "first sample flags 0x%x\n", first_sample_flags);
    for (i = 0; i < entries; i++) {
        unsigned sample_size = frag->size;
        int sample_flags = i ? frag->flags : first_sample_flags;
        unsigned sample_duration = frag->duration;
        int keyframe;

        if (flags & 0x100) sample_duration = get_be32(pb);
        if (flags & 0x200) sample_size     = get_be32(pb);
        if (flags & 0x400) sample_flags    = get_be32(pb);
        if (flags & 0x800) {
            sc->ctts_data[sc->ctts_count].count = 1;
            sc->ctts_data[sc->ctts_count].duration = get_be32(pb);
            sc->ctts_count++;
        }
        if ((keyframe = st->codec->codec_type == AVMEDIA_TYPE_AUDIO ||
             (flags & 0x004 && !i && !sample_flags) || sample_flags & 0x2000000))
            distance = 0;
        av_add_index_entry(st, offset, dts, sample_size, distance,
                           keyframe ? AVINDEX_KEYFRAME : 0);
        av_dlog(c->fc, "AVIndex stream %d, sample %d, offset %"PRIx64", dts %"PRId64", "
                "size %d, distance %d, keyframe %d\n", st->index, sc->sample_count+i,
                offset, dts, sample_size, distance, keyframe);
        distance++;
        dts += sample_duration;
        offset += sample_size;
    }
    frag->moof_offset = offset;
    st->duration = dts;
    return 0;
}

/* this atom should be null (from specs), but some buggy files put the 'moov' atom inside it... */
/* like the files created with Adobe Premiere 5.0, for samples see */
/* http://graphics.tudelft.nl/~wouter/publications/soundtests/ */
static int mov_read_wide(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    int err;

    if (atom.size < 8)
        return 0; /* continue */
    if (get_be32(pb) != 0) { /* 0 sized mdat atom... use the 'wide' atom size */
        url_fskip(pb, atom.size - 4);
        return 0;
    }
    atom.type = get_le32(pb);
    atom.size -= 8;
    if (atom.type != MKTAG('m','d','a','t')) {
        url_fskip(pb, atom.size);
        return 0;
    }
    err = mov_read_mdat(c, pb, atom);
    return err;
}

static int mov_read_cmov(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
#if CONFIG_ZLIB
    ByteIOContext ctx;
    uint8_t *cmov_data;
    uint8_t *moov_data; /* uncompressed data */
    long cmov_len, moov_len;
    int ret = -1;

    get_be32(pb); /* dcom atom */
    if (get_le32(pb) != MKTAG('d','c','o','m'))
        return -1;
    if (get_le32(pb) != MKTAG('z','l','i','b')) {
        av_log(c->fc, AV_LOG_ERROR, "unknown compression for cmov atom !");
        return -1;
    }
    get_be32(pb); /* cmvd atom */
    if (get_le32(pb) != MKTAG('c','m','v','d'))
        return -1;
    moov_len = get_be32(pb); /* uncompressed size */
    cmov_len = atom.size - 6 * 4;

    cmov_data = av_malloc(cmov_len);
    if (!cmov_data)
        return AVERROR(ENOMEM);
    moov_data = av_malloc(moov_len);
    if (!moov_data) {
        av_free(cmov_data);
        return AVERROR(ENOMEM);
    }
    get_buffer(pb, cmov_data, cmov_len);
    if(uncompress (moov_data, (uLongf *) &moov_len, (const Bytef *)cmov_data, cmov_len) != Z_OK)
        goto free_and_return;
    if(init_put_byte(&ctx, moov_data, moov_len, 0, NULL, NULL, NULL, NULL) != 0)
        goto free_and_return;
    atom.type = MKTAG('m','o','o','v');
    atom.size = moov_len;
#ifdef DEBUG
//    { int fd = open("/tmp/uncompheader.mov", O_WRONLY | O_CREAT); write(fd, moov_data, moov_len); close(fd); }
#endif
    ret = mov_read_default(c, &ctx, atom);
free_and_return:
    av_free(moov_data);
    av_free(cmov_data);
    return ret;
#else
    av_log(c->fc, AV_LOG_ERROR, "this file requires zlib support compiled in\n");
    return -1;
#endif
}

/* edit list atom */
static int mov_read_elst(MOVContext *c, ByteIOContext *pb, MOVAtom atom)
{
    MOVStreamContext *sc;
    int i, entries, version;

    if (c->fc->nb_streams < 1)
        return 0;
    sc = c->fc->streams[c->fc->nb_streams-1]->priv_data;

    version = get_byte(pb); /* version */
    get_be24(pb); /* flags */
    entries = get_be32(pb); /* entries */

    if((uint64_t)entries*12+8 > atom.size)
        return -1;
    sc->elst_data = av_malloc(entries * sizeof(*sc->elst_data));
    if (!sc->elst_data)
        return AVERROR(ENOMEM);
    sc->elst_count = entries;

    for (i = 0; i < entries; i++) {
        if (version == 1) {
            sc->elst_data[i].duration = get_be64(pb);
            sc->elst_data[i].time     = get_be64(pb);
        } else {
            sc->elst_data[i].duration = get_be32(pb); /* segment duration */
            sc->elst_data[i].time     = get_be32(pb); /* media time */
        }
        get_be32(pb); /* Media rate */
    }

    av_dlog(c->fc, "track[%i].edit_count = %i\n", c->fc->nb_streams-1, entries);
    return 0;
}

static const MOVParseTableEntry mov_default_parse_table[] = {
{ MKTAG('a','v','s','s'), mov_read_extradata },
{ MKTAG('c','h','p','l'), mov_read_chpl },
{ MKTAG('c','o','6','4'), mov_read_stco },
{ MKTAG('c','o','l','r'), mov_read_colr },
{ MKTAG('c','s','l','g'), mov_read_cslg },
{ MKTAG('c','t','t','s'), mov_read_ctts }, /* composition time to sample */
{ MKTAG('d','i','n','f'), mov_read_default },
{ MKTAG('d','r','e','f'), mov_read_dref },
{ MKTAG('e','d','t','s'), mov_read_default },
{ MKTAG('e','l','s','t'), mov_read_elst },
{ MKTAG('e','n','d','a'), mov_read_enda },
{ MKTAG('f','i','e','l'), mov_read_fiel },
{ MKTAG('f','t','y','p'), mov_read_ftyp },
{ MKTAG('g','l','b','l'), mov_read_glbl },
{ MKTAG('h','d','l','r'), mov_read_hdlr },
{ MKTAG('i','l','s','t'), mov_read_ilst },
{ MKTAG('j','p','2','h'), mov_read_extradata },
{ MKTAG('k','e','y','s'), mov_read_keys },
{ MKTAG('m','d','a','t'), mov_read_mdat },
{ MKTAG('m','d','h','d'), mov_read_mdhd },
{ MKTAG('m','d','i','a'), mov_read_default },
{ MKTAG('m','e','t','a'), mov_read_meta },
{ MKTAG('m','i','n','f'), mov_read_default },
{ MKTAG('m','o','o','f'), mov_read_moof },
{ MKTAG('m','o','o','v'), mov_read_moov },
{ MKTAG('m','v','e','x'), mov_read_default },
{ MKTAG('m','v','h','d'), mov_read_mvhd },
{ MKTAG('S','M','I',' '), mov_read_extradata }, /* Sorenson extension ??? */
{ MKTAG('Q','D','C','A'), mov_read_extradata },
{ MKTAG('a','l','a','c'), mov_read_extradata }, /* alac specific atom */
{ MKTAG('a','v','c','C'), mov_read_glbl },
{ MKTAG('p','a','s','p'), mov_read_pasp },
{ MKTAG('s','t','b','l'), mov_read_default },
{ MKTAG('s','t','c','o'), mov_read_stco },
{ MKTAG('s','t','p','s'), mov_read_stps },
{ MKTAG('s','t','r','f'), mov_read_strf },
{ MKTAG('s','t','s','c'), mov_read_stsc },
{ MKTAG('s','t','s','d'), mov_read_stsd }, /* sample description */
{ MKTAG('s','t','s','s'), mov_read_stss }, /* sync sample */
{ MKTAG('s','t','s','z'), mov_read_stsz }, /* sample size */
{ MKTAG('s','t','t','s'), mov_read_stts },
{ MKTAG('s','t','z','2'), mov_read_stsz }, /* compact sample size */
{ MKTAG('t','k','h','d'), mov_read_tkhd }, /* track header */
{ MKTAG('t','f','h','d'), mov_read_tfhd }, /* track fragment header */
{ MKTAG('t','r','a','k'), mov_read_trak },
{ MKTAG('t','r','a','f'), mov_read_default },
{ MKTAG('t','r','e','f'), mov_read_default },
{ MKTAG('c','h','a','p'), mov_read_chap },
{ MKTAG('t','r','e','x'), mov_read_trex },
{ MKTAG('t','r','u','n'), mov_read_trun },
{ MKTAG('u','d','t','a'), mov_read_default },
{ MKTAG('w','a','v','e'), mov_read_default },
{ MKTAG('e','s','d','s'), mov_read_esds },
{ MKTAG('d','a','c','3'), mov_read_dac3 }, /* AC-3 info */
{ MKTAG('w','i','d','e'), mov_read_wide }, /* place holder */
{ MKTAG('c','m','o','v'), mov_read_cmov },
{ MKTAG('t','a','p','t'), mov_read_default },
{ 0, NULL }
};

static int mov_probe(AVProbeData *p)
{
    unsigned int offset;
    uint32_t tag;
    int score = 0;

    /* check file header */
    offset = 0;
    for(;;) {
        /* ignore invalid offset */
        if ((offset + 8) > (unsigned int)p->buf_size)
            return score;
        tag = AV_RL32(p->buf + offset + 4);
        switch(tag) {
        /* check for obvious tags */
        case MKTAG('j','P',' ',' '): /* jpeg 2000 signature */
        case MKTAG('m','o','o','v'):
        case MKTAG('m','d','a','t'):
        case MKTAG('p','n','o','t'): /* detect movs with preview pics like ew.mov and april.mov */
        case MKTAG('u','d','t','a'): /* Packet Video PVAuthor adds this and a lot of more junk */
        case MKTAG('f','t','y','p'):
            return AVPROBE_SCORE_MAX;
        /* those are more common words, so rate then a bit less */
        case MKTAG('e','d','i','w'): /* xdcam files have reverted first tags */
        case MKTAG('w','i','d','e'):
        case MKTAG('f','r','e','e'):
        case MKTAG('j','u','n','k'):
        case MKTAG('p','i','c','t'):
            return AVPROBE_SCORE_MAX - 5;
        case MKTAG(0x82,0x82,0x7f,0x7d):
        case MKTAG('s','k','i','p'):
        case MKTAG('u','u','i','d'):
        case MKTAG('p','r','f','l'):
            offset = AV_RB32(p->buf+offset) + offset;
            /* if we only find those cause probedata is too small at least rate them */
            score = AVPROBE_SCORE_MAX - 50;
            break;
        default:
            /* unrecognized tag */
            return score;
        }
    }
    return score;
}

// must be done after parsing all trak because there's no order requirement
static void mov_read_chapters(AVFormatContext *s)
{
    MOVContext *mov = s->priv_data;
    AVStream *st = NULL;
    MOVStreamContext *sc;
    int64_t cur_pos;
    int i;

    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->id == mov->chapter_track) {
            st = s->streams[i];
            break;
        }
    if (!st) {
        av_log(s, AV_LOG_ERROR, "Referenced QT chapter track not found\n");
        return;
    }

    st->discard = AVDISCARD_ALL;
    sc = st->priv_data;
    cur_pos = url_ftell(s->pb);

    for (i = 0; i < st->nb_index_entries; i++) {
        AVIndexEntry *sample = &st->index_entries[i];
        int64_t end = i+1 < st->nb_index_entries ? st->index_entries[i+1].timestamp : st->duration;
        uint8_t *title;
        uint16_t ch;
        int len, title_len;
        ByteIOContext *pb = sc->sample_dref[i];

        if (url_fseek(pb, sample->pos, SEEK_SET) != sample->pos) {
            av_log(s, AV_LOG_ERROR, "Chapter %d not found in file\n", i);
            goto finish;
        }

        // the first two bytes are the length of the title
        len = get_be16(pb);
        if (len > sample->size-2)
            continue;
        title_len = 2*len + 1;
        if (!(title = av_mallocz(title_len)))
            goto finish;

        // The samples could theoretically be in any encoding if there's an encd
        // atom following, but in practice are only utf-8 or utf-16, distinguished
        // instead by the presence of a BOM
        ch = get_be16(pb);
        if (ch == 0xfeff)
            avio_get_str16be(pb, len, title, title_len);
        else if (ch == 0xfffe)
            avio_get_str16le(pb, len, title, title_len);
        else {
            AV_WB16(title, ch);
            get_strz(pb, title + 2, len - 1);
        }

        ff_new_chapter(s, i, st->time_base, sample->timestamp, end, title);
        av_freep(&title);
    }
finish:
    url_fseek(s->pb, cur_pos, SEEK_SET);
}

static void mov_read_timecode(AVFormatContext *s, AVStream *st)
{
    int64_t pos = url_ftell(s->pb);
    AVIndexEntry *e;
    int framenum;
    char timecode[32];

    if (!st->nb_index_entries)
        return;
    e = &st->index_entries[0];
    url_fseek(s->pb, e->pos, SEEK_SET);
    framenum = get_be32(s->pb);
    if (ff_framenum_to_timecode(timecode, framenum,
                                st->codec->flags2 & CODEC_FLAG2_DROP_FRAME_TIMECODE,
                                st->codec->time_base.den) < 0) {
        av_log(s, AV_LOG_ERROR, "error reading timecode\n");
        return;
    }
    av_metadata_set(&s->metadata, "timecode", timecode);
    url_fseek(s->pb, pos, SEEK_SET);
}

static int mov_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    MOVContext *mov = s->priv_data;
    ByteIOContext *pb = s->pb;
    int i, err;
    MOVAtom atom = { AV_RL32("root") };

    mov->fc = s;
    /* .mov and .mp4 aren't streamable anyway (only progressive download if moov is before mdat) */
    if(!url_is_streamed(pb))
        atom.size = url_fsize(pb);
    else
        atom.size = INT64_MAX;

    mov->metadata = &s->metadata;
    /* check MOV header */
    if ((err = mov_read_default(mov, pb, atom)) < 0) {
        av_log(s, AV_LOG_ERROR, "error reading header: %d\n", err);
        return err;
    }
    if (!mov->found_moov) {
        av_log(s, AV_LOG_ERROR, "error, moov atom not found, file broken\n");
        return -1;
    }
    av_dlog(mov->fc, "on_parse_exit_offset=%lld\n", url_ftell(pb));

    if (!url_is_streamed(pb)) {
        if (mov->chapter_track > 0)
            mov_read_chapters(s);
        for (i = 0; i < s->nb_streams; i++) {
            AVStream *st = s->streams[i];
            if (st->codec->codec_tag == AV_RL32("tmcd"))
                mov_read_timecode(s, st);
        }
    }

    for (i = 0; i < mov->keys_count; i++)
        av_freep(&mov->keys_data[i]);
    av_freep(&mov->keys_data);
    mov->keys_count = 0;

    return 0;
}

static AVIndexEntry *mov_find_next_sample(AVFormatContext *s, AVStream **st)
{
    AVIndexEntry *sample = NULL;
    int64_t best_dts = INT64_MAX;
    int i;
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *avst = s->streams[i];
        MOVStreamContext *msc = avst->priv_data;
        if (msc->current_sample < avst->nb_index_entries) {
            AVIndexEntry *current_sample = &avst->index_entries[msc->current_sample];
            int64_t dts = av_rescale(current_sample->timestamp, AV_TIME_BASE, msc->time_scale);
            ByteIOContext *pb = msc->sample_dref[msc->current_sample];
            av_dlog(s, "stream %d, sample %d, dts %"PRId64"\n", i, msc->current_sample, dts);
            if (!sample || (url_is_streamed(s->pb) && current_sample->pos < sample->pos) ||
                (!url_is_streamed(s->pb) &&
                 ((pb != s->pb && dts < best_dts) || (pb == s->pb &&
                 ((FFABS(best_dts - dts) <= AV_TIME_BASE && current_sample->pos < sample->pos) ||
                  (FFABS(best_dts - dts) > AV_TIME_BASE && dts < best_dts)))))) {
                sample = current_sample;
                best_dts = dts;
                *st = avst;
            }
        }
    }
    return sample;
}

static int mov_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MOVContext *mov = s->priv_data;
    MOVStreamContext *sc;
    AVIndexEntry *sample;
    AVStream *st = NULL;
    int ret;
 retry:
    sample = mov_find_next_sample(s, &st);
    if (!sample) {
        mov->found_mdat = 0;
        if (!url_is_streamed(s->pb) ||
            mov_read_default(mov, s->pb, (MOVAtom){ AV_RL32("root"), INT64_MAX }) < 0 ||
            url_feof(s->pb))
            return AVERROR_EOF;
        av_dlog(s, "read fragments, offset 0x%llx\n", url_ftell(s->pb));
        goto retry;
    }
    sc = st->priv_data;
    /* must be done just before reading, to avoid infinite loop on sample */
    sc->current_sample++;

    if (st->discard != AVDISCARD_ALL) {
        ByteIOContext *pb = sc->sample_dref[sc->current_sample - 1];
        if (url_fseek(pb, sample->pos, SEEK_SET) != sample->pos) {
            av_log(mov->fc, AV_LOG_ERROR, "stream %d, offset 0x%"PRIx64": partial file\n",
                   sc->ffindex, sample->pos);
            return -1;
        }
        ret = av_get_packet(pb, pkt, sample->size);
        if (ret < 0)
            return ret;
#if CONFIG_DV_DEMUXER
        if (mov->dv_demux && sc->dv_audio_container) {
            dv_produce_packet(mov->dv_demux, pkt, pkt->data, pkt->size);
            av_free(pkt->data);
            pkt->size = 0;
            ret = dv_get_packet(mov->dv_demux, pkt);
            if (ret < 0)
                return ret;
        }
#endif
    }

    pkt->stream_index = sc->ffindex;
    pkt->dts = sample->timestamp;
    if (sc->ctts_data) {
        pkt->pts = pkt->dts + sc->dts_shift + sc->ctts_data[sc->ctts_index].duration;
        /* update ctts context */
        sc->ctts_sample++;
        if (sc->ctts_index < sc->ctts_count &&
            sc->ctts_data[sc->ctts_index].count == sc->ctts_sample) {
            sc->ctts_index++;
            sc->ctts_sample = 0;
        }
    } else {
        int64_t next_dts = (sc->current_sample < st->nb_index_entries) ?
            st->index_entries[sc->current_sample].timestamp : st->duration;
        pkt->duration = next_dts - pkt->dts;
        pkt->pts = pkt->dts;
    }
    if (st->discard == AVDISCARD_ALL)
        goto retry;
    pkt->flags |= sample->flags & AVINDEX_KEYFRAME ? AV_PKT_FLAG_KEY : 0;
    pkt->pos = sample->pos;
    //av_dlog(s, "stream %d, pts %"PRId64", dts %"PRId64", pos 0x%"PRIx64", duration %d\n",
    //        pkt->stream_index, pkt->pts, pkt->dts, pkt->pos, pkt->duration);
    return 0;
}

static int mov_seek_stream(AVFormatContext *s, AVStream *st, int64_t timestamp, int flags)
{
    MOVStreamContext *sc = st->priv_data;
    int sample, time_sample;
    int i;

    sample = av_index_search_timestamp(st, timestamp, flags);
    av_dlog(s, "stream %d, timestamp %"PRId64", sample %d\n", st->index, timestamp, sample);
    if (sample < 0 && st->nb_index_entries && timestamp < st->index_entries[0].timestamp)
        sample = 0;
    if (sample < 0) /* not sure what to do */
        return -1;
    sc->current_sample = sample;
    av_dlog(s, "stream %d, found sample %d\n", st->index, sc->current_sample);
    /* adjust ctts index */
    if (sc->ctts_data) {
        time_sample = 0;
        for (i = 0; i < sc->ctts_count; i++) {
            int next = time_sample + sc->ctts_data[i].count;
            if (next > sc->current_sample) {
                sc->ctts_index = i;
                sc->ctts_sample = sc->current_sample - time_sample;
                break;
            }
            time_sample = next;
        }
    }
    return sample;
}

static int mov_read_seek(AVFormatContext *s, int stream_index, int64_t sample_time, int flags)
{
    AVStream *st;
    int64_t seek_timestamp, timestamp;
    int sample;
    int i;

    if (stream_index >= s->nb_streams)
        return -1;
    if (sample_time < 0)
        sample_time = 0;

    st = s->streams[stream_index];
    sample = mov_seek_stream(s, st, sample_time, flags);
    if (sample < 0)
        return -1;

    /* adjust seek timestamp to found sample timestamp */
    seek_timestamp = st->index_entries[sample].timestamp;

    for (i = 0; i < s->nb_streams; i++) {
        st = s->streams[i];
        if (stream_index == i)
            continue;

        timestamp = av_rescale_q(seek_timestamp, s->streams[stream_index]->time_base, st->time_base);
        mov_seek_stream(s, st, timestamp, flags);
    }
    return 0;
}

static int mov_read_close(AVFormatContext *s)
{
    MOVContext *mov = s->priv_data;
    int i, j;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MOVStreamContext *sc = st->priv_data;

        av_freep(&sc->ctts_data);
        for (j = 0; j < sc->drefs_count; j++) {
            if (sc->drefs[j].pb && sc->drefs[j].pb != s->pb)
                url_fclose(sc->drefs[j].pb);
            av_freep(&sc->drefs[j].path);
            av_freep(&sc->drefs[j].dir);
        }
        av_freep(&sc->sample_dref);
        av_freep(&sc->dref_ids);
        av_freep(&sc->drefs);
        av_freep(&st->codec->palctrl);
    }

    if (mov->dv_demux) {
        for(i = 0; i < mov->dv_fctx->nb_streams; i++) {
            av_freep(&mov->dv_fctx->streams[i]->codec);
            av_freep(&mov->dv_fctx->streams[i]);
        }
        av_freep(&mov->dv_fctx);
        av_freep(&mov->dv_demux);
    }

    av_freep(&mov->trex_data);

    return 0;
}

AVInputFormat ff_mov_demuxer = {
    "mov,mp4,m4a,3gp,3g2,mj2",
    NULL_IF_CONFIG_SMALL("QuickTime/MPEG-4/Motion JPEG 2000 format"),
    sizeof(MOVContext),
    mov_probe,
    mov_read_header,
    mov_read_packet,
    mov_read_close,
    mov_read_seek,
};
