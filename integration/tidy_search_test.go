package integration

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/zenibako/pico8pocket/internal/testcart"
	"github.com/zenibako/pico8pocket/offline"
)

const cakeListing = "<script id=cart_data_script>\npdat=[['90001',42,`Post`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,0,7,2,'0',[],2,21,7,`Cake Factory`,``]];</script>"
const emptyTidyListing = `<div id="listing_page_nav"></div><br> &nbsp [no posts found] <br><br></div>`

func TestTidyLookupRetryAndEmptySearchThroughWASI(t *testing.T) {
	h := newHarness(t, "host", "www.lexaloffle.com")
	data := testcart.PNG(92)
	source := filepath.Join(h.pocket, offline.CommonPath, "cards/mots_8_ball_pool.p8.png")
	write(t, source, data)
	previous := http.DefaultClient
	requests := 0
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		requests++
		status, body := 200, "<html>Temporarily unavailable</html>"
		if r.Method != "GET" || r.URL.Host != "www.lexaloffle.com" || r.URL.Scheme != "https" {
			t.Fatal(r.URL)
		}
		switch requests {
		case 1:
			if r.URL.Path != "/bbs/" || r.URL.Query().Get("pid") != "mots_8_ball_pool" {
				t.Fatal(r.URL)
			}
			status, body = 404, ""
		case 2, 3:
			if r.URL.Path != "/bbs/lister.php" || r.URL.Query().Get("search") != "mots 8 ball pool" || r.URL.Query().Get("page") != "1" {
				t.Fatal(r.URL)
			}
			if requests == 3 {
				body = emptyTidyListing
			}
		default:
			t.Fatal("unexpected request", r.URL)
		}
		return &http.Response{StatusCode: status, Header: make(http.Header), Body: io.NopCloser(strings.NewReader(body)), Request: r}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	main := menu(t, h.call("start", nil))
	library := menu(t, h.call("handle_response", answer(t, main, "Library")))
	prompt := menu(t, h.call("handle_response", answer(t, library, "Tidy filenames")))
	if !strings.Contains(prompt.Choice.Query, "BBS cart metadata missing") || !slices.Contains(prompt.Choice.Choices, "Retry") || slices.Contains(prompt.Choice.Choices, "Check more results") {
		t.Fatal("misleading failure choices", prompt)
	}
	prompt = menu(t, h.call("handle_response", answer(t, prompt, "Retry")))
	if !strings.Contains(prompt.Choice.Query, "No verified byte match") || !strings.Contains(prompt.Choice.Query, "Checked 0 identities; 0 verified matches") || slices.Contains(prompt.Choice.Choices, "Retry") || slices.Contains(prompt.Choice.Choices, "Check more results") {
		t.Fatal("empty search treated as failure or more results", prompt)
	}
	done := menu(t, h.call("handle_response", answer(t, prompt, "Skip file")))
	got, err := os.ReadFile(source)
	if requests != 3 || !strings.Contains(done.Choice.Query, "unmatched 1") || err != nil || !bytes.Equal(got, data) {
		t.Fatal("skip changed source or queue", done, requests, err)
	}
	h.assertPreserved()
}

func TestTidyTitleCollisionAndAutomaticPaginationThroughWASI(t *testing.T) {
	h := newHarness(t, "host", "www.lexaloffle.com")
	data, wrong := testcart.PNG(85), testcart.PNG(86)
	common := filepath.Join(h.pocket, offline.CommonPath)
	source := filepath.Join(common, "favorites/cake_factory.p8.png")
	write(t, source, data)
	write(t, offline.CachePath(h.host, "cake_factory"), wrong)
	pack := []byte("preserved bundle")
	write(t, filepath.Join(h.pocket, offline.PackPath), pack)
	previous := http.DefaultClient
	var requests []time.Time
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		requests = append(requests, time.Now())
		if r.Method != "GET" || r.URL.Scheme != "https" || r.URL.Host != "www.lexaloffle.com" {
			t.Fatal(r.URL)
		}
		var body []byte
		switch len(requests) {
		case 1:
			if r.URL.Path != "/bbs/" || r.URL.Query().Get("pid") != "cake_factory" {
				t.Fatal(r.URL)
			}
			body = []byte("<script id=cart_data_script>\npdat=[['cake_factory-0',42,`Wrong Game`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,90002,7,2,'0',[],0,1,,``,``]];</script>")
		case 2:
			if r.URL.Path != "/bbs/get_cart.php" || r.URL.Query().Get("lid") != "cake_factory" {
				t.Fatal(r.URL)
			}
			body = wrong
		case 3:
			if r.URL.Path != "/bbs/lister.php" || r.URL.Query().Get("search") != "cake factory" || r.URL.Query().Get("page") != "1" {
				t.Fatal(r.URL)
			}
			body = []byte(cakeListing + "<a href='#mode=carts&page=2'>Next</a>")
		case 4:
			if r.URL.Path != "/bbs/get_cart.php" || r.URL.Query().Get("lid") != "90001" {
				t.Fatal(r.URL)
			}
			body = data
		case 5:
			if r.URL.Path != "/bbs/lister.php" || r.URL.Query().Get("search") != "cake factory" || r.URL.Query().Get("page") != "2" {
				t.Fatal(r.URL)
			}
			if got, err := os.ReadFile(source); err != nil || !bytes.Equal(got, data) {
				t.Fatal("paired before later page was checked", err)
			}
			body = []byte(emptyTidyListing)
		default:
			t.Fatal("unexpected request", r.URL)
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body)), Request: r}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	main := menu(t, h.call("start", nil))
	library := menu(t, h.call("handle_response", answer(t, main, "Library")))
	// Five real WASI HTTP boundary calls retain production's 15-second cooldown.
	done := menu(t, h.callWithin("handle_response", answer(t, library, "Tidy filenames"), 75*time.Second))
	if !strings.Contains(done.Choice.Query, "Renamed 1") || len(requests) != 5 {
		t.Fatal(done, requests)
	}
	for i := 1; i < len(requests); i++ {
		if gap := requests[i].Sub(requests[i-1]); gap < 15*time.Second {
			t.Fatal("cooldown", gap)
		}
	}
	got, err := os.ReadFile(filepath.Join(common, "favorites/Cake Factory [90001].p8.png"))
	if err != nil || !bytes.Equal(got, data) || !bytes.Equal(h.pack(), pack) {
		t.Fatal("changed cart/bundle", err)
	}
	got, err = os.ReadFile(offline.CachePath(h.host, "cake_factory"))
	if err != nil || !bytes.Equal(got, wrong) {
		t.Fatal("alias cache replaced", err)
	}
	done = menu(t, h.call("handle_response", answer(t, done, "Tidy filenames")))
	if !strings.Contains(done.Choice.Query, "already tidy 1") || len(requests) != 5 {
		t.Fatal(done, requests)
	}
	h.assertPreserved()
}

func TestTidyAmbiguityAndEditedSearchThroughWASI(t *testing.T) {
	h := newHarness(t, "pocket", "www.lexaloffle.com")
	data := testcart.PNG(87)
	source := filepath.Join(h.pocket, offline.CommonPath, "mystery.p8.png")
	write(t, source, data)
	write(t, offline.CachePath(h.host, "90001"), data)
	write(t, offline.CachePath(h.host, "90002"), data)
	previous := http.DefaultClient
	requests := 0
	http.DefaultClient = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		requests++
		status, body := 200, []byte(cakeListing)
		switch requests {
		case 1:
			if r.URL.Path != "/bbs/" || r.URL.Query().Get("pid") != "mystery" {
				t.Fatal(r.URL)
			}
			status, body = 404, nil
		case 2:
			if r.URL.Path != "/bbs/lister.php" || r.URL.Query().Get("search") != "mystery" {
				t.Fatal(r.URL)
			}
			body = []byte(strings.Replace(cakeListing, "]]", "],['90002',42,`Post`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,0,7,2,'0',[],2,21,7,`Other Identity`,``]]", 1))
		case 3:
			if r.URL.Path != "/bbs/lister.php" || r.URL.Query().Get("search") != "cake revised" {
				t.Fatal(r.URL)
			}
		default:
			t.Fatal("unexpected request", r.URL)
		}
		return &http.Response{StatusCode: status, Header: make(http.Header), Body: io.NopCloser(bytes.NewReader(body)), Request: r}, nil
	})}
	t.Cleanup(func() { http.DefaultClient = previous })
	main := menu(t, h.call("start", nil))
	library := menu(t, h.call("handle_response", answer(t, main, "Library")))
	prompt := menu(t, h.call("handle_response", answer(t, library, "Tidy filenames")))
	if !strings.Contains(prompt.Choice.Query, "Multiple verified identities") {
		t.Fatal(prompt)
	}
	if got, err := os.ReadFile(source); err != nil || !bytes.Equal(got, data) {
		t.Fatal("source changed before decision", err)
	}
	out := h.call("handle_response", answer(t, prompt, "Edit search"))
	var text struct{ Text struct{ Name string } }
	if err := json.Unmarshal(out, &text); err != nil || text.Text.Name == "" {
		t.Fatal(string(out), err)
	}
	input, _ := json.Marshal(map[string]any{"Answer": map[string]string{"name": text.Text.Name, "value": "cake revised"}})
	done := menu(t, h.call("handle_response", input))
	if !strings.Contains(done.Choice.Query, "Renamed 1") || requests != 3 {
		t.Fatal(done, requests)
	}
	if got, err := os.ReadFile(filepath.Join(h.pocket, offline.CommonPath, "Cake Factory [90001].p8.png")); err != nil || !bytes.Equal(got, data) {
		t.Fatal("incorrect resumed rename", err)
	}
	h.assertPreserved()
}
