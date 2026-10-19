# Pin updates and releases

This repository owns the Pico8Pocket app, thumbnail tools and release process.
The existing [architecture](architecture.md#hardware-and-runtime) defines the
matched os20 SDK/API/runtime combination. The approved
[thumbnail-tools design](superpowers/specs/2026-10-19-cart-thumbnails-design.md)
defines the independent Go CLI/WASI/plugin and its hardware verification limit.
`scripts/fetch-deps.sh` remains the authoritative dependency lock; no second
copy of those pins is introduced. Release app source is the exact Git commit.

## Prepare a runtime pin update

Run **Prepare runtime pin update** (`update-runtime.yml`) in this repository.
Supply a published `zenibako/openfpgaCore` runtime release tag, or leave it
empty to resolve the latest stable release once. The job requires the three
os20 assets and API 0.9.x/save-state support. It compares the old and candidate
source trees directly, and uploads `runtime-pin-candidate` with `REVIEW.md`
and `runtime-pin.patch`. The report records the release checkout commit,
candidate source SHA and hashes of downloaded assets.

This read-only workflow does not create a branch, commit, PR or release. The
new hashes identify a candidate; they are not independent evidence of device
compatibility. Inspect the runtime source/release and patch. Apply it in a
clean release checkout at the report's commit, first using
`git apply --check runtime-pin.patch`, then `git apply runtime-pin.patch`.
An empty patch means the bundle is already pinned; do not invent a commit.

For local preparation, use a separate clean runtime-source checkout at the
candidate release tag. Fetch the previous `FORK_COMMIT` too (a shallow fetch
of that SHA is sufficient). Download the three assets from that same release
into a separate folder, then run:

```sh
python3 scripts/release.py prepare-runtime --source /path/to/runtime-source \
  --runtime /path/to/downloaded-bundle --tag pocket-runtime-COMMIT
# After reviewing build/runtime-update/REVIEW.md and runtime-pin.patch:
python3 scripts/release.py prepare-runtime --source /path/to/runtime-source \
  --runtime /path/to/downloaded-bundle --tag pocket-runtime-COMMIT --apply
python3 -m unittest discover -s tests -p 'test_*.py'
```

The helper updates the API source SHA, runtime URL/hashes and current runtime
references in the README, architecture and notices together. It preserves
os20 and all unrelated SDK/Fake-08/z8lua pins. New SDK/API versions or another
bitstream variant require a separate compatibility change. Existing dependency
checkouts at old pins are refused by `make deps`; keep them for recovery and
use a fresh dedicated build checkout for a candidate instead of discarding a
user's dependency modifications.

## Verify the committed candidate

Review and sign a conventional commit on a topic branch. **Build and verify
distribution** (`thumbnail-tools.yml`) runs on pushes, PRs and explicit
dispatches. It requires:

- Go tests/vet on Linux, macOS and Windows.
- Pinned TinyGo/Binaryen tool builds and actual compiled plugin/WASI tests.
- Host C/C++ regression tests with pinned dependencies. Local Celeste acceptance
  checks skip without the user's cart; this is reported, not hardware evidence.
- The public, cartridge-free core built with Ubuntu 24.04 GCC 13.2.0
  (`gcc-riscv64-unknown-elf=13.2.0-11ubuntu1+12`) and validated as an APF package.
- Complete distribution assembly, exact runtime hashes inside the core ZIP,
  RISC-V ELF identity, per-file provenance and global checksum verification.

`pico8pocket-distribution` contains the versioned core ZIP, plugin and manifest,
six native thumbnail bundles, WASI CLI, thumbnail docs/licenses, source archive,
`CORE-PROVENANCE.json` and `SHA256SUMS`. The old tools-only checksum file is
replaced by one covering the complete set. Provenance records this repository's
commit, dependency pins and per-core-file hashes. The source archive is the
tracked app/tool source at that commit; dependency URLs/revisions and local
patches allow fetching/reconstructing the corresponding external sources.

For local core checks, run `make deps`, then `make test`, and
`make package USE_SDK_CONTAINER=0`. Do not run a parallel dependency fetch
alongside compilation. Local builds with another compiler are test builds;
release CI records and enforces GCC 13.2.0. Never use `make package-local` for
public artifacts. Neither workflow downloads cartridges or includes saves.

## Draft and publication

Complete CI must be green on the intended commit. Test the candidate on a real
Pocket using private carts: launch, controls/audio, persistence, Memories and
sleep/wake. Record missing checks explicitly; a runtime or API change needs
renewed hardware compatibility checks. Native-picker thumbnail rendering also
requires device evidence, as described in the thumbnail design.

After choosing and committing the core version, explicitly dispatch the build
workflow on that ref with `version=vMAJOR.MINOR.PATCH` and `create_draft=true`.
The version must match `dist/core/core.json`. After all checks pass, CI creates
a fresh tag bound to the tested commit and a complete draft, never a stable
release. Existing tags are refused, including tags at the same commit. If
draft creation fails after tag creation, inspect the tag/draft state and ask
the maintainer how to recover; do not delete tags or retry blindly.
Inspect assets, checksums and notes
before an explicitly authorized stable publication. Do not overwrite an
existing tag/release or publish a tools-only latest release.

The repository-owned [release skill](../.agents/skills/pico8pocket-release/SKILL.md)
coordinates this process and retains the device/A-B installation guidance.
Claude reads the same skill through a symlink.
