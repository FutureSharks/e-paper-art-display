#ifndef STATUS_LED_H
#define STATUS_LED_H

/*
 * Boot-stage feedback on the Feather's onboard NeoPixel, so the device can be
 * watched without a serial monitor attached - useful once it is in the frame
 * and the USB port is no longer convenient.
 *
 * Active only when DEBUG_MODE is set: the NeoPixel and the rail that powers it
 * both cost current that a battery-powered frame should not be spending.
 *
 * Colours:
 *   white    booting
 *   orange   reading the battery gauge
 *   blue     associating with WiFi
 *   cyan     syncing the clock
 *   yellow   fetching the manifest and the image
 *   magenta  writing to the panel (the ~35 s refresh)
 *   green    done
 *   red      failed
 */

enum StatusStage
{
  STATUS_BOOT,
  STATUS_BATTERY,
  STATUS_WIFI,
  STATUS_NTP,
  STATUS_FETCH,
  STATUS_DISPLAY,
  STATUS_DONE,
  STATUS_ERROR,
};

/* Power up the NeoPixel and show STATUS_BOOT. No-op unless DEBUG_MODE. */
void status_led_begin();

/* Show the colour for a stage. */
void status_led(StatusStage stage);

/* Blank the pixel and drop its power rail. Call before deep sleep - a NeoPixel
 * holds its colour and keeps drawing until it loses power. */
void status_led_off();

#endif
