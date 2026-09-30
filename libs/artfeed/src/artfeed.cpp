/*
 * artfeed.cpp: see artfeed.h.
 */
#include "artfeed.h"

#include <ArduinoLog.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdarg.h>
#include "el133.h"

/* The Mozilla root store that ships with arduino-esp32, embedded in the core's
 * mbedTLS build. Costs flash rather than RAM, and keeps working when the host
 * rotates its leaf or intermediate certificates - which it will. Pinning one
 * root instead would be smaller and considerably more fragile. */
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

/* Chunk pulled off the socket and pushed to the panel in one go. Small enough
 * to stay clear of the TLS buffers, large enough that per-write overhead is
 * irrelevant against a 960 KB frame. */
static const size_t ARTFEED_CHUNK = 2048;

/* A manifest is one filename per line; this caps how many we will consider. */
static const size_t ARTFEED_MAX_ENTRIES = 512;

/* Any clock at or past this (2023-11-14) is good enough to check a certificate
 * against. Below it, the clock has not been set since power-on. */
static const time_t ARTFEED_CLOCK_SANE = 1700000000;

static ArtfeedError last_error;

const ArtfeedError &artfeed_last_error()
{
    return last_error;
}

/* Start the record for a request to `url`, clearing the last reason. */
static void error_begin(const String &url)
{
    strlcpy(last_error.url, url.c_str(), sizeof(last_error.url));
    last_error.reason[0] = '\0';
}

static void error_reason(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void error_reason(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_error.reason, sizeof(last_error.reason), fmt, args);
    va_end(args);
}

/* The reason phrase for the statuses a static file host is likely to send,
 * since HTTPClient only has text for its own negative codes. */
static const char *http_reason(int code)
{
    switch (code)
    {
    case 301: return " Moved Permanently";
    case 302: return " Found";
    case 400: return " Bad Request";
    case 401: return " Unauthorized";
    case 403: return " Forbidden";
    case 404: return " Not Found";
    case 408: return " Request Timeout";
    case 429: return " Too Many Requests";
    case 500: return " Internal Server Error";
    case 502: return " Bad Gateway";
    case 503: return " Service Unavailable";
    case 504: return " Gateway Timeout";
    default:  return "";
    }
}

ArtfeedConfig artfeed_default_config()
{
    /* Deliberately not read from the sketch's src/config.h: a library is
     * compiled out of tree and cannot see it. The sketch overrides these. */
    ArtfeedConfig cfg = {};
    cfg.base_url = nullptr;
    cfg.sequential = true;
    cfg.avoid_recent = 8;
    cfg.insecure = false;
    cfg.http_timeout_ms = 20000;
    cfg.on_stage = nullptr;
    return cfg;
}

uint32_t artfeed_next_index()
{
    Preferences prefs;
    prefs.begin("artfeed", true);
    uint32_t pos = prefs.getUInt("seq", 0);
    prefs.end();
    return pos;
}

/* Shared setup for both requests. The caller owns both objects. */
static void artfeed_prepare(const ArtfeedConfig &cfg, NetworkClientSecure &client,
                            HTTPClient &http)
{
    if (cfg.insecure)
    {
        client.setInsecure();
    }
    else
    {
        client.setCACertBundle(rootca_crt_bundle_start,
                               (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
    }
    client.setTimeout(cfg.http_timeout_ms / 1000);
    http.setTimeout(cfg.http_timeout_ms);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
}

/* Report why a request failed in enough detail to tell the causes apart: a
 * negative code is a transport failure (DNS, TCP, TLS), a positive one is an
 * HTTP status and the server said something worth reading. A 404 body of
 * "404: Not Found" means the file is not on the host - most often because the
 * commit adding it has not been pushed. The short form goes in last_error. */
static void artfeed_report_failure(NetworkClientSecure &client, HTTPClient &http,
                                   const char *what, const char *url, int code)
{
    const String code_text = http.errorToString(code);
    Log.error("%s failed" CR, what);
    Log.error("  url    %s" CR, url);
    Log.error("  code   %d (%s)" CR, code, code_text.c_str());

    if (code < 0)
    {
        /* Transport-level: nothing HTTP to show, but mbedTLS usually has a
         * reason, and the WiFi state distinguishes "lost the AP" from
         * "handshake rejected". */
        char err[128] = {0};
        int tls = client.lastError(err, sizeof(err));
        Log.error("  tls    %d %s" CR, tls, err[0] ? err : "(no detail)");
        Log.error("  wifi   %s, rssi %d dBm, dns %s" CR,
                  WiFi.isConnected() ? "connected" : "DISCONNECTED", (int)WiFi.RSSI(),
                  WiFi.dnsIP().toString().c_str());

        if (!WiFi.isConnected())
            error_reason("Error %d: %s (WiFi dropped)", code, code_text.c_str());
        else if (tls != 0 && err[0] != '\0')
            error_reason("Error %d: %s (TLS: %s)", code, code_text.c_str(), err);
        else
            error_reason("Error %d: %s", code, code_text.c_str());
    }
    else
    {
        error_reason("HTTP %d%s", code, http_reason(code));
        Log.error("  length %d" CR, http.getSize());
        String body = http.getString();
        if (body.length() > 160)
            body = body.substring(0, 160) + "...";
        body.replace("\n", " ");
        Log.error("  body   %s" CR, body.c_str());
    }
    Log.error("  heap   %u free" CR, (unsigned)ESP.getFreeHeap());
}

/* Remember the last few choices so the same piece does not come round twice in
 * a row. Stored as a ring in NVS. */
static bool artfeed_recently_shown(Preferences &prefs, uint8_t depth, const char *name)
{
    char key[16];
    for (uint8_t i = 0; i < depth; i++)
    {
        snprintf(key, sizeof(key), "r%u", (unsigned)i);
        String seen = prefs.getString(key, "");
        if (seen.length() > 0 && seen.equals(name))
            return true;
    }
    return false;
}

static void artfeed_remember(Preferences &prefs, uint8_t depth, const char *name)
{
    if (depth == 0)
        return;

    uint8_t head = prefs.getUChar("head", 0) % depth;
    char key[16];
    snprintf(key, sizeof(key), "r%u", (unsigned)head);
    prefs.putString(key, name);
    prefs.putUChar("head", (uint8_t)((head + 1) % depth));
}

static inline void report(const ArtfeedConfig &cfg, ArtfeedStage stage)
{
    if (cfg.on_stage != nullptr)
        cfg.on_stage(stage);
}

bool artfeed_pick(const ArtfeedConfig &cfg, char *name, size_t name_len, uint32_t *position,
                  uint32_t *count_out)
{
    report(cfg, ARTFEED_STAGE_MANIFEST);

    String url = String(cfg.base_url) + "manifest.txt";

    NetworkClientSecure client;
    HTTPClient http;
    artfeed_prepare(cfg, client, http);

    Log.notice("GET %s" CR, url.c_str());
    error_begin(url);
    if (!http.begin(client, url))
    {
        Log.error("Cannot parse url %s" CR, url.c_str());
        error_reason("Not a valid URL");
        return false;
    }

    uint32_t t0 = millis();
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        artfeed_report_failure(client, http, "manifest GET", url.c_str(), code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    Log.verbose("Manifest: %u bytes in %u ms" CR, (unsigned)body.length(),
                (unsigned)(millis() - t0));

    /* Collect the line offsets rather than copying the names; a manifest of a
     * few hundred entries is only a few KB, but there is no reason to double
     * it. */
    int starts[ARTFEED_MAX_ENTRIES];
    int lengths[ARTFEED_MAX_ENTRIES];
    size_t count = 0;

    int i = 0;
    const int len = body.length();
    while (i < len && count < ARTFEED_MAX_ENTRIES)
    {
        int end = body.indexOf('\n', i);
        if (end < 0)
            end = len;

        int lineEnd = end;
        while (lineEnd > i && (body[lineEnd - 1] == '\r' || body[lineEnd - 1] == ' '))
            lineEnd--;

        if (lineEnd > i && body[i] != '#')
        {
            starts[count] = i;
            lengths[count] = lineEnd - i;
            count++;
        }
        i = end + 1;
    }

    if (count == 0)
    {
        /* The request succeeded but produced no usable lines - normally an
         * error page served with a 200, or a manifest of only comments. */
        String preview = body.substring(0, body.length() < 160 ? body.length() : 160);
        preview.replace("\n", " ");
        Log.error("Manifest has no usable entries; body was: %s" CR, preview.c_str());
        error_reason("Manifest has no usable entries (%u bytes)", (unsigned)body.length());
        return false;
    }
    Log.notice("Manifest lists %u image(s)" CR, (unsigned)count);

    Preferences prefs;
    prefs.begin("artfeed", false);

    size_t chosen = 0;
    uint8_t depth = 0;

    if (cfg.sequential)
    {
        /* Resume where the last wake left off. Taken modulo the current count
         * so the position stays valid when images are added or removed. */
        uint32_t pos = prefs.getUInt("seq", 0) % count;
        chosen = (size_t)pos;
        prefs.putUInt("seq", (uint32_t)((pos + 1) % count));
    }
    else
    {
        /* esp_random() is a real hardware RNG, so no seeding needed. Try a
         * handful of times to dodge a recent repeat, then accept whatever we
         * have - with fewer images than the avoid depth, every choice is a
         * repeat. */
        depth = cfg.avoid_recent;
        if (depth >= count)
            depth = (count > 1) ? (uint8_t)(count - 1) : 0;

        for (int attempt = 0; attempt < 12; attempt++)
        {
            chosen = (size_t)(esp_random() % count);
            String candidate =
                body.substring(starts[chosen], starts[chosen] + lengths[chosen]);
            if (depth == 0 || !artfeed_recently_shown(prefs, depth, candidate.c_str()))
                break;
        }
    }

    String pick = body.substring(starts[chosen], starts[chosen] + lengths[chosen]);
    if (!cfg.sequential)
        artfeed_remember(prefs, depth, pick.c_str());
    prefs.end();

    if (pick.length() + 1 > name_len)
    {
        Log.error("Name \"%s\" too long for buffer" CR, pick.c_str());
        error_reason("Image name is over %u characters: %s", (unsigned)(name_len - 1),
                     pick.c_str());
        return false;
    }
    strncpy(name, pick.c_str(), name_len - 1);
    name[name_len - 1] = '\0';

    Log.notice("Chose \"%s\" (%u of %u, %s)" CR, name, (unsigned)(chosen + 1),
               (unsigned)count, cfg.sequential ? "in order" : "random");

    if (position != nullptr)
        *position = (uint32_t)(chosen + 1);
    if (count_out != nullptr)
        *count_out = (uint32_t)count;
    return true;
}

bool artfeed_show(const ArtfeedConfig &cfg, const char *name)
{
    String url = String(cfg.base_url) + name;

    /* Init the panel first. It takes ~7 s of blocking delays, and doing that
     * after the response headers arrive leaves the socket unread for long
     * enough that the receive window fills and the connection stalls or the
     * server drops it. Nothing is displayed until the refresh, so paying this
     * cost before we know the download will succeed is harmless. */
    uint32_t t0 = millis();
    el133_init_panel();

    NetworkClientSecure client;
    HTTPClient http;
    artfeed_prepare(cfg, client, http);

    Log.notice("GET %s" CR, url.c_str());
    error_begin(url);
    if (!http.begin(client, url))
    {
        Log.error("Cannot parse url %s" CR, url.c_str());
        error_reason("Not a valid URL");
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        artfeed_report_failure(client, http, "image GET", url.c_str(), code);
        http.end();
        return false;
    }

    int size = http.getSize();
    if (size >= 0 && (size_t)size != EL133_STREAM_BYTES)
    {
        /* Almost always an HTML error page served with a 200, or a frame packed
         * for different geometry. */
        Log.error("Wrong length: %d bytes, expected %u - not a packed frame?" CR, size,
                  (unsigned)EL133_STREAM_BYTES);
        error_reason("Got %d bytes, expected %u - not a packed frame?", size,
                     (unsigned)EL133_STREAM_BYTES);
        http.end();
        return false;
    }

    report(cfg, ARTFEED_STAGE_DOWNLOAD);
    Log.notice("Streaming %u bytes" CR, (unsigned)EL133_STREAM_BYTES);
    t0 = millis();

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[ARTFEED_CHUNK];
    bool ok = true;

    for (int half = 0; half < 2 && ok; half++)
    {
        el133_stream_begin(half);

        size_t remaining = EL133_HALF_BYTES;
        uint32_t lastData = millis();
        size_t nextMark = EL133_HALF_BYTES - EL133_HALF_BYTES / 4;

        while (remaining > 0)
        {
            size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
            int got = stream->readBytes(buf, want);

            if (got > 0)
            {
                el133_stream_write(buf, (size_t)got);
                remaining -= (size_t)got;
                lastData = millis();

                if (remaining <= nextMark)
                {
                    Log.verbose("  half %d: %u of %u bytes" CR, half,
                                (unsigned)(EL133_HALF_BYTES - remaining),
                                (unsigned)EL133_HALF_BYTES);
                    nextMark = (nextMark > EL133_HALF_BYTES / 4)
                                   ? nextMark - EL133_HALF_BYTES / 4
                                   : 0;
                }
                continue;
            }

            /* An empty read is not automatically the end: TLS records arrive in
             * bursts and the window can stall briefly. Only give up once the
             * peer has gone and there is nothing buffered, or nothing has
             * arrived for a while. */
            if (!stream->connected() && stream->available() == 0)
            {
                Log.error("Connection closed with %u bytes of half %d outstanding" CR,
                          (unsigned)remaining, half);
                error_reason("Connection closed after %u of %u bytes",
                             (unsigned)(half * EL133_HALF_BYTES + EL133_HALF_BYTES - remaining),
                             (unsigned)EL133_STREAM_BYTES);
                ok = false;
                break;
            }
            if (millis() - lastData > cfg.http_timeout_ms)
            {
                Log.error("Stalled for %u ms with %u bytes of half %d outstanding" CR,
                          (unsigned)(millis() - lastData), (unsigned)remaining, half);
                error_reason("Download stalled for %u s after %u of %u bytes",
                             (unsigned)((millis() - lastData) / 1000),
                             (unsigned)(half * EL133_HALF_BYTES + EL133_HALF_BYTES - remaining),
                             (unsigned)EL133_STREAM_BYTES);
                ok = false;
                break;
            }
            delay(10);
        }

        el133_stream_end();
    }

    http.end();

    if (!ok)
    {
        /* Skipping the refresh leaves whatever is on the panel alone. */
        Log.error("Download incomplete, not refreshing" CR);
        return false;
    }
    Log.notice("Streamed %u bytes in %u ms" CR, (unsigned)EL133_STREAM_BYTES,
               (unsigned)(millis() - t0));

    /* The radio is the expensive part of the wake cycle and the refresh is the
     * long part; no reason to overlap them. */
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Log.verbose("Radio off before refresh" CR);

    report(cfg, ARTFEED_STAGE_REFRESH);
    if (!el133_refresh())
    {
        error_reason("The panel did not refresh");
        return false;
    }
    return true;
}
