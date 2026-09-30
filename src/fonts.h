#ifndef FONTS_H
#define FONTS_H

#include "roboto_types.h"

/*
 * The only Roboto sizes linked. Each roboto<N>.h defines its glyphs with
 * internal linkage, so including one in two files would link it twice: they
 * are included once, in fonts.cpp, and shared from here. Swap the headers
 * there for another roboto<N>.h to change a size.
 */

/* The status line, and the detail lines of the error screen. */
extern const GFXfont &FONT_SMALL;

/* The first line of the error screen. */
extern const GFXfont &FONT_LARGE;

#endif
