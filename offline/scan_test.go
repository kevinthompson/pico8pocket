package offline

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestStandaloneScanCanCancelBeforeReturningPartialResults(t *testing.T) {
	pocket := t.TempDir()
	for i := 0; i < 10; i++ {
		if err := AtomicWrite(filepath.Join(pocket, CommonPath, fmt.Sprintf("%d.p8.png", i)), cartPNG()); err != nil {
			t.Fatal(err)
		}
	}
	cancelled := errors.New("cancelled scan")
	calls := 0
	files, _, err := StandaloneFiles(pocket, func() error {
		calls++
		if calls >= 3 {
			return cancelled
		}
		return nil
	})
	if !errors.Is(err, cancelled) || len(files) != 0 {
		t.Fatal("cancelled scan exposed a partial rename set", files, err)
	}
}

func TestStandaloneScanBoundsWholeTree(t *testing.T) {
	pocket := t.TempDir()
	for _, folder := range []string{"a", "b"} {
		dir := filepath.Join(pocket, CommonPath, folder)
		if err := os.MkdirAll(dir, 0755); err != nil {
			t.Fatal(err)
		}
		for i := 0; i < 2050; i++ {
			if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("%d.txt", i)), nil, 0644); err != nil {
				t.Fatal(err)
			}
		}
	}
	files, _, err := StandaloneFiles(pocket, nil)
	if err == nil || !strings.Contains(err.Error(), "4096") || len(files) != 0 {
		t.Fatal("entry bound reset between directories", files, err)
	}
}

func TestStandaloneScanBoundsDirectoryDepth(t *testing.T) {
	pocket := t.TempDir()
	dir := filepath.Join(pocket, CommonPath)
	for i := 0; i < 33; i++ {
		dir = filepath.Join(dir, "n")
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if files, _, err := StandaloneFiles(pocket, nil); err == nil || len(files) != 0 {
		t.Fatal("unbounded directory traversal", files, err)
	}
}

func TestStandalonePathRejectsEscapesAndSymlinkedParents(t *testing.T) {
	pocket := t.TempDir()
	for _, dir := range []string{"..", "../outside", "/outside", `..\outside`, "C:/outside"} {
		file := StandaloneFile{Name: "90001.p8.png", ID: "90001", Dir: dir}
		if _, err := file.Path(pocket); err == nil {
			t.Fatal("unsafe cart directory accepted", dir)
		}
	}
	common := filepath.Join(pocket, CommonPath)
	if err := os.MkdirAll(common, 0755); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	if err := os.WriteFile(filepath.Join(outside, "90001.p8.png"), cartPNG(), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(common, "linked")); err != nil {
		t.Skip("filesystem does not permit test symlinks:", err)
	}
	file := StandaloneFile{Name: "90001.p8.png", ID: "90001", Dir: "linked"}
	if _, err := file.Path(pocket); err == nil {
		t.Fatal("symlinked cart directory accepted")
	}
}

func TestStandaloneScanRejectsSymlinkedCommonAncestor(t *testing.T) {
	pocket := t.TempDir()
	if err := os.MkdirAll(filepath.Join(pocket, "Assets"), 0755); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	if err := os.Mkdir(filepath.Join(outside, "common"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(pocket, "Assets/pico8")); err != nil {
		t.Skip("filesystem does not permit test symlinks:", err)
	}
	if _, _, err := StandaloneFiles(pocket, nil); err == nil {
		t.Fatal("scan followed a symlink into another asset tree")
	}
}
