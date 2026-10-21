package pocketplugin

import (
	"bytes"
	"errors"
	"github.com/zenibako/pico8pocket/offline"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestLibraryTidiesLegacyCartWithoutChangingBundle(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 25)
	oldPack := []byte("unchanged bundle")
	put(t, offline.CachePath(h, "15133"), data)
	put(t, filepath.Join(p, offline.CartPath, "15133.p8.png"), data)
	put(t, filepath.Join(p, offline.PackPath), oldPack)
	s := New(p, h, nil, nil)
	library := respond(t, s, start(t, s), "Library")
	done := respond(t, s, library, "Tidy filenames")
	if !strings.Contains(done.Choice.Query, "Renamed 1") {
		t.Fatal(done)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CartPath, "celeste [15133].p8.png")), data) || !bytes.Equal(read(t, filepath.Join(p, offline.PackPath)), oldPack) {
		t.Fatal("cart bytes or bundle changed")
	}
}

const detailFixture = "<script id=cart_data_script>\npdat=[['fixture_cart-0',42,`Public Game Title`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,90001,7,2,'0',[],0,1,,``,``],];</script>"

func TestTidyLooksUpMissingTitlesOnceAndRemainsOfflineAfterward(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 26)
	s := New(p, h, nil, nil)
	for _, id := range []string{"90001", "90002"} {
		put(t, offline.CachePath(h, id), data)
		put(t, filepath.Join(p, offline.CartPath, id+".p8.png"), data)
	}
	put(t, filepath.Join(p, offline.CartPath, "90003.p8.png"), []byte("invalid PNG"))
	put(t, filepath.Join(p, offline.CartPath, "manual_cart.p8.png"), data)
	now := time.Unix(1700000000, 0)
	s.Now = func() time.Time { return now }
	waits, calls := 0, 0
	s.Sleep = func(d time.Duration) error {
		if d != 15*time.Second {
			t.Fatal(d)
		}
		waits++
		now = now.Add(d)
		return nil
	}
	s.Details = func(id string) (int, []byte, string, error) {
		calls++
		return 200, []byte(strings.ReplaceAll(detailFixture, "90001", id)), "", nil
	}
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "Renamed 2") || !strings.Contains(summary, "unmatched 1") || !strings.Contains(summary, "invalid 1") || calls != 2 || waits != 1 {
		t.Fatal(summary, err, calls, waits)
	}
	for _, id := range []string{"90001", "90002"} {
		if !bytes.Equal(read(t, filepath.Join(p, offline.CartPath, "Public Game Title ["+id+"].p8.png")), data) {
			t.Fatal(id)
		}
	}
	s.Details = func(string) (int, []byte, string, error) {
		t.Fatal("tidy refetched saved metadata")
		return 0, nil, "", nil
	}
	summary, err = s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "already tidy 2") {
		t.Fatal(summary, err)
	}
}

func TestTidyRateLimitAndCancellationPreserveOriginalNames(t *testing.T) {
	for _, rateLimit := range []bool{true, false} {
		p, h := roots(t)
		data := fixture(t, 27)
		source := filepath.Join(p, offline.CartPath, "90001.p8.png")
		put(t, source, data)
		put(t, offline.CachePath(h, "90001"), data)
		s := New(p, h, nil, nil)
		calls := 0
		cancelled := false
		s.CheckCancel = func() error {
			if cancelled {
				return ErrCancelled
			}
			return nil
		}
		s.Details = func(string) (int, []byte, string, error) {
			calls++
			if rateLimit {
				return 429, nil, "120", nil
			}
			cancelled = true
			return 200, []byte(detailFixture), "", nil
		}
		_, err := s.TidyFilenames()
		if err == nil || calls != 1 {
			t.Fatal(err, calls)
		}
		if rateLimit && !strings.Contains(err.Error(), "429") {
			t.Fatal(err)
		}
		if !rateLimit && !errors.Is(err, ErrCancelled) {
			t.Fatal(err)
		}
		if !bytes.Equal(read(t, source), data) {
			t.Fatal("interrupted tidy changed original")
		}
	}
}
