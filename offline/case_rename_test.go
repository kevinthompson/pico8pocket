package offline

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCaseRenameFailureRetainsCart(t *testing.T) {
	for _, tc := range []struct {
		name        string
		moveFails   bool
		failPublish map[int]bool
		backup      bool
	}{
		{"initial move", true, nil, false},
		{"publication with successful rollback", false, map[int]bool{1: true}, false},
		{"publication and rollback", false, map[int]bool{1: true, 2: true}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			source, target := filepath.Join(dir, "Cart.p8.png"), filepath.Join(dir, "cart.p8.png")
			data := cartPNG()
			if err := os.WriteFile(source, data, 0644); err != nil {
				t.Fatal(err)
			}
			failure := errors.New("injected rename failure")
			rename := func(from, to string) error {
				if tc.moveFails {
					return failure
				}
				return os.Rename(from, to)
			}
			calls := 0
			publish := func(from, to string) error {
				calls++
				if tc.failPublish[calls] {
					return failure
				}
				return copyCaseCart(from, to)
			}
			_, err := renameCaseAlias(source, target, rename, publish)
			if !errors.Is(err, failure) {
				t.Fatal("rename failure not reported", err)
			}
			entries, readErr := os.ReadDir(dir)
			if readErr != nil || len(entries) != 1 {
				t.Fatal("cart missing or unexpected partial files", entries, readErr)
			}
			path := filepath.Join(dir, entries[0].Name())
			if tc.backup {
				if !strings.HasPrefix(entries[0].Name(), ".pico8-tidy-") || !strings.Contains(err.Error(), path) {
					t.Fatal("recovery filename not reported", err, entries)
				}
			} else if path != source {
				t.Fatal("original filename not restored", path)
			}
			got, readErr := os.ReadFile(path)
			if readErr != nil || !bytes.Equal(got, data) {
				t.Fatal("cart bytes changed", readErr)
			}
		})
	}
}

func TestCaseRenamePreservesDestinationCreatedDuringMove(t *testing.T) {
	dir := t.TempDir()
	source, target := filepath.Join(dir, "Cart.p8.png"), filepath.Join(dir, "cart.p8.png")
	data, other := cartPNG(), []byte("new destination")
	if err := os.WriteFile(source, data, 0644); err != nil {
		t.Fatal(err)
	}
	calls := 0
	rename := func(from, to string) error {
		calls++
		if err := os.Rename(from, to); err != nil {
			return err
		}
		if calls == 1 {
			return os.WriteFile(target, other, 0644)
		}
		return nil
	}
	result, err := renameCaseAlias(source, target, rename, copyCaseCart)
	if err == nil && result != "conflict" {
		t.Fatal("destination collision not reported", result)
	}
	got, readErr := os.ReadFile(target)
	if readErr != nil || !bytes.Equal(got, other) {
		t.Fatal("new destination overwritten", readErr)
	}
	entries, readErr := os.ReadDir(dir)
	if readErr != nil || len(entries) != 2 {
		t.Fatal(entries, readErr)
	}
	for _, entry := range entries {
		if entry.Name() == "cart.p8.png" {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		got, readErr = os.ReadFile(path)
		if readErr != nil || !bytes.Equal(got, data) {
			t.Fatal("original cart lost", path, readErr)
		}
		if err != nil && !strings.Contains(err.Error(), path) {
			t.Fatal("recovery filename not reported", err)
		}
	}
}

func TestCaseRenameExclusivePublicationAndRestorePreserveNewFiles(t *testing.T) {
	for _, stage := range []int{1, 2} {
		name := "publication"
		if stage == 2 {
			name = "restoration"
		}
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			source, target := filepath.Join(dir, "Cart.p8.png"), filepath.Join(dir, "cart.p8.png")
			data, other := cartPNG(), []byte("concurrently added cart")
			if err := os.WriteFile(source, data, 0644); err != nil {
				t.Fatal(err)
			}
			calls, collisionPath := 0, ""
			publish := func(from, to string) error {
				calls++
				if stage == 2 && calls == 1 {
					return errors.New("publication failed")
				}
				if calls == stage {
					collisionPath = to
					if err := os.WriteFile(to, other, 0644); err != nil {
						return err
					}
				}
				return copyCaseCart(from, to)
			}
			_, err := renameCaseAlias(source, target, os.Rename, publish)
			if err == nil || collisionPath == "" {
				t.Fatal("late collision not reported", err)
			}
			got, readErr := os.ReadFile(collisionPath)
			if readErr != nil || !bytes.Equal(got, other) {
				t.Fatal("concurrent file overwritten", readErr)
			}
			entries, readErr := os.ReadDir(dir)
			if readErr != nil || len(entries) != 2 {
				t.Fatal("original or concurrent cart lost", entries, readErr)
			}
			for _, entry := range entries {
				path := filepath.Join(dir, entry.Name())
				if path == collisionPath {
					continue
				}
				got, readErr = os.ReadFile(path)
				if readErr != nil || !bytes.Equal(got, data) {
					t.Fatal("original cart bytes changed", path, readErr)
				}
				if strings.HasPrefix(entry.Name(), ".pico8-tidy-") && !strings.Contains(err.Error(), path) {
					t.Fatal("recovery path not reported", err)
				}
			}
		})
	}
}
