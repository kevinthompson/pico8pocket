# Pico8Pocket project routing

Read [README.md](README.md), [architecture](docs/architecture.md) and the relevant
[compatibility notes](docs/compatibility.md) before changing behavior. The target
is PICO-8 v0.2.7. `scripts/fetch-deps.sh` owns dependency pins; record the selected
SDK and actual packaged binaries when measuring. A cached `.deps` tree can be
stale; reconcile identity differences before using it, without changing pins to
make a measurement fit.

Project skills live physically in `.agents/skills`; `.claude/skills` links to
that complete collection so Codex and Claude discover the same four skills:

- [Compatibility](.agents/skills/pico8pocket-compat/SKILL.md): cart semantics,
  z8lua patches and save-state checks.
- [Performance](.agents/skills/pico8pocket-performance/SKILL.md): slow scenes,
  profiler interpretation and optimization checks.
- [Release](.agents/skills/pico8pocket-release/SKILL.md): packaging and device
  installation and hardware acceptance.
- [Pocket benchmarking](.agents/skills/pocket-benchmarking/SKILL.md): shared
  harness discovery and evidence collection/comparison.

Use [the benchmark guide](docs/BENCHMARKING.md) for platform selection and
screenshot-backed device comparisons. The shipping architecture is os20/90 MHz;
historical bare-metal os25 simulation does not qualify its device performance.
Ordinary compatibility or packaging work need not use the shared collector.
Keep private cartridges, profiles, measurements and screenshots out of commits
and public packages. Existing compatibility, native and release checks remain
required for changes that affect them.
