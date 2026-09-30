#ifndef TEXT_BOX_H
#define TEXT_BOX_H

#include "roboto_types.h"

/*
 * Black text on a white box in the top-left corner of the next frame streamed
 * to the panel. Shared by the status line and the error screen, which differ
 * only in font and padding.
 *
 * The box is drawn into a buffer sized to the text, one byte per pixel, and
 * handed to the panel driver as an overlay, so the 960 KB frame itself is
 * never held in memory.
 */

/* Draw `text` - one or more lines, split on '\n' - and attach it to the
 * driver. Returns false, leaving no box, if there is no memory for it. */
bool text_box_begin(const GFXfont *font, int pad_x, int pad_y, const char *text);

/* Detach the box from the driver and free it. Safe to call regardless. */
void text_box_end();

#endif
