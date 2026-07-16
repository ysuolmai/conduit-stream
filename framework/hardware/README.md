# hardware

Bill of materials, wiring, and (later) board/enclosure notes.

## v0.0.1 BOM (all already on hand)

| Part | Detail |
|------|--------|
| MCU board | ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM, USB-C, native USB) |
| DAC | PCM5102A I2S breakout (GY-PCM5102), onboard 3.5mm jack |
| Speaker | Harman Kardon Aura Studio 3 (powered, 3.5mm AUX in) |
| Cables | Dupont wires, 3.5mm AUX cable, USB-C cable |

Wiring and the config-pin settings live in [`../docs/WIRING.md`](../docs/WIRING.md).

## Known limitation

PCM5102A headers are not soldered yet ("Project Gravity"). Fine for a first
smoke test; solder before trusting any result or chasing a bug. See the wiring
doc's Project Gravity note.
