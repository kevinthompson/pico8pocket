package offline

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
)

func ReadBounded(path string, limit int64) ([]byte, error) {
	if limit < 0 || limit >= int64(^uint64(0)>>1) {
		return nil, fmt.Errorf("invalid read limit")
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if info.Size() > limit {
		return nil, fmt.Errorf("file too large: %s", path)
	}
	data, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("file grew beyond limit: %s", path)
	}
	return data, nil
}

// AtomicWrite publishes with a same-directory rename. The previous destination
// remains available until every byte has been written and flushed successfully.
func AtomicWrite(path string, data []byte) error {
	parent := filepath.Dir(path)
	if err := os.MkdirAll(parent, 0755); err != nil {
		return err
	}
	temporary := path + ".pocket-sync-tmp"
	f, err := os.OpenFile(temporary, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	success := false
	defer func() {
		f.Close()
		if !success {
			os.Remove(temporary)
		}
	}()
	if _, err = f.Write(data); err != nil {
		return err
	}
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	if err = os.Rename(temporary, path); err != nil {
		return err
	}
	success = true
	return nil
}
func CachePath(host, id string) string {
	return filepath.Join(host, "pico8-offline-cache", id+".p8.png")
}
func CachedCart(host, id string) ([]byte, error) {
	if err := ValidateID(id); err != nil {
		return nil, err
	}
	data, err := ReadBounded(CachePath(host, id), MaxCart)
	if err != nil {
		return nil, err
	}
	if err = ValidatePNG(data); err != nil {
		return nil, err
	}
	return data, nil
}

// ImportCache reads only the explicit source directory and validates the entire
// selection before changing any cache file.
func ImportCache(source, host, collection string) (int, error) {
	if collection != "arcade" && collection != "sorcerer" && collection != "both" {
		return 0, fmt.Errorf("collection must be arcade, sorcerer or both")
	}
	catalog, err := Catalog()
	if err != nil {
		return 0, err
	}
	staged := make(Carts)
	for _, cart := range catalog {
		if collection != "both" && collection != cart.Collection {
			continue
		}
		data, err := ReadBounded(filepath.Join(source, cart.ID+".p8.png"), MaxCart)
		if err != nil {
			return 0, err
		}
		if err = ValidatePNG(data); err != nil {
			return 0, fmt.Errorf("%s: %w", cart.ID, err)
		}
		staged[cart.ID] = data
	}
	for _, cart := range catalog {
		if data, ok := staged[cart.ID]; ok {
			if err := AtomicWrite(CachePath(host, cart.ID), data); err != nil {
				return 0, err
			}
		}
	}
	return len(staged), nil
}
