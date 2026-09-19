# Conduit Stream

**Turn a powered speaker or passive speaker into a Wi-Fi AirPlay endpoint — with an ESP32-S3 Super Mini N4R2 and either a PCM5102A or MAX98357A.** No WiiM, no cloud, no account. Open, local, self-contained.

> A personal experiment: a from-scratch **AirPlay 1 (RAOP) receiver** on an ESP32-S3, designed, built, and debugged live against a real iPhone and Mac. It's not a product — it's a "can a microcontroller pretend to be an AirPort Express?" experiment. It can.

## Why I built this

The Harman Kardon Aura Studio 3 sounds great — and has no Wi-Fi. AUX and Bluetooth only. I just wanted to AirPlay to it.

The market's answer is a Wi-Fi audio streamer (WiiM and friends). The sleekest one I found was ~$100 — for a box whose whole job is to put audio on a 3.5 mm jack.

My first hack was an old Android phone wired into the AUX. It worked, but keeping it powered, awake, and reliable was its own running battle. Wrong tool.

So I built the thing that should exist: a ~$6 microcontroller that *is* the Wi-Fi endpoint. No phone, no $100 box, no cloud, no account — just the speaker, an ESP32, and a DAC.

## Status

**Works.** An iPhone or Mac discovers the device as *MiniSpeaker-XXX*, connects, and streams lossless ALAC audio through the selected audio board.

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
                                    ┌─ PCM5102A ──line level──▶ powered speaker (AUX)
                                    └─ MAX98357A ──speaker──▶ passive speaker
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
| MCU | ESP32-S3 Super Mini N4R2 (4 MB flash, 2 MB quad PSRAM, native USB-C) |
| Audio option A | PCM5102A I2S breakout (line out to a powered speaker) |
| Audio option B | MAX98357A I2S mono amplifier (directly to a passive speaker) |

Wiring for both supported combinations is in [`framework/docs/WIRING.md`](framework/docs/WIRING.md). The two firmware targets use GPIO11 = DIN, GPIO12 = BCLK, GPIO13 = LRCK/LRC.

## Build & flash

Needs [PlatformIO Core](https://platformio.org/install/cli) and the ESP-IDF toolchain (PlatformIO pulls it on first build).

```bash
cd framework
pio run -e esp32-s3-n4r2-pcm5102a     # PCM5102A build
pio run -e esp32-s3-n4r2-max98357a    # MAX98357A build
pio run -e esp32-s3-n4r2-pcm5102a -t upload
pio test -e native                     # run the host unit tests
```

If no Wi-Fi SSID has been saved, the device opens the **MiniSpeaker-XXX** setup
network automatically. The captive page lists nearby 2.4 GHz networks; choose
one, enter its password, and save. The device restarts and the same
**MiniSpeaker-XXX** name appears in the AirPlay menu. If the captive page does
not open automatically, browse to `http://192.168.4.1/`.

Once configured, boot goes directly to the saved network: SoftAP, DNS and HTTP
stay off during normal AirPlay operation. To reconfigure Wi-Fi or update the
firmware, release BOOT after power-up, then hold BOOT for 3 seconds. The device
restarts into the setup network for that boot only. Existing Wi-Fi credentials
remain intact unless **Save and restart** is pressed, so leaving the portal or
power-cycling without saving returns to the old network.

The setup page also accepts a firmware upload. Use the `*-ota.bin` matching the
connected audio board. A successful update preserves the NVS partition and its
saved Wi-Fi credentials.

## Firmware downloads

Each version tag triggers one GitHub Actions run that builds both supported
variants and publishes one GitHub Release containing:

- `minispeaker-esp32s3-n4r2-pcm5102a.bin`
- `minispeaker-esp32s3-n4r2-pcm5102a-ota.bin`
- `minispeaker-esp32s3-n4r2-max98357a.bin`
- `minispeaker-esp32s3-n4r2-max98357a-ota.bin`
- `SHA256SUMS`

The two files without `-ota` are merged images for USB flashing at address
`0x0`. The `-ota.bin` files contain only the application and are exclusively for
the setup page; do not interchange PCM5102A and MAX98357A variants.

## Onboard LEDs

The ESP32-S3 Super Mini has three LEDs. The WS2812 RGB LED and the discrete red
LED share GPIO48, so WS2812 status data can make the red LED flicker briefly.
Five seconds after Wi-Fi connects, firmware clears the RGB LED, releases its RMT
driver, and holds GPIO48 low so both controllable LEDs remain off.

The blue LED is the battery-charger status indicator and has no ESP32 GPIO. It
is on while charging, off with a connected battery when not charging, and may
blink when no battery is present; firmware cannot turn it off.

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
| next | Artwork and a small control UI |

## Credits

Stands on the shoulders of [shairport-sync](https://github.com/mikebrady/shairport-sync) (Mike Brady) and David Hammerton's ALAC decoder. The RAOP protocol details are theirs; the ESP-IDF implementation here is a clean-room reimplementation guided by them.

## License

Personal experiment — no warranty. The embedded RAOP RSA key is public knowledge from the open-source AirPlay ecosystem.
