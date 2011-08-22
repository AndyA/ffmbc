/*
 * Various utilities for command line tools
 * Copyright (c) 2000-2003 Fabrice Bellard
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

/* Include only the enabled headers since some compilers (namely, Sun
   Studio) will not omit unused inline functions and create undefined
   references to libraries that are not being built. */

#include "config.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libpostproc/postprocess.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavcodec/opt.h"
#include "cmdutils.h"
#include "version.h"
#if CONFIG_NETWORK
#include "libavformat/network.h"
#endif
#if HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

const char **opt_names;
const char **opt_values;
static int opt_name_count;
AVCodecContext *avcodec_opts;
AVFormatContext *avformat_opts;
SwsContext *sws_opts;

static const int this_year = 2011;

void init_opts(void)
{
    avcodec_opts = avcodec_alloc_context();
    avformat_opts = avformat_alloc_context();
#if CONFIG_SWSCALE
    sws_opts = sws_alloc_context();
#endif
}

void reset_opts(void)
{
    av_freep(&opt_names);
    av_freep(&opt_values);
    opt_name_count = 0;
}

void uninit_opts(void)
{
    av_freep(&avcodec_opts);
    av_freep(&avformat_opts->key);
    av_freep(&avformat_opts);
#if CONFIG_SWSCALE
    av_freep(&sws_opts);
#endif
    reset_opts();
}

static void log_stdout(void* ptr, int level, const char* fmt, va_list vl)
{
    vfprintf(stdout, fmt, vl);
}

int show_options(const char *name, const char *type, void *obj, int req_flags)
{
    if (!obj || !name)
        return -1;

    av_log_set_callback(log_stdout);

    if (name && type)
        printf("%s %s options:\n", name, type);
    else
        printf("%s options:\n", name);
    av_opt_list(obj, NULL, NULL, req_flags, 0);

    av_log_set_callback(av_log_default_callback);

    return 0;
}

double parse_number_or_die(const char *context, const char *numstr, int type, double min, double max)
{
    char *tail;
    const char *error;
    double d = av_strtod(numstr, &tail);
    if (*tail)
        error= "Expected number for %s but found: %s\n";
    else if (d < min || d > max)
        error= "The value for %s was %s which is not within %f - %f\n";
    else if(type == OPT_INT64 && (int64_t)d != d)
        error= "Expected int64 for %s but found %s\n";
    else
        return d;
    fprintf(stderr, error, context, numstr, min, max);
    exit(1);
}

int64_t parse_time_or_die(const char *context, const char *timestr, int is_duration)
{
    int64_t us = parse_date(timestr, is_duration);
    if (us == INT64_MIN) {
        fprintf(stderr, "Invalid %s specification for %s: %s\n",
                is_duration ? "duration" : "date", context, timestr);
        exit(1);
    }
    return us;
}

void show_help_options(const OptionDef *options, const char *msg, int mask, int value)
{
    const OptionDef *po;
    int first;

    first = 1;
    for(po = options; po->name != NULL; po++) {
        char buf[64];
        if ((po->flags & mask) == value) {
            if (first) {
                printf("%s", msg);
                first = 0;
            }
            av_strlcpy(buf, po->name, sizeof(buf));
            if (po->flags & HAS_ARG) {
                av_strlcat(buf, " ", sizeof(buf));
                av_strlcat(buf, po->argname, sizeof(buf));
            }
            printf("-%-17s  %s\n", buf, po->help);
        }
    }
}

static const OptionDef* find_option(const OptionDef *po, const char *name){
    while (po->name != NULL) {
        if (!strcmp(name, po->name))
            break;
        po++;
    }
    return po;
}

void parse_options(int argc, char **argv, const OptionDef *options,
                   void (* parse_arg_function)(const char*))
{
    const char *opt, *arg;
    int optindex, handleoptions=1;
    const OptionDef *po;

    /* parse options */
    optindex = 1;
    while (optindex < argc) {
        opt = argv[optindex++];

        if (handleoptions && opt[0] == '-' && opt[1] != '\0') {
            int bool_val = 1;
            if (opt[1] == '-' && opt[2] == '\0') {
                handleoptions = 0;
                continue;
            }
            opt++;
            po= find_option(options, opt);
            if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
                /* handle 'no' bool option */
                po = find_option(options, opt + 2);
                if (!(po->name && (po->flags & OPT_BOOL)))
                    goto unknown_opt;
                bool_val = 0;
            }
            if (!po->name)
                po= find_option(options, "default");
            if (!po->name) {
unknown_opt:
                fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], opt);
                exit(1);
            }
            arg = NULL;
            if (po->flags & HAS_ARG) {
                arg = argv[optindex++];
                if (!arg) {
                    fprintf(stderr, "%s: missing argument for option '%s'\n", argv[0], opt);
                    exit(1);
                }
            }
            if (po->flags & OPT_STRING) {
                *po->u.str_arg = arg;
            } else if (po->flags & OPT_BOOL) {
                *po->u.int_arg = bool_val;
            } else if (po->flags & OPT_INT) {
                *po->u.int_arg = parse_number_or_die(opt, arg, OPT_INT64, INT_MIN, INT_MAX);
            } else if (po->flags & OPT_INT64) {
                *po->u.int64_arg = parse_number_or_die(opt, arg, OPT_INT64, INT64_MIN, INT64_MAX);
            } else if (po->flags & OPT_FLOAT) {
                *po->u.float_arg = parse_number_or_die(opt, arg, OPT_FLOAT, -INFINITY, INFINITY);
            } else if (po->flags & OPT_FUNC2) {
                if (po->u.func2_arg(opt, arg) < 0) {
                    fprintf(stderr, "%s: failed to set value '%s' for option '%s'\n", argv[0], arg, opt);
                    exit(1);
                }
            } else {
                po->u.func_arg(arg);
            }
            if(po->flags & OPT_EXIT)
                exit(0);
        } else {
            if (parse_arg_function)
                parse_arg_function(opt);
        }
    }
}

int opt_default(const char *opt, const char *arg)
{
    AVCodec *p = NULL;
    AVOutputFormat *oformat = NULL;
    const AVOption *o = av_find_opt(avcodec_opts, opt, NULL, 0, 0);
    if (o)
        goto out;
    o = av_find_opt(avformat_opts, opt, NULL, 0, 0);
    if (o)
        goto out;
    o = av_find_opt(sws_opts, opt, NULL, 0, 0);
    if (o)
        goto out;

    while ((p = av_codec_next(p))) {
        const AVClass *c = p->priv_class;
        if (c && av_find_opt(&c, opt, NULL, 0, 0))
            goto out;
    }
    while ((oformat = av_oformat_next(oformat))) {
        const AVClass *c = oformat->priv_class;
        if (c && av_find_opt(&c, opt, NULL, 0, 0))
            goto out;
    }

    av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'\n", opt);
    exit(1);

 out:
    opt_values = av_realloc(opt_values, sizeof(void*)*(opt_name_count+1));
    opt_values[opt_name_count] = arg;
    opt_names = av_realloc(opt_names, sizeof(void*)*(opt_name_count+1));
    opt_names[opt_name_count++]= opt;
    return 0;
}

int opt_loglevel(const char *opt, const char *arg)
{
    const struct { const char *name; int level; } log_levels[] = {
        { "quiet"  , AV_LOG_QUIET   },
        { "panic"  , AV_LOG_PANIC   },
        { "fatal"  , AV_LOG_FATAL   },
        { "error"  , AV_LOG_ERROR   },
        { "warning", AV_LOG_WARNING },
        { "info"   , AV_LOG_INFO    },
        { "verbose", AV_LOG_VERBOSE },
        { "debug"  , AV_LOG_DEBUG   },
    };
    char *tail;
    int level;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
        if (!strcmp(log_levels[i].name, arg)) {
            av_log_set_level(log_levels[i].level);
            return 0;
        }
    }

    level = strtol(arg, &tail, 10);
    if (*tail) {
        fprintf(stderr, "Invalid loglevel \"%s\". "
                        "Possible levels are numbers or:\n", arg);
        for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++)
            fprintf(stderr, "\"%s\"\n", log_levels[i].name);
        exit(1);
    }
    av_log_set_level(level);
    return 0;
}

int opt_timelimit(const char *opt, const char *arg)
{
#if HAVE_SETRLIMIT
    int lim = parse_number_or_die(opt, arg, OPT_INT64, 0, INT_MAX);
    struct rlimit rl = { lim, lim + 1 };
    if (setrlimit(RLIMIT_CPU, &rl))
        perror("setrlimit");
#else
    fprintf(stderr, "Warning: -%s not implemented on this OS\n", opt);
#endif
    return 0;
}

void set_context_opts(void *ctx, int flags, AVCodec *codec)
{
    int i;
    void *priv_ctx=NULL;
    AVCodecContext *avctx = NULL;
    AVFormatContext *s = NULL;

    if(!strcmp("AVCodecContext", (*(AVClass**)ctx)->class_name)){
        avctx = ctx;
        if(codec && codec->priv_class && avctx->priv_data){
            priv_ctx= avctx->priv_data;
        }
    } else if (!strcmp("AVFormatContext", (*(AVClass**)ctx)->class_name)) {
        s = ctx;
        if (s->oformat && s->oformat->priv_class) {
            priv_ctx = s->priv_data;
        }
    }

    for(i=0; i<opt_name_count; i++){
        if (priv_ctx) {
            if (av_find_opt(priv_ctx, opt_names[i], NULL, flags, flags)) {
                if (av_set_string3(priv_ctx, opt_names[i], opt_values[i], 0, NULL) < 0)
                    goto error;
            } else
                goto global;
        } else {
        global:
            if (av_find_opt(ctx, opt_names[i], NULL, flags, flags)) {
                if (av_set_string3(ctx, opt_names[i], opt_values[i], 0, NULL) < 0) {
                error:
                    fprintf(stderr, "Invalid value '%s' for option '%s'\n",
                            opt_values[i], opt_names[i]);
                    exit(1);
                }
            }
        }
    }

    if ((avctx && avctx->debug) || (s && s->debug))
        av_log_set_level(AV_LOG_DEBUG);
}

void print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    fprintf(stderr, "%s: %s\n", filename, errbuf_ptr);
}

static int warned_cfg = 0;

#define INDENT        1
#define SHOW_VERSION  2
#define SHOW_CONFIG   4

#define PRINT_LIB_INFO(outstream,libname,LIBNAME,flags)                 \
    if (CONFIG_##LIBNAME) {                                             \
        const char *indent = flags & INDENT? "  " : "";                 \
        if (flags & SHOW_VERSION) {                                     \
            unsigned int version = libname##_version();                 \
            fprintf(outstream, "%slib%-9s %2d.%3d.%2d / %2d.%3d.%2d\n", \
                    indent, #libname,                                   \
                    LIB##LIBNAME##_VERSION_MAJOR,                       \
                    LIB##LIBNAME##_VERSION_MINOR,                       \
                    LIB##LIBNAME##_VERSION_MICRO,                       \
                    version >> 16, version >> 8 & 0xff, version & 0xff); \
        }                                                               \
        if (flags & SHOW_CONFIG) {                                      \
            const char *cfg = libname##_configuration();                \
            if (strcmp(FFMPEG_CONFIGURATION, cfg)) {                    \
                if (!warned_cfg) {                                      \
                    fprintf(outstream,                                  \
                            "%sWARNING: library configuration mismatch\n", \
                            indent);                                    \
                    warned_cfg = 1;                                     \
                }                                                       \
                fprintf(stderr, "%s%-11s configuration: %s\n",          \
                        indent, #libname, cfg);                         \
            }                                                           \
        }                                                               \
    }                                                                   \

static void print_all_libs_info(FILE* outstream, int flags)
{
    PRINT_LIB_INFO(outstream, avutil,   AVUTIL,   flags);
    PRINT_LIB_INFO(outstream, avcodec,  AVCODEC,  flags);
    PRINT_LIB_INFO(outstream, avformat, AVFORMAT, flags);
    PRINT_LIB_INFO(outstream, avdevice, AVDEVICE, flags);
    PRINT_LIB_INFO(outstream, avfilter, AVFILTER, flags);
    PRINT_LIB_INFO(outstream, swscale,  SWSCALE,  flags);
    PRINT_LIB_INFO(outstream, postproc, POSTPROC, flags);
}

void show_banner(void)
{
    fprintf(stderr, "%s version " FFMPEG_VERSION "\nCopyright (c) %d-%d Baptiste Coudurier and the FFmpeg developers\n",
            program_name, program_birth_year, this_year);
}

void show_version(void) {
    fprintf(stderr, "built on %s %s with %s %s\n",
            __DATE__, __TIME__, CC_TYPE, CC_VERSION);
    fprintf(stderr, "configuration: " FFMPEG_CONFIGURATION "\n");
    print_all_libs_info(stderr, INDENT|SHOW_CONFIG);
    print_all_libs_info(stdout, SHOW_VERSION);
}

void show_license(void)
{
    printf(
#if CONFIG_NONFREE
    "This version of %s has nonfree parts compiled in.\n"
    "Therefore it is not legally redistributable.\n",
    program_name
#elif CONFIG_GPL
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation; version 2 of the License.\n"
    "\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with %s; if not, write to the Free Software\n"
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA\n",
    program_name, program_name, program_name
#endif
    );
}

void list_fmts(void (*get_fmt_string)(char *buf, int buf_size, int fmt), int nb_fmts)
{
    int i;
    char fmt_str[128];
    for (i=-1; i < nb_fmts; i++) {
        get_fmt_string (fmt_str, sizeof(fmt_str), i);
        fprintf(stdout, "%s\n", fmt_str);
    }
}

void show_formats(void)
{
    AVInputFormat *ifmt=NULL;
    AVOutputFormat *ofmt=NULL;
    const char *last_name;

    printf(
        "File formats:\n"
        " D. = Demuxing supported\n"
        " .E = Muxing supported\n"
        " --\n");
    last_name= "000";
    for(;;){
        int decode=0;
        int encode=0;
        const char *name=NULL;
        const char *long_name=NULL;

        while((ofmt= av_oformat_next(ofmt))) {
            if((name == NULL || strcmp(ofmt->name, name)<0) &&
                strcmp(ofmt->name, last_name)>0){
                name= ofmt->name;
                long_name= ofmt->long_name;
                encode=1;
            }
        }
        while((ifmt= av_iformat_next(ifmt))) {
            if((name == NULL || strcmp(ifmt->name, name)<0) &&
                strcmp(ifmt->name, last_name)>0){
                name= ifmt->name;
                long_name= ifmt->long_name;
                encode=0;
            }
            if(name && strcmp(ifmt->name, name)==0)
                decode=1;
        }
        if(name==NULL)
            break;
        last_name= name;

        printf(
            " %s%s %-15s %s\n",
            decode ? "D":" ",
            encode ? "E":" ",
            name,
            long_name ? long_name:" ");
    }
}

void show_codecs(void)
{
    AVCodec *p=NULL, *p2;
    const char *last_name;
    printf(
        "Codecs:\n"
        " D..... = Decoding supported\n"
        " .E.... = Encoding supported\n"
        " ..V... = Video codec\n"
        " ..A... = Audio codec\n"
        " ..S... = Subtitle codec\n"
        " ...S.. = Supports draw_horiz_band\n"
        " ....D. = Supports direct rendering method 1\n"
        " .....T = Supports weird frame truncation\n"
        " ------\n");
    last_name= "000";
    for(;;){
        int decode=0;
        int encode=0;
        int cap=0;
        const char *type_str;

        p2=NULL;
        while((p= av_codec_next(p))) {
            if((p2==NULL || strcmp(p->name, p2->name)<0) &&
                strcmp(p->name, last_name)>0){
                p2= p;
                decode= encode= cap=0;
            }
            if(p2 && strcmp(p->name, p2->name)==0){
                if(p->decode) decode=1;
                if(p->encode) encode=1;
                cap |= p->capabilities;
            }
        }
        if(p2==NULL)
            break;
        last_name= p2->name;

        switch(p2->type) {
        case AVMEDIA_TYPE_VIDEO:
            type_str = "V";
            break;
        case AVMEDIA_TYPE_AUDIO:
            type_str = "A";
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            type_str = "S";
            break;
        default:
            type_str = "?";
            break;
        }
        printf(
            " %s%s%s%s%s%s %-15s %s",
            decode ? "D": (/*p2->decoder ? "d":*/" "),
            encode ? "E":" ",
            type_str,
            cap & CODEC_CAP_DRAW_HORIZ_BAND ? "S":" ",
            cap & CODEC_CAP_DR1 ? "D":" ",
            cap & CODEC_CAP_TRUNCATED ? "T":" ",
            p2->name,
            p2->long_name ? p2->long_name : "");
       /* if(p2->decoder && decode==0)
            printf(" use %s for decoding", p2->decoder->name);*/
        printf("\n");
    }
    printf("\n");
    printf(
"Note, the names of encoders and decoders do not always match, so there are\n"
"several cases where the above table shows encoder only or decoder only entries\n"
"even though both encoding and decoding are supported. For example, the h263\n"
"decoder corresponds to the h263 and h263p encoders, for file formats it is even\n"
"worse.\n");
}

void show_bsfs(void)
{
    AVBitStreamFilter *bsf=NULL;

    printf("Bitstream filters:\n");
    while((bsf = av_bitstream_filter_next(bsf)))
        printf("%s\n", bsf->name);
    printf("\n");
}

void show_protocols(void)
{
    URLProtocol *up=NULL;

    printf("Supported file protocols:\n"
           "I.. = Input  supported\n"
           ".O. = Output supported\n"
           "..S = Seek   supported\n"
           "FLAGS NAME\n"
           "----- \n");
    while((up = av_protocol_next(up)))
        printf("%c%c%c   %s\n",
               up->url_read  ? 'I' : '.',
               up->url_write ? 'O' : '.',
               up->url_seek  ? 'S' : '.',
               up->name);
}

void show_filters(void)
{
    AVFilter av_unused(**filter) = NULL;

    printf("Filters:\n");
#if CONFIG_AVFILTER
    while ((filter = av_filter_next(filter)) && *filter)
        printf("%-16s %s\n", (*filter)->name, (*filter)->description);
#endif
}

void show_pix_fmts(void)
{
    enum PixelFormat pix_fmt;

    printf(
        "Pixel formats:\n"
        "I.... = Supported Input  format for conversion\n"
        ".O... = Supported Output format for conversion\n"
        "..H.. = Hardware accelerated format\n"
        "...P. = Paletted format\n"
        "....B = Bitstream format\n"
        "FLAGS NAME            NB_COMPONENTS BITS_PER_PIXEL\n"
        "-----\n");

#if !CONFIG_SWSCALE
#   define sws_isSupportedInput(x)  0
#   define sws_isSupportedOutput(x) 0
#endif

    for (pix_fmt = 0; pix_fmt < PIX_FMT_NB; pix_fmt++) {
        const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[pix_fmt];
        printf("%c%c%c%c%c %-16s       %d            %2d\n",
               sws_isSupportedInput (pix_fmt)      ? 'I' : '.',
               sws_isSupportedOutput(pix_fmt)      ? 'O' : '.',
               pix_desc->flags & PIX_FMT_HWACCEL   ? 'H' : '.',
               pix_desc->flags & PIX_FMT_PAL       ? 'P' : '.',
               pix_desc->flags & PIX_FMT_BITSTREAM ? 'B' : '.',
               pix_desc->name,
               pix_desc->nb_components,
               av_get_bits_per_pixel(pix_desc));
    }
}

int read_yesno(void)
{
    int c = getchar();
    int yesno = (toupper(c) == 'Y');

    while (c != '\n' && c != EOF)
        c = getchar();

    return yesno;
}

int read_file(const char *filename, char **bufptr, size_t *size)
{
    FILE *f = fopen(filename, "rb");

    if (!f) {
        fprintf(stderr, "Cannot read file '%s': %s\n", filename, strerror(errno));
        return AVERROR(errno);
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *bufptr = av_malloc(*size + 1);
    if (!*bufptr) {
        fprintf(stderr, "Could not allocate file buffer\n");
        fclose(f);
        return AVERROR(ENOMEM);
    }
    fread(*bufptr, 1, *size, f);
    (*bufptr)[*size++] = '\0';

    fclose(f);
    return 0;
}

FILE *get_preset_file(char *filename, size_t filename_size,
                      const char *preset_name, int is_path, const char *codec_name)
{
    FILE *f = NULL;
    int i;
    const char *base[3]= { getenv("FFMPEG_DATADIR"),
                           getenv("HOME"),
                           FFMPEG_DATADIR,
                         };

    if (is_path) {
        av_strlcpy(filename, preset_name, filename_size);
        f = fopen(filename, "r");
    } else {
        for (i = 0; i < 3 && !f; i++) {
            if (!base[i])
                continue;
            snprintf(filename, filename_size, "%s%s/%s.ffpreset", base[i], i != 1 ? "" : "/.ffmpeg", preset_name);
            f = fopen(filename, "r");
            if (!f && codec_name) {
                snprintf(filename, filename_size,
                         "%s%s/%s-%s.ffpreset", base[i],  i != 1 ? "" : "/.ffmpeg", codec_name, preset_name);
                f = fopen(filename, "r");
            }
        }
    }

    return f;
}

#if CONFIG_AVFILTER

static int ffsink_init(AVFilterContext *ctx, const char *args, void *opaque)
{
    FFSinkContext *priv = ctx->priv;

    if (!opaque)
        return AVERROR(EINVAL);
    *priv = *(FFSinkContext *)opaque;

    return 0;
}

static void null_end_frame(AVFilterLink *inlink) { }

static int ffsink_query_formats(AVFilterContext *ctx)
{
    FFSinkContext *priv = ctx->priv;
    enum PixelFormat pix_fmts[] = { priv->pix_fmt, PIX_FMT_NONE };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

AVFilter ffsink = {
    .name      = "ffsink",
    .priv_size = sizeof(FFSinkContext),
    .init      = ffsink_init,

    .query_formats = ffsink_query_formats,

    .inputs    = (AVFilterPad[]) {{ .name          = "default",
                                    .type          = AVMEDIA_TYPE_VIDEO,
                                    .end_frame     = null_end_frame,
                                    .min_perms     = AV_PERM_READ, },
                                  { .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name = NULL }},
};

int get_filtered_video_frame(AVFilterContext *ctx, AVFrame *frame,
                             AVFilterBufferRef **picref_ptr)
{
    int ret;
    AVFilterBufferRef *picref;

    *picref_ptr = NULL;
    if ((ret = avfilter_request_frame(ctx->inputs[0])) < 0)
        return ret;
    if (!(picref = ctx->inputs[0]->cur_buf))
        return AVERROR(ENOENT);
    *picref_ptr = picref;
    ctx->inputs[0]->cur_buf = NULL;

    memcpy(frame->data,     picref->data,     sizeof(frame->data));
    memcpy(frame->linesize, picref->linesize, sizeof(frame->linesize));
    frame->interlaced_frame = picref->video->interlaced;
    frame->top_field_first  = picref->video->top_field_first;

    return 1;
}

#endif /* CONFIG_AVFILTER */
