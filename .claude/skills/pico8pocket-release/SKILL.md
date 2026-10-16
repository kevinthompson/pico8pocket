---
name: pico8pocket-release
description: Build, package and hand Pico8Pocket test builds to the user for Analogue Pocket testing, including side-by-side A/B cores, the SD-card layout, the platform banner and the PR/CodeRabbit review flow. Use whenever producing a zip for the user, changing packaging or core metadata, or opening and merging PRs in this repo.
argument-hint: "[card: SD mount path, e.g. /Volumes/Pocket]"
---

# Pico8Pocket builds, device testing and PRs

## Building a test zip

- Run `make test`, then `make package USE_SDK_CONTAINER=0`. The second writes
  `releases/pico8pocket-v<VERSION>.zip` and must print "APF package
  validation passed: zenibako.pico8 on pico8".
- Run `rm -rf .obj/pico8pocket` first if `include/p8p/vm_fasttext.h` changed.
- Copy the zip to the repo root under a descriptive name, for example
  `pico8-zenibako-v0.0.33-<topic>.zip` (`*.zip` is gitignored), and send it
  with SendUserFile. The user tests every change on a real Pocket.
- Resolve dependency revisions and runtime hashes from `scripts/fetch-deps.sh`.
  The build stages Diablo's SDK sources/tools with the API headers and Pocket
  runtime from `zenibako/openfpgaCore` (currently `pocket-runtime-a58aacd`)
  into `.deps/openfpgaos-sdk`, the default `SDK_ROOT`.
- Treat `os20.rbf_r`, `os.bin` and `loader.bin` as one pinned release set.
  The FPGA fit's BRAM bootloader and OS `.fasttext` are re-patched during the
  firmware build; `os.bin` requires the bitstream patched from that same build.
  Mixing runtime versions can black-screen even when the application ELF is
  unchanged. Verify all three packaged files against the fetch-script hashes.
- Public zips never contain carts. `make package-local` bundles local carts
  and is for private use only.

## Match simulator evidence to the device build

- The pinned shipped platform is `os20`: dual-issue RV32 at 90 MHz, with
  32 KiB I-cache and 64 KiB D-cache (`docs/architecture.md`). Performance
  acceptance uses the separate os20 adapter in `openfpga-os25-harness`, with
  explicit `--platform os20` and a verified final Lua seed; follow
  `../pico8pocket-performance/SKILL.md`. The harness still defaults to os25.
- Record source revision, ELF SHA-256, compiler version/flags, libc, alignment
  and BRAM size for the packaged hardware build. Compare the bitstream, OS and
  loader hashes with the pins in `scripts/fetch-deps.sh`; a core name or version
  alone does not identify the installed binary.
- The os20 simulator uses frozen OS `8318f25` and its own runtime/checkpoint
  build. The current fork retains that CPU/cache/SDRAM configuration but adds
  save-state gateware and firmware. Source-profile agreement does not establish
  fitted-bitstream or device timing parity; presentation, adaptive skipping,
  OS/SD and live audio scheduling are omitted. Recheck the production source
  profile whenever the runtime pin changes. Report simulated results separately
  from device FPS; require the same scene's device A/B evidence for a hardware
  performance claim.
  The Butterscotch os25 calibration cannot establish Pico8Pocket os20 parity.

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
- To install both from a Mac, set `$CARD` to the card's mount path from the
  **card** argument, asking the user if it wasn't given. The files the two
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
