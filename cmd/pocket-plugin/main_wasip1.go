//go:build tinygo && wasip1

package main

import (
	"errors"
	"strings"

	"github.com/extism/go-pdk"
	"github.com/zenibako/pico8pocket/offline"
	"github.com/zenibako/pico8pocket/pocketplugin"
)

//go:wasmimport extism:host/user print_msg
func printMsg(uint64) uint64

func logMessage(message string) {
	memory := pdk.AllocateString(message + "\n")
	defer memory.Free()
	printMsg(memory.Offset())
}

func fetchCart(id string) (int, []byte, string, error) {
	if err := offline.ValidateID(id); err != nil {
		return 0, nil, "", err
	}
	request := pdk.NewHTTPRequest(pdk.MethodGet, "https://www.lexaloffle.com/bbs/get_cart.php?cat=7&play_src=2&lid="+id)
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
	if memory.Length() > offline.MaxCart {
		return status, nil, retryAfter, errors.New("HTTP response exceeds 2 MiB cartridge limit")
	}
	if status != 200 {
		return status, nil, retryAfter, nil
	}
	return status, response.Body(), retryAfter, nil
}

const stateKey = "pico8-offline-conversation"

func newSession() *pocketplugin.Session {
	// Absolute preopen names avoid depending on WASI directory enumeration order.
	return pocketplugin.New("/pocket", "/host", logMessage, fetchCart)
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
