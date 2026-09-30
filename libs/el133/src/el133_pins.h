/* Hardware configuration for the Pimoroni Inky Impression 13.3" (PIM774,
 * EL133UF1 / Spectra 6) driven from an Adafruit Feather ESP32-C6.
 *
 * Names on the left are the Feather's silkscreen labels, which the board
 * variant maps to the GPIOs given in the comments. The HAT column is the
 * physical pin on the Inky's 40-pin Pi connector.
 *
 * Pin choice:
 *   - SCK/MO/MI are the Feather's hardware SPI pins, so the peripheral needs no
 *     rerouting.
 *   - A1 (GPIO4) and A3 (GPIO5) are deliberately unused: they are ESP32-C6
 *     strapping pins, sampled at reset. Our chip-selects and reset idle high
 *     but float while the ESP32 is in reset, and the HAT does not pull them, so
 *     boot behaviour would depend on a floating line.
 *   - Also avoided: GPIO8/GPIO9/GPIO15 (strapping, NeoPixel, LED) and GPIO20
 *     (STEMMA/I2C power switch).
 *   - SDA/SCL are left alone for I2C: the onboard MAX17048 fuel gauge sits on
 *     that bus at 0x36. BUSY takes RX instead.
 *   - RX (GPIO17) is UART0's receive pin, which is only free because the build
 *     sets CDCOnBoot=cdc - see sketch.yaml. With that option off, Serial is
 *     UART0 and this pin carries the console.
 *
 * The panel is dual-COG: CS_M takes portrait columns 0..599, CS_S takes
 * 600..1199.
 *
 * The HAT is powered from 3V3 (header pin 1) and GND; there is no enable pin,
 * EPD_3V3 is tied to 3V3 through a ferrite. Note the Feather's 3V3 regulator
 * has to carry the panel's boost-converter inrush as well as the radio.
 *
 * BUSY is open-drain and needs a pull-up - the internal one works for
 * bring-up, an external 10k is better for a permanent build.
 */
#pragma once

#define EL133_PIN_SCLK SCK  // GPIO21 <- header pin 23
#define EL133_PIN_MISO MISO // GPIO23 <- header pin 21 (unused, panel readback)
#define EL133_PIN_MOSI MOSI // GPIO22 <- header pin 19
#define EL133_PIN_DC   A4   // GPIO3  <- header pin 15
#define EL133_PIN_RST  A2   // GPIO6  <- header pin 13
#define EL133_PIN_BUSY RX   // GPIO17 <- header pin 11 (active low)
#define EL133_PIN_CS_M A0   // GPIO1  <- header pin 37 (BCM26, left half)
#define EL133_PIN_CS_S A5   // GPIO2  <- header pin 36 (BCM16, right half)
