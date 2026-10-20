# Offline BBS collections and cartridge handoffs

The user approved a Pocket Sync downloader and core-side offline loader, initially
Pico Arcade's 35 referenced games and Trail of the Sorcerer's companion. This
implements the two cooperating pieces discussed in this task. The user supplied
PICO-8 0.2.7a6 for private reference checks; it is never redistributed.

## Existing decisions and measured constraints

- `docs/compatibility.md` excludes networking on Pocket. Downloads run on desktop.
- `docs/architecture.md#filesystem-constraint` plans an index because APF cannot
  enumerate arbitrary SD directories. The pinned SDK also cannot resolve arbitrary
  paths through fopen; use an optional bound bundle instead of new OS APIs.
- `scripts/validate-pocket.sh` preserves dense entries 0..8 and save entries 9..18.
  Append optional slot ID 20 after all existing slots. Do not change fillers.
- `compatibility/incompatible-carts.json` owns confirmed incompatibilities; update
  the two launcher entries to require collection capabilities, retaining Backrooms.
- Official Pocket Plugin ABI: openfpga-library/pocket-plugin commit
  39dae308f7db00d77a4314fc76ad5d8d5b09b81b, Rust WASI/Extism. Pocket Sync mounts
  only pocket/ and host/, not arbitrary Mac paths, and cannot invoke PICO-8.
- Verified BBS download endpoint: https://www.lexaloffle.com/bbs/get_cart.php
  with cat=7, play_src=2, lid=<numeric or named BBS reference>. Check named IDs
  empirically before claiming their support.

## Shared collection and bundle

`compatibility/offline-carts.json` contains version 1 and a carts array with id,
name and collection (arcade/sorcerer). It records 35 Arcade targets and one
Sorcerer target. No game code or images are committed. The installer embeds this
JSON; a checked generator emits `include/p8p/bbs_catalog.inc` for core preflight.

`cards/bbs.p8pack` is a single optional APF file, slot 20. Under desktop Pocket
root its full path is Assets/pico8/common/cards/bbs.p8pack. The public core ZIP
contains no placeholder pack, so updating the core cannot replace this user data.

Bundle v1 is little endian:

- 16-byte header: magic bytes P8PBBS1 followed by NUL (8 bytes), uint32 entry
  count, uint32 reserved=0.
- Each 64-byte entry: NUL-terminated ASCII id[48] without #, uint32 payload
  offset, uint32 payload size, uint32 standard CRC32, uint32 reserved=0.
- Contiguous payload bytes follow the table in table order. IDs are unique,
  lowercase letters/digits/underscore/hyphen, max 47 bytes; count <=256.
- Maximum file size is 64 MiB, maximum cart payload 2 MiB, all ranges within
  file and below LONG_MAX. Reject duplicates, bad terminators, reserved fields,
  overlap/truncation, CRC mismatch, unsupported versions and invalid cart data.
- Store original downloaded .p8.png bytes. The core decodes on demand and retains
  at most its active cart and a bounded breadcrumb stack, not the full bundle.

Collection capabilities are Arcade=1 and Sorcerer=2. A collection capability is
present only if every ID in that collection is indexed. Payload CRC/decoder errors
are still checked when opening a target. Blocked launcher fingerprints need their
corresponding capability; without it they explain which offline collection to
install. Backrooms remains unconditionally incompatible.

## Desktop plugin

The plugin offers Install/Update both collections, Arcade only, Sorcerer only,
installed/cache status and Exit. HTTP is restricted to Lexaloffle hosts. Each
response must be HTTP 200, bounded, a 160x205 cartridge PNG and structurally valid.
Use the documented get_cart endpoint and validated IDs; no HTML scraping needed.

Keep verified downloads in the plugin's host/ cache. Install individual target
carts for standalone picking and an indexed pack for in-game handoffs. Preserve
previous valid cached/installed targets when a download fails. Write temporary
files then rename, and publish a replacement bundle only when all selected
collection downloads validate. Merge existing valid pack entries so installing
one collection does not remove the other. Show failed IDs and actionable errors.

The ABI is start()/handle_response(), serde messages Choice/Text/Exit and
Answer/Kill. Unit messages serialize as strings ("Exit", "Kill"). Process work
incrementally within one Install call, print progress, wait 15 seconds between
HTTP requests and handle cancellation without corrupting the last
valid bundle. Distribution artifacts are plugin.wasm and plugin.json.

A native helper can import a user-specified PICO-8 cache and/or use their supplied
PICO-8 in a dedicated test profile. It stages the same validated assets for plugin
consumption; the WASI plugin cannot launch the app or scan the user's home.
No real SD installation, GitHub publication or global toolchain changes are implied.

## Runtime handoff and app ownership

The VM exposes a bounded pending request containing kind (load/back), id[64],
breadcrumb[64], params[256]. A load of a #id ends the current top-level, _init,
frame or menu callback before any subsequent cart statements. The host services
requests after returning out of Lua; it never closes an executing Lua state.
Preserve the original Lua compiler for existing non-BBS uses and loadstring.

Native Lua load already has a legacy Eris permanent ID. Number old built-ins
unchanged, then append wrappers for load/extcmd and update original-value tracking.
Do not add sorted globals before the original enumeration. extcmd("go_back")
queues a return request; other existing extcmd calls retain current behavior.

The app-owned multicart manager reads/decode the target first, flushes cartdata,
then loads it through a contextual runtime load entry. On success it replaces the
active decoded cart and refreshes content hash, Memory ID, system menu/controls,
input suppression, profiler/scheduler and audio state. Missing or corrupt targets
show an error and retain a usable Select menu. No request resumes the suspended
old cartridge after a failure. Restart uses the currently active cart/context.

If a nonempty breadcrumb was supplied, retain the prior decoded cart/context in
an eight-level stack. Returning restarts the parent (not a frozen VM snapshot).
The menu displays a dedicated return entry without consuming any of the five
cartridge menuitem slots. stat(6) returns parameters; stat(100) returns the current
breadcrumb label or nil. Core reads block with audio paused during transitions.

State format extends to version 2 for parameter strings and accepts legacy version
1 states with empty parameters. Existing fixed-state/audio layouts and permanent
IDs stay valid. Pending requests are never serialized. Breadcrumbs belong to the
current launch session and are not invented by restoring a state on a fresh
standalone launch. Normal saves retain the current session's return navigation.

## Verification and delivery

Use project-owned fixture carts for load from top-level/init/update/draw/menu,
noncontinuation, params, back, nested depth, missing/bad targets, restart and save
identity. Exercise real app-loop handoffs, menus and failure recovery using host
platform hooks. Verify legacy state import and current state round trips.

Build the actual WASI artifact and run it under an Extism/Pocket Plugin host
against a temporary fake Pocket. Test HTTP failures, malformed PNG, cache reuse,
partial updates, cancellation and atomic installation. Use real downloaded carts
privately for all 35 selections and Sorcerer handoff; compare selected semantics
with supplied PICO-8. Run full host suite, Pocket build and package validation.
Physical Pocket testing remains separately identified if no device is available.
