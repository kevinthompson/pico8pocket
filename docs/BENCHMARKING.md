# Pico8Pocket benchmark evidence and platform selection

Start with the existing [architecture](architecture.md#hardware-and-runtime),
[PICO-8 compatibility target](compatibility.md) and [release process](RELEASING.md).
They define the shipping os20/90 MHz configuration, behavior and acceptance gates.
Use the local [pocket-benchmarking skill](../.agents/skills/pocket-benchmarking/SKILL.md)
to locate the maintained `pocket-harness` checkout. Its `benchmarks/README.md`
owns collection, `PLATFORMS.md` owns profiles/comparison contracts and
`SELECTION.md` owns project-specific selection. Commands below run there; paths
under `build/` are private examples, not checked-in measurements.

## Choose candidates before measuring

Shortlist configurations compatible with this application's ISA, FPU, memory,
SDK/API, storage and lifecycle requirements. Start from the current shipping
profile. Record candidate/selected/incompatible/deferred dispositions and reasons
in a private selection note. The matrix shows os20/os25 × 90/100 MHz as separate
axes; this project need not build or test all four. A generic runtime release
is a candidate, not evidence of a compatible Pico8Pocket package.

Choose representative carts and scenes, a route version, frame-budget/visible-rate
and audio targets, warmup, sampling and correctness criteria before running.
Celeste 2, Kiloman, Moss Moss and Tetyis are existing performance priorities;
keep required companion carts available privately. Use repeated paired trials on
the same Pocket, including normal gameplay pacing and separately labeled fixed-work
diagnostics. Recommend the best eligible tested configuration against those
criteria; report a tie or insufficient evidence when appropriate. A single tested
profile demonstrates suitability, not superiority over untested configurations.

## Identify the package actually measured

`scripts/fetch-deps.sh` defines intended pins. Record the app revision, dirty
changes, selected `SDK_ROOT`, compiler/version, flags and relevant BRAM/layout.
Reconcile source pins, cached `.deps`, release provenance and actual packaged
bytes before drawing conclusions. A local SDK override or stale cache is not
identified by the source lock alone. Investigate mismatches; do not rewrite pins,
profile hashes or artifact identities to hide them.

For each real candidate, create and verify a profile using the shared platform
guide. Declare the actual OS/SDK sources, ISA, issue width, caches, features and
clock. Hash the packaged `os.bin`, `os20.rbf_r` (or the actual candidate RBF),
Chip32 `loader.bin` and the CPU configuration used to build it. Keep the OS/RBF/
loader release set matched. `profile verify` checks those local file hashes;
it does not establish embedded loader contents, source-to-binary correspondence,
FPGA fit/timing closure or which bytes ran on the Pocket.

Hash the packaged application ELF too. The assembled tree puts it and `os.bin`
under `Assets/pico8/common/`, with the RBF and loader under
`Cores/zenibako.pico8/`; variants rename application/configuration/save files.
Use actual package paths and compare the installed ELF against the candidate ZIP
when checking a card. Record effective `pico8pocket.ini`, persisted `.cfg`, display,
audio, controls, pacing, initial save state and observer settings in `conditions`.
Keep a stable `conditions.asset_manifest_sha256` for the real cart/companion set,
with per-cart SHA-256 and content identity. Source revisions, filenames and the
common release version alone do not identify the measured package.

## Capture and archive device evidence

Pico8Pocket currently exposes profiler/menu screens without a shared durable
benchmark exporter. Toggle the profiler with Select + physical X. Fix each cart
hash, scene/input route, initial state/seed, warmup and measured segment. Reach
the same scene in each build and capture original screenshots after sampling;
capture pauses can disturb timing. Record which scene was actually reached,
including whether it was recreated by a route or restored from a save.

Transcribe only displayed values into `device-summary` rows, with stable IDs
and literal units. Keep L (`logical_hz`) separate from V (`visible_estimate_hz`).
V is derived from L and the render divisor: while running with R > 1 it is L / R
rounded up; otherwise it equals L. It does not count presented frames and omits
the periodic skipped probe frames at R1. Visible-rate acceptance needs a separate
presentation-rate measurement and recorded method; reserve `visible_hz` for that
evidence. A fast logical rate or a V estimate does not establish actual display FPS.

Record U/D/A/P/F in explicit units, together with render divisor/skipping and
audio mode. These timings are smoothed, rounded to the nearest millisecond and
capped at 99. A displayed 99 is a censored reading, not an exact 99 ms: the
smoothed value can be 98.5 ms or anything higher, including 150 or 250 ms. Preserve
that observation in screenshots/notes, but omit the saturated metric from numeric
comparisons; use matching metric rosters on both sides or obtain an unsaturated
measurement. The collector has no bound-aware comparison mode. Converting a
rounded millisecond reading to microseconds does not add precision.

Do not sum overlapping counters or infer percentiles/sample counts from a
screenshot. The overlay's F covers runtime work beyond U + D. Keep audio-enabled
gameplay separate from muted or no-draw diagnostics; those modes do not establish
normal play performance.

Prepare the collector's full run metadata: project `pico8pocket`, cart/game and
case IDs, exact expected row roster, route version, sampling, repeat, physical
device ID/firmware, source/ELF/toolchain, effective conditions and bound profile.
Use null only where the format permits unknown values; never invent samples or
identity. Reusing a screenshot with a new repeat number is not another trial.

```sh
python3 -m benchmarks import --format device-summary \
  --source build/pico-values.json --metadata build/pico-run.json \
  --attachment build/pico-profiler.png --profile build/platforms/pico-os20-90.json
python3 -m benchmarks verify build/benchmarks/RUN_ID
python3 -m benchmarks matrix --runs build/benchmarks/RUN_ID \
  --profiles build/platforms/pico-os20-90.json --project pico8pocket
```

Attach every required screen. Import preserves the raw source/screenshots and
profile; it performs no OCR or transcription/correctness certification. A bound
`device_observed` record can remain unchecked; it is not correctness PASS.
Matrix counts are imports, not independent trials, and different application
identities produce separate scopes. Interpret untested cells through the shortlist.
No profile or measurement data belongs in this public repository.

## Use the comparison matching the question

| Mode | Required controls and meaning |
| --- | --- |
| Default `software` | Fixed platform/profile, compiler/flags/SDK/runtime, workload/conditions and sampling; application source/ELF may change. Two bound runs require identical profiles; bound/unbound comparisons fail. Legacy unbound pairs remain provisional. |
| `platform --vary ...` | Same known application source/ELF and toolchain, physical device, assets, complete workload/conditions and sampling. Declare exactly detected `clock`, `variant` and/or `runtime` factors. Different ELFs fail this mode. |
| `configuration --reason ...` | Known source/ELF/toolchain on both complete profile-bound device runs; differing ELFs/toolchains/profiles may describe a whole working configuration. Device, assets, full workload/conditions, adapter and sampling stay matched. The reason is required; `--vary` is unavailable. |

```sh
python3 -m benchmarks compare build/benchmarks/BASE build/benchmarks/CANDIDATE
python3 -m benchmarks compare build/benchmarks/BASE build/benchmarks/CANDIDATE \
  --mode platform --vary clock
python3 -m benchmarks compare build/benchmarks/BASE build/benchmarks/CANDIDATE \
  --mode configuration --reason "The candidate SDK requires a different application ELF."
```

Choose the applicable command, using actual differing factors. CPU/config/source
changes can require additional `--vary` declarations; follow `PLATFORMS.md`.
Configuration output retains changed components, field/presence differences, both
full build/profile contexts and source checks/limitations. Its descriptive delta
cannot isolate OS, CPU, cache or clock causality or choose an automatic winner.
Preserve unchecked screenshot status. Neither mode rescales 90/100 MHz timings;
use observed variability and independent acceptance checks in the selection note.

## Keep acceptance and timing domains separate

Preserve `make test`, applicable native/cart frame and save-state checks, package
validation/BRAM fit and all [release gates](RELEASING.md). Hardware acceptance still
needs launch, controls, graphics/audio, cartdata/progress, Memories, sleep/wake,
and affected offline slot-20/handoff/return paths. Missing private fixtures or
device checks remain explicitly unperformed. Host timing and instruction counts
are diagnostics; they are not Pocket FPS or independently checked compatibility.

The historical 2026-10-11 PICO-8 os25 experiment used a separate bare-metal build,
not the shipping ELF. It excluded presentation, adaptive render skipping and OS
scheduling; code layout and Lua hash seeding affected its results. Preserve its
original model/clock and labels. It cannot qualify shipping os20 device timing
or be converted by frequency scaling. The current shared harness separately
supports bounded os20/90 MHz PICO-8 runtime replay through its canonical
`os25-harness` skill and `pico8/README.md`. Select `--platform os20` explicitly
(the default remains os25), preserve fixed final seed/provenance and output
oracles, and retain compiler/allocator/cold-cache and omitted OS/presentation
limits. Its modeled CPU configuration does not prove fitted-bitstream or device
timing parity. Butterscotch's separate os25 calibration does not calibrate PICO-8.
The collector's `harness-revision` importer remains Butterscotch os25 only;
profiles and device summaries extend neither that importer nor the RTL model.

For side-by-side testing, use the existing [variant-core instructions](RELEASING.md)
and release skill. Prefer an isolated benchmark core with separate settings,
progress, save/Memory identities and measured build identity. A distinct core ID
or renamed variant supplies no results exporter, and shared runtime files must
not overwrite another installed runtime. Benchmark-core diagnostics and gameplay
pacing/lifecycle acceptance need their own cases.
