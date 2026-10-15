# Third-party notices

Pico8Pocket source code authored for this repository is licensed under the
MIT License in `LICENSE`. External projects, generated patches, toolchains and
binary components remain under their respective licenses.

The build fetches pinned revisions of:

- [Diablo for openfpgaOS](https://github.com/openfpgaOS/Diablo), used as the
  pinned source of the API 0.9.0 SDK sources, libraries and container
  tooling.
- [zenibako/openfpgaCore](https://github.com/zenibako/openfpgaCore), an
  openfpgaOS fork with native Pocket save states, used as the pinned source
  of the SDK API headers and of the Pocket `os20.rbf_r`, `os.bin` and
  `loader.bin` runtime files (release `pocket-runtime-a58aacd`).
- [openfpgaOS SDK](https://github.com/openfpgaOS/openfgpaSDK), whose
  SDK-authored components are provided under Apache-2.0. The upstream SDK
  notice documents additional components embedded in the runtime binaries;
  copies of the SDK license and notice are included in each release.
- [Fake-08](https://github.com/jtothebell/fake-08), used as the pinned source
  for z8lua, miniz and compatibility references. Its license notice is
  included in each release. Selected fixes from the newer
  [jtothebell/z8lua](https://github.com/jtothebell/z8lua) branch are applied as
  local patches. The `print()` text engine in `src/runtime/runtime.cpp` is
  ported from Fake-08, and `include/p8p/pico8_font.h` holds the PICO-8
  default font data from Fake-08 (which credits tac08). The P8SCII/UTF-8
  charmap in `src/runtime/cart.c` comes from zepto8 by Sam Hocevar via
  Fake-08, under the WTFPL.

The public source repository and public release archives contain no PICO-8
cartridges, ROMs, commercial game data or third-party sound banks. Users must
provide cartridges they are legally entitled to use.

PICO-8 is a product of Lexaloffle Games LLP. Analogue Pocket and openFPGA are
products and technologies of Analogue. Pico8Pocket is an independent,
unofficial compatibility project and is not affiliated with, endorsed by, or
sponsored by Lexaloffle Games LLP or Analogue.
