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
// to Go, operating on an RGB byte slice rather than a canvas ImageData. The
// clarity stage, the fast dynamic-range path and level compression were not
// ported.
// See NOTICE in this directory for the full list of changes.

// The tone pipeline, ported from paperlesspaper's epdoptimize (Apache-2.0).
//
// Source images are prepared for emissive screens. E-paper is reflective, has
// perhaps a third of the brightness range, and cannot go anywhere near sRGB's
// gamut, so throwing a photograph straight at a nearest-colour search wastes
// most of what the panel can do: shadows crush to black and highlights clip to
// the white ink. These stages redistribute the image into the range the panel
// actually has, before any colour is chosen.
//
// Stage order matches epdoptimize's applyImageProcessing:
//
//	paper normalisation -> tone mapping -> dynamic range compression
//
// Three parts of that library are deliberately not ported: "clarity" (local
// contrast, marginal for photographs), the "fast" dynamic-range path, which
// exists only to keep a browser preview responsive, and level compression,
// which none of the presets use.
package main

import "math"

// Image is a byte-per-channel RGB buffer. epdoptimize operates on canvas
// ImageData, which is bytes, and its presets are tuned against the rounding
// that implies - so this stays in bytes rather than moving to floats.
type Image struct {
	W, H int
	Pix  []uint8 // 3 bytes per pixel
}

func NewImage(w, h int) *Image {
	return &Image{W: w, H: h, Pix: make([]uint8, w*h*3)}
}

// f is shorthand for an optional float in the option structs below, where the
// zero value and "unset" have to be told apart.
func f(v float64) *float64 { return &v }

func fv(p *float64, def float64) float64 {
	if p == nil {
		return def
	}
	return *p
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

type PaperNormalization struct {
	Mode                string // "off" | "warmPaper"
	Strength            *float64
	MinLuma             *float64
	SaturationThreshold *float64
	WarmBiasThreshold   *float64
	BlackAnchor         *float64
	PreserveRed         *float64
	PaperWhite          *RGB
}

type ToneMapping struct {
	Mode string // "off" | "contrast" | "scurve"
	// Exposure is in stops: 0 is neutral, 1 doubles brightness.
	Exposure float64
	// Saturation and Contrast are offsets: 0 is neutral, 0.5 means 1.5x.
	Saturation        float64
	Contrast          float64
	Strength          *float64
	ShadowBoost       float64
	HighlightCompress *float64
	Midpoint          *float64
}

type DynamicRange struct {
	Mode           string // "off" | "display" | "auto"
	Strength       *float64
	Black          *RGB
	White          *RGB
	LowPercentile  *float64
	HighPercentile *float64
}

type ImageProcessing struct {
	PaperNormalization *PaperNormalization
	ToneMapping        *ToneMapping
	DynamicRange       *DynamicRange
}

// applyImageProcessing runs every configured stage in order. palette supplies
// the black and white endpoints that dynamic range compression compresses
// into, and should be the calibrated colours being dithered against.
func applyImageProcessing(img *Image, o ImageProcessing, palette []RGB) {
	applyPaperNormalization(img, o.PaperNormalization)
	applyToneMapping(img, o.ToneMapping)
	applyDynamicRange(img, o.DynamicRange, palette)
}

// ---------------------------------------------------------------------------
// Paper normalisation
// ---------------------------------------------------------------------------

func isRedInk(r, g, b uint8, saturation float64) bool {
	return saturation >= 0.34 &&
		int(r) >= int(g)+24 &&
		int(r) >= int(b)+28
}

// applyPaperNormalization neutralises the warm cast of aged paper, pulls near
// neutral darks towards true black, and leaves saturated reds alone - printed
// reds are the first thing to go muddy if you white-balance a poster scan.
func applyPaperNormalization(img *Image, o *PaperNormalization) {
	if o == nil || o.Mode == "off" || o.Mode == "" {
		return
	}
	strength := clamp(fv(o.Strength, 1), 0, 1)
	if strength == 0 {
		return
	}

	minLuma := fv(o.MinLuma, 86)
	saturationThreshold := fv(o.SaturationThreshold, 0.44)
	warmBiasThreshold := fv(o.WarmBiasThreshold, 8)
	blackAnchor := clamp(fv(o.BlackAnchor, 0.85), 0, 1)
	preserveRed := clamp(fv(o.PreserveRed, 0.75), 0, 1)
	paperWhite := RGB{248, 248, 248}
	if o.PaperWhite != nil {
		paperWhite = *o.PaperWhite
	}

	data := img.Pix
	for i := 0; i < len(data); i += 3 {
		r, g, b := data[i], data[i+1], data[i+2]
		luma := luma709(r, g, b)
		saturation := saturationOf(r, g, b)

		if isRedInk(r, g, b, saturation) {
			redBoost := strength * preserveRed
			data[i] = clampByte(float64(r) + (255-float64(r))*0.08*redBoost)
			data[i+1] = clampByte(float64(g) * (1 - 0.08*redBoost))
			data[i+2] = clampByte(float64(b) * (1 - 0.12*redBoost))
			continue
		}

		darkNeutralMask := normalize(112-luma, 0, 72) *
			normalize(0.42-saturation, 0, 0.32)
		if darkNeutralMask > 0 {
			amount := darkNeutralMask * blackAnchor * strength
			data[i] = clampByte(float64(r) * (1 - 0.72*amount))
			data[i+1] = clampByte(float64(g) * (1 - 0.72*amount))
			data[i+2] = clampByte(float64(b) * (1 - 0.72*amount))
			continue
		}

		warmBias := math.Min(
			float64(r)-float64(b),
			(float64(r)+float64(g))/2-float64(b),
		)
		warmPaperMask := normalize(luma, minLuma, 210) *
			normalize(245-luma, 0, 80) *
			normalize(saturationThreshold-saturation, 0, saturationThreshold) *
			normalize(warmBias, warmBiasThreshold, 34)
		if warmPaperMask <= 0 {
			continue
		}

		amount := warmPaperMask * strength
		targetLuma := math.Min(252,
			luma+(float64(paperWhite[0])-luma)*(0.72+0.2*strength))
		neutral := [3]float64{
			targetLuma + (float64(paperWhite[0])-248)*0.4,
			targetLuma + (float64(paperWhite[1])-248)*0.4,
			targetLuma + (float64(paperWhite[2])-248)*0.4,
		}

		data[i] = clampByte(float64(r) + (neutral[0]-float64(r))*amount)
		data[i+1] = clampByte(float64(g) + (neutral[1]-float64(g))*amount)
		data[i+2] = clampByte(float64(b) + (neutral[2]-float64(b))*amount)
	}
}

// ---------------------------------------------------------------------------
// Tone mapping
// ---------------------------------------------------------------------------

const shadowToneResponse = 1.5

func exposureToMultiplier(stops float64) float64 { return math.Pow(2, stops) }

func linearToMultiplier(adj float64) float64 { return math.Max(0, adj+1) }

func contrastToMultiplier(adj float64) float64 {
	if adj < 0 {
		return math.Max(0.5, 1+adj*0.5)
	}
	return adj + 1
}

// buildScurveLookup bends shadows and highlights around a midpoint. Negative
// highlightBoost - which is what every preset uses - raises the highlight
// exponent above 1 and so compresses the top end into the white ink's reach.
func buildScurveLookup(strength, shadowBoost, highlightBoost, midpoint float64) *[256]uint8 {
	mid := clamp(midpoint, 0.01, 0.99)
	shadowExponent := clamp(1-strength*shadowBoost*shadowToneResponse, 0.15, 3)
	highlightExponent := clamp(1-strength*highlightBoost, 0.15, 3)

	var lookup [256]uint8
	for v := 0; v < 256; v++ {
		n := float64(v) / 255
		var result float64
		if n <= mid {
			result = math.Pow(n/mid, shadowExponent) * mid
		} else {
			result = mid + math.Pow((n-mid)/(1-mid), highlightExponent)*(1-mid)
		}
		lookup[v] = clampByte(result * 255)
	}
	return &lookup
}

// applyToneMapping applies exposure, saturation and either a contrast scale or
// an S-curve. Everything except saturation is a per-channel function of the
// input byte, so it collapses into lookup tables.
func applyToneMapping(img *Image, o *ToneMapping) {
	if o == nil {
		return
	}

	mode := o.Mode
	exposure := exposureToMultiplier(o.Exposure)
	saturation := linearToMultiplier(o.Saturation)
	contrast := contrastToMultiplier(o.Contrast)

	// An unset mode behaves as an S-curve; "contrast" gets the linear scale.
	defaultStrength := 0.0
	if mode == "scurve" {
		defaultStrength = 0.9
	}
	var scurve *[256]uint8
	if (mode == "" || mode == "scurve") && fv(o.Strength, defaultStrength) != 0 {
		scurve = buildScurveLookup(
			fv(o.Strength, defaultStrength),
			o.ShadowBoost,
			fv(o.HighlightCompress, -1.5),
			fv(o.Midpoint, 0.5),
		)
	}

	var exposureLookup, toneLookup [256]uint8
	for v := 0; v < 256; v++ {
		exposureLookup[v] = clampByte(float64(v) * exposure)
		tone := v
		if mode != "off" {
			if mode == "" || mode == "contrast" {
				tone = int(clampByte((float64(tone)-128)*contrast + 128))
			}
			if scurve != nil {
				tone = int(scurve[tone])
			}
		}
		toneLookup[v] = uint8(tone)
	}

	data := img.Pix
	for i := 0; i < len(data); i += 3 {
		var r, g, b uint8

		if saturation == 1 {
			r = toneLookup[exposureLookup[data[i]]]
			g = toneLookup[exposureLookup[data[i+1]]]
			b = toneLookup[exposureLookup[data[i+2]]]
		} else {
			// HSL saturation scale, kept in floats between exposure and tone.
			r0 := float64(exposureLookup[data[i]]) / 255
			g0 := float64(exposureLookup[data[i+1]]) / 255
			b0 := float64(exposureLookup[data[i+2]]) / 255

			max := math.Max(r0, math.Max(g0, b0))
			min := math.Min(r0, math.Min(g0, b0))
			lightness := (max + min) / 2
			rf, gf, bf := r0, g0, b0

			if max != min {
				delta := max - min
				var sat float64
				if lightness > 0.5 {
					sat = delta / (2 - max - min)
				} else {
					sat = delta / math.Max(max+min, 0.000001)
				}

				var hue float64
				switch max {
				case r0:
					shift := 0.0
					if g0 < b0 {
						shift = 6
					}
					hue = ((g0-b0)/delta + shift) / 6
				case g0:
					hue = ((b0-r0)/delta + 2) / 6
				default:
					hue = ((r0-g0)/delta + 4) / 6
				}

				newSat := clamp(sat*saturation, 0, 1)
				c := (1 - math.Abs(2*lightness-1)) * newSat
				x := c * (1 - math.Abs(math.Mod(hue*6, 2)-1))
				m := lightness - c/2

				switch int(math.Floor(hue * 6)) {
				case 0:
					rf, gf, bf = c+m, x+m, m
				case 1:
					rf, gf, bf = x+m, c+m, m
				case 2:
					rf, gf, bf = m, c+m, x+m
				case 3:
					rf, gf, bf = m, x+m, c+m
				case 4:
					rf, gf, bf = x+m, m, c+m
				default:
					rf, gf, bf = c+m, m, x+m
				}
			}

			r = toneLookup[clampByte(rf*255)]
			g = toneLookup[clampByte(gf*255)]
			b = toneLookup[clampByte(bf*255)]
		}

		data[i], data[i+1], data[i+2] = r, g, b
	}
}

// ---------------------------------------------------------------------------
// Dynamic range compression
// ---------------------------------------------------------------------------

const (
	lightnessHistogramScale = 100
	lightnessHistogramBins  = 100*lightnessHistogramScale + 1
)

func percentileFromHistogram(histogram []uint32, count int, p float64) float64 {
	if count <= 0 {
		return 0
	}
	target := int(clamp(math.Round(float64(count-1)*p), 0, float64(count-1)))
	seen := 0
	for i, n := range histogram {
		seen += int(n)
		if seen > target {
			return float64(i) / lightnessHistogramScale
		}
	}
	return 100
}

// getDynamicRangeChromaProtection backs the compression off over saturated
// pixels, where dragging L about visibly shifts the hue.
func getDynamicRangeChromaProtection(r, g, b uint8) float64 {
	return smoothstep(0.18, 0.68, saturationOf(r, g, b)) * 0.85
}

const chromaGuardSteps = 5

func isProtectedChromaFit(sourceLuma float64, result RGB, sourceSaturation float64) bool {
	if sourceSaturation < 0.16 {
		return true
	}
	resultSaturation := saturationOf(result[0], result[1], result[2])
	if resultSaturation >= math.Max(0.12, sourceSaturation*0.72) {
		return true
	}
	return luma709(result[0], result[1], result[2]) <= sourceLuma+4
}

// labToRgbWithChromaGuard moves a pixel's lightness towards targetL, but backs
// off - by bisection - if brightening it would wash its colour out. Raising L
// in LAB on an already-saturated colour runs it out of the sRGB gamut, and the
// clamp that follows desaturates it; this finds how far it can go first.
func labToRgbWithChromaGuard(source RGB, sourceL, a, b, targetL, amount float64) RGB {
	sourceSaturation := saturationOf(source[0], source[1], source[2])
	sourceLuma := luma709(source[0], source[1], source[2])

	toRgb := func(fit float64) RGB {
		return labToRgb(sourceL+(targetL-sourceL)*fit, a, b)
	}

	result := toRgb(amount)
	if targetL <= sourceL || isProtectedChromaFit(sourceLuma, result, sourceSaturation) {
		return result
	}

	low, high := 0.0, amount
	protected := source
	for step := 0; step < chromaGuardSteps; step++ {
		mid := (low + high) / 2
		candidate := toRgb(mid)
		if isProtectedChromaFit(sourceLuma, candidate, sourceSaturation) {
			low = mid
			protected = candidate
		} else {
			high = mid
		}
	}
	return protected
}

// levelRGB resolves an optional endpoint colour to a concrete one.
func levelRGB(v *RGB, fallback uint8) RGB {
	if v == nil {
		return RGB{fallback, fallback, fallback}
	}
	return *v
}

// getPaletteEndpoints finds the darkest and lightest inks available, which is
// the range everything has to fit into.
func getPaletteEndpoints(palette []RGB, black, white *RGB) (RGB, RGB) {
	blackOut := levelRGB(black, 0)
	whiteOut := levelRGB(white, 255)
	if (black != nil && white != nil) || len(palette) == 0 {
		return blackOut, whiteOut
	}

	darkest, lightest := palette[0], palette[0]
	for _, c := range palette {
		if luma709(c[0], c[1], c[2]) < luma709(darkest[0], darkest[1], darkest[2]) {
			darkest = c
		}
		if luma709(c[0], c[1], c[2]) > luma709(lightest[0], lightest[1], lightest[2]) {
			lightest = c
		}
	}
	if black == nil {
		blackOut = darkest
	}
	if white == nil {
		whiteOut = lightest
	}
	return blackOut, whiteOut
}

// applyDynamicRange remaps LAB lightness into the panel's range. In "display"
// mode the source is assumed to span the full 0..100; in "auto" mode the real
// span is measured from a histogram first, which rescues faded scans.
func applyDynamicRange(img *Image, o *DynamicRange, palette []RGB) {
	if o == nil || o.Mode == "off" {
		return
	}
	mode := o.Mode
	if mode == "" {
		mode = "display"
	}
	strength := clamp(fv(o.Strength, 1), 0, 1)
	if strength == 0 {
		return
	}

	black, white := getPaletteEndpoints(palette, o.Black, o.White)
	blackL := rgbToLab(black[0], black[1], black[2])[0]
	whiteL := rgbToLab(white[0], white[1], white[2])[0]
	targetRange := whiteL - blackL
	if targetRange <= 0 {
		return
	}

	data := img.Pix
	sourceBlackL, sourceWhiteL := 0.0, 100.0

	if mode == "auto" {
		histogram := make([]uint32, lightnessHistogramBins)
		count := 0
		for i := 0; i < len(data); i += 3 {
			l := rgbToLabLightness(data[i], data[i+1], data[i+2])
			bin := int(clamp(math.Round(l*lightnessHistogramScale), 0,
				float64(lightnessHistogramBins-1)))
			histogram[bin]++
			count++
		}
		sourceBlackL = percentileFromHistogram(histogram, count, fv(o.LowPercentile, 0.01))
		sourceWhiteL = percentileFromHistogram(histogram, count, fv(o.HighPercentile, 0.99))
	}

	sourceRange := sourceWhiteL - sourceBlackL
	if sourceRange <= 0.0001 {
		return
	}

	for i := 0; i < len(data); i += 3 {
		src := RGB{data[i], data[i+1], data[i+2]}
		lab := rgbToLab(src[0], src[1], src[2])
		normalizedL := clamp((lab[0]-sourceBlackL)/sourceRange, 0, 1)
		compressedL := blackL + normalizedL*targetRange
		protection := getDynamicRangeChromaProtection(src[0], src[1], src[2])

		out := labToRgbWithChromaGuard(src, lab[0], lab[1], lab[2],
			compressedL, strength*(1-protection))
		data[i], data[i+1], data[i+2] = out[0], out[1], out[2]
	}
}
