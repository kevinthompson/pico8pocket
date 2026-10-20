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

	"github.com/zenibako/pico8pocket/offline"
)

// FetchFunc retrieves only the catalog ID supplied by the installer.
type FetchFunc func(id string) (status int, body []byte, retryAfter string, err error)

// ErrCancelled lets native hosts interrupt preparation without publishing a bundle.
var ErrCancelled = errors.New("installation cancelled")

// Session owns one conversation and roots supplied by the host, never by answers.
type Session struct {
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
	return s.menu("Pocket Sync: offline Pico Arcade and Sorcerer collections. Install uses verified cache; Update refreshes downloads.")
}
func (s *Session) menu(query string) ([]byte, error) {
	s.sequence++
	s.name = fmt.Sprintf("pico8-offline-%d", s.sequence)
	s.choices = []string{"Install both collections", "Install Arcade", "Install Sorcerer", "Update both collections", "Update Arcade", "Update Sorcerer", "Installed / cache status", "Exit"}
	return json.Marshal(struct {
		Choice struct {
			Name    string   `json:"name"`
			Query   string   `json:"query"`
			Choices []string `json:"choices"`
		} `json:"Choice"`
	}{struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	}{s.name, query, s.choices}})
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
		return nil, errors.New("answer does not match the current choice")
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
	var summary string
	if value == "Installed / cache status" {
		summary, err = s.Status()
	} else {
		collection := "both"
		if strings.HasSuffix(value, "Arcade") {
			collection = "arcade"
		}
		if strings.HasSuffix(value, "Sorcerer") {
			collection = "sorcerer"
		}
		summary, err = s.Install(collection, strings.HasPrefix(value, "Update"))
	}
	if err != nil {
		summary = "Stopped: " + err.Error() + ". The last published bundle is preserved. Retry Install to reuse verified downloads."
	}
	s.log(summary)
	return s.menu(summary)
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
	summary := strings.Join(lines, "; ") + ". Install reuses verified cache; Update downloads fresh copies."
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
		if err = offline.AtomicWrite(filepath.Join(s.pocketRoot, offline.CartPath, cart.ID+".p8.png"), data); err != nil {
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
	Started     bool     `json:"started"`
	Exited      bool     `json:"exited"`
	Sequence    uint64   `json:"sequence"`
	Name        string   `json:"name"`
	Choices     []string `json:"choices"`
	LastRequest int64    `json:"last_request,omitempty"`
}

func (s *Session) Snapshot() ([]byte, error) {
	var stamp int64
	if !s.LastRequest.IsZero() {
		stamp = s.LastRequest.UnixNano()
	}
	return json.Marshal(conversationState{s.started, s.exited, s.sequence, s.name, s.choices, stamp})
}
func (s *Session) Restore(data []byte) error {
	var state conversationState
	if err := json.Unmarshal(data, &state); err != nil {
		return err
	}
	if !state.Started || state.Sequence == 0 || (!state.Exited && (state.Name == "" || len(state.Choices) == 0)) {
		return errors.New("invalid saved conversation")
	}
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
			s.log("Waiting between downloads to respect Lexaloffle rate limits.")
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
	s.log("Downloading #" + id + "...")
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
