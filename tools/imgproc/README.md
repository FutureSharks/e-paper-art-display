# `imgproc`: Image Processor

The image processing in here is a translation of [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize) by Claude Opus into go; only `main.go`, which scales, crops and packs the frames, is new. This is because I wanted a standalone CLI application without the JavaScript/TypeScript/UI bloat.

See [NOTICE](NOTICE) and [LICENSE-APACHE-2.0](LICENSE-APACHE-2.0).

## How it works

This tool converts source images into the exact byte stream the firmware pushes over SPI, so the ESP32 does no decoding and needs no framebuffer.

Run it from the repository root; the default input and output paths are relative to it.

```sh
go run ./tools/imgproc                    # artwork/*.jpg -> artwork/processed/
go run ./tools/imgproc -preset restore    # faded scans and paintings
go run ./tools/imgproc -preset posterscan # warm paper, strong flat colour
go run ./tools/imgproc -list-presets      # also -list-palettes, -list-kernels
```

Alongside each source image it writes `<name>.preview.png`: the exact cropped
1200x1600 frame the panel will show, in the palette's calibrated colours, so you
can see what the panel will make of it before flashing anything. Previews are
build output and are gitignored. `-preview-source full` instead dithers the whole
uncropped image at its own resolution, which is easier to flip against the
original but dithers at a finer pitch than the panel has.

Previews are lossless paletted PNGs rather than JPEGs: a six-colour dither is
the worst case for JPEG, whose chroma subsampling and ringing blur exactly the
dot pattern you are trying to judge. With only six colours the PNG is stored at
4 bits per pixel, so it is also small.

The `.bin` files are always the cropped panel frame, 960,000 bytes each.

Settings that affect the packed output are recorded in `manifest.txt`, so
changing a flag repacks everything; otherwise only images newer than their
output are redone.

Deleting a source image deletes its `.bin` from the output directory on the
next run, so it drops out of the manifest and the device's rotation. Only `.bin`
files are removed; anything else in the output directory is left alone.

`-random-prefix` (off by default) names each frame `xxxx_name.bin`, where `xxxx`
is four random lowercase letters/digits. The manifest is sorted by name, so the
device, which steps through it in order, shows the images in a shuffled order.
Prefixes are kept between runs so packing stays incremental; `-force` re-rolls
them for a new order. Toggling the flag renames existing frames rather than
repacking them.

### The error screen

The image shown when something goes wrong - no WiFi, a failed download, a flat
battery - is built into the firmware as `src/error_image.h`, because by then the
network is usually what has failed. To replace it, pack one image with
`-error-image` and rebuild the firmware:

```sh
go run ./tools/imgproc -error-image path/to/image.jpg -preset vivid
```

It takes the same tone and dither flags as a normal run, writes a zlib-compressed
frame to `src/error_image.h` (`-error-header` to change that), and leaves
`artwork/processed/` alone. The current one was packed with `-preset vivid`; its
source image is not in the repository.

### Why the tone presets matter

E-paper is reflective and has perhaps a third of sRGB's brightness range, so
feeding it a photograph straight from a phone wastes most of what it can do:
shadows crush to black and highlights clip to the white ink. The presets
redistribute the image into the range the panel actually has - most importantly
by compressing LAB lightness into the palette's real endpoints - before any ink
is chosen. On a typical image that cuts the pixels lying outside the palette's
reach from ~19% to ~14%, and the error the dither has to throw away with them.

The tone pipeline, the calibrated palettes and the dither kernels are ported
from [paperlesspaper/epdoptimize](https://github.com/paperlesspaper/epdoptimize),
Copyright 2025 Robert Guehne, a browser library licensed under Apache-2.0, and
adapted to run offline in Go. Its "clarity" stage, its level compression, its
fast preview paths and its automatic preset recommender are not ported.

`palette.go`, `tone.go`, `dither.go` and `preset.go` are therefore Apache-2.0
rather than MIT like the rest of this repository. `NOTICE` records
what was taken and how it was changed, and `LICENSE-APACHE-2.0`
is a copy of the licence.

Each palette entry carries two colours: the calibrated one, which is what the
ink actually looks like once it has settled, and the device primary the
controller is sent. Dithering against the measured colour and only then emitting
the device code is what stops the output looking like a 1990s GIF. `-saturation`
blends between the two if you want punch over accuracy.
