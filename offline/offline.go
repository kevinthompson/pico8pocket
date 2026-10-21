// Package offline validates and stages original PICO-8 BBS cartridge images.
// It uses only the standard library so the same checks run natively and in WASI.
package offline

import "fmt"

const MaxCart = 2 * 1024 * 1024
const MaxPack = 64 * 1024 * 1024
const PackPath = "Assets/pico8/common/cards/bbs.p8pack"
const CartPath = "Assets/pico8/common/cards/bbs"

type Cart struct {
	ID         string
	Name       string
	Collection string
}
type Carts map[string][]byte

func ValidateID(id string) error {
	if len(id) == 0 || len(id) > 47 {
		return fmt.Errorf("invalid cartridge ID: %q", id)
	}
	for i := 0; i < len(id); i++ {
		c := id[i]
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return fmt.Errorf("invalid cartridge ID: %q", id)
		}
	}
	return nil
}
