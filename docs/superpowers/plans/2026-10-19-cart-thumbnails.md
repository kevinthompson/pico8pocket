> Historical: thumbnail tooling was retired on 2026-10-20 in favor of
> [Offline Collections](../../OFFLINE-COLLECTIONS.md).

# Cart thumbnail implementation plan

> For agentic workers: use subagent-driven-development with scoped file ownership.

Goal: shared Go generator, native/WASI CLI and Pocket Sync plugin for experimental
Analogue Library artwork; keep the existing C runner unchanged.

Architecture: approved design at ../specs/2026-10-19-cart-thumbnails-design.md.
Go module github.com/zenibako/pico8pocket at the repository root.

## Task 1: shared generator
Create thumbnail/ label.go, library.go, generate.go and behavior tests. API:
Generate(root string, limit int) (Report,error), limit 0 means all, 1 means pilot.
Report fields Generated, Unchanged, Skipped int; Warnings []string; Paths []string.
Paths are generated/unchanged paths relative to the card root. No-label and
malformed inputs are per-cart skips; missing source root is an error. Deterministic
recursive ordering. Preserve differing existing artwork and all carts/saves.
Write failing tests, implement, run tests. No C dependency setup needed.

## Task 2: plugin session
Create pocketplugin/session.go and tests; cmd/pocket-plugin/main_wasip1.go;
plugin.json. Independently implement the public Extism/Pocket Sync Choice/Answer/
Kill protocol. New(root,logger), Start() and Handle([]byte) methods returning
([]byte,error). Strict JSON including unknown/duplicate keys, unique conversation
names, exact offered choices; reject replay and calls after Exit. Menu: pilot,
all, short help, Exit. Run Generate synchronously only on selected generation.
Short completion count and unverified-picker reminder, no long logs.

## Task 3: CLI, build, integration and handoff
Create cmd/cart-thumbnails CLI with --root and --first, scripts/build-tools.sh,
Makefile opt-in tools targets, native/WASI/plugin artifacts, integration test
for actual built plugin in both preopen orders. Tests must invoke the generated
plugin, verify a real image and unchanged cart/save bytes. Add CI for tools only;
do not change release/runner behavior. Document short install and pilot steps,
artwork path, incomplete picker verification, and preservation policy. Produce
checksums and a local package excluding purchased carts. Verify go test ./...,
go vet ./..., actual compiled host tests, standard WASI CLI, six native builds.
Run make test if dependencies allow; report existing mismatched runtime cache.

## Interface audit / rulings
| Tasks | Contract | Ruling |
|---|---|---|
| 1 / 2 | thumbnail.Generate(root,limit), Report | Fixed API above; root supplied by host, never an answer |
| 1 / 3 | CLI and plugin share generator | No duplicate conversion implementation |
| 2 / 3 | start, handle_response exports; /pocket | Test both preopen orders |
| 1 | Existing artwork / repeat generation | Equal bytes count unchanged; differing bytes skipped with warning |
| 2 | Alpha hardware behavior | Completion instructs hardware test; no claim of picker support |
| 3 | C baseline dependency mismatch | Preserve caches; independent Go checks proceed |

## Completion evidence

Implemented all three tasks on feat/pocket-sync-thumbnails from origin/main
77bcd69. Reviewed shared generator and plugin independently; corrected PNG
alpha-premultiplication and WASI parent-path behavior exposed by real host tests.
Final standard-Go WASI uses an absolute /pocket preopen; TinyGo host tests cover
both relative preopen orders used by Pocket Sync.

Verified: go test ./... with REQUIRE_PLUGIN_TESTS and both built WASM paths,
go vet ./..., race tests for generator/session/CLI, all six native builds,
actual Apple silicon bundled CLI, all 12 artifact hashes. Existing C host tests
passed with make -o deps test; ordinary make test remains blocked by the older
cached openfpgaCore dependency and no dependency revision was changed.

Local outputs: build/pico8pocket-thumbnail-tools.zip and
build/pico8pocket-thumbnail-pilot.zip. The pilot is project-owned, contains no
user carts, and does not install a runner. Physical slot-4 picker association
remains unverified. No release or listing submission is part of this commit.
