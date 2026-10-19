// Command cart-thumbnails generates experimental Analogue Library cart artwork.
package main

import (
	"flag"
	"fmt"
	"io"
	"os"

	"github.com/zenibako/pico8pocket/thumbnail"
)

// run handles a single CLI invocation; no root is assumed implicitly.
func run(args []string, out io.Writer) int {
	flags := flag.NewFlagSet("cart-thumbnails", flag.ContinueOnError)
	flags.SetOutput(out)
	root := flags.String("root", "", "SD card root (required)")
	first := flags.Bool("first", false, "generate one test thumbnail")
	if err := flags.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return 0
		}
		return 2
	}
	if *root == "" || flags.NArg() != 0 {
		fmt.Fprintln(out, "Usage: cart-thumbnails --root <SD card> [--first]")
		return 2
	}
	limit := 0
	if *first {
		limit = 1
	}
	report, err := thumbnail.Generate(*root, limit)
	if err != nil {
		fmt.Fprintln(out, "Error:", err)
		return 1
	}
	fmt.Fprintf(out, "Generated: %d; unchanged: %d; skipped: %d\n", report.Generated, report.Unchanged, report.Skipped)
	for _, p := range report.Paths {
		fmt.Fprintln(out, p)
	}
	for _, warning := range report.Warnings {
		fmt.Fprintln(out, "Warning:", warning)
	}
	fmt.Fprintln(out, "Native picker display for cartridge slot 4 needs Pocket testing.")
	return 0
}

func main() { os.Exit(run(os.Args[1:], os.Stdout)) }
