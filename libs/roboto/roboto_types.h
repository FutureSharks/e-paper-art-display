#pragma once
#include <stdint.h>

/*
 * Font data types for the generated roboto<size>.h headers.
 *
 * The headers come from epdiy's fontconvert.py and were written against its
 * epd_driver.h. Only the types are needed here, not the driver, so they are
 * reproduced with the same field order - the headers initialise them
 * positionally.
 */

typedef struct
{
    uint16_t width;           /* bitmap size in pixels */
    uint16_t height;
    uint16_t advance_x;       /* distance to the next glyph's origin */
    int16_t left;             /* origin to the bitmap's left edge */
    int16_t top;              /* baseline to the bitmap's top edge */
    uint32_t compressed_size; /* bytes at data_offset (zlib stream if compressed) */
    uint32_t data_offset;     /* into GFXfont.bitmap */
} GFXglyph;

/* A run of consecutive code points, stored from glyph[offset] onwards. */
typedef struct
{
    uint32_t first;
    uint32_t last;
    uint32_t offset;
} UnicodeInterval;

/* Glyph bitmaps are 4 bits per pixel, (width + 1) / 2 bytes per row, even
 * columns in the low nibble. 0 is no ink, 15 is solid. */
typedef struct
{
    uint8_t *bitmap;
    GFXglyph *glyph;
    UnicodeInterval *intervals;
    uint32_t interval_count;
    uint8_t compressed; /* each glyph is its own zlib stream */
    uint16_t advance_y; /* line height */
    int ascender;
    int descender;
} GFXfont;
