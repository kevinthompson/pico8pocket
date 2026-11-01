# Compatibility target

The behavioral target is the official PICO-8 v0.2.7 specification:

- 128×128 display and base/extended palettes;
- six-button input for player 0;
- 64 KB base RAM and 32 KB cartridge ROM layout;
- P8 Lua syntax, 16.16 fixed-point numbers and VM timing semantics;
- `_init`, `_update`, `_update60`, `_draw`, `flip` and pause-menu behavior;
- graphics, map, memory, math, string, table, audio and persistence APIs;
- `.p8`, `.p8.png`, legacy and current compressed-code encodings;
- local multicart `load`/`reload` behavior;
- `cartdata`, `dget` and `dset` persistence.

Out of scope for the Pocket hardware target:

- online SPLORE/BBS downloads, because Pocket has no network interface;
- multiplayer input beyond player 0, by product decision;
- PICO-8 editor and development tools. This project is a player.

## Initial test set

- Celeste Classic: first real-game acceptance target.
- PICO-8 `API.P8`: broad API smoke test once legally supplied by the user.
- Small project-owned carts for fixed-point overflow, shorthand syntax,
  palette/transparency, map sharing, `btnp`, audio effects, multicart loading,
  persistence and 30/60 FPS scheduling.

Compatibility claims must be backed by a named cart/test and a Pocket run.

For device performance comparisons, use the [benchmark guide](BENCHMARKING.md):
record each cart hash and route, retain manual screenshot limitations, and keep
correctness/save-state checks independent from timing. Importing a result or
selecting a faster platform does not establish compatibility.

## Known incompatible revisions

[The README list](../README.md#known-incompatible-cartridges) is generated from
[`compatibility/incompatible-carts.json`](../compatibility/incompatible-carts.json).
The runtime checks those decoded ROM/Lua content hashes before cartridge code
executes, including on restart. Conditional entries require their complete
offline collection index; unconditional entries are rejected. This does not change saved-state identities or
block other revisions by filename. An unlisted revision is not a claim of support.

The initial list was checked against older agent sessions and supplied-cart
experiment reports on 2026-10-20:

- Pico Arcade: all 35 launch selections need BBS `load()` handoffs. The offline
  Arcade bundle supplies those targets.
- Trail of the Sorcerer: its intro calls `load("#mot_sorcerergame")`; the offline
  Sorcerer collection supplies that companion.
- Pico Backrooms: the current host runtime reproduces the historical native/RV32
  failure at frame 122, calling undefined `rrectfill()` at cart line 554.
- Peral: the extended seed-7 smoke reaches `load("#peral_game", "", params)`
  at frame 234; the companion is outside this collection and translations need
  730 parameter bytes, beyond this loader's 255-byte bound.
- Pico Night Funkin: the extended smoke reaches `load("#fnf_select")`;
  that selection cart and subsequent song carts are outside this collection.
  Both additional launchers link the existing offline-loader support issue.

Grippy's differing native repeats were attributed to Lua hash seeding, not a
demonstrated gameplay failure. Historical Yarn, Snail, Tetyis and Moss failures
were repaired. Low FPS in the old os25 smoke windows also does not establish
incompatibility. These cases are not blocked.

To maintain the list, use `build/tools/pico8pocket_compat_scan CART --identity`
after `make compat-scan`, record reproduction evidence and the support issue in
the registry, then run `python3 scripts/incompatible-carts.py`. `make test`
checks generation and public identity/lookup fixtures. For local rejection,
restart and menu tests, also set any of `P8P_TEST_PICO_ARCADE`,
`P8P_TEST_TRAIL_OF_THE_SORCERER` and `P8P_TEST_PICO_BACKROOMS` to the corresponding
private cart file before running `make test`. Missing private fixtures are
reported as skipped; never commit the cartridges.

`P8P_TEST_PERAL` and `P8P_TEST_PICO_NIGHT_FUNKIN` exercise the two later launcher
findings using the same rejection/restart and capability checks.

## Offline loader verification

On 2026-10-20, the host scanner selected all 35 targets through Pico Arcade's
X-button launch route, ran each for 600 neutral-input frames and returned to the
original launcher identity. Sorcerer's `newrandomgame()` / `rungame()` path
loaded its genuine companion and ran 600 frames with shared cartdata. Its
original load call supplies no breadcrumb, so no parent-return row is expected.
These are launch/handoff smoke checks, not complete playthroughs or Pocket
performance measurements. Space Picanoid also exposed a legacy source-terminal
NUL; compilation now ignores trailing terminators while decoded bytes and saved
identities remain unchanged, matching the supplied PICO-8 reference export.

The actual Extism/WASI plugin installed all 36 validated original PNGs from
cache, preserving every payload byte. Separate host runs exercised real HTTPS,
15-second request spacing, cancellation and failed-update preservation. The
project-owned app tests cover identity changes, return input suppression and
missing-target Select-menu recovery. A genuine version-1 state fixture checks
backward loading alongside current state round trips.

For private route checks, build `make compat-scan`, set `P8P_BBS_PACK` to the
installed pack's absolute path and run:

```sh
build/tools/pico8pocket_compat_scan ARCADE_CART --arcade-launch 1 600
build/tools/pico8pocket_compat_scan SORCERER_CART --handoff '(function() newrandomgame() rungame() end)()' 600
```

Repeat Arcade indices 1 through 35. Physical Pocket slot-20 loading and the
interactive updater UI still need device/application verification.

## Alpha implementation status

Implemented and covered by host tests:

- text `.p8` ROM-section parsing;
- RGBA/RGB `.p8.png` extraction with uncompressed, legacy `:c:` and current
  PXA compressed-code streams;
- P8 Lua parsing/execution with 16.16 fixed-point numbers;
- `_init`, `_update`, `_update60`, `_draw`, 30/60 FPS selection and player-0
  `btn`/`btnp` state, including raw P8SCII button-glyph constants used by
  compressed PNG carts;
- cooperative top-level and `_init` execution with real `flip()` yielding;
- `run()` cartridge restart plus nil-safe PICO-8 table helpers and relative
  mouse `stat(38)`/`stat(39)` defaults;
- extended graphics (`sspr`, `tline`, ovals and `fillp`), sprite/map remapping,
  big-map memory, draw-state aliases and 8/16/32-bit memory APIs;
- persistent `cartdata`, `dget` and `dset` records in Pocket save storage;
- four-channel `sfx`/`music` playback (synthesized at 24 kHz, output at 48 kHz) with built-in waveforms, loops,
  music patterns, fades and the standard note effects;
- Eris-based Lua state persistence together with RAM, renderer, input, RNG and
  audio state, compressed per cartridge with state previews;
- offline `load("#id", breadcrumb, parameters)`, `stat(6)` parameters,
  `stat(100)` breadcrumb and `extcmd("go_back")`, including host-managed return
  history and Select-menu return; the shared catalog initially contains 36 carts;
- `stat(80)`-`stat(85)` and `stat(90)`-`stat(95)` report a fixed 1970-01-01
  00:00:00 because the runtime has no wall clock; carts can treat a year before
  2000 as no date;
- `0x5f2c` screen modes 1-3 (stretch), 5-7 (mirror), 129-131 (flip) and 133-135
  (rotate), applied to the whole screen at presentation. Bit 6 is ignored, and
  other values show the normal screen. The official manual does not document
  this register; the mode list comes from the PICO-8 wiki;
- 120 error-free headless frames of the official Celeste Classic cart.

Not yet compatibility-complete:

- exact custom-instrument, filter, reverb and hardware-audio behavior;
- pause/menu callbacks, general P8SCII/custom font behavior beyond button
  constants, and the remaining obscure draw-state/hardware aliases;
- arbitrary local-path `load()`/`reload()` multicart behavior and physical
  Pocket verification of the new offline BBS loader;
- ZIP extraction and the graphical indexed card browser;
- full API/conformance coverage and physical-Pocket performance validation.
