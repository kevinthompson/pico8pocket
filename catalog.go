package pico8pocket

import _ "embed"

// OfflineCatalog is the shared curated cartridge index. It contains no game assets.
//
//go:embed compatibility/offline-carts.json
var OfflineCatalog []byte
