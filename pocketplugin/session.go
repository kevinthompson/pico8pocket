// Package pocketplugin implements the Pocket Sync offline collection conversation.
package pocketplugin

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

// FetchFunc retrieves a validated cart ID supplied by the collection installer or browser.
type FetchFunc func(id string) (status int, body []byte, retryAfter string, err error)

// ErrCancelled lets native hosts interrupt preparation without publishing a bundle.
var ErrCancelled = errors.New("installation cancelled")

// Session owns one conversation and roots supplied by the host, never by answers.
type Session struct {
	Details              FetchFunc
	List                 ListingFetchFunc
	OpenURL              func(string) error
	browser              *browserState
	screen               string
	collection           string
	searchInput          bool
	pocketRoot, hostRoot string
	logger               func(string)
	fetch                FetchFunc
	started, exited      bool
	sequence             uint64
	name                 string
	choices              []string
	// Sleep is invoked only between actual network requests. A failing sleep cancels
	// preparation; the default uses short WASI polls so host cancellation is prompt.
	Sleep func(time.Duration) error
	// Now supplies the request clock. Hosts and tests may replace it together with Sleep.
	Now func() time.Time
	// LastRequest survives separate actions. WASI restores it from an independent
	// Extism variable so even cancellation before conversation persistence retains it.
	LastRequest time.Time
	// OnRequest persists LastRequest immediately before Fetch for cancellation,
	// then again after Fetch completes to start the full courtesy cooldown.
	OnRequest func(time.Time)
	// CheckCancel may interrupt between cache reads, standalone writes and publication.
	CheckCancel func() error
}

func New(pocketRoot, hostRoot string, logger func(string), fetch FetchFunc) *Session {
	return &Session{pocketRoot: pocketRoot, hostRoot: hostRoot, logger: logger, fetch: fetch, Sleep: pollSleep, Now: time.Now}
}
func pollSleep(duration time.Duration) error {
	deadline := time.Now().Add(duration)
	for {
		left := time.Until(deadline)
		if left <= 0 {
			return nil
		}
		if left > 100*time.Millisecond {
			left = 100 * time.Millisecond
		}
		time.Sleep(left)
	}
}
func (s *Session) Start() ([]byte, error) {
	if s.started {
		return nil, errors.New("session already started")
	}
	s.started = true
	return s.menu("Pico8Pocket")
}
func (s *Session) menu(query string) ([]byte, error) {
	s.browser = nil
	s.screen, s.collection = "home", ""
	return s.prompt(query, []string{"Browse BBS", "Collections", "Library", "Exit"})
}
func (s *Session) browseMenu(query string) ([]byte, error) {
	s.browser = nil
	s.screen, s.collection = "browse", ""
	return s.prompt(query, []string{"Recent", "Popular", "Search", "Back"})
}
func (s *Session) libraryMenu(query string) ([]byte, error) {
	s.browser = nil
	s.screen, s.collection = "library", ""
	return s.prompt(query, []string{"Status", "Tidy filenames", "Back"})
}
func (s *Session) collectionsMenu() ([]byte, error) {
	s.screen, s.collection = "collections", ""
	return s.prompt("Choose a collection.", []string{"Pico Arcade", "Sorcerer", "Both collections", "Back"})
}
func (s *Session) collectionMenu() ([]byte, error) {
	s.screen = "collection"
	label := map[string]string{"both": "Both collections", "arcade": "Pico Arcade", "sorcerer": "Sorcerer"}[s.collection]
	return s.prompt(label+". Install reuses verified cache. Refresh downloads retrieves fresh copies.", []string{"Install", "Refresh downloads", "Back"})
}
func (s *Session) prompt(query string, choices []string) ([]byte, error) {
	s.searchInput = false
	s.sequence++
	s.name = fmt.Sprintf("pico8-offline-%d", s.sequence)
	s.choices = choices
	return json.Marshal(map[string]any{"Choice": map[string]any{"name": s.name, "query": query, "choices": choices}})
}
func (s *Session) searchPrompt(message string) ([]byte, error) {
	s.browser = nil
	s.screen, s.collection = "browse", ""
	s.searchInput = true
	s.sequence++
	s.name = fmt.Sprintf("pico8-offline-%d", s.sequence)
	s.choices = nil
	query := fmt.Sprintf("Search public BBS carts (up to %d characters). Submit blank to go back.", bbs.MaxSearch)
	if message != "" {
		query = message + "\n" + query
	}
	return json.Marshal(map[string]any{"Text": map[string]string{"name": s.name, "query": query}})
}
func (s *Session) Handle(input []byte) ([]byte, error) {
	if !s.started || s.exited {
		return nil, errors.New("session is not active")
	}
	name, value, kill, err := parseResponse(input)
	if err != nil {
		return nil, err
	}
	if kill {
		return s.finish()
	}
	if name != s.name {
		return nil, errors.New("answer does not match the current prompt")
	}
	if s.searchInput {
		query, err := bbs.NormalizeSearch(value)
		if err != nil {
			return s.searchPrompt(err.Error())
		}
		if query == "" {
			return s.browseMenu("Browse public BBS carts.")
		}
		return s.loadListing(bbs.Recent, 1, query)
	}
	offered := false
	for _, v := range s.choices {
		if value == v {
			offered = true
			break
		}
	}
	if !offered {
		return nil, errors.New("answer was not an offered choice")
	}
	if value == "Exit" {
		return s.finish()
	}
	if s.browser != nil {
		return s.handleBrowser(value)
	}
	switch s.screen {
	case "home":
		switch value {
		case "Browse BBS":
			return s.browseMenu("Browse public BBS carts.")
		case "Collections":
			return s.collectionsMenu()
		case "Library":
			return s.libraryMenu("Manage installed carts. Tidy uses saved titles or BBS metadata; files without a verified match are skipped.")
		}
	case "library":
		switch value {
		case "Back":
			return s.menu("Pico8Pocket")
		case "Tidy filenames":
			summary, err := s.TidyFilenames()
			if err != nil {
				summary += "\nStopped: " + err.Error()
			}
			s.log(summary)
			return s.libraryMenu(summary)
		case "Status":
			summary, err := s.Status()
			if err != nil {
				summary = "Stopped: " + err.Error()
			}
			s.log(summary)
			return s.libraryMenu(summary)
		}
	case "browse":
		switch value {
		case "Recent":
			return s.loadListing(bbs.Recent, 1, "")
		case "Popular":
			return s.loadListing(bbs.Popular, 1, "")
		case "Search":
			return s.searchPrompt("")
		case "Back":
			return s.menu("Pico8Pocket")
		}
	case "collections":
		switch value {
		case "Back":
			return s.menu("Pico8Pocket")
		case "Pico Arcade":
			s.collection = "arcade"
		case "Sorcerer":
			s.collection = "sorcerer"
		case "Both collections":
			s.collection = "both"
		}
		return s.collectionMenu()
	case "collection":
		if value == "Back" {
			return s.collectionsMenu()
		}
		summary, err := s.Install(s.collection, value == "Refresh downloads")
		if err != nil {
			summary = "Stopped: " + err.Error() + ". The last published bundle is preserved. Retry Install to reuse verified downloads."
		}
		s.log(summary)
		return s.menu(summary)
	}
	return nil, errors.New("invalid menu action")
}
func (s *Session) finish() ([]byte, error) {
	s.exited = true
	s.name = ""
	s.choices = nil
	return []byte(`"Exit"`), nil
}
func (s *Session) log(message string) {
	if s.logger != nil {
		s.logger(message)
	}
}
func (s *Session) cancel() error {
	if s.CheckCancel != nil {
		return s.CheckCancel()
	}
	return nil
}

func (s *Session) existingPack() (offline.Carts, error) {
	path := filepath.Join(s.pocketRoot, offline.PackPath)
	data, err := offline.ReadBounded(path, offline.MaxPack)
	if os.IsNotExist(err) {
		return make(offline.Carts), nil
	}
	if err != nil {
		return nil, err
	}
	carts, err := offline.DecodePack(data)
	if err != nil {
		return nil, fmt.Errorf("existing bundle invalid: %w", err)
	}
	return carts, nil
}

// Status reports verified cache and bundle counts for each curated collection.
func (s *Session) Status() (string, error) {
	catalog, err := offline.Catalog()
	if err != nil {
		return "", err
	}
	installed, packErr := s.existingPack()
	var lines []string
	for _, group := range []string{"arcade", "sorcerer"} {
		total, cached, present := 0, 0, 0
		for _, cart := range catalog {
			if cart.Collection != group {
				continue
			}
			total++
			if _, ok := installed[cart.ID]; ok {
				present++
			}
			if _, e := offline.CachedCart(s.hostRoot, cart.ID); e == nil {
				cached++
			}
		}
		lines = append(lines, fmt.Sprintf("%s: installed %d/%d, verified cache %d/%d", group, present, total, cached, total))
	}
	summary := strings.Join(lines, "; ") + ". Install reuses verified cache; Refresh downloads retrieves fresh copies."
	if packErr != nil {
		summary += " " + packErr.Error() + "."
	}
	return summary, nil
}

// Install prepares a complete selected set, writes standalone carts, then atomically
// merges and publishes the bundle. A nil fetch supports explicit cache-only use.
func (s *Session) Install(collection string, refresh bool) (string, error) {
	if collection != "both" && collection != "arcade" && collection != "sorcerer" {
		return "", errors.New("collection must be arcade, sorcerer or both")
	}
	catalog, err := offline.Catalog()
	if err != nil {
		return "", err
	}
	selected := make([]offline.Cart, 0, len(catalog))
	for _, cart := range catalog {
		if collection == "both" || collection == cart.Collection {
			selected = append(selected, cart)
		}
	}
	// Reject a malformed published bundle before modifying its selected standalone files.
	carts, err := s.existingPack()
	if err != nil {
		return "", err
	}
	s.log("Installing " + collection + " collection(s).")
	var failures []string
	for i, cart := range selected {
		if err = s.cancel(); err != nil {
			return "", err
		}
		s.log(fmt.Sprintf("Checking %s (%d/%d).", cart.ID, i+1, len(selected)))
		if !refresh {
			if _, e := offline.CachedCart(s.hostRoot, cart.ID); e == nil {
				continue
			}
		}
		if s.fetch == nil {
			failures = append(failures, cart.ID+": verified cache unavailable; network disabled")
			continue
		}
		if err = s.beforeRequest(cart.ID); err != nil {
			return "", err
		}
		status, data, retryAfter, fetchErr := s.fetch(cart.ID)
		// Start the cooldown after the response. Persisting only the earlier
		// reservation would include host-call setup time in the measured gap.
		s.recordRequest()
		if status == 429 {
			if retryAfter == "" {
				retryAfter = "not supplied"
			}
			return "", fmt.Errorf("%s: HTTP 429; Retry-After: %s; no automatic retry; wait before choosing Install", cart.ID, retryAfter)
		}
		if errors.Is(fetchErr, ErrCancelled) {
			return "", fetchErr
		}
		if fetchErr == nil && status != 200 {
			fetchErr = fmt.Errorf("HTTP %d", status)
		}
		if fetchErr == nil {
			fetchErr = offline.ValidatePNG(data)
		}
		if fetchErr == nil {
			if err = s.cancel(); err != nil {
				return "", err
			}
		}
		if fetchErr == nil {
			fetchErr = offline.AtomicWrite(offline.CachePath(s.hostRoot, cart.ID), data)
		}
		if fetchErr != nil {
			if errors.Is(fetchErr, ErrCancelled) {
				return "", fetchErr
			}
			failures = append(failures, cart.ID+": "+fetchErr.Error())
		}
	}
	if len(failures) > 0 {
		return "", fmt.Errorf("download failed: %s", strings.Join(failures, "; "))
	}
	for i, cart := range selected {
		if err = s.cancel(); err != nil {
			return "", err
		}
		data, e := offline.CachedCart(s.hostRoot, cart.ID)
		if e != nil {
			return "", fmt.Errorf("%s: %w", cart.ID, e)
		}
		if _, err = offline.PublishStandalone(s.pocketRoot, s.hostRoot, cart.ID, cart.Name, data); err != nil {
			return "", err
		}
		carts[cart.ID] = data
		s.log(fmt.Sprintf("Prepared standalone carts %d/%d.", i+1, len(selected)))
	}
	data, err := offline.EncodePack(carts)
	if err != nil {
		return "", err
	}
	if err = s.cancel(); err != nil {
		return "", err
	}
	if err = offline.AtomicWrite(filepath.Join(s.pocketRoot, offline.PackPath), data); err != nil {
		return "", err
	}
	return fmt.Sprintf("Installed %d selected cartridges; bundle contains %d total.", len(selected), len(carts)), nil
}

// conversationState is saved in Extism variables, which survive guest reinitialization.
// Paths and executable callbacks always come from the host's new Session.
type conversationState struct {
	Started     bool          `json:"started"`
	Exited      bool          `json:"exited"`
	Sequence    uint64        `json:"sequence"`
	Name        string        `json:"name"`
	Choices     []string      `json:"choices"`
	LastRequest int64         `json:"last_request,omitempty"`
	Browser     *browserState `json:"browser,omitempty"`
	Screen      string        `json:"screen"`
	Collection  string        `json:"collection,omitempty"`
	SearchInput bool          `json:"search_input,omitempty"`
}

func (s *Session) Snapshot() ([]byte, error) {
	var stamp int64
	if !s.LastRequest.IsZero() {
		stamp = s.LastRequest.UnixNano()
	}
	return json.Marshal(conversationState{s.started, s.exited, s.sequence, s.name, s.choices, stamp, s.browser, s.screen, s.collection, s.searchInput})
}
func (s *Session) Restore(data []byte) error {
	var state conversationState
	if err := json.Unmarshal(data, &state); err != nil {
		return err
	}
	if !state.Started || state.Sequence == 0 || (!state.Exited && (state.Name == "" || (len(state.Choices) == 0 && !state.SearchInput))) {
		return errors.New("invalid saved conversation")
	}
	if state.Screen != "home" && state.Screen != "library" && state.Screen != "browse" && state.Screen != "collections" && state.Screen != "collection" {
		return errors.New("invalid saved menu")
	}
	if state.Screen == "collection" && state.Collection != "both" && state.Collection != "arcade" && state.Collection != "sorcerer" {
		return errors.New("invalid saved collection")
	}
	if state.SearchInput && (state.Screen != "browse" || state.Browser != nil || len(state.Choices) != 0) {
		return errors.New("invalid saved text input")
	}
	if state.Browser != nil && state.Screen != "browse" {
		return errors.New("invalid saved browser menu")
	}
	s.screen, s.collection, s.searchInput = state.Screen, state.Collection, state.SearchInput
	if err := validateBrowser(state.Browser); err != nil {
		return err
	}
	s.browser = state.Browser
	s.started = state.Started
	s.exited = state.Exited
	s.sequence = state.Sequence
	s.name = state.Name
	s.choices = state.Choices
	if state.LastRequest != 0 {
		saved := time.Unix(0, state.LastRequest)
		if saved.After(s.LastRequest) {
			s.LastRequest = saved
		}
	}
	return nil
}

// beforeRequest gates network calls across menu actions without delaying cache use.
func (s *Session) beforeRequest(id string) error {
	now := s.Now
	if now == nil {
		now = time.Now
	}
	if !s.LastRequest.IsZero() {
		elapsed := now().Sub(s.LastRequest)
		if elapsed < 0 {
			elapsed = 0
		}
		remaining := 15*time.Second - elapsed
		if remaining > 0 {
			s.log("Waiting between requests to respect Lexaloffle rate limits.")
			sleep := s.Sleep
			if sleep == nil {
				sleep = pollSleep
			}
			if err := sleep(remaining); err != nil {
				return err
			}
		}
	}
	if err := s.cancel(); err != nil {
		return err
	}
	s.log("Requesting " + id + "...")
	s.recordRequest()
	return nil
}

func (s *Session) recordRequest() {
	now := s.Now
	if now == nil {
		now = time.Now
	}
	s.LastRequest = now()
	if s.OnRequest != nil {
		s.OnRequest(s.LastRequest)
	}
}
