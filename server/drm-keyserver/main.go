package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
)

// defaultAddr is the loopback-friendly demo port. It is a value parameter
// (-addr), not a switch: the server always listens somewhere, and the operator
// chooses where.
const defaultAddr = ":9101"

// defaultDataDir keeps the key file next to wherever the operator started the
// binary, which is what the README's copy-pasteable workflow assumes.
const defaultDataDir = "./drmdata"

// usageText is printed on -h. It documents the exact curl workflow so an
// operator does not have to open the README for the happy path.
const usageText = `
drm-keyserver - a self-contained AES-128 / CENC key server for CicadaPlayerNext.

Endpoints:
  GET    /healthz                 liveness + key count
  GET    /admin/keys              list every key record (key material included)
  POST   /admin/keys              create a key (body optional: {"kid":"...","label":"..."})
  GET    /admin/keys/{kid}        one key record
  DELETE /admin/keys/{kid}        delete a key (204)
  GET    /admin/sign?kid=&ttl=    mint a signed /key URL (needs X-DRM-Secret if -secret is set)
  GET    /key/{kid}               raw 16 key bytes (add ?format=json for JSON)
  POST   /license                 {"kid":"...","scheme":"cenc"} -> key + iv_size
  GET    /license/{kid}           same license in GET form

Token format: <exp>.<sig> with sig = hex(HMAC-SHA256(secret, kid + newline + exp)),
passed as /key/<kid>?exp=<exp>&sig=<sig>. With -secret empty, enforcement is off
and a loud warning is logged; the server still serves keys.
`

func main() {
	os.Exit(run(os.Args[1:]))
}

// run is the testable entry point: it returns the process exit code instead of
// calling os.Exit itself, so a test could drive the whole startup path.
func run(args []string) int {
	fs := flag.NewFlagSet("drm-keyserver", flag.ContinueOnError)
	fs.Usage = func() {
		fmt.Fprint(fs.Output(), usageText)
		fs.PrintDefaults()
	}
	addr := fs.String("addr", defaultAddr, "listen address, host:port")
	dataDir := fs.String("data-dir", defaultDataDir, "directory holding keys.json")
	secret := fs.String("secret", "", "HMAC signing secret; empty disables token enforcement and logs a warning")
	// key-ttl is the default lifetime of a signed URL minted by /admin/sign when
	// the caller omits ?ttl=. It is a value, not a switch: it always applies.
	keyTTL := fs.Int("key-ttl", defaultTokenTTL, "default signed-URL lifetime in seconds")
	if err := fs.Parse(args); err != nil {
		// flag already printed the problem and the usage text.
		return 2
	}
	if fs.NArg() > 0 {
		fmt.Fprintf(os.Stderr, "unexpected positional arguments: %s\n", strings.Join(fs.Args(), " "))
		fs.Usage()
		return 2
	}
	if *keyTTL <= 0 || *keyTTL > maxTokenTTL {
		fmt.Fprintf(os.Stderr, "-key-ttl must be between 1 and %d seconds\n", maxTokenTTL)
		return 2
	}
	// The flag only supplies a default; keep it in one place so /admin/sign and
	// the startup log cannot disagree.
	defaultTokenTTL = *keyTTL

	store, err := NewKeyStore(*dataDir)
	if err != nil {
		log.Printf("fatal: %v", err)
		return 1
	}
	signer := NewSigner(*secret)
	srv := NewServer(store, signer)

	log.Printf("drm-keyserver starting")
	log.Printf("  listen address : %s", *addr)
	log.Printf("  data dir       : %s", store.Dir())
	log.Printf("  key file       : %s", store.Path())
	log.Printf("  keys loaded    : %d", store.Count())
	log.Printf("  default ttl    : %ds", defaultTokenTTL)
	if signer.Enforced() {
		// Only the fact of enforcement is logged, never the secret.
		log.Printf("  token checks   : ON (HMAC-SHA256, -secret is set)")
	} else {
		log.Printf("  token checks   : OFF ************************************************")
		log.Printf("  token checks   : OFF * -secret is empty: /key/{kid} and /license are open")
		log.Printf("  token checks   : OFF * anyone who can reach this port can read every key")
		log.Printf("  token checks   : OFF * run with -secret for anything but a local demo")
		log.Printf("  token checks   : OFF ************************************************")
	}

	// No Read/Write/Idle timeouts are set: this service deliberately has no
	// watchdog or retry behaviour, and a timeout here would be a silent failure
	// mode for a player mid-playback rather than a safety feature.
	httpSrv := &http.Server{
		Addr:    *addr,
		Handler: srv.Routes(),
	}
	if err := httpSrv.ListenAndServe(); err != nil {
		log.Printf("fatal: %v", err)
		return 1
	}
	return 0
}
