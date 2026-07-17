#include "audio_volume.h"
#include <math.h>

int32_t audio_volume_db_to_q16(float db) {
    float gain;
    if (db <= -144.0f) {
        gain = 0.0f;                       // AirPlay mute sentinel (a flag, not a dB)
    } else {
        if (db >  0.0f)  db =  0.0f;       // clamp out-of-range senders to [-30, 0]
        if (db < -30.0f) db = -30.0f;
        gain = powf(10.0f, db / 20.0f);    // == powf(10, 0.05*db)
    }
    return (int32_t)(gain * 65536.0f + 0.5f);   // Q16.16; 1.0 == 65536
}

void audio_volume_apply(int16_t *buf, size_t n_samples, int32_t fix_q16) {
    for (size_t i = 0; i < n_samples; i++) {
        // 64-bit product: 32768 * 65536 == 2^31 would overflow signed int32.
        // Gain is clamped to <= unity so amplification/overflow is structurally
        // impossible; the clamp below only guards rounding at the extreme.
        int64_t prod = (int64_t)buf[i] * fix_q16;
        // Round-half-AWAY-from-zero, done on the magnitude: a signed `>> 16` is a
        // floor (toward -inf), which would bias every negative sample down by 1
        // (e.g. an exact -20000 at unity would become -20001). Rounding the
        // absolute value then re-applying the sign keeps +v and -v symmetric.
        int64_t mag = prod >= 0 ? prod : -prod;
        int32_t v   = (int32_t)((mag + 0x8000) >> 16);
        if (prod < 0) v = -v;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}
