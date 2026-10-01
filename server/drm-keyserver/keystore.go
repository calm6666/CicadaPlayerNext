package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// File and directory permissions. The key file holds secret material, so it is
// created owner-only on POSIX; the directory is tightened for the same reason
// even though MkdirAll leaves an existing directory's mode untouched.
const (
	dataDirPerm  = 0o700
	keysFilePerm = 0o600
	keysFileName = "keys.json"

	// kidHexLen and keyHexLen are the canonical lengths of a KID and of an
	// AES-128 key once hex encoded (16 bytes -> 32 hex chars). The whole API
	// speaks this one representation so there is never any doubt about what a
	// 32-character string means.
	kidHexLen = 32
	keyHexLen = 32
)

// labelMaxLen bounds the human-readable label. The label is metadata only and
// ends up in an atomic file name on some callers' side, so it is kept short and
// free of path-hostile characters.
const labelMaxLen = 128

// apiError carries the HTTP status that belongs to a failure. Handlers emit it
// as {"error":"..."} so that no error path can ever fall through to an empty
// 200. The message is deliberately safe to show a client: it never contains key
// material.
type apiError struct {
	Status int
	Msg    string
}

// Error implements the error interface so apiError can travel as a plain error
// value between the store and the HTTP layer.
func (e *apiError) Error() string { return e.Msg }

// newAPIError builds an apiError without naming the struct at every call site.
func newAPIError(status int, format string, args ...any) *apiError {
	return &apiError{Status: status, Msg: fmt.Sprintf(format, args...)}
}

var (
	// kidPattern accepts the canonical form only: 32 lowercase hex chars.
	// Uppercase is rejected rather than normalised because two spellings of the
	// same KID would make lookups (and duplicate detection) ambiguous.
	kidPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

	// keyPattern accepts the same shape for externally supplied key material.
	keyPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

	// labelPattern rejects control characters, quotes and path separators:
	// labels flow into logs and into callers' file names, and neither should
	// have to defend against a label.
	labelPattern = regexp.MustCompile(`^[\p{L}\p{N} ._@:+-]{0,128}$`)
)

// KeyRecord is one AES-128 key. KID and Key are hex; both are exactly 16 bytes
// of material. Created is RFC3339 so the on-disk file stays diff-friendly.
//
// Scheme records which CENC protection scheme the key was issued for. It is
// metadata for /license (it decides iv_size) and defaults to cenc, because the
// HLS AES-128 path used by the transcoder has no notion of scheme at all.
type KeyRecord struct {
	KID     string `json:"kid"`
	Key     string `json:"key"`
	Created string `json:"created"`
	Label   string `json:"label,omitempty"`
	Scheme  string `json:"scheme,omitempty"`
}

// KeyStore is the in-memory index plus its backing JSON file. Every mutation
// writes the whole file atomically, which keeps crash behaviour simple: the
// file is either the previous or the next complete state.
type KeyStore struct {
	mu       sync.RWMutex
	dir      string
	path     string
	records  map[string]KeyRecord
	keyOrder []string // insertion order, so listings are stable across restarts
}

// NewKeyStore opens (or creates) the store rooted at dir. A missing keys.json
// is not an error: that is simply a fresh deployment. A present but unreadable
// or malformed file IS an error, because silently starting empty would look
// like every previously issued key had vanished.
func NewKeyStore(dir string) (*KeyStore, error) {
	if strings.TrimSpace(dir) == "" {
		return nil, errors.New("data dir must not be empty")
	}
	if err := os.MkdirAll(dir, dataDirPerm); err != nil {
		return nil, fmt.Errorf("create data dir %s: %w", dir, err)
	}
	s := &KeyStore{
		dir:     dir,
		path:    filepath.Join(dir, keysFileName),
		records: make(map[string]KeyRecord),
	}
	if err := s.load(); err != nil {
		return nil, err
	}
	return s, nil
}

// Dir reports the directory the store persists into. Used by the startup log.
func (s *KeyStore) Dir() string { return s.dir }

// Path reports the full path of the JSON file. Used by the startup log.
func (s *KeyStore) Path() string { return s.path }

// load reads keys.json if it exists. Records are validated on the way in so a
// hand-edited file cannot inject a malformed KID into the index.
func (s *KeyStore) load() error {
	raw, err := os.ReadFile(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read %s: %w", s.path, err)
	}
	if len(strings.TrimSpace(string(raw))) == 0 {
		// An empty file is what a truncated write looks like; refuse it instead
		// of pretending the store is empty.
		return fmt.Errorf("%s is empty", s.path)
	}
	var records []KeyRecord
	if err := json.Unmarshal(raw, &records); err != nil {
		return fmt.Errorf("parse %s: %w", s.path, err)
	}
	for _, rec := range records {
		if err := validateRecord(rec); err != nil {
			return fmt.Errorf("%s: %w", s.path, err)
		}
		if _, dup := s.records[rec.KID]; dup {
			return fmt.Errorf("%s: duplicate kid %s", s.path, rec.KID)
		}
		if rec.Created == "" {
			rec.Created = time.Now().UTC().Format(time.RFC3339)
		}
		s.records[rec.KID] = rec
		s.keyOrder = append(s.keyOrder, rec.KID)
	}
	return nil
}

// saveLocked persists the current index. Callers must hold s.mu.
//
// The write goes to a temporary file in the same directory and is then renamed
// over the target: rename is atomic within a filesystem, so a reader (or a
// crash) never observes a half-written key file.
func (s *KeyStore) saveLocked() error {
	records := s.listLocked()
	raw, err := json.MarshalIndent(records, "", "  ")
	if err != nil {
		return fmt.Errorf("encode keys: %w", err)
	}
	raw = append(raw, '\n')

	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, raw, keysFilePerm); err != nil {
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	// WriteFile only applies the mode when it creates the file; enforce it in
	// case the temp file survived an earlier crash with looser permissions.
	if err := os.Chmod(tmp, keysFilePerm); err != nil {
		return fmt.Errorf("chmod %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, s.path); err != nil {
		// Leaving a stray .tmp behind would be confusing on the next start.
		_ = os.Remove(tmp)
		return fmt.Errorf("rename %s: %w", tmp, err)
	}
	return nil
}

// listLocked returns the records in insertion order. Callers must hold s.mu.
func (s *KeyStore) listLocked() []KeyRecord {
	out := make([]KeyRecord, 0, len(s.keyOrder))
	for _, kid := range s.keyOrder {
		rec, ok := s.records[kid]
		if !ok {
			continue
		}
		out = append(out, rec)
	}
	return out
}

// Count reports how many keys are held. Feeds /healthz.
func (s *KeyStore) Count() int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.records)
}

// List returns every record with its key material. This is the admin surface
// and is intentionally unredacted: the caller is either the operator or a
// local transcoding script that needs the raw bytes.
func (s *KeyStore) List() []KeyRecord {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.listLocked()
}

// Lookup returns the record for kid. The error is an *apiError so handlers can
// map it straight onto a status code.
func (s *KeyStore) Lookup(kid string) (KeyRecord, error) {
	if err := validateKID(kid); err != nil {
		return KeyRecord{}, newAPIError(http.StatusBadRequest, "malformed kid: %s", err)
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	rec, ok := s.records[kid]
	if !ok {
		return KeyRecord{}, newAPIError(http.StatusNotFound, "unknown kid %s", kid)
	}
	return rec, nil
}

// KeyIDExists reports whether kid is already known. Convenience for callers
// that want to pre-flight a KID before doing expensive work.
func (s *KeyStore) KeyIDExists(kid string) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	_, ok := s.records[kid]
	return ok
}

// Create validates in, fills in whatever it left blank, persists, and returns
// the stored record.
//
// A caller-supplied kid is honoured verbatim (the transcoding flow often
// derives the KID from the asset so that the manifest can be written before the
// key exists). Everything else comes from crypto/rand.
func (s *KeyStore) Create(in KeyRecord) (KeyRecord, error) {
	if in.KID != "" {
		if err := validateKID(in.KID); err != nil {
			return KeyRecord{}, newAPIError(http.StatusBadRequest, "malformed kid: %s", err)
		}
	} else {
		kid, err := NewKID()
		if err != nil {
			return KeyRecord{}, err
		}
		in.KID = kid
	}
	if in.Key != "" {
		if !keyPattern.MatchString(in.Key) {
			return KeyRecord{}, newAPIError(http.StatusBadRequest,
				"malformed key: want %d lowercase hex chars", keyHexLen)
		}
	} else {
		key, err := NewKeyHex()
		if err != nil {
			return KeyRecord{}, err
		}
		in.Key = key
	}
	if err := validateLabel(in.Label); err != nil {
		return KeyRecord{}, err
	}
	if !isSupportedScheme(in.Scheme) {
		return KeyRecord{}, newAPIError(http.StatusBadRequest,
			"unsupported scheme %q: want %s or %s", in.Scheme, SchemeCENC, SchemeCBCS)
	}
	if in.Scheme == "" {
		in.Scheme = SchemeCENC
	}
	in.Created = time.Now().UTC().Format(time.RFC3339)

	s.mu.Lock()
	defer s.mu.Unlock()
	if _, dup := s.records[in.KID]; dup {
		return KeyRecord{}, newAPIError(http.StatusConflict, "kid %s already exists", in.KID)
	}
	// Persist before publishing to the index: if the write fails the KID is
	// still free and the caller can retry, whereas the reverse order would hand
	// out a key that a restart would forget.
	s.records[in.KID] = in
	s.keyOrder = append(s.keyOrder, in.KID)
	if err := s.saveLocked(); err != nil {
		delete(s.records, in.KID)
		s.keyOrder = s.keyOrder[:len(s.keyOrder)-1]
		return KeyRecord{}, err
	}
	return in, nil
}

// Delete removes the record for kid. A missing KID is the caller's business, so
// the error says which case it was.
func (s *KeyStore) Delete(kid string) error {
	if err := validateKID(kid); err != nil {
		return newAPIError(http.StatusBadRequest, "malformed kid: %s", err)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	rec, ok := s.records[kid]
	if !ok {
		return newAPIError(http.StatusNotFound, "unknown kid %s", kid)
	}
	delete(s.records, kid)
	for i, k := range s.keyOrder {
		if k == kid {
			s.keyOrder = append(s.keyOrder[:i], s.keyOrder[i+1:]...)
			break
		}
	}
	if err := s.saveLocked(); err != nil {
		// Roll back so memory and disk agree; the delete reported failure.
		s.records[kid] = rec
		s.keyOrder = append(s.keyOrder, kid)
		return err
	}
	return nil
}

// validateRecord rejects a record loaded from disk that could not have been
// produced by Create. Catching this at startup beats discovering it at play
// time, when a bad key looks like a corrupt stream.
func validateRecord(rec KeyRecord) error {
	if err := validateKID(rec.KID); err != nil {
		return fmt.Errorf("kid: %w", err)
	}
	if !keyPattern.MatchString(rec.Key) {
		return fmt.Errorf("kid %s: key must be %d lowercase hex chars", rec.KID, keyHexLen)
	}
	if err := validateLabel(rec.Label); err != nil {
		return fmt.Errorf("kid %s: %w", rec.KID, err)
	}
	if !isSupportedScheme(rec.Scheme) {
		return fmt.Errorf("kid %s: unsupported scheme %q", rec.KID, rec.Scheme)
	}
	if rec.Created != "" {
		if _, err := time.Parse(time.RFC3339, rec.Created); err != nil {
			return fmt.Errorf("kid %s: created is not RFC3339: %w", rec.KID, err)
		}
	}
	return nil
}

// validateKID enforces the single canonical KID spelling.
func validateKID(kid string) error {
	if kid == "" {
		return errors.New("kid must not be empty")
	}
	if !kidPattern.MatchString(kid) {
		return fmt.Errorf("want %d lowercase hex chars, got %d chars", kidHexLen, len(kid))
	}
	return nil
}

// validateLabel keeps labels printable and bounded. An empty label is legal:
// the field is optional metadata.
func validateLabel(label string) error {
	if label == "" {
		return nil
	}
	if len(label) > labelMaxLen {
		return newAPIError(http.StatusBadRequest, "label too long: max %d bytes", labelMaxLen)
	}
	if !labelPattern.MatchString(label) {
		return newAPIError(http.StatusBadRequest,
			"malformed label: printable letters, digits and . _ @ : + - only")
	}
	return nil
}

// NewKID returns a fresh 16-byte key id as lowercase hex.
func NewKID() (string, error) {
	buf := make([]byte, KeyBytes)
	if _, err := rand.Read(buf); err != nil {
		return "", fmt.Errorf("generate kid: %w", err)
	}
	return hex.EncodeToString(buf), nil
}

// NewKeyHex returns a fresh AES-128 key as lowercase hex.
func NewKeyHex() (string, error) {
	buf := make([]byte, KeyBytes)
	if _, err := rand.Read(buf); err != nil {
		// The error text carries no random bytes, so it is safe to surface.
		return "", fmt.Errorf("generate key: %w", err)
	}
	return hex.EncodeToString(buf), nil
}

// decodeHexKey turns a stored hex key into the raw 16 bytes. The length check
// is redundant with validation, and that is the point: this is the last gate
// before bytes reach a player.
func decodeHexKey(hexKey string) ([]byte, error) {
	raw, err := hex.DecodeString(hexKey)
	if err != nil {
		return nil, newAPIError(http.StatusInternalServerError, "stored key is not hex")
	}
	if len(raw) != KeyBytes {
		return nil, newAPIError(http.StatusInternalServerError,
			"stored key is %d bytes, want %d", len(raw), KeyBytes)
	}
	return raw, nil
}
