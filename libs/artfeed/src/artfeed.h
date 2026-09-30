/*
 * artfeed.h: pull a pre-packed e-paper frame over HTTPS and stream it to the
 * panel.
 *
 * The frames are static files on a web host - in this project, in the repo
 * itself served through raw.githubusercontent.com. A manifest lists what is
 * available; the device takes the next one in order (or one at random) and
 * streams it into the controllers without ever holding the 960 KB frame in
 * memory.
 *
 * Bringing up WiFi and the clock is the caller's job - see src/setup_wifi.h
 * and src/setup_time.h. Nothing reaches the panel until the refresh, so a
 * failed download leaves the previous image in place.
 */
#pragma once

#include <Arduino.h>

/* Where the work has got to. Reported from inside the library because the
 * download and the refresh both happen within artfeed_show(), so a caller
 * cannot tell them apart from outside. */
enum ArtfeedStage
{
    ARTFEED_STAGE_MANIFEST, // reading the list of images
    ARTFEED_STAGE_DOWNLOAD, // streaming a frame into the controllers
    ARTFEED_STAGE_REFRESH,  // the panel's ~35 s refresh
};

struct ArtfeedConfig
{
    /* Base URL the image files sit under, with a trailing slash. The manifest
     * is expected at <base_url>manifest.txt. */
    const char *base_url;

    /* Walk the manifest in order instead of picking at random, resuming where
     * the last wake left off. avoid_recent is ignored in this mode. The
     * position is kept in NVS, so it survives deep sleep and power loss. */
    bool sequential;

    /* How many recently shown images to avoid repeating in random mode. Also
     * kept in NVS. 0 disables the check. */
    uint8_t avoid_recent;

    /* Skip TLS certificate validation. Convenient while bringing the network
     * path up; leave false in a real build. */
    bool insecure;

    /* Request timeout, and how long a frame download may stall before it is
     * treated as dead. */
    uint32_t http_timeout_ms;

    /* Optional progress hook, for status indication. May be null. */
    void (*on_stage)(ArtfeedStage stage);
};

/* Library defaults. The sketch overrides these from src/config.h - a library
 * is compiled out of tree and cannot include the sketch's headers. base_url
 * has no default and must be set. */
ArtfeedConfig artfeed_default_config();

/* Fetch the manifest and choose an image - the next one in order if
 * cfg.sequential, otherwise at random avoiding recent repeats. The chosen name
 * is written to `name`, and if given, its 1-based position in the manifest and
 * the number of entries to `position` and `count`. Returns false if the
 * manifest could not be read or is empty. */
bool artfeed_pick(const ArtfeedConfig &cfg, char *name, size_t name_len,
                  uint32_t *position = nullptr, uint32_t *count = nullptr);

/* Download `name` and stream it into the panel: panel init, both halves, then
 * refresh. Returns false without refreshing if the download fails or is the
 * wrong length, leaving the current image untouched.
 *
 * Drops the radio before the refresh, so WiFi is down on return either way. */
bool artfeed_show(const ArtfeedConfig &cfg, const char *name);

/* Which manifest entry the next sequential pick will use, and how many were
 * seen last time. Both read back from NVS, for logging and diagnostics. */
uint32_t artfeed_next_index();
