// Package thumbnail independently implements PICO-8 label extraction and the
// public Analogue Library image format. It never changes source cartridges.
package thumbnail

import (
	"bytes"
	"fmt"
	"image"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

const maxCartSize = 8 * 1024 * 1024

// Report summarizes generation. Paths are relative to the card root and include
// images generated or found byte-identical during this call.
type Report struct {
	Generated int
	Unchanged int
	Skipped   int
	Warnings  []string
	Paths     []string
}

// Generate recursively scans the card's conventional cart directory. A zero
// limit processes all carts; a positive limit counts valid label attempts,
// including existing images. Missing labels do not consume the pilot limit.
func Generate(root string, limit int) (Report, error) {
	var report Report
	if limit < 0 {
		return report, fmt.Errorf("limit must be nonnegative")
	}
	root, err := filepath.Abs(root)
	if err != nil {
		return report, err
	}
	source := filepath.Join(root, "Assets", "pico8", "common", "cards")
	output := filepath.Join(root, "System", "Library", "Images", "pico8")
	if err = checkDirectories(source, false); err != nil {
		return report, err
	}
	if err = checkDirectories(output, true); err != nil {
		return report, err
	}
	var carts []string
	err = scan(source, &carts)
	if err != nil {
		return report, err
	}
	sort.Strings(carts)
	attempts := 0
	for _, path := range carts {
		if limit > 0 && attempts >= limit {
			break
		}
		rel, _ := filepath.Rel(root, path)
		warn := func(e error) {
			report.Skipped++
			report.Warnings = append(report.Warnings, fmt.Sprintf("%s: %v", filepath.ToSlash(rel), e))
		}
		if err = checkDirectories(filepath.Dir(path), false); err != nil {
			warn(err)
			continue
		}
		info, e := os.Lstat(path)
		if e != nil {
			warn(e)
			continue
		}
		if !info.Mode().IsRegular() {
			warn(fmt.Errorf("not a regular cartridge"))
			continue
		}
		if info.Size() > maxCartSize {
			warn(fmt.Errorf("cartridge exceeds 8 MiB"))
			continue
		}
		f, e := os.Open(path)
		if e != nil {
			warn(e)
			continue
		}
		data, e := io.ReadAll(io.LimitReader(f, maxCartSize+1))
		closeErr := f.Close()
		if e == nil {
			e = closeErr
		}
		if e != nil {
			warn(e)
			continue
		}
		if len(data) > maxCartSize {
			warn(fmt.Errorf("cartridge exceeds 8 MiB"))
			continue
		}
		var label image.Image
		if strings.HasSuffix(strings.ToLower(path), ".p8.png") {
			label, e = pngLabel(data)
		} else {
			label, e = textLabel(data)
		}
		if e != nil {
			warn(e)
			continue
		}
		if label == nil {
			report.Skipped++
			continue
		}
		attempts++
		dest := filepath.Join(output, cartCRC(data)+".bin")
		pixels := libraryImage(label)
		if e = checkDirectories(output, true); e != nil {
			return report, e
		}
		state, e := writeImage(dest, pixels)
		if e != nil {
			warn(e)
			continue
		}
		outRel, _ := filepath.Rel(root, dest)
		report.Paths = append(report.Paths, filepath.ToSlash(outRel))
		if state {
			report.Generated++
		} else {
			report.Unchanged++
		}
	}
	return report, nil
}

// checkDirectories checks all accessible ancestors (up to the preopen mount in
// WASI). Missing output directories are allowed; symbolic links are rejected.
func checkDirectories(path string, allowMissing bool) error {
	path = filepath.Clean(path)
	parent := filepath.Dir(path)
	// WASI exposes preopened mounts, not their virtual parent directory.
	// Check the mount itself and every descendant, without querying virtual /.
	if parent != path && !(runtime.GOOS == "wasip1" && parent == string(filepath.Separator)) {
		if err := checkDirectories(parent, allowMissing); err != nil {
			return err
		}
	}
	st, err := os.Lstat(path)
	if os.IsNotExist(err) && allowMissing {
		return nil
	}
	if err != nil {
		return err
	}
	if st.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("symlink directory rejected: %s", path)
	}
	if !st.IsDir() {
		return fmt.Errorf("not a directory: %s", path)
	}
	return nil
}

func scan(dir string, carts *[]string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.Type()&os.ModeSymlink != 0 {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		if entry.IsDir() {
			if err = scan(path, carts); err != nil {
				return err
			}
			continue
		}
		name := strings.ToLower(entry.Name())
		if strings.HasSuffix(name, ".p8") || strings.HasSuffix(name, ".p8.png") {
			*carts = append(*carts, path)
		}
	}
	return nil
}

func existingImage(path string, data []byte) (bool, error) {
	st, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if !st.Mode().IsRegular() {
		return false, fmt.Errorf("existing artwork is not a regular file: %s", path)
	}
	if st.Size() != int64(len(data)) {
		return false, fmt.Errorf("preserved differing existing artwork: %s", path)
	}
	old, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	if !bytes.Equal(old, data) {
		return false, fmt.Errorf("preserved differing existing artwork: %s", path)
	}
	return true, nil
}

// writeImage uses a same-directory temporary file and rename. The second
// existence check preserves artwork created during preparation. As with other
// portable path-based filesystem APIs, concurrently replacing directories or
// creating the destination between the final check and rename is unsupported.
func writeImage(path string, data []byte) (bool, error) {
	same, err := existingImage(path, data)
	if err != nil || same {
		return false, err
	}
	if err = os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return false, err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".thumbnail-*")
	if err != nil {
		return false, err
	}
	temp := f.Name()
	defer os.Remove(temp)
	_, err = f.Write(data)
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		return false, err
	}
	same, err = existingImage(path, data)
	if err != nil || same {
		return false, err
	}
	if err = os.Rename(temp, path); err != nil {
		return false, err
	}
	return true, nil
}
