package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// maxBodyBytes caps request bodies. /admin/keys and /license take tiny JSON
// documents, so anything larger is a mistake or an attack and is refused before
// it is buffered.
const maxBodyBytes = 64 * 1024

// Server holds the wiring shared by every handler. It is a struct rather than a
// pile of globals so tests can build one per case without touching process
// state.
type Server struct {
	store  *KeyStore
	signer *Signer

	// now is injectable so token-expiry tests do not have to sleep.
	now func() time.Time
}

// NewServer wires a store and a signer into a Server.
func NewServer(store *KeyStore, signer *Signer) *Server {
	return &Server{store: store, signer: signer, now: time.Now}
}

// Routes returns the handler for the whole service.
//
// Dispatch is done by hand rather than with the Go 1.22 "METHOD /path/{wild}"
// patterns on purpose: the module targets Go 1.21, where ServeMux treats those
// strings as literal paths, so using them would compile today and silently 404
// everywhere on the older toolchain. Manual dispatch also guarantees that an
// unknown path gets a JSON 404 instead of net/http's empty plain-text reply.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.dispatch)
	return mux
}

// dispatch routes one request to its handler.
func (s *Server) dispatch(w http.ResponseWriter, r *http.Request) {
	// methodNotAllowed helps the common "wrong verb" mistake produce a clear
	// 405 instead of a misleading 404.
	methodNotAllowed := func() {
		writeError(w, http.StatusMethodNotAllowed, "method %s not allowed for %s", r.Method, r.URL.Path)
	}
	// kidFromPath extracts the single path segment after prefix, rejecting
	// anything deeper so /key/<kid>/extra is not silently treated as /key/<kid>.
	kidFromPath := func(prefix string) (string, bool) {
		rest := strings.TrimPrefix(r.URL.Path, prefix)
		if rest == "" || strings.Contains(rest, "/") {
			return "", false
		}
		kid, err := url.PathUnescape(rest)
		if err != nil {
			return "", false
		}
		return kid, true
	}

	switch {
	case r.URL.Path == "/":
		if r.Method != http.MethodGet {
			methodNotAllowed()
			return
		}
		s.handleRoot(w, r)
	case r.URL.Path == "/healthz":
		if r.Method != http.MethodGet {
			methodNotAllowed()
			return
		}
		s.handleHealth(w, r)
	case r.URL.Path == "/admin/keys":
		switch r.Method {
		case http.MethodGet:
			s.handleListKeys(w, r)
		case http.MethodPost:
			s.handleCreateKey(w, r)
		default:
			methodNotAllowed()
		}
	case r.URL.Path == "/admin/sign":
		if r.Method != http.MethodGet {
			methodNotAllowed()
			return
		}
		s.handleSign(w, r)
	case r.URL.Path == "/license":
		if r.Method != http.MethodPost {
			methodNotAllowed()
			return
		}
		s.handleLicensePost(w, r)
	case strings.HasPrefix(r.URL.Path, "/admin/keys/"):
		kid, ok := kidFromPath("/admin/keys/")
		if !ok {
			s.handleNotFound(w, r)
			return
		}
		switch r.Method {
		case http.MethodGet:
			s.handleGetKey(w, r, kid)
		case http.MethodDelete:
			s.handleDeleteKey(w, r, kid)
		default:
			methodNotAllowed()
		}
	case strings.HasPrefix(r.URL.Path, "/key/"):
		kid, ok := kidFromPath("/key/")
		if !ok {
			s.handleNotFound(w, r)
			return
		}
		if r.Method != http.MethodGet {
			methodNotAllowed()
			return
		}
		s.handleKey(w, r, kid)
	case strings.HasPrefix(r.URL.Path, "/license/"):
		kid, ok := kidFromPath("/license/")
		if !ok {
			s.handleNotFound(w, r)
			return
		}
		if r.Method != http.MethodGet {
			methodNotAllowed()
			return
		}
		s.handleLicenseGet(w, r, kid)
	default:
		s.handleNotFound(w, r)
	}
}

// handleRoot answers the bare origin with a pointer to /healthz. Without it a
// browser hitting the listen address would get a 404 that looks like the server
// is down.
func (s *Server) handleRoot(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"service":   "drm-keyserver",
		"status":    "ok",
		"healthz":   "/healthz",
		"endpoints": []string{"/healthz", "/admin/keys", "/admin/sign", "/key/{kid}", "/license"},
	})
}

// handleNotFound is the catch-all. It exists so every 404 in the service has the
// same {"error":"..."} shape as every other failure; net/http's default reply
// is plain text with an empty body, which clients cannot parse uniformly.
func (s *Server) handleNotFound(w http.ResponseWriter, r *http.Request) {
	writeError(w, http.StatusNotFound, "no route for %s %s", r.Method, r.URL.Path)
}

// handleHealth reports liveness plus the key count, so an operator can confirm
// the store actually loaded something without reading the data dir.
func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"status": "ok",
		"keys":   s.store.Count(),
	})
}

// handleListKeys serves the admin listing, key material included. It is
// deliberately not redacted: the transcoding script needs the bytes, and
// redacting here would only move the secret to a different endpoint. Exposure is
// handled by binding to a trusted interface and by the token on /key.
func (s *Server) handleListKeys(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.store.List())
}

// handleGetKey serves a single admin record, key material included.
func (s *Server) handleGetKey(w http.ResponseWriter, r *http.Request, kid string) {
	rec, err := s.store.Lookup(kid)
	if err != nil {
		writeAPIError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, rec)
}

// handleCreateKey creates a key. The body is optional: `curl -X POST
// /admin/keys` with no body must work, because that is the one-liner in the
// README. An empty body therefore means "generate everything".
func (s *Server) handleCreateKey(w http.ResponseWriter, r *http.Request) {
	var req KeyRecord
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBodyBytes))
	if err != nil {
		writeError(w, http.StatusBadRequest, "cannot read request body: %v", err)
		return
	}
	if len(strings.TrimSpace(string(body))) > 0 {
		dec := json.NewDecoder(strings.NewReader(string(body)))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&req); err != nil {
			writeError(w, http.StatusBadRequest, "malformed JSON body: %v", err)
			return
		}
	}
	rec, err := s.store.Create(req)
	if err != nil {
		writeAPIError(w, err)
		return
	}
	// Log the identifier, never the key material.
	log.Printf("created key kid=%s label=%q scheme=%s", rec.KID, rec.Label, normalizeScheme(rec.Scheme))
	writeJSON(w, http.StatusCreated, rec)
}

// handleDeleteKey removes a key and reports it with 204. Revocation only takes
// effect on the next fetch: a client that already has the key keeps it, which
// the README states plainly.
func (s *Server) handleDeleteKey(w http.ResponseWriter, r *http.Request, kid string) {
	if err := s.store.Delete(kid); err != nil {
		writeAPIError(w, err)
		return
	}
	log.Printf("deleted key kid=%s", kid)
	w.WriteHeader(http.StatusNoContent)
}

// handleSign mints a protected URL for a kid. The admin secret travels in the
// X-DRM-Secret header, not in the query string, so it does not end up in access
// logs or shell history.
func (s *Server) handleSign(w http.ResponseWriter, r *http.Request) {
	if err := s.signer.requireSecret(r.Header.Get("X-DRM-Secret")); err != nil {
		writeAPIError(w, err)
		return
	}
	kid := r.URL.Query().Get("kid")
	if err := validateKID(kid); err != nil {
		writeError(w, http.StatusBadRequest, "malformed kid: %s", err)
		return
	}
	ttl, err := TTLFromQuery(r.URL.Query().Get("ttl"))
	if err != nil {
		writeAPIError(w, err)
		return
	}
	// A signed URL for a kid that does not exist would 404 at fetch time. Fail
	// now, while the operator is watching, instead of leaving a broken playlist.
	if !s.store.KeyIDExists(kid) {
		writeError(w, http.StatusNotFound, "unknown kid %s", kid)
		return
	}
	base := ""
	if r.URL.Query().Get("absolute") == "1" {
		base = requestBaseURL(r)
	}
	signed, exp, err := s.signer.SignedURL(base, kid, ttl)
	if err != nil {
		writeAPIError(w, err)
		return
	}
	log.Printf("signed url kid=%s exp=%d", kid, exp)
	writeJSON(w, http.StatusOK, map[string]any{"url": signed, "exp": exp})
}

// handleKey delivers key material to the player.
//
// The default representation is the raw 16 bytes, because that is what an HLS
// #EXT-X-KEY URI must serve and what CicadaPlayerNext's HLSStream::updateKey()
// expects: it opens the URL and reads exactly 16 bytes. Changing this default
// would silently break playback, so ?format=json is opt-in.
func (s *Server) handleKey(w http.ResponseWriter, r *http.Request, kid string) {
	if err := validateKID(kid); err != nil {
		writeError(w, http.StatusBadRequest, "malformed kid: %s", err)
		return
	}
	if err := s.signer.VerifyRequest(kid, r.URL.Query(), s.now()); err != nil {
		writeError(w, http.StatusUnauthorized, "%s", tokenErrMessage(err))
		return
	}
	rec, err := s.store.Lookup(kid)
	if err != nil {
		writeAPIError(w, err)
		return
	}
	// Keys must never sit in a shared cache or a proxy.
	w.Header().Set("Cache-Control", "no-store")

	format := r.URL.Query().Get("format")
	switch {
	case isClearKeyFormat(format):
		// W3C ClearKey licence. This is the shape an EME/MediaDrm ClearKey
		// client (and the kernel's ContentKeyFetcher) can actually consume;
		// see clearkey.go for why the admin JSON cannot be substituted here.
		payload, err := buildClearKeyLicense(rec)
		if err != nil {
			writeAPIError(w, err)
			return
		}
		if scheme := r.URL.Query().Get("scheme"); scheme != "" {
			writeAPIError(w, clearkeyUnsupportedScheme(scheme))
			return
		}
		log.Printf("served clearkey licence kid=%s", kid)
		writeJSON(w, http.StatusOK, payload)
	case format == "", format == "bin", format == "raw":
		raw, err := decodeHexKey(rec.Key)
		if err != nil {
			writeAPIError(w, err)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", "16")
		log.Printf("served key kid=%s format=raw", kid)
		if _, err := w.Write(raw); err != nil {
			// The status is already committed; there is nothing left to change
			// but the log line.
			log.Printf("write key kid=%s: %v", kid, err)
		}
	case format == "json":
		payload, err := buildKeyJSON(rec, r.URL.Query().Get("scheme"))
		if err != nil {
			writeAPIError(w, err)
			return
		}
		log.Printf("served key kid=%s format=json scheme=%s", kid, payload.Scheme)
		writeJSON(w, http.StatusOK, payload)
	default:
		writeError(w, http.StatusBadRequest,
			"unknown format %q: want raw (default), json or clearkey", format)
	}
}

// handleLicensePost is the CENC license call: a decrypter that has just parsed
// a tenc/senc box posts the KID and gets the key back.
func (s *Server) handleLicensePost(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxBodyBytes))
	if err != nil {
		writeError(w, http.StatusBadRequest, "cannot read request body: %v", err)
		return
	}
	var req LicenseRequest
	dec := json.NewDecoder(strings.NewReader(string(body)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "malformed JSON body: %v", err)
		return
	}
	if err := s.signer.VerifyRequest(req.KID, r.URL.Query(), s.now()); err != nil {
		writeError(w, http.StatusUnauthorized, "%s", tokenErrMessage(err))
		return
	}
	s.writeLicense(w, req.KID, req.Scheme, req.Format)
}

// handleLicenseGet is the same license in GET form. It is convenient for a
// browser, for the transcoder's own checks, and for a client that would rather
// put the KID in the path than in a body.
func (s *Server) handleLicenseGet(w http.ResponseWriter, r *http.Request, kid string) {
	if err := s.signer.VerifyRequest(kid, r.URL.Query(), s.now()); err != nil {
		writeError(w, http.StatusUnauthorized, "%s", tokenErrMessage(err))
		return
	}
	s.writeLicense(w, kid, r.URL.Query().Get("scheme"), r.URL.Query().Get("format"))
}

// writeLicense is the shared tail of both /license forms.
//
// format selects the shape: empty/"json" is this service's own licence
// {"kid","key","scheme","iv_size"}, "clearkey" is the W3C ClearKey document that
// a ClearKey DASH client expects (see clearkey.go).
func (s *Server) writeLicense(w http.ResponseWriter, kid, scheme, format string) {
	rec, err := s.store.Lookup(kid)
	if err != nil {
		writeAPIError(w, err)
		return
	}

	if isClearKeyFormat(format) {
		if scheme != "" {
			writeAPIError(w, clearkeyUnsupportedScheme(scheme))
			return
		}
		payload, err := buildClearKeyLicense(rec)
		if err != nil {
			writeAPIError(w, err)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		log.Printf("issued clearkey licence kid=%s", payload.Keys[0].Kid)
		writeJSON(w, http.StatusOK, payload)
		return
	}

	payload, err := buildLicense(rec, scheme)
	if err != nil {
		writeAPIError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	log.Printf("issued license kid=%s scheme=%s iv_size=%d", payload.KID, payload.Scheme, payload.IVSize)
	writeJSON(w, http.StatusOK, payload)
}

// requestBaseURL reconstructs the scheme://host prefix for an absolute signed
// URL. It trusts the Host header, which is why absolute URLs are opt-in: with
// enforcement on, a spoofed Host cannot leak a key (the signature is still
// bound to the real kid and a valid exp), it can only produce a URL that points
// somewhere the operator did not intend.
func requestBaseURL(r *http.Request) string {
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	if proto := r.Header.Get("X-Forwarded-Proto"); proto != "" {
		scheme = proto
	}
	return scheme + "://" + r.Host
}

// writeJSON writes v as the response body with the given status. It marshals
// before touching the ResponseWriter so an encoding failure can still become a
// 500 instead of a truncated 200.
func writeJSON(w http.ResponseWriter, status int, v any) {
	raw, err := json.Marshal(v)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "encode response: %v", err)
		return
	}
	raw = append(raw, '\n')
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	if _, err := w.Write(raw); err != nil {
		log.Printf("write response: %v", err)
	}
}

// writeError is the only way an error leaves this service: an explicit status
// plus {"error":"..."}. There is no path that returns a bare status or an empty
// 200 body.
func writeError(w http.ResponseWriter, status int, format string, args ...any) {
	msg := format
	if len(args) > 0 {
		msg = fmt.Sprintf(format, args...)
	}
	writeJSON(w, status, map[string]string{"error": msg})
}

// writeAPIError maps an error onto its status. An error that is not an
// *apiError is a bug or an I/O failure and becomes a 500 with a generic
// message, so internal details (for example a filesystem path) are not echoed
// to clients by accident.
func writeAPIError(w http.ResponseWriter, err error) {
	var apiErr *apiError
	if errors.As(err, &apiErr) {
		writeError(w, apiErr.Status, "%s", apiErr.Msg)
		return
	}
	log.Printf("internal error: %v", err)
	writeError(w, http.StatusInternalServerError, "internal error")
}
