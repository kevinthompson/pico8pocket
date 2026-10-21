# Changelog

## Unreleased

- Browse paginated Recent and Popular public BBS releases in the Pocket Sync
  plugin. View a cart's BBS page or download its original PNG to the Pocket.
  Listing and cart requests share pacing; cached installs work offline and
  individual downloads preserve managed dependency bundles.

- Replace the Cart Thumbnails plugin with Offline Collections under the same
  repository URL. Release CI uses pinned Go/TinyGo builds, cartridge-free actual
  WASI tests, complete core provenance and a new exact plugin asset inventory.
  Retire thumbnail CLIs and reuse the existing Go/TinyGo tooling; preserve SD artwork.

- Add a Pocket Sync / pupdate plugin for Pico Arcade's 35 BBS games and Trail
  of the Sorcerer's companion, with verified cache reuse, paced downloads and
  atomic offline-bundle updates. No game assets or PICO-8 binaries are bundled.
- Add offline BBS cart handoffs, parameters and session return history, with a
  Select-menu return row and refreshed per-cart save/control identities. The
  optional bundle occupies tail slot 20; existing save slots stay in place.
  Runtime state version 2 preserves parameters and still reads version 1.
  Launcher compatibility restrictions clear when their collection is installed.
- Legacy compressed carts ending in a source terminator, including Space
  Picanoid, compile without changing decoded cart bytes or save identities.
- Known incompatible cartridge revisions are blocked before execution with an
  explanation and Select-menu recovery. The compiled list and README table
  share one registry, initially covering Pico Arcade, Trail of the Sorcerer
  and Pico Backrooms. Matching uses decoded content rather than filenames.
- Lua-to-Lua calls set up and tear down their frames inside the
  interpreter loop instead of through `luaD_precall`/`luaD_poscall`, and C
  functions reached by a tail call (`return peek(...)`) or used as a `for ...
  in` iterator (`pairs`, `ipairs`) take the same fast path as ordinary API
  calls. A Lua call costs about 15% less on the Pocket's CPU; whole frames
  run 1-3% faster (Celeste 2, Kiloman, Moss Moss, Grippy). Frames are
  identical on all 68 local carts; hooks, errors and coroutines behave as
  before.
- `circfill()` and solid `rectfill()` clip each shape once and fill rows
  four pixels at a time, instead of a clipped span call per row. Rows above
  or below the clip rectangle are skipped. Output is identical (frame
  hashes on 114 carts plus a fuzzed camera/clip/fillp/pal cart); Celeste 2's
  tower room saves about 2-3% of a frame.
- `exit()` is a no-op instead of an undefined global. It is not PICO-8 API,
  but Yarn calls it while drawing its score panel once all levels are
  unlocked, which stopped the cart with "attempt to call global 'exit'". Carts
  that define their own `exit` (Snail) keep theirs; save states are unaffected.
- Muting audio in the system menu skips synthesizing it: music, notes,
  effects envelopes, fades and `serial()` PCM keep advancing exactly as they
  would audibly (`stat()` and music timing are unchanged, verified against
  normal rendering on 69 carts), but no samples are generated. Muting now
  frees the audio share of every frame (A in the profiler, 3-5 ms in Celeste
  2); before, it rendered everything and then multiplied it by zero.
- The PICO-8 platform shows a PICO-8 banner in the Pocket's platform list
  (`Platforms/_images/pico8.bin`, generated from
  `assets/branding/pico8-platform.png` by `scripts/platform-image.py`).
- The core is now `zenibako.pico8` on the `pico8` platform: its files live
  in `Cores/zenibako.pico8`, `Assets/pico8/...` and `Platforms/pico8.json`
  instead of `Askent.pico8pocket`/`pico8pocket`. To upgrade, move your
  carts from `Assets/pico8pocket/common/cards/` to
  `Assets/pico8/common/cards/` and delete `Cores/Askent.pico8pocket`,
  `Assets/pico8pocket` and `Platforms/pico8pocket.json`. Save states,
  settings and cartdata kept by the Pocket under `Saves/pico8pocket` move
  to `Saves/pico8` the same way.
- Cartridge pause-menu entries from `menuitem()` are listed in the Select
  system menu, right after RESUME (24 of 65 carts in one tested library add
  them). A selects an entry and closes the menu unless the callback returns
  true; left/right are passed to the callback, the index's button filter
  (`menuitem(1|0x300, ...)`) is honoured, and button glyphs in labels are
  shown as letters. The main menu scrolls when the list is longer than the
  screen. Menu entries are not part of save states: loading one keeps the
  entries the cart has set up since it started. Existing save states still
  load.
- Audio voices are synthesized at 24 kHz (PICO-8 itself runs at 22050 Hz)
  and interpolated to the 48 kHz output, roughly halving audio CPU: Kiloman's
  music drops from about 890 to 470 host instructions per output sample,
  Tetyis by a third. Loudness envelopes and pitch match the previous output
  (checked on seven carts); save states keep their 48 kHz units, so files
  move freely between this and earlier builds.
- Frame skipping no longer gets stuck off. The update-only cost it relies on
  was only measured on skipped frames, so one slow update (Kiloman loads the
  next room inside `_update` when you drop into it) could switch skipping
  off for the rest of the room at about 12 FPS. While a cart is over budget
  without skipping, one draw in 15 is now skipped to re-measure.
- `menuitem(i, label)` without a callback relabels an entry and keeps its
  callback, as in PICO-8. Moss Moss's colorblind toggle could be switched
  on but not off.
- `split()` of a non-string (`nil`, `false`) returns nothing instead of
  raising an error, as in PICO-8. Turning Moss Moss's colorblind mode back
  off stopped the cart with "bad argument #1 to 'split'".
- `==` and `~=` compare numbers, nil, booleans and strings inside the
  interpreter loop instead of calling out; only tables and userdata, which
  may have `__eq`, still take the call (2-3% of Moss Moss's frame).
- Save states with a large Lua heap save again. The store reserved the
  compressor's worst case for the raw size, so any state over ~256 KB raw
  was refused even though it compresses far smaller: Moss Moss's ~500 KB
  state takes 84 KB.
- Global names read inside `local _ENV=obj` code that miss the object and
  resolve through its `__index` table are found without leaving the
  interpreter loop. Moss Moss, which runs its objects this way, needs 4.4%
  fewer host instructions per frame; field reads and `__index` functions
  (Celeste 2) are unaffected. `luaV_equalobj_` moved out of BRAM to make
  room, as `==` no longer needs it for common types.
- A global name that resolves to nil no longer interns the
  `"__PICO8_SANDBOX"` string for the env-fallback lookup; the sandbox is
  read from the registry's globals slot. Celeste 2 does about 360 such
  lookups per frame and needs 4.4% fewer host instructions per frame; the
  smaller interpreter loop frees about 760 bytes of BRAM.
- PICO-8 API calls cost less: arguments are read straight from the Lua
  stack instead of through `lua_gettop`/`lua_type`/`lua_tonumberx`, and the
  interpreter calls light C functions through a short dedicated path when no
  call or return hooks are set. Celeste 2 makes about 1,900 such calls per
  frame; together with the sandbox change it needs 9.4% fewer host
  instructions per frame than before, Moss Moss 2.7% fewer.
- The frame-skip budget at 30 fps is 32 ms instead of 30 ms (a frame lasts
  33.3 ms). Celeste 2's tower rooms and grapple rope, whose frames fit,
  were dropping to every other frame drawn; on the Pocket they now stay at
  30 visible FPS.
- The diagnostics overlay shows F, the full runtime step the frame-skip
  scheduler times (update and draw plus audio serviced mid-frame, GC and
  frame setup). R1 is kept while F + A + P fits the budget; U and D alone
  can add up to less.
- `time()`/`t()` compute frames ÷ FPS with integer arithmetic instead of a
  double-precision divide and conversion, which the Pocket emulates in
  software (identical results, checked over 236 million frame counts).
  Celeste 2 calls `time()` for every pixel of its grapple rope: drawing an
  80-pixel rope costs about 30% fewer instructions.
- Table reads and writes with a constant name (`spr`, `level.width`,
  `self.x`, `o.x = ...`) remember, per instruction, where in the table they
  last found the key, and check that one slot before doing a full hash
  lookup. Objects built the same way share the hint. In Celeste 2's tower
  room the RV32 build runs 4.8% fewer instructions per frame (measured on
  an RV32 build under QEMU). A hint is the node's byte offset, kept in an
  array parallel to the bytecode (four bytes per Lua instruction), so a hit
  needs no index arithmetic: another 2.8% in that room, together with
  `<`/`<=` comparing numbers inside the interpreter loop instead of calling
  out.
- The interpreter keeps the Lua program counter in a register and writes
  it back only before calls, errors, metamethods and the collector, checks
  hooks only at servicepoints, and no longer range-checks every opcode. On
  the Pocket the per-instruction dispatch drops from 17 to 11 RV32
  instructions with no branches, and the interpreter loop takes 600 fewer
  bytes of BRAM. Error messages report the same lines as before.
- `load()` and `loadfile()` accept Lua source only, not precompiled
  bytecode, which Lua 5.2 runs without verifying (PICO-8 has neither
  function).
- Fill-pattern shapes (`fillp` with `rectfill`, `circfill` and friends) are
  drawn four pixels at a time instead of clipping and pattern-testing each
  pixel separately, and `rectfill` works out its pattern rows once per
  rectangle. Celeste 2's tower room, whose background columns and fog are
  pattern fills, needs 29% fewer host instructions per drawn frame. 27 of
  the 65 tested carts use `fillp`; their frames are unchanged, as are those
  of a randomized pattern/camera/clip/palette test cart.
- The interpreter checks its service hook (audio and input pumping during
  long frames) only at jumps, loop back-edges and calls instead of before
  every instruction, with the hook period cut from 8192 to 2048 so service
  happens as often as before. Celeste 2 needs 12.7% fewer host instructions
  per frame than when the Celeste check was added. Carts that
  split a long `_init` across frames may now reach their first frame a
  frame or so earlier or later.
- The `all()` iterator reads the table directly and computes `#t` only at
  empty slots instead of on every step. Moss Moss, which iterates with
  `all()` throughout, needs about 26% fewer host instructions per frame.
- Numbers are converted to text (`tostr`, `print(n)`, `..`) with integer
  arithmetic instead of a double-precision `sprintf`, which the Pocket's
  single-precision CPU emulated in software: about 4200 RV32 instructions
  per number down to about 40. The text is identical (checked for all 2^32
  values against glibc and against the SDK's musl).

## 0.0.33 — 2026-10-06

Local hardware-test build.

### Highlights

- Moves the SDK and Pocket runtime (`os20` bitstream, `os.bin`) to the
  openfpgaOS API 0.9.0 generation shipped with Diablo 1.1.0. The application
  only uses the video, audio and input APIs, which are unchanged; the new
  bitstream and kernel still need a physical Pocket regression pass.
- Fetches Diablo as a pinned shallow, sparse Git checkout instead of a
  GitHub-generated tarball, so builds no longer depend on tarball checksums
  staying stable. The three runtime files remain SHA-256 pinned.
- Backports two newer z8lua fixes: `chr()` accepts up to 32767 values like
  PICO-8 v0.2.5+, and the obsolete `atan2(x, 0x8000)` bug emulation is gone.
- `peek(addr, n)` now allows up to 32767 values and reserves Lua stack space
  first. Previously any `n` above ~20 wrote past the C function's stack slots
  and could corrupt the heap.
- The 2× presentation pass resolves the palette and pixel doubling through a
  per-frame lookup table and writes two doubled source pixels per 32-bit store,
  halving framebuffer stores and removing the per-pixel palette branch.
- Host runtime tests build with GCC as well as Clang.
- Unflipped `spr()` calls share `map()`'s unpacked 8×8 tile blitter instead
  of the general per-pixel sprite path, and fully visible `print()` glyphs
  write their pixels directly. String arguments to `print()` skip the
  `tostring` conversion. Rendered frames are byte-identical.
- Lua's many small allocations are served from size-class free lists instead
  of musl's `malloc`.
- The Lua `debug` library is no longer visible to cartridges. PICO-8 has no
  such global, and Tetyis printed its table address on screen because of it.
  Existing save states remain compatible.
- Clip bounds poked into draw-state RAM are clamped to the screen, so they
  can no longer make renderers write past the framebuffer.

- The z8lua interpreter reads and writes existing entries of plain tables
  (string or integer keys) directly in its dispatch loop instead of calling
  `luaV_gettable`/`luaV_settable`. Globals, fields, array elements and method
  lookups all take this path; metatables, missing keys, string indexing and
  the `_ENV` sandbox fallback keep the original code.
- The Lua garbage collector waits until the heap reaches 4× the live data
  (Lua's default is 2×) before starting a cycle.
- `blit_tile` replaces `draw_sprite` in the 14 KiB on-chip BRAM so the larger
  interpreter loop still fits.
- `make deps` records the applied z8lua patch set and reapplies all patches
  from a clean tree when it changes, instead of checking each patch alone.

- `.p8` text carts now load like PICO-8 itself: UTF-8 glyphs in the Lua
  source (control codes, button symbols, characters 128-255) are converted
  back to single P8SCII bytes. Kiloman previously crashed decoding its data
  strings because they kept their multi-byte UTF-8 form.
- `print()` is a port of Fake-08's PICO-8 text engine: the complete
  256-glyph default font (including the smaller uppercase letters and
  button/kana glyphs), custom fonts at `0x5600`, and P8SCII control codes
  such as `\f` colour, `\#` background, cursor moves, wide/tall/inverted
  modes, one-off glyphs and `\^!`/`\^@` memory pokes. `print(str, col)`
  sets the colour and `print()` returns the right-most x position.
- Drawing can be redirected with `0x5f55` (to the sprite sheet or upper
  memory) and back, which carts such as Kiloman use to compose sprites.
- `palt(bitfield)` sets transparency for all sixteen colours at once.

- Writes into screen memory (`poke`, `memset`, `memcpy`, `reload`, print
  pokes) update only the affected framebuffer pixels instead of packing and
  unpacking the whole screen. Kiloman `memset`s its health bars about 26
  times per frame, which previously cost over half a million pixel
  conversions per frame.
- Full-screen packing and unpacking use aligned word loads and stores.
- `split()` and other number conversions parse plain decimal integers up to
  32767 directly instead of through `strtod` (exactly equivalent; checked
  against `strtod` for every integer string from -99999 to 99999).

- `spr()` with a negative sprite number draws nothing. Kiloman uses `-1`
  as "no sprite", which previously painted a black box under the player
  while shooting.
- The audio queue adapts to the frame time: it keeps about 1.5x the time
  between top-ups queued (43-170 ms), so slow frames no longer drain the
  buffer and make music stutter, while fast carts keep the low latency.

- Input follows PICO-8's once-per-frame sampling. The mid-frame updates that
  keep busy-wait loops responsive may now only add buttons for `btn()`: a
  quick tap released during a long frame is no longer dropped before the cart
  reads it, and a press arriving after the cart's `btnp()` check fires on the
  next frame instead of never.
- `btnp()` honours the repeat delay and interval at `0x5f5c`/`0x5f5d`,
  including 255 to disable repeating, as Kiloman does.
- Custom SFX instruments: notes with the custom bit play SFX 0-7 as an
  instrument at its own speed, loops and effects, transposed relative to C-2
  and scaled by the note volume, restarting as PICO-8 does. 31 of 65 carts in
  one tested library use custom instruments or filters.
- SFX filters: detune (second oscillator), buzz and noiz waveform variants,
  reverb (two feedback delays) and dampen (high-shelf filters), plus the
  hardware reverb/low-pass bits at `0x5f41`/`0x5f43`. Behaviour follows
  zepto8/Fake-08. Carts that use neither feature render bit-identical audio
  (checked on 34 carts); audio CPU in Kiloman roughly doubles on the host.
- Saved states keep the 0.0.32 audio layout; instrument voices and filter
  memory are not saved and restart on the next note.
- `serial(0x808, ...)` plays raw 8-bit PCM at 5512 Hz and `stat(108)` reports
  the queued samples; Impossible Mission RT previously hung waiting on it.
- `_update` and `_draw` run as a coroutine, so `flip()` inside them ends the
  frame as in PICO-8 (Kiloman's pauses, Explorers' menus) instead of
  returning immediately.
- The fill-pattern glyph constants (`▒`, `█`, `…`) are defined, so
  `fillp(▒)` no longer clears the pattern.
- A colour passed to `pset`, `line`, `rect(fill)`, `circ(fill)` or
  `oval(fill)` also becomes the pen colour, as in PICO-8.
- `pal()` also resets transparency, as in PICO-8. Tetyis's falling piece
  kept the ghost piece's transparent colour and was drawn as an outline.
- `stat(26)`/`stat(56)` report the ticks played in the current music
  pattern. The piconiccc demo synchronises its scenes to them and stopped
  with an error.
- `trace()` returns its message and `cstore()` is accepted as a no-op, like
  Fake-08. Pirates Trial crashed in `trace`. Both are numbered after the
  existing built-ins, so earlier save states still load.
- `local x += 1` and the other compound operators are accepted after
  `local`, as in PICO-8 (`local x = x + 1` using the enclosing `x`). Space
  Station Demo and The Lost Night failed to load; Fake-08 rejects them too.
- An inverted clip rectangle no longer produces negative-length spans; Snail
  crashed on it.

Profiling Tetyis on the host showed its own `_draw` repeats the HUD, falling
piece and ghost piece ten times per frame (a misplaced `end`). Together, these
changes cut host instructions per gameplay frame by 26% with byte-identical
output.

### Measured on Pocket hardware

Profiler readings from test builds during development (U/D in ms):

| Cart | Before | After |
| --- | --- | --- |
| Tetyis (gameplay) | L20 V5, U24 D73 | L32 V8, U15 D58 |
| Kiloman (stage) | crashed on load | L40 V10, U4 D48 |

Moss Moss runs from its `.p8.png`; the multi-platform export ZIP that
contains it cannot be loaded directly.

### Known limitations

- PICO-8's distort and half-rate hardware audio bits (`0x5f42`, `0x5f40`)
  and the click-removing crossfade between notes are not implemented.
- The PICO-8 pause menu and `menuitem()` entries are not available.
- Save-state persistence, `cartdata`, cartridge selection and menu input need
  a physical Pocket regression pass on the new API 0.9.0 runtime.

## 0.0.32 — 2026-08-22

Local hardware-test build.

### Highlights

- Moves the Pocket hardware/runtime base from the July openfpgaOS API 0.7.0
  snapshot to the matched API 0.8.1 SDK and runtime shipped by the official
  Diablo port. The application, OS image and `os20` bitstream now come from a
  mutually compatible generation rather than mixing old SDK headers with new
  binaries.
- Uses the newer dual-issue VexiiRiscv `rv32imafc` configuration at a
  timing-closed 90 MHz, with 32 KiB instruction and 64 KiB data caches and the
  August openfpgaOS Pocket fixes.
- Pins and verifies the Diablo source archive and all three Pocket runtime
  files by checksum so local builds cannot silently drift with upstream.

### Expected impact

- This is primarily a hardware/runtime-base test. It may improve consistency,
  cache behaviour and input/runtime correctness, but does not remove z8lua's
  interpreter cost by itself; CPU-heavy cartridges still need real-device
  profiling before performance claims are made.
- Save-state persistence, cartridge selection, menu input and audio must be
  regression-tested on physical Pocket hardware because the kernel/SDK base
  changed together.

## 0.0.31 — 2026-07-25

Second public alpha release.

### Highlights

- Reworks adaptive rendering around separately measured update and draw costs.
  Overloaded 30/60 FPS carts now skip drawing only when it materially protects
  game speed, recover visual cadence faster after heavy effects, and retain a
  bounded amount of frame-time debt instead of introducing extra waits.
- Expands the opt-in Select+X profiler with labelled logical/visible FPS,
  update/draw/audio/presentation timings, render divisor, approximate Lua work
  and PICO-8 API call categories. This makes CPU, drawing and presentation
  bottlenecks distinguishable on real Pocket hardware.
- Adds faster ordinary `map()` tile rendering, cheaper `mget()`/`fget()` and
  Lua table iteration, no-op palette/transparency fast paths, and a unity-gain
  audio path. Effect- and tile-heavy cartridges spend less time in common hot
  loops, although CPU-bound carts can still miss their target frame rate.
- Removes the experimental `FULL SOFT` scaler after hardware testing showed a
  visibly blurry result. Crisp pixel-perfect `2X` remains the default and the
  only enlarged display mode in this release.
- Confirms on physical Pocket hardware that all ten save-state slots persist
  after leaving and reopening the core, cartridge selection works on every
  fresh launch, Select is the sole menu shortcut, and the profiler starts off.
- Keeps the public package cartridge-free. No PICO-8 games, ROMs or third-party
  sound banks are included.

### Known limitations

- This is still an alpha and does not provide complete PICO-8 compatibility.
- Multicart `load()` switching, ZIP extraction, native 11× display output and
  the PICO-8 pause menu are not implemented.
- CPU-heavy cartridges such as BAS Escape, UFO and later Celeste 2 scenes can
  still run substantially below their intended frame rate.

## 0.0.27 — 2026-07-23

First public alpha release.

### Highlights

- Runs single-cartridge `.p8` and `.p8.png` PICO-8 games on Analogue Pocket.
- Adds a Select-driven system menu with per-cartridge controls, ten compressed
  save-state slots with previews, restart and cartridge switching.
- Implements PICO-8 graphics, input, fixed-point Lua execution, four-channel
  audio, `cartdata`, `dget` and `dset` compatibility.
- Provides pixel-perfect `2X` and optional full-screen soft scaling.
- Adds an opt-in FPS/runtime/audio/presentation profiler with Select+X.
- Fixes persistent-state slot alignment for the `os20` Pocket runtime.
- Includes compatibility and performance work for minified carts, heavy
  effects, large off-screen fills and responsive input under load.

### Known limitations

- This is an alpha and does not provide complete PICO-8 compatibility.
- Multicart `load()` switching, ZIP extraction and the PICO-8 pause menu are
  not implemented.
- CPU-heavy cartridges may run below their intended frame rate.
- Save persistence changes require confirmation on physical Pocket hardware.
- No cartridges, ROMs or third-party sound banks are included. Users must
  provide cartridges they are legally entitled to use.
