---
name: pico8pocket-performance
description: Measure and optimize Pico8Pocket's frame time on the Analogue Pocket (RV32 openfpgaOS) - reading the profiler overlay, reproducing a slow scene, counting RV32 instructions, BRAM placement and the frame-skip scheduler. Use when a cart runs slowly on the Pocket, when the user sends overlay screenshots, or before changing the interpreter, drawing or audio hot paths.
argument-hint: "[harness: path or host:path of a zenibako/pocket-harness checkout]"
---

# Pico8Pocket performance

## The hardware, and what that implies

- The pinned `os20` platform is dual-issue VexiiRiscv rv32imafc at 90 MHz:
  32 KiB instruction cache, 64 KiB data cache, single-precision FPU only.
  Read `docs/architecture.md` and `scripts/fetch-deps.sh` for the current
  hardware configuration and runtime pins. `double` arithmetic is software-emulated
  at hundreds of instructions per operation, so keep it off per-call paths
  (`time()` used to divide in double; see `api_time`).
- 14,336 bytes of zero-wait BRAM for code (`APP_BRAM` in
  `.deps/openfpgaos-sdk/src/sdk/app.ld`), in section `.app_fasttext`. The z8lua
  dispatch loop and a few helpers live there:
  - `include/p8p/vm_fasttext.h` (`OF_FASTTEXT` declarations, force-included
    into z8lua)
  - `P8P_FASTTEXT` in `src/runtime/runtime.cpp`
  Check the size with
  `riscv64-unknown-elf-size -A .obj/pico8pocket/app.elf | grep fast`.
  An overflow fails the link ("region APP_BRAM overflowed").
- Changing `vm_fasttext.h` does not trigger a rebuild. Run
  `rm -rf .obj/pico8pocket` before `make package`.
- Use os20 RTL frame cycles for performance decisions. Historical cache shares
  and QEMU instruction counts describe their sampled scene, ELF and platform;
  they do not establish a universal bottleneck or predict dual-issue frame time.

## Reading the overlay (Select + X)

- **Left column:**
  - L: logical FPS. V: visible FPS.
  - U, D: the cart's `_update` and `_draw` ms.
  - A: audio pump ms. P: present ms.
  - R: render divisor (R02 = every other frame drawn).
  - K: Lua activity.
  - F: the whole runtime step that the scheduler times. It includes audio
    serviced mid-frame, GC and frame setup, so F can exceed U + D.
- **Right column:** API call counts per frame by group.
  - S: sprites. G: graphics. M: memory. C: draw state (`pal`/`palt`/`clip`...).
  - T: text. B: buttons. Q: helpers.
- **Scheduler** (`src/app/scheduler.c`): it keeps R1 while F + A + P fits the
  budget, 32 ms at 30 FPS or 15 ms at 60 FPS. Update cost is only measured on
  skipped frames. At R1 and over budget, one draw in 15 is skipped to
  re-measure it.
- A screenshot showing R02 with U + D + A + P under budget means F is larger.
  Ask for F before optimizing anything.

## Reproducing a slow scene

1. Read the cart: `build/tools/pico8pocket_compat_scan CART --source > cart.lua`
   (needs `make compat-scan`). Find a function that jumps to the scene, such
   as `goto_level(n)`.
2. Host profile of that scene:
   ```sh
   valgrind --tool=callgrind build/tools/pico8pocket_compat_scan CART \
       --bench "(function() goto_level(2) level_intro=0 return 1 end)()" 200 1 draw
   ```
   Subtract a 0-frame run to remove load cost. For update-only cost, use
   `nodraw` instead of `draw`.
3. **Optional RV32 instruction diagnostics:**
   ```sh
   tools/rv32-profile/profile.sh CART "<setup lua>" 30 20
   ```
   This is a separate picolibc/QEMU build. It prints executed instructions
   and top functions, with a legacy 8 KiB I-cache model; the shipped core has
   a 32 KiB I-cache. Treat these counts as diagnostics, not os20 cycle timing
   or proof of hardware build parity. Prefer the os20 harness below for timing.
   - x86 counts can mislead. x86 spills registers that RV32 keeps, so
     dispatch-loop changes may be invisible on x86 and real on RV32, or the
     reverse.
   - `memset`/`memcpy` come from picolibc there and look slower than the
     SDK's.
   - To attribute cost to source lines or opcodes, build with debug info
     (`-g`) and read `build/rv32-profile/report.txt`. For per-address counts,
     extend `tools/rv32-profile/icsim.c`; it already supports a PC range in
     arguments 5 and 6 that writes `pcnt.txt`.

## Priority regression check

- `make celeste` compares Celeste carts against `build/celeste-baseline`:
  frame hash plus callgrind instructions per frame.
  `make celeste-baseline` records the baseline.
- Carts live only in `assets/cards/`, which is gitignored; never commit them.
  A fresh workspace has none, and the Lexaloffle site may be unreachable
  from a cloud session, so ask the user for their cards zip (68 carts as of
  2026-10-11) before measuring or comparing frames. Celeste 2 needs
  companion carts that the zip does not include.
- **Which carts matter:** Celeste 2 is the priority test cart, then
  Kiloman, Moss Moss and Tetyis. Bunny Envmap is a tech demo: list its
  numbers, but don't investigate or optimise for its regressions.
  Backrooms fails on `main` as well.
- For A/B traces, fix the **final** Lua hash seed using the harness's
  `--hash-seed 0x5eed1234`. Confirm `PICOSEED 1592594996` in native/QEMU logs.
  Overriding `luai_makeseed()` alone leaves address mixing in `makeseed(L)`.
  Production seeding stays unchanged. Fixed seeding removes that confounder;
  code placement can still change cycles, so record exact ELF/layout and use
  matching routes and replay windows rather than a blanket noise threshold.

## Historical RV32 diagnostic profile (Celeste 2 tower room)

The roughly 1.6M instructions per frame below came from a separate diagnostic
build, not a calibrated os20 frame-time breakdown. Reprofile the selected scene
and exact binary before treating these shares as current hardware costs.

- **Interpreter loop:** about half the frame. Dispatch is 11 instructions per
  Lua instruction. Constant-key table access (`GETTABUP`/`GETTABLE`) uses
  per-instruction inline caches (`patches/z8lua-inline-cache.patch`,
  `z8lua-vm-tuning.patch`).
- **API calls:** they go through `luaD_callcfast`. C API functions read their
  arguments with `arg_int`/`arg_value`, not `lua_gettop`/`lua_tonumberx`.
- **Drawing:**
  - `blit_tile` (in BRAM) handles 8x8 sprites.
  - Patterned fills write 4 pixels at a time (`draw_rect_pattern`,
    `fill_pattern_span`).
- **Audio:** synthesized at 24 kHz and interpolated to 48 kHz. When muted,
  synthesis is skipped (`p8p_audio_skip`) but timing still advances.

## Before calling an optimization done

- Run `make test`, then `make package USE_SDK_CONTAINER=0`; the package must
  validate and BRAM must still fit.
- Compare frames on every local cart. Take one scanner from the base and one
  from your change (`scripts/compare-frames.sh build`), then
  `scripts/compare-frames.sh diff old new carts...`.
  - Expect identical frames unless the change is meant to alter output.
  - Kiloman and Grippy change with binary layout alone.
  - Fuzz anything with edge cases (fill patterns with camera, clip and
    palette, for example) using a small generated cart run through both
    scanners.
- The legacy scanner comparison does not verify the final Lua seed and compares
  frame hashes, not independent palette/PCM bytes. Use matching fixed-seed os20
  cases for the affected carts and compare pixels, display palettes, PCM and
  pair counts independently. Require repeated native/QEMU output, same-ELF
  RTL output/memory oracles, scanout and a corrupted-pixel negative control.
  Export raw channels before claiming byte identity; hashes alone do not prove it.
  Standard `pico8/run.py` has no raw-channel exporter or cross-case compare action.
  Use a separately reviewed collector for byte comparisons; otherwise report
  output-hash agreement and leave byte identity unverified.
- For interpreter changes, also check that runtime error messages keep their
  line numbers, and that save states still replay (`make test` covers both).
- Ship a test zip and ask the user for overlay screenshots of the same scene.
  Host numbers are estimates.

## Shared device evidence collection

Use the local [pocket-benchmarking router](../pocket-benchmarking/SKILL.md) and
[project guide](../../../docs/BENCHMARKING.md) for profile selection, actual
package identity, screenshot-backed device summaries and comparison modes.
Keep per-cart hashes/routes, logical and visible rates, render divisor and audio
settings separate. Collection complements every native, package and device check
above; importing a summary neither certifies transcription nor adds RTL support.

## Simulated runs: use the bounded os20 adapter

Before creating, building or replaying a simulated case, read
[bounded os20 replay](references/os20-replay.md) for harness discovery, platform
verification, commands, provenance and independent output/negative controls.
Always select `--platform os20`; the CLI and Python API default to os25.
The adapter models runtime capacity at 90 MHz; it excludes device presentation,
render skipping and OS scheduling. Require hardware evidence for device FPS or
full parity claims, and preserve historical os25 evidence at its original clock.

## Historical bare-metal os25 evidence

The 2026-10-11 PICO-8 experiment used a separate bare-metal os25 build, not the
shipped application ELF. Preserve its original model/clock, code-layout and
Lua-hash-seed qualifications. Current bounded os20 runtime replay is a separate
workflow; neither model includes the complete device presentation, render-skip
and OS scheduling paths. Historical os25 timings cannot be frequency-rescaled
into os20 device FPS. The collector's `harness-revision` import remains limited
to Butterscotch os25 reports and does not import these PICO-8 replay results.
