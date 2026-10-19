package thumbnail

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func tempDir(t *testing.T) string {
	t.Helper()
	p, e := filepath.EvalSymlinks(t.TempDir())
	if e != nil {
		t.Fatal(e)
	}
	return p
}

func put(t *testing.T, root, path string, data []byte) {
	t.Helper()
	p := filepath.Join(root, path)
	if err := os.MkdirAll(filepath.Dir(p), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, data, 0644); err != nil {
		t.Fatal(err)
	}
}
func textCart(label string) []byte {
	return []byte("pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\nprint('test')\n" + label)
}
func labelRows() string { return "__label__\n" + strings.Repeat(strings.Repeat("0", 128)+"\n", 128) }
func outputPath(data []byte) string {
	return fmt.Sprintf("System/Library/Images/pico8/%08x.bin", crc32.ChecksumIEEE(data))
}
func read(t *testing.T, root, path string) []byte {
	t.Helper()
	b, e := os.ReadFile(filepath.Join(root, path))
	if e != nil {
		t.Fatal(e)
	}
	return b
}
func pixel(b []byte, x, y int) []byte { i := 8 + (y*128+x)*4; return b[i : i+4] }

func TestPNGCornerRotationAndPreservation(t *testing.T) {
	root := tempDir(t)
	im := image.NewNRGBA(image.Rect(0, 0, 160, 205))
	im.SetNRGBA(16, 24, color.NRGBA{255, 0, 0, 255})
	im.SetNRGBA(143, 24, color.NRGBA{0, 255, 0, 255})
	im.SetNRGBA(16, 151, color.NRGBA{0, 0, 255, 255})
	im.SetNRGBA(143, 151, color.NRGBA{255, 255, 0, 255})
	var buf bytes.Buffer
	if e := png.Encode(&buf, im); e != nil {
		t.Fatal(e)
	}
	cart := buf.Bytes()
	put(t, root, "Assets/pico8/common/cards/sub/game.p8.png", cart)
	put(t, root, "Assets/pico8/common/save.dat", []byte("save"))
	r, e := Generate(root, 0)
	if e != nil || r.Generated != 1 || len(r.Paths) != 1 {
		t.Fatalf("report %+v error %v", r, e)
	}
	b := read(t, root, outputPath(cart))
	if len(b) != 65544 || binary.LittleEndian.Uint32(b) != 0x41504920 || binary.LittleEndian.Uint16(b[4:]) != 128 || binary.LittleEndian.Uint16(b[6:]) != 128 {
		t.Fatal("invalid AP_I header")
	}
	for _, v := range []struct {
		x, y int
		want []byte
	}{{0, 0, []byte{0, 255, 0, 255}}, {127, 0, []byte{0, 255, 255, 255}}, {0, 127, []byte{0, 0, 255, 255}}, {127, 127, []byte{255, 0, 0, 255}}} {
		if !bytes.Equal(pixel(b, v.x, v.y), v.want) {
			t.Fatalf("corner %d,%d: %v", v.x, v.y, pixel(b, v.x, v.y))
		}
	}
	if !bytes.Equal(read(t, root, "Assets/pico8/common/cards/sub/game.p8.png"), cart) || string(read(t, root, "Assets/pico8/common/save.dat")) != "save" {
		t.Fatal("original changed")
	}
	r, e = Generate(root, 0)
	if e != nil || r.Unchanged != 1 || r.Generated != 0 {
		t.Fatalf("repeat %+v %v", r, e)
	}
}
func TestTextPaletteAndConflictingArtwork(t *testing.T) {
	root := tempDir(t)
	label := labelRows()
	label = strings.Replace(label, strings.Repeat("0", 128), "8g"+strings.Repeat("0", 126), 1)
	cart := textCart(label)
	put(t, root, "Assets/pico8/common/cards/game.p8", cart)
	r, e := Generate(root, 0)
	if e != nil || r.Generated != 1 {
		t.Fatalf("%+v %v", r, e)
	}
	b := read(t, root, outputPath(cart))
	if !bytes.Equal(pixel(b, 0, 127), []byte{77, 0, 255, 255}) || !bytes.Equal(pixel(b, 0, 126), []byte{20, 24, 41, 255}) {
		t.Fatalf("palette wrong: %v %v", pixel(b, 0, 127), pixel(b, 0, 126))
	}
	put(t, root, outputPath(cart), []byte("custom artwork"))
	r, e = Generate(root, 0)
	if e != nil || r.Skipped != 1 || len(r.Warnings) != 1 || string(read(t, root, outputPath(cart))) != "custom artwork" {
		t.Fatalf("conflict %+v %v", r, e)
	}
}
func TestSkipsMalformedAndMissingLabelsBeforePilot(t *testing.T) {
	root := tempDir(t)
	put(t, root, "Assets/pico8/common/cards/00.p8", textCart(""))
	put(t, root, "Assets/pico8/common/cards/01.p8", textCart("__label__\n000\n"))
	put(t, root, "Assets/pico8/common/cards/02.p8.png", []byte("bad png"))
	put(t, root, "Assets/pico8/common/cards/03.p8", textCart(labelRows()))
	put(t, root, "Assets/pico8/common/cards/04.p8", append(textCart(labelRows()), '\n'))
	r, e := Generate(root, 1)
	if e != nil || r.Generated != 1 || r.Skipped != 3 || len(r.Warnings) != 2 {
		t.Fatalf("%+v %v", r, e)
	}
}
func TestInvalidRootAndLimit(t *testing.T) {
	if _, e := Generate(tempDir(t), 0); e == nil {
		t.Fatal("missing cards accepted")
	}
	if _, e := Generate(tempDir(t), -1); e == nil {
		t.Fatal("negative limit accepted")
	}
}
func TestSymlinkContainment(t *testing.T) {
	for _, part := range []string{"root", "Assets", "Assets/pico8/common/cards", "System", "System/Library/Images/pico8"} {
		t.Run(part, func(t *testing.T) {
			root := tempDir(t)
			outside := tempDir(t)
			put(t, root, "Assets/pico8/common/cards/cart.p8", textCart(labelRows()))
			if part == "root" {
				link := filepath.Join(tempDir(t), "card")
				if e := os.Symlink(root, link); e != nil {
					t.Fatal(e)
				}
				root = link
			} else {
				p := filepath.Join(root, part)
				if e := os.RemoveAll(p); e != nil {
					t.Fatal(e)
				}
				if e := os.MkdirAll(filepath.Dir(p), 0755); e != nil {
					t.Fatal(e)
				}
				if e := os.Symlink(outside, p); e != nil {
					t.Fatal(e)
				}
			}
			if _, e := Generate(root, 0); e == nil {
				t.Fatal("symlink accepted")
			}
			es, e := os.ReadDir(outside)
			if e != nil || len(es) != 0 {
				t.Fatal("outside modified")
			}
		})
	}
}
func TestCartAndOutputSymlinksSkipped(t *testing.T) {
	root := tempDir(t)
	cart := textCart(labelRows())
	put(t, root, "Assets/pico8/common/cards/real.p8", cart)
	outside := filepath.Join(tempDir(t), "outside")
	if e := os.WriteFile(outside, []byte("sentinel"), 0644); e != nil {
		t.Fatal(e)
	}
	if e := os.Symlink(outside, filepath.Join(root, "Assets/pico8/common/cards/link.p8")); e != nil {
		t.Fatal(e)
	}
	out := filepath.Join(root, outputPath(cart))
	if e := os.MkdirAll(filepath.Dir(out), 0755); e != nil {
		t.Fatal(e)
	}
	if e := os.Symlink(outside, out); e != nil {
		t.Fatal(e)
	}
	r, e := Generate(root, 0)
	if e != nil || r.Generated != 0 || r.Skipped < 1 {
		t.Fatalf("%+v %v", r, e)
	}
	b, _ := os.ReadFile(outside)
	if string(b) != "sentinel" {
		t.Fatal("symlink target modified")
	}
}
func TestCRCVector(t *testing.T) {
	if got := cartCRC([]byte("123456789")); got != "cbf43926" {
		t.Fatal(got)
	}
}

func TestInputBoundsAndLabelValidation(t *testing.T) {
	cases := []struct {
		name string
		data []byte
		ext  string
	}{
		{"oversized", bytes.Repeat([]byte{'x'}, maxCartSize+1), ".p8"},
		{"bad-digit", textCart(strings.Replace(labelRows(), strings.Repeat("0", 128), "!"+strings.Repeat("0", 127), 1)), ".p8"},
		{"extra-row", textCart(labelRows() + strings.Repeat("0", 128) + "\n"), ".p8"},
	}
	var pngData bytes.Buffer
	if err := png.Encode(&pngData, image.NewNRGBA(image.Rect(0, 0, 128, 128))); err != nil {
		t.Fatal(err)
	}
	cases = append(cases, struct {
		name string
		data []byte
		ext  string
	}{"wrong-dimensions", pngData.Bytes(), ".p8.png"})
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			root := tempDir(t)
			put(t, root, "Assets/pico8/common/cards/cart"+c.ext, c.data)
			r, e := Generate(root, 0)
			if e != nil || r.Skipped != 1 || len(r.Warnings) != 1 || r.Generated != 0 {
				t.Fatalf("%+v %v", r, e)
			}
			if _, e = os.Stat(filepath.Join(root, "System")); !os.IsNotExist(e) {
				t.Fatal("output created for invalid cart")
			}
		})
	}
}

func TestDeterministicPilotAndUnchangedLimit(t *testing.T) {
	root := tempDir(t)
	a := textCart(labelRows())
	b := append(textCart(labelRows()), []byte("__gfx__\n")...)
	put(t, root, "Assets/pico8/common/cards/z.p8", b)
	put(t, root, "Assets/pico8/common/cards/a/nested.p8", a)
	r, e := Generate(root, 1)
	if e != nil || r.Generated != 1 || len(r.Paths) != 1 || r.Paths[0] != outputPath(a) {
		t.Fatalf("first %+v %v", r, e)
	}
	r, e = Generate(root, 1)
	if e != nil || r.Unchanged != 1 || r.Generated != 0 {
		t.Fatalf("again %+v %v", r, e)
	}
	if _, e = os.Stat(filepath.Join(root, outputPath(b))); !os.IsNotExist(e) {
		t.Fatal("pilot generated second cart")
	}
	r, e = Generate(root, 0)
	if e != nil || r.Unchanged != 1 || r.Generated != 1 {
		t.Fatalf("all %+v %v", r, e)
	}
}

func TestPNGSourceRGBSurvivesAlphaPayload(t *testing.T) {
	root := tempDir(t)
	im := image.NewNRGBA(image.Rect(0, 0, 160, 205))
	im.SetNRGBA(16, 24, color.NRGBA{R: 200, G: 90, B: 40, A: 128})
	im.SetNRGBA(17, 24, color.NRGBA{R: 30, G: 80, B: 170, A: 0})
	var buf bytes.Buffer
	if err := png.Encode(&buf, im); err != nil {
		t.Fatal(err)
	}
	cart := buf.Bytes()
	put(t, root, "Assets/pico8/common/cards/alpha.p8.png", cart)
	r, err := Generate(root, 0)
	if err != nil || r.Generated != 1 {
		t.Fatalf("%+v %v", r, err)
	}
	b := read(t, root, outputPath(cart))
	for _, p := range []struct {
		x, y int
		want []byte
	}{{0, 127, []byte{40, 90, 200, 255}}, {0, 126, []byte{170, 80, 30, 255}}} {
		if got := pixel(b, p.x, p.y); !bytes.Equal(got, p.want) {
			t.Errorf("pixel %d,%d = %v, want %v", p.x, p.y, got, p.want)
		}
	}
}
