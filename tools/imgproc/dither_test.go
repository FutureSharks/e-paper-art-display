package main

import (
	"math"
	"testing"
)

// The point of error diffusion is that a flat input reproduces on average,
// even though no single ink matches it. If the mean of the dithered result
// drifts from the input, error is being lost or double-counted somewhere.
func TestErrorDiffusionPreservesMean(t *testing.T) {
	entries, err := lookupPalette("spectra6")
	if err != nil {
		t.Fatal(err)
	}
	colors := blend(entries, 1)
	pal := newMatchPalette(colors[:], matchRGB)
	kernel := diffusionKernels["floydSteinberg"]

	// Greys spanning the panel's range, plus a few saturated targets.
	targets := []RGB{
		{64, 64, 64}, {96, 96, 96}, {128, 128, 128}, {160, 160, 160},
		{96, 64, 64}, {64, 96, 80}, {110, 100, 60},
	}

	for _, target := range targets {
		for _, serpentine := range []bool{false, true} {
			img := NewImage(240, 240)
			for i := 0; i < len(img.Pix); i += 3 {
				img.Pix[i], img.Pix[i+1], img.Pix[i+2] = target[0], target[1], target[2]
			}

			indices := errorDiffuse(img, pal, kernel, serpentine)

			var sum [3]float64
			for _, idx := range indices {
				c := colors[idx]
				for ch := 0; ch < 3; ch++ {
					sum[ch] += float64(c[ch])
				}
			}

			n := float64(len(indices))
			for ch := 0; ch < 3; ch++ {
				mean := sum[ch] / n
				// Only the border can lose error, so a few levels of drift is
				// expected; anything more means the diffusion is leaking.
				if drift := math.Abs(mean - float64(target[ch])); drift > 3 {
					t.Errorf("target %v serpentine=%t channel %d: mean %.1f, drift %.1f",
						target, serpentine, ch, mean, drift)
				}
			}
		}
	}
}

// A flat input must not produce large contiguous runs of one ink: that is the
// signature of error running away instead of being spent locally.
func TestErrorDiffusionDoesNotRunAway(t *testing.T) {
	entries, err := lookupPalette("spectra6")
	if err != nil {
		t.Fatal(err)
	}
	colors := blend(entries, 1)
	pal := newMatchPalette(colors[:], matchRGB)

	img := NewImage(600, 200)
	for i := 0; i < len(img.Pix); i += 3 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2] = 150, 150, 150
	}

	indices := errorDiffuse(img, pal, diffusionKernels["floydSteinberg"], true)

	longest, run := 0, 0
	for x := 1; x < 600; x++ {
		if indices[100*600+x] == indices[100*600+x-1] {
			run++
			if run > longest {
				longest = run
			}
		} else {
			run = 0
		}
	}
	if longest > 40 {
		t.Errorf("longest single-ink run on a flat row is %d px, expected a mix", longest)
	}
}

// Round-tripping through LAB should land back on roughly the same colour;
// a sign error or a bad matrix shows up immediately here.
func TestLabRoundTrip(t *testing.T) {
	for _, c := range []RGB{
		{0, 0, 0}, {255, 255, 255}, {128, 128, 128},
		{31, 34, 38}, {185, 199, 201}, {98, 32, 30}, {35, 86, 58},
	} {
		lab := rgbToLab(c[0], c[1], c[2])
		back := labToRgb(lab[0], lab[1], lab[2])
		for ch := 0; ch < 3; ch++ {
			if d := int(back[ch]) - int(c[ch]); d > 1 || d < -1 {
				t.Errorf("%v -> LAB %v -> %v, channel %d off by %d", c, lab, back, ch, d)
			}
		}
	}
	// And the L of rgbToLab must agree with the histogram-only shortcut.
	for _, c := range []RGB{{0, 0, 0}, {60, 120, 200}, {255, 255, 255}} {
		full := rgbToLab(c[0], c[1], c[2])[0]
		quick := rgbToLabLightness(c[0], c[1], c[2])
		if math.Abs(full-quick) > 1e-9 {
			t.Errorf("%v: rgbToLab L=%v but rgbToLabLightness=%v", c, full, quick)
		}
	}
}

// Every palette must supply all six panel roles, or writeFrame would index a
// colour the controller has no code for.
func TestPalettesCoverPanelRoles(t *testing.T) {
	for _, name := range paletteNames() {
		entries, err := lookupPalette(name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		for i, role := range panelRoles {
			if entries[i].Name != role {
				t.Errorf("%s: slot %d is %q, want %q", name, i, entries[i].Name, role)
			}
		}
	}
}

// Every preset must name a kernel and matching mode that actually exist.
func TestPresetsAreResolvable(t *testing.T) {
	for _, name := range presetNames() {
		p := presets[name]
		if _, err := lookupKernel(p.Kernel); err != nil {
			t.Errorf("preset %s: %v", name, err)
		}
		if !validMatchMode(p.ColorMatching) {
			t.Errorf("preset %s: bad colour matching %q", name, p.ColorMatching)
		}
	}
}
