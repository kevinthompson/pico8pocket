package offline

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"hash/crc32"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// Fixtures are blank, project-owned images, never downloaded cartridges.
func pngChunk(kind string, data []byte) []byte {
	b := make([]byte, len(data)+12)
	binary.BigEndian.PutUint32(b, uint32(len(data)))
	copy(b[4:], kind)
	copy(b[8:], data)
	binary.BigEndian.PutUint32(b[len(b)-4:], crc32.ChecksumIEEE(b[4:len(b)-4]))
	return b
}
func cartPNG() []byte {
	header := make([]byte, 13)
	binary.BigEndian.PutUint32(header, 160)
	binary.BigEndian.PutUint32(header[4:], 205)
	header[8] = 8
	header[9] = 6
	var pixels bytes.Buffer
	w := zlib.NewWriter(&pixels)
	w.Write(make([]byte, 205*(160*4+1)))
	w.Close()
	out := append([]byte("\x89PNG\r\n\x1a\n"), pngChunk("IHDR", header)...)
	out = append(out, pngChunk("IDAT", pixels.Bytes())...)
	return append(out, pngChunk("IEND", nil)...)
}
func TestIdentifiers(t *testing.T) {
	for _, id := range []string{"15133", "magic_bubble", "a-b", strings.Repeat("a", 47)} {
		if err := ValidateID(id); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"", "#15133", "../cart", "a/b", "Cart", "a.b", strings.Repeat("a", 48)} {
		if ValidateID(id) == nil {
			t.Errorf("accepted %q", id)
		}
	}
}
func TestCatalogStrictSchema(t *testing.T) {
	c, err := Catalog()
	if err != nil || len(c) != 36 {
		t.Fatalf("catalog %d: %v", len(c), err)
	}
	arcade := 0
	for _, cart := range c {
		if cart.Collection == "arcade" {
			arcade++
		}
	}
	if arcade != 35 {
		t.Fatalf("arcade %d", arcade)
	}
	good := `{"version":1,"carts":[{"id":"a","name":"A","collection":"arcade"}]}`
	if _, err := DecodeCatalog([]byte(good)); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{strings.Replace(good, `"version":1`, `"version":2`, 1),
		strings.Replace(good, `"version":1`, `"Version":1`, 1),
		strings.Replace(good, `"id":"a"`, `"ID":"a"`, 1), strings.Replace(good, `"version":1`, `"version":1,"version":1`, 1), strings.Replace(good, `"name":"A",`, "", 1), strings.Replace(good, `"name":"A"`, `"name":null`, 1), strings.Replace(good, `"name":"A"`, `"name":"A","unknown":1`, 1), good + `{}`, `{"version":1,"carts":[]}`} {
		if _, err := DecodeCatalog([]byte(bad)); err == nil {
			t.Errorf("accepted %s", bad)
		}
	}
}
func TestCatalogRejectsInvalidUnicode(t *testing.T) {
	for _, name := range []string{`\ud800`, `\udc00`, string([]byte{0xff})} {
		data := []byte(`{"version":1,"carts":[{"id":"a","name":"` + name + `","collection":"arcade"}]}`)
		if _, err := DecodeCatalog(data); err == nil {
			t.Errorf("accepted invalid Unicode %q", name)
		}
	}
	valid := []byte(`{"version":1,"carts":[{"id":"a","name":"\ud83d\ude00","collection":"arcade"}]}`)
	if _, err := DecodeCatalog(valid); err != nil {
		t.Fatal(err)
	}
}

func TestPNGValidation(t *testing.T) {
	p := cartPNG()
	if err := ValidatePNG(p); err != nil {
		t.Fatal(err)
	}
	for _, n := range []int{0, 8, 33, len(p) - 1} {
		if ValidatePNG(p[:n]) == nil {
			t.Errorf("accepted truncated %d", n)
		}
	}
	bad := append([]byte(nil), p...)
	bad[40] ^= 1
	if ValidatePNG(bad) == nil {
		t.Fatal("accepted CRC corruption")
	}
	for name, mutation := range map[string]func([]byte) []byte{
		"animation": func(p []byte) []byte {
			return append(append(append([]byte(nil), p[:33]...), pngChunk("acTL", make([]byte, 8))...), p[33:]...)
		},
		"dimensions": func(p []byte) []byte {
			h := append([]byte(nil), p[16:29]...)
			h[3] = 159
			return append(append([]byte(nil), p[:8]...), append(pngChunk("IHDR", h), p[33:]...)...)
		},
		"deflate": func(p []byte) []byte {
			n := int(binary.BigEndian.Uint32(p[33:]))
			d := append([]byte(nil), p[41:41+n]...)
			d[0] = 0
			return append(append(append([]byte(nil), p[:33]...), pngChunk("IDAT", d)...), p[45+n:]...)
		},
		"trailing": func(p []byte) []byte { return append(p, 0) },
	} {
		t.Run(name, func(t *testing.T) {
			if ValidatePNG(mutation(append([]byte(nil), p...))) == nil {
				t.Fatal("accepted invalid PNG")
			}
		})
	}
	if ValidatePNG(make([]byte, MaxCart+1)) == nil {
		t.Fatal("accepted oversized PNG")
	}
}
func TestPackContract(t *testing.T) {
	carts := Carts{"b": cartPNG(), "a": cartPNG()}
	p, err := EncodePack(carts)
	if err != nil {
		t.Fatal(err)
	}
	if string(p[:8]) != "P8PBBS1\x00" || p[16] != 'a' || binary.LittleEndian.Uint32(p[64:]) != 144 {
		t.Fatal("wire format changed")
	}
	got, err := DecodePack(p)
	if err != nil || len(got) != 2 || !bytes.Equal(got["a"], carts["a"]) {
		t.Fatalf("roundtrip: %v", err)
	}
	again, _ := EncodePack(got)
	if !bytes.Equal(again, p) {
		t.Fatal("nondeterministic pack")
	}
	for _, pos := range []int{0, 12, 63, 64, 72, 76} {
		bad := append([]byte(nil), p...)
		bad[pos] ^= 1
		if _, err := DecodePack(bad); err == nil {
			t.Errorf("accepted corruption at %d", pos)
		}
	}
	dup := append([]byte(nil), p...)
	dup[80] = 'a'
	if _, err := DecodePack(dup); err == nil {
		t.Fatal("accepted duplicate")
	}
	if _, err := DecodePack(p[:len(p)-1]); err == nil {
		t.Fatal("accepted truncation")
	}
	if _, err := EncodePack(Carts{}); err == nil {
		t.Fatal("encoded empty pack")
	}
	empty := make([]byte, 16)
	copy(empty, "P8PBBS1\x00")
	if got, err := DecodePack(empty); err != nil || len(got) != 0 {
		t.Fatal("empty v1 upgrade", err)
	}
}
func TestBoundedReadAndAtomicReplacement(t *testing.T) {
	path := filepath.Join(t.TempDir(), "nested", "cart")
	if err := AtomicWrite(path, []byte("old")); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadBounded(path, 2); err == nil {
		t.Fatal("unbounded read")
	}
	if err := AtomicWrite(path, []byte("new")); err != nil {
		t.Fatal(err)
	}
	got, err := ReadBounded(path, 3)
	if err != nil || string(got) != "new" {
		t.Fatal(string(got), err)
	}
}
func TestImportValidatesAllInputsBeforeWriting(t *testing.T) {
	source, host := t.TempDir(), t.TempDir()
	catalog, _ := Catalog()
	png := cartPNG()
	for _, cart := range catalog {
		if err := os.WriteFile(filepath.Join(source, cart.ID+".p8.png"), png, 0600); err != nil {
			t.Fatal(err)
		}
	}
	first := catalog[0].ID
	if err := AtomicWrite(CachePath(host, first), []byte("old")); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(source, catalog[len(catalog)-1].ID+".p8.png"), []byte("invalid"), 0600)
	if _, err := ImportCache(source, host, "both"); err == nil {
		t.Fatal("imported malformed input")
	}
	got, _ := os.ReadFile(CachePath(host, first))
	if string(got) != "old" {
		t.Fatal("changed existing cache before validating all selected inputs")
	}
	os.WriteFile(filepath.Join(source, catalog[len(catalog)-1].ID+".p8.png"), png, 0600)
	if count, err := ImportCache(source, host, "both"); err != nil || count != 36 {
		t.Fatal(count, err)
	}
	if got, err := CachedCart(host, first); err != nil || !bytes.Equal(got, png) {
		t.Fatal(err)
	}
	if _, err := CachedCart(host, "../escape"); err == nil {
		t.Fatal("accepted unsafe cache path")
	}
	if _, err := ImportCache(source, host, "unknown"); err == nil {
		t.Fatal("accepted unknown collection")
	}
}

func TestRejectedAtomicReplacementKeepsPublishedFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "cart")
	if err := AtomicWrite(path, []byte("published")); err != nil {
		t.Fatal(err)
	}
	// A blocked temporary pathname makes replacement fail before rename.
	if err := os.Mkdir(path+".pocket-sync-tmp", 0755); err != nil {
		t.Fatal(err)
	}
	if err := AtomicWrite(path, []byte("replacement")); err == nil {
		t.Fatal("expected blocked write to fail")
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "published" {
		t.Fatalf("lost published file: %q %v", data, err)
	}
}

func TestPNGRejectsExtraCompressedBytesAndInvalidPixels(t *testing.T) {
	original := cartPNG()
	n := int(binary.BigEndian.Uint32(original[33:]))
	idat := original[41 : 41+n]
	for name, compressed := range map[string][]byte{
		"trailing zlib data": append(append([]byte(nil), idat...), 0),
		"missing pixels": func() []byte {
			var out bytes.Buffer
			w := zlib.NewWriter(&out)
			w.Write([]byte{0})
			w.Close()
			return out.Bytes()
		}(),
		"invalid filter": func() []byte {
			raw := make([]byte, 205*(160*4+1))
			raw[0] = 5
			var out bytes.Buffer
			w := zlib.NewWriter(&out)
			w.Write(raw)
			w.Close()
			return out.Bytes()
		}(),
	} {
		t.Run(name, func(t *testing.T) {
			p := append(append(append([]byte(nil), original[:33]...), pngChunk("IDAT", compressed)...), original[45+n:]...)
			if ValidatePNG(p) == nil {
				t.Fatal("accepted malformed pixels")
			}
		})
	}
}

// TinyGo's wasm32 int is narrower than native Go's int. High wire counts must
// remain invalid on every architecture, before conversion or table arithmetic.
func TestPackRejectsHighBitEntryCounts(t *testing.T) {
	for _, count := range []uint32{0x80000000, 0x80000001, 0xffffffff} {
		t.Run(strconv.FormatUint(uint64(count), 16), func(t *testing.T) {
			data := make([]byte, 16)
			copy(data, "P8PBBS1\x00")
			binary.LittleEndian.PutUint32(data[8:], count)
			if _, err := DecodePack(data); err == nil {
				t.Fatalf("accepted entry count %#x", count)
			}
		})
	}
}

func TestPackRejectsTableLimitsAndTrailingData(t *testing.T) {
	empty := make([]byte, 16)
	copy(empty, "P8PBBS1\x00")
	binary.LittleEndian.PutUint32(empty[8:], 257)
	if _, err := DecodePack(empty); err == nil {
		t.Fatal("accepted excessive entry count")
	}
	carts := make(Carts)
	for i := 0; i < 257; i++ {
		carts["fixture_"+strconv.Itoa(i)] = cartPNG()
	}
	if _, err := EncodePack(carts); err == nil {
		t.Fatal("encoded excessive entry count")
	}
	p, err := EncodePack(Carts{"a": cartPNG()})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := DecodePack(append(p, 0)); err == nil {
		t.Fatal("accepted trailing bundle data")
	}
}
