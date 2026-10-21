package integration

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	extism "github.com/extism/go-sdk"
	"github.com/tetratelabs/wazero"
	"github.com/zenibako/pico8pocket/internal/testcart"
	"github.com/zenibako/pico8pocket/offline"
)

type choice struct {
	Choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	} `json:"Choice"`
}

type harness struct {
	t            *testing.T
	plugin       *extism.Plugin
	pocket, host string
	preserved    map[string][]byte
	onLog        func(string)
	onOpen       func(string)
}

func artifact(t *testing.T) string {
	t.Helper()
	path := os.Getenv("POCKET_PLUGIN_WASM")
	if path == "" {
		if os.Getenv("REQUIRE_PLUGIN_TESTS") == "1" {
			t.Fatal("POCKET_PLUGIN_WASM required: build the actual TinyGo plugin first")
		}
		t.Skip("set POCKET_PLUGIN_WASM to test the compiled TinyGo plugin")
	}
	return path
}

func newHarness(t *testing.T, first string, allowedHosts ...string) *harness {
	t.Helper()
	wasm := artifact(t)
	root := t.TempDir()
	h := &harness{t: t, pocket: filepath.Join(root, "card"), host: filepath.Join(root, "host")}
	h.preserved = map[string][]byte{
		"Assets/pico8/common/cards/local-fixture.p8": testcart.Text(),
		"Saves/pico8/common/progress.sav":            []byte("existing save fixture"),
		"System/Library/Images/pico8/existing.bin":   []byte("existing artwork fixture"),
	}
	for name, data := range h.preserved {
		write(t, filepath.Join(h.pocket, name), data)
	}
	if err := os.MkdirAll(h.host, 0755); err != nil {
		t.Fatal(err)
	}
	fs := wazero.NewFSConfig()
	if first == "pocket" {
		fs = fs.WithDirMount(h.pocket, "pocket").WithDirMount(h.host, "host")
	} else {
		fs = fs.WithDirMount(h.host, "host").WithDirMount(h.pocket, "pocket")
	}
	log := extism.NewHostFunctionWithStack("print_msg", func(_ context.Context, p *extism.CurrentPlugin, stack []uint64) {
		msg, _ := p.ReadString(stack[0])
		t.Log(msg)
		if h.onLog != nil {
			h.onLog(msg)
		}
		stack[0] = 0
	}, []extism.ValueType{extism.ValueTypeI64}, []extism.ValueType{extism.ValueTypeI64})
	openURL := extism.NewHostFunctionWithStack("open_url", func(_ context.Context, p *extism.CurrentPlugin, stack []uint64) {
		if h.onOpen == nil {
			t.Error("offline install unexpectedly requested open_url")
		} else {
			url, _ := p.ReadString(stack[0])
			h.onOpen(url)
		}
		stack[0] = 0
	}, []extism.ValueType{extism.ValueTypeI64}, []extism.ValueType{extism.ValueTypeI64})
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	t.Cleanup(cancel)
	p, err := extism.NewPlugin(ctx,
		extism.Manifest{Wasm: []extism.Wasm{extism.WasmFile{Path: wasm}}, AllowedHosts: allowedHosts},
		extism.PluginConfig{EnableWasi: true, ModuleConfig: wazero.NewModuleConfig().WithFSConfig(fs).WithSysWalltime().WithSysNanotime().WithSysNanosleep(), RuntimeConfig: wazero.NewRuntimeConfig().WithCloseOnContextDone(true)},
		[]extism.HostFunction{log, openURL})
	if err != nil {
		t.Fatal(err)
	}
	h.plugin = p
	t.Cleanup(func() { _ = p.Close(context.Background()) })
	return h
}

func write(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}
}

func (h *harness) call(fn string, in []byte) []byte {
	h.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	code, out, err := h.plugin.CallWithContext(ctx, fn, in)
	if err != nil || code != 0 {
		h.t.Fatalf("%s: code=%d err=%v out=%s", fn, code, err, out)
	}
	return out
}

func menu(t *testing.T, out []byte) choice {
	t.Helper()
	var m choice
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m.Choice.Name == "" || len(m.Choice.Choices) == 0 {
		t.Fatalf("missing Choice menu: %s", out)
	}
	return m
}

func answer(t *testing.T, m choice, action string) []byte {
	t.Helper()
	for _, value := range m.Choice.Choices {
		if value == action {
			out, err := json.Marshal(map[string]any{"Answer": map[string]string{"name": m.Choice.Name, "value": action}})
			if err != nil {
				t.Fatal(err)
			}
			return out
		}
	}
	t.Fatalf("menu lacks %q: %+v", action, m)
	return nil
}

func fixtures(t *testing.T) offline.Carts {
	t.Helper()
	// Read the canonical JSON independently so a plugin catalog omission cannot
	// silently reduce the number or identity of cartridges exercised here.
	data, err := os.ReadFile("../compatibility/offline-carts.json")
	if err != nil {
		t.Fatal(err)
	}
	var catalog struct {
		Carts []struct {
			ID string `json:"id"`
		} `json:"carts"`
	}
	if err := json.Unmarshal(data, &catalog); err != nil {
		t.Fatal(err)
	}
	if len(catalog.Carts) != 36 {
		t.Fatalf("catalog has %d entries, want 36", len(catalog.Carts))
	}
	carts := make(offline.Carts)
	for i, cart := range catalog.Carts {
		carts[cart.ID] = testcart.PNG(byte(i))
	}
	return carts
}

func (h *harness) seed(carts offline.Carts) {
	for id, data := range carts {
		write(h.t, offline.CachePath(h.host, id), data)
	}
}

func (h *harness) pack() []byte {
	h.t.Helper()
	data, err := os.ReadFile(filepath.Join(h.pocket, offline.PackPath))
	if err != nil {
		h.t.Fatal(err)
	}
	return data
}

func (h *harness) assertPreserved() {
	h.t.Helper()
	for name, want := range h.preserved {
		got, err := os.ReadFile(filepath.Join(h.pocket, name))
		if err != nil || !bytes.Equal(got, want) {
			h.t.Fatalf("changed existing card fixture %s: %v", name, err)
		}
	}
}

func assertPack(t *testing.T, encoded []byte, want offline.Carts) {
	t.Helper()
	got, err := offline.DecodePack(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != len(want) {
		t.Fatalf("pack has %d entries, want %d", len(got), len(want))
	}
	for id, data := range want {
		if !bytes.Equal(got[id], data) {
			t.Fatalf("original fixture bytes changed or missing for %s", id)
		}
	}
}

// Both mount orders exercise the actual Extism/WASI preopens, file writes,
// automatic cached install loop, menu ABI, and isolation from existing SD data.
func TestCompiledPlugin(t *testing.T) {
	artifact(t)
	for _, first := range []string{"pocket", "host"} {
		t.Run(first, func(t *testing.T) {
			h := newHarness(t, first)
			carts := fixtures(t)
			h.seed(carts)
			m := menu(t, h.call("start", nil))
			out := h.call("handle_response", answer(t, m, "Install both collections"))
			m = menu(t, out)
			if !strings.HasPrefix(m.Choice.Query, "Installed 36") {
				t.Fatalf("install did not finish automatically: %s", out)
			}
			assertPack(t, h.pack(), carts)
			for id, want := range carts {
				got, err := os.ReadFile(offline.CachePath(h.host, id))
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("cache changed for %s: %v", id, err)
				}
				got, err = os.ReadFile(filepath.Join(h.pocket, offline.CartPath, id+".p8.png"))
				if err != nil || !bytes.Equal(got, want) {
					t.Fatalf("standalone cartridge changed for %s: %v", id, err)
				}
			}
			h.assertPreserved()
			m = menu(t, h.call("handle_response", answer(t, m, "Installed / cache status")))
			if out := h.call("handle_response", answer(t, m, "Exit")); string(out) != `"Exit"` {
				t.Fatalf("Exit ABI: %s", out)
			}
			for _, kill := range []string{`"Kill"`, `{"Kill":{}}`} {
				h.call("start", nil)
				if out := h.call("handle_response", []byte(kill)); string(out) != `"Exit"` {
					t.Fatalf("Kill %s: %s", kill, out)
				}
			}
			assertPack(t, h.pack(), carts)
		})
	}
}

func TestCompiledPluginMergeAndDeniedHTTPPreserveFiles(t *testing.T) {
	h := newHarness(t, "host")
	carts := fixtures(t)
	old := offline.Carts{"15133": carts["15133"]}
	encoded, err := offline.EncodePack(old)
	if err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(h.pocket, offline.PackPath), encoded)
	h.seed(offline.Carts{"mot_sorcerergame": carts["mot_sorcerergame"]})
	m := menu(t, h.call("start", nil))
	m = menu(t, h.call("handle_response", answer(t, m, "Install Sorcerer")))
	old["mot_sorcerergame"] = carts["mot_sorcerergame"]
	merged := h.pack()
	assertPack(t, merged, old)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	code, out, err := h.plugin.CallWithContext(ctx, "handle_response", answer(t, m, "Update Sorcerer"))
	if err == nil && code == 0 {
		failure := menu(t, out)
		if strings.HasPrefix(failure.Choice.Query, "Installed ") || !strings.Contains(strings.ToLower(failure.Choice.Query), "http") {
			t.Fatalf("manifest-denied HTTP did not report failure: %s", out)
		}
	}
	if !bytes.Equal(h.pack(), merged) {
		t.Fatal("denied HTTP replaced the last valid pack")
	}
	cache, err := os.ReadFile(offline.CachePath(h.host, "mot_sorcerergame"))
	if err != nil || !bytes.Equal(cache, carts["mot_sorcerergame"]) {
		t.Fatalf("denied HTTP changed cache: %v", err)
	}
	h.assertPreserved()
}

// WASI uses 32-bit ints: validate the raw uint32 count before conversion so a
// high-bit count cannot wrap into an accepted empty bundle on the actual guest.
func TestCompiledPluginMalformedPublishedPackPreservesFiles(t *testing.T) {
	artifact(t)
	for name, count := range map[string]uint32{"high_bit": 0x80000000, "all_bits": 0xffffffff} {
		t.Run(name, func(t *testing.T) {
			h := newHarness(t, "host")
			malformed := make([]byte, 16)
			copy(malformed, "P8PBBS1\x00")
			binary.LittleEndian.PutUint32(malformed[8:], count)
			write(t, filepath.Join(h.pocket, offline.PackPath), malformed)
			// Different original and cached images detect premature standalone writes.
			standalone := filepath.Join(h.pocket, offline.CartPath, "mot_sorcerergame.p8.png")
			original := testcart.PNG(99)
			write(t, standalone, original)
			h.seed(offline.Carts{"mot_sorcerergame": testcart.PNG(100)})
			m := menu(t, h.call("start", nil))
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			code, out, callErr := h.plugin.CallWithContext(ctx, "handle_response", answer(t, m, "Install Sorcerer"))
			if callErr != nil || code != 0 {
				t.Errorf("malformed count %#x trapped instead of reporting rejection: code=%d err=%v", count, code, callErr)
			} else if !strings.HasPrefix(menu(t, out).Choice.Query, "Stopped:") {
				t.Errorf("malformed count %#x did not reject installation: %s", count, out)
			}
			if !bytes.Equal(h.pack(), malformed) {
				t.Error("malformed published pack was overwritten")
			}
			got, err := os.ReadFile(standalone)
			if err != nil || !bytes.Equal(got, original) {
				t.Errorf("selected standalone cartridge changed before rejecting malformed pack: %v", err)
			}
			h.assertPreserved()
		})
	}
}

func TestCompiledPluginCancellationPreservesPack(t *testing.T) {
	h := newHarness(t, "pocket")
	carts := fixtures(t)
	h.seed(carts)
	old := offline.Carts{"mot_sorcerergame": carts["mot_sorcerergame"]}
	before, err := offline.EncodePack(old)
	if err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(h.pocket, offline.PackPath), before)
	m := menu(t, h.call("start", nil))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	checked := 0
	h.onLog = func(msg string) {
		if strings.HasPrefix(msg, "Checking ") {
			checked++
			if checked == 2 {
				cancel()
			}
		}
	}
	timer := time.AfterFunc(5*time.Second, cancel)
	defer timer.Stop()
	_, _, err = h.plugin.CallWithContext(ctx, "handle_response", answer(t, m, "Install Arcade"))
	if err == nil || ctx.Err() == nil {
		t.Fatalf("expected actual context cancellation during automatic install, got %v", err)
	}
	if checked != 2 {
		t.Fatalf("cancellation did not interrupt the actual cartridge loop: checked %d", checked)
	}
	if !bytes.Equal(h.pack(), before) {
		t.Fatal("hard cancellation replaced the last valid pack")
	}
	assertPack(t, h.pack(), old)
	h.assertPreserved()
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) { return f(request) }

// The pinned Go Extism SDK uses http.DefaultClient. Replacing its transport lets
// the actual guest and host HTTP ABI run without opening any network connection.
// Unlike a fake session clock, the gap here measures the compiled WASI timer.
func TestCompiledPluginDownloadCourtesyTimer(t *testing.T) {
	h := newHarness(t, "host", "www.lexaloffle.com")
	carts := fixtures(t)
	h.seed(carts)
	missing := map[string]bool{"62288": true, "64085": true}
	for id := range missing {
		if err := os.Remove(offline.CachePath(h.host, id)); err != nil {
			t.Fatal(err)
		}
	}
	var requests []time.Time
	previous := http.DefaultClient
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		id := request.URL.Query().Get("lid")
		if request.Method != http.MethodGet || request.URL.Scheme != "https" || request.URL.Host != "www.lexaloffle.com" || request.URL.Path != "/bbs/get_cart.php" || !missing[id] {
			t.Fatalf("unexpected mocked request: %s %s", request.Method, request.URL)
		}
		delete(missing, id)
		requests = append(requests, time.Now())
		return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(carts[id])), Request: request}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	m := menu(t, h.call("start", nil))
	out := h.call("handle_response", answer(t, m, "Install Arcade"))
	if !strings.HasPrefix(menu(t, out).Choice.Query, "Installed 35") {
		t.Fatalf("mocked automatic install failed: %s", out)
	}
	if len(requests) != 2 {
		t.Fatalf("got %d HTTP requests, want exactly two cache misses", len(requests))
	}
	if gap := requests[1].Sub(requests[0]); gap < 15*time.Second {
		t.Fatalf("WASI courtesy timer elapsed %v, want at least 15s", gap)
	}
	delete(carts, "mot_sorcerergame")
	assertPack(t, h.pack(), carts)
	h.assertPreserved()
}

// Reopening the menu must not reset the courtesy interval after a real request.
// Mock responses stay inside the real SDK HTTP host ABI without BBS traffic.
func TestCompiledPluginCrossCallCourtesyTimer(t *testing.T) {
	h := newHarness(t, "host", "www.lexaloffle.com")
	var requests []time.Time
	var latest []byte
	previous := http.DefaultClient
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodGet || request.URL.Scheme != "https" || request.URL.Host != "www.lexaloffle.com" || request.URL.Path != "/bbs/get_cart.php" || request.URL.Query().Get("lid") != "mot_sorcerergame" {
			t.Fatalf("unexpected mocked request: %s %s", request.Method, request.URL)
		}
		requests = append(requests, time.Now())
		latest = testcart.PNG(byte(len(requests)))
		return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(latest)), Request: request}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	for i := 0; i < 2; i++ {
		m := menu(t, h.call("start", nil))
		out := h.call("handle_response", answer(t, m, "Update Sorcerer"))
		if !strings.HasPrefix(menu(t, out).Choice.Query, "Installed 1") {
			t.Fatalf("mocked update failed: %s", out)
		}
	}
	if len(requests) != 2 {
		t.Fatalf("got %d HTTP requests, want two separate updates", len(requests))
	}
	if gap := requests[1].Sub(requests[0]); gap < 15*time.Second {
		t.Fatalf("WASI courtesy interval reset across start/handle_response: %v, want at least 15s", gap)
	}
	assertPack(t, h.pack(), offline.Carts{"mot_sorcerergame": latest})
	h.assertPreserved()
}

func TestCompiledPluginBrowseAndDownload(t *testing.T) {
	h := newHarness(t, "host", "www.lexaloffle.com")
	const listing = "<script id=cart_data_script>\npdat=[['90001',42,`Forum title`,\"thumb\",256,170,\"date\",7,\"FixtureAuthor\",\"date\",0,\"\",3,1,0,7,2,'0',[],2,21,7,`Fixture cart`,``],];</script>"
	png := testcart.PNG(90)
	oldPack := []byte("untouched user bundle")
	write(t, filepath.Join(h.pocket, offline.PackPath), oldPack)
	previous := http.DefaultClient
	var paths []string
	var times []time.Time
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != "GET" || r.URL.Scheme != "https" || r.URL.Host != "www.lexaloffle.com" {
			t.Fatalf("unexpected request %s", r.URL)
		}
		paths = append(paths, r.URL.Path)
		times = append(times, time.Now())
		body := []byte(listing)
		switch r.URL.Path {
		case "/bbs/lister.php":
			q := r.URL.Query()
			if q.Get("cat") != "7" || q.Get("sub") != "2" || q.Get("mode") != "carts" || q.Get("orderby") != "ts" || q.Get("page") != "1" || q.Get("popular") != "1" {
				t.Fatalf("wrong public Popular listing: %s", r.URL)
			}
		case "/bbs/get_cart.php":
			if r.URL.Query().Get("lid") != "90001" {
				t.Fatalf("wrong selected cart: %s", r.URL)
			}
			body = png
		default:
			t.Fatalf("unexpected path %s", r.URL.Path)
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body)), Request: r}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	c := menu(t, h.call("start", nil))
	list := menu(t, h.call("handle_response", answer(t, c, "Browse popular carts")))
	if !strings.Contains(list.Choice.Choices[0], "Fixture cart") {
		t.Fatal(list)
	}
	detail := menu(t, h.call("handle_response", answer(t, list, list.Choice.Choices[0])))
	opened := ""
	h.onOpen = func(url string) { opened = url }
	detail = menu(t, h.call("handle_response", answer(t, detail, "Open BBS page")))
	if opened != "https://www.lexaloffle.com/bbs/?pid=90001" {
		t.Fatal(opened)
	}
	done := menu(t, h.call("handle_response", answer(t, detail, "Download to Pocket")))
	if !strings.Contains(done.Choice.Query, "Downloaded") {
		t.Fatal(done)
	}
	if len(paths) != 2 || paths[0] != "/bbs/lister.php" || paths[1] != "/bbs/get_cart.php" {
		t.Fatal(paths)
	}
	if gap := times[1].Sub(times[0]); gap < 15*time.Second {
		t.Fatalf("listing/cart request interval %v", gap)
	}
	standalone, err := os.ReadFile(filepath.Join(h.pocket, offline.CartPath, "90001.p8.png"))
	if err != nil || !bytes.Equal(standalone, png) {
		t.Fatalf("download changed bytes: %v", err)
	}
	if !bytes.Equal(h.pack(), oldPack) {
		t.Fatal("browsing altered dependency bundle")
	}
	// A repeated install uses the original cache with zero extra HTTP calls.
	h.call("handle_response", answer(t, done, "Download to Pocket"))
	if len(paths) != 2 {
		t.Fatal("cached standalone redownloaded")
	}
	h.assertPreserved()
}
