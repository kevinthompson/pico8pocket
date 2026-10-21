package offline

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"sort"
)

func EncodePack(carts Carts) ([]byte, error) {
	if len(carts) == 0 || len(carts) > 256 {
		return nil, fmt.Errorf("bundle needs 1..256 cartridges")
	}
	size := 16 + 64*len(carts)
	ids := make([]string, 0, len(carts))
	for id, data := range carts {
		if err := ValidateID(id); err != nil {
			return nil, err
		}
		if err := ValidatePNG(data); err != nil {
			return nil, fmt.Errorf("%s: %w", id, err)
		}
		if len(data) > MaxPack-size {
			return nil, fmt.Errorf("bundle exceeds 64 MiB")
		}
		size += len(data)
		ids = append(ids, id)
	}
	sort.Strings(ids)
	out := make([]byte, 16+64*len(carts), size)
	copy(out, "P8PBBS1\x00")
	binary.LittleEndian.PutUint32(out[8:], uint32(len(carts)))
	for i, id := range ids {
		p := 16 + 64*i
		data := carts[id]
		copy(out[p:p+48], id)
		binary.LittleEndian.PutUint32(out[p+48:], uint32(len(out)))
		binary.LittleEndian.PutUint32(out[p+52:], uint32(len(data)))
		binary.LittleEndian.PutUint32(out[p+56:], crc32.ChecksumIEEE(data))
		out = append(out, data...)
	}
	return out, nil
}
func DecodePack(data []byte) (Carts, error) {
	if len(data) < 16 || len(data) > MaxPack || !bytes.Equal(data[:8], []byte("P8PBBS1\x00")) || binary.LittleEndian.Uint32(data[12:]) != 0 {
		return nil, fmt.Errorf("invalid bundle header")
	}
	// Bound the wire value before converting: TinyGo's wasm32 int is 32 bits.
	wireCount := binary.LittleEndian.Uint32(data[8:])
	if wireCount > 256 {
		return nil, fmt.Errorf("invalid bundle table")
	}
	count := int(wireCount)
	if 16+64*count > len(data) {
		return nil, fmt.Errorf("invalid bundle table")
	}
	expected := 16 + 64*count
	carts := make(Carts, count)
	for i := 0; i < count; i++ {
		p := 16 + 64*i
		raw := data[p : p+48]
		term := bytes.IndexByte(raw, 0)
		if term < 0 {
			return nil, fmt.Errorf("unterminated bundle ID")
		}
		for _, b := range raw[term:] {
			if b != 0 {
				return nil, fmt.Errorf("nonzero bundle ID padding")
			}
		}
		if binary.LittleEndian.Uint32(data[p+60:]) != 0 {
			return nil, fmt.Errorf("nonzero bundle reserved bytes")
		}
		id := string(raw[:term])
		if err := ValidateID(id); err != nil {
			return nil, err
		}
		offset := uint64(binary.LittleEndian.Uint32(data[p+48:]))
		size := uint64(binary.LittleEndian.Uint32(data[p+52:]))
		if offset != uint64(expected) || size == 0 || size > MaxCart || offset+size > uint64(len(data)) {
			return nil, fmt.Errorf("invalid bundle payload range")
		}
		payload := data[int(offset):int(offset+size)]
		if crc32.ChecksumIEEE(payload) != binary.LittleEndian.Uint32(data[p+56:]) {
			return nil, fmt.Errorf("bundle CRC mismatch: %s", id)
		}
		if err := ValidatePNG(payload); err != nil {
			return nil, fmt.Errorf("%s: %w", id, err)
		}
		if _, ok := carts[id]; ok {
			return nil, fmt.Errorf("duplicate bundle ID: %s", id)
		}
		carts[id] = append([]byte(nil), payload...)
		expected += int(size)
	}
	if expected != len(data) {
		return nil, fmt.Errorf("trailing bundle data")
	}
	return carts, nil
}
