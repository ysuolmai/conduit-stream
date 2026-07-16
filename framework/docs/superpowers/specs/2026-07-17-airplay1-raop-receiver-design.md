# Conduit Stream — AirPlay 1 (RAOP) Receiver — Design

**Milestone:** v0.2 — "stream from an Apple device and hear it"
**Date:** 2026-07-17
**Hardware:** ESP32-S3 N16R8 + PCM5102A DAC → Harman Kardon Aura Studio 3 (AUX). No new components.
**Depends on:** v0.0.1 audio path (I2S → PCM5102A), confirmed working.

## 1. Overview

Add an AirPlay 1 (RAOP — Remote Audio Output Protocol) receiver so an iPhone or Mac
can discover the device on the LAN, connect, and stream audio that plays through the
existing PCM5102A → Aura path. AirPlay 1, not 2: AirPlay 2's HomeKit pairing + PTP
sync crypto is impractical on this class of MCU, while RAOP is well-understood and
in-reach with ESP-IDF primitives.

This is pure firmware. The ESP32-S3 already has 2.4 GHz Wi-Fi; flash (16 MB, dual
3 MB OTA slots) and PSRAM (8 MB) comfortably hold the ALAC decoder, mbedTLS, and the
jitter buffer.

## 2. Goals / Non-Goals

### Goals (this spec)
- Discoverable in the iOS/macOS AirPlay menu (mDNS `_raop._tcp` + correct TXT records).
- Connect and negotiate: RSA `Apple-Challenge`, AES session-key exchange.
- Receive, decrypt, and decode ALAC audio → PCM → DAC, playing cleanly.
- **Volume**: honor `SET_PARAMETER volume`, applied as software gain on PCM.
- **Metadata**: decode DAAP text metadata (title / artist / album); log it (display later).
- Clean audio over real Wi-Fi: jitter buffer + RTP retransmit.
- Graceful `STOP` / `TEARDOWN` / `FLUSH`, reconnect after Wi-Fi drop.
- RGB status LED (connecting / connected-idle / streaming).

### Non-Goals (explicitly deferred)
- **Artwork** — large JPEG buffers, no display to show them. Add at v-next.
- **Multi-room / true clock sync** — free-run the DAC clock; single speaker only.
- **AirPlay 2** — out of scope, likely infeasible on this MCU.
- **Wi-Fi provisioning UX** (SoftAP/BLE) — creds come from Kconfig→NVS; provisioning
  is its own later milestone.
- **Multiple simultaneous senders** — AirPlay is single-session.

## 3. Key Decisions (resolved in brainstorming)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Sequencing | Leapfrog straight to AirPlay 1 (one spec) | Built in demoable phases internally |
| Feature bar | Play + volume + text metadata | Artwork/sync deferred (YAGNI) |
| Timing/reliability | PSRAM jitter buffer + RTP retransmit, **free-run clock** | Best quality-for-effort on a single speaker |
| Build strategy | **Hybrid** — our protocol glue, ported ALAC, mbedTLS crypto | Fits plugin design; no reinvented codec |
| Wi-Fi creds | Hardcoded via Kconfig, seeded into NVS | Keeps spec focused on RAOP |
| Status LED | Include (RGB on GPIO48) | Cheap, fills the `system` LED slot |

Format alignment: I2S is already `44100 Hz / 16-bit / stereo` — exactly RAOP's ALAC
output. Decoded PCM feeds the existing DAC path with **no resampling**.

## 4. Architecture & Data Flow

RAOP decomposes across the existing components; `audio` stays transport-agnostic.

```
Apple sender
   │  mDNS _raop._tcp  ──────────────►  mdns/          (advertise service + TXT)
   │  RTSP/TCP (control) ────────────►  network/raop   (session state machine)
   │  RTP/UDP audio ─────────────────►  network/raop   (RTP receiver + retransmit)
   ▼
[AES-128-CBC decrypt]  ── mbedTLS ───►  network/raop
   ▼
[ALAC decode → PCM]    ── ported ────►  audio/decoder  (Apple ALAC reference)
   ▼
[jitter buffer in PSRAM] ────────────►  audio/buffer   (ring, reorder by RTP seq)
   ▼
[volume scale]          ─────────────►  audio/playback (Playback Manager, software gain)
   ▼
i2s_write_frames  ───────────────────►  audio/i2s      (lifted from main.cpp)
   ▼
PCM5102A → Aura
```

**Component ownership**

- **`audio`** — transport-agnostic core: Playback Manager, PCM jitter buffer, ALAC
  decoder, I2S wrapper. Knows nothing about AirPlay.
- **`network/raop`** — the AirPlay *plugin*: RTSP server, RTP receiver, retransmit,
  AES decrypt, session lifecycle. The only unit that knows RAOP exists. Every path
  ends in "push decoded PCM + volume/metadata to the Playback Manager."
- **`network/wifi`** — station bring-up from NVS creds.
- **`mdns`** — thin wrapper over IDF's managed `mdns`; advertises `_raop._tcp`.
- **`system`** — NVS config, boot sequencing, logging, health, RGB status LED,
  the (public) RAOP RSA private key constant.
- **`src/main.cpp`** — shrinks to orchestration only.

**Architectural boundary:** `network/raop` depends on `audio`'s public interface;
`audio` never depends on `network`. That is the spec's "Playback Manager stays
transport-agnostic" rule made literal — a future REST or Bluetooth plugin feeds the
same interface.

## 5. RAOP Protocol Layer (`network/raop`)

### 5a. mDNS advertisement (`mdns`)
Advertise `_raop._tcp` on the RTSP port. Service name `<deviceid>@<name>`
(e.g. `E83DC1F2AC6C@Conduit`). TXT records advertise capability:
`tp=UDP`, `sr=44100`, `ss=16`, `ch=2`, `cn=1` (ALAC), `et=0,1` (encryption: none + RSA/AES),
`sv`, `da=true`, `vn=3`, `md=0,1,2` (metadata flags; text honored, image ignored).
Correct TXT is what makes the speaker appear in the AirPlay menu.

### 5b. RTSP server (session state machine)
TCP listener handling the request sequence:
- `OPTIONS` → respond to `Apple-Challenge` by RSA-signing it with the RAOP private
  key (proves we're a real receiver; without this, iOS refuses to connect).
- `ANNOUNCE` → parse SDP: RSA-encrypted AES key (`rsaaeskey`), IV (`aesiv`), ALAC
  codec params (`fmtp`). Decrypt the AES key with the embedded RAOP private key.
- `SETUP` → allocate the three UDP ports (audio / control / timing); return in `Transport`.
- `RECORD` → session live; prime the jitter buffer.
- `SET_PARAMETER` → `volume` (dB, `-144`=mute … `0`=full) and DAAP `text/parameters`
  metadata → push to Playback Manager.
- `FLUSH` → drop buffered audio. `TEARDOWN` → release ports, silence, LED to idle.

### 5c. RTP receiver + retransmit
UDP socket for audio RTP. Each packet: 12-byte RTP header (seq, timestamp) +
AES-CBC payload. Track sequence numbers; on a gap, request a resend on the **control**
channel and slot the recovered packet back in by sequence before decrypt. The
**timing** channel is answered minimally (keep the sender happy) but **not** used for
clock discipline — free-run.

### 5d. Crypto (mbedTLS, stock on ESP-IDF)
RSA-decrypt the session key once per session; AES-128-CBC decrypt each audio packet.
The RAOP RSA private key is the well-known one shipped by every open receiver; it is
a `system` constant (not a secret).

**Threading:** RTSP on one FreeRTOS task (low rate, blocking TCP fine); RTP
receive + decrypt + decode on a higher-priority task feeding the jitter buffer;
Playback Manager I2S write on its own task. Jitter buffer lives in PSRAM.

## 6. Audio Pipeline (`audio`)

### 6a. I2S wrapper (`audio/i2s`)
Lift the working v0.0.1 code from `main.cpp` verbatim: `i2s_new_channel` +
`I2S_STD_PHILIPS_SLOT` @ 44100/16/stereo, MCLK unused, BCLK=6 / LRCK=7 / DOUT=5.
Wrap behind `i2s_write_frames(buf, n)`. No behavior change — proven code, relocated.
The v0.0.1 sine-tone task becomes a build-time **diagnostic mode**, still triggerable
so the known-good audio path never rots.

### 6b. Playback Manager (`audio/playback`)
Public interface every transport plugin uses:
- `audio_play_pcm(int16 *frames, size_t n, uint32_t rtp_ts)`
- `audio_set_volume(float db)`
- `audio_set_metadata(title, artist, album)`
Owns the playback task: pull ordered PCM from the jitter buffer → apply volume gain →
`i2s_write_frames`. Knows nothing about RAOP.

### 6c. Jitter buffer (`audio/buffer`)
PSRAM ring indexed by **RTP sequence** (so late/retransmitted packets slot into the
correct position before playback). Target depth ~2 s (≈353 KB at 44100×2×2 B/s;
trivial in 8 MB PSRAM), tunable. Decouples the bursty network/decode task from the
steady I2S drain.

### 6d. ALAC decoder (`audio/decoder`)
Ported Apple open-source reference decoder. Configured once from the `fmtp` "magic
cookie" in `ANNOUNCE`; per-frame decodes ALAC → interleaved 16-bit stereo PCM. Pure
function of (frame, config) — unit-testable with captured frames. This is the only
coupling point with `network/raop`: RAOP parses the cookie from SDP and hands the
config to the decoder at session start; the decoder never touches the network.

### 6e. Volume
RAOP dB (`-144`=mute … `0`=full) → linear gain, multiply each int16 sample, clamp.
Software (PCM5102A has no gain pin). Bypassed at full scale to avoid needless math.

### 6f. Underrun / drift handling
- **Underrun** (buffer empty) → I2S writes **silence**, not stale samples; auto-resumes.
- **Free-run drift** — sender/DAC clocks differ by a few ppm; over a long session the
  buffer slowly fills/drains. At high/low watermarks, drop or duplicate a single frame
  (~23 µs, inaudible). Negligible per track; prevents multi-hour drift underrun.

## 7. System, Wi-Fi & Boot (`system`, `network/wifi`)

- **NVS config store** — namespace `conduit`: `wifi_ssid`, `wifi_pass`, `name`
  (default `"Conduit"`). Source of truth after first boot; seeded from Kconfig
  (`CONFIG_CONDUIT_WIFI_SSID/PASS`) on first boot so config changes later need no reflash.
- **Device identity** — `deviceid` from the MAC at boot (e.g. `E83DC1F2AC6C`); not stored.
- **RAOP RSA key** — `system` constant (public knowledge).
- **Wi-Fi** — station, **2.4 GHz only** (S3 has no 5 GHz; AP must expose 2.4 GHz).
  **`WIFI_PS_NONE`** — disable modem power-save (its latency spikes cause RTP
  jitter/dropouts; the single most common ESP32-AirPlay-stutter fix). Reconnect with
  backoff; a drop tears down any live RAOP session.
- **RGB status LED** — addressable LED on GPIO48 via IDF `led_strip`:
  connecting / connected-idle / streaming / needs-creds.

**Boot orchestration (`main.cpp`):**
```
1. nvs_flash_init
2. system_init   → load config, logging, banner, LED
3. audio_init    → I2S up, jitter buffer, playback task (silence, idle)
4. wifi_connect  → station, WIFI_PS_NONE
5. on GOT_IP:    → mdns_advertise(_raop._tcp, port), raop_start()  [listening]
6. on LOST_IP:   → raop_stop(), mdns pause; wifi reconnect loop
```

## 8. Error Handling & Edge Cases

- **Single session** — second sender mid-stream gets RTSP `453` busy.
- **Sender vanishes w/o TEARDOWN** — detect dead RTSP TCP → teardown, silence, LED idle.
- **FLUSH** — drop buffered audio, silence over the gap (no click).
- **Unrecoverable packet loss** — conceal (hold last / silence for that slot), continue.
- **Decrypt/decode error** — drop that packet, rate-limited log, never kill the stream.
- **RTP seq/timestamp wrap** — reorder logic handles 16-bit seq + 32-bit ts wrap.
- **Wi-Fi drop mid-stream** — teardown, LED "reconnecting," resume on reconnect.
- **No creds** — clear one-line error, distinct LED, **no boot-loop**.
- **PSRAM alloc fail at init** — fail loud, don't limp.

## 9. Security

- RTSP/RTP parsers consume untrusted LAN input: **bound every header/body/packet
  length, validate before use, rate-limit logs.**
- **Accepted trade-off:** AirPlay 1 has no pairing — anyone on the LAN can stream to
  the device, and the RAOP RSA key is public. Inherent to RAOP; acceptable for a home
  speaker, stated so it's a deliberate choice.

## 10. Testing

- **Unit (`pio test`, no hardware)** — ALAC decoder (captured frames → known PCM),
  jitter buffer (reorder / seq-wrap / underrun), volume (dB→linear→clamp), RTSP request
  parse (fixtures), RTP header parse, AES decrypt (known vectors).
- **On-target integration** — real iPhone + Mac: appears in AirPlay menu → connects
  (challenge/crypto) → clean audio → volume slider moves level → metadata logs →
  stop/teardown → reconnect after Wi-Fi drop.
- **Loss/jitter** — stream at range to exercise retransmit + underrun paths.
- **Regression net** — v0.0.1 sine-tone diagnostic mode retained as the
  audio-path-vs-RAOP-path divider.

## 11. Build Phasing (demoable increments)

| Phase | Deliverable | Proof |
|-------|-------------|-------|
| 0 | Audio refactor: I2S → `audio` behind `audio_play_pcm()` + jitter buffer + playback task; sine feeds the new interface | Sine still plays → refactor didn't break v0.0.1 |
| 1 | Wi-Fi station (NVS creds, `WIFI_PS_NONE`) + mDNS `_raop._tcp` advertise | Speaker appears in AirPlay menu |
| 2 | RTSP state machine + RSA challenge + AES key exchange | Sender connects and negotiates; no audio yet |
| 3 | RTP receive + AES decrypt + ALAC decode → PCM → playback | First AirPlay audio out the Aura |
| 4 | Retransmit + underrun/drift safeguards | Clean audio under real Wi-Fi loss |
| 5 | Volume + DAAP metadata + RGB LED states | Volume slider works, track logs, LED reflects state |

Phase 3 is "it works"; 4–5 are "it's good."

## 12. Open Questions / Future

- Provisioning UX (SoftAP or BLE) — separate milestone.
- Artwork + a display — v-next.
- REST control API and Bluetooth transport reuse the same Playback Manager interface.
