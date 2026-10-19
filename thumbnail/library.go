package thumbnail

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
)

func cartCRC(data []byte) string { return fmt.Sprintf("%08x", crc32.ChecksumIEEE(data)) }

func libraryImage(label image.Image) []byte {
	out := make([]byte, 8+128*128*4)
	binary.LittleEndian.PutUint32(out, 0x41504920)
	binary.LittleEndian.PutUint16(out[4:], 128)
	binary.LittleEndian.PutUint16(out[6:], 128)
	for y := 0; y < 128; y++ {
		for x := 0; x < 128; x++ {
			r, g, b, _ := label.At(127-y, x).RGBA()
			i := 8 + (y*128+x)*4
			out[i] = uint8(b >> 8)
			out[i+1] = uint8(g >> 8)
			out[i+2] = uint8(r >> 8)
			out[i+3] = 255
		}
	}
	return out
}
