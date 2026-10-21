package pocketplugin

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/offline"
)

func TestTidyVerifiesUncachedCatalogCartAndThenWorksOffline(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 31)
	source := filepath.Join(p, offline.CommonPath, "11437.p8.png")
	manual := filepath.Join(p, offline.CommonPath, "celeste_classic_2.p8.png")
	put(t, source, data)
	put(t, manual, data)
	var logs []string
	s := New(p, h, func(msg string) { logs = append(logs, msg) }, func(id string) (int, []byte, string, error) {
		if id != "11437" {
			t.Fatalf("guessed an identity for a manual filename: %s", id)
		}
		return 200, data, "", nil
	})
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "Renamed 1") || !strings.Contains(summary, "no saved identity: 1") {
		t.Fatal(summary, err)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "picoman [11437].p8.png")), data) || !bytes.Equal(read(t, manual), data) {
		t.Fatal("tidy changed original cart bytes")
	}
	if _, err := os.Stat(source); !os.IsNotExist(err) {
		t.Fatal("legacy filename remains", err)
	}
	if title, err := offline.CachedTitle(h, "11437", data); err != nil || title != "picoman" {
		t.Fatal("verified metadata not saved", title, err)
	}
	if !strings.Contains(strings.Join(logs, "\n"), "celeste_classic_2.p8.png: no saved identity") {
		t.Fatal("unmatched filename has no actionable reason", logs)
	}
	s.fetch = nil
	summary, err = s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "already tidy 1") {
		t.Fatal("repeat tidy needs network", summary, err)
	}
}

func TestTidyVerificationPreservesUnmatchedOriginalAndCache(t *testing.T) {
	for _, tc := range []struct {
		name, reason string
		status       int
		body         []byte
		fetchErr     error
		cancel       bool
		wantErr      bool
	}{
		{"different revision", "different BBS cart bytes: 1", 200, fixture(t, 33), nil, false, false},
		{"invalid response", "BBS verification unavailable: 1", 200, []byte("not a cart"), nil, false, false},
		{"missing cart", "BBS verification unavailable: 1", 404, nil, nil, false, false},
		{"transport failure", "BBS verification unavailable: 1", 0, nil, errors.New("offline"), false, false},
		{"rate limit", "429", 429, nil, nil, false, true},
		{"cancelled during fetch", "cancelled", 200, fixture(t, 32), nil, true, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p, h := roots(t)
			data, oldCache := fixture(t, 32), fixture(t, 34)
			source := filepath.Join(p, offline.CommonPath, "11437.p8.png")
			put(t, source, data)
			put(t, offline.CachePath(h, "11437"), oldCache)
			cancelled := false
			s := New(p, h, nil, func(string) (int, []byte, string, error) {
				cancelled = tc.cancel
				return tc.status, tc.body, "120", tc.fetchErr
			})
			s.CheckCancel = func() error {
				if cancelled {
					return ErrCancelled
				}
				return nil
			}
			summary, err := s.TidyFilenames()
			if tc.wantErr {
				if err == nil || !strings.Contains(err.Error(), tc.reason) {
					t.Fatal(summary, err)
				}
			} else if err != nil || !strings.Contains(summary, tc.reason) {
				t.Fatal(summary, err)
			}
			if !bytes.Equal(read(t, source), data) || !bytes.Equal(read(t, offline.CachePath(h, "11437")), oldCache) {
				t.Fatal("unverified operation changed the cart or cache")
			}
			if _, err := offline.CachedTitle(h, "11437", data); err == nil {
				t.Fatal("saved metadata for unverified cart")
			}
		})
	}
}

func TestTidyVerificationAndMetadataShareRequestPacing(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 35)
	put(t, filepath.Join(p, offline.CommonPath, "90001.p8.png"), data)
	var events []string
	s := New(p, h, nil, func(string) (int, []byte, string, error) {
		events = append(events, "cart")
		return 200, data, "", nil
	})
	now := time.Unix(1700000000, 0)
	s.Now = func() time.Time { return now }
	s.Sleep = func(d time.Duration) error {
		if d != 15*time.Second {
			t.Fatal(d)
		}
		events = append(events, "wait")
		now = now.Add(d)
		return nil
	}
	s.Details = func(string) (int, []byte, string, error) {
		events = append(events, "metadata")
		return 200, []byte(detailFixture), "", nil
	}
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "Renamed 1") || strings.Join(events, ",") != "cart,wait,metadata" {
		t.Fatal(summary, err, events)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "Public Game Title [90001].p8.png")), data) {
		t.Fatal("cart changed")
	}
}

func TestTidyBoundsUnmatchedExamples(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 36)
	for _, id := range []string{"a", "b", "c", "d", "e", "f", "g"} {
		put(t, filepath.Join(p, offline.CommonPath, id+".p8.png"), data)
	}
	var logs []string
	s := New(p, h, func(msg string) { logs = append(logs, msg) }, nil)
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "no saved identity: 7") || len(logs) != 5 {
		t.Fatal("skip reasons missing or unbounded log output", summary, err, logs)
	}
}
