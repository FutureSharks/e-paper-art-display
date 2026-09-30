/*
 * el133.h: driver for the Pimoroni Inky Impression 13.3" (EL133UF1, Spectra 6).
 *
 * Written from Pimoroni's inky_el133uf1.py (MIT), the reference driver for this
 * panel. Its two quirks drive most of the design:
 *
 *   - Each command needs ~300 ms between D/C going low and the opcode being
 *     clocked, or the controller silently drops it. Init therefore takes ~7 s.
 *   - BUSY is active low and open-drain. It needs a pull-up, and a refresh is
 *     only over once the line has asserted and then released.
 *
 * Frame format
 * ------------
 * The panel has two controllers, each driving half of a 1200 x 1600 portrait
 * frame: CS_M takes columns 0..599, CS_S takes 600..1199. Each half is 1600
 * rows of 300 bytes, two pixels per byte, high nibble first. A frame is
 * streamed as the CS_M half followed by the CS_S half; tools/imgproc writes
 * exactly that layout, so nothing is rotated or buffered on the device.
 *
 * Usage
 * -----
 *     el133_begin();                       once, at startup
 *     el133_init_panel();                  reset + init, ~7 s
 *     el133_stream_begin(0);  el133_stream_write(...) ...  el133_stream_end();
 *     el133_stream_begin(1);  el133_stream_write(...) ...  el133_stream_end();
 *     el133_refresh();                     ~20 s, blocking
 *
 * Nothing reaches the glass until el133_refresh(), so an abandoned stream
 * leaves the previous image untouched: skip the refresh and try again.
 *
 * Pins are set in el133_pins.h.
 */
#pragma once

#include <Arduino.h>

/* Boost converter drive, written to BTST_P and BTST_N during init. The boost
 * converters generate the panel's high-voltage rails and draw hardest at power
 * on - the moment a marginal supply browns out.
 *
 *   1 (default)  D8 18  softer, as used by Soldered's Inkplate driver
 *   0            E0 20  as used by Pimoroni's driver
 *
 * If the softer setting ever gives faded or uneven colour, set this to 0. */
#ifndef EL133_SOFT_BOOST
#define EL133_SOFT_BOOST 1
#endif

/* Portrait geometry, as the frame hangs. */
#define EL133_PORTRAIT_W 1200
#define EL133_PORTRAIT_H 1600

/* Bytes in one controller's half, and in a whole frame. */
#define EL133_HALF_BYTES   ((size_t)(EL133_PORTRAIT_W / 4) * EL133_PORTRAIT_H) /* 480000 */
#define EL133_STREAM_BYTES (EL133_HALF_BYTES * 2)                              /* 960000 */

/* The panel's colour codes. 0x4 is not a colour. */
enum
{
    EL133_BLACK = 0x0,
    EL133_WHITE = 0x1,
    EL133_YELLOW = 0x2,
    EL133_RED = 0x3,
    EL133_BLUE = 0x5,
    EL133_GREEN = 0x6,
};

/* Configure SPI and the control pins. */
void el133_begin();

/* Hardware reset, then the init sequence. */
void el133_init_panel();

/* Open a transfer to one controller: 0 = CS_M, 1 = CS_S. Chip-select stays
 * asserted until el133_stream_end(), but the SPI bus is released between
 * writes, so it is safe to block on network I/O mid-frame. */
void el133_stream_begin(int half);

/* Push frame bytes; any chunk size. */
void el133_stream_write(const uint8_t *data, size_t len);

/* Close the transfer. */
void el133_stream_end();

/* Power on, refresh, wait for BUSY, power off. Returns false if the panel never
 * asserted BUSY - it ignored the refresh - or never released it. */
bool el133_refresh();

/* ---- Overlay ----------------------------------------------------------
 *
 * A rectangle painted over a frame as it streams through - a status line, say -
 * without buffering the frame. Coordinates are portrait: x 0..1199 across,
 * y 0..1599 down. `pixels` holds w*h colour codes, row-major, and must stay
 * valid until the stream ends. Applies to el133_stream_write() until cleared. */
struct El133Overlay
{
    int x, y, w, h;
    const uint8_t *pixels;
};

/* Set the overlay (the struct is copied, the pixels are not), or pass nullptr
 * to clear it. Returns false, leaving no overlay, if it is empty or off the
 * panel. */
bool el133_stream_overlay(const El133Overlay *overlay);
