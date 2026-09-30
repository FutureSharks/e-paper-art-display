# e-paper-art-display

A battery-powered, e-paper daily art display.
An ESP32 based e-paper art display

An ESP32 microcontroller wakes on a schedule, connects to Wi-Fi, downloads a piece of art and shows it on a 13.3" color e-paper panel. The artwork image is preprocessed locally using [tools/imgproc](tools/imgproc), then pushed to GitHub. Because the panel holds the image with no power consumption, the battery is only used while the image is changing, so battery life can be months or even years with a daily image refresh.

## Quick Start

To build your own:

1. Copy the template: `cp src/secrets_template.h src/secrets.h`
2. Edit `src/secrets.h` with your own values for `WIFI_SSID` and `WIFI_PASSWORD`
3. Install the [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation)
4. Run `arduino-cli compile` from the root of this repo; the provided [sketch.yaml](sketch.yaml) supplies the board and build settings
5. Review [src/config.h](src/config.h) and adjust it as needed:
   1. By default the frame refreshes once a day at 05:00 local time. You can change this; the comments in the file explain how.
   2. If you fork this repo, you will need to change `ART_BASE_URL`
6. To upload, run `arduino-cli upload --port <your port device>`

### Adding artwork

Place your artwork images in `artwork/`, run `imgproc` (`go run ./tools/imgproc`) to process them, then commit the files in `artwork/processed/` and push.

To find artwork, [tools/art-fetcher](tools/art-fetcher) (`go run ./tools/art-fetcher`) downloads public-domain, portrait-orientation paintings from Wikimedia Commons into `tools/art-fetcher/downloads/`. Copy the ones you want into `artwork/`.

## Hardware

An [Adafruit Feather ESP32-C6](https://learn.adafruit.com/adafruit-esp32-c6-feather) controls a [Pimoroni Inky Impression 13.3"](https://www.berrybase.de/en/pimoroni-inky-impression-13-3-epaper-display-fuer-raspberry-pi-spectra-6-1600x1200-6-farben-gpio) display. This display module uses a 1200×1600 (portrait) E Ink Spectra 6 panel (model `EL133UF1`). A full refresh takes about 35 seconds.

The whole build is powered by a single-cell LiPo plugged into the Feather's JST connector. The Feather charges the cell over USB-C and reports its level through its onboard MAX17048 fuel gauge, and its 3V3 regulator supplies the panel as well as the ESP32.

The Inky is actually a Raspberry Pi HAT with a 40-pin header, so the Feather is connected to it via a proto PCB. See [Pin map](#pin-map) below.

### Pin map

Pin map for the Adafruit Feather ESP32-C6. ESP32 pins are defined in [`libs/el133/src/el133_pins.h`](libs/el133/src/el133_pins.h); the HAT header column is the physical pin on the Inky's 40-pin Pi connector.

| Signal              | Pi GPIO pin  | Pi HAT header pin            | Feather pin name | Feather GPIO pin |
|---------------------|--------------|------------------------------|------------------|------------------|
| SCLK                | 11           | 23                           | SCK              | 21               |
| MOSI                | 10           | 19                           | MO               | 22               |
| INKY_CS (CS_M)      | 26           | 37                           | A0               | 1                |
| INKY_CSB_S (CS_S)   | 16           | 36                           | A5               | 2                |
| INKY_D/C            | 22           | 15                           | A4               | 3                |
| INKY_RESET          | 27           | 13                           | A2               | 6                |
| INKY_BUSY           | 17           | 11                           | RX               | 17               |
| 3V3                 | -            | 1, 17                        | 3V3              | -                |
| GND                 | -            | 6, 9, 14, 20, 25, 30, 34, 39 | GND              | -                |

Notes:

- **CS_M drives portrait columns 0–599, CS_S drives 600–1199.** Swapping them
  mirrors the two halves of the image.
- **BUSY is active low and open-drain**, so it needs a pull-up. The driver
  enables the ESP32's internal one; an external 10 kΩ to 3V3 is better for a
  permanent build, since the internal pull-up is ~45 kΩ and weak against noise on
  a jumper.
- **The Inky has no enable pin.** `EPD_3V3` is tied to 3V3 through a ferrite, so
  the panel powers up as soon as the rail does. All the high-voltage rails
  (VGH +28 V, VGL −21 V, VDDP/VDDN/VCOM) are generated on the HAT by boost
  converters the panel gates itself.
- **None of these land on a C6 strapping pin** (those are GPIO4, 5, 8, 9 and 15).
  That rules out `A1` (GPIO4) and `A3` (GPIO5): they are sampled at reset, and
  a chip-select or reset line floats while the ESP32 is held in reset.
- **SDA (GPIO19) and SCL (GPIO18) are kept free for I2C**, where the onboard
  MAX17048 fuel gauge lives at 0x36. BUSY uses RX instead.
- **RX (GPIO17) is only available because the build sets `CDCOnBoot=cdc`.**
  With that option off, `Serial` is UART0 on GPIO16/17, which would both take
  this pin and hide the log, since the board has no USB-UART bridge, only the
  C6's native USB.
- **GPIO20 switches the STEMMA QT / I2C power rail.** If a device on the
  connector does not answer, drive it high before scanning. The onboard fuel
  gauge is normally on the unswitched side of that.

## Credits

- **Claude Opus wrote most of what is in this repo.**
- [myembeddedstuff/EInk_PictureFrame_GoogleDrive](https://github.com/myembeddedstuff/EInk_PictureFrame_GoogleDrive): the idea of pulling images from a public cloud service, although that project seems non-functional.
- [pimoroni/inky](https://github.com/pimoroni/inky): `libs/el133` is written from its `inky_el133uf1.py`, and `imgproc` uses its palettes.
- [dmellok/el133-pico-driver](https://github.com/dmellok/el133-pico-driver): confirmed the 300 ms per-command setup and showed a frame can be streamed without PSRAM.
- [SolderedElectronics/Inkplate-Arduino-library](https://github.com/SolderedElectronics/Inkplate-Arduino-library): its softer `BTST` boost values fixed a battery brownout issue. Only the values are used.
- [Seeed-Studio/Seeed_GFX](https://github.com/Seeed-Studio/Seeed_GFX): the starting point. Its paint path was correct, but its init sequence targets Seeed's own module.
- [FutureSharks/inky-ical](https://github.com/FutureSharks/inky-ical): the firmware layout, the battery module and the Roboto fonts.
- [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize): ported to Go in `tools/imgproc`. See [NOTICE](tools/imgproc/NOTICE).
- [adafruit/Adafruit_MAX1704X](https://github.com/adafruit/Adafruit_MAX1704X): the MAX17048 fuel-gauge driver.
- [thijse/Arduino-Log](https://github.com/thijse/Arduino-Log): logging.
