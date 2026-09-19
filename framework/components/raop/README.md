# `raop` — AirPlay 1 (RAOP) protocol plugin

The only component that knows AirPlay exists. Phase 2 stands up the **RTSP control
server** so a real iPhone/Mac connects and negotiates a session:
`OPTIONS → ANNOUNCE → SETUP → RECORD`. **Phase 3 makes it audible**: on `RECORD`
an RTP receive task reads the bound audio UDP socket, AES-128-CBC-decrypts each
packet, ALAC-decodes it to PCM, and feeds `audio_play_pcm()` — the first AirPlay
audio out the DAC. **Phase 4 makes it robust under real Wi-Fi loss**: a
seq-indexed reorder/jitter buffer replaces arrival-order decode, gaps trigger RTP
**retransmit** on the control socket, the **timing** channel is serviced
(receiver-initiated) so a real sender never tears us down, and a free-run **drift**
watermark keeps a multi-hour session from slowly underrunning.

## RTSP state machine

| Method | Action |
|---|---|
| `OPTIONS` | `200 OK` + `Public:` list. If an `Apple-Challenge` header is present, RSA-sign it and return `Apple-Response`. |
| `ANNOUNCE` | Parse SDP; RSA/OAEP-decrypt `a=rsaaeskey` → 16-byte AES key; base64-decode `a=aesiv` → 16-byte IV; keep `a=fmtp` (ALAC config). `state = ANNOUNCED`. |
| `SETUP` | Parse the sender's `control_port` / `timing_port` from `Transport`; bind our three UDP sockets (audio/control/timing) to ephemeral ports; return them in the `Transport` response. `state = SETUP`. |
| `RECORD` | Require key + `fmtp` (else `400`); **stop the diag tone**, then start the RTP decode task (`raop_rtp_start`, now a 3-socket select loop). `state = RECORDING`. Idempotent on a second `RECORD`. |
| `FLUSH` / `PAUSE` | `200 OK` ack. FLUSH affects buffering only and does **not** tear the decoder down. In Phase 4 the free-run reorder buffer re-anchors on the first packet after the flush gap (a precise `RTP-Info` re-anchor is a noted follow-up). |
| `SET_PARAMETER` / `GET_PARAMETER` | `200 OK` ack (volume/metadata → Phase 5). |
| `TEARDOWN` | Stop the RTP decoder, **resume the diag tone**, close the UDP sockets, reset the session to `IDLE`, release the single-session lock. |
| _unknown_ | `501 Not Implemented`. |

Every response echoes the request's `CSeq`. A 30-second idle timeout applies only
before `RECORD`; an active stream's RTSP channel may legitimately stay quiet while
UDP audio continues. Disconnect, pre-stream timeout, buffer overflow, and server
stop route through `session_teardown_full()`, which performs the same decoder-stop,
tone-resume, and reset sequence.

## Phase 4 — RTP audio receive path (reorder + retransmit + timing + drift)

`raop_rtp.{h,c}` owns ONE FreeRTOS task (pinned to `audio_producer_core()`, prio
above the diag tone) that `select()`s over the session's **three** UDP sockets and
keeps a single producer feeding `audio_play_pcm()`.

**Reorder-before-decode (the key decision).** Each RAOP audio packet is one
independently-decodable ALAC frame (no cross-frame decoder state), and a resend
response is itself an encrypted audio packet. So we buffer the **encrypted** payload
keyed by 16-bit RTP sequence (`rtp_reorder`, PSRAM, 256-packet ≈ 2 s window) and
drain in seq order → decrypt → decode → feed. Late / out-of-order / recovered
packets slot into place and flow through the **single** decode path — no duplicate
"decode a recovered packet" branch (spec §6c/§5c).

Per drained packet:

1. `aes_frame_split()` the payload: the largest multiple of 16 is ciphertext, the
   trailing `len % 16` bytes are **plaintext** copied verbatim.
2. AES-128-CBC-decrypt with the session key, **resetting the IV to the constant
   session IV for every packet** (CBC chains within a packet, never across). PSA
   Crypto: `decrypt_setup(PSA_ALG_CBC_NO_PADDING)` + `set_iv` + `update` + `finish`.
3. `alac_decode_frame()` → interleaved LE int16 stereo PCM (decoder built from
   `a=fmtp` via `alac_cfg_from_fmtp`; see `components/alac`).
4. Feed `audio_play_pcm()` with backpressure (yield+retry on a full ring).

**Gap → conceal, never stall.** If the front seq is missing but still inside the
hold window (`RTP_POP_WAIT`), the drain stops and waits (retransmit has time to
land). Once the write head runs past the hold budget (`RTP_POP_CONCEAL`), we give up
on the hole, push **one frame of silence** to preserve stream duration, and advance
— an unfillable gap is concealed, not stalled forever. All seq math is mod-2¹⁶ so it
survives 16-bit wraparound.

**Retransmit (control socket).** On a front gap, `rtp_reorder_gap()` gives
`[first, count]`; `rtp_resend_build()` emits the 8-byte `0x80 0xD5 htons(1)
htons(first) htons(count)` request to the **sender's** control port (throttled to
≤ every 30 ms per gap). A `0xD6` response is unwrapped (`rtp_resend_unwrap`: strip
the 4-byte wrapper, the inner bytes are a normal audio packet) and injected back
into the reorder buffer by seq.

**Timing (receiver-initiated).** Verified against shairport `rtp_timing_sender` /
`rtp_timing_receiver`: **the receiver is the initiator.** We periodically (~3 s) send
32-byte `0xD2` requests to the sender's timing port and consume the `0xD3` responses,
which we **discard** — we free-run (no clock discipline, spec §5c). A documented
defensive belt answers an inbound `0xD2` with a `0xD3` for non-standard senders that
poll us; standard senders never do. **The task brief's "reply to 0x53 requests"
premise was inverted** — the sender never sends the receiver a timing request.

**Sync (`0xD4`)** on the control socket is logged-and-dropped (free-run).

The peer IP is learned from the first audio datagram's source (shairport does the
same); resend/timing requests are addressed to that IP + the sender's control/timing
ports from the SETUP `Transport` header. Wire formats match shairport-sync `rtp.c`
byte-for-byte (see the Phase 4 plan and `research-phase3-5.md`).

**Free-run drift (spec §6f).** Lives in `components/audio` (`audio_drift` +
`audio_playback`): once per drain cycle, if the PCM ring's fill has crossed a
generous high/low watermark (3/4 and 1/4 of capacity), drop or duplicate **one
frame** (~23 µs, inaudible) so sender/DAC ppm mismatch never slowly underruns a
multi-hour session. `avail == 0` is a true underrun (the silence path owns it), not
drift.

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
- `rtp_reorder.{h,c}` — seq-indexed reorder / gap-detect / conceal (16-bit wrap) → `test/test_rtp_reorder`
- `rtp_resend.{h,c}` — 8-byte `0xD5` resend-request builder + `0xD6` response unwrap → `test/test_rtp_resend`
- `rtp_timing.{h,c}` — `0xD2`/`0xD3` timing codec + NTP64 pack/unpack → `test/test_rtp_timing`
- (`alac_config.{h,c}` in `components/alac` — `a=fmtp` → ALAC config → `test/test_alac_config`)
- (`audio_drift.{h,c}` in `components/audio` — free-run drift drop/dup decision → `test/test_audio_drift`)

Target-build-only glue (sockets / PSA / MAC / FreeRTOS / decoder):

- `raop.c` + `include/raop.h` — the accept/recv task + method dispatch (`raop_server_start`).
- `rtsp_session.{h,c}` — per-session state + the three UDP sockets.
- `raop_crypto.{h,c}` — PSA Crypto (mbedTLS 4.0) key import, challenge sign, AES-key decrypt.
- `raop_key.c` — the embedded RAOP RSA private key.
- `raop_rtp.{h,c}` — Phase 4 RTP receive task: 3-socket select loop, seq-ordered decode, retransmit, receiver-initiated timing, diag-tone handoff.

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

## SET_PARAMETER: volume, metadata, progress (Phase 5, spec §5b)

`SET_PARAMETER` dispatches on `Content-Type` (matched shairport `rtsp.c`), always
acking `200`:

- **`text/parameters`** → `raop_parse_volume()` reads a `volume: <float>\r\n` line
  (dB `-30..0`, `-144` = mute) and calls `audio_set_volume()`; `raop_parse_progress()`
  reads `progress: <start>/<cur>/<end>` (three RTP timestamps @44100 Hz) and logs
  elapsed/total once. Both parsers are **pure** and host-tested (`test/test_raop_volume`).
- **`application/x-dmap-tagged`** → `dmap_parse()` walks the DMAP/DAAP TLVs
  (`[4-char code][BE32 len][value]`, recursing into the `mlit` container) extracting
  `minm`=title, `asar`=artist, `asal`=album as bounded UTF-8. `raop_metadata_update()`
  stores them and logs `now playing: …` **only when a field changes** (senders resend
  redundantly). Pure walker host-tested incl. truncated/overrunning/oversize bodies
  (`test/test_dmap`); the dispatch decision is host-tested (`test/test_setparam`).
- **anything else** (artwork `image/*`, unknown, absent) → ignored.

All lengths are bounded against the untrusted body (spec §9): the parsers never read
past `body+len` (the RTSP body pointer is not NUL-terminated), string copies are
capped at `DMAP_STR_MAX-1` + NUL, and a claimed length overrunning the body stops the
walk. `raop_metadata_clear()` runs on TEARDOWN.

## Session events → status LED

`raop_set_event_cb()` registers a callback fired from the RTSP task on RECORD
(`RAOP_EV_STREAMING`) and on TEARDOWN / idle-reclaim (`RAOP_EV_IDLE`). `main` wires it
to `system_led_set_state` so the LED reflects streaming vs idle without `raop`
depending on the `system` component.

## Public API

```c
void raop_server_start(void);            // called from main on Wi-Fi GOT_IP, after mDNS advertise
void raop_server_stop(void);             // used on Wi-Fi LOST_IP (later phase)
void raop_set_event_cb(raop_event_cb_t); // LED streaming/idle transitions (register before start)
```
