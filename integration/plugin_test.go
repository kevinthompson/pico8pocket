package integration

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	extism "github.com/extism/go-sdk"
	"github.com/tetratelabs/wazero"
	"github.com/zenibako/pico8pocket/internal/testcart"
)

// TestCompiledPlugin checks card isolation and real file generation for both preopen orders.
func TestCompiledPlugin(t *testing.T) {
	wasm := os.Getenv("POCKET_PLUGIN_WASM")
	if wasm == "" {
		if os.Getenv("REQUIRE_PLUGIN_TESTS") == "1" {
			t.Fatal("POCKET_PLUGIN_WASM required")
		}
		t.Skip("set POCKET_PLUGIN_WASM to test a compiled plugin")
	}
	for _, first := range []string{"pocket", "host"} {
		t.Run(first, func(t *testing.T) {
			root := t.TempDir()
			card := filepath.Join(root, "card")
			host := filepath.Join(root, "host")
			cards := filepath.Join(card, "Assets/pico8/common/cards")
			for _, p := range []string{cards, host, filepath.Join(card, "Saves/pico8/common")} {
				if err := os.MkdirAll(p, 0755); err != nil {
					t.Fatal(err)
				}
			}
			cart := filepath.Join(cards, "fixture.p8")
			save := filepath.Join(card, "Saves/pico8/common/progress.sav")
			if err := os.WriteFile(cart, testcart.Text(), 0644); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(save, []byte("preserve save"), 0644); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
			defer cancel()
			fs := wazero.NewFSConfig()
			if first == "pocket" {
				fs = fs.WithDirMount(card, "pocket").WithDirMount(host, "host")
			} else {
				fs = fs.WithDirMount(host, "host").WithDirMount(card, "pocket")
			}
			log := extism.NewHostFunctionWithStack("print_msg", func(_ context.Context, p *extism.CurrentPlugin, stack []uint64) {
				msg, _ := p.ReadString(stack[0])
				t.Log(msg)
				stack[0] = 0
			}, []extism.ValueType{extism.ValueTypeI64}, []extism.ValueType{extism.ValueTypeI64})
			p, err := extism.NewPlugin(ctx, extism.Manifest{Wasm: []extism.Wasm{extism.WasmFile{Path: wasm}}}, extism.PluginConfig{EnableWasi: true, ModuleConfig: wazero.NewModuleConfig().WithFSConfig(fs), RuntimeConfig: wazero.NewRuntimeConfig().WithCloseOnContextDone(true)}, []extism.HostFunction{log})
			if err != nil {
				t.Fatal(err)
			}
			defer p.Close(ctx)
			call := func(fn string, in []byte) []byte {
				t.Helper()
				code, b, err := p.CallWithContext(ctx, fn, in)
				if err != nil || code != 0 {
					t.Fatalf("%s: code=%d err=%v out=%s", fn, code, err, b)
				}
				return b
			}
			var menu struct {
				Choice struct {
					Name    string   `json:"name"`
					Choices []string `json:"choices"`
				}
			}
			if err := json.Unmarshal(call("start", nil), &menu); err != nil {
				t.Fatal(err)
			}
			if len(menu.Choice.Choices) == 0 {
				t.Fatal("no choices")
			}
			answer, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": menu.Choice.Name, "value": menu.Choice.Choices[0]}})
			out := call("handle_response", answer)
			if !bytes.Contains(out, []byte("1")) {
				t.Fatalf("missing count: %s", out)
			}
			files, err := os.ReadDir(filepath.Join(card, "System/Library/Images/pico8"))
			if err != nil || len(files) != 1 {
				t.Fatalf("images=%d err=%v out=%s", len(files), err, out)
			}
			b, err := os.ReadFile(filepath.Join(card, "System/Library/Images/pico8", files[0].Name()))
			if err != nil {
				t.Fatal(err)
			}
			if len(b) != 65544 || !bytes.Equal(b[:8], []byte{0x20, 0x49, 0x50, 0x41, 128, 0, 128, 0}) || !bytes.Equal(b[8:12], []byte{77, 0, 255, 255}) {
				t.Fatal("invalid Library image")
			}
			for name, want := range map[string][]byte{cart: testcart.Text(), save: []byte("preserve save")} {
				got, err := os.ReadFile(name)
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("changed %s: %v", name, err)
				}
			}
			if out := call("handle_response", []byte(`{"Kill":{}}`)); string(out) != `"Exit"` {
				t.Fatalf("cancel did not exit: %s", out)
			}
			entries, err := os.ReadDir(host)
			if err != nil || len(entries) != 0 {
				t.Fatal("host mount changed")
			}
		})
	}
}
