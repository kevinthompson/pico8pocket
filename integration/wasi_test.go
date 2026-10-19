package integration

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
	"github.com/zenibako/pico8pocket/internal/testcart"
)

// TestWASICLI runs the distributed standard-Go command against an actual mounted card.
func TestWASICLI(t *testing.T) {
	wasm := os.Getenv("CART_THUMBNAILS_WASM")
	if wasm == "" {
		if os.Getenv("REQUIRE_PLUGIN_TESTS") == "1" {
			t.Fatal("CART_THUMBNAILS_WASM required")
		}
		t.Skip("set CART_THUMBNAILS_WASM to test the built WASI CLI")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	root := t.TempDir()
	cards := filepath.Join(root, "Assets/pico8/common/cards")
	if err := os.MkdirAll(cards, 0755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cards, "wasi.p8")
	if err := os.WriteFile(path, testcart.Text(), 0644); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(wasm)
	if err != nil {
		t.Fatal(err)
	}
	runtime := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().WithCloseOnContextDone(true))
	defer runtime.Close(ctx)
	if _, err := wasi_snapshot_preview1.Instantiate(ctx, runtime); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	_, err = runtime.InstantiateWithConfig(ctx, data, wazero.NewModuleConfig().WithFSConfig(wazero.NewFSConfig().WithDirMount(root, "/pocket")).WithArgs("cart-thumbnails", "--root", "/pocket", "--first").WithStdout(&out).WithStderr(&out))
	if err != nil {
		t.Fatalf("WASI CLI: %v\n%s", err, out.String())
	}
	if !bytes.Contains(out.Bytes(), []byte("Generated: 1")) {
		t.Fatalf("output: %s", out.String())
	}
	files, err := os.ReadDir(filepath.Join(root, "System/Library/Images/pico8"))
	if err != nil || len(files) != 1 {
		t.Fatalf("files=%d err=%v", len(files), err)
	}
	image, err := os.ReadFile(filepath.Join(root, "System/Library/Images/pico8", files[0].Name()))
	if err != nil {
		t.Fatal(err)
	}
	if len(image) != 65544 || !bytes.Equal(image[8:12], []byte{77, 0, 255, 255}) {
		t.Fatal("bad image pixels")
	}
	original, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(original, testcart.Text()) {
		t.Fatal("cart modified")
	}
}
