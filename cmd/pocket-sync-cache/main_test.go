package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/zenibako/pico8pocket/internal/testcart"
	"github.com/zenibako/pico8pocket/offline"
)

func TestVerifyOriginalBundle(t *testing.T) {
	p, err := offline.EncodePack(offline.Carts{"project_fixture": testcart.PNG(1)})
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "bbs.p8pack")
	if err := os.WriteFile(path, p, 0600); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := run([]string{"verify", path}, &out); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "Valid bundle: 1 cartridges") {
		t.Fatal(out.String())
	}
}
func TestImportAndCacheOnlyInstall(t *testing.T) {
	source, host, pocket := t.TempDir(), t.TempDir(), t.TempDir()
	png := testcart.PNG(2)
	if err := os.WriteFile(filepath.Join(source, "mot_sorcerergame.p8.png"), png, 0600); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := run([]string{"import", source, host, "sorcerer"}, &out); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"install", host, pocket, "sorcerer"}, &out); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(pocket, offline.PackPath))
	if err != nil {
		t.Fatal(err)
	}
	carts, err := offline.DecodePack(data)
	if err != nil || !bytes.Equal(carts["mot_sorcerergame"], png) {
		t.Fatal(err)
	}
	if err := run([]string{"install", host, pocket, "arcade"}, &out); err == nil {
		t.Fatal("missing cache install reported success")
	}
	after, _ := os.ReadFile(filepath.Join(pocket, offline.PackPath))
	if !bytes.Equal(after, data) {
		t.Fatal("failed install replaced valid bundle")
	}
}
func TestUsageAndMalformedBundle(t *testing.T) {
	var out bytes.Buffer
	for _, args := range [][]string{nil, {"verify"}, {"import", "a", "b", "unknown"}, {"install", "a", "b", "unknown"}} {
		if err := run(args, &out); err == nil {
			t.Fatalf("accepted %v", args)
		}
	}
	path := filepath.Join(t.TempDir(), "bad.p8pack")
	os.WriteFile(path, []byte("bad"), 0600)
	if err := run([]string{"verify", path}, &out); err == nil {
		t.Fatal("verified malformed bundle")
	}
}
