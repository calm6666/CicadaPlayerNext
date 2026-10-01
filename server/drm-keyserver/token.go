package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// Token lifetime bounds. The token is a credential with a fixed expiry, not a
// retry or refresh mechanism: once exp passes, the signature is worthless. The
// upper bound keeps a leaked URL from being a permanent grant.
const (
	maxTokenTTL = 86400 * 365
)

// defaultTokenTTL is the lifetime used when a caller omits ?ttl=. It is a
// variable because the -key-ttl flag sets the default for the process; the flag
// carries a value, so there is still exactly one code path and no switch.
var defaultTokenTTL = 3600

// Token errors are sentinel values so handlers can distinguish "no credential"
// (401 with a hint) from "bad credential" (401 without one) without string
// matching.
var (
	// ErrTokenMissing means no token was supplied at all.
	ErrTokenMissing = errors.New("token required")
	// ErrTokenMalformed means the token was present but not <exp>.<sig>.
	ErrTokenMalformed = errors.New("malformed token")
	// ErrTokenExpired means the signature was valid but exp is in the past.
	ErrTokenExpired = errors.New("token expired")
	// ErrTokenSignature means the signature did not match.
	ErrTokenSignature = errors.New("token signature mismatch")
)

// Signer issues and verifies the URL credentials.
//
// Enforcement is derived from the secret, never from a boolean switch: an empty
// secret means the operator deliberately ran the demo without one, and every
// token check then passes. A non-empty secret means every protected request
// must carry a valid signature. There is no third state and no way to disable
// enforcement while a secret is configured.
type Signer struct {
	secret []byte
}

// NewSigner returns a Signer for secret. An empty secret yields a pass-through
// signer; callers should log that state loudly at startup.
func NewSigner(secret string) *Signer {
	if secret == "" {
		return &Signer{}
	}
	return &Signer{secret: []byte(secret)}
}

// Enforced reports whether token verification is active.
func (s *Signer) Enforced() bool { return len(s.secret) > 0 }

// Sign returns the token <exp>.<sig> for kid, where sig is
// hex(HMAC-SHA256(secret, kid + "\n" + exp)).
//
// The kid is bound into the signature so a token for one key cannot be replayed
// against another. The exp value is signed as the exact decimal string that
// appears in the URL, which keeps verification a byte comparison with no
// re-formatting step to get wrong.
func (s *Signer) Sign(kid string, exp int64) (string, error) {
	if err := validateKID(kid); err != nil {
		return "", newAPIError(http.StatusBadRequest, "malformed kid: %s", err)
	}
	if !s.Enforced() {
		return "", newAPIError(http.StatusBadRequest,
			"signing requires -secret; no secret is configured on this server")
	}
	if exp <= 0 {
		return "", newAPIError(http.StatusBadRequest, "exp must be a positive unix timestamp")
	}
	expStr := strconv.FormatInt(exp, 10)
	return expStr + "." + s.mac(kid, expStr), nil
}

// SignedURL builds the full protected URL for kid with the given lifetime.
//
// ttl is a duration in seconds, not a switch: it must be present and positive,
// and it is capped so no single URL can outlive maxTokenTTL. The returned path
// already contains the query string, so it can be dropped into an
// #EXT-X-KEY URI verbatim (HLS allows a query string in URI="").
func (s *Signer) SignedURL(baseURL, kid string, ttl time.Duration) (string, int64, error) {
	if !s.Enforced() {
		return "", 0, newAPIError(400,
			"signing requires -secret; no secret is configured on this server")
	}
	if ttl <= 0 {
		return "", 0, newAPIError(400, "ttl must be a positive number of seconds")
	}
	if ttl > maxTokenTTL*time.Second {
		return "", 0, newAPIError(400, "ttl must not exceed %d seconds", maxTokenTTL)
	}
	exp := time.Now().Add(ttl).Unix()
	token, err := s.Sign(kid, exp)
	if err != nil {
		return "", 0, err
	}
	path := "/key/" + kid + "?exp=" + strconv.FormatInt(exp, 10) +
		"&sig=" + strings.TrimPrefix(token, strconv.FormatInt(exp, 10)+".")
	if baseURL == "" {
		return path, exp, nil
	}
	return strings.TrimRight(baseURL, "/") + path, exp, nil
}

// VerifyToken checks a token against kid and the current time. now is injected
// so tests do not have to sleep.
//
// Comparison uses hmac.Equal, which is constant time for equal-length inputs,
// so a wrong signature leaks nothing about how wrong it was.
func (s *Signer) VerifyToken(kid, token string, now time.Time) error {
	if !s.Enforced() {
		return nil
	}
	if token == "" {
		return ErrTokenMissing
	}
	expStr, sig, ok := strings.Cut(token, ".")
	if !ok || expStr == "" || sig == "" || strings.Contains(sig, ".") {
		return ErrTokenMalformed
	}
	exp, err := strconv.ParseInt(expStr, 10, 64)
	if err != nil {
		return ErrTokenMalformed
	}
	want := s.mac(kid, expStr)
	got, err := hex.DecodeString(sig)
	if err != nil {
		return ErrTokenMalformed
	}
	wantRaw, err := hex.DecodeString(want)
	if err != nil {
		return ErrTokenMalformed
	}
	if !hmac.Equal(wantRaw, got) {
		return ErrTokenSignature
	}
	// Expiry is checked after the signature on purpose: an attacker cannot use
	// error-timing to learn whether a guessed exp was plausible.
	if now.Unix() >= exp {
		return ErrTokenExpired
	}
	return nil
}

// VerifyRequest pulls the token out of a request URL and verifies it. exp and
// sig are read as separate query parameters (the shape /admin/sign returns), and
// a single token= parameter carrying the combined <exp>.<sig> form is accepted
// too, so a proxy that rewrites the query does not silently break playback.
func (s *Signer) VerifyRequest(kid string, q url.Values, now time.Time) error {
	if !s.Enforced() {
		return nil
	}
	if combined := q.Get("token"); combined != "" {
		return s.VerifyToken(kid, combined, now)
	}
	expStr := q.Get("exp")
	sig := q.Get("sig")
	if expStr == "" || sig == "" {
		return ErrTokenMissing
	}
	return s.VerifyToken(kid, expStr+"."+sig, now)
}

// mac computes the hex HMAC for one (kid, exp) pair.
func (s *Signer) mac(kid, expStr string) string {
	m := hmac.New(sha256.New, s.secret)
	// Write cannot fail for a hash.Hash, and the payload is fixed by the
	// documented token format, so the error is intentionally ignored.
	_, _ = m.Write([]byte(kid + "\n" + expStr))
	return hex.EncodeToString(m.Sum(nil))
}

// secretMatches compares a presented admin secret against the configured one in
// constant time. It returns true when no secret is configured, matching the
// pass-through behaviour of the rest of the token layer.
func (s *Signer) secretMatches(presented string) bool {
	if !s.Enforced() {
		return true
	}
	return hmac.Equal([]byte(presented), s.secret)
}

// requireSecret is the shared guard for the admin signing surface.
func (s *Signer) requireSecret(presented string) error {
	if !s.Enforced() {
		return nil
	}
	if presented == "" {
		return newAPIError(http.StatusUnauthorized, "missing X-DRM-Secret header")
	}
	if !s.secretMatches(presented) {
		return newAPIError(http.StatusUnauthorized, "bad X-DRM-Secret header")
	}
	return nil
}

// TTLFromQuery parses a ttl parameter, applying the documented default and
// range. An absent parameter is not an error; a malformed one is, because
// silently substituting the default would hand out a URL with a lifetime the
// caller did not ask for.
func TTLFromQuery(ttlParam string) (time.Duration, error) {
	if ttlParam == "" {
		return time.Duration(defaultTokenTTL) * time.Second, nil
	}
	secs, err := strconv.ParseInt(ttlParam, 10, 64)
	if err != nil {
		return 0, newAPIError(http.StatusBadRequest, "ttl must be an integer number of seconds")
	}
	if secs <= 0 {
		return 0, newAPIError(http.StatusBadRequest, "ttl must be positive")
	}
	if secs > maxTokenTTL {
		return 0, newAPIError(http.StatusBadRequest, "ttl must not exceed %d seconds", maxTokenTTL)
	}
	return time.Duration(secs) * time.Second, nil
}

// tokenErrMessage maps a token error onto a client-safe message. It never
// echoes the presented signature.
func tokenErrMessage(err error) string {
	switch {
	case errors.Is(err, ErrTokenMissing):
		return "token required: add ?exp=<unix>&sig=<hex>, or use /admin/sign"
	case errors.Is(err, ErrTokenExpired):
		return "token expired"
	case errors.Is(err, ErrTokenSignature):
		return "token signature mismatch"
	case errors.Is(err, ErrTokenMalformed):
		return "malformed token: want <exp>.<sig> where sig is hex(HMAC-SHA256(secret, kid + newline + exp))"
	default:
		return "token rejected"
	}
}
