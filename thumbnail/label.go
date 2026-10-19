package thumbnail

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"strings"
)

var palette = [32]color.NRGBA{
	{0, 0, 0, 255}, {29, 43, 83, 255}, {126, 37, 83, 255}, {0, 135, 81, 255},
	{171, 82, 54, 255}, {95, 87, 79, 255}, {194, 195, 199, 255}, {255, 241, 232, 255},
	{255, 0, 77, 255}, {255, 163, 0, 255}, {255, 236, 39, 255}, {0, 228, 54, 255},
	{41, 173, 255, 255}, {131, 118, 156, 255}, {255, 119, 168, 255}, {255, 204, 170, 255},
	{41, 24, 20, 255}, {17, 29, 53, 255}, {66, 33, 54, 255}, {18, 83, 89, 255},
	{116, 47, 41, 255}, {73, 51, 59, 255}, {162, 136, 121, 255}, {243, 239, 125, 255},
	{190, 18, 80, 255}, {255, 108, 36, 255}, {168, 231, 46, 255}, {0, 181, 67, 255},
	{6, 90, 181, 255}, {117, 70, 101, 255}, {255, 110, 89, 255}, {255, 157, 129, 255},
}

func pngLabel(data []byte) (image.Image, error) {
	cfg, err := png.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	if cfg.Width != 160 || cfg.Height != 205 {
		return nil, fmt.Errorf("expected a 160x205 cartridge PNG, got %dx%d", cfg.Width, cfg.Height)
	}
	im, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	label := image.NewNRGBA(image.Rect(0, 0, 128, 128))
	for y := 0; y < 128; y++ {
		for x := 0; x < 128; x++ {
			pixel := color.NRGBAModel.Convert(im.At(x+16, y+24)).(color.NRGBA)
			pixel.A = 255
			label.SetNRGBA(x, y, pixel)
		}
	}
	return label, nil
}

func textLabel(data []byte) (image.Image, error) {
	lines := strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n")
	start := -1
	for i, line := range lines {
		if line == "__label__" {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return nil, nil
	}
	if len(lines)-start < 128 {
		return nil, fmt.Errorf("label requires 128 rows")
	}
	im := image.NewNRGBA(image.Rect(0, 0, 128, 128))
	for y := 0; y < 128; y++ {
		line := lines[start+y]
		if len(line) != 128 {
			return nil, fmt.Errorf("label row %d requires 128 pixels", y+1)
		}
		for x := 0; x < 128; x++ {
			idx := strings.IndexByte("0123456789abcdefghijklmnopqrstuv", line[x])
			if idx < 0 {
				return nil, fmt.Errorf("invalid label pixel in row %d", y+1)
			}
			im.SetNRGBA(x, y, palette[idx])
		}
	}
	if start+128 < len(lines) && lines[start+128] != "" && !strings.HasPrefix(lines[start+128], "__") {
		return nil, fmt.Errorf("label has more than 128 rows")
	}
	return im, nil
}
