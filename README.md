# Pico8Pocket

PICO-8-compatible player for the Analogue Pocket, running on the
[openfpgaOS RISC-V environment](https://github.com/openfpgaOS/openfpgaSDK).

The compatibility target is PICO-8 **v0.2.7**. Both `.p8` and `.p8.png`
cartridges are in scope. The current alpha executes P8 Lua through z8lua,
decodes text and PNG carts, renders a 128×128 indexed framebuffer and packages
a native RV32 openFPGA core for Pocket.

Read [a personal note from the creator](ABOUT_ME.md) about how Pico8Pocket
started and how you can help improve it.

## Current state

- Host tests cover `.p8`, current and legacy compressed `.p8.png`, ZIP
  detection, integer scaling, P8 Lua execution and graphics/input calls.
  Celeste is the priority test cart: a locally supplied
  `assets/cards/celeste.p8.png` and/or `celeste_classic_2.p8.png` (never
  committed) adds an acceptance test to `make test` (600 frames of scripted
  play, then a mid-game save state that must replay the next 90 frames
  exactly), and `make celeste` compares frame output and host instructions
  per frame with the baseline recorded by `make celeste-baseline`.
- APF launches the core through a core-specific instance descriptor in slot 0;
  the Pocket file picker binds the selected cartridge to slot 4.
- `_init`, `_update`/`_update60` and `_draw` run at cartridge-selected 30 or
  60 FPS. Top-level and `_init` code also run cooperatively, so cartridges
  built around an infinite loop and `flip()` yield real frames instead of
  locking during load.
- Overloaded cartridges use adaptive render decimation: gameplay and input
  keep updating at full rate while `_draw` and video presentation fall back
  independently (60 FPS carts to 30, 20 or 15 FPS; 30 FPS carts to 15). This
  prevents effect-heavy scenes from slowing the entire game clock. A 30 FPS
  frame keeps every draw while its measured cost fits 32 ms of the 33.3 ms
  frame (15 ms at 60 FPS). The profiler's white number reports the resulting
  visible frame rate.
- The compatibility layer includes scaled `sspr`, texture-mapped `tline`,
  `oval`/`ovalfill`, patterned fills, 16/32-bit memory access, draw-state RAM
  aliases, remappable sprite/map memory and PICO-8's 32 KiB big-map mode.
- The audio backend implements `sfx()`/`music()`, four channels, the eight
  built-in waveforms, SFX loops, music patterns, fades and the standard
  slide/vibrato/drop/fade/arpeggio effects. Voices are synthesized at 24 kHz
  (PICO-8 itself runs at 22,050 Hz) and interpolated to the 48 kHz stereo
  output, entirely in integer/fixed point to leave the RISC-V CPU available
  for the Lua VM. Muting in the system menu skips synthesis altogether while
  music and effects keep their timing, so a CPU-heavy cart can be muted to
  gain a few milliseconds per frame.
- The current hardware-safe path scales 128×128 to 256×256 inside a proven
  320×288 openfpgaOS scanout. Pocket then scales the source 5×, producing a
  correct 1280×1280 image. Native 1408×1408 (11×) remains the target, but the
  stock bitstream's direct 128×128 mode crops the image on real hardware and
  is disabled until its scanout timing is fixed.
- Hardware testing showed that the experimental soft full-screen scaler was
  too blurry, so it has been removed. `2X` remains the default crisp,
  pixel-perfect path while native 11× output is investigated separately.
- Drawing uses an unpacked one-byte-per-pixel working surface and lazily
  synchronizes PICO-8's packed screen RAM for memory APIs. This removes the
  packed read-modify-write from the common sprite/pixel path and maps the
  display palette during the existing 2× presentation pass.
- Default controls are `B = O`, `A = X`, `Start = Pause`; `Y` and `X` are
  alternate `O` and `X` buttons. The in-core menu has a global profile plus a
  content-hash-keyed override for each cartridge; D-pad, A/B/X/Y, L/R and Start
  can all be reassigned while Select remains reserved for the system UI.
- Pressing Select opens the PICO-8-styled system menu and fully pauses the
  cartridge with a short audio fade. Holding Select and pressing the physical X
  button, or pressing X from the main system menu, toggles the opt-in
  profiler. Its labelled rows show logical FPS (`L`), visible FPS (`V`), update
  time (`U`), draw time (`D`), audio time (`A`), presentation time (`P`), the
  active render divisor (`R`), a Lua activity count (`K`) and the full runtime
  step the frame-skip scheduler measures (`F`: update and draw plus audio
  serviced mid-frame, garbage collection and frame setup). A second column counts sprite/map work (`S`), primitive
  graphics calls (`G`), memory/map/flag access (`M`), draw-state changes (`C`),
  text calls (`T`), input reads (`B`) and helpers such as `rnd`, `all` and
  `foreach` (`Q`). The profiler is hidden on every fresh launch.
- The paused cartridge behind the system menu is rendered through a dedicated
  15%-brightness palette before the checkerboard shade is applied, while menu
  text and panels retain their full palette brightness.
- Controller state is sampled from a lightweight Lua instruction hook while a
  cartridge frame is running. Short gameplay presses and system-button edges
  are retained until the main loop consumes them, including when emulation is
  below its target frame rate. Raw P8SCII and UTF-8 button-glyph constants are
  registered as the six standard buttons, matching compact PNG carts that use
  `btn(⬅️)`, `btn(🅾️)` or `btn(❎)` instead of numeric button indices.
- `.p8` text carts have their UTF-8 glyphs converted back to P8SCII bytes on
  load, and `print()` implements PICO-8's text engine: the full default font,
  custom fonts and P8SCII control codes, including memory pokes. Drawing can
  be redirected to the sprite sheet through `0x5f55`.
- Nested PICO-8 shorthand statements now share their terminating source
  newline correctly. This fixes minified cartridges such as Moss Moss where a
  nested one-line `if` previously swallowed the following movement code into
  the wrong branch.
- The interpreter is tuned for os20's dual-issue 90 MHz RV32 CPU (no
  double-precision FPU, 32 KiB instruction and 64 KiB data caches, 14 KiB of
  zero-wait BRAM that holds the dispatch loop). z8lua is patched (`patches/`) to keep the program
  counter in a register, check its service hook only at jumps and calls,
  cache the hash slot of every constant-name table access, compare numbers,
  strings and nil inline, follow `__index` chains for `_ENV` objects, call
  PICO-8 API functions through a short path and format numbers in fixed
  point. `time()`, `all()` and fill patterns avoid software doubles,
  per-element API calls and per-pixel work. Scaled sprites and ellipses avoid
  per-pixel division/64-bit implicit-equation work.
- The menu provides Quick Save, nine numbered state slots, 64×64 previews,
  guarded deletion, restart, cart switching, controls, pixel-perfect scale,
  volume/mute, cartridge info and English or Russian UI. States serialize Lua
  closures/globals through Eris together with RAM, framebuffer, palettes, RNG,
  input and audio channel position.
- State files are compressed and keyed by cartridge content rather than path.
  Each of Pocket's ten 256 KiB nonvolatile slots stores the corresponding
  logical state for several cartridges and evicts the least-recently-saved
  record only when that particular physical slot fills.
- `cartdata`, `dget` and `dset` use the same nonvolatile packed store and are
  keyed by the cartridge-supplied data ID. Dirty data is flushed periodically,
  on state saves and whenever the system menu opens.
- `Select Cart` directs the user to AnalogueOS Core Settings → Cartridge. The
  cartridge data slot is user-reloadable and restart-on-load. Its filename is
  deliberately not persisted, so every fresh core launch opens Pocket's native
  cartridge picker instead of reopening the previously used cart.
- Public releases contain no cartridges or third-party sound banks. The
  `Assets/pico8/common/cards/` directory contains only installation
  instructions; users provide cartridges they are entitled to use.
- `make package` produces the public, cartridge-free Pocket ZIP containing the
  pinned `os20` bitstream and statically linked `rv32imafc` application.
  `make package-local` is an explicit private-build target which additionally
  bundles carts found under `assets/cards/`, `Доп игры` and `Топ игры`.
- Runtime failures show their actual Lua error on screen instead of the old
  rainbow bring-up square. Select remains available on that screen, allowing
  the cartridge to be restarted or exited without restarting the core.

- Cartridge `menuitem()` entries appear in the Select system menu right after
  RESUME, with their callbacks, left/right input and button filters.

This is still an alpha, not a full-compatibility release. Offline BBS handoffs
use the separately installed collection described below. Arbitrary local-path
multicart switching, ZIP extraction and the indexed in-core browser remain
unfinished. Save-state persistence, menu/input behavior, profiling and the current 2X
presentation path have been validated on physical Pocket hardware. CPU-heavy
cartridges and scaler behavior beyond the established 2X path still need more
compatibility and performance work.

## Known incompatible cartridges

The core checks the exact cartridge revisions below before running their Lua.
Launchers require their complete offline collection; other blocked revisions
display the reason and workaround. Press Select to open the system menu.
Matching uses decoded ROM and Lua content, so renaming a cart or changing its
PNG label does not bypass the list. Different content is checked independently;
an unlisted cart is not a compatibility guarantee.

<!-- incompatible-carts:begin -->
| Cartridge revision | Status | Reason | Workaround / support |
|---|---|---|---|
| Pico Arcade (`pico_arcade.p8.png`) | Offline collection required | Needs the offline Arcade collection and cartridge-loading support. | Install the Arcade collection with the Pocket Sync plugin. [Issue](https://gitlab.zenibako.cloud/zenibako/dotfiles/-/work_items/68) |
| Trail of the Sorcerer (`trail_of_the_sorcerer.p8.png`) | Offline collection required | Gameplay needs the offline Sorcerer companion collection. | Install the Sorcerer collection with the Pocket Sync plugin. [Issue](https://gitlab.zenibako.cloud/zenibako/dotfiles/-/work_items/68) |
| Pico Backrooms (`pico_backrooms-1.p8.png`) | Blocked | Crashes during gameplay: this revision calls unsupported rrectfill(). | Use a compatible revision when available; rounded-rectangle support is tracked. [Issue](https://gitlab.zenibako.cloud/zenibako/dotfiles/-/work_items/69) |
| Peral (`peral.p8.png`) | Blocked | Needs #peral_game and launch parameters larger than the current 255-byte limit. | This launcher is outside the initial offline collection. Use another cartridge. [Issue](https://gitlab.zenibako.cloud/zenibako/dotfiles/-/work_items/68) |
| Pico Night Funkin (`pico_night_funkin.p8.png`) | Blocked | Needs #fnf_select and further game carts outside the initial offline collection. | Use another cartridge until the full companion collection is supported. [Issue](https://gitlab.zenibako.cloud/zenibako/dotfiles/-/work_items/68) |
<!-- incompatible-carts:end -->

The list records confirmed launch/gameplay blockers, not low frame rates or
test-harness nondeterminism. Historical Yarn, Snail, Tetyis and Moss fixes are
not blocked. The Pocket runs installed BBS carts offline; downloads run on the
computer through the plugin below. Unlisted revisions can still fail at runtime.

Maintainers: edit [the registry](compatibility/incompatible-carts.json), then run
`python3 scripts/incompatible-carts.py` to update the compiled list and this table.
`make test` checks that both match. `make compat-scan` builds a tool whose
`CART --identity` mode prints a decoded content hash without executing the cart.
Add only reproduced failures, record the tested revision and evidence, and
remove its fingerprint once the support path is verified.

## Offline Pico Arcade and Sorcerer

The [Pocket Sync plugin](docs/OFFLINE-COLLECTIONS.md) downloads Pico Arcade's
35 referenced games and Trail of the Sorcerer's `mot_sorcerergame` companion.
Its **Browse BBS** menu offers Recent, Popular and text search of public BBS
releases, with paginated results and downloading a selected cart to the Pocket
as a standalone file, without logging in.
Build the Go/TinyGo plugin with `make offline-plugin-tools`; its distribution
pair is written to `build/offline-plugin/`. Pocket Sync installs plugins
from GitHub releases containing `plugin.wasm` and `plugin.json`. These artifacts
replace the former Cart Thumbnails plugin in the next complete release, using
the same repository URL. Existing SD artwork is preserved. The native helper can install a verified local cache
before publication; see the linked instructions. Once distributed, choose
**Collections**, select **Pico Arcade**, **Sorcerer**, or **Both collections**,
then **Install**. **Refresh downloads** retrieves fresh copies; **Library → Status** reports installed and cached counts. **Library → Tidy
filenames** scans `Assets/pico8/common/` and its subfolders, renaming verified PNG
carts in place to `Title [BBS ID].p8.png` using saved metadata or paced BBS title
lookups. Unmatched files and conflicts are preserved.
Downloads are cached and paced; a rate limit stops the batch so it can resume
later from the cache. No PICO-8 installation is needed for these downloads.

Standalone filenames use `Title [BBS ID].p8.png`; cache and bundle keys retain
the original IDs so collection handoffs still match. The plugin writes standalone
carts into `Assets/pico8/common/cards/bbs/` and
the core's offline bundle into `Assets/pico8/common/cards/bbs.p8pack`. Keep your
launcher carts in the normal card directory and select them as usual. Install
the matching complete collection to clear its launch restriction. The core
checks each requested payload's CRC and cartridge encoding before loading it.
Core update packages omit the bundle so updates preserve your installed games.

Cart `load("#id", "return label", "parameters")` requests switch outside Lua.
Select opens a return row when the cart supplied a label; `extcmd("go_back")`
also returns. Returning restarts the parent cart. Up to eight parents are kept
for the current session. Save states retain parameters, but do not recreate
return history after a fresh launch. Version-1 save states remain readable.

The plugin cannot launch the PICO-8 app or browse your Mac's files. If you prefer
your existing PICO-8 BBS cache, use the explicit native importer in the
[plugin instructions](docs/OFFLINE-COLLECTIONS.md#optional-explicit-cache-importer).
Game assets and the licensed PICO-8 executable are never included in this repo
or the plugin. Host tests cover the loader and actual Extism plugin execution;
the new offline path still needs a physical Pocket run.

## AI development disclosure

Pico8Pocket has been developed with substantial assistance from OpenAI Codex
and Claude Code for implementation, tests, documentation and companion tools.
The maintainer directs changes and tests on hardware; the original creator's
[personal note](ABOUT_ME.md) also describes AI use. This disclosure does not
attribute every inherited Fake-08, z8lua, SDK or runtime component to AI.

The core package includes a `declare_ai` list in `updaters.json`. Pocket Sync's
numeric AI-ness score comes from the separate
[openFPGA AI report](https://github.com/openfpga-library/openfpga-ai-check),
not a measured percentage of AI-written source. A missing report is not a
claim that AI was unused.

## Build

Run the portable tests:

```sh
make test
```

Fetch the pinned upstream dependencies:

```sh
make deps
```

Build and assemble the Pocket SD tree:

```sh
make pocket
```

`make deps` stages the SDK the build uses in `.deps/openfpgaos-sdk/`:
Diablo's SDK sources, libraries and container tooling, the API headers from
`main` of the openfpgaOS fork
[zenibako/openfpgaCore](https://github.com/zenibako/openfpgaCore) (`a58aacd`),
and that fork's device-tested Pocket runtime (`os20.rbf_r`, `os.bin`,
`loader.bin`) from its
[`pocket-runtime-a58aacd`](https://github.com/zenibako/openfpgaCore/releases/tag/pocket-runtime-a58aacd)
release, each checked against a SHA-256 pin. The fork's runtime adds native
save states and sleep/wake: `core.json` sets `sleep_supported`, which turns
on both the Pocket's Memories and sleep. Built against an SDK without the
save-state API (`SDK_ROOT=...`), the core advertises neither.

The result is written to `build/pocket/pico8pocket/`. `make package` creates a
public, cartridge-free release ZIP under `releases/`. With a local Homebrew
RISC-V toolchain, use:

```sh
make package USE_SDK_CONTAINER=0
```

For private hardware testing with locally supplied cartridges, use:

```sh
make package-local USE_SDK_CONTAINER=0
```

The local archive is named `pico8pocket-v<VERSION>-local.zip` so it cannot be
mistaken for the public artifact. Local cartridge directories are ignored by
Git.

To compare two builds on hardware, repackage one as a separate core that
installs beside the other and shares its cards:

```sh
scripts/variant-core.sh releases/pico8pocket-v<VERSION>.zip b "Test build B" b.zip
```

It becomes `zenibako.pico8b`; its program, configuration, settings and save
files get their own names, so neither core's files replace the other's.

The platform banner `dist/platforms/_images/pico8.bin` is generated from
`assets/branding/pico8-platform.png`; after changing the PNG, regenerate it
with:

```sh
scripts/platform-image.py assets/branding/pico8-platform.png dist/platforms/_images/pico8.bin
```

The RISC-V build uses the same container toolchain as openfpgaOS. Docker,
OrbStack, or Apple's `container` runtime is required unless a compatible
`riscv64-elf`/`riscv64-unknown-elf` toolchain is installed locally.

Install by extracting the release ZIP at the root of the Pocket SD card,
merging its `Cores`, `Assets` and `Platforms` folders with the existing ones.
The core is `zenibako.pico8` on the `pico8` platform (shown as PICO-8):

```text
/Cores/zenibako.pico8/
/Assets/pico8/common/              application, OS and cards/
/Assets/pico8/zenibako.pico8/      launch entry
/Platforms/pico8.json
/Platforms/_images/pico8.bin       PICO-8 banner in the platform list
```

On launch, Pocket's native asset picker asks for a `.p8` or `.p8.png`
cartridge.

Pocket Sync associates the core with the cartridge slot. Fixed internal files
(including the JSON launch descriptor) have no game-file filters. The `png`
filter covers `.p8.png` carts; ZIP archives must be extracted first.

Upgrading from a release that installed `Askent.pico8pocket`: move your carts
from `/Assets/pico8pocket/common/cards/` to `/Assets/pico8/common/cards/`, then
delete `/Cores/Askent.pico8pocket`, `/Assets/pico8pocket` and
`/Platforms/pico8pocket.json`. Save states, settings and cartdata are kept per
platform under `/Saves`: to keep them, move the contents of
`/Saves/pico8pocket/` to `/Saves/pico8/` before starting the new core.

## Cartridge location

Install cartridges under:

```text
/Assets/pico8/common/cards/
```

Analogue's APF inserts `common` for platform assets, so this is the canonical
Pocket path corresponding to the requested `/assets/pico8/cards` area.
At launch, Pocket's native asset picker selects the primary cart. The
planned in-core browser will consume an index stored beside the cards; see
[architecture.md](docs/architecture.md) for the APF filesystem constraint.

## Upstream bases

- Diablo/openfpgaOS SDK commit `8cb3198d50618696e07f5b49ad086a369dc57706`
  (API 0.9.0, as shipped with Diablo 1.1.0): SDK sources, libraries and
  container tooling
- openfpgaOS fork zenibako/openfpgaCore commit
  `a58aacd940c1b433c030b97f8207d2c7e03307c8`: API headers, and the Pocket
  runtime from its `pocket-runtime-a58aacd` release
- openfpgaOS SDK commit `628a12b551ac8137373c477e97466b84d153d2af`
  (license and redistribution notices)
- Fake-08 commit `814991a2571ad3970e386cef48f3b148aa1c27b9`, with z8lua at
  its submodule pin `e6928578` plus local patches (including backports of
  newer upstream z8lua fixes)
- PICO-8 behavior target: [official v0.2.7 manual](https://www.lexaloffle.com/dl/docs/pico-8_manual.html)

These revisions are pinned by `scripts/fetch-deps.sh` so builds do not silently
change when upstream moves. Diablo is fetched as a shallow, sparse Git checkout
of the pinned commit (only `src/sdk` and `tools`); the fork checkout supplies
`src/firmware/api`. The three Pocket runtime files are downloaded from the fork's
pinned release and verified by SHA-256. Keep `os20.rbf_r`, `os.bin` and
`loader.bin` together: the bitstream's BRAM contents must match its OS firmware
build. The assembled SDK is staged at `.deps/openfpgaos-sdk`.

## License and disclaimer

Pico8Pocket source code authored for this repository is available under the
[MIT License](LICENSE). Dependencies and generated release components retain
their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

Public source and release archives contain no PICO-8 cartridges, ROMs,
commercial game data or third-party sound banks. Users must provide cartridges
they are legally entitled to use.

PICO-8 is a product of Lexaloffle Games LLP. Analogue Pocket and openFPGA are
products and technologies of Analogue. Pico8Pocket is an independent,
unofficial compatibility project and is not affiliated with, endorsed by, or
sponsored by Lexaloffle Games LLP or Analogue.

## Releases and plugin migration

The [release guide](docs/RELEASING.md) covers the complete core/plugin distribution,
matched runtime pins, provenance, checksums and explicit draft/publication steps.
`make offline-plugin-tools` builds the public offline plugin files; native tests
run on Linux, macOS and Windows, and actual WASI tests use project-owned fixtures.
Cart thumbnail generation and its native/WASI downloads are retired. Keep
`https://github.com/zenibako/pico8pocket` configured as the Pocket Sync plugin URL
to receive the offline installer with the next complete release.
