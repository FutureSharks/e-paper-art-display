#include "text_box.h"

#include <Arduino.h>
#include <ArduinoLog.h>

#include "el133.h"
#include "roboto_font.h"

/* The glyphs are antialiased to 16 levels, but the panel has no greys: at or
 * above this coverage a pixel is inked, below it the box shows through. */
static const uint8_t INK_THRESHOLD = 8;

/* Longest text accepted, and most lines; anything past either is dropped. */
static const size_t MAX_TEXT = 256;
static const int MAX_LINES = 4;

static uint8_t *pixels = nullptr;

struct Canvas
{
  uint8_t *pixels;
  int w, h;
};

static void plot(int32_t x, int32_t y, uint8_t alpha, void *ctx)
{
  Canvas *canvas = (Canvas *)ctx;
  if (alpha < INK_THRESHOLD || x < 0 || y < 0 || x >= canvas->w || y >= canvas->h)
    return;
  canvas->pixels[(size_t)y * canvas->w + x] = EL133_BLACK;
}

bool text_box_begin(const GFXfont *font, int pad_x, int pad_y, const char *text)
{
  text_box_end();

  /* Split a copy in place, one terminated string per line. */
  char copy[MAX_TEXT];
  strlcpy(copy, text, sizeof(copy));

  char *lines[MAX_LINES];
  int count = 0;
  for (char *p = copy; count < MAX_LINES;)
  {
    lines[count++] = p;
    char *newline = strchr(p, '\n');
    if (newline == nullptr)
      break;
    *newline = '\0';
    p = newline + 1;
  }

  int32_t widest = 0;
  for (int i = 0; i < count; i++)
    widest = max(widest, roboto_measure(font, lines[i]));

  Canvas canvas;
  canvas.w = min((int)widest + 2 * pad_x, EL133_PORTRAIT_W);
  canvas.h = min((count - 1) * font->advance_y + font->ascender - font->descender + 2 * pad_y,
                 EL133_PORTRAIT_H);

  canvas.pixels = (uint8_t *)malloc((size_t)canvas.w * canvas.h);
  if (canvas.pixels == nullptr)
  {
    Log.warning("No memory for a %dx%d text box, leaving it off" CR, canvas.w, canvas.h);
    return false;
  }
  memset(canvas.pixels, EL133_WHITE, (size_t)canvas.w * canvas.h);

  for (int i = 0; i < count; i++)
  {
    int32_t x = pad_x;
    if (!roboto_draw(font, lines[i], &x, pad_y + font->ascender + i * font->advance_y, plot,
                     &canvas))
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
