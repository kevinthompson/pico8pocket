package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/zenibako/pico8pocket/internal/testcart"
)

// TestCLIRequiresCardRoot catches accidental default writes to the working directory.
func TestCLIRequiresCardRoot(t *testing.T) {
	var out bytes.Buffer
	if run(nil, &out) == 0 {
		t.Fatal("missing root succeeded")
	}
}

// TestCLIPilotGeneratesOneAndPreservesInputs exercises the real command boundary.
func TestCLIPilotGeneratesOneAndPreservesInputs(t *testing.T) {
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	cards := filepath.Join(root, "Assets/pico8/common/cards")
	if err := os.MkdirAll(cards, 0755); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"a.p8", "b.p8"} {
		if err := os.WriteFile(filepath.Join(cards, name), testcart.Text(), 0644); err != nil {
			t.Fatal(err)
		}
	}
	var out bytes.Buffer
	if code := run([]string{"--root", root, "--first"}, &out); code != 0 {
		t.Fatalf("exit %d: %s", code, out.String())
	}
	files, err := os.ReadDir(filepath.Join(root, "System/Library/Images/pico8"))
	if err != nil || len(files) != 1 {
		t.Fatalf("generated files=%d err=%v", len(files), err)
	}
	if !bytes.Contains(out.Bytes(), []byte("Generated: 1")) {
		t.Fatal(out.String())
	}
	b, err := os.ReadFile(filepath.Join(cards, "a.p8"))
	if err != nil || !bytes.Equal(b, testcart.Text()) {
		t.Fatal("cart changed")
	}
}
