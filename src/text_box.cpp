#include "text_box.h"

#include <Arduino.h>
#include <ArduinoLog.h>

#include "el133.h"
#include "roboto_font.h"

/* The glyphs are antialiased to 16 levels, but the panel has no greys: at or
 * above this coverage a pixel is inked, below it the box shows through. */
static const uint8_t INK_THRESHOLD = 8;

/* Longest text accepted, and most lines after wrapping; anything past either
 * is dropped. */
static const size_t MAX_TEXT = 512;
static const int MAX_LINES = 8;

/* A wrapped line may break after any of these. */
static const char BREAK_AFTER[] = "/ -_";

static uint8_t *pixels = nullptr;

struct Canvas
{
  uint8_t *pixels;
  int w, h;
};

struct Line
{
  const char *text;
  const GFXfont *font;
  int baseline;
};

static void plot(int32_t x, int32_t y, uint8_t alpha, void *ctx)
{
  Canvas *canvas = (Canvas *)ctx;
  if (alpha < INK_THRESHOLD || x < 0 || y < 0 || x >= canvas->w || y >= canvas->h)
    return;
  canvas->pixels[(size_t)y * canvas->w + x] = EL133_BLACK;
}

/* How many bytes from the start of `text` fit in `width` pixels: all of it,
 * else up to the last break character that fits, else as many whole
 * characters as fit - and always at least one, so wrapping makes progress.
 * `text` is terminated early while it is measured, then put back. */
static size_t fit(const GFXfont *font, char *text, int32_t width)
{
  const size_t len = strlen(text);
  if (roboto_measure(font, text) <= width)
    return len;

  size_t fits = 0, soft = 0;
  for (size_t i = 1; i <= len; i++)
  {
    /* Only measure at the end of a UTF-8 character. */
    if (i < len && (text[i] & 0xC0) == 0x80)
      continue;

    const char saved = text[i];
    text[i] = '\0';
    const bool ok = roboto_measure(font, text) <= width;
    text[i] = saved;
    if (!ok)
      break;

    fits = i;
    if (strchr(BREAK_AFTER, text[i - 1]) != nullptr)
      soft = i;
  }

  if (soft > 0)
    return soft;
  if (fits > 0)
    return fits;

  /* Not even one character fits: take it anyway, with its continuation
   * bytes. */
  size_t one = 1;
  while (one < len && (text[one] & 0xC0) == 0x80)
    one++;
  return one;
}

bool text_box_begin(const GFXfont *font, const GFXfont *rest_font, int pad_x, int pad_y,
                    const char *text)
{
  text_box_end();

  if (rest_font == nullptr)
    rest_font = font;

  /* Split a copy in place, one terminated string per paragraph. */
  char copy[MAX_TEXT];
  strlcpy(copy, text, sizeof(copy));

  /* Each wrapped line is copied out with its own terminator. */
  char wrapped[MAX_TEXT + MAX_LINES];
  size_t used = 0;

  Line lines[MAX_LINES];
  int count = 0;
  const int32_t max_width = EL133_PORTRAIT_W - 2 * pad_x;

  char *p = copy;
  for (int paragraph = 0; p != nullptr && count < MAX_LINES; paragraph++)
  {
    char *newline = strchr(p, '\n');
    if (newline != nullptr)
      *newline = '\0';

    const GFXfont *line_font = paragraph == 0 ? font : rest_font;
    do
    {
      const size_t n = fit(line_font, p, max_width);
      memcpy(wrapped + used, p, n);
      wrapped[used + n] = '\0';
      lines[count++] = {wrapped + used, line_font, 0};
      used += n + 1;

      /* A space the line broke after is not carried onto the next one. */
      p += n;
      while (*p == ' ')
        p++;
    } while (*p != '\0' && count < MAX_LINES);

    p = newline != nullptr ? newline + 1 : nullptr;
  }

  /* Baselines: a font's own line spacing within it, and its full ascent below
   * the full descent of a different font above. */
  lines[0].baseline = pad_y + lines[0].font->ascender;
  for (int i = 1; i < count; i++)
  {
    const GFXfont *above = lines[i - 1].font;
    const GFXfont *here = lines[i].font;
    lines[i].baseline = lines[i - 1].baseline +
                        (here == above ? here->advance_y : here->ascender - above->descender);
  }

  /* The box is one byte a pixel and can run to a few hundred KB, so shed lines
   * from the bottom rather than lose the whole message. */
  Canvas canvas;
  for (; count > 0; count--)
  {
    int32_t widest = 0;
    for (int i = 0; i < count; i++)
      widest = max(widest, roboto_measure(lines[i].font, lines[i].text));

    canvas.w = min((int)widest + 2 * pad_x, EL133_PORTRAIT_W);
    canvas.h = min(lines[count - 1].baseline - lines[count - 1].font->descender + pad_y,
                   EL133_PORTRAIT_H);
    canvas.pixels = (uint8_t *)malloc((size_t)canvas.w * canvas.h);
    if (canvas.pixels != nullptr)
      break;
    Log.warning("No memory for a %dx%d text box" CR, canvas.w, canvas.h);
  }

  if (count == 0)
  {
    Log.warning("Leaving the text box off" CR);
    return false;
  }
  memset(canvas.pixels, EL133_WHITE, (size_t)canvas.w * canvas.h);

  for (int i = 0; i < count; i++)
  {
    int32_t x = pad_x;
    if (!roboto_draw(lines[i].font, lines[i].text, &x, lines[i].baseline, plot, &canvas))
      Log.warning("Text box only partly drawn" CR);
  }

  El133Overlay overlay = {0, 0, canvas.w, canvas.h, canvas.pixels};
  if (!el133_stream_overlay(&overlay))
  {
    free(canvas.pixels);
    return false;
  }

  pixels = canvas.pixels;
  return true;
}

void text_box_end()
{
  el133_stream_overlay(nullptr);
  free(pixels);
  pixels = nullptr;
}
