// imgproc turns ordinary photographs into frames the Inky Impression 13.3"
// (EL133UF1 / Spectra 6) can display directly.
//
// Everything expensive happens here rather than on the device: scaling,
// cropping, tone mapping into the panel's much narrower range, reduction to
// its six inks, dithering, and the split between the two controllers. The
// panel is hung in portrait, so images are composed directly in the
// controllers' portrait scan order - no rotation is needed. What lands in
// artwork/processed/ is the exact byte stream the firmware pushes over SPI,
// so the ESP32 needs no image decoding and no framebuffer - it copies bytes
// from the socket to the panel.
//
// The tone pipeline, the calibrated palettes and the dither kernels are ported
// from paperlesspaper's epdoptimize (Apache-2.0), which is a browser library;
// see palette.go, tone.go and dither.go.
//
// Usage, from the repository root - the default -in and -out are relative to
// it, and the Go module is rooted there:
//
//	go run ./tools/imgproc                        # artwork/*.jpg -> artwork/processed/
//	go run ./tools/imgproc -preset restore        # rescue faded scans and paintings
//	go run ./tools/imgproc -preset posterscan     # warm paper, strong flat colour
//	go run ./tools/imgproc -fit contain           # letterbox instead of cropping
//	go run ./tools/imgproc -random-prefix         # shuffle the manifest order (see CYCLE_ALL_IMAGES)
//	go run ./tools/imgproc -list-presets          # and -list-palettes, -list-kernels
//
// Alongside each source image it writes a JPEG preview of the dithered result
// at the source's own resolution, so the two can be flipped between in any
// image viewer. Previews are build output and are gitignored.
//
// Output format, per .bin file (960,000 bytes):
//
//	bytes      0..479,999  CS_M half: 1600 rows of 300 bytes, portrait columns 0..599
//	bytes 480,000..959,999  CS_S half: same, portrait columns 600..1199
//
// Two pixels per byte, high nibble first, using the panel's colour codes.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"image"
	"image/jpeg"
	_ "image/png"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Panel geometry. The glass is 1600x1200 landscape, but the controllers scan
// it as 1200x1600 portrait, which is also how the frame hangs.
const (
	portraitW = 1200
	portraitH = 1600

	rowBytes    = 300 // one portrait row for one controller (600 px / 2)
	halfBytes   = rowBytes * portraitH
	streamBytes = halfBytes * 2
)

// previewSuffix replaces the source's extension, so images/art/foo.jpg gets
// images/art/foo.preview.jpg. findSources skips anything matching it, or the
// next run would treat previews as new source images.
const previewSuffix = ".preview.jpg"

type config struct {
	palette       [6]PaletteEntry
	paletteName   string
	dither        [6]RGB // calibrated colours, blended by -saturation
	preview       [6]RGB // what the preview is painted with
	saturation    float64
	preset        Preset
	processing    ImageProcessing
	fit           string
	kernel        []kernelTap
	kernelName    string
	matchMode     string
	serpentine    bool
	writePreview  bool
	previewSource string
	previewQual   int
}

// fingerprint is every setting that changes the packed stream. It is recorded
// in the manifest so that changing one of them invalidates the whole batch -
// mtimes alone cannot see a flag change.
func (c config) fingerprint() string {
	return fmt.Sprintf(
		"palette=%s saturation=%.3f preset=%s fit=%s kernel=%s match=%s serpentine=%t",
		c.paletteName, c.saturation, c.preset.Name, c.fit, c.kernelName,
		c.matchMode, c.serpentine,
	)
}

func main() {
	in := flag.String("in", "artwork", "directory of source images")
	out := flag.String("out", "artwork/processed", "directory for packed frames")
	paletteName := flag.String("palette", "spectra6",
		"calibrated display palette (-list-palettes)")
	presetName := flag.String("preset", "balanced",
		"tone processing preset (-list-presets)")
	saturation := flag.Float64("saturation", 1,
		"1 = dither against measured ink, 0 = against idealised primaries")
	fit := flag.String("fit", "cover",
		"cover (fill the panel, cropping overflow) or contain (letterbox on white)")
	kernelName := flag.String("dither", "",
		"error diffusion kernel; empty uses the preset's (-list-kernels)")
	matchMode := flag.String("match", "",
		"colour matching: rgb, lab, chroma or weighted; empty uses the preset's")
	serpentine := flag.Bool("serpentine", true,
		"reverse alternate rows while diffusing, which suppresses worming")
	writePreview := flag.Bool("preview", true,
		"write a JPEG of the result next to each source image")
	previewSource := flag.String("preview-source", "full",
		"full (whole image at its own resolution) or panel (the 1200x1600 frame)")
	previewColors := flag.String("preview-colors", "calibrated",
		"calibrated (how the panel will look) or device (the raw primaries)")
	previewQual := flag.Int("preview-quality", 92, "JPEG quality for previews")
	force := flag.Bool("force", false,
		"repack images whose output is already up to date (and re-roll -random-prefix)")
	randomPrefix := flag.Bool("random-prefix", false,
		"prefix each frame with 4 random characters (abcd_name.bin) so a sorted manifest is shuffled; "+
			"prefixes are kept between runs, -force re-rolls them")

	listPalettes := flag.Bool("list-palettes", false, "print the available palettes and exit")
	listPresets := flag.Bool("list-presets", false, "print the available presets and exit")
	listKernels := flag.Bool("list-kernels", false, "print the available dither kernels and exit")
	flag.Parse()

	switch {
	case *listPalettes:
		for _, name := range paletteNames() {
			fmt.Printf("  %-20s %s\n", name, paletteSpecs[name].desc)
		}
		return
	case *listPresets:
		for _, name := range presetNames() {
			p := presets[name]
			fmt.Printf("  %-12s %s\n", name, p.Description)
			fmt.Printf("  %-12s   dither=%s match=%s\n", "", p.Kernel, p.ColorMatching)
		}
		return
	case *listKernels:
		for _, name := range kernelNames() {
			fmt.Printf("  %s\n", name)
		}
		return
	}

	cfg, err := buildConfig(*paletteName, *presetName, *fit, *kernelName, *matchMode,
		*previewSource, *previewColors, *saturation, *previewQual, *serpentine, *writePreview)
	if err != nil {
		fatalf("%v", err)
	}

	sources, err := findSources(*in)
	if err != nil {
		fatalf("scanning %s: %v", *in, err)
	}
	if len(sources) == 0 {
		fatalf("no .jpg/.jpeg/.png files in %s", *in)
	}

	if err := os.MkdirAll(*out, 0o755); err != nil {
		fatalf("creating %s: %v", *out, err)
	}

	manifest := filepath.Join(*out, "manifest.txt")
	fingerprint := cfg.fingerprint()
	// A settings change makes every existing .bin wrong, not just stale.
	settingsChanged := readManifestSettings(manifest) != fingerprint
	repackAll := *force || settingsChanged
	if settingsChanged && !*force {
		fmt.Println("settings changed since the last run; repacking everything")
	}

	fmt.Printf("%s\n\n", fingerprint)

	stems := make(map[string]bool, len(sources))
	for _, src := range sources {
		stems[sourceStem(src)] = true
	}

	var packed []string
	for _, src := range sources {
		name := outputName(*out, sourceStem(src), stems, *randomPrefix, repackAll)
		dst := filepath.Join(*out, name)
		previewPath := previewPathFor(src)

		if !repackAll && upToDate(src, dst, previewPath, cfg.writePreview) {
			fmt.Printf("  %-44s up to date\n", filepath.Base(src))
			packed = append(packed, name)
			continue
		}

		if err := process(src, dst, previewPath, cfg); err != nil {
			// One bad file should not stop the batch; it just will not appear
			// in the manifest, so the device never asks for it.
			fmt.Fprintf(os.Stderr, "  %-44s FAILED: %v\n", filepath.Base(src), err)
			continue
		}
		fmt.Printf("  %-44s -> %s\n", filepath.Base(src), name)
		packed = append(packed, name)
	}

	if len(packed) == 0 {
		fatalf("nothing packed successfully")
	}

	sort.Strings(packed)
	if err := writeManifest(manifest, packed, fingerprint); err != nil {
		fatalf("writing %s: %v", manifest, err)
	}
	fmt.Printf("\n%d image(s), %s\n", len(packed), manifest)
}

func buildConfig(paletteName, presetName, fit, kernelName, matchMode,
	previewSource, previewColors string, saturation float64, previewQual int,
	serpentine, writePreview bool) (config, error) {

	var cfg config

	if fit != "cover" && fit != "contain" {
		return cfg, fmt.Errorf("-fit must be cover or contain, got %q", fit)
	}
	if saturation < 0 || saturation > 1 {
		return cfg, fmt.Errorf("-saturation must be between 0 and 1, got %v", saturation)
	}
	if previewSource != "full" && previewSource != "panel" {
		return cfg, fmt.Errorf("-preview-source must be full or panel, got %q", previewSource)
	}
	if previewColors != "calibrated" && previewColors != "device" {
		return cfg, fmt.Errorf("-preview-colors must be calibrated or device, got %q", previewColors)
	}
	if previewQual < 1 || previewQual > 100 {
		return cfg, fmt.Errorf("-preview-quality must be between 1 and 100, got %d", previewQual)
	}

	entries, err := lookupPalette(paletteName)
	if err != nil {
		return cfg, err
	}
	preset, err := lookupPreset(presetName)
	if err != nil {
		return cfg, err
	}

	if kernelName == "" {
		kernelName = preset.Kernel
	}
	kernel, err := lookupKernel(kernelName)
	if err != nil {
		return cfg, err
	}

	if matchMode == "" {
		matchMode = preset.ColorMatching
	}
	if !validMatchMode(matchMode) {
		return cfg, fmt.Errorf("-match must be one of %s, got %q",
			strings.Join(matchModeNames(), ", "), matchMode)
	}

	cfg = config{
		palette:       entries,
		paletteName:   strings.ToLower(paletteName),
		dither:        blend(entries, saturation),
		saturation:    saturation,
		preset:        preset,
		processing:    preset.processing(),
		fit:           fit,
		kernel:        kernel,
		kernelName:    kernelName,
		matchMode:     matchMode,
		serpentine:    serpentine,
		writePreview:  writePreview,
		previewSource: previewSource,
		previewQual:   previewQual,
	}

	// The preview is normally painted in the calibrated colours, because those
	// are what the ink looks like; the device primaries are only useful for
	// checking which ink was chosen where.
	cfg.preview = cfg.dither
	if previewColors == "device" {
		cfg.preview = deviceColors(entries)
	}
	return cfg, nil
}

// prefixLen and prefixAlphabet define the -random-prefix tag: four characters,
// followed by prefixSep, ahead of the source's name.
const (
	prefixLen      = 4
	prefixAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
	prefixSep      = "_"
)

func sourceStem(src string) string {
	return strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))
}

// hasPrefix reports whether name is a randomly prefixed frame for stem.
func hasPrefix(name, stem string) bool {
	tag, ok := strings.CutSuffix(name, prefixSep+stem+".bin")
	if !ok || len(tag) != prefixLen {
		return false
	}
	return strings.Trim(tag, prefixAlphabet) == ""
}

// outputName picks the .bin name for a source and moves any existing frame
// for it - plain or prefixed - to that name, so toggling -random-prefix does
// not leave stale copies or force a repack.
//
// Without -random-prefix the name is just the stem. With it, the existing
// prefix is reused so reruns stay incremental; reroll (-force or a settings
// change) picks a new one, which reshuffles the order. A file whose name
// matches another source's own stem is never treated as a variant of this one.
func outputName(outDir, stem string, stems map[string]bool, randomPrefix, reroll bool) string {
	var existing []string
	if entries, err := os.ReadDir(outDir); err == nil {
		for _, e := range entries {
			n := e.Name()
			if e.IsDir() || !strings.HasSuffix(n, ".bin") {
				continue
			}
			if n == stem+".bin" || (hasPrefix(n, stem) && !stems[strings.TrimSuffix(n, ".bin")]) {
				existing = append(existing, n)
			}
		}
	}

	name := stem + ".bin"
	if randomPrefix {
		name = ""
		for _, n := range existing {
			if n != stem+".bin" && !reroll {
				name = n
				break
			}
		}
		if name == "" {
			tag := make([]byte, prefixLen)
			for i := range tag {
				tag[i] = prefixAlphabet[rand.Intn(len(prefixAlphabet))]
			}
			name = string(tag) + prefixSep + stem + ".bin"
		}
	}

	renamed := false
	for _, n := range existing {
		if n == name {
			continue
		}
		from := filepath.Join(outDir, n)
		// Carry one up-to-date frame across to the new name; drop the rest.
		if !renamed && !reroll {
			if _, err := os.Stat(filepath.Join(outDir, name)); err != nil &&
				os.Rename(from, filepath.Join(outDir, name)) == nil {
				renamed = true
				continue
			}
		}
		os.Remove(from)
	}
	return name
}

func previewPathFor(src string) string {
	return strings.TrimSuffix(src, filepath.Ext(src)) + previewSuffix
}

func findSources(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range entries {
		if e.IsDir() || strings.HasSuffix(strings.ToLower(e.Name()), previewSuffix) {
			continue
		}
		// Preview renders live alongside the originals; they are derived
		// images, not sources, and packing them would duplicate every piece.
		if strings.Contains(strings.ToLower(e.Name()), ".preview") {
			continue
		}
		switch strings.ToLower(filepath.Ext(e.Name())) {
		case ".jpg", ".jpeg", ".png":
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	sort.Strings(out)
	return out, nil
}

// upToDate reports whether every output for src exists and is newer than it.
func upToDate(src, dst, previewPath string, wantPreview bool) bool {
	si, err := os.Stat(src)
	if err != nil {
		return false
	}

	di, err := os.Stat(dst)
	if err != nil || di.Size() != streamBytes || !di.ModTime().After(si.ModTime()) {
		return false
	}

	if wantPreview {
		pi, err := os.Stat(previewPath)
		if err != nil || !pi.ModTime().After(si.ModTime()) {
			return false
		}
	}
	return true
}

func process(src, dst, previewPath string, cfg config) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()

	img, _, err := image.Decode(bufio.NewReader(f))
	if err != nil {
		return fmt.Errorf("decoding: %w", err)
	}

	pal := newMatchPalette(cfg.dither[:], cfg.matchMode)

	// The panel frame. Tone mapping and dithering both happen at the final
	// size, so error diffuses across pixels the panel actually has rather
	// than being smeared by a later resize.
	frame := resample(img, portraitW, portraitH, cfg.fit)
	applyImageProcessing(frame, cfg.processing, cfg.dither[:])
	indices := errorDiffuse(frame, pal, cfg.kernel, cfg.serpentine)

	if err := writeFrame(dst, indices); err != nil {
		return err
	}

	if !cfg.writePreview {
		return nil
	}

	previewIndices, w, h := indices, portraitW, portraitH
	if cfg.previewSource == "full" {
		// The whole image, uncropped, at its own resolution - the version to
		// flip against the original. Note that the dither runs at this size,
		// so the dot pattern is finer than the panel's, and an "auto" dynamic
		// range pass measures this framing rather than the cropped one.
		b := img.Bounds()
		w, h = b.Dx(), b.Dy()
		full := toImage(img)
		applyImageProcessing(full, cfg.processing, cfg.dither[:])
		previewIndices = errorDiffuse(full, pal, cfg.kernel, cfg.serpentine)
	}

	return writeJPEG(previewPath, render(previewIndices, w, h, cfg.preview[:]), cfg.previewQual)
}

// toImage copies a decoded image into the byte buffer the stages work on.
func toImage(src image.Image) *Image {
	b := src.Bounds()
	out := NewImage(b.Dx(), b.Dy())

	i := 0
	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			// Go returns 16-bit premultiplied values; scale to 0..255.
			r, g, bl, _ := src.At(x, y).RGBA()
			out.Pix[i] = uint8(r / 257)
			out.Pix[i+1] = uint8(g / 257)
			out.Pix[i+2] = uint8(bl / 257)
			i += 3
		}
	}
	return out
}

// resample scales src to exactly w x h using bilinear interpolation.
//
// "cover" scales to fill and centre-crops the overflow; "contain" scales to fit
// and pads with white, which on this panel is a real ink colour rather than an
// absence of one.
func resample(src image.Image, w, h int, fit string) *Image {
	b := src.Bounds()
	sw, sh := b.Dx(), b.Dy()

	scale := math.Max(float64(w)/float64(sw), float64(h)/float64(sh))
	if fit == "contain" {
		scale = math.Min(float64(w)/float64(sw), float64(h)/float64(sh))
	}

	dw, dh := int(math.Round(float64(sw)*scale)), int(math.Round(float64(sh)*scale))
	offX, offY := (dw-w)/2, (dh-h)/2 // negative under "contain": the padding

	out := NewImage(w, h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			dx, dy := x+offX, y+offY
			i := (y*w + x) * 3

			if dx < 0 || dy < 0 || dx >= dw || dy >= dh {
				out.Pix[i], out.Pix[i+1], out.Pix[i+2] = 255, 255, 255 // letterbox
				continue
			}

			// Map back into source coordinates, sampling at pixel centres.
			fx := (float64(dx)+0.5)/scale - 0.5
			fy := (float64(dy)+0.5)/scale - 0.5
			r, g, bl := bilinear(src, b, sw, sh, fx, fy)
			out.Pix[i] = clampByte(r)
			out.Pix[i+1] = clampByte(g)
			out.Pix[i+2] = clampByte(bl)
		}
	}
	return out
}

func bilinear(src image.Image, b image.Rectangle, sw, sh int, fx, fy float64) (float64, float64, float64) {
	x0, y0 := int(math.Floor(fx)), int(math.Floor(fy))
	tx, ty := fx-float64(x0), fy-float64(y0)

	at := func(x, y int) (float64, float64, float64) {
		x = clampInt(x, 0, sw-1)
		y = clampInt(y, 0, sh-1)
		r, g, bl, _ := src.At(b.Min.X+x, b.Min.Y+y).RGBA()
		return float64(r) / 257, float64(g) / 257, float64(bl) / 257
	}

	r00, g00, b00 := at(x0, y0)
	r10, g10, b10 := at(x0+1, y0)
	r01, g01, b01 := at(x0, y0+1)
	r11, g11, b11 := at(x0+1, y0+1)

	lerp := func(a, b, t float64) float64 { return a + (b-a)*t }
	r := lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty)
	g := lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty)
	bl := lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty)
	return r, g, bl
}

// writeFrame splits the portrait image at column 600, one half per controller,
// and packs two pixels per byte. The image is already in scan order, so this is
// a straight walk across it - which is what lets the device stream from a
// socket to SPI without a framebuffer.
func writeFrame(dst string, indices []uint8) error {
	buf := make([]byte, 0, streamBytes)

	for half := 0; half < 2; half++ {
		base := half * 600
		for y := 0; y < portraitH; y++ {
			for k := 0; k < rowBytes; k++ {
				xe := base + 2*k // even portrait column -> high nibble
				xo := xe + 1
				hi := panelCodes[indices[y*portraitW+xe]]
				lo := panelCodes[indices[y*portraitW+xo]]
				buf = append(buf, hi<<4|lo)
			}
		}
	}

	if len(buf) != streamBytes {
		return fmt.Errorf("packed %d bytes, expected %d", len(buf), streamBytes)
	}
	return os.WriteFile(dst, buf, 0o644)
}

func writeJPEG(path string, img *Image, quality int) error {
	nrgba := image.NewNRGBA(image.Rect(0, 0, img.W, img.H))
	for i, n := 0, img.W*img.H; i < n; i++ {
		nrgba.Pix[i*4] = img.Pix[i*3]
		nrgba.Pix[i*4+1] = img.Pix[i*3+1]
		nrgba.Pix[i*4+2] = img.Pix[i*3+2]
		nrgba.Pix[i*4+3] = 255
	}

	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	w := bufio.NewWriter(f)
	if err := jpeg.Encode(w, nrgba, &jpeg.Options{Quality: quality}); err != nil {
		return err
	}
	if err := w.Flush(); err != nil {
		return err
	}
	return f.Close()
}

const manifestSettingsPrefix = "# settings: "

func writeManifest(path string, names []string, fingerprint string) error {
	var sb strings.Builder
	sb.WriteString("# Generated by tools/imgproc - one packed frame per line.\n")
	sb.WriteString(manifestSettingsPrefix + fingerprint + "\n")
	for _, n := range names {
		sb.WriteString(n)
		sb.WriteByte('\n')
	}
	return os.WriteFile(path, []byte(sb.String()), 0o644)
}

// readManifestSettings returns the settings the existing frames were packed
// with, or "" if there is no manifest or it predates the settings line.
func readManifestSettings(path string) string {
	body, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(body), "\n") {
		if rest, ok := strings.CutPrefix(strings.TrimSpace(line), manifestSettingsPrefix); ok {
			return strings.TrimSpace(rest)
		}
	}
	return ""
}

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "imgproc: "+format+"\n", args...)
	os.Exit(1)
}
