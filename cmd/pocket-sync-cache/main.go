// pocket-sync-cache imports explicit cache paths and installs without networking.
package main

import (
	"fmt"
	"io"
	"os"

	"github.com/zenibako/pico8pocket/offline"
	"github.com/zenibako/pico8pocket/pocketplugin"
)

const usage = "Usage: pocket-sync-cache import SOURCE_CACHE HOST_DIR both|arcade|sorcerer\n       pocket-sync-cache install HOST_DIR POCKET_ROOT both|arcade|sorcerer\n       pocket-sync-cache verify FILE.p8pack"

func run(args []string, out io.Writer) error {
	if len(args) == 0 {
		return fmt.Errorf("%s", usage)
	}
	switch args[0] {
	case "import":
		if len(args) != 4 {
			return fmt.Errorf("%s", usage)
		}
		count, err := offline.ImportCache(args[1], args[2], args[3])
		if err != nil {
			return err
		}
		_, err = fmt.Fprintf(out, "Imported %d verified cartridges into %s/pico8-offline-cache\n", count, args[2])
		return err
	case "install":
		if len(args) != 4 {
			return fmt.Errorf("%s", usage)
		}
		session := pocketplugin.New(args[2], args[1], func(message string) { fmt.Fprintln(out, message) }, nil)
		summary, err := session.Install(args[3], false)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintln(out, summary)
		return err
	case "verify":
		if len(args) != 2 {
			return fmt.Errorf("%s", usage)
		}
		data, err := offline.ReadBounded(args[1], offline.MaxPack)
		if err != nil {
			return err
		}
		carts, err := offline.DecodePack(data)
		if err != nil {
			return err
		}
		_, err = fmt.Fprintf(out, "Valid bundle: %d cartridges, %d bytes\n", len(carts), len(data))
		return err
	default:
		return fmt.Errorf("%s", usage)
	}
}
func main() {
	if err := run(os.Args[1:], os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
