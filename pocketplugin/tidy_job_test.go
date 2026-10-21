package pocketplugin

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

func tidyListing(carts ...bbs.Cart) []byte {
	var rows []string
	for _, c := range carts {
		rows = append(rows, fmt.Sprintf("[%q,42,`Post`,\"thumb\",256,170,\"date\",7,\"Tester\",\"date\",0,\"\",1,2,0,7,2,'0',[],2,21,7,%q,``]", c.ID, c.Title))
	}
	return []byte("<script id=cart_data_script>\npdat=[" + strings.Join(rows, ",") + "]; </script>")
}

func fastTidy(s *Session) {
	now := time.Unix(1700000000, 0)
	s.Now = func() time.Time { return now }
	s.Sleep = func(d time.Duration) error { now = now.Add(d); return nil }
}

func restoreTidy(t *testing.T, s *Session, p, h string) *Session {
	t.Helper()
	state, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	next := New(p, h, s.logger, s.fetch)
	next.List, next.Details, next.Now, next.Sleep = s.List, s.Details, s.Now, s.Sleep
	if err := next.Restore(state); err != nil {
		t.Fatal(err)
	}
	return next
}

func TestTidyTitleSearchRecoversCollisionWithoutPoisoningAlias(t *testing.T) {
	p, h := roots(t)
	data, wrong := fixture(t, 50), fixture(t, 51)
	source := filepath.Join(p, offline.CommonPath, "favorites/cake_factory.p8.png")
	put(t, source, data)
	put(t, offline.CachePath(h, "cake_factory"), wrong)
	pack := []byte("preserved bundle")
	put(t, filepath.Join(p, offline.PackPath), pack)
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		if id == "32957" {
			return 200, data, "", nil
		}
		return 200, wrong, "", nil
	})
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) {
		body := strings.ReplaceAll(detailFixture, "fixture_cart-0", "cake_factory-0")
		return 200, []byte(body), "", nil
	}
	s.List = func(order bbs.Order, page int, query string) (int, []byte, string, error) {
		if order != bbs.Recent || page != 1 || query != "cake factory" {
			t.Fatal(order, page, query)
		}
		return 200, tidyListing(bbs.Cart{ID: "32957", Title: "Cake Factory 1.0"}, bbs.Cart{ID: "90002", Title: "Other Cake"}), "", nil
	}
	library := respond(t, s, start(t, s), "Library")
	done := respond(t, s, library, "Tidy filenames")
	if !strings.Contains(done.Choice.Query, "Renamed 1") {
		t.Fatal(done)
	}
	target := filepath.Join(p, offline.CommonPath, "favorites/Cake Factory 1.0 [32957].p8.png")
	if !bytes.Equal(read(t, target), data) || !bytes.Equal(read(t, offline.CachePath(h, "cake_factory")), wrong) || !bytes.Equal(read(t, filepath.Join(p, offline.PackPath)), pack) {
		t.Fatal("wrong identity, bytes or bundle changed")
	}
	if _, err := os.Stat(source); !os.IsNotExist(err) {
		t.Fatal("source remains", err)
	}
	if title, err := offline.CachedTitle(h, "32957", data); err != nil || title != "Cake Factory 1.0" {
		t.Fatal(title, err)
	}
	if _, err := offline.CachedTitle(h, "cake_factory", data); err == nil {
		t.Fatal("collision poisoned original alias metadata")
	}
	s.fetch, s.List, s.Details = nil, nil, nil
	done = respond(t, s, done, "Tidy filenames")
	if !strings.Contains(done.Choice.Query, "already tidy 1") {
		t.Fatal(done)
	}
}

func TestTidyAmbiguityResumesAfterChoosingVerifiedIdentity(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 52)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), data)
	s := New(p, h, nil, func(string) (int, []byte, string, error) { return 200, data, "", nil })
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
		return 200, tidyListing(bbs.Cart{ID: "90001", Title: "First"}, bbs.Cart{ID: "90002", Title: "Second"}), "", nil
	}
	library := respond(t, s, start(t, s), "Library")
	prompt := respond(t, s, library, "Tidy filenames")
	var choice string
	for _, v := range prompt.Choice.Choices {
		if strings.Contains(v, "(#90002)") {
			choice = v
		}
	}
	if choice == "" || !strings.Contains(prompt.Choice.Query, "verified") {
		t.Fatal("missing verified resolution choices", prompt)
	}
	s = restoreTidy(t, s, p, h)
	done := respond(t, s, prompt, choice)
	if !strings.Contains(done.Choice.Query, "Renamed 1") || !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "Second [90002].p8.png")), data) {
		t.Fatal(done)
	}
}

func TestTidyPauseRejectsChangedSourceAndCanSkip(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 53)
	source := filepath.Join(p, offline.CommonPath, "mystery.p8.png")
	put(t, source, data)
	s := New(p, h, nil, func(string) (int, []byte, string, error) { return 200, data, "", nil })
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
		return 200, tidyListing(bbs.Cart{ID: "90001", Title: "First"}, bbs.Cart{ID: "90002", Title: "Second"}), "", nil
	}
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	s = restoreTidy(t, s, p, h)
	changed := fixture(t, 54)
	put(t, source, changed)
	var choice string
	for _, v := range prompt.Choice.Choices {
		if strings.Contains(v, "(#90001)") {
			choice = v
		}
	}
	if choice == "" {
		t.Fatal(prompt)
	}
	prompt = respond(t, s, prompt, choice)
	if !strings.Contains(strings.ToLower(prompt.Choice.Query), "changed") {
		t.Fatal(prompt)
	}
	done := respond(t, s, prompt, "Skip file")
	if !strings.Contains(done.Choice.Query, "unmatched 1") || !bytes.Equal(read(t, source), changed) {
		t.Fatal(done)
	}
}

func TestTidyRestoreRejectsEscapedQueueBeforeUsingIt(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 55)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), data)
	s := New(p, h, nil, nil)
	fastTidy(s)
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) { return 200, tidyListing(), "", nil }
	respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	b, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	var state map[string]any
	if err := json.Unmarshal(b, &state); err != nil {
		t.Fatal(err)
	}
	job, ok := state["tidy"].(map[string]any)
	if !ok {
		t.Fatal("missing paused job", string(b))
	}
	files := job["files"].([]any)
	files[0].(map[string]any)["Dir"] = "../../outside"
	b, err = json.Marshal(state)
	if err != nil {
		t.Fatal(err)
	}
	if err := New(p, h, nil, nil).Restore(b); err == nil {
		t.Fatal("escaped source restored")
	}
}

func tidyTextAnswer(t *testing.T, s *Session, prompt choiceScreen, option, value, p, h string) choiceScreen {
	t.Helper()
	b, err := s.Handle(answer(prompt, option))
	if err != nil {
		t.Fatal(err)
	}
	var text struct{ Text struct{ Name string } }
	if err := json.Unmarshal(b, &text); err != nil || text.Text.Name == "" {
		t.Fatal(string(b), err)
	}
	saved := restoreTidy(t, s, p, h)
	*s = *saved
	input, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": text.Text.Name, "value": value}})
	b, err = s.Handle(input)
	return screen(t, b, err)
}

func TestTidyBatchesCandidatesAndResumesWithoutRepeating(t *testing.T) {
	p, h := roots(t)
	data, wrong := fixture(t, 56), fixture(t, 57)
	put(t, filepath.Join(p, offline.CommonPath, "unknown.p8.png"), data)
	var checked []string
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		checked = append(checked, id)
		if id == "90011" {
			return 200, data, "", nil
		}
		return 200, wrong, "", nil
	})
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
		var carts []bbs.Cart
		for i := 1; i <= 11; i++ {
			carts = append(carts, bbs.Cart{ID: fmt.Sprint(90000 + i), Title: "Game"})
		}
		return 200, tidyListing(carts...), "", nil
	}
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	if len(checked) != 10 {
		t.Fatal("unbounded batch", checked)
	}
	s = restoreTidy(t, s, p, h)
	done := respond(t, s, prompt, "Check more results")
	if len(checked) != 11 || !strings.Contains(done.Choice.Query, "Renamed 1") {
		t.Fatal(checked, done)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "Game [90011].p8.png")), data) {
		t.Fatal("wrong output")
	}
}

func TestTidyManualIDRetryPreservesIdentityAndCooldown(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 58)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), data)
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		if id != "90001" {
			t.Fatal(id)
		}
		return 200, data, "", nil
	})
	fastTidy(s)
	manualRequests := 0
	s.Details = func(id string) (int, []byte, string, error) {
		if id != "90001" {
			return 404, nil, "", nil
		}
		manualRequests++
		if manualRequests == 1 {
			return 429, nil, "30", nil
		}
		return 200, []byte(detailFixture), "", nil
	}
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) { return 200, tidyListing(), "", nil }
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	prompt = tidyTextAnswer(t, s, prompt, "Enter BBS ID", "90001", p, h)
	if !strings.Contains(prompt.Choice.Query, "429") {
		t.Fatal(prompt)
	}
	stamp := s.LastRequest
	s = restoreTidy(t, s, p, h)
	done := respond(t, s, prompt, "Retry")
	if manualRequests != 2 || !strings.Contains(done.Choice.Query, "Renamed 1") || s.LastRequest.Sub(stamp) < 30*time.Second {
		t.Fatal(manualRequests, done, s.LastRequest.Sub(stamp))
	}
}

func TestTidyEditSearchAndConflictFilenameSurviveTextPrompts(t *testing.T) {
	p, h := roots(t)
	data, other := fixture(t, 59), fixture(t, 60)
	common := filepath.Join(p, offline.CommonPath)
	put(t, filepath.Join(common, "mystery.p8.png"), data)
	put(t, filepath.Join(common, "Existing [90001].p8.png"), other)
	// Keep the unrelated destination offline and already tidy when scanned.
	if err := offline.RememberTitle(h, "90001", "Existing", other); err != nil {
		t.Fatal(err)
	}
	s := New(p, h, nil, func(string) (int, []byte, string, error) { return 200, data, "", nil })
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(_ bbs.Order, _ int, q string) (int, []byte, string, error) {
		if q == "new title" {
			return 200, tidyListing(bbs.Cart{ID: "90001", Title: "Existing"}), "", nil
		}
		return 200, tidyListing(), "", nil
	}
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	prompt = tidyTextAnswer(t, s, prompt, "Edit search", "new title", p, h)
	if !strings.Contains(prompt.Choice.Query, "Destination already exists") {
		t.Fatal(prompt)
	}
	done := tidyTextAnswer(t, s, prompt, "Choose another filename", "Alternate", p, h)
	if !strings.Contains(done.Choice.Query, "Renamed 1; already tidy 1") || !bytes.Equal(read(t, filepath.Join(common, "Existing [90001].p8.png")), other) || !bytes.Equal(read(t, filepath.Join(common, "Alternate [90001].p8.png")), data) {
		t.Fatal(done)
	}
}

func TestTidyStopKeepsCompletedChangesAndChangedTextDoesNotFetch(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 61)
	put(t, filepath.Join(p, offline.CommonPath, "11437.p8.png"), data)
	source := filepath.Join(p, offline.CommonPath, "mystery.p8.png")
	put(t, source, data)
	s := New(p, h, nil, func(string) (int, []byte, string, error) { return 200, data, "", nil })
	fastTidy(s)
	s.Details = func(id string) (int, []byte, string, error) {
		if id != "mystery" {
			t.Fatal("changed source was looked up", id)
		}
		return 404, nil, "", nil
	}
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) { return 200, tidyListing(), "", nil }
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	put(t, source, fixture(t, 62))
	prompt = tidyTextAnswer(t, s, prompt, "Enter BBS ID", "90001", p, h)
	if !strings.Contains(prompt.Choice.Query, "changed") {
		t.Fatal(prompt)
	}
	done := respond(t, s, prompt, "Stop")
	if !strings.Contains(done.Choice.Query, "Renamed 1") || !strings.Contains(done.Choice.Query, "1 files remain unprocessed") || !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "picoman [11437].p8.png")), data) {
		t.Fatal(done)
	}
}

func TestTidyDoesNotCallAPartialBatchUnique(t *testing.T) {
	p, h := roots(t)
	data, wrong := fixture(t, 63), fixture(t, 64)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), data)
	requests := 0
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		requests++
		if id == "90001" || id == "90011" {
			return 200, data, "", nil
		}
		return 200, wrong, "", nil
	})
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
		var carts []bbs.Cart
		for i := 1; i <= 11; i++ {
			carts = append(carts, bbs.Cart{ID: fmt.Sprint(90000 + i), Title: fmt.Sprint("Game ", i)})
		}
		return 200, tidyListing(carts...), "", nil
	}
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	if requests != 10 || s.tidy == nil || s.tidy.Index != 0 || len(s.tidy.Pending.Matches) != 1 {
		t.Fatal(requests, prompt)
	}
	s = restoreTidy(t, s, p, h)
	prompt = respond(t, s, prompt, "Check more results")
	if requests != 11 || !strings.Contains(prompt.Choice.Query, "Multiple verified identities") || len(s.tidy.Pending.Matches) != 2 {
		t.Fatal(requests, prompt)
	}
}

func TestTidyDerivedQueriesAndUnsupportedBBSIDsCanResume(t *testing.T) {
	for _, name := range []string{"0.p8.png", "00123.p8.png", strings.Repeat("a", 119) + " b [mystery].p8.png", "bad\x01title [mystery].p8.png", "_.p8.png"} {
		t.Run(name, func(t *testing.T) {
			if strings.ContainsRune(name, '\x01') && os.PathSeparator == '\\' {
				t.Skip("control filename unsupported on Windows")
			}
			p, h := roots(t)
			put(t, filepath.Join(p, offline.CommonPath, name), fixture(t, 65))
			s := New(p, h, nil, nil)
			fastTidy(s)
			s.List = func(order bbs.Order, page int, q string) (int, []byte, string, error) {
				if _, err := bbs.ListingURL(order, page, q); err != nil {
					t.Fatal("invalid derived search", q, err)
				}
				return 200, tidyListing(), "", nil
			}
			prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
			s = restoreTidy(t, s, p, h)
			done := respond(t, s, prompt, "Skip file")
			if !strings.Contains(done.Choice.Query, "unmatched 1") {
				t.Fatal(done)
			}
		})
	}
}

func TestTidyPaginationKeepsEarlierMatchesAndDeduplicatesIDs(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 66)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), data)
	fetched := 0
	s := New(p, h, nil, func(string) (int, []byte, string, error) { fetched++; return 200, data, "", nil })
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 404, nil, "", nil }
	s.List = func(_ bbs.Order, page int, _ string) (int, []byte, string, error) {
		if page == 1 {
			return 200, append(tidyListing(bbs.Cart{ID: "90001", Title: "First"}), []byte("<a href='#mode=carts&page=2'>Next</a>")...), "", nil
		}
		if page != 2 {
			t.Fatal(page)
		}
		return 200, tidyListing(bbs.Cart{ID: "90001", Title: "First"}, bbs.Cart{ID: "90002", Title: "Second"}), "", nil
	}
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	if s.tidy == nil || fetched != 1 {
		t.Fatal("partial page auto-published", prompt)
	}
	s = restoreTidy(t, s, p, h)
	prompt = respond(t, s, prompt, "Check more results")
	if fetched != 2 || len(s.tidy.Pending.Matches) != 2 {
		t.Fatal("duplicate verification or lost matches", fetched, prompt)
	}
}

func TestTidyMetadataWriteFailureLeavesSourceForRetry(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 67)
	source := filepath.Join(p, offline.CommonPath, "11437.p8.png")
	put(t, source, data)
	put(t, filepath.Join(h, "pico8-offline-metadata"), []byte("blocked directory"))
	s := New(p, h, nil, func(string) (int, []byte, string, error) { return 200, data, "", nil })
	fastTidy(s)
	prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	if !bytes.Equal(read(t, source), data) || !s.tidy.Pending.Retry {
		t.Fatal("source removed before metadata succeeded", prompt)
	}
	if err := os.Remove(filepath.Join(h, "pico8-offline-metadata")); err != nil {
		t.Fatal(err)
	}
	s = restoreTidy(t, s, p, h)
	done := respond(t, s, prompt, "Retry")
	if !strings.Contains(done.Choice.Query, "Renamed 1") {
		t.Fatal(done)
	}
}

func TestTidyUnavailableCandidateCannotEstablishUniqueness(t *testing.T) {
	for _, status := range []int{503, 429, 200} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			p, h := roots(t)
			data := fixture(t, 68)
			source := filepath.Join(p, offline.CommonPath, "mystery.p8.png")
			put(t, source, data)
			attempts := 0
			s := New(p, h, nil, func(id string) (int, []byte, string, error) {
				if id == "90002" {
					attempts++
					if attempts == 1 {
						return status, []byte("invalid response"), "", nil
					}
				}
				return 200, data, "", nil
			})
			fastTidy(s)
			s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
				return 200, tidyListing(bbs.Cart{ID: "90001", Title: "First"}, bbs.Cart{ID: "90002", Title: "Second"}), "", nil
			}
			prompt := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
			if s.tidy == nil || !s.tidy.Pending.Retry || s.tidy.Pending.Cursor != 1 || len(s.tidy.Pending.Checked) != 1 || !bytes.Equal(read(t, source), data) {
				t.Fatal("unavailable candidate was excluded", prompt)
			}
			s = restoreTidy(t, s, p, h)
			prompt = respond(t, s, prompt, "Retry")
			if attempts != 2 || !strings.Contains(prompt.Choice.Query, "Multiple verified identities") {
				t.Fatal(attempts, prompt)
			}
		})
	}
}

func TestTidyMultiCartUsesVerifiedVersionIdentity(t *testing.T) {
	p, h := roots(t)
	data, wrong := fixture(t, 69), fixture(t, 70)
	put(t, filepath.Join(p, offline.CommonPath, "90001.p8.png"), data)
	put(t, offline.CachePath(h, "90001"), wrong)
	other := strings.ReplaceAll(detailFixture, "fixture_cart-0", "other_cart-0")
	other = strings.ReplaceAll(other, "Public Game Title", "Other Game")
	row := other[strings.Index(other, "['other_cart-0'"):strings.Index(other, ",];")]
	multi := strings.Replace(detailFixture, ",];", ","+row+",];", 1)
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		if id == "other_cart-0" {
			return 200, data, "", nil
		}
		if id != "fixture_cart-0" {
			t.Fatal("wrong candidate", id)
		}
		return 200, wrong, "", nil
	})
	fastTidy(s)
	s.Details = func(string) (int, []byte, string, error) { return 200, []byte(multi), "", nil }
	done := respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	if !strings.Contains(done.Choice.Query, "Renamed 1") || !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "Other Game [other_cart-0].p8.png")), data) || !bytes.Equal(read(t, offline.CachePath(h, "90001")), wrong) {
		t.Fatal(done)
	}
}

func TestTidyRestoreRejectsMalformedProgressBeforeMutation(t *testing.T) {
	p, h := roots(t)
	put(t, filepath.Join(p, offline.CommonPath, "mystery.p8.png"), fixture(t, 71))
	s := New(p, h, nil, nil)
	respond(t, s, respond(t, s, start(t, s), "Library"), "Tidy filenames")
	saved, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*conversationState){
		func(v *conversationState) { v.Tidy.Files = append(v.Tidy.Files, v.Tidy.Files[0]) },
		func(v *conversationState) { v.Tidy.Pending.Cursor = -1 },
		func(v *conversationState) { v.Tidy.Pending.Hash = "invalid" },
		func(v *conversationState) { v.Tidy.Totals.Renamed = 1 },
		func(v *conversationState) { v.SearchInput = true },
		func(v *conversationState) { v.Tidy.Pending.Matches = []bbs.Cart{{ID: "90001", Title: "Unverified"}} },
	} {
		var v conversationState
		if err := json.Unmarshal(saved, &v); err != nil {
			t.Fatal(err)
		}
		mutate(&v)
		changed, _ := json.Marshal(v)
		next := New(p, h, nil, nil)
		if err := next.Restore(changed); err == nil || next.tidy != nil || next.started {
			t.Fatal("malformed state accepted or partially applied", err)
		}
	}
}
