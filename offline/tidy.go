package offline

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type StandaloneFile struct {
	Name, ID string
	// Dir is relative to CommonPath; empty means the common root.
	Dir string
}

// Path checks the relative location and refuses symlinked directories or files.
func (file StandaloneFile) Path(pocket string) (string, error) {
	dir, err := file.relativeDirectory()
	if err != nil {
		return "", err
	}
	parent, err := commonDirectory(pocket, dir)
	if err != nil {
		return "", err
	}
	source := filepath.Join(parent, file.Name)
	info, err := os.Lstat(source)
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("cart source is not a regular file: %s", source)
	}
	return source, nil
}

// Validate checks a saved scan entry without accessing host-controlled roots.
func (file StandaloneFile) Validate() error { _, err := file.relativeDirectory(); return err }

func (file StandaloneFile) relativeDirectory() (string, error) {
	id, err := StandaloneID(file.Name)
	if err != nil || id != file.ID {
		return "", fmt.Errorf("invalid source identity")
	}
	dir := file.Dir
	if dir == "" {
		dir = "."
	}
	if strings.ContainsAny(dir, `\:`) || filepath.IsAbs(dir) {
		return "", fmt.Errorf("invalid cart directory")
	}
	dir = filepath.Clean(filepath.FromSlash(dir))
	if dir == ".." || strings.HasPrefix(dir, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("cart directory escapes common root")
	}
	return dir, nil
}

func commonDirectory(pocket, relative string) (string, error) {
	directory := pocket
	for _, component := range strings.Split(filepath.ToSlash(filepath.Join(CommonPath, relative)), "/") {
		directory = filepath.Join(directory, component)
		info, err := os.Lstat(directory)
		if err != nil {
			return "", err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "", fmt.Errorf("cart directory is not a regular directory: %s", directory)
		}
	}
	return directory, nil
}

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

// StandaloneFiles recursively scans CommonPath, without following symlinks.
// The entry and depth limits cover the entire traversal before any renaming.
func StandaloneFiles(pocket string, checkCancel func() error) ([]StandaloneFile, int, error) {
	root, err := commonDirectory(pocket, ".")
	if os.IsNotExist(err) {
		return nil, 0, nil
	}
	if err != nil {
		return nil, 0, err
	}
	var result []StandaloneFile
	skipped, total := 0, 0
	var walk func(string, int) error
	walk = func(relative string, depth int) error {
		if checkCancel != nil {
			if err := checkCancel(); err != nil {
				return err
			}
		}
		f, err := os.Open(filepath.Join(root, relative))
		if err != nil {
			return err
		}
		defer f.Close()
		for {
			entries, err := f.ReadDir(128)
			for _, entry := range entries {
				if checkCancel != nil {
					if err := checkCancel(); err != nil {
						return err
					}
				}
				total++
				if total > 4096 {
					return fmt.Errorf("common cart tree exceeds 4096 entries")
				}
				info, statErr := entry.Info()
				if statErr != nil {
					skipped++
					continue
				}
				if info.Mode()&os.ModeSymlink != 0 {
					continue
				}
				if info.IsDir() {
					if depth >= 32 {
						return fmt.Errorf("common cart tree exceeds 32 directory levels")
					}
					if err := walk(filepath.Join(relative, entry.Name()), depth+1); err != nil {
						return err
					}
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
				result = append(result, StandaloneFile{Name: entry.Name(), ID: id, Dir: filepath.ToSlash(relative)})
			}
			if err == io.EOF {
				break
			}
			if err != nil {
				return err
			}
		}
		return nil
	}
	if err := walk(".", 0); err != nil {
		return nil, 0, err
	}
	return result, skipped, nil
}

// TidyStandalone copies exclusively before removing the original. It never
// overwrites an existing destination and keeps the source on publication failure.
// Results are renamed, unchanged, or conflict.
func TidyStandalone(pocket string, file StandaloneFile, title string, data []byte) (string, error) {
	return TidyStandaloneAs(pocket, file, file.ID, title, data)
}

// TidyStandaloneAs publishes a verified destination identity while preserving
// source-path validation, exclusive creation and byte rechecks.
func TidyStandaloneAs(pocket string, file StandaloneFile, targetID, title string, data []byte) (string, error) {
	if err := ValidatePNG(data); err != nil {
		return "", err
	}
	source, err := file.Path(pocket)
	if err != nil {
		return "", err
	}
	name, err := CartFilename(targetID, title)
	if err != nil {
		return "", err
	}
	if name == file.Name {
		return "unchanged", nil
	}
	target := filepath.Join(filepath.Dir(source), name)
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
			if strings.EqualFold(file.Name, name) {
				return renameCaseAlias(source, target, os.Rename, copyCaseCart)
			}
			return "unchanged", nil
		}
		existing, e := ReadBounded(target, MaxCart)
		if e != nil || !bytes.Equal(existing, data) {
			return "conflict", nil
		}
	} else if err != nil {
		return "", err
	} else {
		if err = writeTidyFile(f, data); err != nil {
			return "", err
		}
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

// writeTidyFile receives only a newly and exclusively created destination.
func writeTidyFile(f *os.File, data []byte) error {
	success := false
	defer func() {
		f.Close()
		if !success {
			os.Remove(f.Name())
		}
	}()
	if _, err := f.Write(data); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	success = true
	return nil
}
