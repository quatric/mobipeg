/*
 * Em-Flash (Pixter Multimedia .mo fla2/fla3) renderer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_EMFLASH_H
#define AVCODEC_EMFLASH_H

#include <stdint.h>

typedef struct EFMovie EFMovie;

/**
 * Parse an uncompressed SWF file (FWS) of version 3..5.
 * The SWF data must stay valid until ff_emflash_close().
 * out_w / out_h select the output size, 0 means stage size / 20 (pixels).
 */
EFMovie *ff_emflash_open(const uint8_t *swf, int size, int out_w, int out_h);

void  ff_emflash_get_size(const EFMovie *m, int *w, int *h);
float ff_emflash_get_rate(const EFMovie *m);
int   ff_emflash_get_nframes(const EFMovie *m);

/** Draw the current display list into an RGBA buffer (stride in bytes). */
void ff_emflash_render(EFMovie *m, uint8_t *rgba, int stride);

/** Advance the root timeline (and every playing sprite) by one frame. */
void ff_emflash_step(EFMovie *m);

/**
 * Press any button that has been on stage for the given number of frames,
 * so menus that wait for input keep going.  0 (the default) disables it.
 */
void ff_emflash_set_autoclick(EFMovie *m, int frames);

/** Seed of Math.random() and random(); the default is 1. */
void ff_emflash_set_seed(EFMovie *m, uint32_t seed);

void ff_emflash_close(EFMovie *m);

#endif /* AVCODEC_EMFLASH_H */
