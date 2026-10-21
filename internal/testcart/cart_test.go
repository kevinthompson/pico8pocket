package testcart

import (
	"bytes"
	"image/png"
	"testing"
)

func TestPNGFixturesAreDistinctLabeledRGBA8(t *testing.T) {
	seen := make(map[string]bool)
	for i := byte(0); i < 36; i++ {
		data := PNG(i)
		if !bytes.Contains(data, []byte("PROJECT-OWNED FIXTURE")) {
			t.Fatal("missing fixture provenance")
		}
		if data[24] != 8 || data[25] != 6 {
			t.Fatal("fixture is not RGBA8")
		}
		image, err := png.Decode(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		if image.Bounds().Dx() != 160 || image.Bounds().Dy() != 205 {
			t.Fatal("wrong cartridge dimensions")
		}
		if seen[string(data)] {
			t.Fatal("duplicate cartridge fixture")
		}
		seen[string(data)] = true
		if !bytes.Equal(data, PNG(i)) {
			t.Fatal("fixture generator is not deterministic")
		}
	}
}
