package pocketplugin

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

const tidyBatch = 10
const maxTidyState = 1024 * 1024
const maxTidyMatches = 20

// Only paths, fingerprints and public metadata cross prompt boundaries. Cart
// bytes and filesystem roots are always read again from the current host.
type tidyJob struct {
	Files   []offline.StandaloneFile `json:"files"`
	Index   int                      `json:"index"`
	Skipped int                      `json:"skipped"`
	Totals  tidyTotals               `json:"totals"`
	Pending *tidyPending             `json:"pending,omitempty"`
}
type tidyTotals struct {
	Renamed, Unchanged, Unmatched, Conflicts, Invalid int
	Reasons                                           map[string]int
}

func (c tidyTotals) summary() string {
	return (tidyCounts{renamed: c.Renamed, unchanged: c.Unchanged, unmatched: c.Unmatched, conflicts: c.Conflicts, invalid: c.Invalid, reasons: c.Reasons}).summary()
}

type tidyPending struct {
	Hash       string     `json:"hash"`
	Query      string     `json:"query"`
	Mode       string     `json:"mode"`
	Identity   string     `json:"identity,omitempty"`
	Page       int        `json:"page"`
	Loaded     bool       `json:"loaded"`
	Candidates []bbs.Cart `json:"candidates,omitempty"`
	Cursor     int        `json:"cursor"`
	HasNext    bool       `json:"has_next"`
	Checked    []string   `json:"checked,omitempty"`
	Matches    []bbs.Cart `json:"matches,omitempty"`
	Input      string     `json:"input,omitempty"`
	Note       string     `json:"note,omitempty"`
	Retry      bool       `json:"retry,omitempty"`
	Changed    bool       `json:"changed,omitempty"`
	Conflict   bool       `json:"conflict,omitempty"`
	Selected   *bbs.Cart  `json:"selected,omitempty"`
}

func tidyHash(data []byte) string { h := sha256.Sum256(data); return hex.EncodeToString(h[:]) }
func tidyQuery(file offline.StandaloneFile) string {
	title := strings.TrimSuffix(file.Name, ".p8.png")
	if at := strings.LastIndex(title, " ["); at >= 0 && strings.HasSuffix(title, "]") {
		title = title[:at]
	}
	title = strings.NewReplacer("_", " ", "-", " ").Replace(title)
	title = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, title)
	title = strings.Join(strings.Fields(title), " ")
	if title == "" {
		title = file.ID
	}
	chars := []rune(title)
	if len(chars) > bbs.MaxSearch {
		title = string(chars[:bbs.MaxSearch])
	}
	return strings.TrimSpace(title)
}
func (s *Session) startTidy() ([]byte, error) {
	files, skipped, err := offline.StandaloneFiles(s.pocketRoot, s.cancel)
	if err != nil {
		return s.libraryMenu("Stopped: " + err.Error())
	}
	sort.Slice(files, func(i, j int) bool {
		return filepath.Join(files[i].Dir, files[i].Name) < filepath.Join(files[j].Dir, files[j].Name)
	})
	j := &tidyJob{Files: files, Skipped: skipped, Totals: tidyTotals{Unmatched: skipped, Reasons: map[string]int{}}}
	if skipped > 0 {
		j.Totals.Reasons["unrecognized filename or unreadable entry"] = skipped
	}
	encoded, err := json.Marshal(j)
	if err != nil || len(encoded) > maxTidyState/2 {
		return s.libraryMenu("Stopped: cart scan is too large to resume safely.")
	}
	s.tidy = j
	s.browser = nil
	s.screen = "tidy"
	return s.stepTidy()
}
func (s *Session) finishTidy(message string) ([]byte, error) {
	j := s.tidy
	summary := j.Totals.summary()
	if message != "" {
		summary = message + "\n" + summary
	}
	if j.Index < len(j.Files) {
		summary += fmt.Sprintf("\n%d files remain unprocessed.", len(j.Files)-j.Index)
	}
	s.log(summary)
	s.tidy = nil
	return s.libraryMenu(summary)
}
func (s *Session) currentTidyData() ([]byte, error) {
	j := s.tidy
	path, err := j.Files[j.Index].Path(s.pocketRoot)
	if err != nil {
		return nil, err
	}
	data, err := offline.ReadBounded(path, offline.MaxCart)
	if err != nil {
		return nil, err
	}
	if err = offline.ValidatePNG(data); err != nil {
		return nil, err
	}
	if j.Pending != nil && tidyHash(data) != j.Pending.Hash {
		return nil, errors.New("source file changed while awaiting a decision")
	}
	return data, nil
}
func (s *Session) skipTidy(reason string) {
	j := s.tidy
	if j.Pending.Conflict {
		j.Totals.Conflicts++
	} else {
		j.Totals.Unmatched++
		if j.Totals.Reasons == nil {
			j.Totals.Reasons = map[string]int{}
		}
		if j.Totals.Reasons[reason] == 0 && len(j.Totals.Reasons) < 5 {
			file := j.Files[j.Index]
			s.log("Skipped " + filepath.ToSlash(filepath.Join(file.Dir, file.Name)) + ": " + reason + ".")
		}
		j.Totals.Reasons[reason]++
	}
	j.Index++
	j.Pending = nil
}
func (s *Session) publishTidy(c bbs.Cart, data []byte) (bool, error) {
	j := s.tidy
	if err := s.cancel(); err != nil {
		return false, err
	}
	// Save verified metadata first: a metadata write failure must never leave a
	// successfully removed source at the current queue position.
	if err := offline.RememberTitle(s.hostRoot, c.ID, c.Title, data); err != nil {
		return false, err
	}
	if !containsTidyID(j.Pending.Checked, c.ID) {
		j.Pending.Checked = append(j.Pending.Checked, c.ID)
	}
	result, err := offline.TidyStandaloneAs(s.pocketRoot, j.Files[j.Index], c.ID, c.Title, data)
	if err != nil {
		return false, err
	}
	if result == "conflict" {
		j.Pending.Conflict = true
		j.Pending.Selected = &c
		j.Pending.Matches = []bbs.Cart{c}
		j.Pending.Note = "Destination already exists with different bytes. Choose another filename or skip this file."
		return false, nil
	}
	if result == "renamed" {
		j.Totals.Renamed++
	} else {
		j.Totals.Unchanged++
	}
	j.Index++
	j.Pending = nil
	return true, nil
}
func (s *Session) tidyFailure(err error) ([]byte, error) {
	if errors.Is(err, ErrCancelled) {
		return s.finishTidy("Stopped: " + err.Error())
	}
	s.tidy.Pending.Note = "Stopped: " + err.Error() + ". Completed changes are kept."
	s.tidy.Pending.Retry = true
	return s.showTidy()
}
func (s *Session) stepTidy() ([]byte, error) {
	j := s.tidy
	catalog, err := offline.Catalog()
	if err != nil {
		return s.finishTidy("Stopped: " + err.Error())
	}
	titles := map[string]string{}
	for _, c := range catalog {
		titles[c.ID] = c.Name
	}
	for j.Index < len(j.Files) {
		if err = s.cancel(); err != nil {
			return s.finishTidy("Stopped: " + err.Error())
		}
		data, e := s.currentTidyData()
		if e != nil {
			if j.Pending != nil {
				j.Pending.Changed = true
				j.Pending.Note = "Source file changed or is unavailable; it has been left untouched."
				return s.showTidy()
			}
			j.Totals.Invalid++
			j.Index++
			continue
		}
		if j.Pending == nil {
			file := j.Files[j.Index]
			j.Pending = &tidyPending{Hash: tidyHash(data), Query: tidyQuery(file), Mode: "alias", Identity: file.ID, Page: 1}
			if title, e := offline.CachedTitle(s.hostRoot, file.ID, data); e == nil {
				done, e := s.publishTidy(bbs.Cart{ID: file.ID, Title: title}, data)
				if e != nil {
					return s.tidyFailure(e)
				}
				if !done {
					return s.showTidy()
				}
				continue
			}
			if title := titles[file.ID]; title != "" {
				j.Pending.Candidates = []bbs.Cart{{ID: file.ID, Title: title}}
				j.Pending.Loaded = true
			}
		}
		p := j.Pending
		p.Retry = false
		if p.Changed || p.Conflict {
			return s.showTidy()
		}
		budget := tidyBatch
		for {
			if err := s.cancel(); err != nil {
				return s.finishTidy("Stopped: " + err.Error())
			}
			// Continue batches automatically, but recheck the source between
			// batches/pages rather than carrying stale bytes through a long scan.
			data, e = s.currentTidyData()
			if e != nil {
				p.Changed = true
				p.Note = "Source file changed or is unavailable; it has been left untouched."
				return s.showTidy()
			}
			if budget == 0 {
				budget = tidyBatch
			}
			if !p.Loaded {
				if p.Mode == "search" {
					e = s.loadTidySearch()
				} else {
					e = s.loadTidyIdentity(p.Identity)
				}
				if e != nil {
					return s.tidyFailure(e)
				}
			}
			for p.Cursor < len(p.Candidates) && budget > 0 && len(p.Checked) < 512 && len(p.Matches) < maxTidyMatches {
				c := p.Candidates[p.Cursor]
				if containsTidyID(p.Checked, c.ID) {
					p.Cursor++
					continue
				}
				reason, e := s.verifyTidyCandidate(c.ID, data, true)
				if e != nil {
					return s.tidyFailure(e)
				}
				p.Checked = append(p.Checked, c.ID)
				p.Cursor++
				budget--
				if reason == "" && !containsTidyMatch(p.Matches, c.ID) {
					p.Matches = append(p.Matches, c)
				}
			}
			if len(p.Matches) == 1 && p.Cursor == len(p.Candidates) && !(p.Mode == "search" && p.HasNext) {
				done, e := s.publishTidy(p.Matches[0], data)
				if e != nil {
					return s.tidyFailure(e)
				}
				if !done {
					return s.showTidy()
				}
				break
			}
			if len(p.Matches) > 1 {
				p.Note = "Multiple verified identities match this file. Choose which BBS identity to use."
				return s.showTidy()
			}
			if p.Cursor < len(p.Candidates) {
				if len(p.Checked) >= 512 {
					p.Note = "Verification limit reached before all candidates were checked. Refine the search or choose a verified identity."
					return s.showTidy()
				}
				continue
			}
			if p.Mode == "alias" {
				p.Mode = "search"
				p.Loaded = false
				p.Candidates = nil
				p.Cursor = 0
				p.HasNext = false
				continue
			}
			if p.Mode == "search" && p.HasNext {
				if len(p.Checked) >= 512 || p.Page >= bbs.MaxPage {
					p.Note = "Search limit reached before all pages were checked. Refine the search or choose a verified identity."
					return s.showTidy()
				}
				advanceTidyPage(p)
				continue
			}
			p.Note = "No verified byte match was found. Refine the search, enter a BBS ID, or skip."
			return s.showTidy()
		}
	}
	return s.finishTidy("")
}
func (s *Session) loadTidyIdentity(id string) error {
	p := s.tidy.Pending
	p.Candidates = nil
	p.Cursor = 0
	p.Loaded = true
	p.HasNext = false
	if s.Details == nil {
		return nil
	}
	if _, err := bbs.DetailURL(id); err != nil {
		return nil
	}
	if err := s.beforeRequest("BBS metadata #" + id); err != nil {
		p.Loaded = false
		return err
	}
	status, body, retry, err := s.Details(id)
	s.recordRequest()
	if status == 429 {
		p.Loaded = false
		return httpStatus(status, retry)
	}
	if errors.Is(err, ErrCancelled) {
		return err
	}
	if e := s.cancel(); e != nil {
		return e
	}
	if err != nil {
		p.Loaded = false
		return err
	}
	if status == 404 {
		return nil
	}
	if err := httpStatus(status, retry); err != nil {
		p.Loaded = false
		return err
	}
	carts, err := bbs.ParseDetails(body, id)
	if err != nil {
		return nil
	}
	for _, c := range carts {
		key := c.Alias
		if len(carts) == 1 {
			key = id
		}
		p.Candidates = append(p.Candidates, bbs.Cart{ID: key, Title: c.Title, Author: c.Author})
	}
	return nil
}
func advanceTidyPage(p *tidyPending) {
	p.Page++
	p.Loaded = false
	p.Candidates = nil
	p.Cursor = 0
	p.HasNext = false
}

func (s *Session) loadTidySearch() error {
	p := s.tidy.Pending
	p.Candidates = nil
	p.Cursor = 0
	p.HasNext = false
	if s.List == nil {
		p.Loaded = true
		return nil
	}
	if err := s.beforeRequest("BBS title search: " + p.Query); err != nil {
		return err
	}
	status, body, retry, err := s.List(bbs.Recent, p.Page, p.Query)
	s.recordRequest()
	if err != nil {
		return err
	}
	if err = httpStatus(status, retry); err != nil {
		return err
	}
	if err = s.cancel(); err != nil {
		return err
	}
	page, err := bbs.Parse(body, p.Page)
	if err != nil {
		return err
	}
	p.Candidates = page.Carts
	// The browser parser suppresses Next at MaxPage. At that ceiling, keep
	// coverage unresolved rather than treating the policy limit as exhaustion.
	p.HasNext = page.HasNext || p.Page == bbs.MaxPage
	p.Loaded = true
	return nil
}
func containsTidyID(ids []string, id string) bool {
	for _, v := range ids {
		if v == id {
			return true
		}
	}
	return false
}
func containsTidyMatch(carts []bbs.Cart, id string) bool {
	for _, v := range carts {
		if v.ID == id {
			return true
		}
	}
	return false
}
func tidyUse(c bbs.Cart) string { return "Use " + cartChoice(c) }
func tidyMore(p *tidyPending) bool {
	return len(p.Matches) < maxTidyMatches && len(p.Checked) < 512 && (p.Cursor < len(p.Candidates) || !p.Loaded || (p.Mode == "search" && p.HasNext && p.Page < bbs.MaxPage))
}
func (s *Session) showTidy() ([]byte, error) {
	j, p := s.tidy, s.tidy.Pending
	choices := []string{}
	if !p.Changed {
		for _, c := range p.Matches {
			choices = append(choices, tidyUse(c))
		}
		if p.Conflict {
			choices = append(choices, "Choose another filename")
		}
		if p.Retry {
			choices = append(choices, "Retry")
		}
		choices = append(choices, "Edit search", "Enter BBS ID")
		if !p.Retry && !p.Conflict && tidyMore(p) {
			choices = append(choices, "Check more results")
		}
	}
	choices = append(choices, "Skip file", "Stop")
	p.Input = ""
	s.screen = "tidy"
	query := fmt.Sprintf("File %d/%d: %s\n%s\nSearch: %s. Checked %d identities; %d verified matches.\n%s", j.Index+1, len(j.Files), filepath.ToSlash(filepath.Join(j.Files[j.Index].Dir, j.Files[j.Index].Name)), p.Note, p.Query, len(p.Checked), len(p.Matches), j.Totals.summary())
	return s.prompt(query, choices)
}
func (s *Session) tidyText(kind string) ([]byte, error) {
	p := s.tidy.Pending
	p.Input = kind
	query := "Enter a BBS title to search. Blank returns to the resolution options."
	if kind == "id" {
		query = "Enter a BBS post ID or cart alias. Only matching cart bytes can be used. Blank returns to options."
	}
	if kind == "filename" {
		query = "Enter a different title for this verified cart. Its BBS ID stays intact. Blank returns to options."
	}
	s.searchInput = true
	s.sequence++
	s.name = fmt.Sprintf("pico8-offline-%d", s.sequence)
	s.choices = nil
	return json.Marshal(map[string]any{"Text": map[string]string{"name": s.name, "query": query}})
}
func (s *Session) handleTidyText(value string) ([]byte, error) {
	p := s.tidy.Pending
	kind := p.Input
	if strings.TrimSpace(value) == "" {
		return s.showTidy()
	}
	value, err := bbs.NormalizeSearch(value)
	if err != nil {
		p.Note = err.Error()
		return s.showTidy()
	}
	if kind == "filename" {
		if p.Selected == nil {
			return nil, errors.New("missing verified filename selection")
		}
		c := *p.Selected
		c.Title = value
		return s.applyTidyChoice(c)
	}
	if kind == "id" {
		if _, err := bbs.DetailURL(value); err != nil {
			p.Note = err.Error()
			return s.showTidy()
		}
	}
	p.Mode = "search"
	p.Page = 1
	p.Candidates = nil
	p.Cursor = 0
	p.Checked = nil
	p.Matches = nil
	p.Loaded = false
	p.HasNext = false
	p.Conflict = false
	p.Selected = nil
	p.Input = ""
	if kind == "id" {
		p.Mode = "manual"
		p.Identity = value
	} else {
		p.Query = value
	}
	return s.stepTidy()
}
func (s *Session) applyTidyChoice(c bbs.Cart) ([]byte, error) {
	p := s.tidy.Pending
	data, err := s.currentTidyData()
	if err != nil {
		p.Changed = true
		p.Note = "Source file changed or is unavailable; it has been left untouched."
		return s.showTidy()
	}
	reason, err := s.verifyTidyCandidate(c.ID, data, true)
	if err != nil {
		return s.tidyFailure(err)
	}
	if reason != "" {
		p.Note = "Verified copy could not be rechecked: " + reason
		return s.showTidy()
	}
	done, err := s.publishTidy(c, data)
	if err != nil {
		return s.tidyFailure(err)
	}
	if !done {
		return s.showTidy()
	}
	return s.stepTidy()
}
func (s *Session) handleTidyChoice(value string) ([]byte, error) {
	j, p := s.tidy, s.tidy.Pending
	switch value {
	case "Stop":
		return s.finishTidy("Stopped. Completed changes are kept.")
	case "Skip file":
		reason := "no verified match"
		if p.Changed {
			reason = "source changed"
		}
		s.skipTidy(reason)
		return s.stepTidy()
	case "Edit search":
		return s.tidyText("search")
	case "Enter BBS ID":
		return s.tidyText("id")
	case "Choose another filename":
		return s.tidyText("filename")
	case "Retry":
		if p.Selected != nil {
			return s.applyTidyChoice(*p.Selected)
		}
		return s.stepTidy()
	case "Check more results":
		if p.Cursor >= len(p.Candidates) && p.Loaded && p.Mode == "search" && p.HasNext {
			advanceTidyPage(p)
		}
		return s.stepTidy()
	}
	for _, c := range j.Pending.Matches {
		if value == tidyUse(c) {
			return s.applyTidyChoice(c)
		}
	}
	return nil, errors.New("invalid tidy action")
}

func validTidyCart(c bbs.Cart) bool {
	if _, err := bbs.DetailURL(c.ID); err != nil {
		return false
	}
	if !utf8.ValidString(c.Title) || strings.TrimSpace(c.Title) == "" || utf8.RuneCountInString(c.Title) > 200 || !utf8.ValidString(c.Author) || utf8.RuneCountInString(c.Author) > 80 {
		return false
	}
	for _, v := range c.Title + c.Author {
		if unicode.IsControl(v) {
			return false
		}
	}
	return true
}
func validateTidy(j *tidyJob) error {
	bad := func() error { return errors.New("invalid saved tidy job") }
	if j == nil {
		return nil
	}
	if len(j.Files) == 0 || len(j.Files) > 4096 || j.Skipped < 0 || j.Skipped+len(j.Files) > 4096 || j.Index < 0 || j.Index >= len(j.Files) || j.Pending == nil {
		return bad()
	}
	seen := map[string]bool{}
	for _, f := range j.Files {
		if f.Validate() != nil {
			return bad()
		}
		key := filepath.ToSlash(filepath.Join(f.Dir, f.Name))
		if seen[key] {
			return bad()
		}
		seen[key] = true
	}
	c := j.Totals
	if c.Renamed < 0 || c.Unchanged < 0 || c.Unmatched < 0 || c.Conflicts < 0 || c.Invalid < 0 || c.Renamed+c.Unchanged+c.Unmatched+c.Conflicts+c.Invalid != j.Index+j.Skipped || len(c.Reasons) > 16 {
		return bad()
	}
	sum := 0
	for reason, count := range c.Reasons {
		if len(reason) > 100 || count < 1 {
			return bad()
		}
		sum += count
	}
	if sum != c.Unmatched {
		return bad()
	}
	p := j.Pending
	hash, err := hex.DecodeString(p.Hash)
	if err != nil || len(hash) != 32 {
		return bad()
	}
	if q, err := bbs.NormalizeSearch(p.Query); err != nil || q != p.Query || q == "" {
		return bad()
	}
	if p.Mode != "alias" && p.Mode != "search" && p.Mode != "manual" {
		return bad()
	}
	if offline.ValidateID(p.Identity) != nil {
		return bad()
	}
	if p.Mode == "manual" {
		if _, err := bbs.DetailURL(p.Identity); err != nil {
			return bad()
		}
	}
	if !p.Loaded && (len(p.Candidates) != 0 || p.Cursor != 0 || p.HasNext) {
		return bad()
	}
	if p.Page < 1 || p.Page > bbs.MaxPage || len(p.Candidates) > 60 || p.Cursor < 0 || p.Cursor > len(p.Candidates) || len(p.Checked) > 512 || len(p.Matches) > maxTidyMatches || len(p.Note) > 2048 {
		return bad()
	}
	if p.Input != "" && p.Input != "search" && p.Input != "id" && p.Input != "filename" {
		return bad()
	}
	seen = map[string]bool{}
	for _, id := range p.Checked {
		if _, e := bbs.DetailURL(id); e != nil || seen[id] {
			return bad()
		}
		seen[id] = true
	}
	candidateIDs := map[string]bool{}
	for _, c := range p.Candidates {
		if !validTidyCart(c) || candidateIDs[c.ID] {
			return bad()
		}
		candidateIDs[c.ID] = true
	}
	seen = map[string]bool{}
	for _, c := range p.Matches {
		if !validTidyCart(c) || seen[c.ID] || !containsTidyID(p.Checked, c.ID) {
			return bad()
		}
		seen[c.ID] = true
	}
	if p.Conflict != (p.Selected != nil) || p.Input == "filename" && !p.Conflict {
		return bad()
	}
	if p.Selected != nil && (!validTidyCart(*p.Selected) || !containsTidyMatch(p.Matches, p.Selected.ID)) {
		return bad()
	}
	return nil
}
