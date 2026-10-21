package pocketplugin

import (
	"bytes"
	"errors"
	"fmt"
	"path/filepath"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

type tidyCounts struct{ renamed, unchanged, unmatched, conflicts, invalid int }

func (c tidyCounts) summary() string {
	return fmt.Sprintf("Renamed %d; already tidy %d; unmatched %d; conflicts %d; invalid %d.", c.renamed, c.unchanged, c.unmatched, c.conflicts, c.invalid)
}

// TidyFilenames only moves validated originals with a byte-bound ID association.
// BBS metadata fills missing titles; dependency bundles and cache IDs stay intact.
func (s *Session) TidyFilenames() (string, error) {
	var counts tidyCounts
	files, skipped, err := offline.StandaloneFiles(s.pocketRoot)
	if err != nil {
		return counts.summary(), err
	}
	counts.unmatched = skipped
	catalog, err := offline.Catalog()
	if err != nil {
		return counts.summary(), err
	}
	titles := map[string]string{}
	for _, cart := range catalog {
		titles[cart.ID] = cart.Name
	}
	for _, file := range files {
		if err = s.cancel(); err != nil {
			return counts.summary(), err
		}
		data, e := offline.ReadBounded(filepath.Join(s.pocketRoot, offline.CartPath, file.Name), offline.MaxCart)
		if e != nil || offline.ValidatePNG(data) != nil {
			counts.invalid++
			continue
		}
		title, metaErr := offline.CachedTitle(s.hostRoot, file.ID, data)
		if metaErr != nil {
			cached, cacheErr := offline.CachedCart(s.hostRoot, file.ID)
			if cacheErr != nil || !bytes.Equal(cached, data) {
				counts.unmatched++
				continue
			}
			title = titles[file.ID]
			if title == "" {
				if s.Details == nil {
					counts.unmatched++
					continue
				}
				if _, e = bbs.DetailURL(file.ID); e != nil {
					counts.unmatched++
					continue
				}
				if e = s.beforeRequest("BBS metadata #" + file.ID); e != nil {
					return counts.summary(), e
				}
				status, body, retry, fetchErr := s.Details(file.ID)
				s.recordRequest()
				if status == 429 {
					return counts.summary(), httpStatus(status, retry)
				}
				if errors.Is(fetchErr, ErrCancelled) {
					return counts.summary(), fetchErr
				}
				if fetchErr == nil {
					fetchErr = httpStatus(status, retry)
				}
				if fetchErr != nil {
					counts.unmatched++
					s.log("Metadata unavailable for #" + file.ID + ": " + fetchErr.Error())
					continue
				}
				cart, parseErr := bbs.ParseDetail(body, file.ID)
				if parseErr != nil {
					counts.unmatched++
					s.log("Metadata unavailable for #" + file.ID + ": " + parseErr.Error())
					continue
				}
				title = cart.Title
			}
			if e = offline.RememberTitle(s.hostRoot, file.ID, title, data); e != nil {
				return counts.summary(), e
			}
		}
		if err = s.cancel(); err != nil {
			return counts.summary(), err
		}
		result, e := offline.TidyStandalone(s.pocketRoot, file, title, data)
		if e != nil {
			return counts.summary(), e
		}
		switch result {
		case "renamed":
			counts.renamed++
		case "unchanged":
			counts.unchanged++
		case "conflict":
			counts.conflicts++
		}
	}
	return counts.summary(), nil
}
