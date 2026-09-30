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
// to Go; a "none" preset was added and the options each preset sets were
// narrowed to the stages that were ported.
// See NOTICE in this directory for the full list of changes.

// Processing presets, transcribed from paperlesspaper's epdoptimize
// (Apache-2.0). The numbers are tuned against the calibrated palettes in
// palette.go and against byte-domain arithmetic, so they should be moved
// together with those or not at all.
package main

import (
	"fmt"
	"math"
	"sort"
	"strings"
)

// Preset bundles a tone pipeline with the dither settings it was tuned with.
type Preset struct {
	Name        string
	Description string

	PaperNormalization *PaperNormalization
	ToneMapping        *ToneMapping
	DynamicRange       *DynamicRange

	ColorMatching string
	Kernel        string
}

func (p Preset) processing() ImageProcessing {
	return ImageProcessing{
		PaperNormalization: p.PaperNormalization,
		ToneMapping:        p.ToneMapping,
		DynamicRange:       p.DynamicRange,
	}
}

// epdoptimize stores exposure in stops and the other adjustments as offsets,
// but its preset table is written in terms of plain multipliers. These two
// helpers keep that readable here too, rounded to three places as it does.
func expFromMul(multiplier float64) float64 { return round3(math.Log2(multiplier)) }

func linFromMul(multiplier float64) float64 { return round3(multiplier - 1) }

func round3(v float64) float64 { return math.Round(v*1000) / 1000 }

var presets = map[string]Preset{
	"none": {
		Name:        "none",
		Description: "No tone processing at all - straight to the nearest ink.",
		// Every stage nil, so applyImageProcessing is a no-op.
		ColorMatching: matchRGB,
		Kernel:        "floydSteinberg",
	},
	"balanced": {
		Name:        "balanced",
		Description: "Compresses display luminance range for general photo conversion.",
		ToneMapping: &ToneMapping{
			Mode: "contrast",
		},
		DynamicRange: &DynamicRange{
			Mode:     "display",
			Strength: f(1),
		},
		ColorMatching: matchRGB,
		Kernel:        "floydSteinberg",
	},
	"dynamic": {
		Name:        "dynamic",
		Description: "S-curve tone mapping for brighter, punchier photographic output.",
		ToneMapping: &ToneMapping{
			Mode:              "scurve",
			Saturation:        linFromMul(1.3),
			Strength:          f(0.9),
			ShadowBoost:       0,
			HighlightCompress: f(-1.5),
			Midpoint:          f(0.5),
		},
		DynamicRange:  &DynamicRange{Mode: "off"},
		ColorMatching: matchRGB,
		Kernel:        "floydSteinberg",
	},
	"vivid": {
		Name:        "vivid",
		Description: "Boosts colour and applies a gentler S-curve for illustrations.",
		ToneMapping: &ToneMapping{
			Mode:              "scurve",
			Exposure:          expFromMul(1.1),
			Saturation:        linFromMul(1.6),
			Strength:          f(0.7),
			ShadowBoost:       0.1,
			HighlightCompress: f(-1.3),
			Midpoint:          f(0.5),
		},
		DynamicRange:  &DynamicRange{Mode: "off"},
		ColorMatching: matchRGB,
		Kernel:        "floydSteinberg",
	},
	"soft": {
		Name:        "soft",
		Description: "Reduces contrast and uses Stucki diffusion for smoother tones.",
		ToneMapping: &ToneMapping{
			Mode:       "contrast",
			Saturation: linFromMul(1.1),
			Contrast:   linFromMul(0.9),
		},
		DynamicRange: &DynamicRange{
			Mode:     "display",
			Strength: f(1),
		},
		ColorMatching: matchRGB,
		Kernel:        "stucki",
	},
	"grayscale": {
		Name:        "grayscale",
		Description: "Removes saturation and uses LAB matching for monochrome work.",
		ToneMapping: &ToneMapping{
			Mode:              "scurve",
			Saturation:        linFromMul(0),
			Strength:          f(0.8),
			ShadowBoost:       0.1,
			HighlightCompress: f(-1.4),
			Midpoint:          f(0.5),
		},
		DynamicRange: &DynamicRange{
			Mode:     "display",
			Strength: f(1),
		},
		ColorMatching: matchLAB,
		Kernel:        "floydSteinberg",
	},
	"restore": {
		Name:        "restore",
		Description: "Expands faded scans and paintings before mapping them to the display range.",
		ToneMapping: &ToneMapping{
			Mode:              "scurve",
			Exposure:          expFromMul(1.08),
			Saturation:        linFromMul(0.9),
			Strength:          f(1),
			ShadowBoost:       0.25,
			HighlightCompress: f(-0.75),
			Midpoint:          f(0.46),
		},
		DynamicRange: &DynamicRange{
			Mode:           "auto",
			Strength:       f(0.9),
			LowPercentile:  f(0.02),
			HighPercentile: f(0.98),
		},
		ColorMatching: matchLAB,
		Kernel:        "floydSteinberg",
	},
	"posterscan": {
		Name:        "posterScan",
		Description: "Neutralises warm paper, anchors black ink, preserves strong poster colours.",
		PaperNormalization: &PaperNormalization{
			Mode:                "warmPaper",
			Strength:            f(0.95),
			MinLuma:             f(82),
			SaturationThreshold: f(0.56),
			WarmBiasThreshold:   f(8),
			BlackAnchor:         f(0.95),
			PreserveRed:         f(0.85),
			PaperWhite:          &RGB{248, 248, 246},
		},
		ToneMapping: &ToneMapping{
			Mode:              "scurve",
			Exposure:          expFromMul(1.04),
			Saturation:        linFromMul(1.05),
			Strength:          f(0.92),
			ShadowBoost:       0.08,
			HighlightCompress: f(-0.55),
			Midpoint:          f(0.44),
		},
		DynamicRange: &DynamicRange{
			Mode:           "auto",
			Strength:       f(1),
			LowPercentile:  f(0.015),
			HighPercentile: f(0.985),
		},
		ColorMatching: matchRGB,
		Kernel:        "floydSteinberg",
	},
}

func presetNames() []string {
	names := make([]string, 0, len(presets))
	for name := range presets {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func lookupPreset(name string) (Preset, error) {
	p, ok := presets[strings.ToLower(name)]
	if !ok {
		return Preset{}, fmt.Errorf("unknown preset %q (try -list-presets)", name)
	}
	return p, nil
}
