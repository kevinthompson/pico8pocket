package offline

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"unicode/utf8"
)

func TestCartFilenameKeepsTitleAndIdentityWithinOneSafeComponent(t *testing.T) {
	for _, tc := range []struct{ title, want string }{
		{"Still Life", "Still Life [198121].p8.png"},
		{`../Bad: Title/with\paths?*`, "Bad Title with paths [198121].p8.png"},
		{" . ", "Cart [198121].p8.png"},
		{"CON.foo", "_CON.foo [198121].p8.png"},
		{"雪の日", "雪の日 [198121].p8.png"},
	} {
		got, err := CartFilename("198121", tc.title)
		if err != nil || got != tc.want {
			t.Fatalf("%q: %q %v", tc.title, got, err)
		}
	}
	name, err := CartFilename("198121", strings.Repeat("雪", 100))
	if err != nil || !utf8.ValidString(name) || len(name) > 160 {
		t.Fatal(name, err)
	}
	other, _ := CartFilename("198122", "Still Life")
	if other == "Still Life [198121].p8.png" {
		t.Fatal("different BBS IDs collide")
	}
	if _, err = CartFilename("../escape", "Game"); err == nil {
		t.Fatal("unsafe ID accepted")
	}
}

func TestPublishStandaloneMigratesOnlyMatchingLegacyBytes(t *testing.T) {
	for _, matching := range []bool{true, false} {
		pocket, host := t.TempDir(), t.TempDir()
		data := cartPNG()
		old := data
		if !matching {
			old = []byte("unrelated cart bytes")
		}
		legacy := filepath.Join(pocket, CartPath, "15133.p8.png")
		if err := AtomicWrite(legacy, old); err != nil {
			t.Fatal(err)
		}
		name, err := PublishStandalone(pocket, host, "15133", "Celeste", data)
		if err != nil {
			t.Fatal(err)
		}
		got, err := os.ReadFile(filepath.Join(pocket, CartPath, name))
		if err != nil || !bytes.Equal(got, data) {
			t.Fatal(err)
		}
		_, err = os.Stat(legacy)
		if matching && !os.IsNotExist(err) {
			t.Fatal("matching legacy not migrated")
		}
		if !matching {
			got, err = os.ReadFile(legacy)
			if err != nil || !bytes.Equal(got, old) {
				t.Fatal("unrelated legacy modified")
			}
		}
		title, err := CachedTitle(host, "15133", data)
		if err != nil || title != "Celeste" {
			t.Fatal(title, err)
		}
		if _, err = CachedTitle(host, "15133", []byte("different revision")); err == nil {
			t.Fatal("metadata applied to different cart bytes")
		}
	}
}

func TestFailedTitledPublicationPreservesLegacy(t *testing.T) {
	pocket, host := t.TempDir(), t.TempDir()
	data := cartPNG()
	legacy := filepath.Join(pocket, CartPath, "15133.p8.png")
	if err := AtomicWrite(legacy, data); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(pocket, CartPath, "Celeste [15133].p8.png"), 0755); err != nil {
		t.Fatal(err)
	}
	if _, err := PublishStandalone(pocket, host, "15133", "Celeste", data); err == nil {
		t.Fatal("publication into directory succeeded")
	}
	got, err := os.ReadFile(legacy)
	if err != nil || !bytes.Equal(got, data) {
		t.Fatal("failed publication removed legacy", err)
	}
}

func TestTidyStandalonePreservesConflictsAndDeduplicatesIdenticalFiles(t *testing.T) {
	for _, conflict := range []bool{true, false} {
		pocket := t.TempDir()
		data := cartPNG()
		source := filepath.Join(pocket, CartPath, "15133.p8.png")
		if err := AtomicWrite(source, data); err != nil {
			t.Fatal(err)
		}
		target := filepath.Join(pocket, CartPath, "Celeste [15133].p8.png")
		existing := data
		if conflict {
			existing = []byte("preserved conflict")
		}
		if err := AtomicWrite(target, existing); err != nil {
			t.Fatal(err)
		}
		result, err := TidyStandalone(pocket, StandaloneFile{Name: "15133.p8.png", ID: "15133", Dir: "cards/bbs"}, "Celeste", data)
		if err != nil {
			t.Fatal(err)
		}
		got, _ := os.ReadFile(target)
		if !bytes.Equal(got, existing) {
			t.Fatal("destination overwritten")
		}
		_, err = os.Stat(source)
		if conflict && (result != "conflict" || err != nil) {
			t.Fatal(result, err)
		}
		if !conflict && (result != "renamed" || !os.IsNotExist(err)) {
			t.Fatal(result, err)
		}
	}
}

func TestTidyCaseOnlyTitleChangePreservesCart(t *testing.T) {
	pocket := t.TempDir()
	data := cartPNG()
	source := filepath.Join(pocket, CartPath, "Celeste [15133].p8.png")
	if err := AtomicWrite(source, data); err != nil {
		t.Fatal(err)
	}
	result, err := TidyStandalone(pocket, StandaloneFile{Name: "Celeste [15133].p8.png", ID: "15133", Dir: "cards/bbs"}, "celeste", data)
	if err != nil {
		t.Fatal(result, err)
	}
	got, err := os.ReadFile(filepath.Join(pocket, CartPath, "celeste [15133].p8.png"))
	if err != nil || !bytes.Equal(got, data) {
		t.Fatal("case-only alias lost the cart", result, err)
	}
	entries, err := os.ReadDir(filepath.Dir(source))
	if err != nil || result != "renamed" || len(entries) != 1 || entries[0].Name() != "celeste [15133].p8.png" {
		t.Fatal("case-only tidy did not update the stored filename", result, entries, err)
	}
}

func TestTidyCaseOnlyTitlePreservesDistinctConflictingTarget(t *testing.T) {
	pocket := t.TempDir()
	data := cartPNG()
	source := filepath.Join(pocket, CartPath, "Celeste [15133].p8.png")
	if err := AtomicWrite(source, data); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(pocket, CartPath, "celeste [15133].p8.png")
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if os.IsExist(err) {
		t.Skip("requires a case-sensitive filesystem")
	}
	if err != nil {
		t.Fatal(err)
	}
	other := []byte("preserved distinct target")
	if _, err = f.Write(other); err != nil {
		f.Close()
		t.Fatal(err)
	}
	if err = f.Close(); err != nil {
		t.Fatal(err)
	}
	result, err := TidyStandalone(pocket, StandaloneFile{Name: "Celeste [15133].p8.png", ID: "15133", Dir: "cards/bbs"}, "celeste", data)
	if err != nil || result != "conflict" {
		t.Fatal(result, err)
	}
	for path, want := range map[string][]byte{source: data, target: other} {
		got, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(got, want) {
			t.Fatal("case-only tidy changed a conflicting cart", path, err)
		}
	}
}

func TestStandaloneScanIncludesCommonRootAndNestedCartsAndSkipsSymlinks(t *testing.T) {
	pocket := t.TempDir()
	data := cartPNG()
	dir := filepath.Join(pocket, CartPath)
	if err := AtomicWrite(filepath.Join(dir, "15133.p8.png"), data); err != nil {
		t.Fatal(err)
	}
	if err := AtomicWrite(filepath.Join(dir, "nested", "90001.p8.png"), data); err != nil {
		t.Fatal(err)
	}
	if err := AtomicWrite(filepath.Join(pocket, CommonPath, "90003.p8.png"), data); err != nil {
		t.Fatal(err)
	}
	if err := AtomicWrite(filepath.Join(dir, ".DS_Store"), []byte("metadata")); err != nil {
		t.Fatal(err)
	}
	check := func(t *testing.T) {
		t.Helper()
		files, _, err := StandaloneFiles(pocket, nil)
		if err != nil || len(files) != 3 {
			t.Fatal(files, err)
		}
		want := map[string]string{"15133": "cards/bbs", "90001": "cards/bbs/nested", "90003": "."}
		for _, file := range files {
			if dir, ok := want[file.ID]; !ok || dir != file.Dir {
				t.Fatal("incorrect cart location", file)
			}
			delete(want, file.ID)
		}
		if len(want) != 0 {
			t.Fatal("missing carts", want)
		}
	}
	check(t)
	t.Run("symlink", func(t *testing.T) {
		outside := filepath.Join(t.TempDir(), "90002.p8.png")
		if err := os.WriteFile(outside, data, 0600); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(outside, filepath.Join(dir, "90002.p8.png")); err != nil {
			t.Skip("filesystem does not permit test symlinks:", err)
		}
		if err := os.Symlink(filepath.Dir(outside), filepath.Join(dir, "linked-directory")); err != nil {
			t.Fatal(err)
		}
		check(t)
	})
}
