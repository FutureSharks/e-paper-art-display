#include "roboto_font.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The ESP32 ROM carries miniz's inflater, so glyphs can be unpacked without
 * linking a zlib. */
#include "miniz.h"

/* Decode one UTF-8 code point and step past it. A malformed sequence decodes
 * as U+FFFD, which no font here has, so it is simply skipped. Never steps past
 * the terminator. */
static uint32_t next_codepoint(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t cp;
    int extra;

    if (p[0] < 0x80)
    {
        cp = p[0];
        extra = 0;
    }
    else if ((p[0] & 0xE0) == 0xC0)
    {
        cp = p[0] & 0x1F;
        extra = 1;
    }
    else if ((p[0] & 0xF0) == 0xE0)
    {
        cp = p[0] & 0x0F;
        extra = 2;
    }
    else if ((p[0] & 0xF8) == 0xF0)
    {
        cp = p[0] & 0x07;
        extra = 3;
    }
    else
    {
        *text += 1;
        return 0xFFFD;
    }

    for (int i = 1; i <= extra; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
        {
            *text += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *text += 1 + extra;
    return cp;
}

static const GFXglyph *find_glyph(const GFXfont *font, uint32_t cp)
{
    for (uint32_t i = 0; i < font->interval_count; i++)
    {
        const UnicodeInterval *interval = &font->intervals[i];
        if (cp >= interval->first && cp <= interval->last)
            return &font->glyph[interval->offset + (cp - interval->first)];
    }
    return NULL;
}

/* Unpack one glyph's bitmap into `out`, which holds exactly its packed size. */
static bool unpack_glyph(const GFXfont *font, const GFXglyph *glyph, uint8_t *out,
                         size_t out_len, tinfl_decompressor *inflator)
{
    const uint8_t *src = font->bitmap + glyph->data_offset;

    if (!font->compressed)
    {
        memcpy(out, src, out_len);
        return true;
    }

    size_t in_len = glyph->compressed_size;
    size_t produced = out_len;
    tinfl_init(inflator);
    tinfl_status status =
        tinfl_decompress(inflator, src, &in_len, out, out, &produced,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE && produced == out_len;
}

bool roboto_draw(const GFXfont *font, const char *text, int32_t *x, int32_t y,
                 roboto_plot_fn plot, void *ctx)
{
    /* The inflater's state is ~11 KB - too much for the Arduino loop task's
     * stack, so it lives on the heap for the length of the call. */
    tinfl_decompressor *inflator = NULL;
    uint8_t *bitmap = NULL;
    size_t bitmap_cap = 0;
    bool ok = true;

    if (font->compressed)
    {
        inflator = (tinfl_decompressor *)malloc(sizeof(*inflator));
        if (inflator == NULL)
            return false;
    }

    while (*text != '\0')
    {
        const GFXglyph *glyph = find_glyph(font, next_codepoint(&text));
        if (glyph == NULL)
            continue;

        const size_t stride = (glyph->width + 1) / 2;
        const size_t len = stride * glyph->height;

        if (len > 0)
        {
            if (len > bitmap_cap)
            {
                uint8_t *grown = (uint8_t *)realloc(bitmap, len);
                if (grown == NULL)
                {
                    ok = false;
                    break;
                }
                bitmap = grown;
                bitmap_cap = len;
            }

            if (!unpack_glyph(font, glyph, bitmap, len, inflator))
            {
                ok = false;
                break;
            }

            const int32_t left = *x + glyph->left;
            const int32_t top = y - glyph->top;
            for (uint16_t gy = 0; gy < glyph->height; gy++)
            {
                const uint8_t *row = bitmap + gy * stride;
                for (uint16_t gx = 0; gx < glyph->width; gx++)
                {
                    uint8_t alpha = (gx & 1) ? row[gx / 2] >> 4 : row[gx / 2] & 0x0F;
                    if (alpha != 0)
                        plot(left + gx, top + gy, alpha, ctx);
                }
            }
        }

        *x += glyph->advance_x;
    }

    free(bitmap);
    free(inflator);
    return ok;
}

int32_t roboto_measure(const GFXfont *font, const char *text)
{
    int32_t width = 0;
    while (*text != '\0')
    {
        const GFXglyph *glyph = find_glyph(font, next_codepoint(&text));
        if (glyph != NULL)
            width += glyph->advance_x;
    }
    return width;
}
