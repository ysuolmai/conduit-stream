# audio

Playback Manager, Audio Decoder, PCM buffer, and the I2S driver wrapper live
here. The Playback Manager stays transport-agnostic: AirPlay / REST / Bluetooth
feed PCM into it, it does not know or care which one.

For v0.0.1 the I2S + sine-tone code sits in `src/main.cpp`. First refactor:
lift the I2S driver and PCM buffer into this component behind a small
`audio_play_pcm()` style interface.
