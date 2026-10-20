# Offline BBS implementation plan

> Go port note (2026-10-20): The implemented installer retains the repository's
> Go/TinyGo architecture. `offline/` owns validation/cache/bundle operations,
> `pocketplugin/` owns the session and transaction, and `cmd/pocket-plugin/`
> exports the WASI ABI. The native helper is `cmd/pocket-sync-cache/`; real
> Extism tests are in `integration/`. Build with `make offline-plugin-tools`
> and test with `make offline-plugin-test`. Rust paths/tool commands below
> describe the superseded implementation; the collection and bundle contract
> remains unchanged. Current operations are in `docs/OFFLINE-COLLECTIONS.md`.

**Goal:** Install the requested BBS games with a Pocket Sync plugin and execute
launcher handoffs offline in Pico8Pocket.

**Spec:** docs/superpowers/specs/2026-10-20-offline-bbs-design.md

**Architecture:** A shared curated JSON drives plugin downloads and core bundle
preflight. A checked binary bundle reader supplies decoded carts. The VM queues
handoffs; an app manager performs them outside Lua and refreshes app ownership.

**Constraints:** No game assets/PICO-8 binaries in commits, no OS/SDK pin changes,
no movement of existing data slots, no deployment or publication. Preserve old
save IDs and version-1 state readability. Use signed conventional commits.

### 1. Shared collection and bundle contract

- [x] Add compatibility/offline-carts.json with the 35 exact Arcade # references
  and mot_sorcerergame, and checked C catalog generation.
- [x] Add unsigned collection bits to cart.h, capabilities to registry entries,
  regenerate README and compiled compatibility table.
- [x] Implement include/p8p/bbs.h and src/runtime/bbs.c:
  `p8p_bbs_pack_open(path,error,size)`, `p8p_bbs_pack_close(pack)`,
  `p8p_bbs_pack_features(pack)`,
  `p8p_bbs_pack_load(pack,id,out_cart,error,size)`.
- [x] Add project-owned bundle tests before implementation: malformed header,
  duplicate/range/CRC failures, PNG/text decode, partial/full collection bits.
  Run the C test executable and generator --check tests.

### 2. Runtime request protocol (independent implementer)

- [x] Add context-load and request API to runtime.h/runtime.cpp. Public names:
  `p8p_runtime_load_context(runtime,cart,params,breadcrumb)`,
  `p8p_runtime_set_cart_features(runtime,features)`,
  `p8p_runtime_cart_request(runtime)`, `p8p_runtime_clear_cart_request(runtime)`,
  `p8p_runtime_breadcrumb(runtime)`, `p8p_runtime_request_back(runtime)`.
- [x] Write tests/multicart_runtime_test.cpp first and prove old runtime fails
  noncontinuation/params requests. Cover every protected/resume boundary.
- [x] Preserve native compiler/permanent numbering; append wrappers. Add
  stat(6)/stat(100), contextual restart and version-2 params state with v1 reader.
- [x] Run focused tests and the existing runtime suite; inspect old/new state
  compatibility with a privately compiled HEAD baseline.

### 3. Plugin (independent implementer)

- [x] Add tools/pocket-sync/ Rust cdylib, pinned dependencies/lock, plugin.json
  and build helper; use exact official host-message JSON contract.
- [x] Test pure Rust catalog validation, PNG validation, deterministic bundle
  encode/decode/merge and atomic publication before production routines.
- [x] Add HTTP/caching incremental installer and collection menu. Read shared
  JSON. Stage output in fake Pocket for tests. Produce plugin.wasm/plugin.json.
- [x] Set up a workspace-local WASI toolchain if the installed compiler lacks
  the target; never overwrite system Rust or user profiles.
- [x] Run actual Extism host tests on artifacts, plus live curated fetches into
  ignored private cache. Add a native cache/reference helper when needed.

### 4. App integration (coordinator)

- [x] Add include/p8p/multicart.h and src/app/multicart.c manager, owning a pack
  reader and an eight-level parent stack, borrowing active decoded cart.
- [x] Integrate pending requests, contextual restart, hash/Memory/menu refresh
  and input/audio/scheduler reset in main.c. Add a dedicated return menu row.
- [x] Append optional slot20 cards/bbs.p8pack after slot19 in descriptors;
  update validation preserving the entire existing slot prefix. Omit pack from
  releases; support host path override only for test/desktop execution.
- [x] Extend scanner to use the same manager and expose deterministic routes.
- [x] Add project-owned real app-loop transition/back/missing-target tests,
  then execute the private Arcade35/Sorcerer routes.

### 5. Verification and delivery

- [x] Run `make test`, plugin native+WASI host tests, `make pocket-elf
  USE_SDK_CONTAINER=0`, and affected package/variant validation.
- [x] Review the integrated diff using a separate reviewer; address findings.
- [x] Update README, architecture, compatibility and changelog with exact
  install/update/cache-import instructions and empirical limitations.
- [x] Make GPG-signed commits, verify signatures and clean working tree.
  Report artifact paths, test results and any remaining hardware checks.
