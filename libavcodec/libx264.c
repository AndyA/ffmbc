/*
 * H.264 encoding using the x264 library
 * Copyright (C) 2005  Mans Rullgard <mans@mansr.com>
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

#include "libavutil/opt.h"
#include "avcodec.h"
#include <x264.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct X264Context {
    AVClass        *class;
    x264_param_t    params;
    x264_t         *enc;
    x264_picture_t  pic;
    uint8_t        *sei;
    int             sei_size;
    AVFrame         out_pic;
    const char *preset;
    const char *tune;
    const char *profile;
    const char *level;
    int fastfirstpass;
    const char *keyint;
    const char *keyint_min;
    const char *intra_refresh;
    const char *crf;
    const char *crf_max;
    unsigned bitrate;
    const char *qp;
    const char *bframes;
    const char *b_adapt;
    const char *b_pyramid;
    const char *bframe_bias;
    const char min_keyint;
    const char *scenecut;
    const char *deblock;
    const char *qcomp;
    const char *qblur;
    const char *cplxblur;
    const char *partitions;
    const char *qpmin;
    const char *qpmax;
    const char *qpstep;
    const char *refs;
    int cabac;
    const char *me;
    const char *directpred;
    const char *weightb;
    const char *weightp;
    const char *aq_mode;
    const char *aq_strength;
    const char *rc_lookahead;
    const char *threads;
    int psy;
    const char *psy_rd;
    const char *me_range;
    const char *subme;
    const char *mixed_refs;
    const char *chroma_me;
    const char *dct8x8;
    const char *aud;
    const char *ipratio;
    const char *pbratio;
    const char *chroma_qp_offset;
    unsigned vbv_maxrate;
    unsigned vbv_bufsize;
    const char *vbv_init;
    const char *stats;
} X264Context;

#define OFFSET(x) offsetof(X264Context,x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[]={
    {"preset", "Set the encoding preset", OFFSET(preset), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"tune", "Tune the encoding params", OFFSET(tune), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"fastfirstpass", "Use fast settings when encoding first pass", OFFSET(fastfirstpass), FF_OPT_TYPE_INT, 0, 0, 1, VE},
    {"profile", "Set profile restrictions", OFFSET(profile), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"level", "Specify level (as defined by Annex A)", OFFSET(level), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"g", "Maximum GOP size", OFFSET(keyint), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"intra_refresh", "Use Periodic Intra Refresh instead of IDR frames", OFFSET(intra_refresh), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"crf", "Quality-based VBR", OFFSET(crf), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"crf_max", "With CRF+VBV, limit RF to this value", OFFSET(crf_max), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"cqp", "Force constant QP (0=lossless)", OFFSET(qp), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qscale", "Force constant QP (0=lossless)", OFFSET(qp), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"b", "Set bitrate (in bits/s)", OFFSET(bitrate), FF_OPT_TYPE_INT, 0, 0, INT_MAX, VE},
    {"bf", "Number of B-frames between I and P", OFFSET(bframes), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"b_strategy", "Adaptive B-frame decision method, higher values may lower threading efficiency: 0: Disabled, 1: Fast", OFFSET(b_adapt), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"b_adapt", "Adaptive B-frame decision method, higher values may lower threading efficiency: 0: Disabled, 1: Fast", OFFSET(b_adapt), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"b_pyramid", "Keep some B-frames as reference: none: Disabled, strict: Strictly hierarchical pyramid, normal: Non-strict (not Blu-ray compatible)", OFFSET(b_pyramid), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"bframebias", "Influences how often B-frames are used", OFFSET(bframe_bias), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"keyint_min", "Minimum GOP size", OFFSET(keyint_min), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"sc_threshold", "Scene change threshold", OFFSET(scenecut), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"deblock", "Loop filter parameters <alpha:beta>", OFFSET(deblock), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qmin", "Set min QP", OFFSET(qpmin), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qmax", "Set max QP", OFFSET(qpmax), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qdiff", "Set max QP step", OFFSET(qpstep), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qcomp", "QP curve compression <float>", OFFSET(qcomp), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"qblur", "Reduce fluctuations in QP (after curve compression) <float>", OFFSET(qblur), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"complexityblur", "Reduce fluctuations in QP (before curve compression) <float>", OFFSET(cplxblur), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"partitions", "Partitions to consider: p8x8, p4x4, b8x8, i8x8, i4x4, none, all", OFFSET(partitions), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"refs", "Number of reference frames", OFFSET(refs), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"coder", "0: cavlc, 1: cabac", OFFSET(cabac), FF_OPT_TYPE_INT, 1, 0, 1, VE},
    {"me_method", "Integer pixel motion estimation method", OFFSET(me), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"me", "Integer pixel motion estimation method", OFFSET(me), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"directpred", "Direct MV prediction mode: none, spatial, temporal, auto", OFFSET(directpred), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"weightb", "Weighted prediction for B-frames", OFFSET(weightb), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"wpredp", "Weighted prediction for P-frames: 0: Disabled, 1: Weighted refs, 2: Weighted refs + Duplicates", OFFSET(weightp), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"aq_mode", "AQ method: 0: Disabled, 1: Variance AQ (complexity mask), 2: Auto-variance AQ (experimental)", OFFSET(aq_mode), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"aq_strength", "Reduces blocking and blurring in flat and textured areas", OFFSET(aq_strength), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"rc_lookahead", "Number of frames for frametype lookahead", OFFSET(rc_lookahead), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"threads", "Force a specific number of threads", OFFSET(threads), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"psy", "Psychovisual Optimization: 0: Disabled", OFFSET(psy), FF_OPT_TYPE_INT, 1, 0, 1, VE},
    {"psy_rd", "Strength of psychovisual optimization <rd:trellis>: RD (requires subme>=6), Trellis (requires trellis)", OFFSET(psy_rd), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"me_range", "Maximum motion vector search range", OFFSET(me_range), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"subq", "Subpixel motion estimation and mode decision", OFFSET(subme), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"subme", "Subpixel motion estimation and mode decision", OFFSET(subme), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"mixed_refs", "Decide references on a per partition basis", OFFSET(mixed_refs), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"chroma_me", "Use chroma in motion estimation", OFFSET(chroma_me), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"8x8dct", "Use adaptive spatial transform size", OFFSET(dct8x8), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"aud", "Use access unit delimiters", OFFSET(aud), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"ipratio", "QP factor between I and P", OFFSET(ipratio), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"pbratio", "QP factor between P and B", OFFSET(pbratio), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"chromaoffset", "QP difference between chroma and luma", OFFSET(chroma_qp_offset), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"vbv_maxrate", "Max local bitrate (bit/s)", OFFSET(vbv_maxrate), FF_OPT_TYPE_INT, 0, 0, INT_MAX, VE},
    {"vbv_bufsize", "Set size of the VBV buffer (bits)", OFFSET(vbv_bufsize), FF_OPT_TYPE_INT, 0, 0, INT_MAX, VE},
    {"vbv_init", "Initial VBV buffer occupancy <float>", OFFSET(vbv_init), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {"passlogfile", " Filename for 2 pass stats", OFFSET(stats), FF_OPT_TYPE_STRING, 0, 0, 0, VE},
    {NULL}
};

static const AVClass class = { "libx264", av_default_item_name, options, LIBAVUTIL_VERSION_INT };

static void X264_log(void *p, int level, const char *fmt, va_list args)
{
    static const int level_map[] = {
        [X264_LOG_ERROR]   = AV_LOG_ERROR,
        [X264_LOG_WARNING] = AV_LOG_WARNING,
        [X264_LOG_INFO]    = AV_LOG_INFO,
        [X264_LOG_DEBUG]   = AV_LOG_DEBUG
    };

    if (level < 0 || level > X264_LOG_DEBUG)
        return;

    av_vlog(p, level_map[level], fmt, args);
}


static int encode_nals(AVCodecContext *ctx, uint8_t *buf, int size,
                       x264_nal_t *nals, int nnal, int skip_sei)
{
    uint8_t *p = buf;
    int i;

    for (i = 0; i < nnal; i++){
        memcpy(p, nals[i].p_payload, nals[i].i_payload);
        p += nals[i].i_payload;
    }

    return p - buf;
}

static int X264_frame(AVCodecContext *ctx, uint8_t *buf,
                      int bufsize, void *data)
{
    X264Context *x4 = ctx->priv_data;
    AVFrame *frame = data;
    x264_nal_t *nal;
    int nnal, i;
    x264_picture_t pic_out;

    x264_picture_init( &x4->pic );
    x4->pic.img.i_csp   = X264_CSP_I420;
    x4->pic.img.i_plane = 3;

    if (frame) {
        for (i = 0; i < 3; i++) {
            x4->pic.img.plane[i]    = frame->data[i];
            x4->pic.img.i_stride[i] = frame->linesize[i];
        }

        x4->pic.i_pts  = frame->pts;
        x4->pic.i_type =
            frame->pict_type == FF_I_TYPE ? X264_TYPE_KEYFRAME :
            frame->pict_type == FF_P_TYPE ? X264_TYPE_P :
            frame->pict_type == FF_B_TYPE ? X264_TYPE_B :
                                            X264_TYPE_AUTO;
        if (x4->params.b_tff != frame->top_field_first) {
            x4->params.b_tff = frame->top_field_first;
            x264_encoder_reconfig(x4->enc, &x4->params);
        }
    }

    do {
    if (x264_encoder_encode(x4->enc, &nal, &nnal, frame? &x4->pic: NULL, &pic_out) < 0)
        return -1;

    bufsize = encode_nals(ctx, buf, bufsize, nal, nnal, 0);
    if (bufsize < 0)
        return -1;
    } while (!bufsize && !frame && x264_encoder_delayed_frames(x4->enc));

    /* FIXME: libx264 now provides DTS, but AVFrame doesn't have a field for it. */
    x4->out_pic.pts = pic_out.i_pts;

    switch (pic_out.i_type) {
    case X264_TYPE_IDR:
    case X264_TYPE_I:
        x4->out_pic.pict_type = FF_I_TYPE;
        break;
    case X264_TYPE_P:
        x4->out_pic.pict_type = FF_P_TYPE;
        break;
    case X264_TYPE_B:
    case X264_TYPE_BREF:
        x4->out_pic.pict_type = FF_B_TYPE;
        break;
    }

    x4->out_pic.key_frame = pic_out.b_keyframe;
    if (bufsize)
        x4->out_pic.quality = (pic_out.i_qpplus1 - 1) * FF_QP2LAMBDA;

    return bufsize;
}

static av_cold int X264_close(AVCodecContext *avctx)
{
    X264Context *x4 = avctx->priv_data;

    av_freep(&avctx->extradata);
    av_free(x4->sei);

    if (x4->enc)
        x264_encoder_close(x4->enc);

    return 0;
}

#define OPT_STR(opt, param)                                             \
    do {                                                                \
        if (param && x264_param_parse(&x4->params, opt, param) < 0) {   \
            av_log(avctx, AV_LOG_ERROR,                                 \
                   "bad value for '%s': '%s'\n", opt, param);           \
            return -1;                                                  \
        }                                                               \
    } while (0);                                                        \

static av_cold int X264_init(AVCodecContext *avctx)
{
    X264Context *x4 = avctx->priv_data;

    x4->sei_size = 0;
    x264_param_default(&x4->params);

    if (x4->preset || x4->tune) {
        if (x264_param_default_preset(&x4->params, x4->preset, x4->tune) < 0)
            return -1;
    }

    x4->params.pf_log = X264_log;
    x4->params.p_log_private = avctx;

    if (avctx->gop_size == 0)
        x4->params.i_keyint_max = 0;
    OPT_STR("keyint", x4->keyint);
    OPT_STR("intra-refresh", x4->intra_refresh);

    if (x4->bitrate) {
        x4->params.rc.i_bitrate = x4->bitrate / 1000;
        x4->params.rc.i_rc_method = X264_RC_ABR;
    }

    OPT_STR("qp", x4->qp);
    OPT_STR("crf", x4->crf);
    OPT_STR("crf-max", x4->crf_max);

    if (x4->vbv_bufsize)
        x4->params.rc.i_vbv_buffer_size = x4->vbv_bufsize / 1000;
    else
        x4->params.rc.i_vbv_buffer_size = avctx->rc_buffer_size / 1000;
    if (x4->vbv_maxrate)
        x4->params.rc.i_vbv_max_bitrate = x4->vbv_maxrate / 1000;
    else
        x4->params.rc.i_vbv_max_bitrate = avctx->rc_max_rate / 1000;

    OPT_STR("vbv-init", x4->vbv_init);
    OPT_STR("stats", x4->stats);
    x4->params.rc.b_stat_write = avctx->flags & CODEC_FLAG_PASS1;
    x4->params.rc.b_stat_read = avctx->flags & CODEC_FLAG_PASS2;

    x4->params.b_cabac = x4->cabac;

    OPT_STR("bframes", x4->bframes);
    OPT_STR("b-adapt", x4->b_adapt);
    OPT_STR("b-bias", x4->bframe_bias);
    OPT_STR("b-pyramid", x4->b_pyramid);
    if (avctx->flags2 & CODEC_FLAG2_BPYRAMID)
        x4->params.i_bframe_pyramid = X264_B_PYRAMID_NORMAL;
    OPT_STR("keyint-min", x4->keyint_min);
    OPT_STR("scenecut", x4->scenecut);
    OPT_STR("deblock", x4->deblock);
    if (avctx->flags & CODEC_FLAG_LOOP_FILTER)
        x4->params.b_deblocking_filter = 1;

    OPT_STR("qpmin", x4->qpmin);
    OPT_STR("qpmax", x4->qpmax);
    OPT_STR("qpstep", x4->qpstep);
    OPT_STR("qcomp", x4->qcomp);
    OPT_STR("qblur", x4->qblur);
    OPT_STR("cplxblur", x4->cplxblur);

    OPT_STR("ref", x4->refs);

    x4->params.i_width              = avctx->width;
    x4->params.i_height             = avctx->height;
    x4->params.vui.i_sar_width      = avctx->sample_aspect_ratio.num;
    x4->params.vui.i_sar_height     = avctx->sample_aspect_ratio.den;
    x4->params.i_fps_num = x4->params.i_timebase_den = avctx->time_base.den;
    x4->params.i_fps_den = x4->params.i_timebase_num = avctx->time_base.num;

    OPT_STR("partitions", x4->partitions);
    OPT_STR("direct-pred", x4->directpred);

    OPT_STR("weightb", x4->weightb);
    if (avctx->flags2 & CODEC_FLAG2_WPRED)
        x4->params.analyse.b_weighted_bipred = 1;

    OPT_STR("weightp", x4->weightp);

    OPT_STR("me", x4->me);
    OPT_STR("me-range", x4->me_range);
    OPT_STR("subme", x4->subme);

    x4->params.analyse.b_psy = x4->psy;
    OPT_STR("psy-rd", x4->psy_rd);
    OPT_STR("aq-mode", x4->aq_mode);
    OPT_STR("aq-strength", x4->aq_strength);

    OPT_STR("rc-lookahead", x4->rc_lookahead);

    OPT_STR("mixed-refs", x4->mixed_refs);
    if (avctx->flags2 & CODEC_FLAG2_MIXED_REFS)
        x4->params.analyse.b_mixed_references = 1;
    OPT_STR("chroma-me", x4->chroma_me);
    if (avctx->me_cmp & FF_CMP_CHROMA)
        x4->params.analyse.b_chroma_me = 1;
    OPT_STR("8x8dct", x4->dct8x8);
    if (avctx->flags2 & CODEC_FLAG2_8X8DCT)
        x4->params.analyse.b_transform_8x8 = 1;
    OPT_STR("aud", x4->aud);
    if (avctx->flags2 & CODEC_FLAG2_AUD)
        x4->params.b_aud = 1;

    x4->params.analyse.i_trellis = avctx->trellis;
    x4->params.analyse.i_noise_reduction = avctx->noise_reduction;

    OPT_STR("ipratio", x4->ipratio);
    OPT_STR("pbratio", x4->pbratio);

    OPT_STR("chroma-qp-offset", x4->chroma_qp_offset);

    x4->params.i_log_level    = X264_LOG_DEBUG;

    OPT_STR("threads", x4->threads);

    x4->params.analyse.b_psnr = avctx->flags & CODEC_FLAG_PSNR;
    x4->params.analyse.b_ssim = avctx->flags2 & CODEC_FLAG2_SSIM;

    x4->params.b_interlaced   = avctx->flags & CODEC_FLAG_INTERLACED_DCT;

    x4->params.i_slice_count  = avctx->slices;

    x4->params.vui.b_fullrange = avctx->pix_fmt == PIX_FMT_YUVJ420P;

    if (avctx->flags & CODEC_FLAG_GLOBAL_HEADER)
        x4->params.b_repeat_headers = 0;

    OPT_STR("level", x4->level);

    if (x4->fastfirstpass)
        x264_param_apply_fastfirstpass(&x4->params);

    if (x4->profile)
        if (x264_param_apply_profile(&x4->params, x4->profile) < 0)
            return -1;

    avctx->has_b_frames = x4->params.i_bframe_pyramid ? 2 : !!x4->params.i_bframe;
    avctx->bit_rate = x4->bitrate;
    if (x4->params.rc.i_rc_method == X264_RC_CRF)
        avctx->crf = x4->params.rc.f_rf_constant;
    else if (x4->params.rc.i_rc_method == X264_RC_CQP)
        avctx->cqp = x4->params.rc.i_qp_constant;
    avctx->qmin = x4->params.rc.i_qp_min;
    avctx->qmax = x4->params.rc.i_qp_max;

    x4->enc = x264_encoder_open(&x4->params);
    if (!x4->enc)
        return -1;

    avctx->coded_frame = &x4->out_pic;

    if (avctx->flags & CODEC_FLAG_GLOBAL_HEADER) {
        x264_nal_t *nal;
        int nnal, s, i;

        s = x264_encoder_headers(x4->enc, &nal, &nnal);

        for (i = 0; i < nnal; i++)
            if (nal[i].i_type == NAL_SEI)
                av_log(avctx, AV_LOG_INFO, "%s\n", nal[i].p_payload+25);

        avctx->extradata      = av_malloc(s);
        avctx->extradata_size = encode_nals(avctx, avctx->extradata, s, nal, nnal, 1);
    }

    return 0;
}

AVCodec ff_libx264_encoder = {
    .name           = "libx264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .priv_data_size = sizeof(X264Context),
    .init           = X264_init,
    .encode         = X264_frame,
    .close          = X264_close,
    .capabilities   = CODEC_CAP_DELAY,
    .pix_fmts       = (const enum PixelFormat[]) { PIX_FMT_YUV420P, PIX_FMT_YUVJ420P, PIX_FMT_NONE },
    .long_name      = NULL_IF_CONFIG_SMALL("libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"),
    .priv_class     = &class,
};
