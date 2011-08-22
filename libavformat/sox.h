/*
 * SoX native format common data
 * Copyright (c) 2009 Daniel Verkamp <daniel@drv.nu>
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

#ifndef AVFORMAT_SOX_H
#define AVFORMAT_SOX_H

#define SOX_FIXED_HDR (4 + 8 + 8 + 4 + 4) /**< Size of fixed header without magic */

#define SOX_TAG MKTAG('.', 'S', 'o', 'X')

#endif /* AVFORMAT_SOX_H */
