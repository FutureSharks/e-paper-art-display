// art-fetcher downloads public-domain, portrait-orientation artworks from
// Wikidata and Wikimedia Commons.
package main

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	userAgent   = "EPaperArtFrame/1.0 (https://github.com/FutureSharks/e-paper-art-display)"
	wikidataAPI = "https://www.wikidata.org/w/api.php"
	sparqlURL   = "https://query.wikidata.org/sparql"
	commonsAPI  = "https://commons.wikimedia.org/w/api.php"

	minRatio         = 1.15 // panel is 1200x1600
	maxRatio         = 1.85
	minWidth         = 800
	maxOriginalBytes = 25 << 20
	fallbackWidth    = 3000
	maxDeathYear     = 1955 // EU life+70

	maxAttempts  = 8
	baseDelay    = 5 * time.Second
	maxDelay     = 2 * time.Minute
	maxPace      = 10 * time.Second
	manifestName = "manifest.csv"
)

type artist struct {
	name  string
	max   int
	group string
}

var artists = []artist{
	{"Giuseppe Arcimboldo", 8, "quirky"},
	{"Hieronymus Bosch", 5, "quirky"},
	{"Pieter Bruegel the Elder", 3, "quirky"},
	{"William Blake", 6, "quirky"},
	{"Odilon Redon", 6, "quirky"},
	{"Henri Rousseau", 6, "quirky"},
	{"James Ensor", 5, "quirky"},
	{"Hilma af Klint", 8, "quirky"},
	{"Paul Klee", 8, "quirky"},
	{"Utagawa Kuniyoshi", 8, "quirky"},
	{"Tsukioka Yoshitoshi", 5, "quirky"},
	{"Kawanabe Kyōsai", 4, "quirky"},
	{"Harry Clarke", 4, "quirky"},
	{"Frida Kahlo", 5, "quirky"},
	{"Alphonse Mucha", 8, "punchy"},
	{"Henri Matisse", 6, "punchy"},
	{"Henri de Toulouse-Lautrec", 6, "punchy"},
	{"Gustav Klimt", 8, "punchy"},
	{"Egon Schiele", 5, "punchy"},
	{"Edvard Munch", 6, "punchy"},
	{"Franz Marc", 6, "punchy"},
	{"August Macke", 5, "punchy"},
	{"Ernst Ludwig Kirchner", 5, "punchy"},
	{"Marianne von Werefkin", 4, "punchy"},
	{"Alexej von Jawlensky", 6, "punchy"},
	{"Wassily Kandinsky", 6, "punchy"},
	{"Paul Gauguin", 6, "punchy"},
	{"Vincent van Gogh", 8, "punchy"},
	{"Paul Signac", 4, "punchy"},
	{"Amedeo Modigliani", 6, "punchy"},
	{"Ivan Bilibin", 5, "punchy"},
	{"Katsushika Hokusai", 5, "punchy"},
	{"Utagawa Hiroshige", 6, "punchy"},
	{"Kitagawa Utamaro", 3, "punchy"},
	{"Piet Mondrian", 4, "punchy"},
	{"Kazimir Malevich", 4, "punchy"},
	{"Koloman Moser", 3, "punchy"},
	{"Jan Toorop", 3, "punchy"},
	{"Johannes Vermeer", 4, "classic"},
	{"Jan van Eyck", 3, "classic"},
	{"Sandro Botticelli", 4, "classic"},
}

var sculptureQIDs = map[string]bool{"Q860861": true, "Q179700": true, "Q241045": true}

type imageInfo struct {
	Width    int
	Height   int
	Size     int64
	Mime     string
	URL      string
	ThumbURL string
	Page     string `json:"descriptionurl"`
}

type work struct {
	imageInfo
	qid, title, file, year string
	sitelinks              int
	artist, group          string
}

// HTTP

var (
	client = &http.Client{Timeout: 5 * time.Minute}
	pace   = time.Second // delay between downloads; grows on 429
)

type statusError struct {
	code       int
	retryAfter time.Duration
}

func (e *statusError) Error() string { return fmt.Sprintf("HTTP %d", e.code) }

func fetch(u string, handle func(io.Reader) error) error {
	for attempt := 1; ; attempt++ {
		err := fetchOnce(u, handle)
		if err == nil {
			return nil
		}
		d := min(baseDelay<<(attempt-1), maxDelay)
		wait := d + time.Duration(rand.Int63n(int64(d/2)))
		var se *statusError
		if errors.As(err, &se) {
			if se.code != http.StatusTooManyRequests && se.code < 500 {
				return err
			}
			if se.code == http.StatusTooManyRequests {
				pace = min(pace*2, maxPace)
			}
			wait = max(wait, se.retryAfter)
		}
		if attempt == maxAttempts {
			return err
		}
		fmt.Printf("    %v, retrying in %s (%d/%d)\n", err, wait.Round(time.Second), attempt, maxAttempts-1)
		time.Sleep(wait)
	}
}

func fetchOnce(u string, handle func(io.Reader) error) error {
	req, err := http.NewRequest(http.MethodGet, u, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", userAgent)
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, resp.Body)
		return &statusError{resp.StatusCode, parseRetryAfter(resp.Header.Get("Retry-After"))}
	}
	return handle(resp.Body)
}

func parseRetryAfter(v string) time.Duration {
	if s, err := strconv.Atoi(v); err == nil {
		return time.Duration(s) * time.Second
	}
	if t, err := http.ParseTime(v); err == nil {
		return time.Until(t)
	}
	return 0
}

func getJSON(base string, params url.Values, v any) error {
	var body []byte
	err := fetch(base+"?"+params.Encode(), func(r io.Reader) (err error) {
		body, err = io.ReadAll(r)
		return err
	})
	if err != nil {
		return err
	}
	return json.Unmarshal(body, v)
}

// Wikidata

var (
	yearRe  = regexp.MustCompile(`^[+-]?(\d{4})`)
	qidRe   = regexp.MustCompile(`^Q\d+$`)
	nonWord = regexp.MustCompile(`[^\p{L}\p{N}_\s\p{Z}-]`)
	seps    = regexp.MustCompile(`[\s\p{Z}_-]+`)
)

func resolveArtist(name string) (qid, label string, died int, err error) {
	var search struct {
		Search []struct{ ID string }
	}
	err = getJSON(wikidataAPI, url.Values{
		"action": {"wbsearchentities"}, "search": {name}, "language": {"en"},
		"type": {"item"}, "limit": {"7"}, "format": {"json"},
	}, &search)
	if err != nil || len(search.Search) == 0 {
		return
	}
	ids := make([]string, len(search.Search))
	for i, h := range search.Search {
		ids[i] = h.ID
	}

	type claim struct {
		Mainsnak struct {
			Datavalue struct{ Value json.RawMessage }
		}
	}
	var ents struct {
		Entities map[string]struct {
			Labels map[string]struct{ Value string }
			Claims map[string][]claim
		}
	}
	err = getJSON(wikidataAPI, url.Values{
		"action": {"wbgetentities"}, "ids": {strings.Join(ids, "|")},
		"props": {"claims|labels"}, "languages": {"en"}, "format": {"json"},
	}, &ents)
	if err != nil {
		return
	}

	for _, id := range ids {
		e := ents.Entities[id]
		human := false
		for _, c := range e.Claims["P31"] {
			var v struct{ ID string }
			if json.Unmarshal(c.Mainsnak.Datavalue.Value, &v) == nil && v.ID == "Q5" {
				human = true
				break
			}
		}
		if !human {
			continue
		}
		for _, c := range e.Claims["P570"] {
			var v struct{ Time string }
			if json.Unmarshal(c.Mainsnak.Datavalue.Value, &v) == nil {
				if m := yearRe.FindStringSubmatch(v.Time); m != nil {
					died, _ = strconv.Atoi(m[1])
					break
				}
			}
		}
		label = name
		if l := e.Labels["en"].Value; l != "" {
			label = l
		}
		return id, label, died, nil
	}
	return
}

func worksBy(qid string) ([]work, error) {
	q := fmt.Sprintf(`
SELECT ?item ?itemLabel ?image ?inception ?sl (GROUP_CONCAT(DISTINCT ?type; separator=",") AS ?types) WHERE {
  ?item wdt:P170 wd:%s ; wdt:P18 ?image ; wikibase:sitelinks ?sl .
  OPTIONAL { ?item wdt:P571 ?inception . }
  OPTIONAL { ?item wdt:P31 ?type . }
  SERVICE wikibase:label { bd:serviceParam wikibase:language "en,de,fr,nl,it,ja,ru". }
}
GROUP BY ?item ?itemLabel ?image ?inception ?sl`, qid)

	var res struct {
		Results struct {
			Bindings []map[string]struct{ Value string }
		}
	}
	if err := getJSON(sparqlURL, url.Values{"query": {q}, "format": {"json"}}, &res); err != nil {
		return nil, err
	}

	var works []work
	seen := map[string]bool{}
	for _, r := range res.Results.Bindings {
		item := lastPart(r["item"].Value)
		if seen[item] {
			continue
		}
		sculpture := false
		for _, t := range strings.Split(r["types"].Value, ",") {
			sculpture = sculpture || sculptureQIDs[lastPart(t)]
		}
		if sculpture {
			continue
		}
		file, err := url.PathUnescape(lastPart(r["image"].Value))
		if err != nil {
			continue
		}
		year := ""
		if m := yearRe.FindStringSubmatch(r["inception"].Value); m != nil {
			year = m[1]
		}
		title := r["itemLabel"].Value
		if qidRe.MatchString(title) {
			title = strings.TrimSuffix(file, filepath.Ext(file))
		}
		sl, _ := strconv.Atoi(r["sl"].Value)
		seen[item] = true
		works = append(works, work{qid: item, title: title, file: file, year: year, sitelinks: sl})
	}
	return works, nil
}

func lastPart(s string) string { return s[strings.LastIndex(s, "/")+1:] }

// Commons

func imageInfos(files []string) (map[string]imageInfo, error) {
	info := map[string]imageInfo{}
	for i := 0; i < len(files); i += 50 {
		batch := files[i:min(i+50, len(files))]
		titles := make([]string, len(batch))
		for j, f := range batch {
			titles[j] = "File:" + f
		}
		var data struct {
			Query struct {
				Normalized []struct{ From, To string }
				Pages      map[string]struct {
					Title     string
					ImageInfo []imageInfo
				}
			}
		}
		err := getJSON(commonsAPI, url.Values{
			"action": {"query"}, "format": {"json"}, "titles": {strings.Join(titles, "|")},
			"prop": {"imageinfo"}, "iiprop": {"url|size|mime"}, "iiurlwidth": {strconv.Itoa(fallbackWidth)},
		}, &data)
		if err != nil {
			return nil, err
		}
		norm := map[string]string{}
		for _, n := range data.Query.Normalized {
			norm[n.To] = n.From
		}
		for _, p := range data.Query.Pages {
			if len(p.ImageInfo) == 0 {
				continue
			}
			title := p.Title
			if from, ok := norm[title]; ok {
				title = from
			}
			_, name, _ := strings.Cut(title, ":")
			info[name] = p.ImageInfo[0]
			info[strings.ReplaceAll(name, " ", "_")] = p.ImageInfo[0]
		}
		time.Sleep(300 * time.Millisecond)
	}
	return info, nil
}

func (m imageInfo) portrait() bool {
	if m.Width < minWidth || m.Height == 0 {
		return false
	}
	r := float64(m.Height) / float64(m.Width)
	return r >= minRatio && r <= maxRatio
}

// source returns the original if it's a reasonable JPEG/PNG, otherwise the thumbnail render.
func (m imageInfo) source() (src, ext string) {
	switch {
	case m.Mime == "image/jpeg" && m.Size <= maxOriginalBytes:
		return m.URL, "jpg"
	case m.Mime == "image/png" && m.Size <= maxOriginalBytes:
		return m.URL, "png"
	case m.ThumbURL != "":
		if u, err := url.Parse(m.ThumbURL); err == nil && strings.HasSuffix(strings.ToLower(u.Path), ".png") {
			return m.ThumbURL, "png"
		}
		return m.ThumbURL, "jpg"
	}
	return "", ""
}

// Selection

func selectWorks(list []artist) []work {
	var selected []work
	seenFiles := map[string]bool{}
	for _, a := range list {
		qid, label, died, err := resolveArtist(a.name)
		switch {
		case err != nil:
			fmt.Printf("  ! %s: lookup failed (%v)\n", a.name, err)
			continue
		case qid == "":
			fmt.Printf("  ! %s: not found on Wikidata\n", a.name)
			continue
		case died == 0 || died > maxDeathYear:
			fmt.Printf("  ! %s (%s): died %d - may be in copyright, skipped\n", label, qid, died)
			continue
		}

		works, err := worksBy(qid)
		if err != nil {
			fmt.Printf("  ! %s: query failed (%v)\n", label, err)
			continue
		}
		if len(works) == 0 {
			fmt.Printf("  - %s: no works with images\n", label)
			continue
		}

		files := map[string]bool{}
		for _, w := range works {
			files[w.file] = true
		}
		names := make([]string, 0, len(files))
		for f := range files {
			names = append(names, f)
		}
		sort.Strings(names)
		meta, err := imageInfos(names)
		if err != nil {
			fmt.Printf("  ! %s: image info failed (%v)\n", label, err)
			continue
		}

		var portrait []work
		for _, w := range works {
			m, ok := meta[w.file]
			if !ok {
				m, ok = meta[strings.ReplaceAll(w.file, " ", "_")]
			}
			if ok && m.portrait() && !seenFiles[w.file] {
				w.imageInfo = m
				portrait = append(portrait, w)
			}
		}
		sort.SliceStable(portrait, func(i, j int) bool {
			if portrait[i].sitelinks != portrait[j].sitelinks {
				return portrait[i].sitelinks > portrait[j].sitelinks
			}
			return portrait[i].Width > portrait[j].Width
		})
		chosen := portrait[:min(a.max, len(portrait))]
		for i := range chosen {
			chosen[i].artist, chosen[i].group = label, a.group
			seenFiles[chosen[i].file] = true
		}
		selected = append(selected, chosen...)
		fmt.Printf("  + %-28s %4d works, %3d portrait, taking %d\n", label, len(works), len(portrait), len(chosen))
		time.Sleep(500 * time.Millisecond)
	}
	return selected
}

// Download

func slug(s string, n int) string {
	s = seps.ReplaceAllString(strings.ToLower(strings.TrimSpace(nonWord.ReplaceAllString(s, ""))), "-")
	if r := []rune(s); len(r) > n {
		s = string(r[:n])
	}
	if s = strings.Trim(s, "-"); s == "" {
		return "untitled"
	}
	return s
}

func stripQuery(u string) string {
	u, _, _ = strings.Cut(u, "?")
	return u
}

func exists(path string) bool {
	fi, err := os.Stat(path)
	return err == nil && fi.Size() > 0
}

// readManifest maps source URL -> file from a previous run.
func readManifest(path string) map[string]string {
	out := map[string]string{}
	f, err := os.Open(path)
	if err != nil {
		return out
	}
	defer f.Close()
	rows, err := csv.NewReader(f).ReadAll()
	if err != nil || len(rows) == 0 {
		return out
	}
	fileCol, srcCol := -1, -1
	for i, h := range rows[0] {
		switch h {
		case "file":
			fileCol = i
		case "source_url":
			srcCol = i
		}
	}
	if fileCol < 0 || srcCol < 0 {
		return out
	}
	for _, r := range rows[1:] {
		out[stripQuery(r[srcCol])] = r[fileCol]
	}
	return out
}

// localName reuses a file from a previous run if the source URL matches, even
// if its index has since shifted.
func localName(dir string, i int, w work, src, ext string, prev, owner map[string]string) (string, bool) {
	key := stripQuery(src)
	if f, ok := prev[key]; ok && exists(filepath.Join(dir, f)) {
		return f, true
	}
	name := fmt.Sprintf("%03d_%s_%s.%s", i, slug(w.artist, 25), slug(w.title, 50), ext)
	if o, ok := owner[name]; ok && o != key {
		name = fmt.Sprintf("%03d_%s_%s_%s.%s", i, slug(w.artist, 25), slug(w.title, 50), w.qid, ext)
	}
	return name, exists(filepath.Join(dir, name))
}

func download(src, path string) (int64, error) {
	tmp := path + ".part"
	var n int64
	err := fetch(src, func(r io.Reader) error {
		f, err := os.Create(tmp)
		if err != nil {
			return err
		}
		n, err = io.Copy(f, r)
		if cerr := f.Close(); err == nil {
			err = cerr
		}
		return err
	})
	if err == nil {
		err = os.Rename(tmp, path)
	}
	if err != nil {
		os.Remove(tmp)
	}
	return n, err
}

func run(selected []work, dir string, dryRun bool) error {
	manifestPath := filepath.Join(dir, manifestName)
	prev := readManifest(manifestPath)
	owner := map[string]string{}
	for u, f := range prev {
		owner[f] = u
	}

	rows := [][]string{{"file", "artist", "title", "year", "group", "width", "height", "wikidata", "commons_page", "source_url"}}
	var failed []string
	var got, had int
	for i, w := range selected {
		n := i + 1
		src, ext := w.source()
		if src == "" {
			failed = append(failed, fmt.Sprintf("%s - %s: no usable URL", w.artist, w.title))
			continue
		}
		name, have := localName(dir, n, w, src, ext, prev, owner)
		row := []string{
			name, w.artist, w.title, w.year, w.group, strconv.Itoa(w.Width), strconv.Itoa(w.Height),
			"https://www.wikidata.org/wiki/" + w.qid, w.Page, src,
		}
		switch {
		case dryRun:
			status := "new "
			if have {
				status = "have"
			}
			fmt.Printf("  %s  %s  (%dx%d)\n", status, name, w.Width, w.Height)
			continue
		case have:
			had++
			rows = append(rows, row)
			continue
		}
		size, err := download(src, filepath.Join(dir, name))
		if err != nil {
			failed = append(failed, fmt.Sprintf("%s: %v", name, err))
			fmt.Printf("  [%d/%d] FAILED %s: %v\n", n, len(selected), name, err)
		} else {
			got++
			rows = append(rows, row)
			fmt.Printf("  [%d/%d] %s  %d KB\n", n, len(selected), name, size/1024)
		}
		time.Sleep(pace)
	}
	if dryRun {
		return nil
	}

	var buf strings.Builder
	cw := csv.NewWriter(&buf)
	cw.WriteAll(rows)
	if err := os.WriteFile(manifestPath, []byte(buf.String()), 0o644); err != nil {
		return err
	}
	failedPath := filepath.Join(dir, "failed.txt")
	os.Remove(failedPath)
	if len(failed) > 0 {
		if err := os.WriteFile(failedPath, []byte(strings.Join(failed, "\n")+"\n"), 0o644); err != nil {
			return err
		}
	}
	fmt.Printf("\n%d downloaded, %d already present, %d failed\n", got, had, len(failed))
	if len(failed) > 0 {
		fmt.Printf("See %s\n", failedPath)
	}
	return nil
}

func main() {
	out := flag.String("out", "tools/art-fetcher/downloads", "output directory")
	maxTotal := flag.Int("max-total", 200, "maximum number of images")
	dryRun := flag.Bool("dry-run", false, "list what would be downloaded")
	only := flag.String("only", "", "comma-separated artist name fragments to include")
	flag.Parse()

	list := artists
	if *only != "" {
		list = nil
		for _, a := range artists {
			for _, frag := range strings.Split(*only, ",") {
				if frag = strings.TrimSpace(strings.ToLower(frag)); frag != "" && strings.Contains(strings.ToLower(a.name), frag) {
					list = append(list, a)
					break
				}
			}
		}
	}

	if err := os.MkdirAll(*out, 0o755); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	selected := selectWorks(list)
	sort.SliceStable(selected, func(i, j int) bool { return selected[i].sitelinks > selected[j].sitelinks })
	selected = selected[:min(*maxTotal, len(selected))]
	sort.SliceStable(selected, func(i, j int) bool {
		a, b := selected[i], selected[j]
		if a.group != b.group {
			return a.group < b.group
		}
		if a.artist != b.artist {
			return a.artist < b.artist
		}
		return a.sitelinks > b.sitelinks
	})
	fmt.Printf("\nSelected %d images.\n", len(selected))

	if err := run(selected, *out, *dryRun); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
