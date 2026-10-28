---
name: pico8pocket-release
description: Build and verify Pico8Pocket distributions, prepare matched os20 API/runtime pin updates, coordinate explicit draft releases, and hand off hardware test builds. Use for dependency refreshes, packaging, release metadata, A/B cores and release PRs in the Pico8Pocket repository.
argument-hint: "[card: SD mount path]"
---

# Pico8Pocket builds, device testing and PRs

For device installation, accept an optional SD mount path (`$CARD`) such as `/Volumes/Pocket`.
This repository owns release orchestration. Read [docs/RELEASING.md](../../../docs/RELEASING.md).
The authoritative
dependency pins remain in `scripts/fetch-deps.sh`; app source is this repo's
commit, not a second core repository. This skill lives in `.agents/skills/`,
with a Claude symlink to the same file.

## Matched runtime refresh

- Run **Prepare runtime pin update** in this repository, with a published
  runtime tag or an empty input for the latest stable release. It requires the
  complete `os20.rbf_r`/`os.bin`/`loader.bin` set, resolves the tag to its source
  commit, checks API 0.9.x/save-state headers, and produces a review patch.
- Read `runtime-pin-candidate/REVIEW.md` and the patch. Hashes fingerprint the
  downloaded candidate; they do not establish correctness or hardware testing.
  Compare exact source endpoints after history rewrites. Do not substitute
  os25, advance unrelated SDK/Fake-08/z8lua pins, or regenerate hashes merely
  to accept an unexpected file.
- Apply at the release checkout commit in the report, using `git apply --check`
  first. Local preparation uses `python3 scripts/release.py prepare-runtime`
  with `--source`, `--runtime`, `--tag`, and optional `--apply`; see the release
  guide for fetching both source endpoints. An empty patch is a no-op.
- Review all changed pin/documentation files, run release tests, make a signed
  conventional commit and push a topic branch only with session authorization.
  Preparation success alone is not build or hardware evidence.

## Complete distribution and draft releases

Require **Build and verify distribution** (`distribution.yml`) on the exact candidate commit:
three native Go platforms, actual Extism/WASI fixture integration, C/C++ host
regressions, the pinned GCC 13.2 core build/public APF package, and final
distribution/provenance/checksums. An older green commit does not qualify.
Inspect annotations for runner-acquisition failures; retry only with
authorization and at most twice. Required abandoned/cancelled jobs remain
incomplete; do not suppress failures or retry code failures blindly.

Before release, record physical-Pocket cart launch, controls/audio,
cartdata/progress persistence, Memories and sleep/wake checks. Offline slot-20 loading, handoffs and return navigation need device evidence;
interactive Pocket Sync UI behavior also needs an application run. Use the user's own
carts privately and state unperformed checks. CI does not establish hardware
compatibility.

Obtain the version and explicit release authorization before dispatching with
`create_draft=true`; `version` must be `v` plus the version already committed
in `dist/core/core.json`. The workflow gates a complete draft on all checks:
core ZIP, offline plugin/manifest, collections docs/licenses,
source archive, core provenance and global checksums. Do not publish a tools-only
latest release or include local carts/saves. Inspect the draft assets and
report the link; stable publication requires separate explicit authorization.
Dispatch creates a fresh tag at the tested commit and refuses existing tags.
If it fails after tag creation, inspect state and ask before recovery; do not
delete tags or repeatedly rerun the workflow.

## Building a test zip

- Run `make test`, then `make package USE_SDK_CONTAINER=0`. The second writes
  `releases/pico8pocket-v<VERSION>.zip` and must print "APF package
  validation passed: zenibako.pico8 on pico8".
- Run `rm -rf .obj/pico8pocket` first if `include/p8p/vm_fasttext.h` changed.
- Copy the zip to the repo root under a descriptive name, for example
  `pico8-zenibako-v0.0.33-<topic>.zip` (`*.zip` is gitignored), and send it
  with SendUserFile. The user tests every change on a real Pocket.
- The SDK comes from the pinned Diablo source, with API headers and the
  matched os20 runtime from the fork's pinned runtime release. Read current
  pins from `scripts/fetch-deps.sh`; do not infer a compatible runtime from
  whichever upstream commit happens to be newest.
- Public zips never contain carts. `make package-local` bundles local carts
  and is for private use only.

## SD-card layout (core zenibako.pico8, platform pico8)

```text
Cores/zenibako.pico8/              core.json, data.json, os20.rbf_r, loader.bin, ...
Assets/pico8/common/               pico8pocket.elf, pico8pocket.ini, os.bin,
                                   p8p_pad5/6/7, cards/ (user carts)
Assets/pico8/zenibako.pico8/       pico8pocket.json (the game entry, slot 0)
Platforms/pico8.json
Platforms/_images/pico8.bin        platform banner
```

- The core ID is `author.shortname` from `dist/core/core.json`. The asset and
  platform folders come from `platform_ids[0]`.
  - `scripts/assemble-pocket.sh` reads both.
  - `scripts/validate-pocket.sh` checks the slot layout os20 needs.
- Users must extract with folder **merge**. macOS Finder's Replace deletes
  existing folders; Option-drag and choose Merge.
- Upgrading from `Askent.pico8pocket` means moving carts and saves from the
  `pico8pocket` folders to `pico8`.

## Benchmark evidence

For platform shortlisting, packaged artifact/profile identity and device
comparisons, use [the project benchmark guide](../../../docs/BENCHMARKING.md)
and the local [pocket-benchmarking router](../pocket-benchmarking/SKILL.md).
Variant-core packaging isolates files; it adds no durable results exporter.
Use the canonical harness's `os25-harness` skill for bounded os20 runtime replay;
that simulation does not establish complete device parity. Preserve every build,
compatibility and physical Pocket gate in this skill; collector import success
is not hardware acceptance.

## Side-by-side A/B testing

```sh
scripts/variant-core.sh releases/pico8pocket-v<VER>.zip b "Test build B" b.zip
```

- This produces core `zenibako.pico8b` on the same platform, sharing the
  cards.
- Every file it owns has a unique name: game entry, elf, ini, cfg and saves.
  Same-named game entries in two core folders made one disappear when the
  user copied files, so keep names unique.
- Name the variants A and B, say exactly what differs, and ask for the same
  scene's overlay from each. One screenshot can catch a spike, so don't
  call a regression from a single device overlay; ask for the A/B pair.
- To install both from a Mac, set `$CARD` from the optional SD mount path,
  asking the user if it wasn't given. The files the two
  zips share are identical, so order doesn't matter:
  ```sh
  unzip -o pico8-zenibako-<VER>-A-<topic>.zip -d "$CARD"
  unzip -o pico8-zenibako-<VER>-B-<topic>.zip -d "$CARD"
  dot_clean -m "$CARD/Assets" "$CARD/Cores" "$CARD/Platforms"
  diskutil unmount "$CARD"
  ```
  If the unmount is "dissented by" a `com.apple.Virtualization` process,
  the Claude desktop app has the card open for a session that read it;
  once the copy is done, `diskutil unmount force "$CARD"` is safe.

## Checking what is on a card

`core.json` carries the same version for every build of a release, so it
can't tell builds apart. Compare each core's elf with the same-named elf
in its candidate zip (`unzip -p ZIP Assets/pico8/common/NAME.elf | cmp -
"$CARD"/Assets/pico8/common/NAME.elf`): `pico8pocket.elf` for the
main core and `pico8pocketb.elf` for B, which `scripts/variant-core.sh`
renames. `Cores/zenibako.pico8b` only shows that some B core is installed.

## Platform banner

- Format: 521x165 grayscale, stored rotated 90 degrees counter-clockwise as
  16-bit pixels. The first byte is the inverted brightness and the second is
  0, matching the SDK's reference `openfpgaos.bin`.
- Banners can only be monochrome.
- To regenerate after changing `assets/branding/pico8-platform.png`, run
  `scripts/platform-image.py PNG dist/platforms/_images/pico8.bin`. Decode
  the result back to an image to check it before shipping.

## Branches, pushes and PRs

- Develop on the session's designated branch. After its PR merges, restart
  the branch from `origin/main`. The user may also ask for a new branch per
  change.
- PR descriptions: start with an empty `## Summary` section for the user
  to fill in, then a `---` divider, then the full agent-written details.
- Check that a PR is still open before pushing to its branch. The user
  merges on their own schedule, and commits pushed after the merge never
  reach `main`; put them on a new branch from `main` instead.
- To bring an open branch up to date, merge `main` into it rather than
  rebasing, since rebasing a pushed branch needs a force-push.
- Pushes to GitHub sometimes fail with "Internal Server Error". Retry in a
  loop with a pause; never force-push.
- Commit messages end with the Co-Authored-By and Claude-Session lines. No
  model identifiers in commits or PRs.
- CodeRabbit reviews PRs against `main` automatically:
  - reviews are rate-limited (its review notes on this repo report one per
    hour); a rate-limited PR says when the next is available, so wait until
    then and comment `@coderabbitai review` if it has not started.
  - It never reviews closed or merged PRs.
  - For a PR whose base isn't `main`, comment `@coderabbitai full review`.
  - To review already-merged work, push a branch at the old base and open a
    review-only PR from `main` into it.
- The user wants PRs merged only after CodeRabbit has finished. For each
  finding:
  1. Check it against the code.
  2. Fix the real ones, with a test where possible.
  3. Reply on the thread with the commit and resolve it.
  4. For findings not worth their code, explain why and leave the call to
     the user.

## Offline plugin replacement

The root `plugin.json` is canonical and retains the same repository install URL.
`make offline-plugin-tools` builds Go 1.25.0/TinyGo 0.43.0 WASI artifacts with
Binaryen 133 and retained complete notices in `build/offline-plugin/`. Run
`make offline-plugin-test`, then require the actual Extism fixture harness with
`REQUIRE_PLUGIN_TESTS=1 POCKET_PLUGIN_WASM="$PWD/build/offline-plugin/plugin.wasm" go test ./integration -count=1`.
The harness uses generated carts and local HTTP mocks, including 15-second
courtesy pacing; it has no live-network mode. CI must not download real carts.
The native importer is `go run ./cmd/pocket-sync-cache`. Do not restore retired
thumbnail assets or tools to satisfy old release inventories.
Preserve existing SD artwork, installed bundles, cartridges and saves.
