# Conduit Stream - firmware

Turn any powered speaker with an AUX input into a modern Wi-Fi audio endpoint,
built on an ESP32-S3 + PCM5102A DAC instead of buying a WiiM. Open source, no
cloud, no account, local API, auto-discoverable. Long term this is also the
embedded platform the "Baby Arlo" robot reuses (Wi-Fi provisioning, OTA, audio
in/out, discovery, firmware architecture).

## Status: v0.0.1 - "hear first audio"

This build boots the S3, logs chip / flash / PSRAM, brings up I2S, and streams a
clean 440 Hz sine through the PCM5102A into the Aura Studio 3. No networking yet.

Covers spec milestones 1-6: PlatformIO project, flashing + serial, flash/PSRAM
detection, I2S config, 440 Hz sine, first audio.

## Wiring first

Do **not** skip [`docs/WIRING.md`](docs/WIRING.md). Your original spec had
`SCK not connected`, which leaves the DAC silent. It must be `SCK -> GND`, and
`XSMT -> 3.3V`. Two-minute read, saves a two-hour headache.

## Build / flash / listen

Prereqs: [PlatformIO Core](https://platformio.org/install/cli) (`pip install platformio`)
or the PlatformIO VS Code extension.

```bash
cd firmware
pio run                 # first build pulls the toolchain + ESP-IDF (few min)
pio run -t upload       # flash the S3 over USB-C
pio device monitor      # 115200 baud
```

Expected serial output:

```
conduit: Conduit Stream  |  firmware v0.0.1
conduit: chip: esp32s3, 2 core(s), silicon rev vX.Y
conduit: flash: 16.0 MB
conduit: psram: 8.0 MB (octal)
conduit: i2s: std TX up @ 44100 Hz, 16-bit stereo (BCLK=6, LRCK=7, DOUT=5, MCLK=unused)
conduit: tone: emitting 440 Hz sine (both channels)
```

...and a 440 Hz tone (an "A") out of the Aura. If the log looks like that but the
Aura is silent, it is hardware/config, not firmware. Run the silence checklist in
`docs/WIRING.md`.

> Native-USB S3 devkits: if the monitor is blank, the console may be on USB
> Serial/JTAG. See the commented flags in `platformio.ini` and the note in
> `sdkconfig.defaults`.

## Layout

```
firmware/
  platformio.ini        # ESP32-S3 N16R8, ESP-IDF framework
  sdkconfig.defaults    # 16MB QIO flash + 8MB octal PSRAM
  src/main.cpp          # v0.0.1 boot diagnostics + I2S sine tone
  components/
    audio/              # Playback Manager, PCM buffer, I2S driver (transport-agnostic)
    network/            # Wi-Fi + transport plugins (AirPlay, REST, BT, DLNA)
    mdns/               # discovery
    ota/                # over-the-air updates
    system/             # boot, config/NVS, logging, LED status
  docs/                 # WIRING.md and future protocol notes
  hardware/             # BOM + wiring
```

The `components/*` folders are real but empty ESP-IDF components for now, so the
architecture is in place and the project still builds. The design rule from the
spec holds: the Playback Manager stays transport-agnostic and protocols are
plugins that feed it PCM.

## Roadmap (from the Vision doc)

| Version | Goal |
|---------|------|
| 0.0.1   | 440 Hz test tone through the DAC (**this build**) |
| 0.1     | receive PCM audio over Wi-Fi and play it |
| 0.2     | AirPlay receiver |
| 1.0     | standalone endpoint: AirPlay, REST API, OTA, mDNS, web UI, optional BT |

## Software stack

ESP-IDF (framework) + PlatformIO (tooling) + C++. The code is standard ESP-IDF,
so `idf.py build` works too if you ever want to drop PlatformIO.
