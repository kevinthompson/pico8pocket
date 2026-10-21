package offline

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image/png"
	"io"
)

// ValidatePNG checks the complete file, including chunk CRCs, the zlib checksum
// and exact decoded scanline length. Only the cartridge RGBA8 format is accepted.
func ValidatePNG(data []byte) error {
	if len(data) > MaxCart || !bytes.HasPrefix(data, []byte("\x89PNG\r\n\x1a\n")) {
		return fmt.Errorf("expected a cartridge PNG (maximum 2 MiB)")
	}
	var compressed bytes.Buffer
	seenHeader, seenData, dataEnded, ended := false, false, false, false
	for pos := 8; pos < len(data); {
		if len(data)-pos < 12 {
			return fmt.Errorf("truncated PNG chunk")
		}
		n := uint64(binary.BigEndian.Uint32(data[pos:]))
		end64 := uint64(pos) + n + 12
		if end64 > uint64(len(data)) {
			return fmt.Errorf("truncated PNG data")
		}
		end := int(end64)
		kind := data[pos+4 : pos+8]
		payload := data[pos+8 : end-4]
		for _, c := range kind {
			if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z') {
				return fmt.Errorf("invalid PNG chunk type")
			}
		}
		if kind[2]&32 != 0 {
			return fmt.Errorf("invalid PNG reserved chunk bit")
		}
		if crc32.ChecksumIEEE(data[pos+4:end-4]) != binary.BigEndian.Uint32(data[end-4:]) {
			return fmt.Errorf("PNG chunk CRC mismatch")
		}
		if !seenHeader && string(kind) != "IHDR" {
			return fmt.Errorf("missing PNG header")
		}
		if seenData && string(kind) != "IDAT" {
			dataEnded = true
		}
		switch string(kind) {
		case "IHDR":
			if seenHeader || n != 13 {
				return fmt.Errorf("invalid PNG header")
			}
			seenHeader = true
			if binary.BigEndian.Uint32(payload) != 160 || binary.BigEndian.Uint32(payload[4:]) != 205 || payload[8] != 8 || payload[9] != 6 || payload[10] != 0 || payload[11] != 0 || payload[12] != 0 {
				return fmt.Errorf("expected a nonanimated 160x205 RGBA8 PICO-8 cartridge PNG")
			}
		case "IDAT":
			if dataEnded {
				return fmt.Errorf("nonconsecutive PNG image data")
			}
			seenData = true
			compressed.Write(payload)
		case "IEND":
			if n != 0 || end != len(data) || !seenData {
				return fmt.Errorf("invalid PNG end")
			}
			ended = true
		case "acTL", "fcTL", "fdAT":
			return fmt.Errorf("animated PNG is not a cartridge")
		default:
			if kind[0]&32 == 0 && string(kind) != "PLTE" {
				return fmt.Errorf("unsupported critical PNG chunk")
			}
		}
		pos = end
	}
	if !ended {
		return fmt.Errorf("missing PNG end")
	}
	input := bytes.NewReader(compressed.Bytes())
	reader, err := zlib.NewReader(input)
	if err != nil {
		return fmt.Errorf("PNG zlib: %w", err)
	}
	const stride = 160*4 + 1
	raw, readErr := io.ReadAll(io.LimitReader(reader, 205*stride+1))
	closeErr := reader.Close()
	if readErr != nil {
		return fmt.Errorf("PNG pixels: %w", readErr)
	}
	if closeErr != nil {
		return closeErr
	}
	if len(raw) != 205*stride || input.Len() != 0 {
		return fmt.Errorf("invalid PNG image data length")
	}
	for row := 0; row < 205; row++ {
		if raw[row*stride] > 4 {
			return fmt.Errorf("invalid PNG scanline filter")
		}
	}
	// The standard decoder also enforces PNG ordering and optional chunk rules.
	if _, err := png.Decode(bytes.NewReader(data)); err != nil {
		return fmt.Errorf("PNG: %w", err)
	}
	return nil
}
