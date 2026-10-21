package pocketplugin

import (
	"bytes"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

func TestBrowserOfferedFromMainMenu(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	c := respond(t, s, start(t, s), "Browse BBS")
	for _, want := range []string{"Recent", "Popular", "Search"} {
		found := false
		for _, option := range c.Choice.Choices {
			if option == want {
				found = true
			}
		}
		if !found {
			t.Fatalf("main menu lacks %q", want)
		}
	}
}

const listingFixture = "<a href=#sub=2&page=2&mode=carts>next</a><script id=cart_data_script>\npdat=[['90001',42,`Post`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,0,7,2,'0',[],2,21,7,`Fixture Game`,``],];</script>"

func TestBrowsePersistsSelectionAndDownloadsOnlyStandalone(t *testing.T) {
	p, h := roots(t)
	old := []byte("unrelated existing bundle bytes")
	put(t, filepath.Join(p, offline.PackPath), old)
	data := fixture(t, 90)
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		if id != "90001" {
			t.Fatalf("wrong selected ID: %s", id)
		}
		return 200, data, "", nil
	})
	now := time.Unix(1700000000, 0)
	s.Now = func() time.Time { return now }
	waits := 0
	s.Sleep = func(d time.Duration) error {
		if d != 15*time.Second {
			t.Fatal(d)
		}
		waits++
		now = now.Add(d)
		return nil
	}
	s.List = func(order bbs.Order, page int, query string) (int, []byte, string, error) {
		if order != bbs.Recent || page != 1 {
			t.Fatal(order, page)
		}
		return 200, []byte(listingFixture), "", nil
	}
	c := respond(t, s, start(t, s), "Browse recent carts")
	if !strings.Contains(c.Choice.Choices[0], "Fixture Game") {
		t.Fatal(c)
	}
	saved, err := s.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	restored := New(p, h, nil, s.fetch)
	restored.Now = s.Now
	restored.Sleep = s.Sleep
	restored.List = s.List
	if err = restored.Restore(saved); err != nil {
		t.Fatal(err)
	}
	detail := respond(t, restored, c, c.Choice.Choices[0])
	if !strings.Contains(detail.Choice.Query, "https://www.lexaloffle.com/bbs/?pid=90001") {
		t.Fatal(detail)
	}
	done := respond(t, restored, detail, "Download to Pocket")
	if !strings.Contains(done.Choice.Query, "Downloaded") || waits != 1 {
		t.Fatal(done, waits)
	}
	for _, choice := range done.Choice.Choices {
		if choice == "Download to Pocket" {
			t.Fatal("completed download still offers the same download action")
		}
	}
	if strings.Contains(done.Choice.Query, "Download this cart") {
		t.Fatal("completed download still asks for confirmation")
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CartPath, "Fixture Game [90001].p8.png")), data) || !bytes.Equal(read(t, offline.CachePath(h, "90001")), data) {
		t.Fatal("download bytes changed")
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.PackPath)), old) {
		t.Fatal("standalone download rewrote dependency pack")
	}
	saved, err = restored.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	completed := New(p, h, nil, restored.fetch)
	completed.Sleep, completed.Now, completed.List = restored.Sleep, restored.Now, restored.List
	completed.OpenURL = func(string) error { return nil }
	if err := completed.Restore(saved); err != nil {
		t.Fatal(err)
	}
	done = respond(t, completed, done, "Open BBS page")
	if !strings.Contains(done.Choice.Query, "Downloaded Fixture Game") || !strings.Contains(done.Choice.Query, "bbs/Fixture Game [90001].p8.png") {
		t.Fatal(done)
	}
	for _, choice := range done.Choice.Choices {
		if choice == "Download to Pocket" {
			t.Fatal("opening BBS page forgot completed download")
		}
	}
	restored = completed
	restored.Sleep = func(time.Duration) error { t.Fatal("cached reinstallation waited"); return nil }
	restored.fetch = func(string) (int, []byte, string, error) {
		t.Fatal("cached reinstallation fetched")
		return 0, nil, "", nil
	}
	// Reinstalling is explicit: return to the list and select the cart again.
	list := respond(t, restored, done, "Back to list")
	detail = respond(t, restored, list, list.Choice.Choices[0])
	respond(t, restored, detail, "Download to Pocket")
}
func TestListingPaginationAndOpenPage(t *testing.T) {
	p, h := roots(t)
	s := New(p, h, nil, nil)
	s.Sleep = func(time.Duration) error { return nil }
	s.List = func(order bbs.Order, page int, query string) (int, []byte, string, error) {
		if order != bbs.Popular {
			t.Fatal(order)
		}
		body := listingFixture
		if page == 2 {
			body = strings.Replace(body, "page=2", "page=1", 1)
		}
		return 200, []byte(body), "", nil
	}
	c := respond(t, s, start(t, s), "Browse popular carts")
	second := respond(t, s, c, "Next page")
	for _, option := range second.Choice.Choices {
		if option == "Next page" {
			t.Fatal("invented next page")
		}
	}
	first := respond(t, s, second, "Previous page")
	detail := respond(t, s, first, first.Choice.Choices[0])
	opened := ""
	s.OpenURL = func(url string) error { opened = url; return nil }
	respond(t, s, detail, "Open BBS page")
	if opened != "https://www.lexaloffle.com/bbs/?pid=90001" {
		t.Fatal(opened)
	}
}
func TestFailedListingAndCartDownloadPreserveInstalledFiles(t *testing.T) {
	for _, status := range []int{429, 500, 200} {
		p, h := roots(t)
		data := fixture(t, 80)
		put(t, filepath.Join(p, offline.CartPath, "90001.p8.png"), data)
		s := New(p, h, nil, func(string) (int, []byte, string, error) { return status, []byte("invalid"), "120", nil })
		s.Sleep = func(time.Duration) error { return nil }
		s.List = func(bbs.Order, int, string) (int, []byte, string, error) { return 200, []byte(listingFixture), "", nil }
		c := respond(t, s, start(t, s), "Browse recent carts")
		d := respond(t, s, c, c.Choice.Choices[0])
		failure := respond(t, s, d, "Download to Pocket")
		if !strings.Contains(failure.Choice.Query, "Stopped:") {
			t.Fatal(failure)
		}
		if status == 429 && !strings.Contains(failure.Choice.Query, "120") {
			t.Fatal(failure)
		}
		if !bytes.Equal(read(t, filepath.Join(p, offline.CartPath, "90001.p8.png")), data) {
			t.Fatal("failed download destroyed cart")
		}
		s.List = func(bbs.Order, int, string) (int, []byte, string, error) {
			return status, []byte("bad listing"), "120", nil
		}
		main := respond(t, s, failure, "Back to main menu")
		failure = respond(t, s, main, "Browse recent carts")
		if !strings.Contains(failure.Choice.Query, "Stopped:") {
			t.Fatal(failure)
		}
	}
}
