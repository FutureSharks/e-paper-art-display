// SPDX-License-Identifier: Apache-2.0
//
// This file is a Go port of part of epdoptimize:
//   https://github.com/paperlesspaper/epdoptimize
//   Copyright 2025 Robert Guehne
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. A copy is in
// LICENSE-APACHE-2.0 in this directory, or at
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
//
// This file has been modified from the original. Translated from TypeScript
// to Go; palettes reduced to the six EL133UF1 inks and reordered to that
// controller's colour-code order.
// See NOTICE in this directory for the full list of changes.

// Colour palettes and colour-space maths, ported from paperlesspaper's
// epdoptimize (Apache-2.0, https://github.com/paperlesspaper/epdoptimize).
//
// The central idea borrowed from that library is that a palette has two
// colours per entry: `Color` is what the ink actually looks like once it has
// settled on the panel, and `DeviceColor` is the fully saturated primary the
// controller wants to be told about. Dithering against the measured colour and
// only then emitting the device code is what stops e-paper output from looking
// like a 1990s GIF.
//
// All of the maths here works in 8-bit bytes rather than floats, exactly as
// epdoptimize does, so the presets tuned there transfer without re-tuning.
package main

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
)

// RGB is an 8-bit colour.
type RGB [3]uint8

// LAB is CIELAB with L in 0..100 and a/b roughly in -128..127.
type LAB [3]float64

// PaletteEntry names one ink the panel can lay down.
type PaletteEntry struct {
	Name string
	// Color is the calibrated appearance - dither against this.
	Color RGB
	// DeviceColor is the idealised primary the controller is sent.
	DeviceColor RGB
}

// The panel's roles, in the order the EL133UF1 numbers them, and the 4-bit
// codes themselves. Note 0x4 is not a colour. Palettes are looked up by role
// name rather than by position, so the packed stream stays correct no matter
// what order a palette happens to list its entries in.
var (
	panelRoles = [6]string{"black", "white", "yellow", "red", "blue", "green"}
	panelCodes = [6]byte{0x0, 0x1, 0x2, 0x3, 0x5, 0x6}
)

type paletteSpec struct {
	desc string
	// name, calibrated colour, device colour
	entries [][3]string
}

// The Spectra 6 sets are transcribed from epdoptimize's default-palettes.json.
// "pimoroni" is this tool's own earlier pair of reference palettes, kept so its
// colour choices are still available:
//
//	-palette pimoroni -saturation 0.5 -preset none -match weighted
//
// That is not byte-identical to the pre-port output, and should not be: the old
// error diffusion accumulated in unclamped floats, so in any region brighter
// than the white ink the error grew without bound and blew whole areas out to
// flat colour. This one clamps to a byte per pixel, as epdoptimize does.
var paletteSpecs = map[string]paletteSpec{
	"spectra6": {
		desc: "Spectra 6 as measured by paperlesspaper (default)",
		entries: [][3]string{
			{"black", "#1F2226", "#000000"},
			{"white", "#B9C7C9", "#FFFFFF"},
			{"yellow", "#C1BB1E", "#FFFF00"},
			{"red", "#62201E", "#FF0000"},
			{"blue", "#233F8E", "#0000FF"},
			{"green", "#35563A", "#00FF00"},
		},
	},
	"spectra6-legacy": {
		desc: "Spectra 6, paperlesspaper's earlier and lighter measurement",
		entries: [][3]string{
			{"black", "#191E21", "#000000"},
			{"white", "#E8E8E8", "#FFFFFF"},
			{"yellow", "#EFDE44", "#FFFF00"},
			{"red", "#B21318", "#FF0000"},
			{"blue", "#2157BA", "#0000FF"},
			{"green", "#125F20", "#00FF00"},
		},
	},
	"spectra6-boeber": {
		desc: "Spectra 6, Boeber's measurement - brighter and more saturated",
		entries: [][3]string{
			{"black", "#1F2226", "#000000"},
			{"white", "#D6D6D6", "#FFFFFF"},
			{"yellow", "#DBD529", "#FFFF00"},
			{"red", "#EA4843", "#FF0000"},
			{"blue", "#416CE1", "#0000FF"},
			{"green", "#067406", "#00FF00"},
		},
	},
	"spectra6-aitjcize": {
		desc: "Spectra 6 as measured by aitjcize/epaper-image-convert - darkest",
		entries: [][3]string{
			{"black", "#020202", "#000000"},
			{"white", "#BEC8C8", "#FFFFFF"},
			{"yellow", "#CDCA00", "#FFFF00"},
			{"red", "#871300", "#FF0000"},
			{"blue", "#05409E", "#0000FF"},
			{"green", "#27663C", "#00FF00"},
		},
	},
	"spectra6-ideal": {
		desc: "Idealised primaries - no calibration, for comparison",
		entries: [][3]string{
			{"black", "#000000", "#000000"},
			{"white", "#FFFFFF", "#FFFFFF"},
			{"yellow", "#FFFF00", "#FFFF00"},
			{"red", "#FF0000", "#FF0000"},
			{"blue", "#0000FF", "#0000FF"},
			{"green", "#00FF00", "#00FF00"},
		},
	},
	"pimoroni": {
		desc: "Pimoroni's reference pair, as this tool used before the port",
		entries: [][3]string{
			{"black", "#000000", "#000000"},
			{"white", "#A1A4A5", "#FFFFFF"},
			{"yellow", "#D0BE47", "#FFFF00"},
			{"red", "#9C484B", "#FF0000"},
			{"blue", "#3D3B5E", "#0000FF"},
			{"green", "#3A5B46", "#00FF00"},
		},
	},
}

func paletteNames() []string {
	names := make([]string, 0, len(paletteSpecs))
	for name := range paletteSpecs {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// lookupPalette resolves a named palette into the panel's role order.
func lookupPalette(name string) ([6]PaletteEntry, error) {
	var out [6]PaletteEntry

	spec, ok := paletteSpecs[strings.ToLower(name)]
	if !ok {
		return out, fmt.Errorf("unknown palette %q (try -list-palettes)", name)
	}

	byRole := make(map[string][3]string, len(spec.entries))
	for _, e := range spec.entries {
		byRole[e[0]] = e
	}

	for i, role := range panelRoles {
		e, ok := byRole[role]
		if !ok {
			return out, fmt.Errorf("palette %q has no %q entry", name, role)
		}
		color, err := parseHex(e[1])
		if err != nil {
			return out, fmt.Errorf("palette %q, %s: %w", name, role, err)
		}
		device, err := parseHex(e[2])
		if err != nil {
			return out, fmt.Errorf("palette %q, %s: %w", name, role, err)
		}
		out[i] = PaletteEntry{Name: role, Color: color, DeviceColor: device}
	}
	return out, nil
}

// blend mixes each entry's calibrated colour towards its device colour.
// saturation 1 dithers against the measured ink - the faithful choice - while 0
// dithers against the idealised primaries, which is punchier and less accurate.
func blend(entries [6]PaletteEntry, saturation float64) [6]RGB {
	var out [6]RGB
	for i, e := range entries {
		for c := 0; c < 3; c++ {
			v := float64(e.Color[c])*saturation +
				float64(e.DeviceColor[c])*(1-saturation)
			out[i][c] = clampByte(v)
		}
	}
	return out
}

func deviceColors(entries [6]PaletteEntry) [6]RGB {
	var out [6]RGB
	for i, e := range entries {
		out[i] = e.DeviceColor
	}
	return out
}

// parseHex accepts #rgb and #rrggbb, with or without the leading hash.
func parseHex(s string) (RGB, error) {
	var out RGB
	h := strings.TrimPrefix(strings.TrimSpace(s), "#")

	switch len(h) {
	case 3:
		h = string([]byte{h[0], h[0], h[1], h[1], h[2], h[2]})
	case 6:
	default:
		return out, fmt.Errorf("bad hex colour %q", s)
	}

	for i := 0; i < 3; i++ {
		v, err := strconv.ParseUint(h[i*2:i*2+2], 16, 8)
		if err != nil {
			return out, fmt.Errorf("bad hex colour %q", s)
		}
		out[i] = uint8(v)
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// Colour-space maths. Ported byte-for-byte from epdoptimize's processing.ts so
// that its presets behave here as they do in its web UI.
// ---------------------------------------------------------------------------

// srgbToLinear undoes the sRGB transfer function for each possible byte.
var srgbToLinear = func() [256]float64 {
	var t [256]float64
	for v := 0; v < 256; v++ {
		n := float64(v) / 255
		if n > 0.04045 {
			t[v] = math.Pow((n+0.055)/1.055, 2.4)
		} else {
			t[v] = n / 12.92
		}
	}
	return t
}()

func clamp(v, lo, hi float64) float64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func clampByte(v float64) uint8 {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return 0
	}
	return uint8(math.Round(clamp(v, 0, 255)))
}

// luma709 is Rec.709 relative luminance on byte-valued channels.
func luma709(r, g, b uint8) float64 {
	return 0.2126*float64(r) + 0.7152*float64(g) + 0.0722*float64(b)
}

func labForwardPivot(v float64) float64 {
	if v > 0.008856 {
		return math.Cbrt(v)
	}
	return 7.787*v + 16.0/116.0
}

// rgbToLabLightness is the L of rgbToLab without the chroma work, for the
// histogram pass where only lightness matters.
func rgbToLabLightness(r, g, b uint8) float64 {
	y := srgbToLinear[r]*0.2126729 +
		srgbToLinear[g]*0.7151522 +
		srgbToLinear[b]*0.072175
	return 116*labForwardPivot(y) - 16
}

func rgbToLab(r, g, b uint8) LAB {
	rn, gn, bn := srgbToLinear[r], srgbToLinear[g], srgbToLinear[b]

	x := (rn*0.4124564 + gn*0.3575761 + bn*0.1804375) * 100
	y := (rn*0.2126729 + gn*0.7151522 + bn*0.072175) * 100
	z := (rn*0.0193339 + gn*0.119192 + bn*0.9503041) * 100

	xn := labForwardPivot(x / 95.047)
	yn := labForwardPivot(y / 100)
	zn := labForwardPivot(z / 108.883)

	return LAB{116*yn - 16, 500 * (xn - yn), 200 * (yn - zn)}
}

func labToRgb(l, a, bb float64) RGB {
	y := (l + 16) / 116
	x := a/500 + y
	z := y - bb/200

	pivot := func(v float64) float64 {
		if v > 0.206897 {
			return v * v * v
		}
		return (v - 16.0/116.0) / 7.787
	}
	x = pivot(x) * 95.047
	y = pivot(y) * 100
	z = pivot(z) * 108.883

	xn, yn, zn := x/100, y/100, z/100
	r := xn*3.2404542 + yn*-1.5371385 + zn*-0.4985314
	g := xn*-0.969266 + yn*1.8760108 + zn*0.041556
	b := xn*0.0556434 + yn*-0.2040259 + zn*1.0572252

	gamma := func(v float64) float64 {
		if v > 0.0031308 {
			return 1.055*math.Pow(v, 1/2.4) - 0.055
		}
		return 12.92 * v
	}
	return RGB{
		clampByte(gamma(r) * 255),
		clampByte(gamma(g) * 255),
		clampByte(gamma(b) * 255),
	}
}

// deltaE is plain CIE76 - enough to separate six inks, and what epdoptimize
// uses for its "lab" matching mode.
func deltaE(a, b LAB) float64 {
	dl, da, db := a[0]-b[0], a[1]-b[1], a[2]-b[2]
	return math.Sqrt(dl*dl + da*da + db*db)
}

// saturationOf is HSV saturation.
func saturationOf(r, g, b uint8) float64 {
	max := math.Max(float64(r), math.Max(float64(g), float64(b))) / 255
	min := math.Min(float64(r), math.Min(float64(g), float64(b))) / 255
	if max == 0 {
		return 0
	}
	return (max - min) / max
}

// hueOf returns the HSV hue in degrees.
func hueOf(r, g, b uint8) float64 {
	rf, gf, bf := float64(r)/255, float64(g)/255, float64(b)/255
	max := math.Max(rf, math.Max(gf, bf))
	min := math.Min(rf, math.Min(gf, bf))
	delta := max - min
	if delta == 0 {
		return 0
	}

	var hue float64
	switch max {
	case rf:
		hue = 60 * math.Mod((gf-bf)/delta, 6)
	case gf:
		hue = 60 * ((bf-rf)/delta + 2)
	default:
		hue = 60 * ((rf-gf)/delta + 4)
	}
	if hue < 0 {
		hue += 360
	}
	return hue
}

func hueDistance(a, b float64) float64 {
	delta := math.Mod(math.Abs(a-b), 360)
	return math.Min(delta, 360-delta)
}

func normalize(v, min, max float64) float64 {
	return clamp((v-min)/(max-min), 0, 1)
}

func smoothstep(edge0, edge1, v float64) float64 {
	if edge1 <= edge0 {
		if v >= edge1 {
			return 1
		}
		return 0
	}
	x := normalize(v, edge0, edge1)
	return x * x * (3 - 2*x)
}
