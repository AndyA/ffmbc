/*
 * RSO format common data
 * Copyright (c) 2010 Rafael Carre
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
#include "internal.h"
#include "rso.h"

const AVCodecTag ff_codec_rso_tags[] = {
    { CODEC_ID_PCM_U8,          0x0100 },
    { CODEC_ID_ADPCM_IMA_WAV,   0x0101 },
    { CODEC_ID_NONE, 0 },
};
