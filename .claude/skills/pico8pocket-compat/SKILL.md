---
name: pico8pocket-compat
description: Debug PICO-8 cart compatibility problems and change the patched z8lua interpreter safely - reproducing a runtime error a user saw on the Pocket, deciding cart bug vs core bug, writing and registering z8lua patches, and keeping save states loadable. Use for any "cart X errors / misbehaves" report and before editing anything under .deps or patches/.
---

# Pico8Pocket compatibility and z8lua patches

## Reproducing a user-reported cart error

1. Get the cart's Lua with `build/tools/pico8pocket_compat_scan CART --source`.
   Error lines such as `[string "cart.p8"]:1989` refer to this text.
2. Read the failing line and work out which state reaches it. Often it is
   save data (`cartdata`/`dget`) or a late game stage.
3. Reproduce without playing:
   ```sh
   build/tools/pico8pocket_compat_scan CART \
       --bench "(function() max_level_id=10 return 1 end)()" 30 1
   build/tools/pico8pocket_compat_scan CART --eval FRAMES BUTTONS "expr"
   ```
   `--bench` runs the Lua expression after load. `--eval` prints a value
   after N frames.
4. Decide whose bug it is:
   - Check the official manual (v0.2.7 target) and Fake-08 (`.deps/fake-08`).
   - Example: Yarn calls `exit()`, which exists in neither, so it was a cart
     bug. A no-op shim kept it playable.
   - Explain this to the user and offer the choice. Note that cartdata keeps a
     crash recurring at every launch.
5. Count the local carts affected:
   `for f in carts/*; do scan $f --source | grep -q PATTERN && echo $f; done`.
   A cart may define the same name itself; Snail defines its own `exit`, and
   cart globals override built-ins.
6. Add a regression test to `tests/runtime_test.cpp`, using an inline
   `__lua__` cart like the surrounding blocks. Check the test fails without
   the fix.

## Editing z8lua: always through patches

- `scripts/fetch-deps.sh` checks out the pinned Fake-08/z8lua and applies
  `patches/z8lua-*.patch` in the order of its `Z8LUA_PATCHES` list, using
  `git apply`.
- `make deps` (also run by `make test` and `make compat-scan`) resets the tree
  whenever the patch set's digest changes. Edits made directly in `.deps` are
  silently lost.
- Workflow:
  1. Copy the current files to `a/` and `b/` in the scratchpad.
  2. Edit `b/`.
  3. Write the patch: a short description paragraph, then
     `diff -u a/f b/f` with headers rewritten to `diff --git a/f b/f`,
     `--- a/f`, `+++ b/f`.
  4. Add it to `Z8LUA_PATCHES` after the patches it builds on.
  5. Run `make compat-scan`, then `cmp` the files under `.deps` with `b/` to
     prove the patch applies.
- Patches stack, so a later patch's context includes the earlier ones. To
  change an already committed patch, add a new patch on top rather than
  rewriting history.
- The Pocket build compiles z8lua as C++ (`-x c++`), and fix32 is a C++ class.
  Keep patch code valid in both C and C++.
- VM invariants the patches rely on:
  - The program counter is a local `pc`. Anything that can call out, error,
    hook or yield must `savepc()`/`Protect`.
  - Hooks are only checked at servicepoints (jumps, back-edges, calls).
  - Load mode is text-only, because opcodes are trusted.
- After touching the VM:
  - Compare runtime error messages and line numbers between old and new
    scanners for a set of erroring snippets (arithmetic on nil, calling nil,
    `for` limit, comparisons, concat, `__index` errors, C-API argument
    errors).
  - Run the frame comparison (`scripts/compare-frames.sh`).

## Save-state compatibility (Eris)

- Permanent object IDs are assigned in `eris.__p8p_init` (`bootstrap_lua` in
  `src/runtime/runtime.cpp`). The order is the sorted keys of `_G`, then the
  `late` list.
- A new global must go in `late_runtime_api` and in the matching
  `late={...}` list in that Lua, otherwise every existing save state breaks.
  Reusing an existing C function value (such as `api_stub`) adds no ID.
- New runtime or Proto fields are not persisted. Rebuild them lazily (for
  example `Proto.icache`, allocated on first call) and keep the serialized
  layouts unchanged.
- Audio state is saved in the 0.0.32 layout with 48 kHz units, converted on
  save and load.
- To check cross-version: build a small harness against the base commit's
  sources and against yours. Save after N frames with the old build, load
  with the new, and compare the next 60 frames' hashes across all local
  carts. They must be identical.

## Determinism caveats when comparing builds

- `scripts/compare-frames.sh` is a legacy frame-hash check using
  `luai_makeseed()=0u` and `setarch -R`; it does not verify the final `g->seed`
  after address mixing. Do not dismiss a DIFF as layout without investigating it.
- For deterministic target replay, follow `../pico8pocket-performance/SKILL.md`
  and the harness's `pico8/README.md`: create independent baseline/candidate
  cases with explicit `--platform os20 --hash-seed 0x5eed1234`. Confirm the final
  seed in native/QEMU (`PICOSEED 1592594996`) and the 90 MHz MMIO clock in RTL
  (`PICOCPU 90000000`). Keep setup, input, window, audio and build flags equal.
  Require `capture.json`'s `native_output_equal=true` as well as `passed=true`.
- Compare pixels, display palette, PCM and sample-pair counts independently;
  require raw exports before asserting byte identity. Same-ELF pointer-bearing
  checkpoint hashes are not cross-build or cross-version save-state oracles.
  Standard harness commands export channel hashes, not raw channel bytes;
  use a separately reviewed collector or limit the claim to hash agreement.
- os20 matches the documented CPU configuration. Exact shipped-bitstream,
  compiler and OS/presentation parity remain separate evidence; retain device
  error/input and save-load checks where the change affects those paths.
- Changes to hook timing shift when scripted input lands in carts with long
  `_init` slicing. Check those carts for errors and plausible frames rather
  than identical hashes.
