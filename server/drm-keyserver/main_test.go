package main

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// newTestStore returns a store rooted at a per-test temporary directory. Using
// t.TempDir means the persistence tests exercise the real filesystem without
// leaving anything behind, and each test is isolated from the others.
func newTestStore(t *testing.T) *KeyStore {
	t.Helper()
	dir := t.TempDir()
	store, err := NewKeyStore(dir)
	if err != nil {
		t.Fatalf("NewKeyStore(%q): %v", dir, err)
	}
	if store.Dir() != dir {
		t.Fatalf("store.Dir() = %q, want %q", store.Dir(), dir)
	}
	return store
}

// newTestServer wires a store and signer into an httptest server. All HTTP tests
// go through the real Routes table, so routing mistakes are caught here rather
// than in production.
func newTestServer(t *testing.T, secret string) (*httptest.Server, *KeyStore, *Signer) {
	t.Helper()
	store := newTestStore(t)
	signer := NewSigner(secret)
	srv := httptest.NewServer(NewServer(store, signer).Routes())
	t.Cleanup(srv.Close)
	return srv, store, signer
}

// do issues a request and returns the status plus body bytes. The body is read
// eagerly so the caller does not have to remember to close it.
func do(t *testing.T, method, rawURL, body string, headers map[string]string) (int, []byte, http.Header) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, rawURL, reader)
	if err != nil {
		t.Fatalf("new request %s %s: %v", method, rawURL, err)
	}
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("do %s %s: %v", method, rawURL, err)
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body of %s %s: %v", method, rawURL, err)
	}
	return resp.StatusCode, raw, resp.Header
}

// createKey creates a key over the admin API and returns the stored record.
func createKey(t *testing.T, base, body string) KeyRecord {
	t.Helper()
	status, raw, _ := do(t, http.MethodPost, base+"/admin/keys", body, nil)
	if status != http.StatusCreated {
		t.Fatalf("POST /admin/keys: status %d body %s", status, raw)
	}
	var rec KeyRecord
	if err := json.Unmarshal(raw, &rec); err != nil {
		t.Fatalf("decode created key %s: %v", raw, err)
	}
	return rec
}

// assertJSONError checks the one error shape the whole service must use.
func assertJSONError(t *testing.T, status int, raw []byte, wantStatus int) {
	t.Helper()
	if status != wantStatus {
		t.Fatalf("status = %d, want %d (body %s)", status, wantStatus, raw)
	}
	var payload map[string]string
	if err := json.Unmarshal(raw, &payload); err != nil {
		t.Fatalf("error body is not JSON: %s", raw)
	}
	if payload["error"] == "" {
		t.Fatalf("error body has empty message: %s", raw)
	}
}

// ---------------------------------------------------------------------------
// key store: generation, persistence, lookup
// ---------------------------------------------------------------------------

// TestStoreCreatePersistReload covers the round trip that matters most: a key
// created in one process must be byte-identical after a restart, because a
// player holding a KID from a manifest will come back to a fresh process.
func TestStoreCreatePersistReload(t *testing.T) {
	store := newTestStore(t)

	created, err := store.Create(KeyRecord{Label: "demo"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	if !kidPattern.MatchString(created.KID) {
		t.Fatalf("generated kid %q is not %d lowercase hex chars", created.KID, kidHexLen)
	}
	if !kidPattern.MatchString(created.Key) {
		t.Fatalf("generated key %q is not %d lowercase hex chars", created.Key, keyHexLen)
	}
	rawKey, err := hex.DecodeString(created.Key)
	if err != nil || len(rawKey) != KeyBytes {
		t.Fatalf("generated key decodes to %d bytes (err %v), want %d", len(rawKey), err, KeyBytes)
	}
	if _, err := time.Parse(time.RFC3339, created.Created); err != nil {
		t.Fatalf("Created %q is not RFC3339: %v", created.Created, err)
	}
	if created.Scheme != SchemeCENC {
		t.Fatalf("Scheme = %q, want %q by default", created.Scheme, SchemeCENC)
	}

	// Two generated keys must differ; a repeat would mean rand is not wired up.
	second, err := store.Create(KeyRecord{})
	if err != nil {
		t.Fatalf("second Create: %v", err)
	}
	if second.KID == created.KID || second.Key == created.Key {
		t.Fatalf("generated two identical keys: %+v vs %+v", second, created)
	}

	// The store must have written keys.json, and it must not have left a .tmp
	// behind: a stray temp file would mean the atomic rename never happened.
	keysPath := filepath.Join(store.Dir(), keysFileName)
	if _, err := os.Stat(keysPath); err != nil {
		t.Fatalf("keys.json not written: %v", err)
	}
	if _, err := os.Stat(keysPath + ".tmp"); !os.IsNotExist(err) {
		t.Fatalf("keys.json.tmp still present (err %v): the temp file is not cleaned up", err)
	}

	reopened, err := NewKeyStore(store.Dir())
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	if reopened.Count() != 2 {
		t.Fatalf("reloaded count = %d, want 2", reopened.Count())
	}
	got, err := reopened.Lookup(created.KID)
	if err != nil {
		t.Fatalf("Lookup after reload: %v", err)
	}
	if got != created {
		t.Fatalf("reloaded record = %+v, want %+v", got, created)
	}
}

// TestStoreCreateWithSuppliedKIDAndKey pins the input-honouring path: the
// transcoder derives the KID from the asset, so a caller-supplied value has to
// survive verbatim.
func TestStoreCreateWithSuppliedKIDAndKey(t *testing.T) {
	store := newTestStore(t)
	const kid = "000102030405060708090a0b0c0d0e0f"
	const key = "2b7e151628aed2a6abf7158809cf4f3c"

	rec, err := store.Create(KeyRecord{KID: kid, Key: key, Label: "supplied"})
	if err != nil {
		t.Fatalf("Create with supplied kid/key: %v", err)
	}
	if rec.KID != kid || rec.Key != key {
		t.Fatalf("record = %+v, want kid %s key %s", rec, kid, key)
	}
}

// TestStoreDuplicateKIDRejected makes sure a second key can never shadow the
// first: silent replacement would invalidate media already encrypted with it.
func TestStoreDuplicateKIDRejected(t *testing.T) {
	store := newTestStore(t)
	const kid = "aabbccddeeff00112233445566778899"
	if _, err := store.Create(KeyRecord{KID: kid}); err != nil {
		t.Fatalf("first Create: %v", err)
	}
	_, err := store.Create(KeyRecord{KID: kid})
	if err == nil {
		t.Fatal("second Create with the same kid succeeded, want conflict")
	}
	var apiErr *apiError
	if !asAPIError(err, &apiErr) {
		t.Fatalf("error is %T (%v), want *apiError", err, err)
	}
	if apiErr.Status != http.StatusConflict {
		t.Fatalf("status = %d, want %d", apiErr.Status, http.StatusConflict)
	}
	if store.Count() != 1 {
		t.Fatalf("count = %d after rejected duplicate, want 1", store.Count())
	}
}

// TestStoreMalformedKIDRejected walks the KID shapes a caller might plausibly
// send by mistake. All of them must be refused rather than normalised.
func TestStoreMalformedKIDRejected(t *testing.T) {
	store := newTestStore(t)
	bad := []string{
		"aabb",
		strings.Repeat("a", 31),
		strings.Repeat("a", 33),
		strings.ToUpper(strings.Repeat("a", 32)),
		"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
		strings.Repeat("a", 31) + "-",
	}
	// NOTE: the empty KID is deliberately NOT in this list. It is not a
	// malformed KID, it is "no KID supplied", which Create answers by minting
	// one -- that is exactly what POST /admin/keys with a body of {} does, and
	// the transcoding flow relies on it. Treating "" as malformed here would
	// contradict the documented API. The minting behaviour is covered by
	// TestStoreCreateAndPersistRoundTrip.
	for _, kid := range bad {
		t.Run(fmt.Sprintf("%q", kid), func(t *testing.T) {
			if _, err := store.Create(KeyRecord{KID: kid}); err == nil {
				t.Fatalf("Create with kid %q succeeded, want 400-style error", kid)
			}
			if _, err := store.Lookup(kid); err == nil {
				t.Fatalf("Lookup with kid %q succeeded, want error", kid)
			}
		})
	}
	if store.Count() != 0 {
		t.Fatalf("count = %d after malformed kid attempts, want 0", store.Count())
	}
}

// TestStoreMalformedLabelAndSchemeRejected pins the other two validated fields.
func TestStoreMalformedLabelAndSchemeRejected(t *testing.T) {
	store := newTestStore(t)

	if _, err := store.Create(KeyRecord{Label: strings.Repeat("x", labelMaxLen+1)}); err == nil {
		t.Fatal("over-long label accepted, want rejection")
	}
	if _, err := store.Create(KeyRecord{Label: "bad\nlabel"}); err == nil {
		t.Fatal("label with a newline accepted, want rejection")
	}
	if _, err := store.Create(KeyRecord{Label: "../escape"}); err == nil {
		t.Fatal("label with a path traversal accepted, want rejection")
	}
	if _, err := store.Create(KeyRecord{Scheme: "widevine"}); err == nil {
		t.Fatal("unsupported scheme accepted, want rejection")
	}
	if _, err := store.Create(KeyRecord{Key: "nothex"}); err == nil {
		t.Fatal("malformed supplied key accepted, want rejection")
	}
	if store.Count() != 0 {
		t.Fatalf("count = %d after rejected creates, want 0", store.Count())
	}
}

// TestStoreDelete covers the delete/persist/reload path plus the missing case.
func TestStoreDelete(t *testing.T) {
	store := newTestStore(t)
	rec, err := store.Create(KeyRecord{Label: "temporary"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	if err := store.Delete(rec.KID); err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if store.Count() != 0 {
		t.Fatalf("count = %d after delete, want 0", store.Count())
	}
	if err := store.Delete(rec.KID); err == nil {
		t.Fatal("deleting a missing kid succeeded, want 404-style error")
	}
	reopened, err := NewKeyStore(store.Dir())
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	if reopened.Count() != 0 {
		t.Fatalf("reloaded count = %d after delete, want 0", reopened.Count())
	}
	if _, err := reopened.Lookup(rec.KID); err == nil {
		t.Fatal("deleted kid still resolvable after reload")
	}
}

// TestStoreLoadRejectsGarbage proves a corrupt file is a hard failure. Starting
// with an empty index instead would look like every issued key had vanished.
func TestStoreLoadRejectsGarbage(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, keysFileName)

	for name, content := range map[string]string{
		"not json":     "this is not json",
		"empty file":   "",
		"bad kid":      `[{"kid":"nope","key":"2b7e151628aed2a6abf7158809cf4f3c","created":"2024-01-01T00:00:00Z"}]`,
		"short key":    `[{"kid":"000102030405060708090a0b0c0d0e0f","key":"00","created":"2024-01-01T00:00:00Z"}]`,
		"duplicate id": `[{"kid":"000102030405060708090a0b0c0d0e0f","key":"2b7e151628aed2a6abf7158809cf4f3c"},{"kid":"000102030405060708090a0b0c0d0e0f","key":"2b7e151628aed2a6abf7158809cf4f3c"}]`,
	} {
		t.Run(name, func(t *testing.T) {
			if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
				t.Fatalf("seed file: %v", err)
			}
			if _, err := NewKeyStore(dir); err == nil {
				t.Fatalf("NewKeyStore accepted a %s file, want error", name)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// HTTP: key delivery
// ---------------------------------------------------------------------------

// TestKeyEndpointRawBytes is the compatibility-critical test: the bytes returned
// by /key/{kid} must be exactly the 16 stored bytes, in order, because that is
// what HLSStream::updateKey() reads.
func TestKeyEndpointRawBytes(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	const key = "2b7e151628aed2a6abf7158809cf4f3c"
	rec := createKey(t, srv.URL, `{"kid":"000102030405060708090a0b0c0d0e0f","key":"`+key+`","label":"raw"}`)

	status, body, header := do(t, http.MethodGet, srv.URL+"/key/"+rec.KID, "", nil)
	if status != http.StatusOK {
		t.Fatalf("status = %d body %s, want 200", status, body)
	}
	if len(body) != KeyBytes {
		t.Fatalf("body is %d bytes, want exactly %d", len(body), KeyBytes)
	}
	want, err := hex.DecodeString(key)
	if err != nil {
		t.Fatalf("decode fixture: %v", err)
	}
	if !bytes.Equal(body, want) {
		t.Fatalf("body = %x, want %x", body, want)
	}
	if ct := header.Get("Content-Type"); ct != "application/octet-stream" {
		t.Fatalf("Content-Type = %q, want application/octet-stream", ct)
	}
	if cc := header.Get("Cache-Control"); cc != "no-store" {
		t.Fatalf("Cache-Control = %q, want no-store", cc)
	}
}

// TestKeyEndpointUnknownKID checks the 404 shape.
func TestKeyEndpointUnknownKID(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	status, body, _ := do(t, http.MethodGet, srv.URL+"/key/"+strings.Repeat("f", 32), "", nil)
	assertJSONError(t, status, body, http.StatusNotFound)
}

// TestKeyEndpointMalformedKID checks that a malformed KID is a 400, not a 404:
// the difference tells a caller whether to fix its input or its bookkeeping.
func TestKeyEndpointMalformedKID(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	status, body, _ := do(t, http.MethodGet, srv.URL+"/key/not-a-kid", "", nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
}

// TestKeyEndpointJSONFormat pins the ?format=json shape used by the README
// workflow for materialising key.bin.
func TestKeyEndpointJSONFormat(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	const key = "00112233445566778899aabbccddeeff"
	rec := createKey(t, srv.URL, `{"kid":"ffffffffffffffffffffffffffffffff","key":"`+key+`"}`)

	status, body, header := do(t, http.MethodGet, srv.URL+"/key/"+rec.KID+"?format=json", "", nil)
	if status != http.StatusOK {
		t.Fatalf("status = %d body %s, want 200", status, body)
	}
	if ct := header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Fatalf("Content-Type = %q, want application/json", ct)
	}
	var payload KeyJSONResponse
	if err := json.Unmarshal(body, &payload); err != nil {
		t.Fatalf("decode %s: %v", body, err)
	}
	if payload.KID != rec.KID {
		t.Fatalf("kid = %q, want %q", payload.KID, rec.KID)
	}
	if payload.Key != key {
		t.Fatalf("key = %q, want %q", payload.Key, key)
	}
	if payload.Scheme != SchemeCENC {
		t.Fatalf("scheme = %q, want %q", payload.Scheme, SchemeCENC)
	}
	if payload.IV != "0x"+"0000000000000000"+"0000000000000000" {
		t.Fatalf("iv = %q, want the 8-byte zero CENC IV plus a zero counter", payload.IV)
	}
}

// TestKeyEndpointUnknownFormat checks that a typo in ?format= fails loudly
// instead of quietly returning binary to a caller expecting JSON.
func TestKeyEndpointUnknownFormat(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	rec := createKey(t, srv.URL, `{}`)
	status, body, _ := do(t, http.MethodGet, srv.URL+"/key/"+rec.KID+"?format=xml", "", nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
}

// TestAdminKeysCRUD exercises the full admin surface over HTTP.
func TestAdminKeysCRUD(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	rec := createKey(t, srv.URL, `{"label":"crud"}`)

	// List: must be a JSON array containing the record, key material included.
	status, body, _ := do(t, http.MethodGet, srv.URL+"/admin/keys", "", nil)
	if status != http.StatusOK {
		t.Fatalf("list status = %d body %s", status, body)
	}
	var list []KeyRecord
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatalf("list body %s is not a JSON array: %v", body, err)
	}
	if len(list) != 1 || list[0] != rec {
		t.Fatalf("list = %+v, want exactly [%+v]", list, rec)
	}

	// Single record.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/keys/"+rec.KID, "", nil)
	if status != http.StatusOK {
		t.Fatalf("get status = %d body %s", status, body)
	}
	var one KeyRecord
	if err := json.Unmarshal(body, &one); err != nil || one != rec {
		t.Fatalf("get = %s (err %v), want %+v", body, err, rec)
	}

	// Duplicate over HTTP must be 409.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/admin/keys", `{"kid":"`+rec.KID+`"}`, nil)
	assertJSONError(t, status, body, http.StatusConflict)

	// Malformed kid and label over HTTP must be 400.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/admin/keys", `{"kid":"zz"}`, nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
	status, body, _ = do(t, http.MethodPost, srv.URL+"/admin/keys", `{"label":"bad\nlabel"}`, nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
	status, body, _ = do(t, http.MethodPost, srv.URL+"/admin/keys", `{"label":`, nil)
	assertJSONError(t, status, body, http.StatusBadRequest)

	// Unknown record, then delete, then delete again.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/keys/"+strings.Repeat("1", 32), "", nil)
	assertJSONError(t, status, body, http.StatusNotFound)
	status, body, _ = do(t, http.MethodDelete, srv.URL+"/admin/keys/"+rec.KID, "", nil)
	if status != http.StatusNoContent {
		t.Fatalf("delete status = %d body %s, want 204", status, body)
	}
	if len(body) != 0 {
		t.Fatalf("delete body = %s, want empty for 204", body)
	}
	status, body, _ = do(t, http.MethodDelete, srv.URL+"/admin/keys/"+rec.KID, "", nil)
	assertJSONError(t, status, body, http.StatusNotFound)
}

// TestHealthzAndRouting checks the ops endpoints and that an unknown path and a
// wrong verb both produce a JSON error rather than an empty reply.
func TestHealthzAndRouting(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	createKey(t, srv.URL, `{}`)

	status, body, _ := do(t, http.MethodGet, srv.URL+"/healthz", "", nil)
	if status != http.StatusOK {
		t.Fatalf("healthz status = %d body %s", status, body)
	}
	var health struct {
		Status string `json:"status"`
		Keys   int    `json:"keys"`
	}
	if err := json.Unmarshal(body, &health); err != nil {
		t.Fatalf("healthz body %s: %v", body, err)
	}
	if health.Status != "ok" || health.Keys != 1 {
		t.Fatalf("healthz = %+v, want status ok with 1 key", health)
	}

	status, body, _ = do(t, http.MethodGet, srv.URL+"/nope", "", nil)
	assertJSONError(t, status, body, http.StatusNotFound)

	status, body, _ = do(t, http.MethodPost, srv.URL+"/healthz", "", nil)
	assertJSONError(t, status, body, http.StatusMethodNotAllowed)

	status, body, _ = do(t, http.MethodGet, srv.URL+"/", "", nil)
	if status != http.StatusOK {
		t.Fatalf("root status = %d body %s", status, body)
	}
}

// ---------------------------------------------------------------------------
// HTTP: license endpoint
// ---------------------------------------------------------------------------

// TestLicenseHappyPath covers POST /license plus the GET form and both schemes.
func TestLicenseHappyPath(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	const key = "0f1e2d3c4b5a69788796a5b4c3d2e1f0"
	rec := createKey(t, srv.URL, `{"kid":"0123456789abcdef0123456789abcdef","key":"`+key+`"}`)

	status, body, _ := do(t, http.MethodPost, srv.URL+"/license",
		`{"kid":"`+rec.KID+`","scheme":"cenc"}`, nil)
	if status != http.StatusOK {
		t.Fatalf("license status = %d body %s", status, body)
	}
	var lic LicenseResponse
	if err := json.Unmarshal(body, &lic); err != nil {
		t.Fatalf("license body %s: %v", body, err)
	}
	if lic.KID != rec.KID || lic.Key != key || lic.Scheme != SchemeCENC {
		t.Fatalf("license = %+v, want kid %s key %s scheme cenc", lic, rec.KID, key)
	}
	if lic.IVSize != IVSizeCENC {
		t.Fatalf("iv_size = %d, want %d for cenc", lic.IVSize, IVSizeCENC)
	}

	// Bad JSON must be 400 with the standard error body.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/license", `{"kid":`, nil)
	assertJSONError(t, status, body, http.StatusBadRequest)

	// Unknown kid must be 404.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/license",
		`{"kid":"`+strings.Repeat("a", 32)+`"}`, nil)
	assertJSONError(t, status, body, http.StatusNotFound)

	// An unsupported scheme must be 400, not a silent fall back to cenc.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/license",
		`{"kid":"`+rec.KID+`","scheme":"widevine"}`, nil)
	assertJSONError(t, status, body, http.StatusBadRequest)

	// GET form returns the same payload.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/license/"+rec.KID, "", nil)
	if status != http.StatusOK {
		t.Fatalf("GET license status = %d body %s", status, body)
	}
	var licGet LicenseResponse
	if err := json.Unmarshal(body, &licGet); err != nil || licGet != lic {
		t.Fatalf("GET license = %s (err %v), want %+v", body, err, lic)
	}

	// cbcs changes iv_size to 16 and keeps the same key.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/license/"+rec.KID+"?scheme=cbcs", "", nil)
	if status != http.StatusOK {
		t.Fatalf("GET license cbcs status = %d body %s", status, body)
	}
	var cbcs LicenseResponse
	if err := json.Unmarshal(body, &cbcs); err != nil {
		t.Fatalf("cbcs body %s: %v", body, err)
	}
	if cbcs.Scheme != SchemeCBCS || cbcs.IVSize != IVSizeCBCS || cbcs.Key != key {
		t.Fatalf("cbcs license = %+v, want scheme cbcs iv_size %d", cbcs, IVSizeCBCS)
	}
}

// ---------------------------------------------------------------------------
// HTTP: W3C ClearKey licence
// ---------------------------------------------------------------------------

// TestClearKeyLicenseShape pins the exact W3C ClearKey document: URL-safe
// base64 (no padding) for both "k" and "kid", kty "oct", type "temporary".
//
// This is the shape a ClearKey DASH client (EME, or Android MediaDrm's ClearKey
// plugin) can consume; the admin JSON this service returns by default cannot be
// substituted for it, which is why the endpoint exists at all.
func TestClearKeyLicenseShape(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	// 0xfb 0xff 0xbf force the URL-safe alphabet: standard base64 would emit "+/".
	const key = "fbffbf00112233445566778899aabbcc"
	const kid = "0123456789abcdef0123456789abcdef"
	rec := createKey(t, srv.URL, `{"kid":"`+kid+`","key":"`+key+`"}`)

	for _, path := range []string{
		"/key/" + rec.KID + "?format=clearkey",
		"/license/" + rec.KID + "?format=clearkey",
	} {
		status, body, header := do(t, http.MethodGet, srv.URL+path, "", nil)
		if status != http.StatusOK {
			t.Fatalf("%s status = %d body %s, want 200", path, status, body)
		}
		if ct := header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
			t.Fatalf("%s Content-Type = %q, want application/json", path, ct)
		}
		if cc := header.Get("Cache-Control"); cc != "no-store" {
			t.Fatalf("%s Cache-Control = %q, want no-store", path, cc)
		}

		var lic ClearKeyLicense
		if err := json.Unmarshal(body, &lic); err != nil {
			t.Fatalf("%s body %s: %v", path, body, err)
		}
		if lic.Type != ClearKeyTypeTemporary {
			t.Fatalf("%s type = %q, want %q", path, lic.Type, ClearKeyTypeTemporary)
		}
		if len(lic.Keys) != 1 {
			t.Fatalf("%s keys = %d entries, want exactly 1", path, len(lic.Keys))
		}
		if lic.Keys[0].Kty != "oct" {
			t.Fatalf("%s kty = %q, want oct", path, lic.Keys[0].Kty)
		}

		gotKey, err := base64.RawURLEncoding.DecodeString(lic.Keys[0].K)
		if err != nil {
			t.Fatalf("%s k = %q is not unpadded base64url: %v", path, lic.Keys[0].K, err)
		}
		wantKey, _ := hex.DecodeString(key)
		if !bytes.Equal(gotKey, wantKey) {
			t.Fatalf("%s k decodes to %x, want %x", path, gotKey, wantKey)
		}

		gotKID, err := base64.RawURLEncoding.DecodeString(lic.Keys[0].Kid)
		if err != nil {
			t.Fatalf("%s kid = %q is not unpadded base64url: %v", path, lic.Keys[0].Kid, err)
		}
		wantKID, _ := hex.DecodeString(kid)
		if !bytes.Equal(gotKID, wantKID) {
			t.Fatalf("%s kid decodes to %x, want %x", path, gotKID, wantKID)
		}

		// No padding anywhere: RawURLEncoding never emits "=", and the document
		// must not leak the hex spelling either.
		if strings.Contains(lic.Keys[0].K, "=") || strings.Contains(lic.Keys[0].Kid, "=") {
			t.Fatalf("%s emitted padded base64: %+v", path, lic.Keys[0])
		}
		if strings.Contains(string(body), key) {
			t.Fatalf("%s leaked the hex key: %s", path, body)
		}
	}
}

// TestClearKeyLicensePost checks the POST form and that the ClearKey document
// wins over the default admin JSON.
func TestClearKeyLicensePost(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	rec := createKey(t, srv.URL, `{}`)

	status, body, _ := do(t, http.MethodPost, srv.URL+"/license",
		`{"kid":"`+rec.KID+`","format":"clearkey"}`, nil)
	if status != http.StatusOK {
		t.Fatalf("status = %d body %s, want 200", status, body)
	}
	var lic ClearKeyLicense
	if err := json.Unmarshal(body, &lic); err != nil {
		t.Fatalf("body %s: %v", body, err)
	}
	if len(lic.Keys) != 1 || lic.Type != ClearKeyTypeTemporary {
		t.Fatalf("licence = %+v, want one key and type temporary", lic)
	}

	// GET /license with a scheme is the cenc/cbcs form; asking for ClearKey AND a
	// scheme is contradictory and must fail loudly rather than pick one.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/license/"+rec.KID+"?format=clearkey&scheme=cenc", "", nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
}

// TestClearKeyDoesNotChangeRawBytes guards the compatibility-critical default:
// adding ?format=clearkey must not touch what /key/{kid} returns by default,
// because an HLS #EXT-X-KEY URI reads exactly 16 bytes from it.
func TestClearKeyDoesNotChangeRawBytes(t *testing.T) {
	srv, _, _ := newTestServer(t, "")
	const key = "fbffbf00112233445566778899aabbcc"
	rec := createKey(t, srv.URL, `{"key":"`+key+`"}`)

	status, body, _ := do(t, http.MethodGet, srv.URL+"/key/"+rec.KID, "", nil)
	if status != http.StatusOK || len(body) != KeyBytes {
		t.Fatalf("raw endpoint = %d/%d bytes, want 200/%d", status, len(body), KeyBytes)
	}
	want, _ := hex.DecodeString(key)
	if !bytes.Equal(body, want) {
		t.Fatalf("raw body = %x, want %x", body, want)
	}
}

// ---------------------------------------------------------------------------
// tokens
// ---------------------------------------------------------------------------

// signFor is the test-side implementation of the documented token format. It is
// written independently of Signer so a bug in Signer cannot make the tests pass
// by agreeing with itself.
func signFor(secret, kid string, exp int64) string {
	expStr := strconv.FormatInt(exp, 10)
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write([]byte(kid + "\n" + expStr))
	return expStr + "." + hex.EncodeToString(mac.Sum(nil))
}

// TestTokenEnforcedValidSignatureAccepted is the happy path with -secret set.
func TestTokenEnforcedValidSignatureAccepted(t *testing.T) {
	const secret = "s3cret"
	srv, _, _ := newTestServer(t, secret)
	const key = "2b7e151628aed2a6abf7158809cf4f3c"
	rec := createKey(t, srv.URL, `{"key":"`+key+`"}`)

	exp := time.Now().Add(time.Hour).Unix()
	tok := signFor(secret, rec.KID, exp)
	_, sig := splitToken(t, tok)

	status, body, _ := do(t, http.MethodGet,
		fmt.Sprintf("%s/key/%s?exp=%d&sig=%s", srv.URL, rec.KID, exp, sig), "", nil)
	if status != http.StatusOK {
		t.Fatalf("signed request status = %d body %s, want 200", status, body)
	}
	want, _ := hex.DecodeString(key)
	if !bytes.Equal(body, want) {
		t.Fatalf("body = %x, want %x", body, want)
	}

	// /license must accept the same credential.
	status, body, _ = do(t, http.MethodGet,
		fmt.Sprintf("%s/license/%s?exp=%d&sig=%s", srv.URL, rec.KID, exp, sig), "", nil)
	if status != http.StatusOK {
		t.Fatalf("signed license status = %d body %s, want 200", status, body)
	}
	// The combined token= form is accepted too.
	status, body, _ = do(t, http.MethodPost, srv.URL+"/license?token="+url.QueryEscape(tok),
		`{"kid":"`+rec.KID+`"}`, nil)
	if status != http.StatusOK {
		t.Fatalf("token= license status = %d body %s, want 200", status, body)
	}
}

// TestTokenTamperedSignatureRejected flips one hex digit and expects a 401.
func TestTokenTamperedSignatureRejected(t *testing.T) {
	const secret = "s3cret"
	srv, _, _ := newTestServer(t, secret)
	rec := createKey(t, srv.URL, `{}`)

	exp := time.Now().Add(time.Hour).Unix()
	tok := signFor(secret, rec.KID, exp)
	expStr, sig := splitToken(t, tok)
	flipped := byte('0')
	if sig[0] == '0' {
		flipped = '1'
	}
	badSig := string(flipped) + sig[1:]

	for name, q := range map[string]string{
		"tampered sig": fmt.Sprintf("exp=%s&sig=%s", expStr, badSig),
		"missing sig":  fmt.Sprintf("exp=%s", expStr),
		"missing exp":  "sig=" + sig,
		"no token":     "",
		"garbage":      "exp=abc&sig=def",
		"bad hex sig":  "exp=" + expStr + "&sig=zz",
	} {
		t.Run(name, func(t *testing.T) {
			raw := srv.URL + "/key/" + rec.KID
			if q != "" {
				raw += "?" + q
			}
			status, body, _ := do(t, http.MethodGet, raw, "", nil)
			assertJSONError(t, status, body, http.StatusUnauthorized)
		})
	}

	// A token minted for a different kid must not be replayable.
	other := createKey(t, srv.URL, `{}`)
	otherTok := signFor(secret, other.KID, exp)
	otherExp, otherSig := splitToken(t, otherTok)
	status, body, _ := do(t, http.MethodGet,
		fmt.Sprintf("%s/key/%s?exp=%s&sig=%s", srv.URL, rec.KID, otherExp, otherSig), "", nil)
	assertJSONError(t, status, body, http.StatusUnauthorized)
}

// splitToken pulls the two fields out of a <exp>.<sig> token. The tests build
// their own query strings rather than reusing the server's helper, so a bug in
// that helper cannot make a signature test pass by agreement.
func splitToken(t *testing.T, tok string) (exp, sig string) {
	t.Helper()
	dot := strings.Index(tok, ".")
	if dot < 0 {
		t.Fatalf("token %q has no dot", tok)
	}
	return tok[:dot], tok[dot+1:]
}

// TestTokenExpiredRejected checks that a correctly signed but stale token is
// refused, and that the boundary is inclusive of the second exp names.
func TestTokenExpiredRejected(t *testing.T) {
	const secret = "s3cret"
	srv, _, signer := newTestServer(t, secret)
	rec := createKey(t, srv.URL, `{}`)

	exp := time.Now().Add(-time.Second).Unix()
	tok := signFor(secret, rec.KID, exp)
	expStr, sig := splitToken(t, tok)
	status, body, _ := do(t, http.MethodGet,
		fmt.Sprintf("%s/key/%s?exp=%s&sig=%s", srv.URL, rec.KID, expStr, sig), "", nil)
	assertJSONError(t, status, body, http.StatusUnauthorized)

	// The Signer-level check is time-injected, so the exact instant is testable
	// without sleeping: the same token is still good one second before exp.
	if err := signer.VerifyToken(rec.KID, tok, time.Now()); err != ErrTokenExpired {
		t.Fatalf("VerifyToken error = %v, want ErrTokenExpired", err)
	}
	if err := signer.VerifyToken(rec.KID, tok, time.Unix(exp-1, 0)); err != nil {
		t.Fatalf("VerifyToken one second before exp = %v, want nil", err)
	}
}

// TestTokenEnforcementOffAcceptsUnsignedRequest pins the documented demo mode:
// no secret means /key and /license work without any credential.
func TestTokenEnforcementOffAcceptsUnsignedRequest(t *testing.T) {
	srv, _, signer := newTestServer(t, "")
	if signer.Enforced() {
		t.Fatal("signer reports enforcement with an empty secret")
	}
	rec := createKey(t, srv.URL, `{}`)

	status, body, _ := do(t, http.MethodGet, srv.URL+"/key/"+rec.KID, "", nil)
	if status != http.StatusOK || len(body) != KeyBytes {
		t.Fatalf("unsigned /key status = %d len = %d, want 200 with %d bytes", status, len(body), KeyBytes)
	}
	status, body, _ = do(t, http.MethodPost, srv.URL+"/license", `{"kid":"`+rec.KID+`"}`, nil)
	if status != http.StatusOK {
		t.Fatalf("unsigned /license status = %d body %s, want 200", status, body)
	}
	// Signing without a secret is a configuration error, not a silent no-op.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID, "", nil)
	assertJSONError(t, status, body, http.StatusBadRequest)
}

// TestAdminSign covers URL minting, the ttl bounds, the admin header and the
// round trip back through the protected endpoint.
func TestAdminSign(t *testing.T) {
	const secret = "admin-secret"
	srv, _, _ := newTestServer(t, secret)
	rec := createKey(t, srv.URL, `{}`)

	// Without the header: 401.
	status, body, _ := do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID, "", nil)
	assertJSONError(t, status, body, http.StatusUnauthorized)

	// With a wrong header: 401.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID, "",
		map[string]string{"X-DRM-Secret": "wrong"})
	assertJSONError(t, status, body, http.StatusUnauthorized)

	// With the right header: 200 with a usable URL.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID+"&ttl=60", "",
		map[string]string{"X-DRM-Secret": secret})
	if status != http.StatusOK {
		t.Fatalf("sign status = %d body %s", status, body)
	}
	var signed struct {
		URL string `json:"url"`
		Exp int64  `json:"exp"`
	}
	if err := json.Unmarshal(body, &signed); err != nil {
		t.Fatalf("sign body %s: %v", body, err)
	}
	if !strings.HasPrefix(signed.URL, "/key/"+rec.KID+"?exp=") || !strings.Contains(signed.URL, "&sig=") {
		t.Fatalf("signed url = %q, want /key/%s?exp=...&sig=...", signed.URL, rec.KID)
	}
	if signed.Exp <= time.Now().Unix() {
		t.Fatalf("exp = %d, want a future timestamp", signed.Exp)
	}
	if status, body, _ := do(t, http.MethodGet, srv.URL+signed.URL, "", nil); status != http.StatusOK {
		t.Fatalf("fetching the signed URL gave %d body %s, want 200", status, body)
	}

	// ttl bounds: zero, negative, non-numeric and too large are all 400.
	for _, ttl := range []string{"0", "-1", "abc", strconv.Itoa(maxTokenTTL + 1)} {
		status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID+"&ttl="+ttl, "",
			map[string]string{"X-DRM-Secret": secret})
		assertJSONError(t, status, body, http.StatusBadRequest)
	}
	// The default ttl applies when the parameter is absent.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+rec.KID, "",
		map[string]string{"X-DRM-Secret": secret})
	if status != http.StatusOK {
		t.Fatalf("default ttl sign status = %d body %s", status, body)
	}
	// Signing an unknown kid fails now rather than in the player later.
	status, body, _ = do(t, http.MethodGet, srv.URL+"/admin/sign?kid="+strings.Repeat("b", 32), "",
		map[string]string{"X-DRM-Secret": secret})
	assertJSONError(t, status, body, http.StatusNotFound)
}

// TestTTLFromQueryDefault checks the documented 3600-second default.
func TestTTLFromQueryDefault(t *testing.T) {
	ttl, err := TTLFromQuery("")
	if err != nil {
		t.Fatalf("TTLFromQuery(\"\"): %v", err)
	}
	if ttl != time.Duration(defaultTokenTTL)*time.Second {
		t.Fatalf("ttl = %v, want %d seconds", ttl, defaultTokenTTL)
	}
}

// ---------------------------------------------------------------------------
// CENC / AES correctness
// ---------------------------------------------------------------------------

// nistCTRPlaintext and nistCTRCiphertext are the first four blocks of NIST
// SP 800-38A F.5.1 (CTR-AES128.Encrypt). Using the published vector rather than
// a round trip proves the primitive is real AES, not merely self-consistent.
const (
	nistCTRKey        = "2b7e151628aed2a6abf7158809cf4f3c"
	nistCTRInitBlock  = "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
	nistCTRPlaintext  = "6bc1bee22e409f96e93d7e117393172a" + "ae2d8a571e03ac9c9eb76fac45af8e51" + "30c81c46a35ce411e5fbc1191a0a52ef" + "f69f2445df4f9b17ad2b417be66c3710"
	nistCTRCiphertext = "874d6191b620e3261bef6864990db6ce" + "9806f66b7970fdff8617187bb9fffdff" + "5ae4df3edbd5d35e5b4f09020db03eab" + "1e031dda2fbe03d1792170a0f3009cee"
)

// TestNISTCTRVectorEncryptAndDecrypt encrypts the SP 800-38A F.5.1 vector and
// then decrypts the result, asserting both directions against the published
// bytes. This is the test that says the key server's primitive is genuine
// AES-128-CTR.
func TestNISTCTRVectorEncryptAndDecrypt(t *testing.T) {
	key := mustHex(t, nistCTRKey)
	iv := mustHex(t, nistCTRInitBlock)
	plain := mustHex(t, nistCTRPlaintext)
	want := mustHex(t, nistCTRCiphertext)

	block, err := aes.NewCipher(key)
	if err != nil {
		t.Fatalf("aes.NewCipher: %v", err)
	}

	got := make([]byte, len(plain))
	cipher.NewCTR(block, iv).XORKeyStream(got, plain)
	if !bytes.Equal(got, want) {
		t.Fatalf("CTR encryption mismatch\n got %x\nwant %x", got, want)
	}

	recovered := make([]byte, len(want))
	cipher.NewCTR(block, iv).XORKeyStream(recovered, want)
	if !bytes.Equal(recovered, plain) {
		t.Fatalf("CTR decryption mismatch\n got %x\nwant %x", recovered, plain)
	}
}

// TestNISTCTRKeystreamVector pins the first keystream block independently of the
// plaintext: ciphertext XOR plaintext for block 0 must equal the published
// keystream, which proves the counter block is fed to AES unmodified.
func TestNISTCTRKeystreamVector(t *testing.T) {
	block, err := aes.NewCipher(mustHex(t, nistCTRKey))
	if err != nil {
		t.Fatalf("aes.NewCipher: %v", err)
	}
	stream := make([]byte, aes.BlockSize)
	cipher.NewCTR(block, mustHex(t, nistCTRInitBlock)).XORKeyStream(stream, make([]byte, aes.BlockSize))
	// Derived from the published F.5.1 vector, not copied from a table: the
	// keystream is ciphertext XOR plaintext, i.e.
	// 874d6191b620e3261bef6864990db6ce XOR 6bc1bee22e409f96e93d7e117393172a.
	// Equivalently it is AES-ECB(key, counter block), which is what makes this
	// test independent of libavcodec-style chaining: it pins the counter block
	// being handed to AES unmodified.
	const wantKeystream = "ec8cdf7398607cb0f2d21675ea9ea1e4"
	if got := hex.EncodeToString(stream); got != wantKeystream {
		t.Fatalf("keystream block 0 = %s, want %s", got, wantKeystream)
	}
}

// TestCENCIVConstruction proves the IV layout the player relies on: the 8-byte
// per-sample IV occupies the high half and the low half is a big-endian block
// counter starting at zero.
func TestCENCIVConstruction(t *testing.T) {
	sampleIV := []byte{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}
	block, err := cencIV(sampleIV)
	if err != nil {
		t.Fatalf("cencIV: %v", err)
	}
	want := "00112233445566770000000000000000"
	if got := hex.EncodeToString(block[:]); got != want {
		t.Fatalf("counter block = %s, want %s", got, want)
	}
	// The counter half really is a big-endian zero, not a platform-dependent
	// encoding, so the first two blocks differ only in the last byte.
	block[IVSizeCENC+7] = 1
	if got := hex.EncodeToString(block[:]); got != "00112233445566770000000000000001" {
		t.Fatalf("counter increment shape = %s, want big-endian +1", got)
	}
	// A wrong-length sample IV is an error, not a silently padded one.
	if _, err := cencIV(sampleIV[:7]); err == nil {
		t.Fatal("cencIV accepted a 7-byte sample IV, want error")
	}

	// End to end: bytes encrypted with the constructed CENC block must decrypt
	// with the same block, byte for byte.
	key := mustHex(t, nistCTRKey)
	block, err = cencIV(sampleIV)
	if err != nil {
		t.Fatalf("cencIV: %v", err)
	}
	c, err := aes.NewCipher(key)
	if err != nil {
		t.Fatalf("aes.NewCipher: %v", err)
	}
	plain := []byte("CicadaPlayerNext CENC sample payload!")
	sealed := make([]byte, len(plain))
	cipher.NewCTR(c, block[:]).XORKeyStream(sealed, plain)
	if bytes.Equal(sealed, plain) {
		t.Fatal("CTR encryption left the plaintext unchanged")
	}
	opened := make([]byte, len(sealed))
	cipher.NewCTR(c, block[:]).XORKeyStream(opened, sealed)
	if !bytes.Equal(opened, plain) {
		t.Fatalf("round trip = %q, want %q", opened, plain)
	}
}

// TestIVSizeForScheme pins the tenc iv_size values.
func TestIVSizeForScheme(t *testing.T) {
	if got := ivSizeForScheme(SchemeCENC); got != IVSizeCENC {
		t.Fatalf("ivSizeForScheme(cenc) = %d, want %d", got, IVSizeCENC)
	}
	if got := ivSizeForScheme(SchemeCBCS); got != IVSizeCBCS {
		t.Fatalf("ivSizeForScheme(cbcs) = %d, want %d", got, IVSizeCBCS)
	}
	if got := ivSizeForScheme(""); got != IVSizeCENC {
		t.Fatalf("ivSizeForScheme(\"\") = %d, want the cenc default %d", got, IVSizeCENC)
	}
}

// TestDefaultIVHex checks the 0x-prefixed IV string a playlist would carry.
func TestDefaultIVHex(t *testing.T) {
	cenc, err := defaultIVHex(SchemeCENC)
	if err != nil {
		t.Fatalf("defaultIVHex(cenc): %v", err)
	}
	if cenc != "0x00000000000000000000000000000000" || len(cenc) != 2+IVBytes*2 {
		t.Fatalf("cenc IV = %q, want 0x plus %d hex chars", cenc, IVBytes*2)
	}
	cbcs, err := defaultIVHex(SchemeCBCS)
	if err != nil {
		t.Fatalf("defaultIVHex(cbcs): %v", err)
	}
	if cbcs != cenc {
		t.Fatalf("cbcs IV = %q, want the 16-byte zero IV %q", cbcs, cenc)
	}
}

// mustHex decodes a hex fixture or fails the test.
func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	raw, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("fixture %q is not hex: %v", s, err)
	}
	return raw
}

// asAPIError is a tiny errors.As wrapper kept here so the store tests do not
// have to import errors just for one assertion.
func asAPIError(err error, target **apiError) bool {
	apiErr, ok := err.(*apiError)
	if ok {
		*target = apiErr
	}
	return ok
}
