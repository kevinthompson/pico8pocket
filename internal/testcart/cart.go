// Package testcart builds project-owned synthetic cart labels for host tests.
package testcart

import "strings"

// Text returns a text cartridge with a red label and no executable gameplay.
func Text() []byte {
	return []byte("pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n-- thumbnail fixture\n__label__\n" + strings.Repeat(strings.Repeat("8", 128)+"\n", 128))
}
