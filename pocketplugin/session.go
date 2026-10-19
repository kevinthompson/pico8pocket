// Package pocketplugin implements the Pocket Sync conversation for cart artwork.
package pocketplugin

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"

	"github.com/zenibako/pico8pocket/thumbnail"
)

const (
	pilot    = "Generate one test thumbnail"
	all      = "Generate all thumbnails"
	help     = "Help"
	back     = "Back"
	exit     = "Exit"
	reminder = "Native picker artwork is unverified; test on Pocket."
)

// Session owns one conversation. Its root is supplied by the host, never input.
type Session struct {
	root            string
	logger          func(string)
	started, exited bool
	sequence        uint64
	name            string
	choices         []string
}

// New creates a conversation rooted at the host-provided SD card.
func New(root string, logger func(string)) *Session { return &Session{root: root, logger: logger} }

// Start opens a conversation once; subsequent starts are rejected.
func (s *Session) Start() ([]byte, error) {
	if s.started {
		return nil, errors.New("session already started")
	}
	s.started = true
	return s.menu()
}

func (s *Session) menu() ([]byte, error) {
	return s.choice("PICO-8 cart thumbnails", []string{pilot, all, help, exit})
}

func (s *Session) choice(query string, choices []string) ([]byte, error) {
	s.sequence++
	s.name = fmt.Sprintf("pico8-thumbnails-%d", s.sequence)
	s.choices = choices
	type choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	}
	return json.Marshal(struct {
		Choice choice `json:"Choice"`
	}{choice{s.name, query, choices}})
}

// Handle validates the current answer before generating artwork or navigating.
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
	switch value {
	case exit:
		return s.finish()
	case back:
		return s.menu()
	case help:
		return s.choice("Copy carts to Assets/pico8/common/cards. Generate thumbnails, then test the native picker. "+reminder, []string{back, exit})
	case pilot, all:
		limit := 0
		if value == pilot {
			limit = 1
		}
		report, err := thumbnail.Generate(s.root, limit)
		if err != nil {
			s.log("Thumbnail generation failed: " + err.Error())
			return s.choice("Generation failed; check the card and cart folder. "+reminder, []string{back, exit})
		}
		summary := fmt.Sprintf("Generated %d, unchanged %d, skipped %d.", report.Generated, report.Unchanged, report.Skipped)
		if len(report.Warnings) > 0 {
			summary += fmt.Sprintf(" Warnings: %d.", len(report.Warnings))
		}
		s.log(summary)
		return s.choice(summary+" "+reminder, []string{back, exit})
	}
	return nil, errors.New("unsupported choice")
}

func (s *Session) log(message string) {
	if s.logger != nil {
		s.logger(message)
	}
}
func (s *Session) finish() ([]byte, error) {
	s.exited = true
	s.name = ""
	s.choices = nil
	return []byte(`"Exit"`), nil
}

// Parse the exact protocol grammar rather than decoding structs: encoding/json
// otherwise accepts case-insensitive and duplicate keys. Invalid input does not
// consume the outstanding choice.
func parseResponse(input []byte) (name, value string, kill bool, err error) {
	invalid := errors.New("invalid Pocket Sync response")
	if len(input) > 8192 {
		return "", "", false, invalid
	}
	d := json.NewDecoder(bytes.NewReader(input))
	token, e := d.Token()
	if e != nil {
		return "", "", false, invalid
	}
	eof := func() bool { _, e := d.Token(); return e == io.EOF }
	if token == "Kill" {
		if !eof() {
			return "", "", false, invalid
		}
		return "", "", true, nil
	}
	if token != json.Delim('{') {
		return "", "", false, invalid
	}
	key, e := d.Token()
	if e == nil && key == "Kill" {
		// The published protocol uses an empty object; deployed hosts also
		// send the unit string handled above. Require the exact object shape.
		for _, want := range []json.Delim{'{', '}', '}'} {
			got, readErr := d.Token()
			if readErr != nil || got != want {
				return "", "", false, invalid
			}
		}
		if !eof() {
			return "", "", false, invalid
		}
		return "", "", true, nil
	}
	if e != nil || key != "Answer" {
		return "", "", false, invalid
	}
	token, e = d.Token()
	if e != nil || token != json.Delim('{') {
		return "", "", false, invalid
	}
	seen := map[string]bool{}
	for d.More() {
		key, e := d.Token()
		if e != nil {
			return "", "", false, invalid
		}
		k, ok := key.(string)
		if !ok || seen[k] || (k != "name" && k != "value") {
			return "", "", false, invalid
		}
		seen[k] = true
		token, e := d.Token()
		if e != nil {
			return "", "", false, invalid
		}
		v, ok := token.(string)
		if !ok {
			return "", "", false, invalid
		}
		if k == "name" {
			name = v
		} else {
			value = v
		}
	}
	token, e = d.Token()
	if e != nil || token != json.Delim('}') || !seen["name"] || !seen["value"] {
		return "", "", false, invalid
	}
	token, e = d.Token()
	if e != nil || token != json.Delim('}') || !eof() {
		return "", "", false, invalid
	}
	return name, value, false, nil
}
