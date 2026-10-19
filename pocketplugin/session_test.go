package pocketplugin

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func tempRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	return root
}

type choiceScreen struct {
	Choice struct {
		Name    string   `json:"name"`
		Query   string   `json:"query"`
		Choices []string `json:"choices"`
	} `json:"Choice"`
}

func screen(t *testing.T, b []byte, err error) choiceScreen {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
	var s choiceScreen
	if err = json.Unmarshal(b, &s); err != nil || s.Choice.Name == "" {
		t.Fatalf("invalid screen %s: %v", b, err)
	}
	return s
}
func start(t *testing.T, s *Session) choiceScreen {
	t.Helper()
	b, e := s.Start()
	return screen(t, b, e)
}
func answer(s choiceScreen, value string) []byte {
	b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": s.Choice.Name, "value": value}})
	return b
}
func respond(t *testing.T, s *Session, c choiceScreen, v string) choiceScreen {
	t.Helper()
	b, e := s.Handle(answer(c, v))
	return screen(t, b, e)
}
func TestMenuHelpAndReplay(t *testing.T) {
	s := New(tempRoot(t), nil)
	c := start(t, s)
	if strings.Join(c.Choice.Choices, "|") != "Generate one test thumbnail|Generate all thumbnails|Help|Exit" {
		t.Fatal(c)
	}
	if _, e := s.Start(); e == nil {
		t.Fatal("duplicate Start accepted")
	}
	h := respond(t, s, c, "Help")
	if h.Choice.Name == c.Choice.Name || !strings.Contains(h.Choice.Query, "Assets/pico8/common/cards") || !strings.Contains(h.Choice.Query, "unverified") {
		t.Fatal(h)
	}
	if _, e := s.Handle(answer(c, "Help")); e == nil {
		t.Fatal("replay accepted")
	}
	m := respond(t, s, h, "Back")
	if m.Choice.Name == h.Choice.Name {
		t.Fatal("name reused")
	}
	b, e := s.Handle(answer(m, "Exit"))
	if e != nil || string(b) != `"Exit"` {
		t.Fatalf("%s %v", b, e)
	}
	if _, e = s.Handle([]byte(`"Kill"`)); e == nil {
		t.Fatal("after exit accepted")
	}
}

// TestKillAndBeforeStart accepts both documented and deployed cancellation forms.
func TestKillAndBeforeStart(t *testing.T) {
	for _, input := range []string{`"Kill"`, `{"Kill":{}}`} {
		t.Run(input, func(t *testing.T) {
			s := New(tempRoot(t), nil)
			if _, e := s.Handle([]byte(input)); e == nil {
				t.Fatal("before Start accepted")
			}
			start(t, s)
			b, e := s.Handle([]byte(input))
			if e != nil || string(b) != `"Exit"` {
				t.Fatalf("%s %v", b, e)
			}
			if _, e := s.Handle([]byte(input)); e == nil {
				t.Fatal("after Exit accepted")
			}
		})
	}
}
func TestStrictAnswers(t *testing.T) {
	invalid := []string{
		`null`, `{}`, `[]`, `"Exit"`, `{"Kill":null}`, `{"Kill":[]}`, `{"Kill":{"extra":true}}`, `{"Kill":{},"extra":0}`, `{"Kill":{},"Kill":{}}`, `{"Kill":{}} null`, `{"Answer":null}`,
		`{"Answer":{"name":"NAME","value":"Help","root":"/tmp"}}`,
		`{"Answer":{"name":"NAME","value":"Help"},"extra":0}`,
		`{"Answer":{"name":"NAME","name":"NAME","value":"Help"}}`,
		`{"Answer":{"name":"NAME","value":"Help","value":"Exit"}}`,
		`{"Answer":{"name":"NAME","value":"Help"},"Answer":{"name":"NAME","value":"Help"}}`,
		`{"Answer":{"name":"NAME","value":"Help"}} null`,
		`{"Answer":{"name":"wrong","value":"Help"}}`,
		`{"Answer":{"name":"NAME","value":"help"}}`,
		`{"Answer":{"name":"NAME","value":"Help","Value":"Exit"}}`,
		`{"answer":{"name":"NAME","value":"Help"}}`,
	}
	for _, input := range invalid {
		t.Run(input, func(t *testing.T) {
			s := New(tempRoot(t), nil)
			c := start(t, s)
			input = strings.ReplaceAll(input, "NAME", c.Choice.Name)
			if _, e := s.Handle([]byte(input)); e == nil {
				t.Fatalf("accepted %s", input)
			}
			respond(t, s, c, "Help")
		})
	}
}
func cart(t *testing.T, root, name string) {
	t.Helper()
	dir := filepath.Join(root, "Assets/pico8/common/cards")
	if e := os.MkdirAll(dir, 0755); e != nil {
		t.Fatal(e)
	}
	data := "pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\nprint('hi')\n__label__\n" + strings.Repeat(strings.Repeat("8", 128)+"\n", 128)
	if e := os.WriteFile(filepath.Join(dir, name+".p8"), []byte(data), 0644); e != nil {
		t.Fatal(e)
	}
}
func TestGenerationPilotAllAndIdempotence(t *testing.T) {
	root := tempRoot(t)
	cart(t, root, "a")
	cart(t, root, "b") // Different bytes give each cart its own CRC32 path.
	p := filepath.Join(root, "Assets/pico8/common/cards/b.p8")
	b, _ := os.ReadFile(p)
	os.WriteFile(p, append(b, []byte("\n")...), 0644)
	var logs []string
	s := New(root, func(v string) { logs = append(logs, v) })
	c := start(t, s)
	done := respond(t, s, c, "Generate one test thumbnail")
	if !strings.Contains(done.Choice.Query, "Generated 1") || !strings.Contains(done.Choice.Query, "Pocket") || !strings.Contains(done.Choice.Query, "unverified") {
		t.Fatal(done)
	}
	images, _ := filepath.Glob(filepath.Join(root, "System/Library/Images/pico8/*"))
	if len(images) != 1 {
		t.Fatal(images)
	}
	c = respond(t, s, done, "Back")
	done = respond(t, s, c, "Generate all thumbnails")
	if !strings.Contains(done.Choice.Query, "Generated 1") || !strings.Contains(done.Choice.Query, "unchanged 1") {
		t.Fatal(done)
	}
	images, _ = filepath.Glob(filepath.Join(root, "System/Library/Images/pico8/*"))
	if len(images) != 2 || len(logs) == 0 {
		t.Fatalf("images=%v logs=%v", images, logs)
	}
}
func TestGenerationErrorRemainsNavigable(t *testing.T) {
	s := New(tempRoot(t), nil)
	c := start(t, s)
	done := respond(t, s, c, "Generate all thumbnails")
	if !strings.Contains(done.Choice.Query, "failed") {
		t.Fatal(done)
	}
	respond(t, s, done, "Back")
}
