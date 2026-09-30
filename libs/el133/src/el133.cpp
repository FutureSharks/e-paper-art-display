/*
 * el133.cpp: see el133.h.
 *
 * The command sequence, register values and chip-select targets follow
 * Pimoroni's inky_el133uf1.py (MIT). The one deliberate departure is
 * BTST_P/BTST_N - see EL133_SOFT_BOOST.
 */
#include "el133.h"

#include "el133_pins.h"
#include <ArduinoLog.h>
#include <SPI.h>

/* Conservative: Pimoroni runs 10 MHz. The frame takes a few seconds to send
 * either way, against a refresh of ~20 s. */
static const SPISettings SPI_CFG(4000000, MSBFIRST, SPI_MODE0);

static const uint32_t CMD_SETUP_MS = 300; /* D/C low to opcode; shorter drops commands */
static const uint32_t BUSY_ASSERT_TIMEOUT_MS = 5000;
static const uint32_t REFRESH_TIMEOUT_MS = 60000;

static const size_t ROW_BYTES = EL133_PORTRAIT_W / 4; /* one controller's row */

/* The C6 has a single general-purpose SPI peripheral. HSPI compiles but is
 * refused at runtime, and the panel silently receives nothing. */
static SPIClass spi(FSPI);

/* Chip-select targets, as a mask. */
enum : uint8_t
{
    CS_M = 1,
    CS_S = 2,
    CS_BOTH = CS_M | CS_S,
};

static void chip_select(uint8_t cs, uint8_t level)
{
    if (cs & CS_M)
        digitalWrite(EL133_PIN_CS_M, level);
    if (cs & CS_S)
        digitalWrite(EL133_PIN_CS_S, level);
}

static void write_bytes(const uint8_t *data, size_t len)
{
    spi.beginTransaction(SPI_CFG);
    spi.writeBytes(data, len);
    spi.endTransaction();
}

/* Select, clock the opcode, and leave D/C high for any data that follows. */
static void open_command(uint8_t cs, uint8_t cmd)
{
    chip_select(cs, LOW);
    digitalWrite(EL133_PIN_DC, LOW);
    delay(CMD_SETUP_MS);
    write_bytes(&cmd, 1);
    digitalWrite(EL133_PIN_DC, HIGH);
}

static void close_command()
{
    chip_select(CS_BOTH, HIGH);
    digitalWrite(EL133_PIN_DC, LOW);
}

static void send_command(uint8_t cs, uint8_t cmd, const uint8_t *data = nullptr, size_t len = 0)
{
    open_command(cs, cmd);
    if (len > 0)
        write_bytes(data, len);
    close_command();
}

void el133_begin()
{
    pinMode(EL133_PIN_CS_M, OUTPUT);
    pinMode(EL133_PIN_CS_S, OUTPUT);
    pinMode(EL133_PIN_DC, OUTPUT);
    pinMode(EL133_PIN_RST, OUTPUT);
    pinMode(EL133_PIN_BUSY, INPUT_PULLUP); /* open-drain: floats without a pull-up */

    close_command();
    digitalWrite(EL133_PIN_RST, HIGH);

    spi.begin(EL133_PIN_SCLK, EL133_PIN_MISO, EL133_PIN_MOSI, -1);
}

/* ---- Init ------------------------------------------------------------- */

#if EL133_SOFT_BOOST
#define BTST 0xD8, 0x18
#else
#define BTST 0xE0, 0x20
#endif

struct InitCommand
{
    uint8_t cs, cmd, len, data[9];
};

/* In the order Pimoroni sends them. Register names are theirs. */
static const InitCommand INIT[] = {
    {CS_M, 0x74, 9, {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55}}, /* ANTM */
    {CS_BOTH, 0xF0, 6, {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10}},                /* CMD66 */
    {CS_BOTH, 0x00, 2, {0xDF, 0x6B}},                                        /* PSR */
    {CS_M, 0xA5, 3, {0x44, 0x54, 0x00}},                                     /* DCDC */
    {CS_BOTH, 0x30, 1, {0x08}},                                              /* PLL */
    {CS_BOTH, 0x50, 1, {0x37}},                                              /* CDI */
    {CS_BOTH, 0x60, 2, {0x03, 0x03}},                                        /* TCON */
    {CS_M, 0x03, 4, {0x00, 0xC0, 0x03, 0xA8}},                               /* POFS */
    {CS_S, 0x03, 4, {0x00, 0xC0, 0x03, 0x9A}},                               /* POFS */
    {CS_BOTH, 0x86, 1, {0x10}},                                              /* AGID */
    {CS_BOTH, 0xE3, 1, {0x22}},                                              /* PWS */
    {CS_BOTH, 0xE0, 1, {0x01}},                                              /* CCSET */
    {CS_BOTH, 0x61, 4, {0x04, 0xB0, 0x03, 0x20}},                            /* TRES */
    {CS_M, 0xA4, 9, {0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00}}, /* CMDA4 */
    {CS_M, 0x01, 6, {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38}},                   /* PWR */
    {CS_M, 0xB6, 1, {0x07}},                                                 /* EN_BUF */
    {CS_M, 0x06, 2, {BTST}},                                                 /* BTST_P */
    {CS_M, 0xB7, 1, {0x01}},                                                 /* BOOST_VDDP_EN */
    {CS_M, 0x05, 2, {BTST}},                                                 /* BTST_N */
    {CS_M, 0xB0, 1, {0x01}},                                                 /* BUCK_BOOST_VDDN */
    {CS_M, 0xB1, 1, {0x02}},                                                 /* TFT_VCOM_POWER */
};

void el133_init_panel()
{
    digitalWrite(EL133_PIN_RST, LOW);
    delay(30);
    digitalWrite(EL133_PIN_RST, HIGH);
    delay(330);

    const uint32_t t0 = millis();
    for (const InitCommand &c : INIT)
        send_command(c.cs, c.cmd, c.data, c.len);

    Log.notice("Panel init complete in %u ms (boost: %s)" CR, (unsigned)(millis() - t0),
               EL133_SOFT_BOOST ? "soft D8 18" : "standard E0 20");
}

/* ---- Streaming and overlay --------------------------------------------- */

static int stream_half;
static size_t stream_pos;

static El133Overlay overlay;
static bool overlay_on = false;

bool el133_stream_overlay(const El133Overlay *o)
{
    overlay_on = false;
    if (o == nullptr)
        return true;

    if (o->pixels == nullptr || o->w <= 0 || o->h <= 0 || o->x < 0 || o->y < 0 ||
        o->x + o->w > EL133_PORTRAIT_W || o->y + o->h > EL133_PORTRAIT_H)
    {
        Log.error("Overlay %dx%d at (%d,%d) is off the panel, ignoring it" CR, o->w, o->h, o->x,
                  o->y);
        return false;
    }

    overlay = *o;
    overlay_on = true;
    return true;
}

/* Paint the overlay into `n` bytes of one row, starting at stream offset `pos`.
 * Byte k of a row carries portrait columns half*600 + 2k (high nibble) and
 * +1 (low nibble). */
static void patch_row(uint8_t *bytes, size_t pos, size_t n)
{
    const int y = (int)(pos / ROW_BYTES);
    const int x0 = stream_half * (EL133_PORTRAIT_W / 2) + 2 * (int)(pos % ROW_BYTES);
    const uint8_t *src = overlay.pixels + (size_t)(y - overlay.y) * overlay.w;

    for (size_t i = 0; i < n; i++)
    {
        const int x = x0 + 2 * (int)i;
        if (x >= overlay.x && x < overlay.x + overlay.w)
            bytes[i] = (uint8_t)((bytes[i] & 0x0F) | (src[x - overlay.x] << 4));
        if (x + 1 >= overlay.x && x + 1 < overlay.x + overlay.w)
            bytes[i] = (uint8_t)((bytes[i] & 0xF0) | (src[x + 1 - overlay.x] & 0x0F));
    }
}

void el133_stream_begin(int half)
{
    stream_half = half;
    stream_pos = 0;
    open_command(half == 0 ? CS_M : CS_S, 0x10); /* DTM */
}

void el133_stream_write(const uint8_t *data, size_t len)
{
    /* The span of stream offsets whose rows the overlay covers in this half;
     * empty when there is nothing to paint. */
    const int half_x0 = stream_half * (EL133_PORTRAIT_W / 2);
    const bool hits = overlay_on && overlay.x < half_x0 + EL133_PORTRAIT_W / 2 &&
                      overlay.x + overlay.w > half_x0;
    const size_t first = hits ? (size_t)overlay.y * ROW_BYTES : SIZE_MAX;
    const size_t last = hits ? (size_t)(overlay.y + overlay.h) * ROW_BYTES : SIZE_MAX;

    while (len > 0)
    {
        size_t n;
        if (stream_pos < first || stream_pos >= last)
        {
            /* Straight through, stopping where the overlay starts. */
            n = stream_pos < first ? min(len, first - stream_pos) : len;
            write_bytes(data, n);
        }
        else
        {
            /* One row at a time: copy, patch, send. The caller's buffer is
             * left alone. */
            uint8_t row[ROW_BYTES];
            n = min(len, ROW_BYTES - stream_pos % ROW_BYTES);
            memcpy(row, data, n);
            patch_row(row, stream_pos, n);
            write_bytes(row, n);
        }

        data += n;
        len -= n;
        stream_pos += n;
    }
}

void el133_stream_end()
{
    close_command();
}

/* ---- Refresh ----------------------------------------------------------- */

static bool busy()
{
    return digitalRead(EL133_PIN_BUSY) == LOW;
}

/* A refresh is over once BUSY has asserted and then released. Waiting for the
 * assert matters: powering off mid-refresh can latch the panel into a fault
 * that only removing power clears. */
static bool wait_for_refresh()
{
    const uint32_t t0 = millis();

    while (!busy())
    {
        if (millis() - t0 > BUSY_ASSERT_TIMEOUT_MS)
        {
            Log.error("BUSY never asserted in %u ms - panel ignored the refresh" CR,
                      (unsigned)BUSY_ASSERT_TIMEOUT_MS);
            return false;
        }
        delay(5);
    }
    Log.verbose("BUSY asserted after %u ms, refreshing" CR, (unsigned)(millis() - t0));

    while (busy())
    {
        if (millis() - t0 > REFRESH_TIMEOUT_MS)
        {
            Log.error("BUSY still low after %u ms - refresh timed out" CR,
                      (unsigned)(millis() - t0));
            return false;
        }
        delay(50);
    }
    Log.notice("Refresh complete, BUSY released after %u ms" CR, (unsigned)(millis() - t0));
    return true;
}

bool el133_refresh()
{
    static const uint8_t zero = 0x00;

    send_command(CS_BOTH, 0x04); /* PON */
    delay(300);

    send_command(CS_BOTH, 0x12, &zero, 1); /* DRF */
    const bool ok = wait_for_refresh();

    send_command(CS_BOTH, 0x02, &zero, 1); /* POF */
    delay(300);

    return ok;
}
