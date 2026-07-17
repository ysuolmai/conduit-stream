# Conduit Stream

**Turn any powered speaker with an AUX jack into a Wi-Fi AirPlay endpoint — with a ~$6 ESP32-S3 and a PCM5102A DAC.** No WiiM, no cloud, no account. Open, local, self-contained.

> A personal experiment: a from-scratch **AirPlay 1 (RAOP) receiver** on an ESP32-S3, designed, built, and debugged live against a real iPhone and Mac. It's not a product — it's a "can a microcontroller pretend to be an AirPort Express?" experiment. It can.

## Status

**Works.** An iPhone or Mac discovers *Conduit* on the network, connects, and streams lossless ALAC audio through the DAC into a powered speaker (a Harman Kardon Aura Studio 3, in my case).

- Discovery (mDNS `_raop._tcp`), the RSA `Apple-Challenge` handshake, AES session-key exchange, RTP audio receive, AES-128-CBC decrypt, ALAC decode → PCM → I2S → DAC.
- Volume (the iOS slider), DAAP track metadata, an RGB status LED.
- RTP reorder + retransmit + a receiver-initiated timing channel; free-run clock with a drift safeguard.
- **111 host unit tests** for the pure logic (parsers, codecs, buffer math); every build stage verified.

Rough edges (it's an experiment): a debug UDP-log mirror and a couple of diagnostic hooks are still in-tree; artwork is intentionally ignored; multi-room sync is out of scope.

## How it works

```
iPhone / Mac ──mDNS _raop._tcp──▶  discovery/   (advertise service + TXT)
             ──RTSP/TCP──────────▶  raop/        (session state machine + RSA/AES via mbedTLS)
             ──RTP/UDP audio─────▶  raop/        (receive · reorder · retransmit · timing)
                                     │  AES-128-CBC decrypt (PSA Crypto)
                                     ▼
                                    alac/         (ALAC frame → 16-bit stereo PCM)
                                     ▼
                                    audio/        (transport-agnostic Playback Manager:
                                     ▼             PSRAM jitter buffer → software volume → I2S)
                                    PCM5102A ──line level──▶ powered speaker (AUX)
```

The **Playback Manager stays transport-agnostic**: protocols are plugins that push PCM into a single `audio_play_pcm()` interface. AirPlay is one such plugin; a REST or Bluetooth transport could be another.

Components (ESP-IDF):
- `audio/` — Playback Manager, SPSC PCM ring buffer (host-tested), I2S wrapper, software volume, drift safeguard.
- `alac/` — vendored David Hammerton ALAC decoder (MIT) + an fmtp→config mapper.
- `raop/` — the AirPlay-1 plugin: RTSP server, RSA/AES crypto, RTP receive/reorder/retransmit/timing.
- `network/` — Wi-Fi station bring-up.
- `discovery/` — mDNS `_raop._tcp` advertisement (wraps Espressif's managed `mdns`).
- `system/` — NVS config, device identity, RGB status LED.

## Hardware

| Part | Detail |
|------|--------|
| MCU | ESP32-S3 (N16R8 — 16 MB flash, 8 MB octal PSRAM, native USB-C) |
| DAC | PCM5102A I2S breakout (GY-PCM5102), 3.5 mm line out |
| Speaker | Any powered speaker with a 3.5 mm AUX in |

Wiring (and the config pins that make the DAC actually produce sound — `SCK→GND`, `XSMT→3.3V`, `FMT→GND`) is in [`framework/docs/WIRING.md`](framework/docs/WIRING.md). **Read it** — a floating `SCK` is silent, and cold solder joints are half of all "dead DAC" reports.

## Build & flash

Needs [PlatformIO Core](https://platformio.org/install/cli) and the ESP-IDF toolchain (PlatformIO pulls it on first build).

```bash
cd framework
# Set your Wi-Fi (lands in the gitignored generated sdkconfig, never committed):
pio run -t menuconfig      #  Conduit Stream → Wi-Fi SSID / Password
pio run -e esp32-s3-n16r8              # build
pio run -e esp32-s3-n16r8 -t upload    # flash over USB-C
pio test -e native                     # run the host unit tests
```

Then pick **Conduit** from your device's AirPlay menu and hit play.

## The interesting part

AirPlay 1 authenticates the *receiver* by making it RSA-sign Apple's challenge with the AirPort Express private key — extracted from the hardware in 2004 and shipped in every open receiver (shairport) since. Apple never rotated it, because AirPlay 1 is frozen. This firmware embeds that same public-knowledge key; that's the whole trust model.

Four bugs only a real device could surface (build-green + 111 host tests all passed):
1. The RSA self-test overflowed a 2.3 KB system-event-task stack → reboot loop.
2. The timing channel fired 3 s too late → iOS dropped the session at 2 s.
3. **The fmtp codec-params parser was silently broken on the 32-bit target** — `(long)UINT32_MAX` overflows to −1, so every valid value was rejected. 64-bit host tests never saw it. This was the "connects, volume works, but won't play" bug.
4. iOS's album-artwork request overflowed the RTSP buffer → mid-stream teardown.

## Roadmap

| Version | Goal |
|---------|------|
| 0.0.1 | 440 Hz test tone through the DAC ✅ |
| 0.1–0.2 | **AirPlay 1 receiver ✅ (this)** |
| next | Wi-Fi provisioning UX, OTA updates, artwork, a small web UI |

## Credits

Stands on the shoulders of [shairport-sync](https://github.com/mikebrady/shairport-sync) (Mike Brady) and David Hammerton's ALAC decoder. The RAOP protocol details are theirs; the ESP-IDF implementation here is a clean-room reimplementation guided by them.

## License

Personal experiment — no warranty. The embedded RAOP RSA key is public knowledge from the open-source AirPlay ecosystem.
