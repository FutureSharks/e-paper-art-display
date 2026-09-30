#ifndef ERROR_SCREEN_H
#define ERROR_SCREEN_H

/*
 * The frame shown when something has gone wrong, built into the firmware
 * because by the time it is needed the network has usually failed. It is a
 * packed frame stored zlib-compressed in src/error_image.h, and is inflated
 * straight into the panel - still without holding the 960 KB frame in memory.
 *
 * What went wrong is written over it in a larger box than the status line's,
 * in the top-left corner.
 */

/* Init the panel, stream the error image with `message` over it (one or more
 * lines, split on '\n'), and refresh. Turn the radio off first - the refresh
 * is the heaviest load in the cycle. Returns false if the panel did not
 * refresh, in which case it keeps whatever it showed before. */
bool error_screen_show(const char *message);

#endif
