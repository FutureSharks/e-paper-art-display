// Copy this file to secrets.h and fill in your values
// secrets.h is in .gitignore - it won't be committed to GitHub

#ifndef SECRETS_H
#define SECRETS_H

// ===== WiFi Configuration =====
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ===== Artwork Source =====
// Artwork is served as static files. Anything reachable over HTTPS
// works; this points at the repo itself. The trailing slash matters, and
// manifest.txt is expected alongside the images.
#define ART_BASE_URL "https://raw.githubusercontent.com/FutureSharks/e-paper-art-display/main/artwork/processed/"

// ===== Artwork Access Token (optional) =====
// Leave empty when ART_BASE_URL is public. Otherwise it is sent with every
// request as "Authorization: Bearer <token>", which works with any host that
// accepts a bearer token. For a private GitHub repo, create a fine-grained
// personal access token limited to that one repo with read-only "Contents"
// permission; GitHub answers a bad or expired one with "HTTP 404". The token
// is stored in the device's flash in plain text, so keep its scope that
// narrow and give it an expiry.
#define ART_AUTH_TOKEN ""

#endif
