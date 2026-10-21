package offline

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"strings"
	"unicode"
	"unicode/utf8"
)

type cartMetadata struct {
	Version int    `json:"version"`
	ID      string `json:"id"`
	Title   string `json:"title"`
	SHA256  string `json:"sha256"`
}

func metadataPath(host, id string) string {
	return filepath.Join(host, "pico8-offline-metadata", id+".json")
}
func validTitle(title string) bool {
	if !utf8.ValidString(title) || strings.TrimSpace(title) == "" || utf8.RuneCountInString(title) > 200 {
		return false
	}
	for _, c := range title {
		if unicode.IsControl(c) {
			return false
		}
	}
	return true
}
func payloadHash(data []byte) string { sum := sha256.Sum256(data); return hex.EncodeToString(sum[:]) }

// RememberTitle binds a public/catalog title to the exact validated cart bytes.
func RememberTitle(host, id, title string, data []byte) error {
	if err := ValidateID(id); err != nil {
		return err
	}
	if !validTitle(title) {
		return fmt.Errorf("invalid cart metadata title")
	}
	if err := ValidatePNG(data); err != nil {
		return err
	}
	raw, err := json.Marshal(cartMetadata{1, id, title, payloadHash(data)})
	if err != nil {
		return err
	}
	return AtomicWrite(metadataPath(host, id), raw)
}

// CachedTitle never applies a title to a different ID or cartridge revision.
func CachedTitle(host, id string, data []byte) (string, error) {
	if err := ValidateID(id); err != nil {
		return "", err
	}
	raw, err := ReadBounded(metadataPath(host, id), 4096)
	if err != nil {
		return "", err
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	var m cartMetadata
	if err = d.Decode(&m); err != nil {
		return "", err
	}
	var tail any
	if d.Decode(&tail) != io.EOF || m.Version != 1 || m.ID != id || !validTitle(m.Title) || m.SHA256 != payloadHash(data) {
		return "", fmt.Errorf("cart metadata does not match this file")
	}
	return m.Title, nil
}
