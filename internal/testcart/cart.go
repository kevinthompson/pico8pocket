// Package testcart builds project-owned synthetic cartridges for host tests.
package testcart

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"strings"
)

// Text returns a text cartridge with a red label and no executable gameplay.
func Text() []byte {
	return []byte("pico-8 cartridge // http://www.pico-8.com\nversion 42\n__lua__\n-- thumbnail fixture\n__label__\n" + strings.Repeat(strings.Repeat("8", 128)+"\n", 128))
}

// PNG returns a distinct 160x205 RGBA8 PNG containing project-owned noise.
// These transaction fixtures contain no games and establish no compatibility.
// A local fixed generator keeps their original bytes stable across Go versions.
func PNG(index byte) []byte {
	var png bytes.Buffer
	png.WriteString("\x89PNG\r\n\x1a\n")
	chunk := func(kind string, data []byte) {
		_ = binary.Write(&png, binary.BigEndian, uint32(len(data)))
		png.WriteString(kind)
		png.Write(data)
		crc := crc32.NewIEEE()
		_, _ = crc.Write([]byte(kind))
		_, _ = crc.Write(data)
		_ = binary.Write(&png, binary.BigEndian, crc.Sum32())
	}
	header := make([]byte, 13)
	binary.BigEndian.PutUint32(header[:4], 160)
	binary.BigEndian.PutUint32(header[4:8], 205)
	header[8], header[9] = 8, 6
	chunk("IHDR", header)
	chunk("tEXt", []byte(fmt.Sprintf("Description\x00PROJECT-OWNED FIXTURE %d; no game content", index)))
	var raw bytes.Buffer
	state := uint32(index) + 1
	for y := 0; y < 205; y++ {
		raw.WriteByte(0)
		for x := 0; x < 160; x++ {
			for channel := 0; channel < 3; channel++ {
				state ^= state << 13
				state ^= state >> 17
				state ^= state << 5
				raw.WriteByte(byte(state))
			}
			raw.WriteByte(255)
		}
	}
	var compressed bytes.Buffer
	writer, _ := zlib.NewWriterLevel(&compressed, zlib.NoCompression)
	_, _ = writer.Write(raw.Bytes())
	_ = writer.Close()
	chunk("IDAT", compressed.Bytes())
	chunk("IEND", nil)
	return png.Bytes()
}
