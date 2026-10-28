---
name: pico8pocket-release
description: Use when validating Pico8Pocket distributions, preparing drafts, publishing explicitly authorized releases, or coordinating runtime updates and device installation.
---

# Pico8Pocket releases

Read repository instructions and the [release guide](../../../docs/RELEASING.md) first.

## Read the shared workflow

Canonical owner: <https://github.com/zenibako/pocket-dev-tools> (formerly
`pocket-harness`). Use the root established in this session, then
`POCKET_DEV_TOOLS_ROOT`, then legacy `POCKET_HARNESS_ROOT`. Resolve conflicting
explicit roots before proceeding. A sibling `pocket-dev-tools` or `pocket-harness`
is only a candidate: verify `.agents/skills/pocket-release/SKILL.md` and
`docs/RELEASING.md`, confirm the Git remote identifies the canonical repository
(or its former name), and record its full commit. Resolve and read that canonical
skill before following its relative links. Apply it together with this project's
local release guide; stronger local checks remain required.

If the shared checkout is unavailable, report the missing dependency and continue
applicable local checks. Do not claim shared validation, copy the shared skill
body, or create cross-repository skill symlinks. A checkout predating the release
skill needs an explicit update; a matching directory name alone is insufficient.

## Project contract

The current os20 SDK/API/runtime pins, complete distribution workflow, offline collections and physical cart/save/Memory/sleep checks remain project-owned. Use this checkout's `distribution.yml` and version rules.

Read the preserved [project procedures](references/project-release.md) for
local commands, device checks and release recovery details.
