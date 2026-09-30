# art-fetcher

Downloads public-domain, portrait-orientation artworks from Wikidata and Wikimedia Commons into `tools/art-fetcher/downloads`.

Run from the repository root:

```bash
go run ./tools/art-fetcher -dry-run          # list what would be downloaded
go run ./tools/art-fetcher                   # download up to 200 images
go run ./tools/art-fetcher -max-total 60
go run ./tools/art-fetcher -only "Klimt,Mucha"
```

- Portrait only (height/width 1.15–1.85), artists who died in or before 1955.
- Existing files are skipped (matched via `downloads/manifest.csv` or by name).
- 429 and 5xx responses are retried with exponential backoff.
- Edit `artists` in `main.go` to change the selection.
