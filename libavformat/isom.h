/*
 * ISO Media common code
 * copyright (c) 2001 Fabrice Bellard
 * copyright (c) 2002 Francois Revol <revol@free.fr>
 * copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@free.fr>
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

#ifndef AVFORMAT_ISOM_H
#define AVFORMAT_ISOM_H

#include "avio.h"
#include "internal.h"
#include "dv.h"

/* isom.c */
extern const AVCodecTag ff_mp4_obj_type[];
extern const AVCodecTag codec_movvideo_tags[];
extern const AVCodecTag codec_movaudio_tags[];
extern const AVCodecTag ff_codec_movsubtitle_tags[];

/* iso code is wanted when metadata is utf-8 */
int ff_mov_iso639_to_lang(const char *lang, int iso_code);
int ff_mov_lang_to_iso639(unsigned code, char to[4]);

/* the QuickTime file format is quite convoluted...
 * it has lots of index tables, each indexing something in another one...
 * Here we just use what is needed to read the chunks
 */

typedef struct {
    int64_t duration;
    int time;
} MOVElst;

typedef struct {
    int count;
    int duration;
} MOVStts;

typedef struct {
    int first;
    int count;
    unsigned id;
} MOVStsc;

typedef struct {
    uint32_t type;
    char *path;
    char *dir;
    char volume[28];
    char filename[64];
    int16_t nlvl_to, nlvl_from;
    ByteIOContext *pb;
} MOVDref;

typedef struct {
    uint32_t type;
    int64_t size; /* total size (excluding the size and type fields) */
} MOVAtom;

struct MOVParseTableEntry;

typedef struct {
    unsigned track_id;
    uint64_t base_data_offset;
    uint64_t moof_offset;
    unsigned stsd_id;
    unsigned duration;
    unsigned size;
    unsigned flags;
} MOVFragment;

typedef struct {
    unsigned track_id;
    unsigned stsd_id;
    unsigned duration;
    unsigned size;
    unsigned flags;
} MOVTrackExt;

typedef struct MOVStreamContext {
    int ffindex;          ///< AVStream index
    int next_chunk;
    unsigned int chunk_count;
    int64_t *chunk_offsets;
    unsigned int stts_count;
    MOVStts *stts_data;
    unsigned int ctts_count;
    MOVStts *ctts_data;
    unsigned int stsc_count;
    MOVStsc *stsc_data;
    unsigned int stps_count;
    unsigned *stps_data;  ///< partial sync sample for mpeg-2 open gop
    int ctts_index;
    int ctts_sample;
    unsigned int sample_size;
    unsigned int sample_count;
    int *sample_sizes;
    unsigned int keyframe_count;
    int *keyframes;
    int time_scale;
    int time_offset;      ///< time offset of the first edit list entry
    int current_sample;
    unsigned int bytes_per_frame;
    unsigned int samples_per_frame;
    int dv_audio_container;
    int *dref_ids;
    int dref_ids_count;
    ByteIOContext **sample_dref;
    int16_t audio_cid;    ///< stsd audio compression id
    unsigned drefs_count;
    MOVDref *drefs;
    int dref_id;
    int width;            ///< tkhd width
    int height;           ///< tkhd height
    int dts_shift;        ///< dts shift when ctts is negative
    AVRational pixel_aspect; ///< information in 'pasp' atom
    MOVElst *elst_data;   ///< edit list
    unsigned elst_count;
} MOVStreamContext;

typedef struct MOVContext {
    AVFormatContext *fc;
    int time_scale;
    int64_t duration;     ///< duration of the longest track
    int found_moov;       ///< 'moov' atom has been found
    int found_mdat;       ///< 'mdat' atom has been found
    DVDemuxContext *dv_demux;
    AVFormatContext *dv_fctx;
    int isom;             ///< 1 if file is ISO Media (mp4/3gp)
    MOVFragment fragment; ///< current fragment in moof atom
    MOVTrackExt *trex_data;
    unsigned trex_count;
    int itunes_metadata;  ///< metadata are itunes style
    int chapter_track;
    AVMetadata **metadata;///< current metadata context (track or global)
    char **keys_data;     ///< metadata keys
    unsigned keys_count;  ///< metadata keys
} MOVContext;

int ff_mp4_read_descr_len(ByteIOContext *pb);
int ff_mp4_read_descr(AVFormatContext *fc, ByteIOContext *pb, int *tag);
int ff_mp4_read_dec_config_descr(AVFormatContext *fc, AVStream *st, ByteIOContext *pb);

#define MP4IODescrTag                   0x02
#define MP4ESDescrTag                   0x03
#define MP4DecConfigDescrTag            0x04
#define MP4DecSpecificDescrTag          0x05

int ff_mov_read_esds(AVFormatContext *fc, ByteIOContext *pb, MOVAtom atom);
enum CodecID ff_mov_get_lpcm_codec_id(int bps, int flags);

int ff_mov_read_stsd_entries(MOVContext *c, ByteIOContext *pb, int entries);

#endif /* AVFORMAT_ISOM_H */
