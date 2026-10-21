package pocketplugin

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
)

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
		// Accept the legacy Go host object alongside the official unit string.
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
