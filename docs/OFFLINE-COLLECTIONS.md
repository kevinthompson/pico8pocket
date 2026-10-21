# Pico8Pocket offline collection plugin

`plugin.wasm` and the root `plugin.json` are the Pocket Sync / pupdate plugin artifacts.
The plugin installs the curated 35-game Pico Arcade collection and Trail of the
Sorcerer's companion from `compatibility/offline-carts.json`. No game assets or
PICO-8 executable are included. PICO-8 is not required for plugin downloads.
It also browses public BBS releases and downloads individually selected carts.

## Use

Pocket Sync's normal installer takes a GitHub repository URL and retrieves
`plugin.wasm` and `plugin.json` from its latest release. The existing repository URL is `https://github.com/zenibako/pico8pocket`.
The next complete release replaces its former Cart Thumbnails plugin with
**Pico8Pocket Offline Collections**, so existing users receive a normal update.
This change prepares that release; it does not publish one. Use the native helper below to install locally
before publication, or the real Extism host harness to test the plugin itself.
After a release is available, add its repository URL using your updater's Pocket
Plugin support and choose your Pocket SD root.

Choose **Collections**, then **Pico Arcade**, **Sorcerer**, or **Both collections**,
and select **Install** or **Refresh downloads**. The selection runs automatically and prints per-cart
progress, waiting at least 15 seconds between network requests. Cached reinstalls
skip network requests and their delay. Closing/cancelling the updater preserves
the previous collection bundle; some standalone carts may already have updated.
A failed selected download prevents bundle publication and names failed IDs.
HTTP 429 immediately stops the batch and reports Retry-After when supplied; wait
before choosing Install again. Successful downloads stay cached, so another
Install retries only missing or invalid cache entries. The plugin never rapidly
retries a rate limited response.

**Install** uses verified cached downloads when available, including offline.
**Refresh downloads** explicitly downloads fresh copies. Failed refreshes preserve old valid
cache entries. **Status** on the main menu reports complete and partial counts.

The plugin can access only updater-provided `pocket/` and `host/` mounts. It uses
`host/pico8-offline-cache/<id>.p8.png` for verified originals and writes:

- `pocket/Assets/pico8/common/cards/bbs/<id>.p8.png` for standalone selection.
- `pocket/Assets/pico8/common/cards/bbs.p8pack` for launcher handoffs (core slot 20).

Installing one collection merges the existing valid bundle, preserving other
entries. A malformed existing bundle is reported and never overwritten. Move an
unusable bundle aside manually before retrying. Same-directory temporary files
and rename preserve the last published file during a cancelled write. A cancelled
session can leave `.pocket-sync-tmp` files; the next write replaces these. Some
standalone carts may already be updated if cancellation occurs during publication;
the bundle becomes visible only after the complete selected set is ready.

## Browse public BBS carts

Choose **Browse BBS**, then **Recent**, **Popular**, or **Search**. Recent uses the BBS's
New Carts listing for PICO-8 releases; Popular uses its New & Popular filter.
Search opens a text field: enter a BBS search term and press Enter or Submit.
Blank input returns to Browse. Queries accept up to 120 characters, including
Unicode; control characters are rejected. This uses the BBS search rules, which
can match tags as well as titles. It searches the public PICO-8 releases catalog,
not just the current page. No account or credentials are required.

Each page shows cart titles, authors and numeric BBS IDs. Previous/Next moves
through the listing while preserving the search term. **New search** opens a
fresh input; **Back to Browse** returns to Recent/Popular/Search. Choosing a cart shows its BBS link and offers **Open BBS
page** and **Download to Pocket**. No account or credentials are required.

Downloads reuse verified originals in the existing host cache and atomically
install `Assets/pico8/common/cards/bbs/<id>.p8.png`. Select that file in the Pocket
asset browser. Individual downloads leave `bbs.p8pack` unchanged; use the managed
collection actions for Arcade/Sorcerer dependencies. Other multicart games may
need additional carts, and downloaded games still depend on core compatibility.

Listing requests and cart downloads share the 15-second network cooldown,
including across menu actions. Only the explicitly requested page/cart is fetched;
there is no background crawl or bulk-download action. Cached cart reinstalls do
not make network requests. HTTP 429 stops the action without automatic retries.
Errors preserve existing installed files and report a retryable menu.

The source is the BBS website's public `lister.php` metadata, with `cat=7`,
`sub=2`, `mode=carts`, `orderby=ts`, `popular=1` for Popular, and URL-encoded
`search` for a submitted query. The parser reads
bounded data literals without executing JavaScript. It rejects unexpected schema
or malformed data; a BBS format change may require updating the adapter.

## Build

Go 1.25.0, TinyGo 0.43.0 and Binaryen 133 are the pinned build tools. The
root `go.mod` and `go.sum` lock the Go dependencies, including Extism Go PDK
1.1.3. Build and test from the repository root:

```sh
make offline-plugin-tools
make offline-plugin-test
```

`make offline-plugin-tools` writes the distribution pair
`build/offline-plugin/plugin.wasm` + `build/offline-plugin/plugin.json`, this
guide, the project license and generated dependency notices. Ship the pair
together; generated binaries are not committed. Keep generated cartridges and
test SD folders inside ignored `build/` directories.

The pure Go `offline/` library validates carts and packs, while `pocketplugin/`
owns the conversation and atomic installation transaction. The TinyGo ABI is
`cmd/pocket-plugin/`; it persists conversation state in Extism variables across
calls. Native tools use the same installer and validators.

Pocket Sync [v7.0.2 host source](https://github.com/neil-morrison44/pocket-sync/blob/0565ed1a1ed7d8d2eba9c3216dfb485e9fdb944b/src-tauri/src/commands/plugins.rs)
sets no per-call timeout and exposes a cancellation handle. The courtesy wait
uses short 100ms WASI polls so host cancellation can interrupt it promptly.

The ABI and mount contract follow the official [Pocket Plugin template at
39dae308](https://github.com/openfpga-library/pocket-plugin/tree/39dae308f7db00d77a4314fc76ad5d8d5b09b81b).
The exports are `start` and `handle_response`. Serde unit variants are the JSON
strings `"Exit"` and `"Kill"`; the template README's object example for Kill is
inconsistent with its actual enum. HTTP is restricted to `www.lexaloffle.com`.
Responses require status 200, at most 2 MiB, full PNG chunk and pixel validation,
and nonanimated noninterlaced 160x205 RGBA8. The Extism host buffers network
responses before plugin validation; hosts should also set an HTTP response limit.

## Optional explicit cache importer

The native companion imports an explicitly supplied PICO-8 BBS cache directory
containing `<id>.p8.png` files. The WASI plugin cannot launch PICO-8 or inspect your
Mac home. Point the importer at the updater's plugin host directory:

```sh
go run ./cmd/pocket-sync-cache import /explicit/cache/directory /explicit/plugin/host both
```

All selected input cartridges validate before cache writes start. Existing valid
cache files survive malformed inputs. For a test SD folder, the same native
transaction engine can assemble the pack with no network:

```sh
go run ./cmd/pocket-sync-cache install /explicit/plugin/host /explicit/fake-pocket both
go run ./cmd/pocket-sync-cache verify /explicit/fake-pocket/Assets/pico8/common/cards/bbs.p8pack
```

## Real WASI host check

`integration/` uses the actual Extism engine with the official demo host's WASI
mounts, `print_msg`/`open_url` signatures and JSON enum protocol. It verifies
complete cache-only installation, mount ordering, cancellation, collection
merging, rejected HTTP and the 15-second courtesy delay with local HTTP mocks.
It creates project-owned PNG fixtures and isolated temporary SD folders. It
does not download games or touch a real SD card.

Build the plugin first, then require the actual WASI checks:

```sh
make offline-plugin-tools
REQUIRE_PLUGIN_TESTS=1 POCKET_PLUGIN_WASM="$PWD/build/offline-plugin/plugin.wasm" go test ./integration -count=1
```

Without `POCKET_PLUGIN_WASM`, the native test suite skips plugin execution unless
`REQUIRE_PLUGIN_TESTS=1` requires it. The harness has no live-network mode; local
mocks exercise network behavior without making BBS requests. Native tests also
use project-owned PNG fixtures. Physical game behavior and interactive Pocket
Sync UI testing remain separate checks.

## Migration from Cart Thumbnails

Keep the same repository URL in Pocket Sync. The replacement declares access to
`www.lexaloffle.com` for BBS downloads; choosing Install/Refresh downloads starts those
downloads. Updating the plugin itself does not download cartridges.
The release replaces the old plugin
menu and removes the thumbnail CLI/WASI downloads. Native Pocket asset browsing
does not consume the generated per-cart artwork, so thumbnail generation is retired.
The installer writes only its collection cache, standalone carts and offline bundle;
it does not remove existing `System/Library/Images/pico8/` artwork, carts or saves.
Previously generated artwork may be removed manually if no longer wanted.

The optional native cache importer remains available from source using the commands
above. Prebuilt native thumbnail archives are no longer part of the distribution.
