package pocketplugin

import (
	"fmt"
	"path/filepath"
	"strconv"

	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
)

// ListingFetchFunc is the transport for one explicitly requested public BBS page.
type ListingFetchFunc func(bbs.Order, int, string) (status int, body []byte, retryAfter string, err error)
type browserState struct {
	Query      string
	Order      bbs.Order
	Page       int
	Listing    bbs.Page
	Selected   int
	Downloaded bool
}

func validateBrowser(b *browserState) error {
	if b == nil {
		return nil
	}
	if _, err := bbs.ListingURL(b.Order, b.Page, b.Query); err != nil {
		return err
	}
	if len(b.Listing.Carts) > 60 || b.Selected < -1 || b.Selected >= len(b.Listing.Carts) || (b.Downloaded && b.Selected < 0) {
		return fmt.Errorf("invalid saved BBS listing")
	}
	for _, cart := range b.Listing.Carts {
		if _, err := strconv.ParseUint(cart.ID, 10, 64); err != nil || offline.ValidateID(cart.ID) != nil || cart.Title == "" || cart.Author == "" {
			return fmt.Errorf("invalid saved BBS cart")
		}
	}
	return nil
}
func (s *Session) loadListing(order bbs.Order, page int, query string) ([]byte, error) {
	fail := func(err error) ([]byte, error) {
		message := "Stopped: " + err.Error()
		if s.browser != nil {
			return s.showListing(message)
		}
		return s.browseMenu(message)
	}
	if s.List == nil {
		return fail(fmt.Errorf("BBS listing unavailable in this host"))
	}
	if _, err := bbs.ListingURL(order, page, query); err != nil {
		return fail(err)
	}
	if err := s.beforeRequest("BBS " + string(order) + " listing"); err != nil {
		return fail(err)
	}
	status, body, retry, err := s.List(order, page, query)
	s.recordRequest()
	if err == nil {
		err = httpStatus(status, retry)
	}
	if err != nil {
		return fail(err)
	}
	listing, err := bbs.Parse(body, page)
	if err != nil {
		return fail(err)
	}
	if err = s.cancel(); err != nil {
		return fail(err)
	}
	s.browser = &browserState{Query: query, Order: order, Page: page, Listing: listing, Selected: -1}
	return s.showListing("")
}
func (s *Session) showListing(message string) ([]byte, error) {
	b := s.browser
	b.Selected = -1
	b.Downloaded = false
	label := "Recent carts"
	if b.Order == bbs.Popular {
		label = "New & popular carts"
	}
	if b.Query != "" {
		label = "Search: " + b.Query
	}
	query := fmt.Sprintf("%s — page %d. Choose a cart for details and download.", label, b.Page)
	if len(b.Listing.Carts) == 0 {
		query += " No carts on this page."
	}
	if message != "" {
		query = message + "\n" + query
	}
	choices := make([]string, 0, len(b.Listing.Carts)+3)
	for _, cart := range b.Listing.Carts {
		choices = append(choices, cartChoice(cart))
	}
	if b.Page > 1 {
		choices = append(choices, "Previous page")
	}
	if b.Listing.HasNext {
		choices = append(choices, "Next page")
	}
	if b.Query != "" {
		choices = append(choices, "New search")
	}
	choices = append(choices, "Back to Browse", "Back to main menu")
	return s.prompt(query, choices)
}
func cartChoice(cart bbs.Cart) string {
	return fmt.Sprintf("%s — %s (#%s)", cart.Title, cart.Author, cart.ID)
}
func (s *Session) showCart(message string) ([]byte, error) {
	cart := s.browser.Listing.Carts[s.browser.Selected]
	query := fmt.Sprintf("%s by %s\n%s\nDownload this cart to the Pocket? Verified cache is reused. Game compatibility depends on the core.", cart.Title, cart.Author, cart.URL())
	choices := []string{"Open BBS page", "Back to list", "Back to main menu"}
	if s.browser.Downloaded {
		query = fmt.Sprintf("Downloaded %s to Pocket.\nSelect bbs/%s.p8.png in the Pocket asset browser.", cart.Title, cart.ID)
	} else {
		choices = append([]string{"Download to Pocket"}, choices...)
	}
	if message != "" {
		query = message + "\n" + query
	}
	return s.prompt(query, choices)
}
func (s *Session) handleBrowser(value string) ([]byte, error) {
	b := s.browser
	if value == "New search" {
		return s.searchPrompt("")
	}
	if value == "Back to Browse" {
		return s.browseMenu("Browse public BBS carts.")
	}
	if value == "Back to main menu" {
		return s.menu("Choose an offline collection or browse public BBS carts.")
	}
	if b.Selected >= 0 {
		cart := b.Listing.Carts[b.Selected]
		switch value {
		case "Back to list":
			return s.showListing("")
		case "Open BBS page":
			if s.OpenURL == nil {
				return s.showCart("Opening a browser is unavailable in this host.")
			}
			if err := s.OpenURL(cart.URL()); err != nil {
				return s.showCart("Stopped: " + err.Error())
			}
			return s.showCart("Opened BBS page.")
		case "Download to Pocket":
			if err := s.downloadStandalone(cart.ID); err != nil {
				return s.showCart("Stopped: " + err.Error())
			}
			b.Downloaded = true
			return s.showCart("")
		}
	} else {
		if value == "Next page" && b.Listing.HasNext {
			return s.loadListing(b.Order, b.Page+1, b.Query)
		}
		if value == "Previous page" && b.Page > 1 {
			return s.loadListing(b.Order, b.Page-1, b.Query)
		}
		for i, cart := range b.Listing.Carts {
			if value == cartChoice(cart) {
				b.Selected = i
				return s.showCart("")
			}
		}
	}
	return nil, fmt.Errorf("invalid BBS browser action")
}
func httpStatus(status int, retry string) error {
	if status == 200 {
		return nil
	}
	if status == 429 {
		if retry == "" {
			retry = "not supplied"
		}
		return fmt.Errorf("HTTP 429; Retry-After: %s; no automatic retry; wait before trying again", retry)
	}
	return fmt.Errorf("HTTP %d", status)
}
func (s *Session) downloadStandalone(id string) error {
	if err := offline.ValidateID(id); err != nil {
		return err
	}
	if err := s.cancel(); err != nil {
		return err
	}
	data, err := offline.CachedCart(s.hostRoot, id)
	if err != nil {
		if s.fetch == nil {
			return fmt.Errorf("verified cache unavailable; network disabled")
		}
		if err = s.beforeRequest("cart #" + id); err != nil {
			return err
		}
		status, body, retry, fetchErr := s.fetch(id)
		s.recordRequest()
		if fetchErr != nil {
			return fetchErr
		}
		if err = httpStatus(status, retry); err != nil {
			return err
		}
		if err = offline.ValidatePNG(body); err != nil {
			return err
		}
		if err = s.cancel(); err != nil {
			return err
		}
		if err = offline.AtomicWrite(offline.CachePath(s.hostRoot, id), body); err != nil {
			return err
		}
		data = body
	}
	if err = s.cancel(); err != nil {
		return err
	}
	return offline.AtomicWrite(filepath.Join(s.pocketRoot, offline.CartPath, id+".p8.png"), data)
}
