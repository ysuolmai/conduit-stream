# Conduit Stream - firmware

AirPlay 1 (RAOP) receiver for the ESP32-S3 Super Mini N4R2. This fork supports
PCM5102A stereo line-out and MAX98357A mono speaker amplification.

## Status: N4R2 AirPlay 1 receiver

The firmware discovers as `MiniSpeaker-XXX` over mDNS, accepts AirPlay 1 audio, decodes
ALAC and sends PCM over I2S. The N4R2 build uses 4 MB flash and 2 MB quad PSRAM.

Covers spec milestones 1-6: PlatformIO project, flashing + serial, flash/PSRAM
detection, I2S config, 440 Hz sine, first audio.

## Wiring first

Do **not** skip [`docs/WIRING.md`](docs/WIRING.md). Both targets use GPIO11 = DIN,
GPIO12 = BCLK and GPIO13 = LRCK/LRC. PCM5102A also needs `SCK -> GND` and
`XSMT -> 3.3V`; MAX98357A needs `SD/EN` high and a differential speaker load.

## Build / flash / listen

Prereqs: [PlatformIO Core](https://platformio.org/install/cli) (`pip install platformio`)
or the PlatformIO VS Code extension.

```bash
cd framework
pio run -e esp32-s3-n4r2-pcm5102a
pio run -e esp32-s3-n4r2-max98357a
pio run -e esp32-s3-n4r2-pcm5102a -t upload
pio device monitor      # 115200 baud
```

Expected serial output:

```
conduit: Conduit Stream  |  AirPlay 1 / N4R2
conduit: chip: esp32s3, 2 core(s), silicon rev vX.Y
conduit: flash: 4.0 MB
conduit: psram: 2.0 MB (quad)
conduit: i2s: std TX up @ 44100 Hz, 16-bit stereo (BCLK=12, LRCK=13, DOUT=11, MCLK=unused)
```

With no saved SSID, connect a phone to the open `MiniSpeaker-XXX` hotspot and
select a 2.4 GHz network in the captive page. Browse to `http://192.168.4.1/` if
the page does not open automatically. After saving, the device restarts,
advertises `_raop._tcp`, and appears under the same `MiniSpeaker-XXX` name in
AirPlay. On later boots it connects directly; SoftAP and HTTP remain off.

Release BOOT after power-up, then hold it for 3 seconds to reboot into the setup
portal for one boot. This does not erase the saved SSID/password. Saving new
credentials replaces them; rebooting without saving returns to the old network.
The same page can install the matching `*-ota.bin` release file while preserving
Wi-Fi settings.

If the saved network does not provide an IP address within 15 seconds after
boot, firmware automatically reboots into that same one-shot setup portal. It
does not erase the old credentials or retry indefinitely.

## Onboard LEDs

There are three LEDs on this board. The WS2812 RGB LED and red LED share GPIO48;
firmware turns both off and holds that pin low five seconds after connecting.
The blue battery-charge LED has no GPIO and cannot be controlled in software.
It can remain on while charging or blink when USB is connected without a battery.

> Native-USB S3 devkits: if the monitor is blank, the console may be on USB
> Serial/JTAG. See the commented flags in `platformio.ini` and the note in
> `sdkconfig.defaults`.

## Layout

```
firmware/
  platformio.ini        # N4R2 PCM5102A and MAX98357A environments
  sdkconfig.defaults.*  # per-output flash/PSRAM/I2S settings
  src/main.cpp          # Wi-Fi, mDNS, RAOP and audio startup
  components/
    audio/              # Playback Manager, PCM buffer, I2S driver (transport-agnostic)
    network/            # Wi-Fi + transport plugins (AirPlay, REST, BT, DLNA)
    mdns/               # discovery
    ota/                # over-the-air updates
    system/             # boot, config/NVS, logging, LED status
  docs/                 # WIRING.md and future protocol notes
  hardware/             # BOM + wiring
```

The Playback Manager stays transport-agnostic; RAOP feeds decoded PCM into the
same PSRAM-backed audio path for both output boards.

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
