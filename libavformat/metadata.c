/*
 * copyright (c) 2009 Michael Niedermayer
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
#include "metadata.h"

static void av_metadata_free_tag(AVMetadataTag *tag)
{
    av_freep(&tag->key);
    av_freep(&tag->value);
    if (tag->attributes) {
        while (tag->attributes->count--) {
            av_freep(&tag->attributes->elems[tag->attributes->count].key);
            av_freep(&tag->attributes->elems[tag->attributes->count].value);
        }
        av_freep(&tag->attributes->elems);
    }
    av_freep(&tag->attributes);
}

AVMetadataTag *
av_metadata_get(AVMetadata *m, const char *key, const AVMetadataTag *prev, int flags)
{
    unsigned int i, j;

    if(!m)
        return NULL;

    if(prev) i= prev - m->elems + 1;
    else     i= 0;

    for(; i<m->count; i++){
        const char *s= m->elems[i].key;
        if(flags & AV_METADATA_MATCH_CASE) for(j=0;         s[j]  ==         key[j]  && key[j]; j++);
        else                               for(j=0; toupper(s[j]) == toupper(key[j]) && key[j]; j++);
        if(key[j])
            continue;
        return &m->elems[i];
    }
    return NULL;
}

int av_metadata_set_custom(AVMetadata **pm, AVMetadataTag **ret, enum AVMetadataType type,
                           const char *key, const char *value, unsigned len,
                           int flags)
{
    AVMetadata *m= *pm;
    AVMetadataTag *tag= av_metadata_get(m, key, NULL, flags);

    if(!key || !pm || !len || !value)
        return -1;

    if (ret)
        *ret = NULL;

    if(!m)
        m=*pm= av_mallocz(sizeof(*m));
    if(!m)
        return AVERROR(ENOMEM);

    if(tag){
        if (flags & AV_METADATA_DONT_OVERWRITE)
            return 0;
        av_metadata_free_tag(tag);
    }else{
        AVMetadataTag *tmp;
        if(m->count >= UINT_MAX / sizeof(*m->elems))
            return -1;
        tmp= av_realloc(m->elems, (m->count+1) * sizeof(*m->elems));
        if(tmp){
            m->elems= tmp;
        }else
            return AVERROR(ENOMEM);
        tag= &m->elems[m->count++];
    }

    if (flags & AV_METADATA_DONT_STRDUP_KEY)
        tag->key = key;
    else
        tag->key = av_strdup(key);

    if (flags & AV_METADATA_DONT_STRDUP_VAL) {
        tag->value = value;
    } else if (type == METADATA_BYTEARRAY) {
        tag->value = av_malloc(len);
        if (!tag->value)
            return AVERROR(ENOMEM);
        memcpy(tag->value, value, len);
    } else {
        tag->value = av_malloc(len+1);
        if (!tag->value)
            return AVERROR(ENOMEM);
        memcpy(tag->value, value, len);
        tag->value[len] = 0;
    }

    tag->len = len;
    tag->type = type;
    tag->attributes = NULL;

    if (ret)
        *ret = tag;

    return 0;
}

int av_metadata_set2(AVMetadata **pm, const char *key, const char *value, int flags)
{
    if (!value)
        return -1;
    return av_metadata_set_custom(pm, NULL, METADATA_STRING, key, value, strlen(value), flags);
}

#if FF_API_OLD_METADATA
int av_metadata_set(AVMetadata **pm, const char *key, const char *value)
{
    return av_metadata_set2(pm, key, value, 0);
}

void av_metadata_conv(AVFormatContext *ctx, const AVMetadataConv *d_conv,
                                            const AVMetadataConv *s_conv)
{
    return;
}
#endif

void av_metadata_free(AVMetadata **pm)
{
    AVMetadata *m= *pm;

    if(m){
        while(m->count--)
            av_metadata_free_tag(&m->elems[m->count]);
        av_freep(&m->elems);
    }
    av_freep(pm);
}

int av_metadata_set_int(AVMetadata **pm, const char *key, int value)
{
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", value);
    return av_metadata_set_custom(pm, NULL, METADATA_INT, key, buf, len, 0);
}

int av_metadata_set_float(AVMetadata **pm, const char *key, double value)
{
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%f", value);
    return av_metadata_set_custom(pm, NULL, METADATA_FLOAT, key, buf, len, 0);
}

int av_metadata_set_bool(AVMetadata **pm, const char *key, int value)
{
    if (value > 0)
        return av_metadata_set_custom(pm, NULL, METADATA_STRING, key, "true", 4, 0);
    else
        return av_metadata_set_custom(pm, NULL, METADATA_STRING, key, "false", 5, 0);
}

const char *av_metadata_get_attribute(AVMetadataTag *tag, const char *key)
{
    int i, j;

    if (!tag->attributes)
        return NULL;

    for (i = 0; i < tag->attributes->count; i++) {
        char *s = tag->attributes->elems[i].key;
        for (j = 0; toupper(s[j]) == toupper(key[j]) && key[j]; j++)
            ;
        if (key[j])
            continue;
        return tag->attributes->elems[i].value;
    }
    return NULL;
}

int av_metadata_set_attribute(AVMetadataTag *tag, const char *key, const char *value)
{
    AVMetadataAttribute *attribute;

    if (!tag->attributes)
        tag->attributes = av_mallocz(sizeof(*tag->attributes));
    if (!tag->attributes)
        return AVERROR_NOMEM;

    if (tag->attributes->count == UINT_MAX / sizeof(*attribute))
        return -1;

    attribute = av_realloc(tag->attributes->elems,
                           (tag->attributes->count+1)*sizeof(*attribute));
    if (!attribute)
        return AVERROR_NOMEM;

    tag->attributes->elems = attribute;
    attribute = &tag->attributes->elems[tag->attributes->count];
    attribute->key = av_strdup(key);
    attribute->value = av_strdup(value);

    tag->attributes->count++;

    return 0;
}

int av_metadata_copy_attributes(AVMetadataTag *otag, AVMetadataTag *itag)
{
    int i;

    if (!itag->attributes)
        return 0;

    for (i = 0; i < itag->attributes->count; i++)
        av_metadata_set_attribute(otag, itag->attributes->elems[i].key,
                                  itag->attributes->elems[i].value);
    return 0;
}

void ff_metadata_conv2(AVMetadata **dst, AVMetadata **pm,
                       const AVMetadataConv *d_conv,
                       const AVMetadataConv *s_conv)
{
    /* TODO: use binary search to look up the two conversion tables
       if the tables are getting big enough that it would matter speed wise */
    const AVMetadataConv *sc, *dc;
    AVMetadataTag *mtag = NULL, *tag;
    AVMetadata *tmp = NULL;
    const char *key;

    if (d_conv == s_conv)
        return;

    while((mtag=av_metadata_get(*pm, "", mtag, AV_METADATA_IGNORE_SUFFIX))) {
        key = mtag->key;
        if (s_conv)
            for (sc=s_conv; sc->native; sc++)
                if (!strcasecmp(key, sc->native)) {
                    key = sc->generic;
                    break;
                }
        if (d_conv)
            for (dc=d_conv; dc->native; dc++)
                if (!strcasecmp(key, dc->generic)) {
                    key = dc->native;
                    break;
                }
        av_metadata_set_custom(&tmp, &tag, mtag->type, key, mtag->value, mtag->len, 0);
        av_metadata_copy_attributes(tag, mtag);
    }
    if (dst) {
        *dst = tmp;
    } else {
        av_metadata_free(pm);
        *pm = tmp;
    }
}

void ff_metadata_conv(AVMetadata **pm,
                      const AVMetadataConv *d_conv,
                      const AVMetadataConv *s_conv)
{
    ff_metadata_conv2(NULL, pm, d_conv, s_conv);
}

void ff_metadata_conv_ctx(AVFormatContext *ctx, const AVMetadataConv *d_conv,
                                                const AVMetadataConv *s_conv)
{
    int i;
    ff_metadata_conv(&ctx->metadata, d_conv, s_conv);
    for (i=0; i<ctx->nb_streams ; i++)
        ff_metadata_conv(&ctx->streams [i]->metadata, d_conv, s_conv);
    for (i=0; i<ctx->nb_chapters; i++)
        ff_metadata_conv(&ctx->chapters[i]->metadata, d_conv, s_conv);
    for (i=0; i<ctx->nb_programs; i++)
        ff_metadata_conv(&ctx->programs[i]->metadata, d_conv, s_conv);
}

void av_metadata_copy(AVMetadata **dst, AVMetadata *src, int flags)
{
    AVMetadataTag *t = NULL, *tag;

    while ((t = av_metadata_get(src, "", t, AV_METADATA_IGNORE_SUFFIX))) {
        av_metadata_set_custom(dst, &tag, t->type, t->key,
                               t->value, t->len, flags);
        if (tag)
            av_metadata_copy_attributes(tag, t);
    }
}
