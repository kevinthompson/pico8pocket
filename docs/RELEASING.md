# Pin updates and releases

This repository owns the Pico8Pocket app, offline collections plugin and release process.
The existing [architecture](architecture.md#hardware-and-runtime) defines the
matched os20 SDK/API/runtime combination. The approved
[offline BBS design](https://github.com/zenibako/pico8pocket/issues/26)
defines the offline installer and optional bundle loader; its Go port note
records the retained Go/TinyGo implementation. The
[collections guide](OFFLINE-COLLECTIONS.md) documents the thumbnail-plugin migration.
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
python3 -m unittest discover -s tests -p 'test_release.py'
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
distribution** (`distribution.yml`) runs on pushes, PRs and explicit
dispatches. It requires:

- Go 1.25.0 native tests and vet on Linux, macOS and Windows.
- A TinyGo 0.43.0 WASI plugin build with Binaryen 133 and real Extism/WASI
  tests using generated
  project-owned fixtures; no private cartridges or live BBS requests are needed.
- Host C/C++ regression tests with pinned dependencies. Local Celeste acceptance
  checks skip without the user's cart; this is reported, not hardware evidence.
- The public, cartridge-free core built with Ubuntu 24.04 GCC 13.2.0
  (`gcc-riscv64-unknown-elf=13.2.0-11ubuntu1+12`) and validated as an APF package.
- Complete distribution assembly, exact runtime hashes inside the core ZIP,
  RISC-V ELF identity, per-file provenance and global checksum verification.

`pico8pocket-distribution` contains the versioned core ZIP, plugin and manifest,
`OFFLINE-COLLECTIONS.md`, project/dependency licenses, source archive,
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
renewed hardware compatibility checks. The new offline path additionally needs
slot-20 bundle loading, Arcade/Sorcerer handoffs and return navigation tested on
Pocket. Real Extism tests do not establish interactive Pocket Sync UI behavior.

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

## Replacing Cart Thumbnails

Keep `https://github.com/zenibako/pico8pocket` as the updater plugin URL. The
root `plugin.json` is canonical; the new display name is **Pico8Pocket Offline
Collections**. The latest complete release still exposes `plugin.wasm` and
`plugin.json`, so existing installations follow their normal update path.
Thumbnail menus, native thumbnail bundles and the thumbnail WASI CLI are
retired. Go/TinyGo now build the offline collection installer. Previously
generated artwork on users' cards is preserved.

`make offline-plugin-tools` writes exactly the plugin pair, `OFFLINE-COLLECTIONS.md`,
`LICENSE.txt`, `TOOL-NOTICES.txt` and intermediate `SHA256SUMS` in
`build/offline-plugin/`. Complete notices for the pinned Go/TinyGo toolchain and
Extism PDK are retained in `tools/plugin-licenses/`; missing notices fail the build. `scripts/release.py assemble`
rejects retired assets, extra downloads/cache files, mismatched manifests and
partial output. The complete distribution retains the core ZIP, source archive,
`CORE-PROVENANCE.json` and one checksum manifest covering every shipped file.
The optional native cache importer is built from source; no native thumbnail
archive or downloaded cart, cache, PICO-8 binary or `bbs.p8pack` is published.

Run `make offline-plugin-test` for native checks. After building, run
`REQUIRE_PLUGIN_TESTS=1 POCKET_PLUGIN_WASM="$PWD/build/offline-plugin/plugin.wasm" go test ./integration -count=1`
for actual WASI checks. The fixture harness exercises complete collection
installation, denied HTTP preservation, hard cancellation and courtesy pacing
with local HTTP mocks. It has no live-network mode. Keep
real-game and physical-device verification separate and record unperformed checks.
