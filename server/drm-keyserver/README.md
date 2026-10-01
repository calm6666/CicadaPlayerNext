# drm-keyserver

A self-contained AES-128 key server for **CicadaPlayerNext**, written in Go with the
standard library only. It issues keys, stores them in a JSON file, hands them to
players over a stable HTTP API, and can protect them with HMAC-signed URLs.

It is **transparent (clear-key) DRM**, not Widevine / PlayReady / FairPlay. Read
[Security model](#security-model) before you point anything public at it.

---

## 1. Build and run

```bash
# from this directory
go build ./...
go vet ./...
go test ./...
```

```bash
# demo mode: no secret, token checks off, a loud warning is logged
go run . -addr :9101 -data-dir ./drmdata

# with token enforcement
go run . -addr :9101 -data-dir ./drmdata -secret 'change-me'
```

Startup logs the listen address, the data dir, the key file, how many keys were
loaded, the default signed-URL lifetime, and whether token enforcement is on. It
**never** logs the secret or any key material.

### Flags

| Flag         | Default     | Meaning                                                                  |
| ------------ | ----------- | ------------------------------------------------------------------------ |
| `-addr`      | `:9101`     | Listen address (`host:port`).                                            |
| `-data-dir`  | `./drmdata` | Directory that holds `keys.json`. Created with mode `0700` if missing.    |
| `-secret`    | *(empty)*   | HMAC signing secret. **Empty means token enforcement is off.**           |
| `-key-ttl`   | `3600`      | Default signed-URL lifetime in seconds when `/admin/sign` omits `?ttl=`.  |

There are no `enable`/`disable` switches. `-secret` carries a value; whether
token checks happen is derived from whether a secret is configured, not from a
separate boolean. The same rule drives the transcoding script, where encryption
is decided by whether `--hls-key-info <file>` was supplied.

There are also no retries, watchdogs, polling loops, or `http.Server`
Read/Write/Idle timeouts. A request either succeeds or fails with an explicit
status.

### On-disk format

`<data-dir>/keys.json`, written atomically (temp file in the same directory, then
`os.Rename`), mode `0600`:

```json
[
  {
    "kid": "000102030405060708090a0b0c0d0e0f",
    "key": "2b7e151628aed2a6abf7158809cf4f3c",
    "created": "2024-05-01T12:00:00Z",
    "label": "demo",
    "scheme": "cenc"
  }
]
```

`kid` and `key` are always **32 lowercase hex characters (16 bytes)**. Uppercase
is rejected, not normalised, so a KID has exactly one spelling in the system.

---

## 2. Endpoints

| Method   | Path                | Purpose                                                        |
| -------- | ------------------- | -------------------------------------------------------------- |
| `GET`    | `/healthz`          | Liveness plus the key count.                                   |
| `GET`    | `/admin/keys`       | Every record **including key material** (admin surface).       |
| `POST`   | `/admin/keys`       | Create a key. Body optional.                                   |
| `GET`    | `/admin/keys/{kid}` | One record, key material included.                             |
| `DELETE` | `/admin/keys/{kid}` | Delete a record. `204`.                                        |
| `GET`    | `/admin/sign`       | Mint a signed `/key` URL. Needs `X-DRM-Secret` when enforced.   |
| `GET`    | `/key/{kid}`        | **Raw 16 key bytes** (`?format=json` for JSON). Player-facing.  |
| `POST`   | `/license`          | CENC license for a KID from a `tenc`/`senc` box.               |
| `GET`    | `/license/{kid}`    | The same license in GET form.                                  |

Every failure returns an explicit status with `{"error":"..."}` — never a bare
status, never an empty `200`.

`/key/{kid}` also sets `Cache-Control: no-store`, because a key must not sit in
a shared cache or a CDN edge.

### `GET /healthz`

```bash
curl -s localhost:9101/healthz
# {"keys":3,"status":"ok"}
```

### `POST /admin/keys`

The body is optional: `curl -X POST` with nothing else generates everything.

```bash
# generate a kid and a key
curl -s -X POST localhost:9101/admin/keys -d '{"label":"demo"}'
# {"kid":"9f2c...","key":"2b7e...","created":"2024-05-01T12:00:00Z","label":"demo","scheme":"cenc"}

# supply the kid (e.g. derived from the asset) and let the server make the key
curl -s -X POST localhost:9101/admin/keys \
  -d '{"kid":"000102030405060708090a0b0c0d0e0f","label":"ep01"}'

# supply both, e.g. to import an existing key
curl -s -X POST localhost:9101/admin/keys \
  -d '{"kid":"000102030405060708090a0b0c0d0e0f","key":"2b7e151628aed2a6abf7158809cf4f3c","scheme":"cbcs"}'
```

`201 Created` with the full record. A duplicate `kid` is `409`. A malformed
`kid`, `key`, `label` or `scheme` is `400`.

### `GET /admin/keys` and `GET /admin/keys/{kid}`

```bash
curl -s localhost:9101/admin/keys
curl -s localhost:9101/admin/keys/000102030405060708090a0b0c0d0e0f
# 404 + {"error":"unknown kid ..."} if it does not exist
```

### `DELETE /admin/keys/{kid}`

```bash
curl -s -o /dev/null -w '%{http_code}\n' -X DELETE \
  localhost:9101/admin/keys/000102030405060708090a0b0c0d0e0f
# 204
```

### `GET /key/{kid}` — the player-facing endpoint

Returns exactly the **raw 16 key bytes** as `application/octet-stream`. This is
the compatibility-critical default: an HLS `#EXT-X-KEY` URI must serve the raw
key, and CicadaPlayerNext's `HLSStream::updateKey()` opens the URL and reads
exactly 16 bytes.

```bash
curl -s localhost:9101/key/000102030405060708090a0b0c0d0e0f | xxd
# 00000000: 2b7e 1516 28ae d2a6 abf7 1588 09cf 4f3c
```

`?format=json` returns the same key in JSON instead (this is the form the
`key.bin` workflow below uses):

```bash
curl -s 'localhost:9101/key/000102030405060708090a0b0c0d0e0f?format=json'
# {"kid":"000102030405060708090a0b0c0d0e0f","key":"2b7e...","scheme":"cenc","iv":"0x00000000000000000000000000000000"}
```

The `iv` field is the 16-byte CENC-style value: the 8-byte per-sample IV (`0` for
the first sample) followed by the 8-byte big-endian block counter. It is not
secret and can go straight into a playlist tag.

Unknown KID → `404` + JSON error. Malformed KID → `400`.

### `GET /admin/sign` — mint a protected URL

```bash
# without -secret this is a 400: there is nothing to sign with
curl -s 'localhost:9101/admin/sign?kid=000102030405060708090a0b0c0d0e0f&ttl=3600'

# with -secret set, pass the secret in the header (not the query string)
curl -s -H 'X-DRM-Secret: change-me' \
  'localhost:9101/admin/sign?kid=000102030405060708090a0b0c0d0e0f&ttl=3600'
# {"exp":1714564800,"url":"/key/000102030405060708090a0b0c0d0e0f?exp=1714564800&sig=9c1f..."}
```

`ttl` defaults to `3600` seconds and must be a positive integer `<= 31536000`.
Signing an unknown KID is a `404` so a typo fails now rather than in a player
later. `absolute=1` prefixes the current host (e.g.
`http://localhost:9101/key/...`); that form trusts the `Host` header, so it is
opt-in and intended for local debugging.

### `POST /license` and `GET /license/{kid}`

This is what a player-side decrypter calls once it has a KID from a `tenc`/`senc`
box.

```bash
curl -s -X POST localhost:9101/license -d '{"kid":"000102030405060708090a0b0c0d0e0f","scheme":"cenc"}'
# {"kid":"0000...","key":"2b7e...","scheme":"cenc","iv_size":8}

curl -s 'localhost:9101/license/000102030405060708090a0b0c0d0e0f?scheme=cbcs'
# {"kid":"0000...","key":"2b7e...","scheme":"cbcs","iv_size":16}
```

`iv_size` is `8` for `cenc` (AES-CTR, 8-byte per-sample IV plus a 64-bit
big-endian block counter starting at 0) and `16` for `cbcs` (AES-CBC with a
constant IV). It is exactly the value a `tenc` box should carry.

Unknown KID → `404`. Bad JSON → `400`. Unsupported scheme → `400` (no silent
fallback to `cenc`).

---

## 3. Token format

A token is a credential with a fixed lifetime. It is not a session, not a retry
token, and not refreshed automatically: when `exp` passes, the signature is
worthless and the client must ask `/admin/sign` for a new URL.

```
token   = <exp>.<sig>
sig     = hex( HMAC-SHA256( secret, kid + "\n" + exp ) )
exp     = Unix seconds, decimal
protected URL = <base>/key/<kid>?exp=<exp>&sig=<sig>
```

Properties, and why they were chosen:

* No base64 and no padding — the token is URL-safe and copy-pasteable as is.
* The KID is inside the signed message, so a token for one key cannot be replayed
  against another key. It also means one leaked URL is one key, not the store.
* `exp` is signed as the exact decimal string that appears in the URL, so
  verification is a byte comparison with no re-formatting step to get wrong.
* Comparison uses `hmac.Equal` (constant time), and expiry is checked *after*
  the signature, so timing cannot be used to probe a guess.
* With `-secret` set, `GET /key/{kid}`, `POST /license` and `GET /license/{kid}`
  all require the credential and otherwise answer `401` + JSON error. Query
  parameters are the primary form; a single `token=<exp>.<sig>` parameter is
  accepted too, so an intermediary that rewrites the query does not break
  playback.
* With `-secret` empty, enforcement is off and the server logs a prominent
  warning at startup. This keeps the demo and the transcoding script runnable
  with no secret, and it is a deliberate state rather than a hidden default.

**HLS playlists.** `#EXT-X-KEY` allows a query string inside `URI="..."`, so a
signed URL goes into a playlist unchanged:

```
#EXT-X-KEY:METHOD=AES-128,URI="/key/000102030405060708090a0b0c0d0e0f?exp=1714564800&sig=9c1f...",IV=0x00000000000000000000000000000000
```

A relative `URI` resolves against the media playlist's own directory, so the
signed path must be absolute-from-the-host-root (`/key/...`) or a full
`https://host/key/...` URL. Mind the lifetime: the token must still be valid when
the player actually fetches it, so use a `ttl` that covers the intended viewing
window, and re-mint the playlist if that window closes.

---

## 4. Transcoding-script integration

The Python transcoder at `D:\hilihili\转码脚本\transcode_all.py` consumes a
three-line HLS `key_info` file (the same shape as ffmpeg's
`-hls_key_info_file`):

```
<key URI exactly as it will appear in #EXT-X-KEY>
<absolute path to a local file containing the raw 16 key bytes>
<optional IV as 0x + 32 hex chars>
```

* Line 1 may carry a query string (that is how the signed URL above works).
* Line 2's file holds **raw binary**, not hex — but the script also accepts 32
  hex characters, so either works.
* Line 3 is optional; omitting it means an all-zero base IV, which is what
  players derive implicitly for sample 0. When present, the last 4 bytes are a
  base sample number and each HLS segment N uses `base[0:12] + bigendian(k+N)`.
* The script reads the key but only ever logs a SHA-256 fingerprint of it.

End-to-end workflow (bash; on Windows use Git Bash so `xxd` and quoting behave):

```bash
KID=000102030405060708090a0b0c0d0e0f
KEY_INFO=key_info.txt

# 1. create a key (skip the kid to have one generated)
curl -s -X POST localhost:9101/admin/keys -d '{"label":"demo"}'
# -> {"kid":"...","key":"..."}   (note the kid for the next steps)

# 2. materialise key.bin: the raw 16 bytes the player will fetch
curl -s "localhost:9101/key/$KID?format=json" | jq -r .key | xxd -r -p > ./keys/$KID.key
#    without jq:
#    curl -s "localhost:9101/key/$KID?format=json" \
#      | grep -o '"key":"[0-9a-f]\{32\}' | cut -d'"' -f4 | xxd -r -p > ./keys/$KID.key
#    or straight from the admin record (identical content):
#    curl -s "localhost:9101/admin/keys/$KID" \
#      | grep -o '"key":"[0-9a-f]\{32\}' | cut -d'"' -f4 | xxd -r -p > ./keys/$KID.key
wc -c ./keys/$KID.key   # must print 16

# 3. get a signed URI for line 1 of key_info (only if -secret is set;
#    if it is not set, use the plain path /key/$KID instead)
URI=$(curl -s -H 'X-DRM-Secret: change-me' \
  "localhost:9101/admin/sign?kid=$KID&ttl=3600" | jq -r .url)

# 4. write key_info.txt: URI, absolute key path, optional IV
printf '%s\n%s\n%s\n' "$URI" "$(pwd)/keys/$KID.key" "0x00000000000000000000000000000000" > "$KEY_INFO"

# 5. transcode
python D:\\hilihili\\转码脚本\\transcode_all.py ... --hls-key-info "$KEY_INFO"
```

Keep `key.bin` **out of the delivery directory** and never sync it to a CDN: the
player fetches the key over HTTP from this server, not from the media bucket.

### HLS AES-128 versus CENC

The two paths share the same keys but are not the same scheme, and mixing them
up produces media that is encrypted one way and decrypted another:

* **HLS `#EXT-X-KEY:METHOD=AES-128`** — AES-128-CBC over each segment, the whole
  16-byte key fetched from the URI. Per-segment IV, usually derived from the
  segment sequence number. This is what `transcode_all.py` produces.
* **CENC `cenc`** — AES-CTR with an 8-byte per-sample IV plus a 64-bit block
  counter, KID and IV carried in `tenc`/`senc`, key fetched from `/license`.
* **CENC `cbcs`** — AES-CBC with a constant 16-byte IV and pattern encryption.

---

## 5. Security model

What this server is: a **demo / self-hosted key server for transparent
(clear-key) DRM**. It is enough to stop casual ripping and to make the pipeline
end-to-end real, and it is not a substitute for a commercial DRM system.

What it actually provides:

* **HMAC-signed, expiring URLs** stop anonymous key scraping. Someone who finds
  the playlist cannot fetch keys without a valid signature, and a signature they
  obtain expires.
* **No key material in logs.** Only KIDs and URLs are logged.
* **`Cache-Control: no-store`** keeps keys out of shared caches.
* **Owner-only file modes** (`0600` file, `0700` dir) on POSIX.

Honest limitations — read these as the real security boundary:

* **A determined client always wins.** The player must hold the symmetric key in
  memory to decrypt, so anyone who can run the client can dump the key. Signed
  URLs raise the cost; they do not change the outcome.
* **The admin surface is unauthenticated by design.** `/admin/keys` and
  `/admin/keys/{kid}` return key material, and `/admin/sign` only needs the
  secret. Bind this server to a trusted interface (or loopback), and put it
  behind your own auth/reverse proxy for anything else. Do **not** expose
  `/admin/*` to the internet.
* **No key rotation, no revocation of already-fetched keys.** `DELETE` removes a
  key from this server, but a client that already downloaded it keeps working.
  Rotating means re-encrypting the media with new keys.
* **Symmetric keys over plain HTTP are readable on the wire.** Use TLS (terminate
  it in front of this server) if the network is not trusted.
* **The secret is compared and used as configured.** Rotating `-secret`
  invalidates every outstanding token, which is the intended way to revoke them.
* **This is not Widevine, PlayReady or FairPlay.** There is no license
  negotiation, no CDM, no hardware-backed key storage, no output protection. In
  particular there is nothing here that satisfies a studio's DRM requirements.
  It deters casual ripping; it cannot stop a motivated attacker.

---

## 6. Testing

```bash
go test ./...
go test -run TestNISTCTRVector -v ./...
```

The suite covers the store round trip through a real temp-dir file, duplicate and
malformed KID rejection, raw-bytes delivery, the JSON shapes, both license forms,
every token outcome (valid, tampered, expired, missing, and enforcement off), the
NIST SP 800-38A F.5.1 CTR-AES128 vector in both directions, and the CENC IV
construction (8-byte per-sample IV plus a big-endian counter starting at 0).
