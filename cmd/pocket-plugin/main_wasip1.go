//go:build tinygo && wasip1

package main

import (
	"encoding/binary"
	"errors"
	"strings"
	"time"

	"github.com/extism/go-pdk"
	"github.com/zenibako/pico8pocket/bbs"
	"github.com/zenibako/pico8pocket/offline"
	"github.com/zenibako/pico8pocket/pocketplugin"
)

//go:wasmimport extism:host/user print_msg
func printMsg(uint64) uint64

//go:wasmimport extism:host/user open_url
func openURL(uint64) uint64

func logMessage(message string) {
	memory := pdk.AllocateString(message + "\n")
	defer memory.Free()
	printMsg(memory.Offset())
}

func fetchCart(id string) (int, []byte, string, error) {
	if err := offline.ValidateID(id); err != nil {
		return 0, nil, "", err
	}
	return fetchURL("https://www.lexaloffle.com/bbs/get_cart.php?cat=7&play_src=2&lid="+id, offline.MaxCart)
}
func fetchListing(order bbs.Order, page int, search string) (int, []byte, string, error) {
	url, err := bbs.ListingURL(order, page, search)
	if err != nil {
		return 0, nil, "", err
	}
	return fetchURL(url, bbs.MaxListing)
}
func fetchDetails(id string) (int, []byte, string, error) {
	url, err := bbs.DetailURL(id)
	if err != nil {
		return 0, nil, "", err
	}
	return fetchURL(url, bbs.MaxListing)
}
func fetchURL(url string, limit int) (int, []byte, string, error) {
	request := pdk.NewHTTPRequest(pdk.MethodGet, url)
	response := request.Send()
	memory := response.Memory()
	if memory.Offset() != 0 {
		defer memory.Free()
	}
	retryAfter := ""
	for key, value := range response.Headers() {
		if strings.EqualFold(key, "Retry-After") {
			retryAfter = value
		}
	}
	status := int(response.Status())
	if status == 429 {
		return status, nil, retryAfter, nil
	}
	if memory.Length() > uint64(limit) {
		return status, nil, retryAfter, errors.New("BBS response exceeds allowed size")
	}
	if status != 200 {
		return status, nil, retryAfter, nil
	}
	return status, response.Body(), retryAfter, nil
}

const stateKey = "pico8-offline-conversation"
const requestKey = "pico8-offline-last-request"

func newSession() *pocketplugin.Session {
	// Absolute preopen names avoid depending on WASI directory enumeration order.
	session := pocketplugin.New("/pocket", "/host", logMessage, fetchCart)
	session.List = fetchListing
	session.Details = fetchDetails
	session.OpenURL = func(url string) error {
		const prefix = "https://www.lexaloffle.com/bbs/?pid="
		if !strings.HasPrefix(url, prefix) || offline.ValidateID(strings.TrimPrefix(url, prefix)) != nil {
			return errors.New("invalid BBS page URL")
		}
		memory := pdk.AllocateString(url)
		defer memory.Free()
		openURL(memory.Offset())
		return nil
	}
	// This variable is intentionally independent of conversation state. Record it
	// before HTTP for cancellation, then refresh it after the response so a fresh
	// start retains the full cooldown without counting host-call setup latency.
	if saved := pdk.GetVar(requestKey); len(saved) == 8 {
		session.LastRequest = time.Unix(0, int64(binary.LittleEndian.Uint64(saved)))
	}
	session.OnRequest = func(stamp time.Time) {
		var saved [8]byte
		binary.LittleEndian.PutUint64(saved[:], uint64(stamp.UnixNano()))
		pdk.SetVar(requestKey, saved[:])
	}
	return session
}
func outputSession(session *pocketplugin.Session, output []byte, err error) int32 {
	if err != nil {
		pdk.SetError(err)
		return 1
	}
	state, err := session.Snapshot()
	if err != nil {
		pdk.SetError(err)
		return 1
	}
	pdk.SetVar(stateKey, state)
	pdk.Output(output)
	return 0
}

//go:wasmexport start
func start() int32 {
	pdk.RemoveVar(stateKey)
	session := newSession()
	output, err := session.Start()
	return outputSession(session, output, err)
}

//go:wasmexport handle_response
func handleResponse() int32 {
	session := newSession()
	if err := session.Restore(pdk.GetVar(stateKey)); err != nil {
		pdk.SetError(err)
		return 1
	}
	output, err := session.Handle(pdk.Input())
	return outputSession(session, output, err)
}
func main() {}
