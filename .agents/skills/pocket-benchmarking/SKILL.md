---
name: pocket-benchmarking
description: Use when choosing Pico8Pocket profiles, preserving routine regression baselines, planning benchmark cores, or collecting and comparing profiler evidence.
---

# Pocket benchmark evidence

## Shared repository identity

The canonical repository is <https://github.com/zenibako/pocket-dev-tools>,
formerly `pocket-harness`. For discovery use a root explicitly established in
the session, then `POCKET_DEV_TOOLS_ROOT`, then legacy `POCKET_HARNESS_ROOT`.
Resolve conflicting explicit roots before proceeding. Both sibling directory
names are candidates only; validate the canonical benchmark skill and collector
as described below. Existing local paths and CLI names remain supported.

Read this project's [benchmark guide](../../../docs/BENCHMARKING.md), then use
the maintained workflow in the shared checkout resolved above. Validate that it contains
`.agents/skills/pocket-benchmarking/SKILL.md` and `benchmarks/__main__.py`;
resolve the shared skill's canonical path before following its relative links.
Read that skill and its `benchmarks/README.md`, `PLATFORMS.md` and `SELECTION.md`
as relevant. Run collector commands from the validated harness root.
If unavailable, report the missing dependency; do not copy or invent the workflow.

Keep project performance, compatibility and release skills responsible for
workload, build and correctness checks. Device summaries need original profiler
screenshots, per-cart hashes/routes, logical rates, V estimates, separately
measured presentation rates, render skipping, audio settings and actual
package/runtime identity. Exclude saturated timing metrics from numeric comparisons;
the project guide defines rounding, censoring and matched-roster requirements. Transcriptions stay manual
and unchecked. Variant-core packaging supplies no durable results exporter.
For bounded os20/90 MHz PICO-8 runtime replay, use the canonical harness's
`.agents/skills/os25-harness/SKILL.md` and `pico8/README.md`. That simulation
excludes production OS/presentation paths and does not establish device parity.
Historical bare-metal os25 experiments retain their original build/model; the
collector's Butterscotch-only `harness-revision` importer does not add PICO-8
report import support or extend any simulation model.

## Routine regression checks

For recurring software checks, require the shared `benchmarks/REGRESSIONS.md`
and read it with this project's benchmark guide. The contract was introduced in
`fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58`; record the actual verified shared
checkout commit. Preserve the accepted reference, create independent candidates,
and require project correctness and negative controls before interpreting deltas.
A missing adapter or fixture is an explicit coverage gap. Collector `verify` or
`compare` is not an output-regression gate or a substitute for device checks.
Use the separate bounded os20/90 MHz adapter and native/cart checks. Require
repeat and native-output agreement; distinguish exported channel hashes from raw
byte equality. Butterscotch's `regression.py` does not accept Pico8 cases.
