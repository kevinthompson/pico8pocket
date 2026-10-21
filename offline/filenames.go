package offline

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"unicode"
	"unicode/utf8"
)

// CartFilename keeps the BBS identity visible while making the SD browser readable.
// The bound leaves room for the ID, extension and same-directory temporary suffix.
func CartFilename(id, title string) (string, error) {
	if err := ValidateID(id); err != nil {
		return "", err
	}
	if !utf8.ValidString(title) {
		return "", fmt.Errorf("invalid cart title")
	}
	title = strings.Map(func(c rune) rune {
		if unicode.IsControl(c) || strings.ContainsRune(`<>:"/\|?*`, c) {
			return ' '
		}
		return c
	}, title)
	title = strings.Trim(strings.Join(strings.Fields(title), " "), " .")
	var short strings.Builder
	for _, c := range title {
		if short.Len()+utf8.RuneLen(c) > 120 {
			break
		}
		short.WriteRune(c)
	}
	title = strings.TrimRight(short.String(), " .")
	if title == "" {
		title = "Cart"
	}
	name := title + " [" + id + "].p8.png"
	head := strings.ToUpper(strings.TrimRight(strings.SplitN(name, ".", 2)[0], " "))
	reserved := head == "CON" || head == "PRN" || head == "AUX" || head == "NUL"
	chars := []rune(head)
	if len(chars) == 4 && (strings.HasPrefix(head, "COM") || strings.HasPrefix(head, "LPT")) && strings.ContainsRune("0123456789¹²³", chars[3]) {
		reserved = true
	}
	if reserved {
		name = "_" + name
	}
	return name, nil
}

// PublishStandalone records byte-bound metadata, publishes the titled original,
// and removes an ID-only predecessor only when its bytes match the new cart.
func PublishStandalone(pocket, host, id, title string, data []byte) (string, error) {
	name, err := CartFilename(id, title)
	if err != nil {
		return "", err
	}
	if err = RememberTitle(host, id, title, data); err != nil {
		return "", err
	}
	if err = AtomicWrite(filepath.Join(pocket, CartPath, name), data); err != nil {
		return "", err
	}
	legacy := filepath.Join(pocket, CartPath, id+".p8.png")
	old, readErr := ReadBounded(legacy, MaxCart)
	if readErr == nil && bytes.Equal(old, data) {
		if err = os.Remove(legacy); err != nil {
			return name, fmt.Errorf("cart installed; legacy filename cleanup: %w", err)
		}
	}
	return name, nil
}
