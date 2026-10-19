//go:build tinygo && wasip1

package main

import (
	"github.com/extism/go-pdk"
	"github.com/zenibako/pico8pocket/pocketplugin"
)

//go:wasmimport extism:host/user print_msg
func printMsg(uint64) uint64

func logMessage(message string) {
	memory := pdk.AllocateString(message)
	defer memory.Free()
	printMsg(memory.Offset())
}

// The host mounts the card here. Answers cannot change this path.
var session = pocketplugin.New("/pocket", logMessage)

//go:wasmexport start
func start() int32 {
	output, err := session.Start()
	if err != nil {
		pdk.SetError(err)
		return 1
	}
	pdk.Output(output)
	return 0
}

//go:wasmexport handle_response
func handleResponse() int32 {
	output, err := session.Handle(pdk.Input())
	if err != nil {
		pdk.SetError(err)
		return 1
	}
	pdk.Output(output)
	return 0
}

func main() {}
