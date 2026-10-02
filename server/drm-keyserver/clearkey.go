package main

import (
	"encoding/base64"
	"encoding/hex"
	"net/http"
)

// ============================================================================
// W3C ClearKey licence
// ============================================================================
//
// Why this exists: "ClearKey" is one of the content protection systems DASH
// registers (urn:uuid:e2719d58-a985-b3c9-781a-b030af78d30e), and its licence
// format is NOT this server's own JSON. W3C Encrypted Media Extensions defines
// it as:
//
//	{
//	  "keys": [ { "kty": "oct", "k": "<base64url key>", "kid": "<base64url kid>" } ],
//	  "type": "temporary"
//	}
//
// Both fields use the **URL-safe** base64 alphabet without padding
// (RFC 4648 §5), and the KID is the raw 16 bytes — not the hex spelling this
// store uses internally. A client that expects ClearKey and receives our admin
// JSON ({"kid":"<hex>","key":"<hex>",...}) cannot parse it, so serving the
// admin shape at a ClearKey laurl would make ClearKey DASH simply not work.
//
// The raw 16-byte endpoint (/key/{kid}) is deliberately unchanged: an HLS
// #EXT-X-KEY URI must return exactly 16 bytes, and ClearKey is opt-in via
// ?format=clearkey.

// ClearKeyKey is one entry of the "keys" array.
type ClearKeyKey struct {
	Kty string `json:"kty"`
	K   string `json:"k"`
	Kid string `json:"kid"`
}

// ClearKeyLicense is the whole licence document.
type ClearKeyLicense struct {
	Keys []ClearKeyKey `json:"keys"`
	Type string        `json:"type"`
}

// ClearKeyTypeTemporary is the only licence type a stateless key server can
// honestly issue: the keys are not persisted on the client.
const ClearKeyTypeTemporary = "temporary"

// FormatClearKey is the ?format= value that selects this licence shape.
const FormatClearKey = "clearkey"

// base64URL encodes 16 raw bytes the way EME ClearKey requires: URL-safe
// alphabet, no padding.
func base64URL(raw []byte) string {
	return base64.RawURLEncoding.EncodeToString(raw)
}

// buildClearKeyLicense renders rec as a W3C ClearKey licence.
//
// The KID and the key are both re-derived from hex here (never copied as text)
// so a malformed record becomes an explicit 500 instead of a licence the
// client silently fails to use.
func buildClearKeyLicense(rec KeyRecord) (ClearKeyLicense, error) {
	rawKey, err := decodeHexKey(rec.Key)
	if err != nil {
		return ClearKeyLicense{}, err
	}
	rawKID, err := hex.DecodeString(rec.KID)
	if err != nil || len(rawKID) != KeyBytes {
		// validateKID already guarantees this shape on the way in, so reaching
		// here means the on-disk file was edited by hand.
		return ClearKeyLicense{}, newAPIError(http.StatusInternalServerError,
			"stored kid for %s is not %d hex bytes", rec.KID, KeyBytes)
	}
	return ClearKeyLicense{
		Keys: []ClearKeyKey{{
			Kty: "oct",
			K:   base64URL(rawKey),
			Kid: base64URL(rawKID),
		}},
		Type: ClearKeyTypeTemporary,
	}, nil
}

// isClearKeyFormat reports whether format selects the ClearKey licence.
// An empty format is NOT ClearKey: the existing callers rely on the default
// staying the raw bytes / admin JSON.
func isClearKeyFormat(format string) bool {
	switch format {
	case FormatClearKey, "w3c-clearkey", "clearkey-json":
		return true
	default:
		return false
	}
}

// clearkeyUnsupportedScheme keeps the error text in one place: the ClearKey
// licence carries no notion of cenc vs cbcs, so a caller that asked for one of
// those explicitly is making a mistake worth naming.
func clearkeyUnsupportedScheme(scheme string) error {
	return newAPIError(http.StatusBadRequest,
		"the ClearKey licence has no encryption scheme: drop scheme=%s (it only applies to format=json)",
		scheme)
}
