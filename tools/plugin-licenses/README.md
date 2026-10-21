# Go/TinyGo plugin notices

These complete license texts are retained from the existing Go/TinyGo toolchain:

- `go.txt`: Go standard library (Go 1.25).
- `tinygo.txt`: TinyGo 0.43.0 runtime and compiler.
- `compiler-rt.txt`: LLVM compiler-rt support linked by TinyGo.
- `extism-go-pdk.txt`: Extism Go PDK v1.1.3.

The shipped plugin imports only the Go standard library and Extism Go PDK.
The native test host uses the Go SDK and wazero; those test-only dependencies
are pinned in `go.mod` and `go.sum` and are not linked into `plugin.wasm`.
`scripts/build-tools.sh` includes all four notices in `TOOL-NOTICES.txt` and
fails if one is missing. No license collection requires network access.
