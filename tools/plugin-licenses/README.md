# Supplemental plugin dependency licenses

The published crates `extism-convert`, `extism-convert-macros`, and
`extism-manifest` version 1.41.0 declare BSD-3-Clause but omit the repository
license file from their crate archives. `extism-1.41.0.txt` is the complete,
unmodified repository license from their recorded source revision:

- Repository: https://github.com/extism/extism
- Revision: `051b765bb6abe4fe95d5e4f489f03ad7b137edbc`
- Source: https://raw.githubusercontent.com/extism/extism/051b765bb6abe4fe95d5e4f489f03ad7b137edbc/LICENSE
- SHA-256: `d01e60805075b591cbecc63b8582788e54e190f699037f1af19fb5294b7a47ab`

Each published crate records this revision in `.cargo_vcs_info.json` (with
`dirty: true`). The notice collector accepts this supplement only for these
three exact crate versions, the matching repository, BSD-3-Clause declaration,
and recorded source revision. A new version or changed provenance requires
reviewing and pinning its actual license text; no generic SPDX substitution is
used. The release collector reads this local file and makes no network request.
