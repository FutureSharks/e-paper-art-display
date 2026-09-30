#ifndef TEXT_BOX_H
#define TEXT_BOX_H

#include "roboto_types.h"

/*
 * Black text on a white box in the top-left corner of the next frame streamed
 * to the panel. Shared by the status line and the error screen, which differ
 * only in fonts and padding.
 *
 * The box is drawn into a buffer sized to the text, one byte per pixel, and
 * handed to the panel driver as an overlay, so the 960 KB frame itself is
 * never held in memory.
 */

/* Draw `text` - one or more lines, split on '\n' - and attach it to the
 * driver. The first line is set in `font` and the rest in `rest_font`, or
 * everything in `font` if that is null. A line too wide for the panel wraps,
 * after a '/', ' ', '-' or '_' where it can, so a URL breaks between its
 * parts. If there is no memory for the whole box, lines are dropped from the
 * bottom until it fits; returns false, leaving no box, if not even the first
 * line does. */
bool text_box_begin(const GFXfont *font, const GFXfont *rest_font, int pad_x, int pad_y,
                    const char *text);

/* Detach the box from the driver and free it. Safe to call regardless. */
void text_box_end();

#endif
