# `raop` — AirPlay 1 (RAOP) protocol plugin

The only component that knows AirPlay exists. Phase 2 stands up the **RTSP control
server** so a real iPhone/Mac connects and negotiates a session:
`OPTIONS → ANNOUNCE → SETUP → RECORD`. **Phase 3 makes it audible**: on `RECORD`
an RTP receive task reads the bound audio UDP socket, AES-128-CBC-decrypts each
packet, ALAC-decodes it to PCM, and feeds `audio_play_pcm()` — the first AirPlay
audio out the DAC.

## RTSP state machine

| Method | Action |
|---|---|
| `OPTIONS` | `200 OK` + `Public:` list. If an `Apple-Challenge` header is present, RSA-sign it and return `Apple-Response`. |
| `ANNOUNCE` | Parse SDP; RSA/OAEP-decrypt `a=rsaaeskey` → 16-byte AES key; base64-decode `a=aesiv` → 16-byte IV; keep `a=fmtp` (ALAC config). `state = ANNOUNCED`. |
| `SETUP` | Parse the sender's `control_port` / `timing_port` from `Transport`; bind our three UDP sockets (audio/control/timing) to ephemeral ports; return them in the `Transport` response. `state = SETUP`. |
| `RECORD` | **Phase 3:** require key + `fmtp` (else `400`); **stop the diag tone**, then start the RTP decode task (`raop_rtp_start`). `state = RECORDING`. Idempotent on a second `RECORD`. |
| `FLUSH` / `PAUSE` | `200 OK` ack. FLUSH affects buffering only (jitter buffer is Phase 4) and does **not** tear the decoder down. |
| `SET_PARAMETER` / `GET_PARAMETER` | `200 OK` ack (volume/metadata → Phase 5). |
| `TEARDOWN` | Stop the RTP decoder, **resume the diag tone**, close the UDP sockets, reset the session to `IDLE`, release the single-session lock. |
| _unknown_ | `501 Not Implemented`. |

Every response echoes the request's `CSeq`. Every abnormal exit (idle-timeout,
disconnect, buffer overflow, server stop) routes through `session_teardown_full()`,
which performs the same decoder-stop + tone-resume + reset.

## Phase 3 — RTP audio receive path

`raop_rtp.{h,c}` owns a FreeRTOS task (pinned to `audio_producer_core()`, prio
above the diag tone) that per datagram:

1. `rtp_parse()` the 12-byte header; dispatch on `packet[1] & 0x7f`. Only the audio
   type (`0x60`) is handled — sync/timing/resend are Phase 4+.
2. `aes_frame_split()` the payload: the largest multiple of 16 is ciphertext, the
   trailing `len % 16` bytes are **plaintext** copied verbatim.
3. AES-128-CBC-decrypt the ciphertext with the session key, **resetting the IV to
   the constant session IV for every packet** (CBC chains within a packet, never
   across). PSA Crypto: `psa_cipher_decrypt_setup(PSA_ALG_CBC_NO_PADDING)` +
   `set_iv` + `update` + `finish`.
4. `alac_decode_frame()` the reconstructed ALAC frame → interleaved LE int16 stereo
   PCM. (Decoder built from `a=fmtp` via `alac_cfg_from_fmtp`; see `components/alac`.)
5. Feed `audio_play_pcm()` with backpressure (yield+retry on a full ring).

Phase 3 free-runs: packets decode in arrival order straight into the PCM ring — no
jitter buffer / reorder / timing / sync / retransmit (all Phase 4). Wire formats
match shairport-sync `rtp.c`/`player.c` (see the plan and `research-phase3-5.md`).

### Single-producer arbitration (correctness-critical)

`audio_play_pcm()` allows exactly **one** producer. The 440 Hz diag tone and the
RAOP decode task are both producers, so `RECORD` does `audio_diag_tone_stop()`
(blocks until the tone task exits and releases the audio path) **before**
`raop_rtp_start()`; teardown reverses it. Both producers pin to
`audio_producer_core()` and bracket their run with
`audio_producer_acquire()`/`release()` (a second concurrent producer is rejected
and logged).

## Pure vs. glue split (the house rule)

Everything host-testable lives in ESP-IDF-free, mbedTLS-free `.c` files that are
`#include`d directly into their Unity test TU (mirrors `audio_ringbuf.c`):

- `base64.{h,c}` — RFC 4648 encode/decode → `test/test_base64`
- `rtsp_parser.{h,c}` — request parse + response build + Transport-port extract → `test/test_rtsp_parser`
- `sdp.{h,c}` — `a=rsaaeskey` / `a=aesiv` / `a=fmtp` extraction → `test/test_sdp`
- `raop_challenge.{h,c}` — the 32-byte pre-signature buffer layout → `test/test_raop_challenge`
- `rtp_parser.{h,c}` — 12-byte RTP header parse + type dispatch → `test/test_rtp_parser`
- `aes_frame.{h,c}` — the AES-CBC ciphertext/plaintext-tail split → `test/test_aes_frame`
- (`alac_config.{h,c}` in `components/alac` — `a=fmtp` → ALAC config → `test/test_alac_config`)

Target-build-only glue (sockets / PSA / MAC / FreeRTOS / decoder):

- `raop.c` + `include/raop.h` — the accept/recv task + method dispatch (`raop_server_start`).
- `rtsp_session.{h,c}` — per-session state + the three UDP sockets.
- `raop_crypto.{h,c}` — PSA Crypto (mbedTLS 4.0) key import, challenge sign, AES-key decrypt.
- `raop_key.c` — the embedded RAOP RSA private key.
- `raop_rtp.{h,c}` — Phase 3 RTP receive/decrypt/decode task + diag-tone handoff.

## Crypto — mbedTLS 4.0 PSA Crypto (differs from shairport's low-level RSA)

The algorithm matches **shairport-sync 3.3.9** (`rtsp.c::apple_challenge` /
`handle_announce`, `common.c::rsa_apply`), but the API does not. ESP-IDF 6.0.1
ships **mbedTLS 4.0.0**, where the legacy `mbedtls_rsa_*` API is private and
`mbedtls_pk_rsa()` is gone — shairport's `mbedtls_rsa_set_padding` + `rsa_pkcs1_*`
approach will not compile. We use the public **PSA Crypto** API instead, bridged
from the parsed PEM via `mbedtls_pk_get_psa_attributes` → `mbedtls_pk_import_into_psa`:

| RAOP operation | shairport (mbedTLS ≤2.x) | ours (mbedTLS 4.0 PSA) |
|---|---|---|
| Apple-Challenge sign | `set_padding(V15, MD_NONE)` + `rsa_pkcs1_sign` | `psa_sign_hash(k, PSA_ALG_RSA_PKCS1V15_SIGN_RAW, buf32, 32, …)` |
| AES-key decrypt | `set_padding(V21, MD_SHA1)` + `rsa_pkcs1_decrypt` | `psa_asymmetric_decrypt(k, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), …)` |

`PSA_ALG_RSA_PKCS1V15_SIGN_RAW` (PKCS#1 v1.5, no DigestInfo prefix) is byte-for-byte
shairport's `MD_NONE` sign; `PSA_ALG_RSA_OAEP(SHA_1)` is its `PKCS_V21/MD_SHA1`
decrypt. The 2048-bit key gives a 256-byte signature/ciphertext and a 16-byte AES key.

`raop_crypto_init()` runs a **boot self-test** (sign → 256 bytes; OAEP
encrypt-then-decrypt a known 16-byte value → matches) so a corrupted embedded key
fails loud instead of silently mis-negotiating. If it fails, the RTSP server is not
started.

### Apple-Challenge byte layout (interop-critical, IPv4)

```
32-byte buffer, zero-filled:
  [0..15]  decoded Apple-Challenge (16 bytes)
  [16..19] local IPv4 from getsockname() on the client socket (network order)
  [20..25] STA MAC (esp_read_mac ESP_MAC_WIFI_STA)
  [26..31] zero padding
→ PKCS#1-v1.5 raw sign → base64 → strip at first '=' → "Apple-Response"
```

Header name is **`Apple-Response`** (what real senders read), not `RSA-Response`.

## Single session (spec §8)

The task `select()`s on the listen socket and the active client socket together, so
while one sender is connected a second connection is accepted only long enough to be
answered **`453 Not Enough Bandwidth`** and closed.

## Security note (spec §9)

RTSP/SDP input is untrusted LAN data: the recv buffer is capped (over-long request →
`400`), the body is clamped to `Content-Length`, every copy is `snprintf`/bounds
checked, and base64 decode bounds its output. **The RAOP RSA private key is public**
(it ships in every open receiver) and AirPlay 1 has no pairing — anyone on the LAN can
stream. This is inherent to RAOP and an accepted trade-off for a home speaker.

## Public API

```c
void raop_server_start(void);  // called from main on Wi-Fi GOT_IP, after mDNS advertise
void raop_server_stop(void);   // used on Wi-Fi LOST_IP (later phase)
```
