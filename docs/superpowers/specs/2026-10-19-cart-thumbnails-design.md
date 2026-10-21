> Historical: thumbnail tooling was retired on 2026-10-20 in favor of
> [Offline Collections](../../OFFLINE-COLLECTIONS.md).

# Pocket Sync cart thumbnails

Approved in chat on 2026-10-19: use the Butterscotch architecture (Go shared logic,
native CLI, TinyGo/Extism plugin) for Analogue's native picker, not a new in-core
browser. This extends the host-side preparation described in
[architecture.md](../../architecture.md#filesystem-constraint); it does not
implement that document's future cards.idx browser.

Read carts recursively from Assets/pico8/common/cards on the connected card.
Extract the 128x128 label from a standard 160x205 .p8.png (offset 16,24), or
from the optional __label__ section of a .p8 text cart. Write Analogue Library
images under System/Library/Images/pico8, named by the CRC32 of the complete
original cart file. Use the documented eight-byte header and rotated opaque
BGRA pixels. No network, cart rewriting, save changes, image deletion or
replacement of differing existing artwork. Skip absent labels and unsupported
files; report malformed carts individually. Reject symlink paths in card I/O.

Pocket Sync offers one-cart pilot generation, all-cart generation, brief help,
and exit. CLI uses the same library. Menu completion explicitly says picker
support still needs hardware confirmation, since carts load through slot 4
and slot 0 is a fixed instance. Missing labels are skipped, not synthesized.

Verification: synthetic pixel/corner fixtures for crop and rotation; text
labels including extended colors and malformed rows; known CRC32 vectors;
original/save sentinels; recursive scan, idempotence, existing-art preservation,
symlink containment; session replay rejection; real compiled Extism host test
with both mount orders. Produce local native/WASI/plugin artifacts and a
one-cart test guide. Do not claim native-picker support until a physical test.

Sources: Analogue Library https://www.analogue.co/developer/docs/openfpga/library
and PICO-8 manual https://www.lexaloffle.com/dl/docs/pico-8_manual.html.
Existing Butterscotch code is AGPL; implement the MIT tooling independently
using the same public protocol and SDK, without copying its implementation.
