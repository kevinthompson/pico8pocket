package bbs

import (
	"net/url"
	"strings"
	"testing"
)

// Project-owned metadata with the BBS lister's observed 24-field row shape.
const sample = "<a href=#sub=2&page=2&mode=carts&orderby=ts>Next</a><script id=cart_data_script>\npdat=[\n['90001', 42, `Forum title`,\"/bbs/thumbs/example.png\",256,170.5,\"2026-10-20 01:00:00\",7,\"Author_8\",\"date\",0,\"\",10,3,0,7,2,'0',[\"puzzle\",],2,21,7,`A &amp; B`,``],\n];\nvar unrelated=1;</script>"

// Empty lister.php searches omit cart_data_script entirely.
const emptyListing = `<div id="listing_page_nav" style="position:relative"></div>
<div>Searching for <b>Unlisted Game</b></div><br><br><br> &nbsp [no posts found] <br><br>
</div>`

func TestExplicitEmptyListingWithoutCartMetadata(t *testing.T) {
	p, err := Parse([]byte(emptyListing), 1)
	if err != nil || len(p.Carts) != 0 || p.HasNext {
		t.Fatalf("empty search: %+v %v", p, err)
	}
	for _, bad := range []string{
		"<html>Temporarily unavailable</html>",
		"[no posts found]",
		strings.Replace(emptyListing, "listing_page_nav", "other", 1),
		strings.Replace(emptyListing, "[no posts found]", "Loading...", 1),
		emptyListing + "<script id=cart_data_script>broken</script>",
		"<script>\npdat=[broken];</script>" + emptyListing,
	} {
		if _, err := Parse([]byte(bad), 1); err == nil {
			t.Fatalf("accepted unknown or malformed listing: %s", bad)
		}
	}
	if _, err := ParseDetail([]byte(emptyListing), "90001"); err == nil {
		t.Fatal("empty listing accepted as cart identity metadata")
	}
}

func TestPublicListingMetadataAndPagination(t *testing.T) {
	p, err := Parse([]byte(sample), 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Carts) != 1 || p.Carts[0] != (Cart{ID: "90001", Title: "A & B", Author: "Author_8"}) || !p.HasNext {
		t.Fatalf("%+v", p)
	}
	// Numeric post IDs resolve get_cart.php; display titles are not cart aliases.
	if !strings.Contains(p.Carts[0].URL(), "?pid=90001") {
		t.Fatal(p.Carts[0])
	}
	p, err = Parse([]byte(strings.Replace(sample, "page=2", "page=1", 1)), 2)
	if err != nil || p.HasNext {
		t.Fatalf("%+v %v", p, err)
	}
}
func TestListingOnlyReadsLiteralData(t *testing.T) {
	for _, bad := range []string{
		strings.Replace(sample, "`A &amp; B`", "`${fetch('https://evil.invalid')}`", 1),
		strings.Replace(sample, "`A &amp; B`", "evil()", 1),
		strings.Replace(sample, "'90001'", "'../escape'", 1),
		strings.Replace(sample, "`A &amp; B`", "`bad\\u0000title`", 1),
		strings.Replace(sample, "<script id=cart_data_script>", "<script>", 1),
		strings.Replace(sample, "\n];", "\n", 1),
		sample + strings.Repeat(" ", MaxListing),
	} {
		if _, err := Parse([]byte(bad), 1); err == nil {
			t.Fatalf("accepted invalid listing: %.100s", bad)
		}
	}
}
func TestDuplicateCartsAndWrongConsoleRejected(t *testing.T) {
	row := sample[strings.Index(sample, "['90001'"):strings.Index(sample, "\n];")]
	duplicate := strings.Replace(sample, "\n];", row+"\n];", 1)
	if _, err := Parse([]byte(duplicate), 1); err == nil {
		t.Fatal("duplicate carts accepted")
	}
	wrong := strings.Replace(sample, ",21,7,", ",21,8,", 1)
	if _, err := Parse([]byte(wrong), 1); err == nil {
		t.Fatal("other console accepted")
	}
}
func TestListingURLConstrainsSourceAndSort(t *testing.T) {
	for _, tc := range []struct {
		order Order
		want  string
	}{
		{Recent, "https://www.lexaloffle.com/bbs/lister.php?cat=7&mode=carts&orderby=ts&page=2&sub=2&use_hurl=1"},
		{Popular, "https://www.lexaloffle.com/bbs/lister.php?cat=7&mode=carts&orderby=ts&page=2&popular=1&sub=2&use_hurl=1"},
	} {
		got, err := ListingURL(tc.order, 2, "")
		if err != nil || got != tc.want {
			t.Fatalf("%s %v", got, err)
		}
	}
	for _, tc := range []struct {
		order Order
		page  int
	}{{Order("https://evil.invalid"), 1}, {Recent, 0}, {Recent, MaxPage + 1}} {
		if _, err := ListingURL(tc.order, tc.page, ""); err == nil {
			t.Fatal("invalid source accepted")
		}
	}
}

func TestSparseArrayEntriesCountTowardLimit(t *testing.T) {
	bad := strings.Replace(sample, `["puzzle",]`, "["+strings.Repeat(",", 9000)+"]", 1)
	if _, err := Parse([]byte(bad), 1); err == nil {
		t.Fatal("accepted 9000 sparse values past metadata node budget")
	}
}

func TestSearchURLPreservesTextWithoutChangingSource(t *testing.T) {
	query := "雪 &cat=8+#?"
	got, err := ListingURL(Recent, 2, query)
	if err != nil {
		t.Fatal(err)
	}
	u, err := url.Parse(got)
	if err != nil {
		t.Fatal(err)
	}
	q := u.Query()
	if u.Scheme != "https" || u.Host != "www.lexaloffle.com" || u.Path != "/bbs/lister.php" || u.Fragment != "" || q.Get("search") != query || q.Get("cat") != "7" || q.Get("page") != "2" {
		t.Fatal(got)
	}
	for _, invalid := range []string{strings.Repeat("a", 121), "bad\nquery", "\x00", string([]byte{255})} {
		if _, err := ListingURL(Recent, 1, invalid); err == nil {
			t.Fatalf("accepted search %q", invalid)
		}
	}
}

const detailSample = "<script id=cart_data_script>\npdat=[['fixture_cart-0',42,`Public Game Title`,\"thumb\",256,170,\"date\",7,\"Author\",\"date\",0,\"\",1,2,90001,7,2,'0',[],0,1,,``,``],];</script>"

func TestDetailMetadataBindsRequestedPostAndFallsBackToPublicTitle(t *testing.T) {
	cart, err := ParseDetail([]byte(detailSample), "90001")
	if err != nil || cart.Title != "Public Game Title" || cart.ID != "90001" {
		t.Fatal(cart, err)
	}
	for _, bad := range []string{
		strings.Replace(detailSample, "90001,7,2", "90002,7,2", 1),
		strings.Replace(detailSample, "90001,7,2", "90001.1,7,2", 1),
		strings.Replace(detailSample, "90001,7,2", "90001,8,2", 1),
		strings.Replace(detailSample, "90001,7,2", "90001,7,1", 1),
		strings.Replace(detailSample, "'fixture_cart-0'", "'../bad'", 1),
		strings.Replace(detailSample, "`Public Game Title`", "evil()", 1),
	} {
		if _, err := ParseDetail([]byte(bad), "90001"); err == nil {
			t.Fatalf("accepted unbound detail: %s", bad)
		}
	}
	if _, err := DetailURL("../escape"); err == nil {
		t.Fatal("unsafe detail URL")
	}
}

func TestDetailAliasesResolveOnlyTheirOwnCart(t *testing.T) {
	other := strings.ReplaceAll(detailSample, "fixture_cart-0", "other_cart-0")
	other = strings.ReplaceAll(other, "Public Game Title", "Other Game")
	row := other[strings.Index(other, "['other_cart-0'"):strings.Index(other, ",];")]
	multi := strings.Replace(detailSample, ",];", ","+row+",];", 1)
	for _, id := range []string{"fixture_cart", "fixture_cart-0"} {
		cart, err := ParseDetail([]byte(multi), id)
		if err != nil || cart.Title != "Public Game Title" || cart.ID != "90001" {
			t.Fatal(cart, err)
		}
		url, err := DetailURL(id)
		if err != nil || url != "https://www.lexaloffle.com/bbs/?pid="+id {
			t.Fatal(url, err)
		}
	}
	for _, id := range []string{"fixture", "fixture_cart-1", "../escape", "a&pid=90001", "", "9007199254740992", "0"} {
		if _, err := ParseDetail([]byte(multi), id); err == nil {
			t.Fatal("unbound alias accepted", id)
		}
	}
}

func TestDetailCandidatesRetainDistinctCartsWithinOnePost(t *testing.T) {
	other := strings.ReplaceAll(detailSample, "fixture_cart-0", "other_cart-0")
	other = strings.ReplaceAll(other, "Public Game Title", "Other Game")
	row := other[strings.Index(other, "['other_cart-0'"):strings.Index(other, ",];")]
	multi := strings.Replace(detailSample, ",];", ","+row+",];", 1)
	if _, err := ParseDetail([]byte(multi), "90001"); err == nil {
		t.Fatal("ambiguous numeric post guessed a title")
	}
	carts, err := ParseDetails([]byte(multi), "90001")
	if err != nil || len(carts) != 2 {
		t.Fatal(carts, err)
	}
	if carts[0].Alias != "fixture_cart-0" || carts[0].Title != "Public Game Title" || carts[1].Alias != "other_cart-0" || carts[1].Title != "Other Game" {
		t.Fatal(carts)
	}
	duplicate := strings.ReplaceAll(multi, "other_cart-0", "fixture_cart-0")
	if _, err := ParseDetails([]byte(duplicate), "90001"); err == nil {
		t.Fatal("duplicate candidate identity accepted")
	}
	wrongPost := strings.ReplaceAll(multi, "90001,7,2", "90002,7,2")
	if _, err := ParseDetail([]byte(wrongPost), "90001"); err == nil {
		t.Fatal("wrong post accepted")
	}
}
