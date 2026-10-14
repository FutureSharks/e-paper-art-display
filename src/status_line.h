#ifndef STATUS_LINE_H
#define STATUS_LINE_H

#include <stdint.h>

#include "battery.h"

/*
 * A single line of small text in the top-left corner of the image - which
 * image this is ("Image 3/14"), its file name, the battery level and when it
 * was drawn - switched by SHOW_IMAGE_INDEX, SHOW_FILE_NAME,
 * SHOW_BATTERY_PERCENTAGE and SHOW_REFRESH_TIME in config.h. A line too wide
 * for the panel wraps onto a second.
 *
 * It is drawn black on a white box into a buffer of a few KB and painted over
 * the frame as it streams to the panel - see text_box.h.
 */

/* Compose the line and hand it to the panel driver, for the next stream.
 * `position` is 1-based. `name` is the frame's file name from the manifest.
 * `battery` is null if the gauge did not answer. Does nothing when every
 * option is off. */
void status_line_begin(uint32_t position, uint32_t count, const char *name,
                       const BatteryStatus *battery);

/* Detach the line from the driver and free it. Safe to call regardless. */
void status_line_end();

#endif
