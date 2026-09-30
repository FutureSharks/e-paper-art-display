#include "status_line.h"

#include <Arduino.h>
#include <ArduinoLog.h>
#include <time.h>

#include "config.h"
#include "text_box.h"

/* Only this one size is linked; roboto_font(size) would pull in all of them.
 * Swap both lines for another roboto<N>.h to change the size. */
#include "roboto10.h"
static const GFXfont &FONT = Roboto10;

/* Padding between the text and the edge of its white box. */
static const int PAD_X = 8;
static const int PAD_Y = 4;

/* Append to `text` with a separator if something is already there. */
static void append(char *text, size_t len, const char *item)
{
  size_t used = strlen(text);
  snprintf(text + used, len - used, "%s%s", used > 0 ? "  |  " : "", item);
}

void status_line_begin(uint32_t position, uint32_t count, const BatteryStatus *battery)
{
  status_line_end();

  char text[96] = "";
  char item[32];

#if SHOW_IMAGE_INDEX
  snprintf(item, sizeof(item), "Image %u/%u", (unsigned)position, (unsigned)count);
  append(text, sizeof(text), item);
#endif

#if SHOW_BATTERY_PERCENTAGE
  if (battery == nullptr)
    snprintf(item, sizeof(item), "Battery unknown");
  else if (!battery->present)
    snprintf(item, sizeof(item), "USB power");
  else
    snprintf(item, sizeof(item), "Battery %d%%", (int)(battery->percent + 0.5f));
  append(text, sizeof(text), item);
#endif

#if SHOW_REFRESH_TIME
  /* Local time, set by time_set() before anything is fetched. Taken now, a
   * few seconds before the stream starts and under a minute before the panel
   * finishes refreshing. */
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  strftime(item, sizeof(item), "%d %b %H:%M", &local);
  append(text, sizeof(text), item);
#endif

  if (text[0] == '\0')
    return;

  if (text_box_begin(&FONT, PAD_X, PAD_Y, text))
    Log.notice("Status line: \"%s\"" CR, text);
}

void status_line_end()
{
  text_box_end();
}
