#include "status_led.h"

#include <Arduino.h>

#include "config.h"
#include "power_rail.h"

#if DEBUG_MODE && defined(PIN_NEOPIXEL)

/* Dim on purpose. Full brightness on these is unpleasant to sit next to and
 * wastes current for no extra information. */
static const uint8_t LEVEL = 10;

static bool led_ready = false;

static void show(uint8_t r, uint8_t g, uint8_t b)
{
  if (!led_ready)
    return;
  neopixelWrite(PIN_NEOPIXEL, r, g, b);
}

void status_led_begin()
{
  /* Same switched rail the STEMMA connector uses; the NeoPixel is dark without
   * it. battery_begin() may already have enabled this, which is harmless. */
  power_rail_on();

  led_ready = true;
  status_led(STATUS_BOOT);
}

void status_led(StatusStage stage)
{
  switch (stage)
  {
  case STATUS_BOOT:    show(LEVEL, LEVEL, LEVEL); break;
  case STATUS_BATTERY: show(LEVEL, LEVEL / 3, 0); break;
  case STATUS_WIFI:    show(0, 0, LEVEL); break;
  case STATUS_NTP:     show(0, LEVEL, LEVEL); break;
  case STATUS_FETCH:   show(LEVEL, LEVEL, 0); break;
  case STATUS_DISPLAY: show(LEVEL, 0, LEVEL); break;
  case STATUS_DONE:    show(0, LEVEL, 0); break;
  case STATUS_ERROR:   show(LEVEL, 0, 0); break;
  }
}

void status_led_off()
{
  show(0, 0, 0);
  led_ready = false;
  power_rail_off();
}

#else // !DEBUG_MODE

void status_led_begin() {}
void status_led(StatusStage) {}
void status_led_off() {}

#endif
