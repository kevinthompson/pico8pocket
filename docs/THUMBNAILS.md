# Experimental Analogue picker artwork

This tool turns your cart's existing label into an Analogue Library image.
**Display in Analogue's native picker is unverified.** Pico8Pocket loads its
cartridge through slot 4 while slot 0 is a fixed instance. Creating a valid
Library image does not establish that the picker will use it.

## One-cart Pocket test

1. Put one `.p8.png` or labeled `.p8` in
   `Assets/pico8/common/cards/` on your SD card.
2. Run **Generate one test thumbnail** in the Pocket Sync plugin, or run the
   native command below.
3. Safely eject the card. Launch Pico8Pocket, enable **Detailed Browser** in
   Analogue OS if available, and highlight that cart in the native picker.
4. Check whether its label appears. Record the Pocket firmware version and
   whether this was the initial picker or Core Settings → Cartridge.

If no artwork appears, keep the current picker and report the result before
building a larger thumbnail collection. No runner or save-slot changes are
needed for this test.

## Pocket Sync plugin

The plugin follows the same Go + TinyGo/Extism architecture as Butterscotch.
It has short menus: one test thumbnail, all thumbnails, help and exit.
It reads only the connected card and needs no network permission.

Until a complete release is published, `plugin.json` and `plugin.wasm` are
local test artifacts. Pocket Sync's **Add plugin via URL** requires them in
this repository's latest stable release; it cannot install a local build
from the repository source alone. For a local development install, use a
Pocket Sync development build or use the native CLI for the hardware pilot.

## Native CLI

Extract the bundle for your computer; Apple silicon Macs use `darwin-arm64`.
For a single test on macOS:

```sh
./cart-thumbnails --root "/Volumes/YOUR_CARD" --first
```

Windows (PowerShell):

```powershell
.\cart-thumbnails.exe --root "E:\" --first
```

Linux:

```sh
./cart-thumbnails --root /media/yourname/YOUR_CARD --first
```

Omit `--first` to process every supported cart recursively. The pilot chooses
the first usable label in sorted path order; its output path is printed.
Carts with the same complete file bytes share the same artwork file.

## Files and preservation

- Inputs: `Assets/pico8/common/cards/`, including subfolders.
- Outputs: `System/Library/Images/pico8/<eight-digit-crc32>.bin`.
- `.p8.png`: use the standard 128×128 label at (16, 24) in the 160×205 image.
- `.p8`: use its optional `__label__` section, including extended palette colors.
- Carts without labels and malformed inputs are skipped and reported.
  Symlink entries are ignored; symlinked card/output paths are rejected.
- Existing identical artwork is unchanged; differing artwork is preserved.
- Carts, saves and unrelated artwork are never deleted or modified.

Images use Analogue's eight-byte Library header and opaque BGRA pixels rotated
90 degrees anticlockwise. The filename uses IEEE CRC32 of the original file,
not decoded ROM bytes. This follows the documented single-asset convention;
slot-4 picker association is the experimental part.

## Build and verify

Go 1.25+, TinyGo 0.43.0 and Binaryen 133 are required to build all artifacts.
The host CLI and WASI CLI use standard Go; the Pocket Sync plugin uses TinyGo.
The new Go tooling is independently implemented under this repository's MIT
license; it does not copy Butterscotch's AGPL implementation.

```sh
make thumbnail-test
make thumbnail-tools
REQUIRE_PLUGIN_TESTS=1 POCKET_PLUGIN_WASM="$PWD/build/thumbnail-tools/plugin.wasm" CART_THUMBNAILS_WASM="$PWD/build/thumbnail-tools/cart-thumbnails.wasm" go test ./integration -count=1
```

The build writes six native bundles, `cart-thumbnails.wasm`, `plugin.wasm`,
`plugin.json`, instructions, licenses/notices and `SHA256SUMS` under
`build/thumbnail-tools/`. These contain no user cartridges or generated images.
The runtime source, metadata and pinned dependencies are unchanged.

References: [Analogue Library format](https://www.analogue.co/developer/docs/openfpga/library),
[PICO-8 label capture](https://www.lexaloffle.com/dl/docs/pico-8_manual.html).
