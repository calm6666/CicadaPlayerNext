package main

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"net/http"
)

// CENC constants.
//
// KeyBytes is 16 because every scheme this server speaks (HLS AES-128, CENC
// cenc, CENC cbcs) uses AES-128. IVBytes is the CENC per-sample IV size: 8
// bytes for 'cenc' (AES-CTR with a 64-bit block counter) and 16 for 'cbcs'
// (AES-CBC with a constant IV).
const (
	KeyBytes = 16
	IVBytes  = 16

	// SchemeCENC is MPEG Common Encryption 'cenc': AES-CTR, iv_size 8.
	// SchemeCBCS is 'cbcs': AES-CBC with pattern encryption, iv_size 16.
	SchemeCENC = "cenc"
	SchemeCBCS = "cbcs"

	// IVSizeCENC and IVSizeCBCS are the iv_size values that belong in a tenc
	// box for each scheme. A player that finds the wrong value here will
	// mis-parse senc and fail on the first sample.
	IVSizeCENC = 8
	IVSizeCBCS = 16
)

// cencIVBlock is the 16-byte counter block CENC feeds to AES-CTR. It is a named
// array type so the length shows up as "16" in every signature and a caller
// cannot confuse it with the 8-byte per-sample IV.
type cencIVBlock [IVBytes]byte

// cencIV builds the 16-byte counter block CENC feeds to AES-CTR for one sample.
//
// Layout is fixed by the spec (ISO/IEC 23001-7): the per-sample IV occupies the
// high 8 bytes and the remaining 8 bytes are a big-endian block counter that
// starts at zero for the first block of the sample. Keeping this in one place
// means the server and any player-side decrypter agree by construction rather
// than by convention.
func cencIV(sampleIV []byte) (cencIVBlock, error) {
	var block cencIVBlock
	if len(sampleIV) != IVSizeCENC {
		return block, fmt.Errorf("cenc sample IV must be %d bytes, got %d", IVSizeCENC, len(sampleIV))
	}
	copy(block[:IVSizeCENC], sampleIV)
	// The counter half is already zero; writing it explicitly documents that
	// the counter starts at 0 rather than relying on the zero value.
	binary.BigEndian.PutUint64(block[IVSizeCENC:], 0)
	return block, nil
}

// isSupportedScheme reports whether scheme is one this server issues keys for.
// An empty scheme is accepted and means "use the default".
func isSupportedScheme(scheme string) bool {
	switch scheme {
	case "", SchemeCENC, SchemeCBCS:
		return true
	default:
		return false
	}
}

// normalizeScheme maps the empty scheme onto the default. Defaulting to cenc
// (rather than guessing) keeps /license responses deterministic, and callers
// that need cbcs say so explicitly.
func normalizeScheme(scheme string) string {
	if scheme == "" {
		return SchemeCENC
	}
	return scheme
}

// ivSizeForScheme returns the tenc iv_size for scheme.
func ivSizeForScheme(scheme string) int {
	if normalizeScheme(scheme) == SchemeCBCS {
		return IVSizeCBCS
	}
	return IVSizeCENC
}

// LicenseResponse is what a player-side decrypter gets back from /license. It
// is the minimum needed to build a decryption context: the key, the scheme, and
// the IV size so the caller knows how to read senc.
type LicenseResponse struct {
	KID     string `json:"kid"`
	Key     string `json:"key"`
	Scheme  string `json:"scheme"`
	IVSize  int    `json:"iv_size"`
	Expires int64  `json:"exp,omitempty"`
}

// LicenseRequest is the POST /license body.
type LicenseRequest struct {
	KID    string `json:"kid"`
	Scheme string `json:"scheme"`
}

// buildLicense assembles the /license payload for rec under scheme.
//
// The chosen scheme wins over the record's stored scheme only when the caller
// asked for one explicitly; otherwise the record's own scheme is used. That
// makes an unnamed request reproduce exactly what the key was created for.
func buildLicense(rec KeyRecord, scheme string) (LicenseResponse, error) {
	if !isSupportedScheme(scheme) {
		return LicenseResponse{}, newAPIError(http.StatusBadRequest, "unsupported scheme %q: want %s or %s",
			scheme, SchemeCENC, SchemeCBCS)
	}
	effective := normalizeScheme(rec.Scheme)
	if scheme != "" {
		effective = scheme
	}
	raw, err := hex.DecodeString(rec.Key)
	if err != nil || len(raw) != KeyBytes {
		return LicenseResponse{}, newAPIError(http.StatusInternalServerError,
			"stored key for kid %s is not %d hex bytes", rec.KID, KeyBytes)
	}
	return LicenseResponse{
		KID:    rec.KID,
		Key:    rec.Key,
		Scheme: effective,
		IVSize: ivSizeForScheme(effective),
	}, nil
}

// KeyJSONResponse is the ?format=json form of GET /key/{kid}. It exists so an
// operator can pipe the key out without a hex decoder, and so a browser hitting
// the URL by hand sees something readable instead of binary.
type KeyJSONResponse struct {
	KID    string `json:"kid"`
	Key    string `json:"key"`
	Scheme string `json:"scheme"`
	IV     string `json:"iv"`
}

// buildKeyJSON assembles the ?format=json view.
//
// IV is the CENC-style sample IV the key is intended to be used with: for cenc
// that is the 8-byte per-sample IV followed by the 8-byte big-endian counter
// starting at 0, which is exactly the block the transcoder hands to AES-CTR.
// It is reported as 0x-prefixed hex so it can go straight into an
// #EXT-X-KEY tag, and it is not secret.
func buildKeyJSON(rec KeyRecord, scheme string) (KeyJSONResponse, error) {
	lic, err := buildLicense(rec, scheme)
	if err != nil {
		return KeyJSONResponse{}, err
	}
	ivHex, err := defaultIVHex(lic.Scheme)
	if err != nil {
		return KeyJSONResponse{}, err
	}
	return KeyJSONResponse{
		KID:    lic.KID,
		Key:    lic.Key,
		Scheme: lic.Scheme,
		IV:     ivHex,
	}, nil
}

// defaultIVHex renders the zero sample IV in the shape a playlist tag wants:
// 0x followed by 32 hex chars. For cenc this is the 8-byte zero IV plus a zero
// counter, i.e. the value players derive implicitly for sample zero.
func defaultIVHex(scheme string) (string, error) {
	if normalizeScheme(scheme) == SchemeCBCS {
		return "0x" + hex.EncodeToString(make([]byte, IVBytes)), nil
	}
	block, err := cencIV(make([]byte, IVSizeCENC))
	if err != nil {
		return "", err
	}
	return "0x" + hex.EncodeToString(block[:]), nil
}
