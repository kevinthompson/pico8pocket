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
	loggedReasons := map[string]bool{}
	skip := func(file offline.StandaloneFile, reason string) {
		counts.skip(reason)
		if !loggedReasons[reason] && len(loggedReasons) < 5 {
			s.log("Skipped " + filepath.ToSlash(filepath.Join(file.Dir, file.Name)) + ": " + reason + ".")
			loggedReasons[reason] = true
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
			verified := cacheErr == nil && bytes.Equal(cached, data)
			title = titles[file.ID]
			if title == "" {
				if !verified && s.Details == nil {
					skip(file, "no saved identity")
					continue
				}
				var reason string
				title, reason, e = s.tidyTitle(file.ID, data)
				if e != nil {
					return counts.summary(), e
				}
				if reason != "" {
					skip(file, reason)
					continue
				}
			}
			if !verified {
				reason, verifyErr := s.verifyTidyCart(file.ID, data)
				if verifyErr != nil {
					return counts.summary(), verifyErr
				}
				if reason != "" {
					skip(file, reason)
					continue
				}
			}
			if e = s.cancel(); e != nil {
				return counts.summary(), e
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
// Metadata resolves noncatalog aliases before they reach this verification step.
// A newer or otherwise different cart is left untouched, including its cache.
func (s *Session) verifyTidyCart(id string, data []byte) (string, error) {
	return s.verifyTidyCandidate(id, data, false)
}

// Interactive jobs must pause on unavailable candidates: a failed request is
// not evidence that the candidate differs, and cannot establish uniqueness.
func (s *Session) verifyTidyCandidate(id string, data []byte, strict bool) (string, error) {
	if _, err := bbs.DetailURL(id); err != nil {
		return "no saved identity", nil
	}
	if cached, err := offline.CachedCart(s.hostRoot, id); err == nil && bytes.Equal(cached, data) {
		return "", nil
	}
	if s.fetch == nil {
		if strict {
			return "", errors.New("cart verification network unavailable")
		}
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
		if strict {
			if fetchErr != nil {
				return "", fetchErr
			}
			if status == 404 {
				return "BBS cart unavailable", nil
			}
			if err := httpStatus(status, retry); err != nil {
				return "", err
			}
			return "", errors.New("BBS verification returned an invalid cartridge")
		}
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

// tidyTitle resolves exact aliases and disambiguates multi-cart posts by bytes.
// A successful HTTP page alone never establishes a cart identity.
func (s *Session) tidyTitle(id string, data []byte) (string, string, error) {
	if s.Details == nil {
		return "", "title unavailable", nil
	}
	if _, err := bbs.DetailURL(id); err != nil {
		return "", "BBS identity unavailable", nil
	}
	if err := s.beforeRequest("BBS metadata #" + id); err != nil {
		return "", "", err
	}
	status, body, retry, fetchErr := s.Details(id)
	s.recordRequest()
	if status == 429 {
		return "", "", httpStatus(status, retry)
	}
	if errors.Is(fetchErr, ErrCancelled) {
		return "", "", fetchErr
	}
	if err := s.cancel(); err != nil {
		return "", "", err
	}
	if fetchErr != nil || httpStatus(status, retry) != nil {
		return "", "title unavailable", nil
	}
	carts, err := bbs.ParseDetails(body, id)
	if err != nil {
		return "", "BBS identity unavailable", nil
	}
	if len(carts) == 1 {
		return carts[0].Title, "", nil
	}
	title, matches := "", 0
	for _, cart := range carts {
		if err := s.cancel(); err != nil {
			return "", "", err
		}
		reason, err := s.verifyTidyCart(cart.Alias, data)
		if err != nil {
			return "", "", err
		}
		if reason == "different BBS cart bytes" {
			continue
		}
		if reason != "" {
			return "", "cart title verification unavailable", nil
		}
		matches++
		title = cart.Title
	}
	if matches == 0 {
		return "", "no matching BBS cart title", nil
	}
	if matches != 1 {
		return "", "ambiguous BBS cart title", nil
	}
	return title, "", nil
}
