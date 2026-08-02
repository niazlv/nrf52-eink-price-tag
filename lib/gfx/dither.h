#ifndef DITHER_H
#define DITHER_H

/*
 * Portable ordered (Bayer 4×4) spatial dithering — the software gray-tone
 * generator. Hardware-independent: it renders onto the active graphics canvas
 * via graphics_draw_pixel(), so it works on any 1-bpp (+ red) buffer the
 * graphics module backs. Used to fake gray/pink and red↔black mixes on a panel
 * that only has B/W/R pixels.
 *
 * `level` is a 0..16 coverage on the 16-step Bayer scale (0 = empty/white,
 * 16 = solid `color`). `phase` shifts the matrix so adjacent swatches don't
 * align their patterns.
 */

/* Fill a rect with a single dithered colour at the given coverage level. */
void dither_fill_rect(int x, int y, int width, int height,
                      int color, int level, int phase);

/* Fill a rect with a two-ink mix: the lowest thresholds become RED, the next
 * band BLACK, the rest WHITE. red_level/black_level are 0..16 coverages that
 * stack (red first, then black on top). */
void dither_fill_rect_mix(int x, int y, int width, int height,
                          int red_level, int black_level, int phase);

#endif /* DITHER_H */
