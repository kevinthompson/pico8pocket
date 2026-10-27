---
name: pocket-benchmarking
description: Use when selecting Pico8Pocket platform profiles, planning benchmark cores, or collecting and comparing device profiler screenshots with pocket-harness.
---

# Pocket benchmark evidence

Read this project's [benchmark guide](../../../docs/BENCHMARKING.md), then use
the maintained workflow in an established `pocket-harness` checkout. Take its
path from the session or `POCKET_HARNESS_ROOT`. With neither, consider only
`../pocket-harness` relative to this repository root. Validate that it contains
`.agents/skills/pocket-benchmarking/SKILL.md` and `benchmarks/__main__.py`;
resolve the shared skill's canonical path before following its relative links.
Read that skill and its `benchmarks/README.md`, `PLATFORMS.md` and `SELECTION.md`
as relevant. Run collector commands from the validated harness root.
If unavailable, report the missing dependency; do not copy or invent the workflow.

Keep project performance, compatibility and release skills responsible for
workload, build and correctness checks. Device summaries need original profiler
screenshots, per-cart hashes/routes, logical and visible rates, render skipping,
audio settings and actual package/runtime identity. Transcriptions stay manual
and unchecked. Variant-core packaging supplies no durable results exporter.
For bounded os20/90 MHz PICO-8 runtime replay, use the canonical harness's
`.agents/skills/os25-harness/SKILL.md` and `pico8/README.md`. That simulation
excludes production OS/presentation paths and does not establish device parity.
Historical bare-metal os25 experiments retain their original build/model; the
collector's Butterscotch-only `harness-revision` importer does not add PICO-8
report import support or extend any simulation model.
