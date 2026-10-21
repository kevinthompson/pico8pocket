package offline

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type StandaloneFile struct{ Name, ID string }

// StandaloneID recognizes only the two filename forms owned by the plugin.
func StandaloneID(name string) (string, error) {
	if filepath.Base(name) != name || strings.ContainsAny(name, `/\`) || !strings.HasSuffix(name, ".p8.png") {
		return "", fmt.Errorf("unrecognized cart filename")
	}
	stem := strings.TrimSuffix(name, ".p8.png")
	if strings.HasSuffix(stem, "]") {
		if at := strings.LastIndex(stem, " ["); at >= 0 {
			stem = stem[at+2 : len(stem)-1]
		}
	}
	if err := ValidateID(stem); err != nil {
		return "", err
	}
	return stem, nil
}

// StandaloneFiles scans one plugin-owned directory, with a bounded entry count.
func StandaloneFiles(pocket string) ([]StandaloneFile, int, error) {
	f, err := os.Open(filepath.Join(pocket, CartPath))
	if os.IsNotExist(err) {
		return nil, 0, nil
	}
	if err != nil {
		return nil, 0, err
	}
	defer f.Close()
	var result []StandaloneFile
	skipped, total := 0, 0
	for {
		entries, err := f.ReadDir(128)
		for _, entry := range entries {
			total++
			if total > 4096 {
				return nil, 0, fmt.Errorf("cart folder exceeds 4096 entries")
			}
			info, statErr := entry.Info()
			if statErr != nil {
				skipped++
				continue
			}
			if !info.Mode().IsRegular() {
				continue
			}
			if !strings.HasSuffix(strings.ToLower(entry.Name()), ".p8.png") {
				continue
			}
			id, err := StandaloneID(entry.Name())
			if err != nil {
				skipped++
				continue
			}
			result = append(result, StandaloneFile{entry.Name(), id})
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, 0, err
		}
	}
	return result, skipped, nil
}

// TidyStandalone copies exclusively before removing the original. It never
// overwrites an existing destination and keeps the source on publication failure.
// Results are renamed, unchanged, or conflict.
func TidyStandalone(pocket string, file StandaloneFile, title string, data []byte) (string, error) {
	if err := ValidatePNG(data); err != nil {
		return "", err
	}
	id, err := StandaloneID(file.Name)
	if err != nil || id != file.ID {
		return "", fmt.Errorf("invalid source identity")
	}
	name, err := CartFilename(id, title)
	if err != nil {
		return "", err
	}
	if name == file.Name {
		return "unchanged", nil
	}
	source := filepath.Join(pocket, CartPath, file.Name)
	target := filepath.Join(pocket, CartPath, name)
	info, err := os.Lstat(source)
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() {
		return "conflict", nil
	}
	current, err := ReadBounded(source, MaxCart)
	if err != nil {
		return "", err
	}
	if !bytes.Equal(current, data) {
		return "conflict", nil
	}
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if os.IsExist(err) {
		targetInfo, e := os.Lstat(target)
		if e != nil || !targetInfo.Mode().IsRegular() {
			return "conflict", nil
		}
		if os.SameFile(info, targetInfo) {
			return "unchanged", nil
		}
		existing, e := ReadBounded(target, MaxCart)
		if e != nil || !bytes.Equal(existing, data) {
			return "conflict", nil
		}
	} else if err != nil {
		return "", err
	} else {
		success := false
		defer func() {
			f.Close()
			if !success {
				os.Remove(target)
			}
		}()
		if _, err = f.Write(data); err != nil {
			return "", err
		}
		if err = f.Sync(); err != nil {
			return "", err
		}
		if err = f.Close(); err != nil {
			return "", err
		}
		success = true
	}
	// Preserve an original changed by another process while the copy was written.
	current, err = ReadBounded(source, MaxCart)
	if err != nil {
		return "", err
	}
	if !bytes.Equal(current, data) {
		return "conflict", nil
	}
	if err = os.Remove(source); err != nil {
		return "", err
	}
	return "renamed", nil
}
