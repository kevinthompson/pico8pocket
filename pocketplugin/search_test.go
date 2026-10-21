package pocketplugin

import (
	"bytes"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

func searchPrompt(t *testing.T, s *Session, c choiceScreen) string {
	t.Helper()
	out, err := s.Handle(answer(c, "Search"))
	if err != nil {
		t.Fatal(err)
	}
	var prompt struct{ Text struct{ Name, Query string } }
	if err := json.Unmarshal(out, &prompt); err != nil || prompt.Text.Name == "" || prompt.Text.Query == "" {
		t.Fatalf("expected text input: %s (%v)", out, err)
	}
	return prompt.Text.Name
}

func textAnswer(name, value string) []byte {
	b, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": name, "value": value}})
	return b
}

func TestSearchTextInputSurvivesRestoreAndBlankReturnsToBrowse(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	browse := respond(t, s, start(t, s), "Browse BBS")
	name := searchPrompt(t, s, browse)
	saved, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	restored := New(p, h, nil, nil)
	if err := restored.Restore(saved); err != nil {
		t.Fatal(err)
	}
	if _, err := restored.Handle(textAnswer("stale", "celeste")); err == nil {
		t.Fatal("stale text answer accepted")
	}
	out, err := restored.Handle(textAnswer(name, "   "))
	c := screen(t, out, err)
	if !strings.Contains(strings.Join(c.Choice.Choices, "|"), "Search") {
		t.Fatalf("blank search did not return to Browse: %+v", c)
	}
}

func TestSearchPersistsQueryThroughPagesAndDownloadsSelectedCart(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 75)
	old := []byte("preserved dependency pack")
	put(t, filepath.Join(p, offline.PackPath), old)
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		if id != "90001" {
			t.Fatal(id)
		}
		return 200, data, "", nil
	})
	now := time.Unix(1700000000, 0)
	s.Now = func() time.Time { return now }
	waits := 0
	s.Sleep = func(d time.Duration) error { waits++; now = now.Add(d); return nil }
	pages := []int{}
	s.List = func(order bbs.Order, page int, query string) (int, []byte, string, error) {
		if order != bbs.Recent || query != "雪 & cat=8" {
			t.Fatal(order, query)
		}
		pages = append(pages, page)
		return 200, []byte(listingFixture), "", nil
	}
	browse := respond(t, s, start(t, s), "Browse BBS")
	name := searchPrompt(t, s, browse)
	out, err := s.Handle(textAnswer(name, "  雪 & cat=8  "))
	list := screen(t, out, err)
	if !strings.Contains(list.Choice.Query, "雪 & cat=8") {
		t.Fatal(list)
	}
	saved, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	restored := New(p, h, nil, s.fetch)
	restored.List, restored.Now, restored.Sleep = s.List, s.Now, s.Sleep
	if err := restored.Restore(saved); err != nil {
		t.Fatal(err)
	}
	second := respond(t, restored, list, "Next page")
	first := respond(t, restored, second, "Previous page")
	detail := respond(t, restored, first, first.Choice.Choices[0])
	done := respond(t, restored, detail, "Download to Pocket")
	if !strings.Contains(done.Choice.Query, "Downloaded") || waits != 3 {
		t.Fatal(done, waits)
	}
	if len(pages) != 3 || pages[0] != 1 || pages[1] != 2 || pages[2] != 1 {
		t.Fatal(pages)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CartPath, "90001.p8.png")), data) || !bytes.Equal(read(t, filepath.Join(p, offline.PackPath)), old) {
		t.Fatal("wrong download or modified bundle")
	}
	list = respond(t, restored, done, "Back to list")
	out, err = restored.Handle(answer(list, "New search"))
	if err != nil || !strings.Contains(string(out), `"Text"`) {
		t.Fatalf("%s %v", out, err)
	}
	var prompt struct{ Text struct{ Name string } }
	if err := json.Unmarshal(out, &prompt); err != nil {
		t.Fatal(err)
	}
	restored.List = func(order bbs.Order, page int, query string) (int, []byte, string, error) {
		if page != 1 || query != "new query" {
			t.Fatal(page, query)
		}
		return 200, []byte("<script id=cart_data_script>pdat=[];</script>"), "", nil
	}
	out, err = restored.Handle(textAnswer(prompt.Text.Name, "new query"))
	empty := screen(t, out, err)
	if !strings.Contains(empty.Choice.Query, "No carts") {
		t.Fatal(empty)
	}
}

func TestInvalidSearchNeverRequestsNetworkAndCanBeCorrected(t *testing.T) {
	for _, invalid := range []string{strings.Repeat("a", 121), "bad\nquery", "bad\x00query"} {
		p, h := roots(t)
		s := New(p, h, nil, nil)
		calls := 0
		s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
			calls++
			return 200, []byte("<script id=cart_data_script>pdat=[];</script>"), "", nil
		}
		browse := respond(t, s, start(t, s), "Browse BBS")
		name := searchPrompt(t, s, browse)
		out, err := s.Handle(textAnswer(name, invalid))
		var prompt struct{ Text struct{ Name string } }
		if err != nil || json.Unmarshal(out, &prompt) != nil || prompt.Text.Name == "" || calls != 0 {
			t.Fatalf("%s %v calls=%d", out, err, calls)
		}
		if _, err = s.Handle(textAnswer(name, "replayed")); err == nil {
			t.Fatal("stale invalid input accepted")
		}
		out, err = s.Handle(textAnswer(prompt.Text.Name, "celeste"))
		empty := screen(t, out, err)
		if calls != 1 || !strings.Contains(empty.Choice.Query, "No carts") {
			t.Fatal(empty, calls)
		}
	}
}

func TestSearchRateLimitReturnsToBrowseWithoutRetrying(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	calls := 0
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
		calls++
		return 429, nil, "120", nil
	}
	browse := respond(t, s, start(t, s), "Browse BBS")
	name := searchPrompt(t, s, browse)
	out, err := s.Handle(textAnswer(name, "celeste"))
	failure := screen(t, out, err)
	if calls != 1 || !strings.Contains(failure.Choice.Query, "HTTP 429") || !strings.Contains(failure.Choice.Query, "120") {
		t.Fatal(failure, calls)
	}
	// The failed text submission is consumed and the user can explicitly try again.
	if _, err := s.Handle(textAnswer(name, "again")); err == nil {
		t.Fatal("replayed failed search accepted")
	}
	searchPrompt(t, s, failure)
}
