# Bounded os20 runtime replay

## Simulated runs: use the bounded os20 adapter

The harness is the private repository
[zenibako/pocket-harness](https://github.com/zenibako/pocket-harness); this
adapter lives in its `pico8/` directory (os20 support since `7e747e7`). Take
the checkout's location from the **harness** argument, then
`OS25_HARNESS_ROOT`; ask once if neither is set, and don't hunt for it.
Read its `README.md`, `AGENTS.md`, canonical
`.agents/skills/os25-harness/SKILL.md` and `pico8/README.md`. The local benchmark
router also supports shared checkout discovery for collection. Carts, cases and
traces stay in its gitignored `experiments/`; never commit them there or here.

From the harness root, verify an existing platform before using it. If
`build/platforms/os20` does not exist, run
`python3 pico8/cpu_platform.py build --jobs 4` once, then verify it. For an
existing directory without `platform.json`, preserve and inspect the failure
logs; use the documented `rtl` resume action if CPU generation succeeded.
Do not rebuild or replace a completed platform or historical cases.

```sh
python3 pico8/cpu_platform.py verify
# Create each baseline/candidate under its own name:
python3 pico8/run.py create --source SOURCE --archive CARTS_ZIP \
  --name NEW_CASE --platform os20 --hash-seed 0x5eed1234 --measure-frames 12
python3 pico8/run.py build --case EXACT_CASE
python3 pico8/run.py native --case EXACT_CASE --cart CART_ID
python3 pico8/run.py capture --case EXACT_CASE --cart CART_ID
python3 pico8/run.py replay --case EXACT_CASE --cart CART_ID
python3 pico8/run.py replay --case EXACT_CASE --cart CART_ID --traffic scanout
python3 pico8/run.py replay --case EXACT_CASE --cart CART_ID --negative
python3 pico8/run.py report --case EXACT_CASE
```

- Always pass `--platform os20`: the CLI and Python API defaults remain os25.
  Require `case.json` to record `platform_name=os20`, `clock_hz=90000000` and
  matching CPU/simulator provenance. RTL must emit `PICOCPU 90000000` after
  reading the actual MMIO register. Compare A/B on the same platform, seed,
  input/setup, frame window, audio model, compiler and BRAM placement.
  Inspect `capture.json` for both `passed=true` and `native_output_equal=true`;
  QEMU repeatability can pass even when native output differs. Resolve cart IDs
  from each case inventory. A runtime change uses separate source snapshots;
  an authorized cart-code change uses distinct, recorded cart archives.
- Reach the user's scene with `--setup`, `--input`, `--warmup-frames` and
  `--trace-frames`; record whether it was recreated or imported from an actual
  save. A scripted route may stay on a title screen. Use requested carts and
  affected scenes before commissioning another full sweep.
  The standard CLI has no save-import option; setup/input recreate a scene.
  Claim an actual save import only with separately validated import evidence.
- Report **simulated** frame time and runtime capacity using the recorded
  clock: `90000000 / mean_cycles`, capped at target FPS for the estimate.
  Preserve historical os25 results with their original CPU and 100 MHz clock;
  they cannot be converted to os20 by rescaling clock alone.
- The platform is pinned to frozen OS `8318f25`. The production runtime and
  hashes come from `scripts/fetch-deps.sh` (currently the `openfpgaCore`
  `pocket-runtime-a58aacd` release). Its CPU/cache/SDRAM source profile agrees
  with the frozen adapter, but save-state gateware and firmware differ. Recheck
  source configuration when the production pin changes; source-profile agreement
  does not prove fitted-bitstream or device timing parity. Keep the bitstream,
  OS and loader from the same pinned release set. Record compiler, libc, flags,
  alignment and BRAM layout against the intended device ELF: the harness's
  GCC13/bounded allocator build can differ from the staged SDK build.
- Runtime replay excludes presentation, adaptive frame skipping, OS/SD and
  live display/audio scheduling; caches start cold and PCM consumption is fixed.
  Require hardware evidence for device FPS or full parity claims. Butterscotch's
  os25 calibration does not calibrate Pico8Pocket on os20.
- Existing single-lane os25 cache/pipeline observers are not validated for
  dual-issue os20. Distinguish instruction shares from cycle/frame-time shares.
