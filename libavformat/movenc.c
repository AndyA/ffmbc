/*
 * MOV, 3GP, MP4 muxer
 * Copyright (c) 2003 Thomas Raivio
 * Copyright (c) 2004 Gildas Bazin <gbazin at videolan dot org>
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

#include "movenc.h"
#include "avformat.h"
#include "riff.h"
#include "avio.h"
#include "isom.h"
#include "avc.h"
#include "avlanguage.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/timecode.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"

#undef NDEBUG
#include <assert.h>

//FIXME support 64 bit variant with wide placeholders
static int64_t updateSize(ByteIOContext *pb, int64_t pos)
{
    int64_t curpos = url_ftell(pb);
    url_fseek(pb, pos, SEEK_SET);
    put_be32(pb, curpos - pos); /* rewrite size */
    url_fseek(pb, curpos, SEEK_SET);

    return curpos - pos;
}

/* Chunk offset atom */
static int mov_write_stco_tag(ByteIOContext *pb, MOVMuxContext *mov,
                              MOVTrack *track)
{
    int i;
    int mode64 = 0; //   use 32 bit size variant if possible
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    if (track->cluster[track->entry-1].pos+mov->stco_offset > UINT32_MAX) {
        mode64 = 1;
        put_tag(pb, "co64");
    } else
        put_tag(pb, "stco");
    put_be32(pb, 0); /* version & flags */
    put_be32(pb, track->entry); /* entry count */
    for (i=0; i<track->entry; i++) {
        if(mode64 == 1)
            put_be64(pb, track->cluster[i].pos+mov->stco_offset);
        else
            put_be32(pb, track->cluster[i].pos+mov->stco_offset);
    }
    return updateSize(pb, pos);
}

/* Sample size atom */
static int mov_write_stsz_tag(ByteIOContext *pb, MOVTrack *track)
{
    int equalChunks = 1;
    int i, j, entries = 0, tst = -1, oldtst = -1;

    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "stsz");
    put_be32(pb, 0); /* version & flags */

    for (i=0; i<track->entry; i++) {
        tst = track->cluster[i].size/track->cluster[i].entries;
        if(oldtst != -1 && tst != oldtst) {
            equalChunks = 0;
        }
        oldtst = tst;
        entries += track->cluster[i].entries;
    }
    if (equalChunks) {
        int sSize = track->cluster[0].size/track->cluster[0].entries;
        sSize = FFMAX(1, sSize); // adpcm mono case could make sSize == 0
        put_be32(pb, sSize); // sample size
        put_be32(pb, entries); // sample count
    }
    else {
        put_be32(pb, 0); // sample size
        put_be32(pb, entries); // sample count
        for (i=0; i<track->entry; i++) {
            for (j=0; j<track->cluster[i].entries; j++) {
                put_be32(pb, track->cluster[i].size /
                         track->cluster[i].entries);
            }
        }
    }
    return updateSize(pb, pos);
}

/* Sample to chunk atom */
static int mov_write_stsc_tag(ByteIOContext *pb, MOVTrack *track)
{
    int index = 0, oldval = -1, i;
    int64_t entryPos, curpos;

    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "stsc");
    put_be32(pb, 0); // version & flags
    entryPos = url_ftell(pb);
    put_be32(pb, track->entry); // entry count
    for (i=0; i<track->entry; i++) {
        if(oldval != track->cluster[i].samplesInChunk)
        {
            put_be32(pb, i+1); // first chunk
            put_be32(pb, track->cluster[i].samplesInChunk); // samples per chunk
            put_be32(pb, 0x1); // sample description index
            oldval = track->cluster[i].samplesInChunk;
            index++;
        }
    }
    curpos = url_ftell(pb);
    url_fseek(pb, entryPos, SEEK_SET);
    put_be32(pb, index); // rewrite size
    url_fseek(pb, curpos, SEEK_SET);

    return updateSize(pb, pos);
}

/* Sync sample atom */
static int mov_write_stss_tag(ByteIOContext *pb, MOVTrack *track, uint32_t flag)
{
    int64_t curpos, entryPos;
    int i, index = 0;
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); // size
    put_tag(pb, flag == MOV_PARTIAL_SYNC_SAMPLE ? "stps" : "stss");
    put_be32(pb, 0); // version & flags
    entryPos = url_ftell(pb);
    put_be32(pb, track->entry); // entry count
    for (i=0; i<track->entry; i++) {
        if (track->cluster[i].flags & flag) {
            put_be32(pb, i+1);
            index++;
        }
    }
    curpos = url_ftell(pb);
    url_fseek(pb, entryPos, SEEK_SET);
    put_be32(pb, index); // rewrite size
    url_fseek(pb, curpos, SEEK_SET);
    return updateSize(pb, pos);
}

static int mov_write_amr_tag(ByteIOContext *pb, MOVTrack *track)
{
    /* We must find out how many AMR blocks there are in one packet */
    static uint16_t packed_size[16] =
        {13, 14, 16, 18, 20, 21, 27, 32, 6, 0, 0, 0, 0, 0, 0, 0};
    int len, frames_per_sample = 0;

    for (len = 0; len < track->vosLen;) {
        len += packed_size[(track->vosData[len] >> 3) & 0x0F];
        frames_per_sample++;
    }

    put_be32(pb, 0x11); /* size */
    if (track->mode == MODE_MOV) put_tag(pb, "samr");
    else                         put_tag(pb, "damr");
    put_tag(pb, "FFMP");
    put_byte(pb, 0); /* decoder version */

    put_be16(pb, 0x81FF); /* Mode set (all modes for AMR_NB) */
    put_byte(pb, 0x00); /* Mode change period (no restriction) */

    put_byte(pb, frames_per_sample); /* Frames per sample */
    return 0x11;
}

static int mov_write_ac3_tag(ByteIOContext *pb, MOVTrack *track)
{
    GetBitContext gbc;
    PutBitContext pbc;
    uint8_t buf[3];
    int fscod, bsid, bsmod, acmod, lfeon, frmsizecod;

    if (track->vosLen < 7)
        return -1;

    put_be32(pb, 11);
    put_tag(pb, "dac3");

    init_get_bits(&gbc, track->vosData+4, track->vosLen-4);
    fscod      = get_bits(&gbc, 2);
    frmsizecod = get_bits(&gbc, 6);
    bsid       = get_bits(&gbc, 5);
    bsmod      = get_bits(&gbc, 3);
    acmod      = get_bits(&gbc, 3);
    if (acmod == 2) {
        skip_bits(&gbc, 2); // dsurmod
    } else {
        if ((acmod & 1) && acmod != 1)
            skip_bits(&gbc, 2); // cmixlev
        if (acmod & 4)
            skip_bits(&gbc, 2); // surmixlev
    }
    lfeon = get_bits1(&gbc);

    init_put_bits(&pbc, buf, sizeof(buf));
    put_bits(&pbc, 2, fscod);
    put_bits(&pbc, 5, bsid);
    put_bits(&pbc, 3, bsmod);
    put_bits(&pbc, 3, acmod);
    put_bits(&pbc, 1, lfeon);
    put_bits(&pbc, 5, frmsizecod>>1); // bit_rate_code
    put_bits(&pbc, 5, 0); // reserved

    flush_put_bits(&pbc);
    put_buffer(pb, buf, sizeof(buf));

    return 11;
}

/**
 * This function writes extradata "as is".
 * Extradata must be formated like a valid atom (with size and tag)
 */
static int mov_write_extradata_tag(ByteIOContext *pb, MOVTrack *track)
{
    put_buffer(pb, track->enc->extradata, track->enc->extradata_size);
    return track->enc->extradata_size;
}

static int mov_write_enda_tag(ByteIOContext *pb)
{
    put_be32(pb, 10);
    put_tag(pb, "enda");
    put_be16(pb, 1); /* little endian */
    return 10;
}

static unsigned int descrLength(unsigned int len)
{
    int i;
    for(i=1; len>>(7*i); i++);
    return len + 1 + i;
}

static void putDescr(ByteIOContext *pb, int tag, unsigned int size)
{
    int i= descrLength(size) - size - 2;
    put_byte(pb, tag);
    for(; i>0; i--)
        put_byte(pb, (size>>(7*i)) | 0x80);
    put_byte(pb, size & 0x7F);
}

static int mov_write_esds_tag(ByteIOContext *pb, MOVTrack *track) // Basic
{
    int64_t pos = url_ftell(pb);
    int decoderSpecificInfoLen = track->vosLen ? descrLength(track->vosLen):0;

    put_be32(pb, 0); // size
    put_tag(pb, "esds");
    put_be32(pb, 0); // Version

    // ES descriptor
    putDescr(pb, 0x03, 3 + descrLength(13 + decoderSpecificInfoLen) +
             descrLength(1));
    put_be16(pb, track->trackID);
    put_byte(pb, 0x00); // flags (= no flags)

    // DecoderConfig descriptor
    putDescr(pb, 0x04, 13 + decoderSpecificInfoLen);

    // Object type indication
    if ((track->enc->codec_id == CODEC_ID_MP2 ||
         track->enc->codec_id == CODEC_ID_MP3) &&
        track->enc->sample_rate > 24000)
        put_byte(pb, 0x6B); // 11172-3
    else
        put_byte(pb, ff_codec_get_tag(ff_mp4_obj_type, track->enc->codec_id));

    // the following fields is made of 6 bits to identify the streamtype (4 for video, 5 for audio)
    // plus 1 bit to indicate upstream and 1 bit set to 1 (reserved)
    if(track->enc->codec_type == AVMEDIA_TYPE_AUDIO)
        put_byte(pb, 0x15); // flags (= Audiostream)
    else
        put_byte(pb, 0x11); // flags (= Visualstream)

    put_byte(pb,  track->enc->rc_buffer_size>>(3+16));    // Buffersize DB (24 bits)
    put_be16(pb, (track->enc->rc_buffer_size>>3)&0xFFFF); // Buffersize DB

    put_be32(pb, FFMAX(track->enc->bit_rate, track->enc->rc_max_rate)); // maxbitrate (FIXME should be max rate in any 1 sec window)
    if(track->enc->rc_max_rate != track->enc->rc_min_rate || track->enc->rc_min_rate==0)
        put_be32(pb, 0); // vbr
    else
        put_be32(pb, track->enc->rc_max_rate); // avg bitrate

    if (track->vosLen) {
        // DecoderSpecific info descriptor
        putDescr(pb, 0x05, track->vosLen);
        put_buffer(pb, track->vosData, track->vosLen);
    }

    // SL descriptor
    putDescr(pb, 0x06, 1);
    put_byte(pb, 0x02);
    return updateSize(pb, pos);
}

static int mov_pcm_le_gt16(enum CodecID codec_id)
{
    return codec_id == CODEC_ID_PCM_S24LE ||
           codec_id == CODEC_ID_PCM_S32LE ||
           codec_id == CODEC_ID_PCM_F32LE ||
           codec_id == CODEC_ID_PCM_F64LE;
}

static int mov_write_ms_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0);
    put_le32(pb, track->tag); // store it byteswapped
    track->enc->codec_tag = av_bswap16(track->tag >> 16);
    ff_put_wav_header(pb, track->enc);
    return updateSize(pb, pos);
}

static int mov_write_wave_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);

    put_be32(pb, 0);     /* size */
    put_tag(pb, "wave");

    put_be32(pb, 12);    /* size */
    put_tag(pb, "frma");
    put_le32(pb, track->tag);

    if (track->enc->codec_id == CODEC_ID_AAC) {
        /* useless atom needed by mplayer, ipod, not needed by quicktime */
        put_be32(pb, 12); /* size */
        put_tag(pb, "mp4a");
        put_be32(pb, 0);
        mov_write_esds_tag(pb, track);
    } else if (mov_pcm_le_gt16(track->enc->codec_id)) {
        mov_write_enda_tag(pb);
    } else if (track->enc->codec_id == CODEC_ID_AMR_NB) {
        mov_write_amr_tag(pb, track);
    } else if (track->enc->codec_id == CODEC_ID_AC3) {
        mov_write_ac3_tag(pb, track);
    } else if (track->enc->codec_id == CODEC_ID_ALAC ||
               track->enc->codec_id == CODEC_ID_QDM2) {
        mov_write_extradata_tag(pb, track);
    } else if (track->enc->codec_id == CODEC_ID_ADPCM_MS ||
               track->enc->codec_id == CODEC_ID_ADPCM_IMA_WAV) {
        mov_write_ms_tag(pb, track);
    }

    put_be32(pb, 8);     /* size */
    put_be32(pb, 0);     /* null tag */

    return updateSize(pb, pos);
}

static int mov_write_glbl_tag(ByteIOContext *pb, MOVTrack *track)
{
    put_be32(pb, track->vosLen+8);
    put_tag(pb, "glbl");
    put_buffer(pb, track->vosData, track->vosLen);
    return 8+track->vosLen;
}

/**
 * Compute flags for 'lpcm' tag.
 * See CoreAudioTypes and AudioStreamBasicDescription at Apple.
 */
static int mov_get_lpcm_flags(enum CodecID codec_id)
{
    switch (codec_id) {
    case CODEC_ID_PCM_F32BE:
    case CODEC_ID_PCM_F64BE:
        return 11;
    case CODEC_ID_PCM_F32LE:
    case CODEC_ID_PCM_F64LE:
        return 9;
    case CODEC_ID_PCM_U8:
        return 10;
    case CODEC_ID_PCM_S16BE:
    case CODEC_ID_PCM_S24BE:
    case CODEC_ID_PCM_S32BE:
        return 14;
    case CODEC_ID_PCM_S8:
    case CODEC_ID_PCM_S16LE:
    case CODEC_ID_PCM_S24LE:
    case CODEC_ID_PCM_S32LE:
        return 12;
    default:
        return 0;
    }
}

static int mov_write_audio_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    int version = 0;
    uint32_t tag = track->tag;

    if (track->mode == MODE_MOV) {
        if (track->timescale > UINT16_MAX) {
            if (mov_get_lpcm_flags(track->enc->codec_id))
                tag = AV_RL32("lpcm");
            version = 2;
        } else if (track->audio_vbr || mov_pcm_le_gt16(track->enc->codec_id) ||
                   track->enc->codec_id == CODEC_ID_ADPCM_MS ||
                   track->enc->codec_id == CODEC_ID_ADPCM_IMA_WAV) {
            version = 1;
        }
    }

    put_be32(pb, 0); /* size */
    put_le32(pb, tag); // store it byteswapped
    put_be32(pb, 0); /* Reserved */
    put_be16(pb, 0); /* Reserved */
    put_be16(pb, 1); /* Data-reference index, XXX  == 1 */

    /* SoundDescription */
    put_be16(pb, version); /* Version */
    put_be16(pb, 0); /* Revision level */
    put_be32(pb, 0); /* Reserved */

    if (version == 2) {
        put_be16(pb, 3);
        put_be16(pb, 16);
        put_be16(pb, 0xfffe);
        put_be16(pb, 0);
        put_be32(pb, 0x00010000);
        put_be32(pb, 72);
        put_be64(pb, av_dbl2int(track->timescale));
        put_be32(pb, track->enc->channels);
        put_be32(pb, 0x7F000000);
        put_be32(pb, av_get_bits_per_sample(track->enc->codec_id));
        put_be32(pb, mov_get_lpcm_flags(track->enc->codec_id));
        put_be32(pb, track->sampleSize);
        put_be32(pb, track->enc->frame_size);
    } else {
        if (track->mode == MODE_MOV) {
            put_be16(pb, track->enc->channels);
            if (track->enc->codec_id == CODEC_ID_PCM_U8 ||
                track->enc->codec_id == CODEC_ID_PCM_S8)
                put_be16(pb, 8); /* bits per sample */
            else
                put_be16(pb, 16);
            put_be16(pb, track->audio_vbr ? -2 : 0); /* compression ID */
        } else { /* reserved for mp4/3gp */
            put_be16(pb, 2);
            put_be16(pb, 16);
            put_be16(pb, 0);
        }

        put_be16(pb, 0); /* packet size (= 0) */
        put_be16(pb, track->timescale); /* Time scale */
        put_be16(pb, 0); /* Reserved */
    }

    if(version == 1) { /* SoundDescription V1 extended info */
        put_be32(pb, track->enc->frame_size); /* Samples per packet */
        put_be32(pb, track->sampleSize / track->enc->channels); /* Bytes per packet */
        put_be32(pb, track->sampleSize); /* Bytes per frame */
        put_be32(pb, 2); /* Bytes per sample */
    }

    if(track->mode == MODE_MOV &&
       (track->enc->codec_id == CODEC_ID_AAC ||
        track->enc->codec_id == CODEC_ID_AC3 ||
        track->enc->codec_id == CODEC_ID_AMR_NB ||
        track->enc->codec_id == CODEC_ID_ALAC ||
        track->enc->codec_id == CODEC_ID_ADPCM_MS ||
        track->enc->codec_id == CODEC_ID_ADPCM_IMA_WAV ||
        track->enc->codec_id == CODEC_ID_QDM2 ||
        mov_pcm_le_gt16(track->enc->codec_id)))
        mov_write_wave_tag(pb, track);
    else if(track->tag == MKTAG('m','p','4','a'))
        mov_write_esds_tag(pb, track);
    else if(track->enc->codec_id == CODEC_ID_AMR_NB)
        mov_write_amr_tag(pb, track);
    else if(track->enc->codec_id == CODEC_ID_AC3)
        mov_write_ac3_tag(pb, track);
    else if(track->enc->codec_id == CODEC_ID_ALAC)
        mov_write_extradata_tag(pb, track);
    else if(track->vosLen > 0)
        mov_write_glbl_tag(pb, track);

    return updateSize(pb, pos);
}

static int mov_write_d263_tag(ByteIOContext *pb)
{
    put_be32(pb, 0xf); /* size */
    put_tag(pb, "d263");
    put_tag(pb, "FFMP");
    put_byte(pb, 0); /* decoder version */
    /* FIXME use AVCodecContext level/profile, when encoder will set values */
    put_byte(pb, 0xa); /* level */
    put_byte(pb, 0); /* profile */
    return 0xf;
}

static int mov_write_avcc_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);

    put_be32(pb, 0);
    put_tag(pb, "avcC");
    ff_isom_write_avcc(track->enc, pb);
    return updateSize(pb, pos);
}

/* also used by all avid codecs (dv, imx, meridien) and their variants */
static int mov_write_avid_tag(ByteIOContext *pb, MOVTrack *track)
{
    int i;
    put_be32(pb, 24); /* size */
    put_tag(pb, "ACLR");
    put_tag(pb, "ACLR");
    put_tag(pb, "0001");
    put_be32(pb, 2); /* yuv 2 / rgb 1 ? */
    put_be32(pb, 0); /* unknown */

    put_be32(pb, 24); /* size */
    put_tag(pb, "APRG");
    put_tag(pb, "APRG");
    put_tag(pb, "0001");
    put_be32(pb, 1); /* unknown */
    put_be32(pb, 0); /* unknown */

    put_be32(pb, 120); /* size */
    put_tag(pb, "ARES");
    put_tag(pb, "ARES");
    put_tag(pb, "0001");
    put_be32(pb, AV_RB32(track->vosData + 0x28)); /* dnxhd cid, some id ? */
    put_be32(pb, track->enc->width);
    /* values below are based on samples created with quicktime and avid codecs */
    if (track->vosData[5] & 2) { // interlaced
        put_be32(pb, track->enc->height/2);
        put_be32(pb, 2); /* unknown */
        put_be32(pb, 0); /* unknown */
        put_be32(pb, 4); /* unknown */
    } else {
        put_be32(pb, track->enc->height);
        put_be32(pb, 1); /* unknown */
        put_be32(pb, 0); /* unknown */
        if (track->enc->height == 1080)
            put_be32(pb, 5); /* unknown */
        else
            put_be32(pb, 6); /* unknown */
    }
    /* padding */
    for (i = 0; i < 10; i++)
        put_be64(pb, 0);

    return 0;
}

static int mp4_get_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag = 0;

    if      (track->enc->codec_id == CODEC_ID_H264)      tag = MKTAG('a','v','c','1');
    else if (track->enc->codec_id == CODEC_ID_AC3)       tag = MKTAG('a','c','-','3');
    else if (track->enc->codec_id == CODEC_ID_PCM_S16BE) tag = MKTAG('t','w','o','s');
    else if (track->enc->codec_id == CODEC_ID_DIRAC)     tag = MKTAG('d','r','a','c');
    else if (track->enc->codec_id == CODEC_ID_MOV_TEXT)  tag = MKTAG('t','x','3','g');
    else if (track->enc->codec_type == AVMEDIA_TYPE_VIDEO) tag = MKTAG('m','p','4','v');
    else if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO) tag = MKTAG('m','p','4','a');

    if ((tag == AV_RL32("mp4v") || tag == AV_RL32("mp4a")) &&
        !ff_codec_get_tag(ff_mp4_obj_type, track->enc->codec_id))
        return 0;

    return tag;
}

static const AVCodecTag codec_ipod_tags[] = {
    { CODEC_ID_H264,   MKTAG('a','v','c','1') },
    { CODEC_ID_MPEG4,  MKTAG('m','p','4','v') },
    { CODEC_ID_AAC,    MKTAG('m','p','4','a') },
    { CODEC_ID_ALAC,   MKTAG('a','l','a','c') },
    { CODEC_ID_AC3,    MKTAG('a','c','-','3') },
    { CODEC_ID_MOV_TEXT, MKTAG('t','x','3','g') },
    { CODEC_ID_MOV_TEXT, MKTAG('t','e','x','t') },
    { CODEC_ID_NONE, 0 },
};

static int ipod_get_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag = track->enc->codec_tag;

    // keep original tag for subs, ipod supports both formats
    if (!(track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE &&
        (tag == MKTAG('t','x','3','g') ||
         tag == MKTAG('t','e','x','t'))))
        tag = ff_codec_get_tag(codec_ipod_tags, track->enc->codec_id);

    if (!av_match_ext(s->filename, "m4a") && !av_match_ext(s->filename, "m4v"))
        av_log(s, AV_LOG_WARNING, "Warning, extension is not .m4a nor .m4v "
               "Quicktime/Ipod might not play the file\n");

    return tag;
}

static int mov_get_dv_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag;

    if (track->enc->width == 720) /* SD */
        if (track->enc->height == 480) /* NTSC */
            if  (track->enc->pix_fmt == PIX_FMT_YUV422P) tag = MKTAG('d','v','5','n');
            else                                         tag = MKTAG('d','v','c',' ');
        else if (track->enc->pix_fmt == PIX_FMT_YUV422P) tag = MKTAG('d','v','5','p');
        else if (track->enc->pix_fmt == PIX_FMT_YUV420P) tag = MKTAG('d','v','c','p');
        else                                             tag = MKTAG('d','v','p','p');
    else if (track->enc->height == 720) /* HD 720 line */
        if  (track->enc->time_base.den == 50)            tag = MKTAG('d','v','h','q');
        else                                             tag = MKTAG('d','v','h','p');
    else if (track->enc->height == 1080) /* HD 1080 line */
        if  (track->enc->time_base.den == 25)            tag = MKTAG('d','v','h','5');
        else                                             tag = MKTAG('d','v','h','6');
    else {
        av_log(s, AV_LOG_ERROR, "unsupported height for dv codec\n");
        return 0;
    }

    return tag;
}

static const struct {
    enum PixelFormat pix_fmt;
    uint32_t tag;
    unsigned bps;
} mov_pix_fmt_tags[] = {
    { PIX_FMT_YUYV422, MKTAG('y','u','v','s'),  0 },
    { PIX_FMT_UYVY422, MKTAG('2','v','u','y'),  0 },
    { PIX_FMT_RGB555BE,MKTAG('r','a','w',' '), 16 },
    { PIX_FMT_RGB555LE,MKTAG('L','5','5','5'), 16 },
    { PIX_FMT_RGB565LE,MKTAG('L','5','6','5'), 16 },
    { PIX_FMT_RGB565BE,MKTAG('B','5','6','5'), 16 },
    { PIX_FMT_GRAY16BE,MKTAG('b','1','6','g'), 16 },
    { PIX_FMT_RGB24,   MKTAG('r','a','w',' '), 24 },
    { PIX_FMT_BGR24,   MKTAG('2','4','B','G'), 24 },
    { PIX_FMT_ARGB,    MKTAG('r','a','w',' '), 32 },
    { PIX_FMT_BGRA,    MKTAG('B','G','R','A'), 32 },
    { PIX_FMT_RGBA,    MKTAG('R','G','B','A'), 32 },
    { PIX_FMT_ABGR,    MKTAG('A','B','G','R'), 32 },
    { PIX_FMT_RGB48BE, MKTAG('b','4','8','r'), 48 },
};

static int mov_get_rawvideo_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag = track->enc->codec_tag;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(mov_pix_fmt_tags); i++) {
        if (track->enc->pix_fmt == mov_pix_fmt_tags[i].pix_fmt) {
            tag = mov_pix_fmt_tags[i].tag;
            track->enc->bits_per_coded_sample = mov_pix_fmt_tags[i].bps;
            break;
        }
    }

    return tag;
}

static int mov_get_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag = track->enc->codec_tag;

    if (!tag || (track->enc->strict_std_compliance >= FF_COMPLIANCE_NORMAL &&
                 (track->enc->codec_id == CODEC_ID_DVVIDEO ||
                  track->enc->codec_id == CODEC_ID_RAWVIDEO ||
                  track->enc->codec_id == CODEC_ID_H263 ||
                  av_get_bits_per_sample(track->enc->codec_id)))) { // pcm audio
        if (track->enc->codec_id == CODEC_ID_DVVIDEO)
            tag = mov_get_dv_codec_tag(s, track);
        else if (track->enc->codec_id == CODEC_ID_RAWVIDEO)
            tag = mov_get_rawvideo_codec_tag(s, track);
        else if (track->enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            tag = ff_codec_get_tag(codec_movvideo_tags, track->enc->codec_id);
            if (!tag) { // if no mac fcc found, try with Microsoft tags
                tag = ff_codec_get_tag(ff_codec_bmp_tags, track->enc->codec_id);
                if (tag)
                    av_log(s, AV_LOG_INFO, "Warning, using MS style video codec tag, "
                           "the file may be unplayable!\n");
            }
        } else if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO) {
            tag = ff_codec_get_tag(codec_movaudio_tags, track->enc->codec_id);
            if (!tag) { // if no mac fcc found, try with Microsoft tags
                int ms_tag = ff_codec_get_tag(ff_codec_wav_tags, track->enc->codec_id);
                if (ms_tag) {
                    tag = MKTAG('m', 's', ((ms_tag >> 8) & 0xff), (ms_tag & 0xff));
                    av_log(s, AV_LOG_INFO, "Warning, using MS style audio codec tag, "
                           "the file may be unplayable!\n");
                }
            }
        } else if (track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE)
            tag = ff_codec_get_tag(ff_codec_movsubtitle_tags, track->enc->codec_id);
    }

    return tag;
}

static const AVCodecTag codec_3gp_tags[] = {
    { CODEC_ID_H263,   MKTAG('s','2','6','3') },
    { CODEC_ID_H264,   MKTAG('a','v','c','1') },
    { CODEC_ID_MPEG4,  MKTAG('m','p','4','v') },
    { CODEC_ID_AAC,    MKTAG('m','p','4','a') },
    { CODEC_ID_AMR_NB, MKTAG('s','a','m','r') },
    { CODEC_ID_AMR_WB, MKTAG('s','a','w','b') },
    { CODEC_ID_MOV_TEXT, MKTAG('t','x','3','g') },
    { CODEC_ID_NONE, 0 },
};

static const AVCodecTag codec_f4v_tags[] = {
    { CODEC_ID_H264,   MKTAG('a','v','c','1') },
    { CODEC_ID_AAC,    MKTAG('m','p','4','a') },
    { CODEC_ID_MP3,    MKTAG('.','m','p','3') },
    { CODEC_ID_VP6F,   MKTAG('V','P','6','F') },
    { CODEC_ID_NONE, 0 },
};

static int mov_find_codec_tag(AVFormatContext *s, MOVTrack *track)
{
    int tag = track->enc->codec_tag;

    if (track->mode == MODE_MP4 || track->mode == MODE_PSP)
        tag = mp4_get_codec_tag(s, track);
    else if (track->mode == MODE_IPOD)
        tag = ipod_get_codec_tag(s, track);
    else if (track->mode & MODE_3GP)
        tag = ff_codec_get_tag(codec_3gp_tags, track->enc->codec_id);
    else if (track->mode & MODE_F4V)
        tag = ff_codec_get_tag(codec_f4v_tags, track->enc->codec_id);
    else
        tag = mov_get_codec_tag(s, track);

    return tag;
}

/** Write uuid atom.
 * Needed to make file play in iPods running newest firmware
 * goes after avcC atom in moov.trak.mdia.minf.stbl.stsd.avc1
 */
static int mov_write_uuid_tag_ipod(ByteIOContext *pb)
{
    put_be32(pb, 28);
    put_tag(pb, "uuid");
    put_be32(pb, 0x6b6840f2);
    put_be32(pb, 0x5f244fc5);
    put_be32(pb, 0xba39a51b);
    put_be32(pb, 0xcf0323f3);
    put_be32(pb, 0x0);
    return 28;
}

static int mov_write_subtitle_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0);    /* size */
    put_le32(pb, track->tag); // store it byteswapped
    put_be32(pb, 0);    /* Reserved */
    put_be16(pb, 0);    /* Reserved */
    put_be16(pb, 1);    /* Data-reference index */

    if (track->enc->extradata_size)
        put_buffer(pb, track->enc->extradata, track->enc->extradata_size);

    return updateSize(pb, pos);
}

static int mov_write_pasp_tag(ByteIOContext *pb, MOVTrack *track)
{
    AVRational sar;
    av_reduce(&sar.num, &sar.den, track->height*track->dar.num,
              track->enc->width*track->dar.den, INT_MAX);

    put_be32(pb, 16);
    put_tag(pb, "pasp");
    put_be32(pb, sar.num);
    put_be32(pb, sar.den);
    return 16;
}

static int mov_write_tapt_tag(ByteIOContext *pb, MOVTrack *track)
{
    int display_width;

    display_width = (uint64_t)track->height*track->dar.num/track->dar.den;

    put_be32(pb, 68);
    put_tag (pb, "tapt");
    put_be32(pb, 20);
    put_tag (pb, "clef");
    put_be32(pb, 0); // version + flags
    put_be32(pb, display_width<<16);
    put_be32(pb, track->height<<16);
    put_be32(pb, 20);
    put_tag (pb, "prof");
    put_be32(pb, 0); // version + flags
    put_be32(pb, display_width<<16);
    put_be32(pb, track->height<<16);
    put_be32(pb, 20);
    put_tag (pb, "enof");
    put_be32(pb, 0); // version + flags
    put_be32(pb, track->enc->width<<16);
    put_be32(pb, track->height    <<16);
    return 68;
}

static int mov_write_clap_tag(ByteIOContext *pb, MOVTrack *track)
{
    put_be32(pb, 40);
    put_tag(pb, "clap");
    put_be32(pb, track->enc->width);
    put_be32(pb, 1);
    put_be32(pb, track->height);
    put_be32(pb, 1);
    put_be32(pb, 0);
    put_be32(pb, 1);
    put_be32(pb, 0);
    put_be32(pb, 1);
    return 40;
}

static int mov_write_fiel_tag(ByteIOContext *pb, MOVTrack *track)
{
    put_be32(pb, 10);
    put_tag(pb, "fiel");
    put_byte(pb, 2);
    // mjpeg stores 2 field independantly, not blended
    if (track->enc->interlaced == 2) { // top field first
        if (track->enc->codec_id == CODEC_ID_MJPEG)
            put_byte(pb, 1);
        else
            put_byte(pb, 9);
    } else {
        if (track->enc->codec_id == CODEC_ID_MJPEG)
            put_byte(pb, 6);
        else
            put_byte(pb, 14);
    }
    return 10;
}

static int mov_write_colr_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    switch (track->enc->color_primaries) {
    case AVCOL_PRI_BT709:
        track->enc->color_transfer = AVCOL_TRC_BT709;
        track->enc->color_matrix = AVCOL_MTX_BT709;
        break;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_BT470BG:
        track->enc->color_transfer = AVCOL_TRC_BT709;
        track->enc->color_matrix = AVCOL_MTX_SMPTE170M;
        break;
    }

    if (track->enc->color_primaries == AVCOL_PRI_UNSPECIFIED &&
        track->enc->color_transfer == AVCOL_TRC_UNSPECIFIED &&
        track->enc->color_matrix == AVCOL_MTX_UNSPECIFIED) {
        if (track->enc->codec_id != CODEC_ID_H264 &&
            track->enc->codec_id != CODEC_ID_MPEG2VIDEO &&
            (track->enc->codec_id != CODEC_ID_RAWVIDEO ||
             track->enc->bits_per_coded_sample) && // RGB sets bps
            track->enc->codec_id != CODEC_ID_V210)
            return 0;
        if (track->enc->height >= 720) {
            av_log(s, AV_LOG_WARNING, "color primaries unspecified, assuming bt709\n");
            track->enc->color_primaries = AVCOL_PRI_BT709;
            track->enc->color_transfer = AVCOL_TRC_BT709;
            track->enc->color_matrix = AVCOL_MTX_BT709;
        } else if (track->enc->width == 720 && track->height == 576) {
            av_log(s, AV_LOG_WARNING, "color primaries unspecified, assuming bt470bg\n");
            track->enc->color_primaries = AVCOL_PRI_BT470BG;
            track->enc->color_transfer = AVCOL_TRC_BT709;
            track->enc->color_matrix = AVCOL_MTX_SMPTE170M;
        } else if (track->enc->width == 720 &&
                   (track->height == 486 || track->height == 480)) {
            av_log(s, AV_LOG_WARNING, "color primaries unspecified, assuming smpte170\n");
            track->enc->color_primaries = AVCOL_PRI_SMPTE170M;
            track->enc->color_transfer = AVCOL_TRC_BT709;
            track->enc->color_matrix = AVCOL_MTX_SMPTE170M;
        } else {
            return 0;
        }
    }

    put_be32(pb, 18);
    put_tag(pb, "colr");
    put_tag(pb, "nclc");
    switch (track->enc->color_primaries) {
    case AVCOL_PRI_BT709:     put_be16(pb, 1); break;
    case AVCOL_PRI_SMPTE170M: put_be16(pb, 6); break;
    case AVCOL_PRI_BT470BG:   put_be16(pb, 5); break;
    default:                  put_be16(pb, 2);
    }
    switch (track->enc->color_transfer) {
    case AVCOL_TRC_BT709:     put_be16(pb, 1); break;
    case AVCOL_TRC_SMPTE170M: put_be16(pb, 1); break; // remapped
    default:                  put_be16(pb, 2);
    }
    switch (track->enc->color_matrix) {
    case AVCOL_TRC_BT709:     put_be16(pb, 1); break;
    case AVCOL_PRI_SMPTE170M: put_be16(pb, 6); break;
    default:                  put_be16(pb, 2);
    }

    return 18;
}

static int mov_write_video_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    char compressor_name[32];
    int padding = 0;

    put_be32(pb, 0); /* size */
    put_le32(pb, track->tag); // store it byteswapped
    put_be32(pb, 0); /* Reserved */
    put_be16(pb, 0); /* Reserved */
    put_be16(pb, 1); /* Data-reference index */

    put_be16(pb, 0); /* Codec stream version */
    put_be16(pb, 0); /* Codec stream revision (=0) */
    if (track->mode == MODE_MOV) {
        put_tag(pb, "FFMP"); /* Vendor */
        if(track->enc->codec_id == CODEC_ID_RAWVIDEO) {
            put_be32(pb, 0); /* Temporal Quality */
            put_be32(pb, 0x400); /* Spatial Quality = lossless*/
        } else {
            put_be32(pb, 0x200); /* Temporal Quality = normal */
            put_be32(pb, 0x200); /* Spatial Quality = normal */
        }
    } else {
        put_be32(pb, 0); /* Reserved */
        put_be32(pb, 0); /* Reserved */
        put_be32(pb, 0); /* Reserved */
    }
    put_be16(pb, track->enc->width); /* Video width */
    put_be16(pb, track->height); /* Video height */
    put_be32(pb, 0x00480000); /* Horizontal resolution 72dpi */
    put_be32(pb, 0x00480000); /* Vertical resolution 72dpi */
    put_be32(pb, 0); /* Data size (= 0) */
    put_be16(pb, 1); /* Frame count (= 1) */

    memset(compressor_name,0,32);
    /* FIXME not sure, ISO 14496-1 draft where it shall be set to 0 */
    if (track->mode == MODE_MOV && track->enc->codec && track->enc->codec->name)
        strncpy(compressor_name,track->enc->codec->name,31);
    put_byte(pb, strlen(compressor_name));
    put_buffer(pb, compressor_name, 31);

    if (track->mode == MODE_MOV && track->enc->bits_per_coded_sample)
        put_be16(pb, track->enc->bits_per_coded_sample);
    else
        put_be16(pb, 0x18); /* Reserved */
    put_be16(pb, 0xffff); /* Reserved */
    if(track->tag == MKTAG('m','p','4','v'))
        mov_write_esds_tag(pb, track);
    else if(track->enc->codec_id == CODEC_ID_H263)
        mov_write_d263_tag(pb);
    else if(track->enc->codec_id == CODEC_ID_SVQ3) {
        mov_write_extradata_tag(pb, track);
    } else if(track->enc->codec_id == CODEC_ID_DNXHD) {
        mov_write_avid_tag(pb, track);
        padding = 1;
    } else if(track->enc->codec_id == CODEC_ID_H264) {
        mov_write_avcc_tag(pb, track);
        if(track->mode == MODE_IPOD)
            mov_write_uuid_tag_ipod(pb);
    } else if(track->enc->codec_id != CODEC_ID_MPEG2VIDEO &&
              track->vosLen > 0)
        mov_write_glbl_tag(pb, track);

    if (track->enc->sample_aspect_ratio.den > 0 &&
        track->enc->sample_aspect_ratio.num > 0 &&
        track->enc->sample_aspect_ratio.den !=
        track->enc->sample_aspect_ratio.num) {
        mov_write_pasp_tag(pb, track);
        if (track->mode == MODE_MOV)
            mov_write_clap_tag(pb, track);
        padding = 1;
    }

    if (track->mode == MODE_MOV) {
        if (track->enc->interlaced > 0) {
            mov_write_fiel_tag(pb, track);
            padding = 1;
        }
        if (mov_write_colr_tag(s, pb, track) > 0)
            padding = 1;
        if (padding)
            put_be32(pb, 0); // padding for FCP
    }

    return updateSize(pb, pos);
}

static int mov_write_rtp_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "rtp ");
    put_be32(pb, 0); /* Reserved */
    put_be16(pb, 0); /* Reserved */
    put_be16(pb, 1); /* Data-reference index */

    put_be16(pb, 1); /* Hint track version */
    put_be16(pb, 1); /* Highest compatible version */
    put_be32(pb, track->max_packet_size); /* Max packet size */

    put_be32(pb, 12); /* size */
    put_tag(pb, "tims");
    put_be32(pb, track->timescale);

    return updateSize(pb, pos);
}

static int mov_write_mac_string(ByteIOContext *pb, const char *name,
                                const char *value, const char *lang, int utf8)
{
    int64_t pos = url_ftell(pb);
    unsigned len = strlen(value);
    put_be32(pb, 0); /* size */
    put_tag(pb, name);
    put_be16(pb, len); /* string length */
    put_be16(pb, ff_mov_iso639_to_lang(lang, utf8));
    put_buffer(pb, value, len);
    return updateSize(pb, pos);
}

static int mov_write_tmcd_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    AVMetadataTag *t = av_metadata_get(s->metadata, "reel_name", NULL, 0);
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "tmcd");
    put_be32(pb, 0); /* reserved */
    put_be16(pb, 0); /* reserved */
    put_be16(pb, 1); /* data reference index */
    put_be32(pb, 0); /* reserved */
    put_be32(pb, !!(track->flags & MOV_TRACK_DROP_TC)); /* flags */
    put_be32(pb, track->timescale); /* timescale */
    put_be32(pb, track->enc->time_base.num); /* frame duration */
    put_byte(pb, av_rescale_rnd(track->timescale, 1, track->enc->time_base.num, AV_ROUND_UP)); /* number of frames */
    put_byte(pb, 0);
    if (t)
        mov_write_mac_string(pb, "name", t->value,
                             av_metadata_get_attribute(t, "language"), 0);
    return updateSize(pb, pos);
}

static int mov_write_stsd_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "stsd");
    put_be32(pb, 0); /* version & flags */
    put_be32(pb, 1); /* entry count */
    if (track->enc->codec_type == AVMEDIA_TYPE_VIDEO)
        mov_write_video_tag(s, pb, track);
    else if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO)
        mov_write_audio_tag(pb, track);
    else if (track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE)
        mov_write_subtitle_tag(pb, track);
    else if (track->enc->codec_tag == MKTAG('r','t','p',' '))
        mov_write_rtp_tag(pb, track);
    else if (track->enc->codec_tag == MKTAG('t','m','c','d'))
        mov_write_tmcd_tag(s, pb, track);

    return updateSize(pb, pos);
}

static int mov_write_cslg_tag(ByteIOContext *pb, MOVTrack *track)
{
    // version 1 does not seem to work
    // and I don't know the difference
    if (track->pts_duration >= INT32_MAX)
        return 0;

    put_be32(pb, 32);
    put_tag(pb, "cslg");
    put_be32(pb, 0); // version+flags
    put_be32(pb, track->delay); // dts shift
    put_be32(pb, track->min_cts - track->delay); // least dts to pts delta
    put_be32(pb, track->max_cts - track->delay); // greatest dts to pts delta
    put_be32(pb, 0); // pts start
    put_be32(pb, track->pts_duration); // pts end
    return 32;
}

static int mov_write_ctts_tag(ByteIOContext *pb, MOVTrack *track)
{
    MOVStts *ctts_entries;
    uint32_t entries = 0;
    uint32_t atom_size = 0;
    int i, offset = 0;

    if (track->mode == MODE_MOV)
        offset = track->delay;

    ctts_entries = av_malloc((track->entry + 1) * sizeof(*ctts_entries)); /* worst case */
    ctts_entries[0].count = 1;
    ctts_entries[0].duration = track->cluster[0].cts;
    for (i=1; i<track->entry; i++) {
        if (track->cluster[i].cts == ctts_entries[entries].duration) {
            ctts_entries[entries].count++; /* compress */
        } else {
            entries++;
            ctts_entries[entries].duration = track->cluster[i].cts;
            ctts_entries[entries].count = 1;
        }
    }
    if (!entries) // all cts are the same, constant delay
        goto out;
    entries++; /* last one */
    atom_size = 16 + (entries * 8);
    put_be32(pb, atom_size); /* size */
    put_tag(pb, "ctts");
    put_be32(pb, 0); /* version & flags */
    put_be32(pb, entries); /* entry count */
    for (i=0; i<entries; i++) {
        put_be32(pb, ctts_entries[i].count);
        put_be32(pb, ctts_entries[i].duration - offset);
    }
 out:
    av_free(ctts_entries);
    return atom_size;
}

/* Time to sample atom */
static int mov_write_stts_tag(ByteIOContext *pb, MOVTrack *track)
{
    MOVStts *stts_entries;
    uint32_t entries = -1;
    uint32_t atom_size;
    int i;

    if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO && !track->audio_vbr) {
        stts_entries = av_malloc(sizeof(*stts_entries)); /* one entry */
        stts_entries[0].count = track->sampleCount;
        stts_entries[0].duration = 1;
        entries = 1;
    } else {
        stts_entries = av_malloc(track->entry * sizeof(*stts_entries)); /* worst case */
        for (i=0; i<track->entry; i++) {
            int64_t duration = i + 1 == track->entry ?
                track->total_duration - track->cluster[i].dts + track->cluster[0].dts : /* readjusting */
                track->cluster[i+1].dts - track->cluster[i].dts;
            if (i && duration == stts_entries[entries].duration) {
                stts_entries[entries].count++; /* compress */
            } else {
                entries++;
                stts_entries[entries].duration = duration;
                stts_entries[entries].count = 1;
            }
        }
        entries++; /* last one */
    }
    atom_size = 16 + (entries * 8);
    put_be32(pb, atom_size); /* size */
    put_tag(pb, "stts");
    put_be32(pb, 0); /* version & flags */
    put_be32(pb, entries); /* entry count */
    for (i=0; i<entries; i++) {
        put_be32(pb, stts_entries[i].count);
        put_be32(pb, stts_entries[i].duration);
    }
    av_free(stts_entries);
    return atom_size;
}

static int mov_write_dref_tag(ByteIOContext *pb)
{
    put_be32(pb, 28); /* size */
    put_tag(pb, "dref");
    put_be32(pb, 0); /* version & flags */
    put_be32(pb, 1); /* entry count */

    put_be32(pb, 0xc); /* size */
    put_tag(pb, "url ");
    put_be32(pb, 1); /* version & flags */

    return 28;
}

static int mov_write_stbl_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "stbl");
    mov_write_stsd_tag(s, pb, track);
    mov_write_stts_tag(pb, track);
    if ((track->enc->codec_type == AVMEDIA_TYPE_VIDEO ||
         track->enc->codec_tag == MKTAG('r','t','p',' ')) &&
        track->hasKeyframes && track->hasKeyframes < track->entry) {
        mov_write_stss_tag(pb, track, track->mode == MODE_MOV ?
                           MOV_SYNC_SAMPLE :
                           MOV_SYNC_SAMPLE | MOV_PARTIAL_SYNC_SAMPLE);
    }
    if (track->mode == MODE_MOV && track->flags & MOV_TRACK_STPS)
        mov_write_stss_tag(pb, track, MOV_PARTIAL_SYNC_SAMPLE);
    if (track->enc->codec_type == AVMEDIA_TYPE_VIDEO &&
        track->flags & MOV_TRACK_CTTS) {
        int ret = mov_write_ctts_tag(pb, track);
        if (ret && track->mode == MODE_MOV)
            mov_write_cslg_tag(pb, track);
    }
    mov_write_stsc_tag(pb, track);
    mov_write_stsz_tag(pb, track);
    mov_write_stco_tag(pb, s->priv_data, track);
    return updateSize(pb, pos);
}

static int mov_write_dinf_tag(ByteIOContext *pb)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "dinf");
    mov_write_dref_tag(pb);
    return updateSize(pb, pos);
}

static int mov_write_nmhd_tag(ByteIOContext *pb)
{
    put_be32(pb, 12);
    put_tag(pb, "nmhd");
    put_be32(pb, 0);
    return 12;
}

static int mov_write_gmhd_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0);      /* size */
    put_tag(pb, "gmhd");
    put_be32(pb, 0x18);   /* gmin size */
    put_tag(pb, "gmin");  /* generic media info */
    put_be32(pb, 0);      /* version & flags */
    put_be16(pb, 0x40);   /* graphics mode = */
    put_be16(pb, 0x8000); /* opColor (r?) */
    put_be16(pb, 0x8000); /* opColor (g?) */
    put_be16(pb, 0x8000); /* opColor (b?) */
    put_be16(pb, 0);      /* balance */
    put_be16(pb, 0);      /* reserved */

    if (track->enc->codec_tag == MKTAG('t','m','c','d')) {
        /* tmcd atom */
        put_be32(pb, 47); /* size */
        put_tag(pb, "tmcd");

        /* tcmi atom */
        put_be32(pb, 39); /* size */
        put_tag(pb, "tcmi");
        put_be32(pb, 0); /* version & flags */
        put_be16(pb, 0); /* font */
        put_be16(pb, 0); /* face */
        put_be16(pb, 12); /* size */
        put_be16(pb, 0); /* reserved */
        put_be16(pb, 65535); /* fg color */
        put_be16(pb, 65535); /* fg color */
        put_be16(pb, 65535); /* fg color */
        put_be16(pb, 0); /* bg color */
        put_be16(pb, 0); /* bg color */
        put_be16(pb, 0); /* bg color */
        put_byte(pb, 6); /* font name length */
        put_buffer(pb, "System", 6);
    }

    return updateSize(pb, pos);
}

static int mov_write_smhd_tag(ByteIOContext *pb)
{
    put_be32(pb, 16); /* size */
    put_tag(pb, "smhd");
    put_be32(pb, 0); /* version & flags */
    put_be16(pb, 0); /* reserved (balance, normally = 0) */
    put_be16(pb, 0); /* reserved */
    return 16;
}

static int mov_write_vmhd_tag(ByteIOContext *pb)
{
    put_be32(pb, 0x14); /* size (always 0x14) */
    put_tag(pb, "vmhd");
    put_be32(pb, 0x01); /* version & flags */
    put_be64(pb, 0); /* reserved (graphics mode = copy) */
    return 0x14;
}

static int mov_write_hdlr_tag(ByteIOContext *pb, MOVTrack *track)
{
    const char *hdlr, *descr = NULL, *hdlr_type = NULL;
    int64_t pos = url_ftell(pb);

    if (!track) { /* no media --> data handler */
        hdlr = "dhlr";
        hdlr_type = "url ";
        descr = "DataHandler";
    } else {
        hdlr = (track->mode == MODE_MOV) ? "mhlr" : "\0\0\0\0";
        if (track->enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            hdlr_type = "vide";
            descr = "VideoHandler";
        } else if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO) {
            hdlr_type = "soun";
            descr = "SoundHandler";
        } else if (track->enc->codec_type == AVMEDIA_TYPE_DATA &&
                   track->enc->codec_tag == MKTAG('t','m','c','d')) {
            hdlr_type = "tmcd";
            descr = "TimeCodeHandler";
        } else if (track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            if (track->tag == MKTAG('t','x','3','g')) hdlr_type = "sbtl";
            else                                      hdlr_type = "text";
            descr = "SubtitleHandler";
        } else if (track->enc->codec_tag == MKTAG('r','t','p',' ')) {
            hdlr_type = "hint";
            descr = "HintHandler";
        }
    }

    put_be32(pb, 0); /* size */
    put_tag(pb, "hdlr");
    put_be32(pb, 0); /* Version & flags */
    put_buffer(pb, hdlr, 4); /* handler */
    put_tag(pb, hdlr_type); /* handler type */
    put_be32(pb ,0); /* reserved */
    put_be32(pb ,0); /* reserved */
    put_be32(pb ,0); /* reserved */
    if (!track || track->mode == MODE_MOV)
        put_byte(pb, strlen(descr)); /* pascal string */
    put_buffer(pb, descr, strlen(descr)); /* handler description */
    if (track && track->mode != MODE_MOV)
        put_byte(pb, 0); /* c string */
    return updateSize(pb, pos);
}

static int mov_write_hmhd_tag(ByteIOContext *pb)
{
    /* This atom must be present, but leaving the values at zero
     * seems harmless. */
    put_be32(pb, 28); /* size */
    put_tag(pb, "hmhd");
    put_be32(pb, 0); /* version, flags */
    put_be16(pb, 0); /* maxPDUsize */
    put_be16(pb, 0); /* avgPDUsize */
    put_be32(pb, 0); /* maxbitrate */
    put_be32(pb, 0); /* avgbitrate */
    put_be32(pb, 0); /* reserved */
    return 28;
}

static int mov_write_minf_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "minf");
    if(track->enc->codec_type == AVMEDIA_TYPE_VIDEO)
        mov_write_vmhd_tag(pb);
    else if (track->enc->codec_type == AVMEDIA_TYPE_AUDIO)
        mov_write_smhd_tag(pb);
    else if (track->enc->codec_type == AVMEDIA_TYPE_DATA)
        mov_write_gmhd_tag(pb, track);
    else if (track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        if (track->tag == MKTAG('t','e','x','t')) mov_write_gmhd_tag(pb, track);
        else                                      mov_write_nmhd_tag(pb);
    } else if (track->tag == MKTAG('r','t','p',' ')) {
        mov_write_hmhd_tag(pb);
    }
    if (track->mode == MODE_MOV) /* FIXME: Why do it for MODE_MOV only ? */
        mov_write_hdlr_tag(pb, NULL);
    mov_write_dinf_tag(pb);
    mov_write_stbl_tag(s, pb, track);
    return updateSize(pb, pos);
}

static int mov_write_mdhd_tag(ByteIOContext *pb, MOVTrack *track)
{
    int version = track->total_duration < INT32_MAX ? 0 : 1;

    (version == 1) ? put_be32(pb, 44) : put_be32(pb, 32); /* size */
    put_tag(pb, "mdhd");
    put_byte(pb, version);
    put_be24(pb, 0); /* flags */
    if (version == 1) {
        put_be64(pb, track->time);
        put_be64(pb, track->time);
    } else {
        put_be32(pb, track->time); /* creation time */
        put_be32(pb, track->time); /* modification time */
    }
    put_be32(pb, track->timescale); /* time scale (sample rate for audio) */
    if (version == 1)
        put_be64(pb, track->total_duration);
    else
        put_be32(pb, track->total_duration); /* duration */
    put_be16(pb, track->language); /* language */
    put_be16(pb, 0); /* reserved (quality) */

    if(version!=0 && track->mode == MODE_MOV){
        av_log(NULL, AV_LOG_ERROR,
            "FATAL error, file duration too long for timebase, this file will not be\n"
            "playable with quicktime. Choose a different timebase or a different\n"
            "container format\n");
    }

    return 32;
}

static int mov_write_mdia_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "mdia");
    mov_write_mdhd_tag(pb, track);
    mov_write_hdlr_tag(pb, track);
    mov_write_minf_tag(s, pb, track);
    return updateSize(pb, pos);
}

static int mov_write_tkhd_tag(ByteIOContext *pb, MOVTrack *track, AVStream *st)
{
    int64_t duration = av_rescale_rnd(track->edit_duration + track->pts_offset,
                                      MOV_TIMESCALE, track->timescale,
                                      AV_ROUND_UP);
    int version = duration < INT32_MAX ? 0 : 1;

    (version == 1) ? put_be32(pb, 104) : put_be32(pb, 92); /* size */
    put_tag(pb, "tkhd");
    put_byte(pb, version);
    if (track->mode == MODE_MOV)
        put_be24(pb, 0xf); /* flags (track enabled) */
    else if (track->tag == AV_RL32("rtp "))
        put_be24(pb, 0x0);
    else
        put_be24(pb, 0x7);
    if (version == 1) {
        put_be64(pb, track->time);
        put_be64(pb, track->time);
    } else {
        put_be32(pb, track->time); /* creation time */
        put_be32(pb, track->time); /* modification time */
    }
    put_be32(pb, track->trackID); /* track-id */
    put_be32(pb, 0); /* reserved */
    (version == 1) ? put_be64(pb, duration) : put_be32(pb, duration);

    put_be32(pb, 0); /* reserved */
    put_be32(pb, 0); /* reserved */
    put_be32(pb, 0x0); /* reserved (Layer & Alternate group) */
    /* Volume, only for audio */
    if(track->enc->codec_type == AVMEDIA_TYPE_AUDIO)
        put_be16(pb, 0x0100);
    else
        put_be16(pb, 0);
    put_be16(pb, 0); /* reserved */

    /* Matrix structure */
    put_be32(pb, 0x00010000); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x00010000); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x40000000); /* reserved */

    /* Track width and height, for visual only */
    if(track->enc->codec_type == AVMEDIA_TYPE_VIDEO ||
       track->enc->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        put_be32(pb, track->enc->width*0x10000);
        put_be32(pb, track->height*0x10000);
    }
    else {
        put_be32(pb, 0);
        put_be32(pb, 0);
    }
    return 0x5c;
}

// This box seems important for the psp playback ... without it the movie seems to hang
static int mov_write_edts_tag(ByteIOContext *pb, MOVTrack *track)
{
    int64_t pts_offset = av_rescale_rnd(track->pts_offset, MOV_TIMESCALE,
                                        track->timescale, AV_ROUND_DOWN);
    int64_t edit_duration = av_rescale_rnd(track->edit_duration, MOV_TIMESCALE,
                                           track->timescale, AV_ROUND_UP);
    int entry_size, entry_count, size, version;

    version = pts_offset >= INT32_MAX || edit_duration >= INT32_MAX;
    entry_size = (version == 1) ? 20 : 12;
    entry_count = 1 + (track->pts_offset > 0);
    size = 24 + entry_count * entry_size;

    /* write the atom data */
    put_be32(pb, size);
    put_tag(pb, "edts");
    put_be32(pb, size - 8);
    put_tag(pb, "elst");
    put_byte(pb, version);
    put_be24(pb, 0); /* flags */
    put_be32(pb, entry_count);

    if (track->pts_offset > 0) { /* add an empty edit to delay presentation */
        if (version == 1) {
            put_be64(pb, pts_offset);
            put_be64(pb, -1);
        } else {
            put_be32(pb, pts_offset);
            put_be32(pb, -1);
        }
        put_be32(pb, 0x00010000);
    }

    /* duration */
    if (version == 1) {
        put_be64(pb, edit_duration);
        put_be64(pb, track->first_edit_pts);
    } else {
        put_be32(pb, edit_duration);
        put_be32(pb, track->first_edit_pts);
    }
    put_be32(pb, 0x00010000);
    return size;
}

static int mov_write_tref_tag(ByteIOContext *pb, MOVTrack *track)
{
    put_be32(pb, 20);   // size
    put_tag(pb, "tref");
    put_be32(pb, 12);   // size (subatom)
    put_le32(pb, track->tref_tag);
    put_be32(pb, track->tref_id);
    return 20;
}

// goes at the end of each track!  ... Critical for PSP playback ("Incompatible data" without it)
static int mov_write_uuid_tag_psp(ByteIOContext *pb, MOVTrack *mov)
{
    put_be32(pb, 0x34); /* size ... reports as 28 in mp4box! */
    put_tag(pb, "uuid");
    put_tag(pb, "USMT");
    put_be32(pb, 0x21d24fce);
    put_be32(pb, 0xbb88695c);
    put_be32(pb, 0xfac9c740);
    put_be32(pb, 0x1c);     // another size here!
    put_tag(pb, "MTDT");
    put_be32(pb, 0x00010012);
    put_be32(pb, 0x0a);
    put_be32(pb, 0x55c40000);
    put_be32(pb, 0x1);
    put_be32(pb, 0x0);
    return 0x34;
}

static int mov_write_udta_sdp(ByteIOContext *pb, AVCodecContext *ctx, int index)
{
    char buf[1000] = "";
    int len;

    ff_sdp_write_media(buf, sizeof(buf), ctx, NULL, NULL, 0, 0);
    av_strlcatf(buf, sizeof(buf), "a=control:streamid=%d\r\n", index);
    len = strlen(buf);

    put_be32(pb, len + 24);
    put_tag (pb, "udta");
    put_be32(pb, len + 16);
    put_tag (pb, "hnti");
    put_be32(pb, len + 8);
    put_tag (pb, "sdp ");
    put_buffer(pb, buf, len);
    return len + 24;
}

static int mov_write_trak_tag(AVFormatContext *s, ByteIOContext *pb, MOVTrack *track, AVStream *st)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "trak");
    mov_write_tkhd_tag(pb, track, st);
    if (track->mode == MODE_MOV &&
        track->enc->sample_aspect_ratio.den > 0 &&
        track->enc->sample_aspect_ratio.num > 0 &&
        track->enc->sample_aspect_ratio.den !=
        track->enc->sample_aspect_ratio.num)
        mov_write_tapt_tag(pb, track);
    mov_write_edts_tag(pb, track); // PSP Movies require edts box
    if (track->tref_tag)
        mov_write_tref_tag(pb, track);
    mov_write_mdia_tag(s, pb, track);
    if (track->mode == MODE_PSP)
        mov_write_uuid_tag_psp(pb,track);  // PSP Movies require this uuid box
    if (track->tag == MKTAG('r','t','p',' '))
        mov_write_udta_sdp(pb, track->rtp_ctx->streams[0]->codec, track->trackID);
    return updateSize(pb, pos);
}

#if 0
/* TODO: Not sorted out, but not necessary either */
static int mov_write_iods_tag(ByteIOContext *pb, MOVMuxContext *mov)
{
    put_be32(pb, 0x15); /* size */
    put_tag(pb, "iods");
    put_be32(pb, 0);    /* version & flags */
    put_be16(pb, 0x1007);
    put_byte(pb, 0);
    put_be16(pb, 0x4fff);
    put_be16(pb, 0xfffe);
    put_be16(pb, 0x01ff);
    return 0x15;
}
#endif

static int mov_write_mvhd_tag(ByteIOContext *pb, MOVMuxContext *mov)
{
    int maxTrackID = 1, i;
    int64_t min_duration = INT64_MAX;
    int version;

    for (i=0; i<mov->nb_streams; i++) {
        MOVTrack *track = &mov->tracks[i];
        int64_t duration = av_rescale_rnd(track->edit_duration +
                                          track->pts_offset, MOV_TIMESCALE,
                                          track->timescale, AV_ROUND_UP);
        if (track->entry == 0)
            continue;
        min_duration = FFMIN(min_duration, duration);
        if (maxTrackID < track->trackID)
            maxTrackID = track->trackID;
    }

    version = min_duration < UINT32_MAX ? 0 : 1;
    (version == 1) ? put_be32(pb, 120) : put_be32(pb, 108); /* size */
    put_tag(pb, "mvhd");
    put_byte(pb, version);
    put_be24(pb, 0); /* flags */
    if (version == 1) {
        put_be64(pb, mov->time);
        put_be64(pb, mov->time);
    } else {
        put_be32(pb, mov->time); /* creation time */
        put_be32(pb, mov->time); /* modification time */
    }
    put_be32(pb, MOV_TIMESCALE);
    if (version == 1)
        put_be64(pb, min_duration); /* duration of shortest track */
    else
        put_be32(pb, min_duration);
    put_be32(pb, 0x00010000); /* reserved (preferred rate) 1.0 = normal */
    put_be16(pb, 0x0100); /* reserved (preferred volume) 1.0 = normal */
    put_be16(pb, 0); /* reserved */
    put_be32(pb, 0); /* reserved */
    put_be32(pb, 0); /* reserved */

    /* Matrix structure */
    put_be32(pb, 0x00010000); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x00010000); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x0); /* reserved */
    put_be32(pb, 0x40000000); /* reserved */

    put_be32(pb, 0); /* reserved (preview time) */
    put_be32(pb, 0); /* reserved (preview duration) */
    put_be32(pb, 0); /* reserved (poster time) */
    put_be32(pb, 0); /* reserved (selection time) */
    put_be32(pb, 0); /* reserved (selection duration) */
    put_be32(pb, 0); /* reserved (current time) */
    put_be32(pb, maxTrackID+1); /* Next track id */
    return 0x6c;
}

static int mov_write_mdir_hdlr_tag(ByteIOContext *pb)
{
    put_be32(pb, 33); /* size */
    put_tag(pb, "hdlr");
    put_be32(pb, 0);
    put_be32(pb, 0);
    put_tag(pb, "mdir");
    put_tag(pb, "appl");
    put_be32(pb, 0);
    put_be32(pb, 0);
    put_byte(pb, 0);
    return 33;
}

static int mov_write_data_tag(ByteIOContext *pb, const char *data,
                              unsigned len, unsigned type)
{
    put_be32(pb, 8+8+len);
    put_tag(pb, "data");
    put_be32(pb, type);
    put_be32(pb, 0);
    put_buffer(pb, data, len);
    return 8+8+len;
}

static int mov_write_3gp_metadata(AVFormatContext *s, ByteIOContext *pb,
                                  const char *name, const char *tag)
{
    AVMetadataTag *t = av_metadata_get(s->metadata, tag, NULL, 0);
    int64_t pos = url_ftell(pb);
    unsigned len;
    if (!t || !t->value)
        return 0;

    if (!(len = strlen(t->value)))
        return 0;

    put_be32(pb, 0);   /* size */
    put_tag (pb, name); /* type */
    put_be32(pb, 0);   /* version + flags */
    if (!strcmp(tag, "yrrc"))
        put_be16(pb, atoi(t->value));
    else {
        put_be16(pb, ff_mov_iso639_to_lang(av_metadata_get_attribute(t, "language"), 1));
        put_buffer(pb, t->value, len+1); /* UTF8 string value */
        if (!strcmp(tag, "albm") &&
            (t = av_metadata_get(s->metadata, "track", NULL, 0)))
            put_byte(pb, atoi(t->value));
    }
    return updateSize(pb, pos);
}

static int mov_write_itunes_string(ByteIOContext *pb, const char *name,
                                   const char *value)
{
    int64_t pos = url_ftell(pb);
    unsigned len = strlen(value);
    put_be32(pb, 0); /* size */
    put_tag(pb, name);
    mov_write_data_tag(pb, value, len, 1);
    return updateSize(pb, pos);
}

static int mov_write_metadata(AVFormatContext *s, ByteIOContext *pb,
                              const char *name, const char *tag)
{
    MOVMuxContext *mov = s->priv_data;
    AVMetadataTag *t = av_metadata_get(s->metadata, tag, NULL, 0);
    if (!t || !t->value || !strlen(t->value))
        return 0;

    if (mov->mode & MODE_MOV)
        return mov_write_mac_string(pb, name, t->value,
                                    av_metadata_get_attribute(t, "language"), 1);
    else
        return mov_write_itunes_string(pb, name, t->value);
}

static int mov_write_covr_tag(AVFormatContext *s, ByteIOContext *pb)
{
    AVMetadataTag *t = av_metadata_get(s->metadata, "cover", NULL, 0);
    int64_t pos = url_ftell(pb);
    const char *mime;
    unsigned type;

    if (!t || !t->value || !t->len)
        return 0;

    mime = av_metadata_get_attribute(t, "mime");
    if (!mime) {
        av_log(s, AV_LOG_ERROR, "error, no mime type set for cover\n");
        return 0;
    }
    if (!strcmp(mime, "image/jpeg"))
        type = 13;
    else if (!strcmp(mime, "image/png"))
        type = 14;
    else if (!strcmp(mime, "image/bmp"))
        type = 27;
    else
        type = 0;

    put_be32(pb, 0); /* size */
    put_tag(pb, "covr");
    mov_write_data_tag(pb, t->value, t->len, type);
    return updateSize(pb, pos);
}

/* iTunes track number */
static int mov_write_trkn_tag(AVFormatContext *s, ByteIOContext *pb)
{
    AVMetadataTag *t = av_metadata_get(s->metadata, "track", NULL, 0);
    int64_t pos = url_ftell(pb);
    uint8_t data[8] = {0};
    char *slash;

    if (!t || !t->value || !t->value[0])
        return 0;

    put_be32(pb, 0); /* size */
    put_tag(pb, "trkn");
    AV_WB16(data+2, atoi(t->value));
    if ((slash = strrchr(t->value, '/')))
        AV_WB16(data+4, atoi(slash+1));
    mov_write_data_tag(pb, data, 8, 0);
    return updateSize(pb, pos);
}

/* iTunes meta data list */
static int mov_write_ilst_tag(AVFormatContext *s, ByteIOContext *pb)
{
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "ilst");
    mov_write_metadata(s, pb, "\251nam", "title");
    mov_write_metadata(s, pb, "\251ART", "artist");
    mov_write_metadata(s, pb, "\251wrt", "composer");
    mov_write_metadata(s, pb, "\251alb", "album");
    mov_write_metadata(s, pb, "\251day", "date");
    mov_write_metadata(s, pb, "\251too", "encoder");
    mov_write_metadata(s, pb, "\251cmt", "comment");
    mov_write_metadata(s, pb, "\251gen", "genre");
    mov_write_metadata(s, pb, "\251grp", "grouping");
    mov_write_metadata(s, pb, "\251lyr", "lyrics");
    mov_write_metadata(s, pb, "aART",    "album_artist");
    mov_write_metadata(s, pb, "cprt",    "copyright");
    mov_write_metadata(s, pb, "desc",    "description");
    mov_write_metadata(s, pb, "ldes",    "synopsis");
    mov_write_metadata(s, pb, "tvsh",    "show");
    mov_write_metadata(s, pb, "tven",    "episode_id");
    mov_write_metadata(s, pb, "tvnn",    "network");
    mov_write_covr_tag(s, pb);
    mov_write_trkn_tag(s, pb);
    return updateSize(pb, pos);
}

/* iTunes meta data tag */
static int mov_write_meta_tag(AVFormatContext *s, ByteIOContext *pb)
{
    int size = 0;
    int64_t pos = url_ftell(pb);
    put_be32(pb, 0); /* size */
    put_tag(pb, "meta");
    put_be32(pb, 0);
    mov_write_mdir_hdlr_tag(pb);
    mov_write_ilst_tag(s, pb);
    size = updateSize(pb, pos);
    return size;
}

static int mov_write_chpl_tag(ByteIOContext *pb, AVFormatContext *s)
{
    int64_t pos = url_ftell(pb);
    int i, nb_chapters = FFMIN(s->nb_chapters, 255);

    put_be32(pb, 0);            // size
    put_tag (pb, "chpl");
    put_be32(pb, 0x01000000);   // version + flags
    put_be32(pb, 0);            // unknown
    put_byte(pb, nb_chapters);

    for (i = 0; i < nb_chapters; i++) {
        AVChapter *c = s->chapters[i];
        AVMetadataTag *t;
        put_be64(pb, av_rescale_q(c->start, c->time_base, (AVRational){1,10000000}));

        if ((t = av_metadata_get(c->metadata, "title", NULL, 0))) {
            int len = FFMIN(strlen(t->value), 255);
            put_byte(pb, len);
            put_buffer(pb, t->value, len);
        } else
            put_byte(pb, 0);
    }
    return updateSize(pb, pos);
}

static int mov_write_udta_tag(ByteIOContext *pb, AVFormatContext *s)
{
    MOVMuxContext *mov = s->priv_data;
    ByteIOContext *pb_buf;
    int i, ret, size;
    uint8_t *buf;

    for (i = 0; i < s->nb_streams; i++)
        if (mov->tracks[i].enc->flags & CODEC_FLAG_BITEXACT)
            return 0;

    ret = url_open_dyn_buf(&pb_buf);
    if(ret < 0)
        return ret;

        if (mov->mode & MODE_3GP) {
            mov_write_3gp_metadata(s, pb_buf, "titl", "title");
            mov_write_3gp_metadata(s, pb_buf, "auth", "author");
            mov_write_3gp_metadata(s, pb_buf, "gnre", "genre");
            mov_write_3gp_metadata(s, pb_buf, "dscp", "comment");
            mov_write_3gp_metadata(s, pb_buf, "albm", "album");
            mov_write_3gp_metadata(s, pb_buf, "cprt", "copyright");
            mov_write_3gp_metadata(s, pb_buf, "yrrc", "year");
        } else if (mov->mode == MODE_MOV) { // the title field breaks gtkpod with mp4 and my suspicion is that stuff is not valid in mp4
            mov_write_metadata(s, pb_buf, "\251ART", "artist");
            mov_write_metadata(s, pb_buf, "\251nam", "title");
            mov_write_metadata(s, pb_buf, "\251aut", "author");
            mov_write_metadata(s, pb_buf, "\251alb", "album");
            mov_write_metadata(s, pb_buf, "\251day", "date");
            mov_write_metadata(s, pb_buf, "\251swr", "encoder");
            mov_write_metadata(s, pb_buf, "\251des", "comment");
            mov_write_metadata(s, pb_buf, "\251gen", "genre");
            mov_write_metadata(s, pb_buf, "\251cpy", "copyright");
        } else {
            /* iTunes meta data */
            mov_write_meta_tag(s, pb_buf);
        }

        if (s->nb_chapters)
            mov_write_chpl_tag(pb_buf, s);

    if ((size = url_close_dyn_buf(pb_buf, &buf)) > 0) {
        put_be32(pb, size+8);
        put_tag(pb, "udta");
        put_buffer(pb, buf, size);
    }
    av_free(buf);

    return 0;
}

static int utf8len(const uint8_t *b)
{
    int len=0;
    int val;
    while(*b){
        GET_UTF8(val, *b++, return -1;)
        len++;
    }
    return len;
}

static int ascii_to_wc(ByteIOContext *pb, const uint8_t *b)
{
    int val;
    while(*b){
        GET_UTF8(val, *b++, return -1;)
        put_be16(pb, val);
    }
    put_be16(pb, 0x00);
    return 0;
}

static void mov_write_psp_udta_tag(ByteIOContext *pb,
                                  const char *str, const char *lang, int type)
{
    int len = utf8len(str)+1;
    if(len<=0)
        return;
    put_be16(pb, len*2+10);            /* size */
    put_be32(pb, type);                /* type */
    put_be16(pb, ff_mov_iso639_to_lang(lang, 1)); /* language */
    put_be16(pb, 0x01);                /* ? */
    ascii_to_wc(pb, str);
}

static int mov_write_uuidusmt_tag(ByteIOContext *pb, AVFormatContext *s)
{
    AVMetadataTag *title = av_metadata_get(s->metadata, "title", NULL, 0);
    int64_t pos, pos2;

    if (title) {
        pos = url_ftell(pb);
        put_be32(pb, 0); /* size placeholder*/
        put_tag(pb, "uuid");
        put_tag(pb, "USMT");
        put_be32(pb, 0x21d24fce); /* 96 bit UUID */
        put_be32(pb, 0xbb88695c);
        put_be32(pb, 0xfac9c740);

        pos2 = url_ftell(pb);
        put_be32(pb, 0); /* size placeholder*/
        put_tag(pb, "MTDT");
        put_be16(pb, 4);

        // ?
        put_be16(pb, 0x0C);                 /* size */
        put_be32(pb, 0x0B);                 /* type */
        put_be16(pb, ff_mov_iso639_to_lang("und", 1)); /* language */
        put_be16(pb, 0x0);                  /* ? */
        put_be16(pb, 0x021C);               /* data */

        mov_write_psp_udta_tag(pb, LIBAVCODEC_IDENT,      "eng", 0x04);
        mov_write_psp_udta_tag(pb, title->value,          "eng", 0x01);
//        snprintf(dt,32,"%04d/%02d/%02d %02d:%02d:%02d",t_st->tm_year+1900,t_st->tm_mon+1,t_st->tm_mday,t_st->tm_hour,t_st->tm_min,t_st->tm_sec);
        mov_write_psp_udta_tag(pb, "2006/04/01 11:11:11", "und", 0x03);

        updateSize(pb, pos2);
        return updateSize(pb, pos);
    }

    return 0;
}

static int mov_write_moov_tag(ByteIOContext *pb, MOVMuxContext *mov,
                              AVFormatContext *s)
{
    int i, j;
    int64_t pos = url_ftell(pb);

    put_be32(pb, 0); /* size placeholder*/
    put_tag(pb, "moov");

    for (i=0; i<mov->nb_streams; i++) {
        MOVTrack *track = &mov->tracks[i];
        int64_t first_pts, first_dec_pts;
        MOVIentry *kf = NULL;

        if (track->entry <= 0)
            continue;

        track->time = mov->time;
        track->trackID = i+1;

        track->edit_duration = track->total_duration;
        first_pts = track->cluster[0].dts + track->cluster[0].cts;
        for (j = 1; j < track->entry; j++) {
            int64_t pts = track->cluster[j].dts + track->cluster[j].cts;
            if (pts >= track->cluster[0].dts + track->cluster[0].cts)
                break;
            first_pts = FFMIN(pts, first_pts);
        }
        if (first_pts > 0) {
            track->pts_offset = first_pts;
            track->pts_duration -= first_pts;
        }

        // search for first keyframe
        for (j = 0; j < track->entry; j++) {
            if (track->cluster[j].flags & (MOV_SYNC_SAMPLE|MOV_PARTIAL_SYNC_SAMPLE)) {
                kf = &track->cluster[j];
                break;
            }
        }
        if (!kf) {
            av_log(s, AV_LOG_WARNING, "track %d has no keyframes\n", i);
            continue;
        }

        // check if first keyframe is reordered
        first_dec_pts = kf->dts + kf->cts;
        for (j++; j < track->entry; j++) {
            int64_t pts = track->cluster[j].dts + track->cluster[j].cts;
            if (pts >= kf->dts + kf->cts)
                break;
            first_dec_pts = FFMIN(pts, first_dec_pts);
        }
        track->delay = first_dec_pts - kf->dts;
        if (kf->flags & MOV_PARTIAL_SYNC_SAMPLE) {
            // unmark partial sync entry for the first kf,
            // offset using edit list
            kf->flags |= MOV_SYNC_SAMPLE;
            // do not display first b frames if keyframe is partial
            track->pts_offset += kf->dts + kf->cts - first_dec_pts;
            first_dec_pts = kf->dts + kf->cts;
        }
        track->edit_duration -= first_dec_pts - first_pts;
        track->first_edit_pts = first_dec_pts - first_pts;

        if (first_pts < 0) {
            track->first_edit_pts = -first_pts;
            track->edit_duration -= -first_pts;
        }
        if (mov->mode != MODE_MOV)
            track->first_edit_pts += track->delay;
    }

    if (mov->chapter_track)
        for (i=0; i<s->nb_streams; i++) {
            mov->tracks[i].tref_tag = MKTAG('c','h','a','p');
            mov->tracks[i].tref_id = mov->tracks[mov->chapter_track].trackID;
        }
    for (i = 0; i < mov->nb_streams; i++) {
        if (mov->tracks[i].tag == MKTAG('r','t','p',' ')) {
            mov->tracks[i].tref_tag = MKTAG('h','i','n','t');
            mov->tracks[i].tref_id =
                mov->tracks[mov->tracks[i].src_track].trackID;
        }
    }
    if (mov->timecode_track)
        for (i=0; i<s->nb_streams; i++) {
            if (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                mov->tracks[i].tref_tag = MKTAG('t','m','c','d');
                mov->tracks[i].tref_id = mov->tracks[mov->timecode_track].trackID;
                mov->tracks[mov->timecode_track].total_duration = mov->tracks[i].total_duration;
                mov->tracks[mov->timecode_track].edit_duration = mov->tracks[i].total_duration;
                break;
            }
        }

    mov_write_mvhd_tag(pb, mov);
    //mov_write_iods_tag(pb, mov);
    for (i=0; i<mov->nb_streams; i++) {
        if(mov->tracks[i].entry > 0) {
            mov_write_trak_tag(s, pb, &(mov->tracks[i]), i < s->nb_streams ? s->streams[i] : NULL);
        }
    }

    if (mov->mode == MODE_PSP)
        mov_write_uuidusmt_tag(pb, s);
    else
        mov_write_udta_tag(pb, s);

    return updateSize(pb, pos);
}

static int mov_write_free_tag(ByteIOContext *pb, MOVMuxContext *mov, unsigned size)
{
    static const uint8_t buffer[1024];
    if (size < 8)
        return -1;
    put_be32(pb, size);
    put_tag(pb, mov->mode == MODE_MOV && size == 8 ? "wide" : "free");
    size -= 8;
    for (; size > 1023; size -= 1024)
        put_buffer(pb, buffer, 1024);
    put_buffer(pb, buffer, size);
    return size;
}

static int mov_write_mdat_tag(ByteIOContext *pb, MOVMuxContext *mov)
{
    mov->mdat_pos = url_ftell(pb);
    put_be32(pb, 0); /* size placeholder*/
    put_tag(pb, "mdat");
    return 8;
}

/* TODO: This needs to be more general */
static int mov_write_ftyp_tag(ByteIOContext *pb, AVFormatContext *s)
{
    MOVMuxContext *mov = s->priv_data;
    int64_t pos = url_ftell(pb);
    int has_h264 = 0, has_video = 0;
    int minor = 0;
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            has_video = 1;
        if (st->codec->codec_id == CODEC_ID_H264)
            has_h264 = 1;
    }

    put_be32(pb, 0); /* size */
    put_tag(pb, "ftyp");

    if (mov->mode == MODE_3GP) {
        put_tag(pb, has_h264 ? "3gp6"  : "3gp4");
        minor =     has_h264 ?   0x100 :   0x200;
    } else if (mov->mode & MODE_3G2) {
        put_tag(pb, has_h264 ? "3g2b"  : "3g2a");
        minor =     has_h264 ? 0x20000 : 0x10000;
    }else if (mov->mode == MODE_PSP)
        put_tag(pb, "MSNV");
    else if (mov->mode == MODE_MP4)
        put_tag(pb, "mp42");
    else if (mov->mode == MODE_F4V)
        put_tag(pb, "f4v ");
    else if (mov->mode == MODE_IPOD)
        put_tag(pb, has_video ? "M4V ":"M4A ");
    else
        put_tag(pb, "qt  ");

    put_be32(pb, minor);

    if(mov->mode == MODE_MOV)
        put_tag(pb, "qt  ");
    else{
        put_tag(pb, "isom");
        put_tag(pb, "iso2");
        if(has_h264)
            put_tag(pb, "avc1");
    }

    if (mov->mode == MODE_3GP)
        put_tag(pb, has_h264 ? "3gp6":"3gp4");
    else if (mov->mode & MODE_3G2)
        put_tag(pb, has_h264 ? "3g2b":"3g2a");
    else if (mov->mode == MODE_PSP)
        put_tag(pb, "MSNV");
    else if (mov->mode == MODE_MP4)
        put_tag(pb, "mp41");
    return updateSize(pb, pos);
}

static void mov_write_uuidprof_tag(ByteIOContext *pb, AVFormatContext *s)
{
    AVCodecContext *VideoCodec = s->streams[0]->codec;
    AVCodecContext *AudioCodec = s->streams[1]->codec;
    int AudioRate = AudioCodec->sample_rate;
    int FrameRate = ((VideoCodec->time_base.den) * (0x10000))/ (VideoCodec->time_base.num);
    int audio_kbitrate= AudioCodec->bit_rate / 1000;
    int video_kbitrate= FFMIN(VideoCodec->bit_rate / 1000, 800 - audio_kbitrate);

    put_be32(pb, 0x94); /* size */
    put_tag(pb, "uuid");
    put_tag(pb, "PROF");

    put_be32(pb, 0x21d24fce); /* 96 bit UUID */
    put_be32(pb, 0xbb88695c);
    put_be32(pb, 0xfac9c740);

    put_be32(pb, 0x0);  /* ? */
    put_be32(pb, 0x3);  /* 3 sections ? */

    put_be32(pb, 0x14); /* size */
    put_tag(pb, "FPRF");
    put_be32(pb, 0x0);  /* ? */
    put_be32(pb, 0x0);  /* ? */
    put_be32(pb, 0x0);  /* ? */

    put_be32(pb, 0x2c);  /* size */
    put_tag(pb, "APRF");   /* audio */
    put_be32(pb, 0x0);
    put_be32(pb, 0x2);   /* TrackID */
    put_tag(pb, "mp4a");
    put_be32(pb, 0x20f);
    put_be32(pb, 0x0);
    put_be32(pb, audio_kbitrate);
    put_be32(pb, audio_kbitrate);
    put_be32(pb, AudioRate);
    put_be32(pb, AudioCodec->channels);

    put_be32(pb, 0x34);  /* size */
    put_tag(pb, "VPRF");   /* video */
    put_be32(pb, 0x0);
    put_be32(pb, 0x1);    /* TrackID */
    if (VideoCodec->codec_id == CODEC_ID_H264) {
        put_tag(pb, "avc1");
        put_be16(pb, 0x014D);
        put_be16(pb, 0x0015);
    } else {
        put_tag(pb, "mp4v");
        put_be16(pb, 0x0000);
        put_be16(pb, 0x0103);
    }
    put_be32(pb, 0x0);
    put_be32(pb, video_kbitrate);
    put_be32(pb, video_kbitrate);
    put_be32(pb, FrameRate);
    put_be32(pb, FrameRate);
    put_be16(pb, VideoCodec->width);
    put_be16(pb, VideoCodec->height);
    put_be32(pb, 0x010001); /* ? */
}

static int mov_parse_mpeg2_frame(AVPacket *pkt, uint32_t *flags)
{
    uint32_t c = -1;
    int i, closed_gop = 0;

    for (i = 0; i < pkt->size - 4; i++) {
        c = (c<<8) + pkt->data[i];
        if (c == 0x1b8) { // gop
            closed_gop = pkt->data[i+4]>>6 & 0x01;
        } else if (c == 0x100) { // pic
            int temp_ref = (pkt->data[i+1]<<2) | (pkt->data[i+2]>>6);
            if (!temp_ref || closed_gop) // I picture is not reordered
                *flags = MOV_SYNC_SAMPLE;
            else
                *flags = MOV_PARTIAL_SYNC_SAMPLE;
            break;
        }
    }
    return 0;
}

int ff_mov_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    MOVMuxContext *mov = s->priv_data;
    ByteIOContext *pb = s->pb;
    MOVTrack *trk = &mov->tracks[pkt->stream_index];
    AVCodecContext *enc = trk->enc;
    unsigned int samplesInChunk = 0;
    int size= pkt->size;

    if (url_is_streamed(s->pb)) return 0; /* Can't handle that */
    if (!size) return 0; /* Discard 0 sized packets */

    if (enc->codec_id == CODEC_ID_ADPCM_MS ||
        enc->codec_id == CODEC_ID_ADPCM_IMA_WAV) {
        samplesInChunk = enc->frame_size;
    } else if (trk->sampleSize)
        samplesInChunk = size/trk->sampleSize;
    else
        samplesInChunk = 1;

    /* copy extradata if it exists */
    if (trk->vosLen == 0 && enc->extradata_size > 0) {
        trk->vosLen = enc->extradata_size;
        trk->vosData = av_malloc(trk->vosLen);
        memcpy(trk->vosData, enc->extradata, trk->vosLen);
    }

    if (enc->codec_id == CODEC_ID_H264 &&
        pkt->size > 4 && AV_RB32(pkt->data) == 0x00000001) {
        /* from x264 or from bytestream h264 */
        /* nal reformating needed */
        size = ff_avc_parse_nal_units(enc, pb, pkt->data, pkt->size);
    } else if (enc->codec_id == CODEC_ID_AAC && pkt->size > 2 &&
               (AV_RB16(pkt->data) & 0xfff0) == 0xfff0) {
        av_log(s, AV_LOG_ERROR, "malformated aac bitstream, use -absf aac_adtstoasc\n");
        return -1;
    } else {
        put_buffer(pb, pkt->data, size);
    }

    if ((enc->codec_id == CODEC_ID_DNXHD ||
         enc->codec_id == CODEC_ID_AMR_NB ||
         enc->codec_id == CODEC_ID_AC3) && !trk->vosLen) {
        /* copy frame to create needed atoms */
        trk->vosLen = size;
        trk->vosData = av_malloc(size);
        if (!trk->vosData)
            return AVERROR(ENOMEM);
        memcpy(trk->vosData, pkt->data, size);
    }

    if (!(trk->entry % MOV_INDEX_CLUSTER_SIZE)) {
        trk->cluster = av_realloc(trk->cluster, (trk->entry + MOV_INDEX_CLUSTER_SIZE) * sizeof(*trk->cluster));
        if (!trk->cluster)
            return -1;
    }

    trk->cluster[trk->entry].pos = url_ftell(pb) - size;
    trk->cluster[trk->entry].samplesInChunk = samplesInChunk;
    trk->cluster[trk->entry].size = size;
    trk->cluster[trk->entry].entries = samplesInChunk;
    trk->cluster[trk->entry].dts = pkt->dts;
    trk->cluster[trk->entry].cts = pkt->pts - pkt->dts;
    trk->total_duration = pkt->dts - trk->cluster[0].dts + pkt->duration;

    if (pkt->pts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_WARNING, "pts has no value\n");
        pkt->pts = pkt->dts;
    }
    if (pkt->dts != pkt->pts)
        trk->flags |= MOV_TRACK_CTTS;
    trk->cluster[trk->entry].cts = pkt->pts - pkt->dts;

    if ((trk->flags & MOV_TRACK_CTTS) && trk->mode == MODE_MOV) {
        trk->min_cts = FFMIN(trk->cluster[trk->entry].cts, trk->min_cts);
        trk->max_cts = FFMAX(trk->cluster[trk->entry].cts, trk->max_cts);
        trk->pts_duration = FFMAX(pkt->pts+pkt->duration, trk->pts_duration);
    }

    trk->cluster[trk->entry].flags = 0;
    if (pkt->flags & AV_PKT_FLAG_KEY) {
        if (enc->codec_id == CODEC_ID_MPEG2VIDEO)
            mov_parse_mpeg2_frame(pkt, &trk->cluster[trk->entry].flags);
        else
            trk->cluster[trk->entry].flags = MOV_SYNC_SAMPLE;
        if (trk->cluster[trk->entry].flags & MOV_PARTIAL_SYNC_SAMPLE)
            trk->flags |= MOV_TRACK_STPS;
        if (trk->cluster[trk->entry].flags & MOV_SYNC_SAMPLE)
            trk->hasKeyframes++;
    }

    trk->entry++;
    trk->sampleCount += samplesInChunk;
    mov->mdat_size += size;

    put_flush_packet(pb);

    if (trk->hint_track >= 0 && trk->hint_track < mov->nb_streams)
        ff_mov_add_hinted_packet(s, pkt, trk->hint_track, trk->entry);
    return 0;
}

static int mov_create_timecode_track(AVFormatContext *s, int tracknum)
{
    MOVMuxContext *mov = s->priv_data;
    MOVTrack *track = &mov->tracks[tracknum];
    AVPacket pkt;
    AVStream *vst = NULL;
    int i, framenum, drop;

    for (i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            vst = s->streams[i];
            break;
        }
    }
    if (!vst) {
        av_log(s, AV_LOG_ERROR, "no video track\n");
        return -1;
    }


    framenum = ff_timecode_to_framenum(mov->timecode, vst->codec->time_base, &drop);
    if (framenum < 0) {
        if (framenum == -1)
            av_log(s, AV_LOG_ERROR, "error parsing timecode, syntax: 00:00:00[;:]00\n");
        else if (framenum == -2)
            av_log(s, AV_LOG_ERROR, "error, unsupported fps for timecode\n");
        else if (framenum == -3)
            av_log(s, AV_LOG_ERROR, "error, drop frame is only allowed with "
                   "30000/1001 or 60000/1001 fps\n");
        return -1;
    }

    track->mode = MODE_MOV;
    track->timescale = vst->codec->time_base.den;
    track->enc = avcodec_alloc_context();
    track->enc->codec_tag = track->tag = AV_RL32("tmcd");
    track->enc->codec_type = AVMEDIA_TYPE_DATA;
    track->enc->time_base = vst->codec->time_base;
    if (drop)
        track->flags |= MOV_TRACK_DROP_TC;

    av_new_packet(&pkt, 4);
    pkt.dts = 0;
    pkt.pts = 0;
    pkt.size = 4;
    AV_WB32(pkt.data, framenum);
    pkt.stream_index = tracknum;
    pkt.duration = 0;
    pkt.flags = AV_PKT_FLAG_KEY;

    ff_mov_write_packet(s, &pkt);

    av_free_packet(&pkt);

    return 0;
}

// QuickTime chapters involve an additional text track with the chapter names
// as samples, and a tref pointing from the other tracks to the chapter one.
static void mov_create_chapter_track(AVFormatContext *s, int tracknum)
{
    MOVMuxContext *mov = s->priv_data;
    MOVTrack *track = &mov->tracks[tracknum];
    AVPacket pkt = { .stream_index = tracknum, .flags = AV_PKT_FLAG_KEY };
    int i, len;

    track->mode = mov->mode;
    track->tag = MKTAG('t','e','x','t');
    track->timescale = MOV_TIMESCALE;
    track->enc = avcodec_alloc_context();
    track->enc->codec_type = AVMEDIA_TYPE_SUBTITLE;

    for (i = 0; i < s->nb_chapters; i++) {
        AVChapter *c = s->chapters[i];
        AVMetadataTag *t;

        int64_t end = av_rescale_q(c->end, c->time_base, (AVRational){1,MOV_TIMESCALE});
        pkt.pts = pkt.dts = av_rescale_q(c->start, c->time_base, (AVRational){1,MOV_TIMESCALE});
        pkt.duration = end - pkt.dts;

        if ((t = av_metadata_get(c->metadata, "title", NULL, 0))) {
            len = strlen(t->value);
            pkt.size = len+2;
            pkt.data = av_malloc(pkt.size);
            AV_WB16(pkt.data, len);
            memcpy(pkt.data+2, t->value, len);
            ff_mov_write_packet(s, &pkt);
            av_freep(&pkt.data);
        }
    }
}

static int mov_write_header(AVFormatContext *s)
{
    ByteIOContext *pb = s->pb;
    MOVMuxContext *mov = s->priv_data;
    int i, hint_track = 0;

    if (url_is_streamed(s->pb)) {
        av_log(s, AV_LOG_ERROR, "muxer does not support non seekable output\n");
        return -1;
    }

    /* Default mode == MP4 */
    mov->mode = MODE_MP4;

    if (s->oformat != NULL) {
        if (!strcmp("3gp", s->oformat->name)) mov->mode = MODE_3GP;
        else if (!strcmp("3g2", s->oformat->name)) mov->mode = MODE_3GP|MODE_3G2;
        else if (!strcmp("mov", s->oformat->name)) mov->mode = MODE_MOV;
        else if (!strcmp("psp", s->oformat->name)) mov->mode = MODE_PSP;
        else if (!strcmp("ipod",s->oformat->name)) mov->mode = MODE_IPOD;
        else if (!strcmp("f4v", s->oformat->name)) mov->mode = MODE_F4V;

        mov_write_ftyp_tag(pb,s);
        if (mov->mode == MODE_PSP) {
            if (s->nb_streams != 2) {
                av_log(s, AV_LOG_ERROR, "PSP mode need one video and one audio stream\n");
                return -1;
            }
            mov_write_uuidprof_tag(pb,s);
        }
    }

    mov->nb_streams = s->nb_streams;

    if (mov->timecode)
        mov->timecode_track = mov->nb_streams++;
    if (mov->mode & (MODE_MOV|MODE_IPOD) && s->nb_chapters)
        mov->chapter_track = mov->nb_streams++;

    if (s->flags & AVFMT_FLAG_RTP_HINT) {
        /* Add hint tracks for each audio and video stream */
        hint_track = mov->nb_streams;
        for (i = 0; i < s->nb_streams; i++) {
            AVStream *st = s->streams[i];
            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO ||
                st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                mov->nb_streams++;
            }
        }
    }

    mov->tracks = av_mallocz(mov->nb_streams*sizeof(*mov->tracks));
    if (!mov->tracks)
        return AVERROR(ENOMEM);

    for(i=0; i<s->nb_streams; i++){
        AVStream *st= s->streams[i];
        MOVTrack *track= &mov->tracks[i];
        AVMetadataTag *lang = av_metadata_get(st->metadata, "language", NULL, 0);
        const char *language;

        track->enc = st->codec;
        if (lang)
            language = mov->mode == MODE_MOV ? lang->value :
                av_convert_lang_to(lang->value, AV_LANG_ISO639_2_TERM);
        else
            language = NULL;
        track->language = ff_mov_iso639_to_lang(language, mov->mode != MODE_MOV);
        track->mode = mov->mode;
        track->tag = mov_find_codec_tag(s, track);
        if (!track->tag) {
            av_log(s, AV_LOG_ERROR, "track %d: could not find tag, "
                   "codec not currently supported in container\n", i);
            goto error;
        }
        /* If hinting of this track is enabled by a later hint track,
         * this is updated. */
        track->hint_track = -1;
        if(st->codec->codec_type == AVMEDIA_TYPE_VIDEO){
            if (track->tag == MKTAG('m','x','3','p') || track->tag == MKTAG('m','x','3','n') ||
                track->tag == MKTAG('m','x','4','p') || track->tag == MKTAG('m','x','4','n') ||
                track->tag == MKTAG('m','x','5','p') || track->tag == MKTAG('m','x','5','n')) {
                if (st->codec->width != 720 || (st->codec->height != 608 && st->codec->height != 512)) {
                    av_log(s, AV_LOG_ERROR, "D-10/IMX must use 720x608 or 720x512 video resolution\n");
                    goto error;
                }
                track->height = track->tag>>24 == 'n' ? 486 : 576;
            } else {
                track->height = st->codec->height;
            }

            track->dar.num = track->enc->width *track->enc->sample_aspect_ratio.num;
            track->dar.den = track->enc->height*track->enc->sample_aspect_ratio.den;

            track->timescale = st->codec->time_base.den;
            if (track->mode == MODE_MOV && track->timescale > 100000)
                av_log(s, AV_LOG_WARNING,
                       "WARNING codec timebase is very high. If duration is too long,\n"
                       "file may not be playable by quicktime. Specify a shorter timebase\n"
                       "or choose different container.\n");
        }else if(st->codec->codec_type == AVMEDIA_TYPE_AUDIO){
            track->timescale = st->codec->sample_rate;
            if(!st->codec->frame_size && !mov_get_lpcm_flags(st->codec->codec_id)) {
                av_log(s, AV_LOG_ERROR, "track %d: codec frame size is not set\n", i);
                goto error;
            }else if(st->codec->codec_id == CODEC_ID_ADPCM_MS ||
                     st->codec->codec_id == CODEC_ID_ADPCM_IMA_WAV){
                if (!st->codec->block_align) {
                    av_log(s, AV_LOG_ERROR, "track %d: codec block align is not set for adpcm\n", i);
                    goto error;
                }
                track->sampleSize = st->codec->block_align;
            }else if(st->codec->frame_size > 1){ /* assume compressed audio */
                track->audio_vbr = 1;
            }else{
                st->codec->frame_size = 1;
                track->sampleSize = (av_get_bits_per_sample(st->codec->codec_id) >> 3) * st->codec->channels;
            }
            if (track->mode != MODE_MOV) {
                if (track->timescale > UINT16_MAX) {
                    av_log(s, AV_LOG_ERROR, "track %d: output format does not support "
                           "sample rate %dhz\n", i, track->timescale);
                    goto error;
                }
                if (track->enc->codec_id == CODEC_ID_MP3 && track->timescale < 16000) {
                    av_log(s, AV_LOG_ERROR, "track %d: muxing mp3 at %dhz is not supported\n",
                           i, track->enc->sample_rate);
                    goto error;
                }
            }
        }else if(st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE){
            track->timescale = st->codec->time_base.den;
            track->height = st->codec->height;
        }

        av_set_pts_info(st, 64, 1, track->timescale);
    }

    if (mov->faststart) {
        if (!strcmp(mov->faststart, "auto"))
            mov->overwrite = 1;
        else if (!strcmp(mov->faststart, "no"))
            mov->overwrite = -1;
        else
            mov->overwrite = atoi(mov->faststart);
        if (mov->overwrite > 1) {
            av_log(s, AV_LOG_INFO, "writing free atom of %d bytes\n", mov->overwrite);
            mov->free_size = mov->overwrite;
        }
    }

    mov->free_pos = url_ftell(pb);
    mov->free_size += 8;
    mov_write_free_tag(pb, mov, mov->free_size);
    mov_write_mdat_tag(pb, mov);
    mov->time = s->timestamp + 0x7C25B080; //1970 based -> 1904 based

    if (mov->chapter_track)
        mov_create_chapter_track(s, mov->chapter_track);

    if (s->flags & AVFMT_FLAG_RTP_HINT) {
        /* Initialize the hint tracks for each audio and video stream */
        for (i = 0; i < s->nb_streams; i++) {
            AVStream *st = s->streams[i];
            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO ||
                st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                ff_mov_init_hinting(s, hint_track, i);
                hint_track++;
            }
        }
    }

    if (mov->timecode_track) {
        if (mov_create_timecode_track(s, mov->timecode_track) < 0)
            return -1;
    }

    put_flush_packet(pb);

    return 0;
 error:
    av_freep(&mov->tracks);
    return -1;
}

static int mov_compute_moov_size(AVFormatContext *s)
{
    MOVMuxContext *mov = s->priv_data;
    ByteIOContext *pb;
    uint8_t *buf;
    int i, size;

    url_open_dyn_buf(&pb);
    mov_write_moov_tag(pb, mov, s);
    put_flush_packet(pb);
    size = url_close_dyn_buf(pb, &buf);
    av_free(buf);

    for (i = 0; i < mov->nb_streams; i++) {
        MOVTrack *track = &mov->tracks[i];
        if (track->entry > 0) {
            if (track->cluster[track->entry-1].pos < UINT32_MAX &&
                track->cluster[track->entry-1].pos +
                size - mov->free_size > UINT32_MAX) {
                size += track->entry*4;
            }
        }
    }

    return size;
}

static int mov_overwrite_file(AVFormatContext *s)
{
    MOVMuxContext *mov = s->priv_data;
    ByteIOContext *rpb, *pb = s->pb;
    int64_t size, data_size = url_fsize(pb) - mov->mdat_pos;
    int64_t start_time, prev_time;
    int moov_size, buf_size, rsize, wsize = 0;
    uint8_t *rbuf, *wbuf;

    if (url_fopen(&rpb, s->filename, URL_RDONLY) < 0) {
        av_log(s, AV_LOG_ERROR, "error reopening file '%s' for read\n", s->filename);
        return AVERROR(EIO);
    }

    moov_size = mov_compute_moov_size(s);
    buf_size = 1024*1024 + moov_size;

    rbuf = av_malloc(buf_size);
    if (!rbuf)
        return AVERROR(ENOMEM);

    wbuf = av_malloc(buf_size);
    if (!wbuf)
        return AVERROR(ENOMEM);

    url_fseek(rpb, mov->mdat_pos, SEEK_SET);
    url_fseek(pb,  mov->free_pos, SEEK_SET);

    av_log(s, AV_LOG_INFO, "replacing header in front, copying %5.2f%-60s\n",
           data_size/(1024.0*1024), "MB");

    size = data_size;
    rsize = get_buffer(rpb, rbuf, FFMIN(buf_size, data_size));
    size -= rsize;

    mov->stco_offset = moov_size - mov->free_size;
    mov_write_moov_tag(pb, mov, s);

    prev_time = start_time = av_gettime();
    while (size > 0) {
        if (url_interrupt_cb())
            break;
        put_buffer(pb, wbuf, wsize);
        FFSWAP(uint8_t*, rbuf, wbuf);
        wsize = rsize;
        rsize = FFMIN(size, buf_size);
        get_buffer(rpb, rbuf, rsize);
        size -= rsize;
        if (av_gettime() - prev_time > 300000) {
            int hours, mins, secs, us;
            double speed;
            prev_time = av_gettime();
            speed = (double)(data_size - size) / (prev_time - start_time);
            break_time(size / speed, &hours, &mins, &secs, &us);
            av_log(s, AV_LOG_INFO,
                   "left=%8.2fMB speed=%7.2fMB/s eta=%02d:%02d:%02d.%02d\r",
                   size/(1024.0*1024), speed, hours, mins, secs,
                   (100 * us) / AV_TIME_BASE);
        }
    }

    url_fclose(rpb);

    put_buffer(pb, wbuf, wsize);
    put_buffer(pb, rbuf, rsize);
    av_free(rbuf);
    av_free(wbuf);

    return 0;
}

static int mov_write_trailer(AVFormatContext *s)
{
    MOVMuxContext *mov = s->priv_data;
    ByteIOContext *pb = s->pb;
    int res = 0;
    int i;

    int64_t moov_pos = url_ftell(pb);

    /* Write size of mdat tag */
    if (mov->mdat_size+8 <= UINT32_MAX) {
        url_fseek(pb, mov->mdat_pos, SEEK_SET);
        put_be32(pb, mov->mdat_size+8);
    } else {
        /* overwrite 'wide' placeholder atom */
        url_fseek(pb, mov->mdat_pos - 8, SEEK_SET);
        put_be32(pb, 1); /* special value: real atom size will be 64 bit value after tag field */
        put_tag(pb, "mdat");
        put_be64(pb, mov->mdat_size+16);
        mov->mdat_pos -= 8;
        mov->free_size -= 8;
    }

    put_flush_packet(pb);

    if (mov->free_size > 8) {
        int moov_size = mov_compute_moov_size(s);
        if (moov_size > mov->free_size) {
            av_log(s, AV_LOG_ERROR, "moov size is bigger than available space\n");
            goto write_end;
        }
        url_fseek(pb, mov->free_pos, SEEK_SET);
        mov_write_moov_tag(pb, mov, s);
        mov_write_free_tag(pb, mov, mov->free_size - moov_size);
    } else if (mov->overwrite > 0 ||
               (mov->overwrite != -1 && moov_pos < 20000000)) {
        if (mov_overwrite_file(s) < 0)
            goto write_end;
    } else {
    write_end:
        url_fseek(pb, moov_pos, SEEK_SET);
        mov_write_moov_tag(pb, mov, s);
    }

    if (mov->chapter_track)
        av_freep(&mov->tracks[mov->chapter_track].enc);

    if (mov->timecode_track)
        av_freep(&mov->tracks[mov->timecode_track].enc);

    for (i=0; i<mov->nb_streams; i++) {
        if (mov->tracks[i].tag == MKTAG('r','t','p',' '))
            ff_mov_close_hinting(&mov->tracks[i]);
        av_freep(&mov->tracks[i].cluster);

        if(mov->tracks[i].vosLen) av_free(mov->tracks[i].vosData);

    }

    put_flush_packet(pb);

    av_freep(&mov->tracks);

    return res;
}

#define FAST_START_OPTION \
    { "faststart", "Pre-allocate space for the header in front of the file: <size or 'auto' or 'no'>\n" \
      "Files are automatically rewritten if size is < 20MB unless 'no' is specified.\n", \
      offsetof(MOVMuxContext, faststart), FF_OPT_TYPE_STRING, 0, 0, 0, AV_OPT_FLAG_ENCODING_PARAM} \

static const AVOption options[] = {
    FAST_START_OPTION,
    { NULL },
};

static const AVClass class = { "isom", av_default_item_name, options, LIBAVUTIL_VERSION_INT };

#if CONFIG_F4V_MUXER
AVOutputFormat ff_f4v_muxer = {
    "f4v",
    NULL_IF_CONFIG_SMALL("Flash F4V format"),
    NULL,
    "f4v",
    sizeof(MOVMuxContext),
    CODEC_ID_AAC,
    CODEC_ID_H264,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){codec_f4v_tags, 0},
    .priv_class = &class,
};
#endif
#if CONFIG_MOV_MUXER
static const AVOption mov_options[] = {
    FAST_START_OPTION,
    { "timecode", "Set timecode value: 00:00:00[:;]00, use ';' before frame number for drop frame",
      offsetof(MOVMuxContext, timecode), FF_OPT_TYPE_STRING, 0, 0, 0, AV_OPT_FLAG_ENCODING_PARAM},
    { NULL },
};

static const AVClass mov_class = { "mov", av_default_item_name, mov_options, LIBAVUTIL_VERSION_INT };

AVOutputFormat ff_mov_muxer = {
    "mov",
    NULL_IF_CONFIG_SMALL("MOV format"),
    NULL,
    "mov",
    sizeof(MOVMuxContext),
    CODEC_ID_AAC,
    CODEC_ID_MPEG4,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){codec_movvideo_tags, codec_movaudio_tags, 0},
    .priv_class = &mov_class,
};
#endif
#if CONFIG_TGP_MUXER
AVOutputFormat ff_tgp_muxer = {
    "3gp",
    NULL_IF_CONFIG_SMALL("3GP format"),
    NULL,
    "3gp",
    sizeof(MOVMuxContext),
    CODEC_ID_AMR_NB,
    CODEC_ID_H263,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){codec_3gp_tags, 0},
    .priv_class = &class,
};
#endif
#if CONFIG_MP4_MUXER
AVOutputFormat ff_mp4_muxer = {
    "mp4",
    NULL_IF_CONFIG_SMALL("MP4 format"),
    "application/mp4",
    "mp4",
    sizeof(MOVMuxContext),
    CODEC_ID_AAC,
    CODEC_ID_MPEG4,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){ff_mp4_obj_type, 0},
    .priv_class = &class,
};
#endif
#if CONFIG_PSP_MUXER
AVOutputFormat ff_psp_muxer = {
    "psp",
    NULL_IF_CONFIG_SMALL("PSP MP4 format"),
    NULL,
    "mp4,psp",
    sizeof(MOVMuxContext),
    CODEC_ID_AAC,
    CODEC_ID_MPEG4,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){ff_mp4_obj_type, 0},
    .priv_class = &class,
};
#endif
#if CONFIG_TG2_MUXER
AVOutputFormat ff_tg2_muxer = {
    "3g2",
    NULL_IF_CONFIG_SMALL("3GP2 format"),
    NULL,
    "3g2",
    sizeof(MOVMuxContext),
    CODEC_ID_AMR_NB,
    CODEC_ID_H263,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){codec_3gp_tags, 0},
    .priv_class = &class,
};
#endif
#if CONFIG_IPOD_MUXER
AVOutputFormat ff_ipod_muxer = {
    "ipod",
    NULL_IF_CONFIG_SMALL("iPod H.264 MP4 format"),
    "application/mp4",
    "m4v,m4a",
    sizeof(MOVMuxContext),
    CODEC_ID_AAC,
    CODEC_ID_H264,
    mov_write_header,
    ff_mov_write_packet,
    mov_write_trailer,
    .flags = AVFMT_GLOBALHEADER,
    .codec_tag = (const AVCodecTag* const []){codec_ipod_tags, 0},
    .priv_class = &class,
};
#endif
