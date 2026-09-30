#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "roboto_types.h"

/*
 * Roboto, pre-rendered at the sizes this firmware uses (points at 150 dpi,
 * which is close to the 13.3" panel's own density), and a small renderer.
 *
 * The renderer does not own a framebuffer: it reports each inked pixel to a
 * callback, so the caller can draw into whatever it has - here, a small strip
 * that is overlaid on a frame as it streams.
 *
 * Each size is its own header: include the one you need (e.g. "roboto10.h")
 * and pass &Roboto10 to the functions below.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per inked pixel. `alpha` is the glyph's coverage there, from 1
 * (barely touched) to 15 (solid). */
typedef void (*roboto_plot_fn)(int32_t x, int32_t y, uint8_t alpha, void *ctx);

/* Draw UTF-8 text starting at *x, with its baseline at y, and advance *x past
 * it. Characters the font does not have are skipped. Returns false if a glyph
 * could not be unpacked - out of memory, or corrupt font data - in which case
 * the text is only partly drawn. */
bool roboto_draw(const GFXfont *font, const char *text, int32_t *x, int32_t y,
                 roboto_plot_fn plot, void *ctx);

/* Width of the text in pixels: the sum of its advances. Cheap - nothing is
 * unpacked. */
int32_t roboto_measure(const GFXfont *font, const char *text);

#ifdef __cplusplus
}
#endif
