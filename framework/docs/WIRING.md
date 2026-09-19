# Conduit Stream - Wiring (ESP32-S3 Super Mini N4R2)

Both supported targets use the same I2S pins. Flash the matching target for the
audio board you connect; do not connect both boards to the same I2S bus while testing.

| ESP32-S3 N4R2 | Audio board | Signal |
|---------------|-------------|--------|
| GPIO11        | DIN         | I2S data out |
| GPIO12        | BCLK/BCK    | bit clock |
| GPIO13        | LRC/LRCK/WS | word-select clock |
| GND           | GND         | common ground |

## PCM5102A: line out to a powered speaker

ESP32-S3 N4R2 -> PCM5102A DAC -> 3.5mm AUX -> powered speaker

The Aura is a *powered* speaker with its own amplifier, and the PCM5102A puts out
*line level*. So DAC jack -> AUX in is exactly right. No separate amp needed.

## I2S / power connections

| ESP32-S3 N4R2 | PCM5102A | Notes |
|----------|----------|-------|
| GPIO11   | DIN      | I2S data out |
| GPIO12   | BCK      | bit clock |
| GPIO13   | LRCK     | word clock (a.k.a. LCK / WS) |
| 5V       | VIN      | breakout has an onboard 3.3V regulator, so 5V is fine; 3.3V also works |
| GND      | GND      | common ground (required) |

GPIO11/12/13 are safe general-purpose pins on the S3. They are not strapping pins
(those are GPIO0/3/45/46), not the USB pins (GPIO19/20), and not the flash/PSRAM
pins. If you re-map, avoid those.

## The config pins your spec was missing (this is what makes it silent)

The PCM5102A runs standalone, but only if you set its mode pins. On the common
purple GY-PCM5102 breakout these are the pads/jumpers on the back:

| Pin  | Set to | Why |
|------|--------|-----|
| SCK  | **GND** | **Required.** No MCLK is wired, so the DAC must run its internal PLL. Grounding SCK selects that mode. Left floating = silence. This is the single most common "no sound" cause. |
| XSMT | **3.3V** | Soft mute. High = unmute. Most boards pull this high by default, but verify. Low = muted = silence. |
| FLT  | GND | Filter = normal latency (default). |
| DEMP | GND | De-emphasis off (default). |
| FMT  | GND | I2S (Philips) format. Matches the firmware's `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG`. High = left-justified = wrong for this config. |

We deliberately do **not** wire MCLK. `gpio_cfg.mclk = I2S_GPIO_UNUSED` in the
firmware, and SCK->GND on the DAC is the matching half of that decision.

## MAX98357A: mono amplifier to a passive speaker

The N4R2 MAX98357A target uses the I2S mono amplifier instead of
the PCM5102A line-out DAC:

| ESP32-S3 | MAX98357A | Notes |
|----------|-----------|-------|
| GPIO11   | DIN       | I2S data |
| GPIO12   | BCLK      | bit clock |
| GPIO13   | LRC/WS    | word select |
| 5V or 3.3V | VIN/VCC | follow the breakout marking |
| GND      | GND       | common ground |

Tie `SD/EN` high if the module has no pull-up. Connect the speaker between
`SPK+` and `SPK-`; neither output is ground-referenced. The firmware mixes
AirPlay stereo to mono before I2S so a mono amplifier does not lose one side.

Flash targets:

- `esp32-s3-n4r2-pcm5102a` for PCM5102A.
- `esp32-s3-n4r2-max98357a` for MAX98357A.

## "Project Gravity" warning

Digital I2S is more forgiving than analog, but BCK / LRCK / DIN still need solid,
continuous contact. Header pins held down by gravity will give you intermittent
clock, which shows up as dropouts, buzzing, or dead silence that comes and goes
when you nudge the breadboard. For a *first* "does it work" test it can be OK, but
if you get flaky results, solder the headers before you debug anything else. Half
the "my DAC is broken" threads online are cold joints and loose jumpers.

## If the Aura is silent, walk this list in order

1. **SCK grounded?** (the big one)
2. **XSMT high / unmuted?**
3. **FMT set to I2S (low)?** Left-justified will often give garbled or no clean tone.
4. **Loose I2S jumpers?** Reseat / solder. Wiggle test.
5. **Aura on the AUX source, volume up?** Obvious, still catches people.
6. **BCK and LRCK swapped?** Easy to cross. Re-check against the table.
7. **Serial log:** if you see `i2s: std TX up ...` and `tone: emitting 440 Hz`,
   the firmware is fine and the problem is downstream (DAC config pins or cabling).
   That log line is your firmware-vs-hardware divider.
