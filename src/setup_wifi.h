#ifndef SETUP_WIFI_H
#define SETUP_WIFI_H

bool wifi_connect();

/* Why the last wifi_connect() failed, in a few words, for the error screen. */
const char *wifi_failure_reason();
void wifi_reset();

#endif
