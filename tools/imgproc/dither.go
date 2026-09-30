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
// to Go. Error diffusion was rewritten around a three-row ring buffer, and a
// "weighted" matching mode was added that has no upstream equivalent. Only
// the error-diffusion kernels were ported.
// See NOTICE in this directory for the full list of changes.

// Palette matching and error-diffusion dithering, ported from paperlesspaper's
// epdoptimize (Apache-2.0).
//
// Six colours only look like a photograph because the error from each pixel's
// rounding is pushed into its neighbours, so a region averages out to a colour
// the panel does not have. Which neighbours, and in what proportion, is the
// diffusion kernel; how "nearest" is measured is the colour matching mode.
package main

import (
	"fmt"
	"math"
	"sort"
)

type kernelTap struct {
	dx, dy int
	factor float64
}

// The kernels epdoptimize offers, transcribed from its diffusion-maps.ts. No
// tap has dy > 2, which is what lets the error buffer below be three rows.
var diffusionKernels = map[string][]kernelTap{
	"floydSteinberg": {
		{1, 0, 7.0 / 16}, {-1, 1, 3.0 / 16}, {0, 1, 5.0 / 16}, {1, 1, 1.0 / 16},
	},
	"falseFloydSteinberg": {
		{1, 0, 3.0 / 8}, {0, 1, 3.0 / 8}, {1, 1, 2.0 / 8},
	},
	"atkinson": {
		{1, 0, 1.0 / 8}, {2, 0, 1.0 / 8},
		{-1, 1, 1.0 / 8}, {0, 1, 1.0 / 8}, {1, 1, 1.0 / 8},
		{0, 2, 1.0 / 8},
	},
	"jarvis": {
		{1, 0, 7.0 / 48}, {2, 0, 5.0 / 48},
		{-2, 1, 3.0 / 48}, {-1, 1, 5.0 / 48}, {0, 1, 7.0 / 48}, {1, 1, 5.0 / 48}, {2, 1, 3.0 / 48},
		{-2, 2, 1.0 / 48}, {-1, 2, 3.0 / 48}, {0, 2, 4.0 / 48}, {1, 2, 3.0 / 48}, {2, 2, 1.0 / 48},
	},
	"stucki": {
		{1, 0, 8.0 / 42}, {2, 0, 4.0 / 42},
		{-2, 1, 2.0 / 42}, {-1, 1, 4.0 / 42}, {0, 1, 8.0 / 42}, {1, 1, 4.0 / 42}, {2, 1, 2.0 / 42},
		{-2, 2, 1.0 / 42}, {-1, 2, 2.0 / 42}, {0, 2, 4.0 / 42}, {1, 2, 2.0 / 42}, {2, 2, 1.0 / 42},
	},
	"burkes": {
		{1, 0, 8.0 / 32}, {2, 0, 4.0 / 32},
		{-2, 1, 2.0 / 32}, {-1, 1, 4.0 / 32}, {0, 1, 8.0 / 32}, {1, 1, 4.0 / 32}, {2, 1, 2.0 / 32},
	},
	"sierra3": {
		{1, 0, 5.0 / 32}, {2, 0, 3.0 / 32},
		{-2, 1, 2.0 / 32}, {-1, 1, 4.0 / 32}, {0, 1, 5.0 / 32}, {1, 1, 4.0 / 32}, {2, 1, 2.0 / 32},
		{-1, 2, 2.0 / 32}, {0, 2, 3.0 / 32}, {1, 2, 2.0 / 32},
	},
	"sierra2": {
		{1, 0, 4.0 / 16}, {2, 0, 3.0 / 16},
		{-2, 1, 1.0 / 16}, {-1, 1, 2.0 / 16}, {0, 1, 3.0 / 16}, {1, 1, 2.0 / 16}, {2, 1, 1.0 / 16},
	},
	"sierra2-4a": {
		{1, 0, 2.0 / 4}, {-1, 1, 1.0 / 4}, {0, 1, 1.0 / 4},
	},
	"fan": {
		{1, 0, 7.0 / 16}, {-2, 1, 1.0 / 16}, {-1, 1, 3.0 / 16}, {0, 1, 5.0 / 16},
	},
	"shiauFan": {
		{1, 0, 4.0 / 8}, {-2, 1, 1.0 / 8}, {-1, 1, 1.0 / 8}, {0, 1, 2.0 / 8},
	},
	"shiauFan2": {
		{1, 0, 7.0 / 14}, {-3, 1, 1.0 / 14}, {-2, 1, 1.0 / 14}, {-1, 1, 2.0 / 14}, {0, 1, 3.0 / 14},
	},
}

func kernelNames() []string {
	names := make([]string, 0, len(diffusionKernels))
	for name := range diffusionKernels {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func lookupKernel(name string) ([]kernelTap, error) {
	if k, ok := diffusionKernels[name]; ok {
		return k, nil
	}
	return nil, fmt.Errorf("unknown dither kernel %q (try -list-kernels)", name)
}

// Colour matching modes.
const (
	// matchRGB is plain Euclidean distance in sRGB - what epdoptimize's
	// presets are tuned against.
	matchRGB = "rgb"
	// matchLAB is CIE76 in LAB, which respects how the eye weights lightness
	// against hue. Better for near-monochrome work, slower.
	matchLAB = "lab"
	// matchChroma is RGB plus a penalty for draining a colourful pixel to a
	// neutral ink, and for landing on the wrong hue.
	matchChroma = "chroma"
	// matchWeighted is this tool's pre-port behaviour: squared RGB distance
	// weighted for perceived brightness.
	matchWeighted = "weighted"
)

func matchModeNames() []string {
	return []string{matchRGB, matchLAB, matchChroma, matchWeighted}
}

func validMatchMode(mode string) bool {
	for _, m := range matchModeNames() {
		if m == mode {
			return true
		}
	}
	return false
}

// matchPalette is a palette with the per-entry values the matcher needs
// precomputed, since it is consulted once per pixel.
type matchPalette struct {
	mode        string
	colors      []RGB
	labs        []LAB
	saturations []float64
	hues        []float64
}

func newMatchPalette(colors []RGB, mode string) *matchPalette {
	p := &matchPalette{
		mode:        mode,
		colors:      colors,
		labs:        make([]LAB, len(colors)),
		saturations: make([]float64, len(colors)),
		hues:        make([]float64, len(colors)),
	}
	for i, c := range colors {
		p.labs[i] = rgbToLab(c[0], c[1], c[2])
		p.saturations[i] = saturationOf(c[0], c[1], c[2])
		p.hues[i] = hueOf(c[0], c[1], c[2])
	}
	return p
}

// closest returns the index of the nearest palette entry.
func (p *matchPalette) closest(r, g, b uint8) int {
	switch p.mode {
	case matchLAB:
		pixel := rgbToLab(r, g, b)
		best, bestDist := 0, math.MaxFloat64
		for i := range p.colors {
			if d := deltaE(p.labs[i], pixel); d < bestDist {
				best, bestDist = i, d
			}
		}
		return best

	case matchWeighted:
		best, bestDist := 0, math.MaxFloat64
		for i, c := range p.colors {
			dr := float64(r) - float64(c[0])
			dg := float64(g) - float64(c[1])
			db := float64(b) - float64(c[2])
			d := 0.299*dr*dr + 0.587*dg*dg + 0.114*db*db
			if d < bestDist {
				best, bestDist = i, d
			}
		}
		return best

	case matchChroma:
		pixelSaturation := saturationOf(r, g, b)
		haveHue := pixelSaturation >= 0.12
		pixelHue := 0.0
		if haveHue {
			pixelHue = hueOf(r, g, b)
		}

		best, bestDist := 0, math.MaxFloat64
		for i, c := range p.colors {
			d := euclideanRGB(r, g, b, c)

			// Penalise spending a colourful pixel on a neutral ink.
			if pixelSaturation >= 0.12 && p.saturations[i] <= 0.12 {
				d += math.Min(330, pixelSaturation*1300)
			}
			// And penalise coloured inks by how far their hue is off.
			if haveHue && p.saturations[i] > 0.12 {
				d += hueDistance(pixelHue, p.hues[i]) * 3
			}

			if d < bestDist {
				best, bestDist = i, d
			}
		}
		return best

	default: // matchRGB
		best, bestDist := 0, math.MaxFloat64
		for i, c := range p.colors {
			if d := euclideanRGB(r, g, b, c); d < bestDist {
				best, bestDist = i, d
			}
		}
		return best
	}
}

func euclideanRGB(r, g, b uint8, c RGB) float64 {
	dr := float64(r) - float64(c[0])
	dg := float64(g) - float64(c[1])
	db := float64(b) - float64(c[2])
	return math.Sqrt(dr*dr + dg*dg + db*db)
}

// errorDiffuse reduces img to palette indices, spreading each pixel's rounding
// error into its not-yet-visited neighbours.
//
// Error is accumulated in a three-row ring buffer rather than a full-image one:
// no kernel reaches further than two rows down, and a full buffer would be
// hundreds of megabytes on a 12-megapixel original.
//
// Serpentine scanning reverses every other row, which breaks up the diagonal
// worming that a fixed left-to-right scan leaves in flat gradients.
func errorDiffuse(img *Image, pal *matchPalette, kernel []kernelTap, serpentine bool) []uint8 {
	w, h := img.W, img.H
	indices := make([]uint8, w*h)

	var errBuf [3][]float64
	for i := range errBuf {
		errBuf[i] = make([]float64, w*3)
	}

	for y := 0; y < h; y++ {
		cur := errBuf[y%3]
		forward := !serpentine || y%2 == 0
		xStart, xEnd, xStep := 0, w, 1
		if !forward {
			xStart, xEnd, xStep = w-1, -1, -1
		}

		for x := xStart; x != xEnd; x += xStep {
			p := (y*w + x) * 3
			e := x * 3

			// Clamping the accumulated value to a byte before matching is what
			// epdoptimize does - it works on a canvas - and it also keeps a
			// blown highlight from carrying error across the whole image.
			r := clampByte(float64(img.Pix[p]) + cur[e])
			g := clampByte(float64(img.Pix[p+1]) + cur[e+1])
			b := clampByte(float64(img.Pix[p+2]) + cur[e+2])

			best := pal.closest(r, g, b)
			indices[y*w+x] = uint8(best)

			chosen := pal.colors[best]
			er := float64(r) - float64(chosen[0])
			eg := float64(g) - float64(chosen[1])
			eb := float64(b) - float64(chosen[2])

			for _, t := range kernel {
				nx := x + t.dx
				if !forward {
					nx = x - t.dx
				}
				ny := y + t.dy
				if nx < 0 || nx >= w || ny >= h {
					continue
				}
				dst := errBuf[ny%3]
				j := nx * 3
				dst[j] += er * t.factor
				dst[j+1] += eg * t.factor
				dst[j+2] += eb * t.factor
			}
		}

		// This row is done; clear it so it can serve as row y+3's buffer.
		for i := range cur {
			cur[i] = 0
		}
	}
	return indices
}

// render paints palette indices back into an image, for previewing.
func render(indices []uint8, w, h int, colors []RGB) *Image {
	out := NewImage(w, h)
	for i, idx := range indices {
		c := colors[idx]
		out.Pix[i*3], out.Pix[i*3+1], out.Pix[i*3+2] = c[0], c[1], c[2]
	}
	return out
}
