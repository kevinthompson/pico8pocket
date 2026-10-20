package pocketplugin

import (
	"bytes"
	"encoding/json"
	"errors"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/offline"
)

type choiceScreen struct {
	Choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	} `json:"Choice"`
}

func screen(t *testing.T, b []byte, e error) choiceScreen {
	t.Helper()
	if e != nil {
		t.Fatal(e)
	}
	var c choiceScreen
	if json.Unmarshal(b, &c) != nil || c.Choice.Name == "" {
		t.Fatalf("invalid choice: %s", b)
	}
	return c
}
func start(t *testing.T, s *Session) choiceScreen {
	t.Helper()
	b, e := s.Start()
	return screen(t, b, e)
}
func answer(c choiceScreen, v string) []byte {
	b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": c.Choice.Name, "value": v}})
	return b
}
func respond(t *testing.T, s *Session, c choiceScreen, v string) choiceScreen {
	t.Helper()
	b, e := s.Handle(answer(c, v))
	return screen(t, b, e)
}
func fixture(t *testing.T, n byte) []byte {
	t.Helper()
	im := image.NewNRGBA(image.Rect(0, 0, 160, 205))
	im.SetNRGBA(0, 0, color.NRGBA{R: n, A: 255})
	var b bytes.Buffer
	if e := png.Encode(&b, im); e != nil {
		t.Fatal(e)
	}
	return b.Bytes()
}
func roots(t *testing.T) (string, string) { t.Helper(); return t.TempDir(), t.TempDir() }
func put(t *testing.T, p string, b []byte) {
	t.Helper()
	if e := offline.AtomicWrite(p, b); e != nil {
		t.Fatal(e)
	}
}
func cached(t *testing.T, host, group string, b []byte) {
	t.Helper()
	c, e := offline.Catalog()
	if e != nil {
		t.Fatal(e)
	}
	for _, c := range c {
		if group == "both" || group == c.Collection {
			put(t, offline.CachePath(host, c.ID), b)
		}
	}
}
func read(t *testing.T, p string) []byte {
	t.Helper()
	b, e := os.ReadFile(p)
	if e != nil {
		t.Fatal(e)
	}
	return b
}
func TestMenuProtocol(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	c := start(t, s)
	want := "Install both collections|Install Arcade|Install Sorcerer|Update both collections|Update Arcade|Update Sorcerer|Installed / cache status|Exit"
	if strings.Join(c.Choice.Choices, "|") != want {
		t.Fatal(c)
	}
	if _, e := s.Start(); e == nil {
		t.Fatal("duplicate start")
	}
	next := respond(t, s, c, "Installed / cache status")
	if !strings.Contains(next.Choice.Query, "arcade: installed 0/35, verified cache 0/35") {
		t.Fatal(next)
	}
	if _, e := s.Handle(answer(c, "Exit")); e == nil {
		t.Fatal("replay accepted")
	}
	b, e := s.Handle(answer(next, "Exit"))
	if e != nil || string(b) != `"Exit"` {
		t.Fatalf("%s %v", b, e)
	}
}
func TestKillAndStrictAnswers(t *testing.T) {
	for _, in := range []string{`"Kill"`, `{"Kill":{}}`} {
		p, h := roots(t)
		s := New(p, h, nil, nil)
		if _, e := s.Handle([]byte(in)); e == nil {
			t.Fatal("before start")
		}
		start(t, s)
		b, e := s.Handle([]byte(in))
		if e != nil || string(b) != `"Exit"` {
			t.Fatalf("%s %v", b, e)
		}
		if _, e := s.Handle([]byte(in)); e == nil {
			t.Fatal("after exit")
		}
	}
	for _, in := range []string{`null`, `"Exit"`, `{"Kill":null}`, `{"Kill":{},"extra":0}`, `{"Answer":{"name":"NAME","value":"Exit","value":"Exit"}}`, `{"Answer":{"Name":"NAME","value":"Exit"}}`, `{"Answer":{"name":"NAME","value":"bad"}}`, `{"Answer":{"name":"NAME","value":"Exit"}} null`, `{"Answer":{"name":"wrong","value":"Exit"}}`} {
		p, h := roots(t)
		s := New(p, h, nil, nil)
		c := start(t, s)
		if _, e := s.Handle([]byte(strings.ReplaceAll(in, "NAME", c.Choice.Name))); e == nil {
			t.Fatalf("accepted %s", in)
		}
		respond(t, s, c, "Installed / cache status")
	}
}
func TestCacheOnlyInstallMergesAndReinstalls(t *testing.T) {
	p, h := roots(t)
	b := fixture(t, 1)
	cached(t, h, "both", b)
	calls := 0
	s := New(p, h, nil, func(string) (int, []byte, string, error) { calls++; return 0, nil, "", errors.New("offline") })
	s.Sleep = func(time.Duration) error { t.Fatal("cache slept"); return nil }
	if _, e := s.Install("sorcerer", false); e != nil {
		t.Fatal(e)
	}
	if _, e := s.Install("arcade", false); e != nil {
		t.Fatal(e)
	}
	pack := read(t, filepath.Join(p, offline.PackPath))
	c, e := offline.DecodePack(pack)
	if e != nil || len(c) != 36 || calls != 0 {
		t.Fatalf("%d %v calls %d", len(c), e, calls)
	}
	if _, e = s.Install("both", false); e != nil {
		t.Fatal(e)
	}
	if !bytes.Equal(pack, read(t, filepath.Join(p, offline.PackPath))) {
		t.Fatal("reinstall changed pack")
	}
	if !bytes.Equal(b, read(t, filepath.Join(p, offline.CartPath, "15133.p8.png"))) {
		t.Fatal("standalone differs")
	}
}
func TestFailedRefreshPreservesCacheAndBundle(t *testing.T) {
	for _, status := range []int{500, 200} {
		p, h := roots(t)
		b := fixture(t, 1)
		cached(t, h, "sorcerer", b)
		s := New(p, h, nil, nil)
		if _, e := s.Install("sorcerer", false); e != nil {
			t.Fatal(e)
		}
		old := read(t, filepath.Join(p, offline.PackPath))
		s.fetch = func(string) (int, []byte, string, error) { return status, []byte("bad PNG"), "", nil }
		if _, e := s.Install("sorcerer", true); e == nil {
			t.Fatal("refresh passed")
		}
		if !bytes.Equal(old, read(t, filepath.Join(p, offline.PackPath))) || !bytes.Equal(b, read(t, offline.CachePath(h, "mot_sorcerergame"))) {
			t.Fatal("refresh damaged previous data")
		}
	}
}
func TestRateLimitStopsEntireBatch(t *testing.T) {
	p, h := roots(t)
	b := fixture(t, 1)
	cached(t, h, "both", b)
	s := New(p, h, nil, nil)
	if _, e := s.Install("both", false); e != nil {
		t.Fatal(e)
	}
	old := read(t, filepath.Join(p, offline.PackPath))
	calls := 0
	s.fetch = func(id string) (int, []byte, string, error) { calls++; return 429, nil, "120", nil }
	_, e := s.Install("both", true)
	if e == nil || !strings.Contains(e.Error(), firstCatalogID(t)) || !strings.Contains(e.Error(), "120") || calls != 1 {
		t.Fatalf("%v calls %d", e, calls)
	}
	if !bytes.Equal(old, read(t, filepath.Join(p, offline.PackPath))) {
		t.Fatal("429 damaged bundle")
	}
}
func TestCancellationDuringCourtesyWaitPreservesBundle(t *testing.T) {
	p, h := roots(t)
	b := fixture(t, 1)
	cached(t, h, "both", b)
	s := New(p, h, nil, nil)
	if _, e := s.Install("both", false); e != nil {
		t.Fatal(e)
	}
	old := read(t, filepath.Join(p, offline.PackPath))
	calls := 0
	s.fetch = func(string) (int, []byte, string, error) { calls++; return 200, fixture(t, 2), "", nil }
	s.Sleep = func(d time.Duration) error {
		if d != 15*time.Second {
			t.Fatal(d)
		}
		return ErrCancelled
	}
	if _, e := s.Install("both", true); !errors.Is(e, ErrCancelled) || calls != 1 {
		t.Fatalf("%v calls %d", e, calls)
	}
	if !bytes.Equal(old, read(t, filepath.Join(p, offline.PackPath))) {
		t.Fatal("cancel damaged bundle")
	}
}
func TestMalformedExistingPackNeverOverwritten(t *testing.T) {
	p, h := roots(t)
	b := fixture(t, 1)
	cached(t, h, "sorcerer", b)
	path := filepath.Join(p, offline.PackPath)
	put(t, path, []byte("malformed"))
	s := New(p, h, nil, nil)
	if _, e := s.Install("sorcerer", false); e == nil {
		t.Fatal("malformed accepted")
	}
	if string(read(t, path)) != "malformed" {
		t.Fatal("malformed overwritten")
	}
}

func TestConversationStateSurvivesNewWASIInstance(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	first := start(t, s)
	saved, e := s.Snapshot()
	if e != nil {
		t.Fatal(e)
	}
	restored := New(p, h, nil, nil)
	if e = restored.Restore(saved); e != nil {
		t.Fatal(e)
	}
	next := respond(t, restored, first, "Installed / cache status")
	if next.Choice.Name == first.Choice.Name {
		t.Fatal("sequence reset")
	}
	if _, e = restored.Handle(answer(first, "Exit")); e == nil {
		t.Fatal("restored replay accepted")
	}
}
func TestAutomaticMenuActionHasNoContinuePrompt(t *testing.T) {
	p, h := roots(t)
	cached(t, h, "sorcerer", fixture(t, 3))
	s := New(p, h, nil, nil)
	done := respond(t, s, start(t, s), "Install Sorcerer")
	if !strings.Contains(done.Choice.Query, "Installed 1 selected") {
		t.Fatal(done)
	}
	for _, v := range done.Choice.Choices {
		if v == "Continue" {
			t.Fatal("manual progress prompt")
		}
	}
}

func firstCatalogID(t *testing.T) string {
	t.Helper()
	c, e := offline.Catalog()
	if e != nil {
		t.Fatal(e)
	}
	return c[0].ID
}

func TestMissingCartPreventsPartialPublication(t *testing.T) {
	p, h := roots(t)
	oldPNG := fixture(t, 1)
	cached(t, h, "sorcerer", oldPNG)
	s := New(p, h, nil, nil)
	if _, e := s.Install("sorcerer", false); e != nil {
		t.Fatal(e)
	}
	old := read(t, filepath.Join(p, offline.PackPath))
	catalog, e := offline.Catalog()
	if e != nil {
		t.Fatal(e)
	}
	calls := 0
	waits := 0
	s.fetch = func(id string) (int, []byte, string, error) {
		calls++
		if id == catalog[1].ID {
			return 500, nil, "", nil
		}
		return 200, fixture(t, 2), "", nil
	}
	s.Sleep = func(d time.Duration) error {
		waits++
		if d != 15*time.Second {
			t.Fatal(d)
		}
		return nil
	}
	if _, e = s.Install("arcade", false); e == nil || !strings.Contains(e.Error(), catalog[1].ID) {
		t.Fatalf("%v", e)
	}
	if calls != 35 || waits != 34 {
		t.Fatalf("calls %d waits %d", calls, waits)
	}
	if !bytes.Equal(old, read(t, filepath.Join(p, offline.PackPath))) {
		t.Fatal("partial collection published")
	}
	if _, e = os.Stat(filepath.Join(p, offline.CartPath, catalog[0].ID+".p8.png")); !os.IsNotExist(e) {
		t.Fatal("standalone written before preparation complete")
	}
	// Successful first downloads remain usable; a retry requests only the failed cart.
	calls = 0
	s.fetch = func(id string) (int, []byte, string, error) {
		calls++
		if id != catalog[1].ID {
			t.Fatal(id)
		}
		return 200, fixture(t, 2), "", nil
	}
	waits = 0
	if _, e = s.Install("arcade", false); e != nil {
		t.Fatal(e)
	}
	if calls != 1 || waits != 0 {
		t.Fatalf("retry calls %d waits %d", calls, waits)
	}
}

func TestCancelledBeforePublicationPreservesPack(t *testing.T) {
	p, h := roots(t)
	cached(t, h, "sorcerer", fixture(t, 1))
	s := New(p, h, nil, nil)
	if _, e := s.Install("sorcerer", false); e != nil {
		t.Fatal(e)
	}
	old := read(t, filepath.Join(p, offline.PackPath))
	cached(t, h, "sorcerer", fixture(t, 2))
	cancelled := false
	s.logger = func(msg string) {
		if strings.HasPrefix(msg, "Prepared standalone carts") {
			cancelled = true
		}
	}
	s.CheckCancel = func() error {
		if cancelled {
			return ErrCancelled
		}
		return nil
	}
	if _, e := s.Install("sorcerer", false); !errors.Is(e, ErrCancelled) {
		t.Fatal(e)
	}
	if !bytes.Equal(old, read(t, filepath.Join(p, offline.PackPath))) {
		t.Fatal("cancelled final publication")
	}
	if bytes.Equal(fixture(t, 1), read(t, filepath.Join(p, offline.CartPath, "mot_sorcerergame.p8.png"))) {
		t.Fatal("test did not reach standalone publication")
	}
}
