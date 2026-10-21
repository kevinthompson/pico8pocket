package pocketplugin

import (
	"bytes"
	"errors"
	"fmt"
	"path/filepath"
	"sort"
	"strings"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

type tidyCounts struct {
	renamed, unchanged, unmatched, conflicts, invalid int
	reasons                                           map[string]int
}

func (c tidyCounts) summary() string {
	summary := fmt.Sprintf("Renamed %d; already tidy %d; unmatched %d; conflicts %d; invalid %d.", c.renamed, c.unchanged, c.unmatched, c.conflicts, c.invalid)
	var reasons []string
	for reason, count := range c.reasons {
		reasons = append(reasons, fmt.Sprintf("%s: %d", reason, count))
	}
	sort.Strings(reasons)
	if len(reasons) > 0 {
		summary += "\nUnmatched reasons — " + strings.Join(reasons, "; ") + "."
	}
	return summary
}

func (c *tidyCounts) skip(reason string) {
	c.unmatched++
	if c.reasons == nil {
		c.reasons = map[string]int{}
	}
	c.reasons[reason]++
}

// TidyFilenames only moves validated originals with a byte-bound ID association.
// BBS metadata fills missing titles; dependency bundles and cache IDs stay intact.
func (s *Session) TidyFilenames() (string, error) {
	var counts tidyCounts
	files, skipped, err := offline.StandaloneFiles(s.pocketRoot, s.cancel)
	if err != nil {
		return counts.summary(), err
	}
	counts.unmatched = skipped
	if skipped > 0 {
		counts.reasons = map[string]int{"unrecognized filename or unreadable entry": skipped}
	}
	loggedSkips := 0
	skip := func(file offline.StandaloneFile, reason string) {
		counts.skip(reason)
		if loggedSkips < 5 {
			s.log("Skipped " + filepath.ToSlash(filepath.Join(file.Dir, file.Name)) + ": " + reason + ".")
			loggedSkips++
		}
	}
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
		source, e := file.Path(s.pocketRoot)
		if e != nil {
			return counts.summary(), e
		}
		data, e := offline.ReadBounded(source, offline.MaxCart)
		if e != nil || offline.ValidatePNG(data) != nil {
			counts.invalid++
			continue
		}
		title, metaErr := offline.CachedTitle(s.hostRoot, file.ID, data)
		if metaErr != nil {
			cached, cacheErr := offline.CachedCart(s.hostRoot, file.ID)
			if cacheErr != nil || !bytes.Equal(cached, data) {
				reason, verifyErr := s.verifyTidyCart(file.ID, titles[file.ID] != "", data)
				if verifyErr != nil {
					return counts.summary(), verifyErr
				}
				if reason != "" {
					skip(file, reason)
					continue
				}
			}
			title = titles[file.ID]
			if title == "" {
				if s.Details == nil {
					skip(file, "title unavailable")
					continue
				}
				if _, e = bbs.DetailURL(file.ID); e != nil {
					skip(file, "title unavailable")
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
					skip(file, "title unavailable")
					continue
				}
				cart, parseErr := bbs.ParseDetail(body, file.ID)
				if parseErr != nil {
					skip(file, "title unavailable")
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

// verifyTidyCart establishes identity without trusting the filename alone.
// Only numeric BBS IDs and curated catalog aliases are eligible for a request.
// A newer or otherwise different cart is left untouched, including its cache.
func (s *Session) verifyTidyCart(id string, catalogued bool, data []byte) (string, error) {
	if _, err := bbs.DetailURL(id); !catalogued && err != nil {
		return "no saved identity", nil
	}
	if s.fetch == nil {
		return "no saved identity (network unavailable)", nil
	}
	if err := s.beforeRequest("verify cart #" + id); err != nil {
		return "", err
	}
	status, body, retry, fetchErr := s.fetch(id)
	s.recordRequest()
	if status == 429 {
		return "", httpStatus(status, retry)
	}
	if errors.Is(fetchErr, ErrCancelled) {
		return "", fetchErr
	}
	if err := s.cancel(); err != nil {
		return "", err
	}
	if fetchErr != nil || httpStatus(status, retry) != nil || offline.ValidatePNG(body) != nil {
		return "BBS verification unavailable", nil
	}
	if !bytes.Equal(body, data) {
		return "different BBS cart bytes", nil
	}
	if err := offline.AtomicWrite(offline.CachePath(s.hostRoot, id), body); err != nil {
		return "", err
	}
	return "", nil
}
