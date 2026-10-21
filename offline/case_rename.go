package offline

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
)

// renameCaseAlias is called only after source and target have the same file
// identity. A reserved temporary name forces case-preserving filesystems to
// store the requested spelling. Never remove the temporary cart on failure.
func renameCaseAlias(source, target string, rename, publish func(string, string) error) (string, error) {
	f, err := os.CreateTemp(filepath.Dir(source), ".pico8-tidy-*")
	if err != nil {
		return "", err
	}
	temporary := f.Name()
	if err = f.Close(); err != nil {
		os.Remove(temporary)
		return "", err
	}
	if err = rename(source, temporary); err != nil {
		os.Remove(temporary) // Still our empty reservation, never a cartridge.
		return "", err
	}
	restore := func() error {
		if e := publish(temporary, source); e != nil {
			return fmt.Errorf("restoring original failed: %v; cart preserved at %s", e, temporary)
		}
		return nil
	}
	failed := func(cause error) (string, error) {
		if e := restore(); e != nil {
			return "", fmt.Errorf("%w; %v", cause, e)
		}
		return "", cause
	}
	// A case-sensitive volume can contain two hard links with case-only names.
	// Moving one link will not free the other. Preserve both, and also refuse a
	// different destination that appeared while the original was being moved.
	targetInfo, err := os.Lstat(target)
	if err == nil {
		movedInfo, e := os.Lstat(temporary)
		result := "conflict"
		if e == nil && os.SameFile(movedInfo, targetInfo) {
			result = "unchanged"
		}
		if e = restore(); e != nil {
			return "", e
		}
		return result, nil
	}
	if !os.IsNotExist(err) {
		return failed(err)
	}
	if err = publish(temporary, target); err != nil {
		return failed(err)
	}
	return "renamed", nil
}

// copyCaseCart keeps the recovery cart until an exclusive copy is complete.
// This also prevents rollback from replacing a newly created original path.
func copyCaseCart(source, target string) error {
	data, err := ReadBounded(source, MaxCart)
	if err != nil {
		return err
	}
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		return err
	}
	if err = writeTidyFile(f, data); err != nil {
		return err
	}
	current, err := ReadBounded(source, MaxCart)
	if err != nil {
		return err
	}
	if !bytes.Equal(current, data) {
		return fmt.Errorf("cart changed during publication: %s", source)
	}
	return os.Remove(source)
}
