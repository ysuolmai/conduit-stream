# AirPlay 1 — Phase 2: RTSP State Machine + RSA Challenge + AES Key Exchange — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal (spec §5b + §5d + phasing row 2):** stand up a new component `components/raop` — the AirPlay
protocol plugin — that runs an **RTSP TCP server on port 5000**, walks a real sender through the
`OPTIONS → ANNOUNCE → SETUP → RECORD` handshake, answers the RSA **Apple-Challenge**, RSA-decrypts
the AES session key, and binds the three UDP sockets — so **a real iPhone/Mac connects and
negotiates a session**. No audio yet (the RTP receive/decrypt/decode loop is Phase 3). The 440 Hz
diagnostic tone keeps running the whole time.

**Architecture:** one new component, `components/raop`, decomposed into small single-responsibility
files. Following the Phase 0/1 discipline, every branch of *pure* logic lives in ESP-IDF-free,
mbedTLS-free `.c` files that compile and run on the host under `pio test -e native`:

- `base64.{h,c}` — pure base64 encode/decode (host-tested against known vectors).
- `rtsp_parser.{h,c}` — pure RTSP request parser + response builder + Transport-port extractor (host-tested).
- `sdp.{h,c}` — pure SDP field extraction (`a=rsaaeskey` / `a=aesiv` / `a=fmtp`) (host-tested).
- `raop_challenge.{h,c}` — pure assembly of the 32-byte pre-signature buffer (host-tested).

The ESP-IDF/mbedTLS glue is verified by **target build** only:

- `raop_key.c` — the well-known (public) RAOP RSA private key as an embedded PEM constant.
- `raop_crypto.{h,c}` — PSA-Crypto (mbedTLS 4.0) key import, Apple-Challenge signing, AES-key OAEP decrypt.
- `rtsp_session.{h,c}` — per-session state + UDP socket bind (audio/control/timing).
- `raop.{c}` + `include/raop.h` — the FreeRTOS accept/recv task and RTSP method dispatch; public `raop_server_start()`.

**Tech Stack:** ESP-IDF 6.0.1 (**mbedTLS 4.0.0** — see the crypto note below), PlatformIO, C11, Unity
(`native` env) for host unit tests. IDF deps: `mbedtls` (PSA Crypto + `mbedtls_pk_parse_key`), `lwip`
(BSD sockets), `esp_netif`, `esp_hw_support` (`esp_read_mac`), `esp_timer`.

**Do NOT touch `audio`** (Phase 0) — Phase 2 adds no audio producer. **Do NOT touch `wifi`/`discovery`/
`system`** except the one-line `main.cpp` call added in Task 10. Keep the 440 Hz diagnostic tone path intact.

---

## ⚠️ Critical toolchain finding — read before writing any crypto

ESP-IDF 6.0.1 ships **mbedTLS 4.0.0** (verified:
`~/.platformio/packages/framework-espidf/components/mbedtls/mbedtls/include/mbedtls/build_info.h` →
`MBEDTLS_VERSION_STRING "4.0.0"`). In mbedTLS 4.0 the **legacy low-level RSA API is private**:
`mbedtls/rsa.h` only exists at `tf-psa-crypto/drivers/builtin/include/mbedtls/private/rsa.h`, which is
**not on the public include path**. That means the classic shairport-sync approach —
`mbedtls_rsa_set_padding()` + `mbedtls_rsa_pkcs1_sign()` / `mbedtls_rsa_pkcs1_decrypt()` on a raw
`mbedtls_rsa_context` — **will not compile** here. There is also **no `mbedtls_pk_rsa()` accessor** in 4.0.

The supported public API is **PSA Crypto** (`psa/crypto.h`, verified reachable at
`.../mbedtls/tf-psa-crypto/include/psa/crypto.h`), bridged from a parsed PEM via
`mbedtls_pk_parse_key()` → `mbedtls_pk_get_psa_attributes()` → `mbedtls_pk_import_into_psa()`
(all verified present in `tf-psa-crypto/include/mbedtls/pk.h`). The two RAOP operations map exactly onto
PSA algorithms (verified in `psa/crypto_values.h` and `psa/crypto.h`):

| RAOP operation | shairport (mbedTLS ≤2.x) | **our PSA (mbedTLS 4.0) equivalent** |
|---|---|---|
| Apple-Challenge sign | `mbedtls_rsa_set_padding(V15, MD_NONE)` + `rsa_pkcs1_sign(MD_NONE, 32, buf)` | `psa_sign_hash(k, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, buf32, 32, sig, …)` |
| AES-key decrypt | `mbedtls_rsa_set_padding(V21, MD_SHA1)` + `rsa_pkcs1_decrypt(...)` | `psa_asymmetric_decrypt(k, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), in, ilen, NULL, 0, out, …)` |

`PSA_ALG_RSA_PKCS1V15_SIGN_RAW` = PKCS#1 v1.5 signature over the raw payload with **no DigestInfo prefix**
— byte-for-byte identical to shairport's `MD_NONE` sign. `PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1)` = RSAES-OAEP
with SHA-1 = shairport's `PKCS_V21 / MD_SHA1` decrypt. The key is 2048-bit, so the signature and
ciphertext block are **256 bytes** and the decrypted AES key is **16 bytes**.

**Verify-before-code:** at implementation time, `grep` the installed headers to confirm the exact
signatures before writing (they are pinned in this plan from the 4.0.0 headers, but confirm):
```bash
MB=~/.platformio/packages/framework-espidf/components/mbedtls/mbedtls
grep -n "psa_sign_hash\|psa_asymmetric_decrypt\|psa_import_key" $MB/tf-psa-crypto/include/psa/crypto.h
grep -n "mbedtls_pk_parse_key\|mbedtls_pk_get_psa_attributes\|mbedtls_pk_import_into_psa" \
     $MB/tf-psa-crypto/include/mbedtls/pk.h
grep -n "PSA_ALG_RSA_PKCS1V15_SIGN_RAW\|PSA_ALG_RSA_OAEP" $MB/tf-psa-crypto/include/psa/crypto_values.h
```

---

## Crypto algorithm (cited) — exactly what we implement

**Source of truth for the RAOP algorithm:** shairport-sync (Mike Brady), tag `3.3.9`,
`rtsp.c::apple_challenge()` + `handle_announce()` and `common.c::rsa_apply()` / `super_secret_key`.
Fetched and quoted during planning:
- `apple_challenge()`: base64-decode `Apple-Challenge`; build `buf = challenge || local-IP-bytes ||
  hardware-address(6)`; **pad to 0x20 (32) bytes**; `rsa_apply(buf, buflen, RSA_MODE_AUTH)`; base64-encode
  the result; **strip at the first `=`**; emit as the **`Apple-Response`** header. (shairport quotes:
  `uint8_t buf[48], *bp = buf; memcpy(bp, chall, chall_len); … pad to 0x20 …
  challresp = rsa_apply(buf, buflen, &resplen, RSA_MODE_AUTH); encoded = base64_enc(...); *strchr(encoded,'=')=0;
  msg_add_header(resp,"Apple-Response",encoded);`)
- `handle_announce()`: `rsaaeskey = base64_dec(prsaaeskey,&len); aeskey = rsa_apply(rsaaeskey,len,&keylen,RSA_MODE_KEY);
  /* keylen==16 */ memcpy(conn->stream.aeskey, aeskey, 16);`
- `rsa_apply()` (mbedTLS build): `RSA_MODE_AUTH` → `mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V15,
  MBEDTLS_MD_NONE)` then a **private-key** PKCS#1 v1.5 operation; `RSA_MODE_KEY` →
  `mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1)` then `mbedtls_rsa_pkcs1_decrypt`.

**Header-name note (interop-critical):** shairport emits **`Apple-Response`** (not `RSA-Response`). Real
Apple senders send `Apple-Challenge` in the `OPTIONS` request and read `Apple-Response` from our reply.
**Use `Apple-Response`.** (The task brief's "RSA-Response" is a misnomer; documented here as the deviation,
resolved in favor of the proven interop name.)

**Our exact byte layout (IPv4 only — the S3 station is IPv4):**
```
pre-sign buffer (32 bytes, zero-filled):
  [0..15]  16 bytes  decoded Apple-Challenge
  [16..19]  4 bytes  local IPv4 address, network byte order (from getsockname() on the accepted socket)
  [20..25]  6 bytes  STA MAC (esp_read_mac(mac, ESP_MAC_WIFI_STA))
  [26..31]  6 bytes  zero padding
→ psa_sign_hash(sign_key, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, buf, 32, sig[256], …)
→ base64-encode sig → truncate at first '=' → "Apple-Response: <that>"
```
Using `getsockname()` on the accepted client socket for the local IP mirrors shairport exactly (it uses the
local end of the sender's connection, so multi-homing / the address the sender actually reached us on is correct).

**The RAOP RSA private key (public knowledge, NOT a secret):** the "AirPort Express" key shipped verbatim by
every open RAOP receiver. Anchors (verified during planning from shairport-sync `3.3.9/common.c`):
- begins: `-----BEGIN RSA PRIVATE KEY-----\nMIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt…`
- ends:   `…2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n-----END RSA PRIVATE KEY-----`

Because the body is ~1.6 KB of base64 that must be **byte-exact** (a single wrong char breaks all interop and
key parsing), **do not hand-transcribe it.** Task 5 obtains it verbatim at implementation time and Task 6's
`raop_crypto_init()` self-test **round-trips** it (encrypt-then-decrypt + sign) so a corrupted key fails **loud
at boot** rather than silently mis-negotiating. Pinned source URL:
`https://raw.githubusercontent.com/mikebrady/shairport-sync/3.3.9/common.c` (the `super_secret_key` constant).
Equivalent verbatim copies live in owntone (`forked-daapd`), `shairport` (James Laird), and `RPiPlay`.

---

## File Structure

```
framework/
  platformio.ini                              # MODIFY: native env -I components/raop/src
  src/main.cpp                                # MODIFY: on_got_ip -> raop_server_start() after mdns advertise
  components/raop/                            # NEW COMPONENT
    CMakeLists.txt                            # CREATE: register srcs, PRIV_REQUIRES
    README.md                                 # CREATE: document the RAOP plugin + Phase 2 scope
    include/raop.h                            # CREATE: public API (raop_server_start / _stop)
    src/base64.h                              # CREATE: pure base64 decls (no ESP-IDF/mbedTLS)
    src/base64.c                              # CREATE: pure base64 encode/decode (host-tested)
    src/rtsp_parser.h                         # CREATE: pure RTSP parse/build decls
    src/rtsp_parser.c                         # CREATE: pure RTSP request parser + response builder (host-tested)
    src/sdp.h                                 # CREATE: pure SDP field-extraction decls
    src/sdp.c                                 # CREATE: pure SDP parser (host-tested)
    src/raop_challenge.h                      # CREATE: pure challenge-assembly decls
    src/raop_challenge.c                      # CREATE: pure 32-byte pre-sign buffer (host-tested)
    src/raop_key.c                            # CREATE: embedded RAOP RSA private key PEM
    src/raop_crypto.h                         # CREATE: crypto API (init/sign/decrypt)
    src/raop_crypto.c                         # CREATE: PSA key import + challenge sign + AES-key OAEP decrypt
    src/rtsp_session.h                        # CREATE: session state + UDP-bind decls
    src/rtsp_session.c                        # CREATE: session state machine + UDP socket bind
    src/raop.c                                # CREATE: RTSP accept/recv task + method dispatch
  test/test_base64/test_base64.c              # CREATE: Unity host tests (encode/decode vectors)
  test/test_rtsp_parser/test_rtsp_parser.c    # CREATE: Unity host tests (request line, headers, body, response, transport)
  test/test_sdp/test_sdp.c                    # CREATE: Unity host tests (rsaaeskey/aesiv/fmtp extraction)
  test/test_raop_challenge/test_raop_challenge.c # CREATE: Unity host tests (32-byte layout)
```

**Pure vs glue split (the house rule):** `base64`, `rtsp_parser`, `sdp`, `raop_challenge` are pure C, zero
ESP-IDF/mbedTLS includes, `#include`d directly into their test TU (mirrors `audio_ringbuf.c`). Everything that
touches PSA, sockets, MAC, or FreeRTOS (`raop_crypto`, `rtsp_session`, `raop`, `raop_key`) is target-build-only.

---

## Task 1: `base64` — pure encode/decode (host-unit-tested)

**Files:** create `components/raop/src/base64.h`, `components/raop/src/base64.c`,
`test/test_base64/test_base64.c`; modify `platformio.ini`.

- [ ] **Step 1: Add the raop pure-source dir to the native include path.** Replace the `[env:native]`
  `build_flags` line in `platformio.ini`:
  ```ini
  build_flags = -I components/audio/src -I components/system/src -I components/discovery/src -I components/raop/src -std=gnu11
  ```

- [ ] **Step 2: Write `components/raop/src/base64.h`:**
  ```c
  // Pure-C base64 (standard alphabet, RFC 4648). No ESP-IDF/mbedTLS deps, so
  // host-unit-testable. RAOP needs both directions: decode Apple-Challenge /
  // rsaaeskey / aesiv from RTSP+SDP, encode the signed challenge response.
  #pragma once
  #include <stddef.h>
  #include <stdint.h>

  // Bytes required to encode n input bytes, including the NUL terminator.
  static inline size_t base64_encoded_size(size_t n) { return ((n + 2) / 3) * 4 + 1; }

  // Decode `in_len` base64 chars into `out` (<= out_cap bytes). Skips ASCII
  // whitespace; accepts optional trailing '=' padding. Returns the number of
  // decoded bytes, or -1 on an invalid character or if the result exceeds out_cap.
  int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap);

  // Encode `in_len` bytes to a NUL-terminated base64 string in `out`. Returns the
  // string length (excluding NUL), or -1 if out_cap < base64_encoded_size(in_len).
  int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
  ```

- [ ] **Step 3: Write the failing host tests `test/test_base64/test_base64.c`:**
  ```c
  #include <unity.h>
  #include <string.h>
  #include "base64.c"  // compile the pure unit directly into the test TU

  void setUp(void) {}
  void tearDown(void) {}

  // RFC 4648 §10 vectors.
  void test_encode_rfc4648_vectors(void) {
      char out[16];
      TEST_ASSERT_EQUAL_INT(0, base64_encode((const uint8_t *)"", 0, out, sizeof(out)));
      TEST_ASSERT_EQUAL_STRING("", out);
      base64_encode((const uint8_t *)"f", 1, out, sizeof(out));      TEST_ASSERT_EQUAL_STRING("Zg==", out);
      base64_encode((const uint8_t *)"fo", 2, out, sizeof(out));     TEST_ASSERT_EQUAL_STRING("Zm8=", out);
      base64_encode((const uint8_t *)"foo", 3, out, sizeof(out));    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
      base64_encode((const uint8_t *)"foob", 4, out, sizeof(out));   TEST_ASSERT_EQUAL_STRING("Zm9vYg==", out);
      base64_encode((const uint8_t *)"fooba", 5, out, sizeof(out));  TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", out);
      base64_encode((const uint8_t *)"foobar", 6, out, sizeof(out)); TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", out);
  }

  void test_decode_roundtrip_and_padding(void) {
      uint8_t out[16];
      TEST_ASSERT_EQUAL_INT(6, base64_decode("Zm9vYmFy", 8, out, sizeof(out)));
      TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
      TEST_ASSERT_EQUAL_INT(1, base64_decode("Zg==", 4, out, sizeof(out)));
      TEST_ASSERT_EQUAL_MEMORY("f", out, 1);
      TEST_ASSERT_EQUAL_INT(2, base64_decode("Zm8=", 4, out, sizeof(out)));
      TEST_ASSERT_EQUAL_MEMORY("fo", out, 2);
  }

  void test_decode_skips_whitespace(void) {  // RTSP/SDP wrap base64 across lines
      uint8_t out[16];
      TEST_ASSERT_EQUAL_INT(6, base64_decode("Zm9v\r\nYmFy", 10, out, sizeof(out)));
      TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
  }

  void test_decode_rejects_bad_char_and_overflow(void) {
      uint8_t out[2];
      TEST_ASSERT_EQUAL_INT(-1, base64_decode("****", 4, out, sizeof(out)));       // invalid char
      TEST_ASSERT_EQUAL_INT(-1, base64_decode("Zm9vYmFy", 8, out, sizeof(out)));   // 6 bytes > out_cap 2
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_encode_rfc4648_vectors);
      RUN_TEST(test_decode_roundtrip_and_padding);
      RUN_TEST(test_decode_skips_whitespace);
      RUN_TEST(test_decode_rejects_bad_char_and_overflow);
      return UNITY_END();
  }
  ```

- [ ] **Step 4: Run — expect FAIL to compile** (`base64.c: No such file or directory`):
  `cd framework && ~/.platformio/penv/bin/pio test -e native`

- [ ] **Step 5: Write `components/raop/src/base64.c`:**
  ```c
  #include "base64.h"

  static const char kEnc[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // -1 for non-alphabet chars; caller treats whitespace and '=' specially.
  static int dec_val(unsigned char c) {
      if (c >= 'A' && c <= 'Z') return c - 'A';
      if (c >= 'a' && c <= 'z') return c - 'a' + 26;
      if (c >= '0' && c <= '9') return c - '0' + 52;
      if (c == '+') return 62;
      if (c == '/') return 63;
      return -1;
  }

  int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
      size_t need = base64_encoded_size(in_len);
      if (out_cap < need) return -1;
      size_t o = 0;
      size_t i = 0;
      while (i + 3 <= in_len) {
          uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
          out[o++] = kEnc[(n >> 18) & 63];
          out[o++] = kEnc[(n >> 12) & 63];
          out[o++] = kEnc[(n >> 6) & 63];
          out[o++] = kEnc[n & 63];
          i += 3;
      }
      size_t rem = in_len - i;
      if (rem == 1) {
          uint32_t n = in[i] << 16;
          out[o++] = kEnc[(n >> 18) & 63];
          out[o++] = kEnc[(n >> 12) & 63];
          out[o++] = '=';
          out[o++] = '=';
      } else if (rem == 2) {
          uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
          out[o++] = kEnc[(n >> 18) & 63];
          out[o++] = kEnc[(n >> 12) & 63];
          out[o++] = kEnc[(n >> 6) & 63];
          out[o++] = '=';
      }
      out[o] = '\0';
      return (int)o;
  }

  int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap) {
      uint32_t acc = 0;
      int nbits = 0;
      size_t o = 0;
      for (size_t i = 0; i < in_len; i++) {
          unsigned char c = (unsigned char)in[i];
          if (c == '=') break;                                   // padding: done
          if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;  // wrapped base64
          int v = dec_val(c);
          if (v < 0) return -1;                                  // invalid char
          acc = (acc << 6) | (uint32_t)v;
          nbits += 6;
          if (nbits >= 8) {
              nbits -= 8;
              if (o >= out_cap) return -1;                       // bound the output (§9)
              out[o++] = (uint8_t)((acc >> nbits) & 0xFF);
          }
      }
      return (int)o;
  }
  ```

- [ ] **Step 6: Run — expect PASS** (`test_base64` green; existing suites still green):
  `cd framework && ~/.platformio/penv/bin/pio test -e native`

- [ ] **Step 7: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/platformio.ini \
          framework/components/raop/src/base64.h \
          framework/components/raop/src/base64.c \
          framework/test/test_base64/test_base64.c
  git commit -m "feat(raop): pure base64 encode/decode with host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 2: `rtsp_parser` — pure request parser + response builder + Transport port (host-unit-tested)

**Files:** create `components/raop/src/rtsp_parser.h`, `.c`, `test/test_rtsp_parser/test_rtsp_parser.c`.

The parser consumes **untrusted LAN input** (spec §9): every copy is bounded, every length validated,
truncation is safe (always NUL-terminated), and the body is clamped to what actually arrived.

- [ ] **Step 1: Write `components/raop/src/rtsp_parser.h`:**
  ```c
  // Pure-C RTSP/1.0 request parser + response builder. No ESP-IDF/mbedTLS deps,
  // so host-unit-testable. Bounds every field against untrusted input (spec §9):
  // over-long tokens are truncated but always NUL-terminated; the body is clamped
  // to the bytes actually present. Header keys compare case-insensitively.
  #pragma once
  #include <stddef.h>
  #include <stdbool.h>

  #define RTSP_MAX_HEADERS 20
  #define RTSP_MAX_NAME    32
  #define RTSP_MAX_VALUE   384   // Apple-Challenge/Transport/RTP-Info fit; rsaaeskey rides in the SDP body

  typedef struct { char name[RTSP_MAX_NAME]; char value[RTSP_MAX_VALUE]; } rtsp_header_t;

  typedef struct {
      char           method[16];    // "OPTIONS", "ANNOUNCE", ...
      char           uri[128];      // request URI (truncated if longer)
      char           version[16];   // "RTSP/1.0"
      rtsp_header_t  headers[RTSP_MAX_HEADERS];
      int            header_count;
      const char    *body;          // points into `buf` after the blank line, or NULL
      size_t         body_len;      // min(Content-Length, bytes present after headers)
      bool           valid;         // request line + headers well-formed
  } rtsp_request_t;

  // Parse a (possibly complete) RTSP request from `buf`/`len`. Returns true when
  // the request line and header block are fully present (a blank CRLF line seen).
  // On true, `body`/`body_len` describe the payload actually available.
  bool rtsp_parse_request(const char *buf, size_t len, rtsp_request_t *out);

  // Case-insensitive header lookup; NULL if absent.
  const char *rtsp_header_get(const rtsp_request_t *r, const char *name);

  // Convenience accessors. Return -1 if the header is absent or unparseable.
  int  rtsp_cseq(const rtsp_request_t *r);
  long rtsp_content_length(const rtsp_request_t *r);

  // Extract an integer sub-field from a Transport header, e.g.
  // rtsp_transport_port("RTP/AVP/UDP;unicast;control_port=6001;timing_port=6002",
  //                     "control_port") -> 6001. Returns -1 if not found.
  int rtsp_transport_port(const char *transport, const char *key);

  // Build an RTSP response into `out`. Always emits the status line, the echoed
  // CSeq, and Server; appends `extra_headers` verbatim (must be pre-formatted with
  // trailing CRLF per line, or NULL); adds Content-Length + body when body != NULL.
  // Returns total bytes written, or -1 on overflow.
  int rtsp_build_response(char *out, size_t out_cap, int status, const char *reason,
                          int cseq, const char *extra_headers,
                          const char *body, size_t body_len);
  ```

- [ ] **Step 2: Write the failing host tests `test/test_rtsp_parser/test_rtsp_parser.c`:**
  ```c
  #include <unity.h>
  #include <string.h>
  #include "rtsp_parser.c"  // compile the pure unit directly into the test TU

  void setUp(void) {}
  void tearDown(void) {}

  static const char kOptions[] =
      "OPTIONS * RTSP/1.0\r\n"
      "CSeq: 1\r\n"
      "Apple-Challenge: Sj/xrb+m1nBUdX+d2llW0Q\r\n"
      "User-Agent: AirPlay/409.16\r\n"
      "\r\n";

  void test_parses_request_line_and_headers(void) {
      rtsp_request_t r;
      TEST_ASSERT_TRUE(rtsp_parse_request(kOptions, strlen(kOptions), &r));
      TEST_ASSERT_EQUAL_STRING("OPTIONS", r.method);
      TEST_ASSERT_EQUAL_STRING("*", r.uri);
      TEST_ASSERT_EQUAL_STRING("RTSP/1.0", r.version);
      TEST_ASSERT_EQUAL_INT(1, rtsp_cseq(&r));
      TEST_ASSERT_EQUAL_STRING("Sj/xrb+m1nBUdX+d2llW0Q", rtsp_header_get(&r, "Apple-Challenge"));
  }

  void test_header_lookup_is_case_insensitive(void) {
      rtsp_request_t r;
      rtsp_parse_request(kOptions, strlen(kOptions), &r);
      TEST_ASSERT_EQUAL_STRING("1", rtsp_header_get(&r, "cseq"));
      TEST_ASSERT_EQUAL_STRING("1", rtsp_header_get(&r, "CSEQ"));
      TEST_ASSERT_NULL(rtsp_header_get(&r, "Nonexistent"));
  }

  void test_body_clamped_to_content_length(void) {
      static const char req[] =
          "ANNOUNCE rtsp://x RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
      rtsp_request_t r;
      TEST_ASSERT_TRUE(rtsp_parse_request(req, strlen(req), &r));
      TEST_ASSERT_EQUAL_INT(5, (int)r.body_len);
      TEST_ASSERT_EQUAL_MEMORY("hello", r.body, 5);
      TEST_ASSERT_EQUAL_INT(5, (int)rtsp_content_length(&r));
  }

  void test_incomplete_request_returns_false(void) {
      static const char partial[] = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n"; // no blank line yet
      rtsp_request_t r;
      TEST_ASSERT_FALSE(rtsp_parse_request(partial, strlen(partial), &r));
  }

  void test_transport_port_extraction(void) {
      const char *t = "RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002";
      TEST_ASSERT_EQUAL_INT(6001, rtsp_transport_port(t, "control_port"));
      TEST_ASSERT_EQUAL_INT(6002, rtsp_transport_port(t, "timing_port"));
      TEST_ASSERT_EQUAL_INT(-1,   rtsp_transport_port(t, "server_port"));
  }

  void test_build_response_echoes_cseq(void) {
      char out[256];
      int n = rtsp_build_response(out, sizeof(out), 200, "OK", 1,
                                  "Apple-Response: abc\r\n", NULL, 0);
      TEST_ASSERT_GREATER_THAN(0, n);
      TEST_ASSERT_NOT_NULL(strstr(out, "RTSP/1.0 200 OK\r\n"));
      TEST_ASSERT_NOT_NULL(strstr(out, "CSeq: 1\r\n"));
      TEST_ASSERT_NOT_NULL(strstr(out, "Apple-Response: abc\r\n"));
      TEST_ASSERT_EQUAL_CHAR('\n', out[n - 1]);  // ends with the blank-line CRLF
  }

  void test_build_response_with_body_sets_content_length(void) {
      char out[256];
      const char *body = "volume: -20.0\r\n";
      rtsp_build_response(out, sizeof(out), 200, "OK", 7, NULL, body, strlen(body));
      TEST_ASSERT_NOT_NULL(strstr(out, "Content-Length: 15\r\n"));
      TEST_ASSERT_NOT_NULL(strstr(out, "\r\n\r\nvolume: -20.0\r\n"));
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_parses_request_line_and_headers);
      RUN_TEST(test_header_lookup_is_case_insensitive);
      RUN_TEST(test_body_clamped_to_content_length);
      RUN_TEST(test_incomplete_request_returns_false);
      RUN_TEST(test_transport_port_extraction);
      RUN_TEST(test_build_response_echoes_cseq);
      RUN_TEST(test_build_response_with_body_sets_content_length);
      return UNITY_END();
  }
  ```

- [ ] **Step 3: Run — expect FAIL to compile.**

- [ ] **Step 4: Write `components/raop/src/rtsp_parser.c`.** Implementation notes (write to make the tests
  pass; keep every copy bounded):
  - `rtsp_parse_request`: find the header terminator `"\r\n\r\n"`; if absent, return `false` (request not yet
    complete — the recv loop keeps reading). Copy the first line's three space-separated tokens into
    `method`/`uri`/`version` with `snprintf(..., sizeof field, "%.*s", ...)` bounding. Walk each subsequent
    header line up to the blank line: split on the first `':'`, trim one optional leading space in the value,
    `snprintf`-bound both into `headers[header_count++]` while `header_count < RTSP_MAX_HEADERS`. Set `body`
    to the char after the `"\r\n\r\n"`; compute `avail = len - (body - buf)`; `cl = rtsp_content_length`;
    `body_len = (cl >= 0 && (size_t)cl < avail) ? (size_t)cl : avail`. Set `valid = true`.
  - `rtsp_header_get`: linear scan, `strcasecmp` on the key.
  - `rtsp_cseq`: `rtsp_header_get(r,"CSeq")` → `strtol`, return -1 if absent.
  - `rtsp_content_length`: `rtsp_header_get(r,"Content-Length")` → `strtol` (base 10), return -1 if absent.
  - `rtsp_transport_port`: `strstr` for `key`, ensure the match is a field boundary (preceded by ';' or start,
    followed by '='), then `atoi` the digits after '='. Return -1 if not found.
  - `rtsp_build_response`: `snprintf` the status line + `CSeq:` + `Server: Conduit/1.0\r\n`; append
    `extra_headers` if non-NULL; if `body` non-NULL append `Content-Length: <body_len>\r\n`; append the blank
    line `\r\n`; then the body bytes. Track the running offset against `out_cap`; return -1 on any overflow.
    Use `strcasecmp`/`strncasecmp` from `<strings.h>` and `strtol`/`atoi` from `<stdlib.h>`.

- [ ] **Step 5: Run — expect PASS** (`test_rtsp_parser` green).

- [ ] **Step 6: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/components/raop/src/rtsp_parser.h \
          framework/components/raop/src/rtsp_parser.c \
          framework/test/test_rtsp_parser/test_rtsp_parser.c
  git commit -m "feat(raop): pure RTSP request parser + response builder with host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 3: `sdp` — pure ANNOUNCE SDP field extraction (host-unit-tested)

**Files:** create `components/raop/src/sdp.h`, `.c`, `test/test_sdp/test_sdp.c`.

- [ ] **Step 1: Write `components/raop/src/sdp.h`:**
  ```c
  // Pure-C extraction of the three RAOP-relevant SDP attributes from an ANNOUNCE
  // body. No ESP-IDF/mbedTLS deps, so host-unit-testable. All fields are copied,
  // bounded, and NUL-terminated (untrusted input, spec §9).
  //   a=rsaaeskey:<base64>   RSA-encrypted AES session key
  //   a=aesiv:<base64>       AES-CBC IV
  //   a=fmtp:<params>        ALAC "magic cookie" params (kept for the Phase 3 decoder)
  #pragma once
  #include <stddef.h>
  #include <stdbool.h>

  typedef struct {
      char rsaaeskey[512];  // base64 (RSA-2048 ciphertext -> 256 bytes -> ~344 b64 chars)
      char aesiv[64];       // base64 (16 bytes -> 24 chars)
      char fmtp[160];       // e.g. "96 352 0 16 40 10 14 2 255 0 0 44100"
      bool has_rsaaeskey, has_aesiv, has_fmtp;
  } sdp_media_t;

  // Parse the SDP `body`/`len`, filling the three attributes it finds. Returns true
  // if at least one attribute was extracted. Missing attributes leave has_* false.
  bool sdp_parse(const char *body, size_t len, sdp_media_t *out);
  ```

- [ ] **Step 2: Write the failing host tests `test/test_sdp/test_sdp.c`** (a representative real ANNOUNCE body):
  ```c
  #include <unity.h>
  #include <string.h>
  #include "sdp.c"

  void setUp(void) {}
  void tearDown(void) {}

  static const char kSdp[] =
      "v=0\r\n"
      "o=iTunes 3641928563 0 IN IP4 10.0.0.5\r\n"
      "s=iTunes\r\n"
      "c=IN IP4 10.0.0.9\r\n"
      "t=0 0\r\n"
      "m=audio 0 RTP/AVP 96\r\n"
      "a=rtpmap:96 AppleLossless\r\n"
      "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
      "a=rsaaeskey:VjVbxWcmYgbBbhwBNlCh3K0CMNtWoB844BuiHGUJT51xg\r\n"
      "a=aesiv:5b7b4d43a53a34dccd2ed845b8e00e7f\r\n";

  void test_extracts_all_three_fields(void) {
      sdp_media_t m;
      TEST_ASSERT_TRUE(sdp_parse(kSdp, strlen(kSdp), &m));
      TEST_ASSERT_TRUE(m.has_fmtp);
      TEST_ASSERT_TRUE(m.has_rsaaeskey);
      TEST_ASSERT_TRUE(m.has_aesiv);
      TEST_ASSERT_EQUAL_STRING("96 352 0 16 40 10 14 2 255 0 0 44100", m.fmtp);
      TEST_ASSERT_EQUAL_STRING("VjVbxWcmYgbBbhwBNlCh3K0CMNtWoB844BuiHGUJT51xg", m.rsaaeskey);
      TEST_ASSERT_EQUAL_STRING("5b7b4d43a53a34dccd2ed845b8e00e7f", m.aesiv);
  }

  void test_missing_fields_flagged_absent(void) {
      static const char sdp[] = "v=0\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n";
      sdp_media_t m;
      TEST_ASSERT_TRUE(sdp_parse(sdp, strlen(sdp), &m));
      TEST_ASSERT_TRUE(m.has_fmtp);
      TEST_ASSERT_FALSE(m.has_rsaaeskey);
      TEST_ASSERT_FALSE(m.has_aesiv);
  }

  void test_empty_body_returns_false(void) {
      sdp_media_t m;
      TEST_ASSERT_FALSE(sdp_parse("", 0, &m));
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_extracts_all_three_fields);
      RUN_TEST(test_missing_fields_flagged_absent);
      RUN_TEST(test_empty_body_returns_false);
      return UNITY_END();
  }
  ```

- [ ] **Step 3: Run — expect FAIL to compile.**

- [ ] **Step 4: Write `components/raop/src/sdp.c`.** Notes: `memset(out,0,sizeof)`; scan line by line
  (split on `\n`, strip a trailing `\r`). For each line matching a prefix, copy the remainder (bounded via
  `snprintf`) into the right field and set the `has_*` flag:
  - `"a=rsaaeskey:"` → `rsaaeskey`
  - `"a=aesiv:"` → `aesiv`
  - `"a=fmtp:"` → `fmtp` (keep the whole param string incl. the leading payload-type number, exactly as the
    Phase 3 ALAC decoder's "magic cookie" builder will consume it)
  Return `has_rsaaeskey || has_aesiv || has_fmtp`.

- [ ] **Step 5: Run — expect PASS.**

- [ ] **Step 6: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/components/raop/src/sdp.h \
          framework/components/raop/src/sdp.c \
          framework/test/test_sdp/test_sdp.c
  git commit -m "feat(raop): pure SDP field extraction (rsaaeskey/aesiv/fmtp) with host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 4: `raop_challenge` — pure 32-byte pre-signature buffer (host-unit-tested)

**Files:** create `components/raop/src/raop_challenge.h`, `.c`, `test/test_raop_challenge/test_raop_challenge.c`.

This isolates the interop-critical **byte layout** of the Apple-Challenge response so it is host-tested
independently of the (untestable-here) RSA signature.

- [ ] **Step 1: Write `components/raop/src/raop_challenge.h`:**
  ```c
  // Pure-C assembly of the RAOP Apple-Challenge pre-signature buffer. No ESP-IDF/
  // mbedTLS deps, so host-unit-testable. Layout (shairport-sync compatible, IPv4):
  //   [0..15]  16-byte decoded Apple-Challenge
  //   [16..19]  4-byte local IPv4 (network byte order)
  //   [20..25]  6-byte MAC
  //   [26..31]  zero padding to 32 bytes
  // raop_crypto then RSA/PKCS1-v1.5-signs this 32-byte buffer raw (no hash).
  #pragma once
  #include <stddef.h>
  #include <stdint.h>

  #define RAOP_CHALLENGE_BUF_LEN 32

  // Zero-fill `out` (exactly 32 bytes) then lay in challenge||ip4||mac. `clen` must
  // be 16 (AirPlay's challenge size); returns the used length before padding (26)
  // on success, or -1 if clen != 16 (would not fit the fixed layout).
  int raop_challenge_assemble(const uint8_t *challenge, size_t clen,
                              const uint8_t ip4[4], const uint8_t mac[6],
                              uint8_t out[RAOP_CHALLENGE_BUF_LEN]);
  ```

- [ ] **Step 2: Write the failing host tests `test/test_raop_challenge/test_raop_challenge.c`:**
  ```c
  #include <unity.h>
  #include <string.h>
  #include "raop_challenge.c"

  void setUp(void) {}
  void tearDown(void) {}

  void test_layout_challenge_ip_mac_padding(void) {
      uint8_t chal[16]; for (int i = 0; i < 16; i++) chal[i] = (uint8_t)(0x10 + i);
      uint8_t ip4[4]   = {10, 0, 0, 9};
      uint8_t mac[6]   = {0xE8, 0x3D, 0xC1, 0xF2, 0xAC, 0x6C};
      uint8_t out[RAOP_CHALLENGE_BUF_LEN];
      TEST_ASSERT_EQUAL_INT(26, raop_challenge_assemble(chal, 16, ip4, mac, out));
      TEST_ASSERT_EQUAL_MEMORY(chal, out, 16);          // challenge
      TEST_ASSERT_EQUAL_MEMORY(ip4, out + 16, 4);        // IPv4
      TEST_ASSERT_EQUAL_MEMORY(mac, out + 20, 6);        // MAC
      for (int i = 26; i < 32; i++) TEST_ASSERT_EQUAL_UINT8(0, out[i]);  // padding
  }

  void test_rejects_wrong_challenge_length(void) {
      uint8_t chal[8] = {0};
      uint8_t ip4[4]  = {0}, mac[6] = {0}, out[RAOP_CHALLENGE_BUF_LEN];
      TEST_ASSERT_EQUAL_INT(-1, raop_challenge_assemble(chal, 8, ip4, mac, out));
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_layout_challenge_ip_mac_padding);
      RUN_TEST(test_rejects_wrong_challenge_length);
      return UNITY_END();
  }
  ```

- [ ] **Step 3: Run — expect FAIL to compile.**

- [ ] **Step 4: Write `components/raop/src/raop_challenge.c`:**
  ```c
  #include "raop_challenge.h"
  #include <string.h>

  int raop_challenge_assemble(const uint8_t *challenge, size_t clen,
                              const uint8_t ip4[4], const uint8_t mac[6],
                              uint8_t out[RAOP_CHALLENGE_BUF_LEN]) {
      if (clen != 16) return -1;               // AirPlay challenge is always 16 bytes
      memset(out, 0, RAOP_CHALLENGE_BUF_LEN);  // zero-pad tail
      memcpy(out + 0,  challenge, 16);
      memcpy(out + 16, ip4, 4);
      memcpy(out + 20, mac, 6);
      return 26;                               // 16 + 4 + 6 used; [26..31] stay zero
  }
  ```

- [ ] **Step 5: Run — expect PASS.**

- [ ] **Step 6: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/components/raop/src/raop_challenge.h \
          framework/components/raop/src/raop_challenge.c \
          framework/test/test_raop_challenge/test_raop_challenge.c
  git commit -m "feat(raop): pure Apple-Challenge pre-signature assembly with host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 5: `raop_key.c` — embed the public RAOP RSA private key (target-only)

**Files:** create `components/raop/src/raop_key.c` (and the `extern` decl goes in `raop_crypto.h`, Task 6).

- [ ] **Step 1: Obtain the key verbatim.** Fetch the `super_secret_key` constant from the pinned source and
  copy it **byte-for-byte** (do not retype the base64):
  ```bash
  # WebFetch or curl the raw file, then copy the PEM between the BEGIN/END markers:
  #   https://raw.githubusercontent.com/mikebrady/shairport-sync/3.3.9/common.c
  ```
  Confirm the anchors match this plan: body starts `MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt…`
  and ends `…2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=`.

- [ ] **Step 2: Write `components/raop/src/raop_key.c`:**
  ```c
  // The well-known "AirPort Express" RAOP RSA private key. This is PUBLIC knowledge
  // — it ships verbatim in every open RAOP receiver (shairport / shairport-sync /
  // owntone / RPiPlay). It is NOT a secret: AirPlay 1 has no pairing, and every
  // receiver must present this same key to satisfy the sender's Apple-Challenge
  // (spec §9 documents this as an accepted, inherent RAOP trade-off).
  //
  // Source (verbatim): shairport-sync 3.3.9, common.c `super_secret_key`.
  // A NUL terminator is required by mbedtls_pk_parse_key; use sizeof(raop_key_pem).
  const char raop_key_pem[] =
      "-----BEGIN RSA PRIVATE KEY-----\n"
      "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
      /* ... paste the remaining base64 lines VERBATIM from the source ... */
      "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
      "-----END RSA PRIVATE KEY-----\n";

  const unsigned int raop_key_pem_len = sizeof(raop_key_pem);  // includes the NUL
  ```
  (No standalone build/commit here — it's compiled and self-tested together with Task 6, then committed there.)

---

## Task 6: `raop_crypto` — PSA key import, challenge sign, AES-key decrypt (target-only)

**Files:** create `components/raop/src/raop_crypto.h`, `.c`. Uses **mbedTLS 4.0 PSA Crypto** per the
toolchain finding above.

- [ ] **Step 1: Write `components/raop/src/raop_crypto.h`:**
  ```c
  // RAOP crypto glue over mbedTLS 4.0 PSA Crypto (target-only; not host-testable —
  // pure framing/layout is covered by base64/raop_challenge tests). Loads the
  // well-known RAOP RSA key once, then signs Apple-Challenges and RSA/OAEP-decrypts
  // the AES session key.
  #pragma once
  #include <stddef.h>
  #include <stdint.h>
  #include "esp_err.h"

  extern const char raop_key_pem[];          // defined in raop_key.c
  extern const unsigned int raop_key_pem_len;

  // Init PSA, parse the RAOP PEM, import two PSA keys (sign + decrypt), and run a
  // self-test round-trip so a corrupted embedded key fails loud. Call once at boot.
  esp_err_t raop_crypto_init(void);

  // RSA/PKCS1-v1.5 raw-sign the 32-byte pre-signature buffer (PSA_ALG_RSA_PKCS1V15_
  // SIGN_RAW). Writes the 256-byte signature to `sig`; returns 0 on success.
  int raop_crypto_sign_challenge(const uint8_t buf32[32],
                                 uint8_t *sig, size_t sig_cap, size_t *sig_len);

  // RSA/OAEP-SHA1 decrypt the RSA-encrypted AES session key (PSA_ALG_RSA_OAEP(SHA_1)).
  // Requires the decrypted length to be exactly 16; writes it to out16. Returns 0 on success.
  int raop_crypto_decrypt_aeskey(const uint8_t *in, size_t in_len, uint8_t out16[16]);
  ```

- [ ] **Step 2: Write `components/raop/src/raop_crypto.c`.** Precise steps (pinned to the verified 4.0 API):
  ```c
  #include "raop_crypto.h"
  #include "psa/crypto.h"
  #include "mbedtls/pk.h"
  #include "esp_log.h"
  #include <string.h>

  static const char *TAG = "raop_crypto";
  static mbedtls_svc_key_id_t s_sign_key;  // usage SIGN_HASH, alg PKCS1V15_SIGN_RAW
  static mbedtls_svc_key_id_t s_dec_key;   // usage DECRYPT|ENCRYPT, alg RSA_OAEP(SHA_1)

  // Import the parsed PEM into a PSA key with the given usage + algorithm policy.
  static esp_err_t import_key(mbedtls_pk_context *pk, psa_key_usage_t usage,
                              psa_algorithm_t alg, mbedtls_svc_key_id_t *out) {
      psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
      // Seed type/bits/curve from the parsed key, requesting `usage`...
      if (mbedtls_pk_get_psa_attributes(pk, PSA_KEY_USAGE_SIGN_HASH, &attr) != 0) return ESP_FAIL;
      // ...then pin the exact RAOP policy (override the pk-suggested defaults):
      psa_set_key_usage_flags(&attr, usage);
      psa_set_key_algorithm(&attr, alg);
      int rc = mbedtls_pk_import_into_psa(pk, &attr, out);
      psa_reset_key_attributes(&attr);
      return rc == 0 ? ESP_OK : ESP_FAIL;
  }

  esp_err_t raop_crypto_init(void) {
      if (psa_crypto_init() != PSA_SUCCESS) { ESP_LOGE(TAG, "psa_crypto_init"); return ESP_FAIL; }

      mbedtls_pk_context pk;
      mbedtls_pk_init(&pk);
      // mbedTLS 4.0 signature: (ctx, key, keylen, pwd, pwdlen). keylen INCLUDES the NUL for PEM.
      int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)raop_key_pem, raop_key_pem_len, NULL, 0);
      if (rc != 0) { ESP_LOGE(TAG, "pk_parse_key: -0x%04x (embedded key corrupt?)", -rc);
                     mbedtls_pk_free(&pk); return ESP_FAIL; }

      esp_err_t e1 = import_key(&pk, PSA_KEY_USAGE_SIGN_HASH,
                                PSA_ALG_RSA_PKCS1V15_SIGN_RAW, &s_sign_key);
      // Decrypt key also gets ENCRYPT so the self-test can round-trip with the public part.
      esp_err_t e2 = import_key(&pk, PSA_KEY_USAGE_DECRYPT | PSA_KEY_USAGE_ENCRYPT,
                                PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), &s_dec_key);
      mbedtls_pk_free(&pk);
      if (e1 != ESP_OK || e2 != ESP_OK) { ESP_LOGE(TAG, "import_into_psa failed"); return ESP_FAIL; }

      // --- Self-test (fails loud on a corrupted key; no real sender needed) ---
      // (a) sign a fixed 32-byte buffer -> expect a 256-byte signature.
      uint8_t buf32[32] = {0}, sig[256]; size_t siglen = 0;
      if (raop_crypto_sign_challenge(buf32, sig, sizeof(sig), &siglen) != 0 || siglen != 256) {
          ESP_LOGE(TAG, "self-test sign failed (siglen=%u)", (unsigned)siglen); return ESP_FAIL;
      }
      // (b) OAEP encrypt-then-decrypt a known 16-byte AES key -> expect it back.
      const uint8_t sample[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
      uint8_t ct[256], pt[16]; size_t ctlen = 0;
      if (psa_asymmetric_encrypt(s_dec_key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1),
                                 sample, 16, NULL, 0, ct, sizeof(ct), &ctlen) != PSA_SUCCESS) {
          ESP_LOGE(TAG, "self-test encrypt failed"); return ESP_FAIL;
      }
      if (raop_crypto_decrypt_aeskey(ct, ctlen, pt) != 0 || memcmp(pt, sample, 16) != 0) {
          ESP_LOGE(TAG, "self-test decrypt round-trip mismatch"); return ESP_FAIL;
      }
      ESP_LOGI(TAG, "RAOP key loaded; sign(256)+OAEP round-trip OK");
      return ESP_OK;
  }

  int raop_crypto_sign_challenge(const uint8_t buf32[32],
                                 uint8_t *sig, size_t sig_cap, size_t *sig_len) {
      psa_status_t s = psa_sign_hash(s_sign_key, PSA_ALG_RSA_PKCS1V15_SIGN_RAW,
                                     buf32, 32, sig, sig_cap, sig_len);
      if (s != PSA_SUCCESS) { ESP_LOGE(TAG, "psa_sign_hash: %d", (int)s); return -1; }
      return 0;
  }

  int raop_crypto_decrypt_aeskey(const uint8_t *in, size_t in_len, uint8_t out16[16]) {
      uint8_t tmp[32]; size_t olen = 0;
      psa_status_t s = psa_asymmetric_decrypt(s_dec_key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1),
                                              in, in_len, NULL, 0, tmp, sizeof(tmp), &olen);
      if (s != PSA_SUCCESS) { ESP_LOGE(TAG, "psa_asymmetric_decrypt: %d", (int)s); return -1; }
      if (olen != 16) { ESP_LOGE(TAG, "AES key len %u != 16", (unsigned)olen); return -1; }
      memcpy(out16, tmp, 16);
      return 0;
  }
  ```
  > **Note on `mbedtls_pk_get_psa_attributes` usage arg:** we pass `PSA_KEY_USAGE_SIGN_HASH` only to make it
  > populate the RSA type/bit-size, then immediately override usage+algorithm with the exact RAOP policy. If
  > the installed header rejects that pattern, import each key from the same parsed `pk`; `mbedtls_pk_free`
  > is called only after both imports. Confirm the three function names against the grep in the toolchain note.

- [ ] **Step 3: Component skeleton + first target build.** Create `components/raop/CMakeLists.txt` and the
  public header now so the component links (server task lands in Task 8 — for this build the CMake lists only
  the files created so far, or stub `raop.c`; simplest is to add all files as they are created and do the full
  build in Task 9). For an isolated crypto check, temporarily register only the created sources:
  ```cmake
  idf_component_register(
      SRCS "src/base64.c" "src/rtsp_parser.c" "src/sdp.c" "src/raop_challenge.c"
           "src/raop_key.c" "src/raop_crypto.c"
      INCLUDE_DIRS "include"
      PRIV_INCLUDE_DIRS "src"
      PRIV_REQUIRES mbedtls
  )
  ```
  (Task 9 finalizes SRCS + PRIV_REQUIRES with the socket/session/server files.) A component with no source
  referenced by the app still compiles; to force it into the link, Task 10's `main.cpp` call pulls it in.
  For now:
  ```bash
  rm -rf framework/.pio/build/esp32-s3-n16r8
  cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
  ```
  Expected: `[SUCCESS]`. (`raop_crypto_init` isn't called yet; this just proves PSA/pk compile + link.)

- [ ] **Step 4: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/components/raop/src/raop_key.c \
          framework/components/raop/src/raop_crypto.h \
          framework/components/raop/src/raop_crypto.c \
          framework/components/raop/CMakeLists.txt \
          framework/components/raop/include/raop.h   # if the stub header already exists
  git commit -m "feat(raop): RAOP RSA key + PSA challenge-sign/AES-key-decrypt (mbedTLS 4.0)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 7: `rtsp_session` — session state + UDP socket bind (target-only)

**Files:** create `components/raop/src/rtsp_session.h`, `.c`.

- [ ] **Step 1: Write `components/raop/src/rtsp_session.h`:**
  ```c
  // Per-connection RAOP session state + the three receiver-side UDP sockets. The
  // RTP receive loop that reads these sockets is Phase 3; Phase 2 binds them and
  // reports their local ports in the SETUP Transport response.
  #pragma once
  #include <stdint.h>
  #include <stdbool.h>

  typedef enum {
      RAOP_IDLE = 0,   // no ANNOUNCE yet
      RAOP_ANNOUNCED,  // AES key/iv + fmtp stored
      RAOP_SETUP,      // UDP sockets bound, ports exchanged
      RAOP_RECORDING,  // RECORD received; streaming would be live (Phase 3)
  } raop_state_t;

  typedef struct {
      raop_state_t state;

      uint8_t  aeskey[16];      // decrypted AES session key
      uint8_t  aesiv[16];       // AES-CBC IV
      bool     have_key;
      char     fmtp[160];       // ALAC magic-cookie params, kept for Phase 3

      // Sender-advertised ports (from the SETUP Transport header).
      int      client_control_port;
      int      client_timing_port;

      // Receiver-side sockets we bind + their local ports (reported back in Transport).
      int      audio_fd,  control_fd,  timing_fd;
      uint16_t audio_port, control_port, timing_port;
  } raop_session_t;

  void raop_session_reset(raop_session_t *s);          // to IDLE; closes any open sockets
  // Bind three UDP sockets to ephemeral ports; fills *_fd and *_port. Returns 0 on success.
  int  raop_session_bind_udp(raop_session_t *s);
  void raop_session_close_udp(raop_session_t *s);       // close fds, zero ports
  ```

- [ ] **Step 2: Write `components/raop/src/rtsp_session.c`.** Notes:
  - `raop_session_reset`: `raop_session_close_udp(s)` then `memset(s,0,sizeof)` and set `state=RAOP_IDLE`,
    `audio_fd=control_fd=timing_fd=-1`.
  - `raop_session_bind_udp`: for each of the three, `socket(AF_INET, SOCK_DGRAM, 0)`; `bind` to
    `sin_addr=INADDR_ANY, sin_port=0` (ephemeral); `getsockname` to read the assigned port into
    `*_port` (`ntohs`). On any failure, close what opened and return -1. lwIP BSD sockets:
    `#include "lwip/sockets.h"`.
  - `raop_session_close_udp`: `close()` each fd if `>= 0`, set to -1, zero the ports.

- [ ] **Step 3:** No standalone build/commit — built and committed with Task 8/9 (the server that drives it).

---

## Task 8: `raop.c` + `include/raop.h` — RTSP accept/recv task + method dispatch (target-only)

**Files:** create `components/raop/include/raop.h`, `components/raop/src/raop.c`.

- [ ] **Step 1: Write the public API `components/raop/include/raop.h`:**
  ```c
  // AirPlay-1 (RAOP) receiver: RTSP control server. Public surface is deliberately
  // tiny — main starts it after Wi-Fi GOT_IP (once esp_netif + the advertised
  // _raop._tcp service exist). Single session at a time (spec §8: a second sender
  // gets 453 Busy). The RTP audio path is Phase 3.
  #pragma once

  // Start the RTSP server task (listens on RAOP_RTSP_PORT = 5000). Idempotent-safe
  // to call once. Runs raop_crypto_init() internally on first start.
  void raop_server_start(void);

  // Stop the server task and tear down any live session (used on Wi-Fi LOST_IP later).
  void raop_server_stop(void);
  ```

- [ ] **Step 2: Write `components/raop/src/raop.c`.** Structure (one FreeRTOS task; blocking TCP is fine at
  RTSP's low rate per spec §5d threading):
  - **Includes:** `lwip/sockets.h`, `freertos/FreeRTOS.h`, `freertos/task.h`, `esp_log.h`, `esp_mac.h`,
    `string.h`, and the local `rtsp_parser.h`, `sdp.h`, `base64.h`, `raop_challenge.h`, `raop_crypto.h`,
    `rtsp_session.h`, `mdns_service.h` (for `RAOP_RTSP_PORT`).
  - **`raop_server_start`:** if already started, return. `raop_crypto_init()` — on failure log and **do not**
    start (can't negotiate without the key). `xTaskCreate(server_task, "raop_rtsp", 6144, NULL, 5, &s_task)`.
  - **`server_task`:** create the listen socket: `socket(AF_INET, SOCK_STREAM, 0)`, `setsockopt(SO_REUSEADDR)`,
    `bind` to `INADDR_ANY:RAOP_RTSP_PORT`, `listen(fd, 1)`. Loop: `accept()`. When a client connects, **if a
    session is already active** (a second sender), immediately write a `453 Not Enough Bandwidth` response and
    `close()` the new socket (spec §8 single-session). Otherwise mark busy and enter `handle_connection(fd)`.
    On return, `raop_session_reset(&s_session)`, clear busy, loop back to `accept()`.
  - **`handle_connection(int fd)`:** a recv buffer (e.g. `char rx[2048]`). Loop:
    - `recv()` more bytes appended to `rx` (track `used`). If `recv <= 0` → sender vanished → return
      (spec §8: dead TCP → teardown).
    - `rtsp_parse_request(rx, used, &req)`; if it returns `false`, the request/body isn't fully in yet —
      keep reading (guard against exceeding `sizeof(rx)`; if a single request overflows the buffer, respond
      `400` and close — bound untrusted input, §9).
    - Once complete, compute the full request length (`headers end + body_len`), `dispatch(fd, &req)`, then
      **remove the consumed bytes** from `rx` (memmove the remainder) so pipelined requests are handled.
  - **`dispatch(fd, req)`** — `strcmp(req->method, ...)`:
    - **OPTIONS:** build a `200 OK` with `Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS,
      GET_PARAMETER, SET_PARAMETER`. If `rtsp_header_get(req,"Apple-Challenge")` present →
      `base64_decode` it (expect 16 bytes); `getsockname(fd)` for local IPv4; `esp_read_mac(mac,
      ESP_MAC_WIFI_STA)`; `raop_challenge_assemble(chal,16,ip4,mac,buf32)`; `raop_crypto_sign_challenge(buf32,
      sig,256,&siglen)`; `base64_encode(sig,256,b64,...)`; **truncate at the first `'='`**; add header
      `Apple-Response: <b64>`. Send.
    - **ANNOUNCE:** `sdp_parse(req->body, req->body_len, &m)`. If `m.has_rsaaeskey`: `base64_decode` →
      `raop_crypto_decrypt_aeskey` → `s_session.aeskey`. If `m.has_aesiv`: `base64_decode` (expect 16) →
      `s_session.aesiv`; set `have_key`. Copy `m.fmtp` → `s_session.fmtp`. `state = RAOP_ANNOUNCED`. `200 OK`.
      On any decode/decrypt failure, respond `400` and reset (rate-limited log; never crash — §8/§9).
    - **SETUP:** `t = rtsp_header_get(req,"Transport")`; `client_control_port =
      rtsp_transport_port(t,"control_port")`, `client_timing_port = rtsp_transport_port(t,"timing_port")`.
      `raop_session_bind_udp(&s_session)`. Respond `200 OK` with
      `Transport: RTP/AVP/UDP;unicast;mode=record;server_port=<audio>;control_port=<control>;timing_port=<timing>\r\n`
      and `Session: 1\r\n`. `state = RAOP_SETUP`.
    - **RECORD:** `state = RAOP_RECORDING`; optionally read `RTP-Info` (seq/rtptime) header for Phase 3.
      Respond `200 OK` with `Audio-Latency: 11025\r\n` (a sane constant; refined in Phase 3/4).
    - **SET_PARAMETER / GET_PARAMETER / FLUSH / PAUSE:** `200 OK` (volume/metadata + real flush are Phase 5/3).
    - **TEARDOWN:** `raop_session_close_udp`, `raop_session_reset`, respond `200 OK`, then return from
      `handle_connection` so the task loops back to `accept()` and releases the single-session lock.
    - **default (unknown):** `501 Not Implemented`.
    - Every response echoes `CSeq` via `rtsp_build_response(..., rtsp_cseq(req), ...)`.
  - **`raop_server_stop`:** signal the task to exit (close the listen fd + a flag), `raop_session_reset`.
  - Keep a single file-static `raop_session_t s_session;` and a `volatile bool s_busy;` guard. Log every
    method + CSeq at INFO, rate-limit error logs (spec §8).

- [ ] **Step 3:** Built + committed in Task 9 (server needs the finalized CMake + session file linked).

---

## Task 9: Component wiring — CMakeLists, README, target build (green)

**Files:** finalize `components/raop/CMakeLists.txt`; create `components/raop/README.md`.

- [ ] **Step 1: Finalize `components/raop/CMakeLists.txt`:**
  ```cmake
  # raop: the AirPlay-1 (RAOP) protocol plugin — RTSP control server, RSA challenge,
  # AES session-key exchange, UDP socket bind. (RTP audio receive/decode is Phase 3.)
  idf_component_register(
      SRCS "src/raop.c"
           "src/rtsp_parser.c"
           "src/rtsp_session.c"
           "src/raop_crypto.c"
           "src/raop_key.c"
           "src/base64.c"
           "src/sdp.c"
           "src/raop_challenge.c"
      INCLUDE_DIRS "include"
      PRIV_INCLUDE_DIRS "src"
      PRIV_REQUIRES mbedtls lwip esp_netif esp_hw_support esp_timer discovery
  )
  ```
  > REQUIRES rationale (public `raop.h` exposes only `void` functions → everything private):
  > `mbedtls` (PSA + pk), `lwip` (BSD sockets), `esp_hw_support` (`esp_read_mac`/`esp_mac.h`),
  > `esp_netif` (kept in case ip info is needed; drop if unused after implementation),
  > `esp_timer` (dead-peer/backoff timers if added), `discovery` (for `RAOP_RTSP_PORT` from `mdns_service.h`).
  > **After editing REQUIRES, wipe the build dir** before rebuilding (stale CMake cache keeps old include paths):
  > `rm -rf framework/.pio/build/esp32-s3-n16r8`.

- [ ] **Step 2: Write `components/raop/README.md`** — document: the RTSP state machine
  (`OPTIONS→ANNOUNCE→SETUP→RECORD`, plus SET/GET_PARAMETER/FLUSH/TEARDOWN), the pure vs. glue split, the PSA
  (mbedTLS 4.0) crypto approach and why it differs from shairport's low-level RSA calls, the single-session
  453 rule, the public RAOP key trade-off (spec §9), and the Phase 3 boundary (RTP receive/decrypt/decode).

- [ ] **Step 3: Full target build:**
  ```bash
  rm -rf framework/.pio/build/esp32-s3-n16r8
  cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
  ```
  Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/components/raop/CMakeLists.txt \
          framework/components/raop/README.md \
          framework/components/raop/include/raop.h \
          framework/components/raop/src/rtsp_session.h \
          framework/components/raop/src/rtsp_session.c \
          framework/components/raop/src/raop.c
  git commit -m "feat(raop): RTSP server task, session state machine, UDP bind, method dispatch

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 10: `main.cpp` integration — start the RTSP server after mDNS advertise

**Files:** modify `framework/src/main.cpp`.

- [ ] **Step 1: Add the include** next to the others: `#include "raop.h"`.

- [ ] **Step 2: Call `raop_server_start()` in `on_got_ip`, after the mDNS advertise:**
  ```c
  static void on_got_ip(void)
  {
      // esp_netif is up now — safe to advertise. Hostname "conduit" -> conduit.local.
      mdns_advertise_raop("conduit", system_config_get_instance_name(), RAOP_RTSP_PORT);
      // Phase 2: now actually man the advertised RTSP port so a sender can connect.
      raop_server_start();
  }
  ```
  Leave everything else (`audio_init`, `audio_diag_tone_start`, NVS, config, Wi-Fi, the no-creds branch) intact.

- [ ] **Step 3: Build + regression host tests:**
  ```bash
  rm -rf framework/.pio/build/esp32-s3-n16r8
  cd framework && ~/.platformio/penv/bin/pio run -e esp32-s3-n16r8
  cd framework && ~/.platformio/penv/bin/pio test -e native
  ```
  Expected: target `[SUCCESS]`; host tests **all green** — `test_ringbuf`, `test_device_id`, `test_raop_txt`,
  `test_base64`, `test_rtsp_parser`, `test_sdp`, `test_raop_challenge`.

- [ ] **Step 4: Commit:**
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git add framework/src/main.cpp
  git commit -m "feat(main): start RAOP RTSP server on GOT_IP after mDNS advertise

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
  ```

---

## Task 11: On-target verification (proves a sender negotiates) + tag

**Files:** none (verification only — no hardware flashing in this workflow; record the steps for whoever has the board).

- [ ] **Step 1:** Flash with real 2.4 GHz creds (`menuconfig` → *Conduit Stream Configuration*). Open the serial log.
- [ ] **Step 2:** Expected boot log adds, after the Phase 1 lines: `raop_crypto: RAOP key loaded; sign(256)+OAEP
  round-trip OK` and `raop: RTSP listening on :5000`.
- [ ] **Step 3:** From a Mac on the same LAN, drive the handshake without an iPhone using a scripted RTSP OPTIONS
  with an `Apple-Challenge`, or simply select the speaker in the **AirPlay menu** and press play: the log should
  show `OPTIONS`(+`Apple-Response` sent) → `ANNOUNCE`(AES key decrypted, 16 bytes) → `SETUP`(UDP ports bound,
  Transport returned) → `RECORD`(state RECORDING). No audio yet (Phase 3) — the sender connects and the
  progress bar may start; that is the Phase 2 success criterion. The 440 Hz tone keeps sounding throughout.
- [ ] **Step 4:** Second-sender test: a concurrent connection attempt gets `453` and is refused (single session).
- [ ] **Step 5:** Tag the phase complete:
  ```bash
  cd /Users/uziiuzair/ooozzy/conduit-stream
  git tag -a v0.2-phase2 -m "Phase 2: RTSP state machine + RSA challenge + AES key exchange; sender negotiates"
  ```

---

## Self-Review

**Deliverable coverage (Phase 2 goal / task brief, 5 items):**
1. RTSP TCP server on 5000, single session (453 on a second sender), one FreeRTOS accept/recv task, started
   from `on_got_ip` after mDNS — Task 8 + Task 10. ✓
2. Pure, host-tested, bounds-checked RTSP request parser (request line, case-insensitive headers incl. CSeq /
   Content-Type / Content-Length / Apple-Challenge, Content-Length-clamped body) + CSeq-echoing response
   builder — Task 2. ✓
3. Method state machine OPTIONS(+challenge) / ANNOUNCE(SDP → AES key decrypt + fmtp store) / SETUP(Transport
   parse + UDP bind + Transport response) / RECORD / FLUSH / SET_PARAMETER / GET_PARAMETER / TEARDOWN /
   501-unknown — Tasks 7 + 8. ✓
4. Crypto: embedded public RAOP RSA key (Task 5), **PSA-Crypto** challenge sign + AES-key OAEP decrypt for
   **mbedTLS 4.0** (Task 6), pure host-tested base64 (Task 1) and challenge assembly (Task 4). ✓
5. Host unit tests for **all** pure logic — base64 vectors, RTSP parse/build/transport, SDP extraction,
   challenge-assembly layout — Tasks 1–4. ✓

**Interop honesty (task brief):** the RSA/AES scheme matches shairport-sync `3.3.9` exactly (cited above) —
same 32-byte challenge layout, same `Apple-Response` header, PKCS#1-v1.5 raw sign, RSA/OAEP-SHA1 key decrypt,
same public key. The **only** deviation from shairport's code is the API surface: mbedTLS 4.0 forced the move
from the (now-private) low-level `mbedtls_rsa_*` to PSA Crypto — the algorithms are byte-identical
(`PSA_ALG_RSA_PKCS1V15_SIGN_RAW` ≡ `MD_NONE` sign; `PSA_ALG_RSA_OAEP(SHA_1)` ≡ `PKCS_V21/MD_SHA1` decrypt).
No crypto is faked: Task 6's boot self-test round-trips the real embedded key and aborts loud on corruption.

**Security (spec §9):** every parser copy is `snprintf`-bounded and NUL-terminated; base64 decode bounds its
output; the RTSP recv buffer is capped and over-long requests get `400`; body is clamped to bytes present;
error logs are rate-limited; the public-key / no-pairing trade-off is documented, not hidden.

**REQUIRES review:** public `raop.h` exposes only `void` functions, so all IDF/mbedTLS deps are `PRIV_REQUIRES`
(`mbedtls lwip esp_netif esp_hw_support esp_timer discovery`); `discovery` supplies `RAOP_RTSP_PORT`. Each
REQUIRES edit is paired with a `.pio/build/esp32-s3-n16r8` wipe per the CLAUDE.md gotcha. `platformio.ini`
native env gains `-I components/raop/src` so the four pure units host-compile.

**Untouched:** `audio` (Phase 0 producer contract intact — no audio producer added), `wifi`, `system`,
`discovery` (only its `RAOP_RTSP_PORT` constant is consumed). The 440 Hz diagnostic tone path is unchanged.

**Deferred to later phases (not Phase 2):** RTP receive + AES-CBC decrypt + ALAC decode → PCM (Phase 3, which
consumes `s_session.aeskey/aesiv/fmtp` and reads the bound UDP sockets); retransmit + underrun/drift (Phase 4);
volume + DAAP metadata + RGB LED (Phase 5). The UDP sockets are bound now but not yet read.
