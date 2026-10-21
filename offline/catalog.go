package offline

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"strconv"
	"unicode/utf8"

	pico8pocket "github.com/zenibako/pico8pocket"
)

func Catalog() ([]Cart, error) { return DecodeCatalog(pico8pocket.OfflineCatalog) }

// DecodeCatalog rejects unknown, duplicate, missing and null fields, including
// duplicate JSON object keys that encoding/json would normally silently replace.
func DecodeCatalog(data []byte) ([]Cart, error) {
	if len(data) > MaxCart {
		return nil, fmt.Errorf("catalog exceeds size limit")
	}
	if err := validateJSONUnicode(data); err != nil {
		return nil, err
	}
	d := json.NewDecoder(bytes.NewReader(data))
	if err := checkJSONValue(d, 0); err != nil {
		return nil, err
	}
	if _, err := d.Token(); err != io.EOF {
		return nil, fmt.Errorf("trailing catalog JSON")
	}
	type entry struct {
		ID         *string `json:"id"`
		Name       *string `json:"name"`
		Collection *string `json:"collection"`
	}
	var c struct {
		Version *uint32  `json:"version"`
		Carts   *[]entry `json:"carts"`
	}
	d = json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(&c); err != nil {
		return nil, err
	}
	if c.Version == nil || *c.Version != 1 || c.Carts == nil || len(*c.Carts) == 0 || len(*c.Carts) > 256 {
		return nil, fmt.Errorf("unsupported or empty catalog")
	}
	carts := make([]Cart, 0, len(*c.Carts))
	seen := make(map[string]bool)
	for _, e := range *c.Carts {
		if e.ID == nil || e.Name == nil || e.Collection == nil {
			return nil, fmt.Errorf("missing catalog entry field")
		}
		if err := ValidateID(*e.ID); err != nil {
			return nil, err
		}
		if seen[*e.ID] || *e.Name == "" || (*e.Collection != "arcade" && *e.Collection != "sorcerer") {
			return nil, fmt.Errorf("invalid catalog entry: %s", *e.ID)
		}
		seen[*e.ID] = true
		carts = append(carts, Cart{ID: *e.ID, Name: *e.Name, Collection: *e.Collection})
	}
	return carts, nil
}
func checkJSONValue(d *json.Decoder, depth int) error {
	if depth > 64 {
		return fmt.Errorf("catalog JSON nesting exceeds limit")
	}
	token, err := d.Token()
	if err != nil {
		return err
	}
	delim, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delim {
	case '{':
		seen := make(map[string]bool)
		for d.More() {
			key, err := d.Token()
			if err != nil {
				return err
			}
			name, ok := key.(string)
			if !ok || seen[name] {
				return fmt.Errorf("duplicate or invalid JSON key: %v", key)
			}
			if depth == 0 && name != "version" && name != "carts" {
				return fmt.Errorf("unknown catalog field: %s", name)
			}
			if depth == 2 && name != "id" && name != "name" && name != "collection" {
				return fmt.Errorf("unknown catalog entry field: %s", name)
			}
			seen[name] = true
			if err := checkJSONValue(d, depth+1); err != nil {
				return err
			}
		}
	case '[':
		for d.More() {
			if err := checkJSONValue(d, depth+1); err != nil {
				return err
			}
		}
	default:
		return fmt.Errorf("unexpected JSON delimiter")
	}
	_, err = d.Token()
	return err
}

// encoding/json replaces malformed Unicode, whereas the shared catalog schema
// rejects it. Check escaped surrogate pairs before token decoding loses them.
func validateJSONUnicode(data []byte) error {
	if !utf8.Valid(data) {
		return fmt.Errorf("catalog is not valid UTF-8")
	}
	quoted := false
	for i := 0; i < len(data); i++ {
		if data[i] == '"' {
			quoted = !quoted
			continue
		}
		if !quoted || data[i] != '\\' {
			continue
		}
		i++
		if i >= len(data) {
			return fmt.Errorf("truncated JSON escape")
		}
		if data[i] != 'u' {
			continue
		}
		if i+4 >= len(data) {
			return fmt.Errorf("truncated Unicode escape")
		}
		value, err := strconv.ParseUint(string(data[i+1:i+5]), 16, 16)
		if err != nil {
			return err
		}
		i += 4
		if value >= 0xdc00 && value <= 0xdfff {
			return fmt.Errorf("unpaired Unicode surrogate")
		}
		if value < 0xd800 || value > 0xdbff {
			continue
		}
		if i+6 >= len(data) || data[i+1] != '\\' || data[i+2] != 'u' {
			return fmt.Errorf("unpaired Unicode surrogate")
		}
		low, err := strconv.ParseUint(string(data[i+3:i+7]), 16, 16)
		if err != nil || low < 0xdc00 || low > 0xdfff {
			return fmt.Errorf("unpaired Unicode surrogate")
		}
		i += 6
	}
	return nil
}
