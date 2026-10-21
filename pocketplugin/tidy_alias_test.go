package pocketplugin

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/offline"
)

func TestTidyResolvesNamedBBSAliasBeforeVerifyingBytes(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 40)
	source := filepath.Join(p, offline.CommonPath, "favorites/fixture_named.p8.png")
	put(t, source, data)
	var events []string
	s := New(p, h, nil, func(id string) (int, []byte, string, error) {
		events = append(events, "cart:"+id)
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
	s.Details = func(id string) (int, []byte, string, error) {
		events = append(events, "metadata:"+id)
		body := strings.ReplaceAll(detailFixture, "fixture_cart-0", "fixture_named-0")
		return 200, []byte(strings.ReplaceAll(body, "Public Game Title", "Named Game")), "", nil
	}
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "Renamed 1") || strings.Join(events, ",") != "metadata:fixture_named,wait,cart:fixture_named" {
		t.Fatal(summary, err, events)
	}
	if !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "favorites/Named Game [fixture_named].p8.png")), data) {
		t.Fatal("cart changed or alias lost")
	}
	if _, err := os.Stat(source); !os.IsNotExist(err) {
		t.Fatal("legacy alias remains", err)
	}
	s.Details = nil
	s.fetch = nil
	if summary, err = s.TidyFilenames(); err != nil || !strings.Contains(summary, "already tidy 1") {
		t.Fatal(summary, err)
	}
}

func TestTidyRejectsGeneralListingFallbackForNamedCart(t *testing.T) {
	p, h := roots(t)
	data := fixture(t, 41)
	source := filepath.Join(p, offline.CommonPath, "barp_the_balldragon.p8.png")
	put(t, source, data)
	s := New(p, h, nil, func(string) (int, []byte, string, error) {
		t.Fatal("downloaded an unbound alias")
		return 0, nil, "", nil
	})
	s.Details = func(string) (int, []byte, string, error) {
		return 200, []byte(strings.ReplaceAll(detailFixture, "90001,7,2", "0,7,2")), "", nil
	}
	summary, err := s.TidyFilenames()
	if err != nil || !strings.Contains(summary, "BBS identity unavailable: 1") || !bytes.Equal(read(t, source), data) {
		t.Fatal(summary, err)
	}
}

func TestTidySelectsMulticartTitleOnlyWithUniqueByteMatch(t *testing.T) {
	for _, tc := range []struct{ name, mode, want string }{
		{"second cart matches", "second", "Renamed 1"},
		{"uncached numeric ID matches", "uncached", "Renamed 1"},
		{"uncached numeric ID differs", "uncachedDifferent", "different BBS cart bytes: 1"},
		{"both carts match", "both", "ambiguous BBS cart title: 1"},
		{"neither cart matches", "neither", "no matching BBS cart title: 1"},
		{"one candidate unavailable", "unavailable", "cart title verification unavailable: 1"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p, h := roots(t)
			data, different := fixture(t, 42), fixture(t, 43)
			source := filepath.Join(p, offline.CommonPath, "90001.p8.png")
			put(t, source, data)
			if !strings.HasPrefix(tc.mode, "uncached") {
				put(t, offline.CachePath(h, "90001"), data)
			}
			other := strings.ReplaceAll(detailFixture, "fixture_cart-0", "other_cart-0")
			other = strings.ReplaceAll(other, "Public Game Title", "Other Game")
			row := other[strings.Index(other, "['other_cart-0'"):strings.Index(other, ",];")]
			multi := strings.Replace(detailFixture, ",];", ","+row+",];", 1)
			s := New(p, h, nil, func(id string) (int, []byte, string, error) {
				if id == "90001" && strings.HasPrefix(tc.mode, "uncached") {
					if tc.mode == "uncachedDifferent" {
						return 200, different, "", nil
					}
					return 200, data, "", nil
				}
				if id != "fixture_cart-0" && id != "other_cart-0" {
					t.Fatal("unexpected candidate", id)
				}
				if tc.mode == "unavailable" && id == "fixture_cart-0" {
					return 404, nil, "", nil
				}
				if tc.mode == "both" || (tc.mode != "neither" && id == "other_cart-0") {
					return 200, data, "", nil
				}
				return 200, different, "", nil
			})
			now := time.Unix(1700000000, 0)
			s.Now = func() time.Time { return now }
			s.Sleep = func(d time.Duration) error {
				if d != 15*time.Second {
					t.Fatal(d)
				}
				now = now.Add(d)
				return nil
			}
			s.Details = func(string) (int, []byte, string, error) { return 200, []byte(multi), "", nil }
			summary, err := s.TidyFilenames()
			if err != nil || !strings.Contains(summary, tc.want) {
				t.Fatal(summary, err)
			}
			if tc.mode == "second" || tc.mode == "uncached" {
				if !bytes.Equal(read(t, filepath.Join(p, offline.CommonPath, "Other Game [90001].p8.png")), data) {
					t.Fatal("wrong title or changed bytes")
				}
			} else {
				if !bytes.Equal(read(t, source), data) {
					t.Fatal("unresolved cart changed")
				}
				if _, err := offline.CachedTitle(h, "90001", data); err == nil {
					t.Fatal("saved an ambiguous title")
				}
			}
		})
	}
}
