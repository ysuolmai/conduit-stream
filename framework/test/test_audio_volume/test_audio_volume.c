// Host unit tests for the pure volume math (dB->Q16.16 gain + per-sample apply).
#include <unity.h>
#include "audio_volume.c"
void setUp(void){} void tearDown(void){}

void test_db_to_q16_vectors(void){
    TEST_ASSERT_EQUAL_INT32(0,     audio_volume_db_to_q16(-144.0f)); // mute
    TEST_ASSERT_EQUAL_INT32(2072,  audio_volume_db_to_q16(-30.0f));
    TEST_ASSERT_EQUAL_INT32(6554,  audio_volume_db_to_q16(-20.0f));
    // 12345 not the research note's 12346: our powf rounds the -14.5 dB boundary
    // (0.188365 * 65536 ~= 12345.5) down by one on this toolchain. ±1 at a
    // rounding boundary is expected (plan note) and inaudible.
    TEST_ASSERT_EQUAL_INT32(12345, audio_volume_db_to_q16(-14.5f));
    TEST_ASSERT_EQUAL_INT32(32846, audio_volume_db_to_q16(-6.0f));
    TEST_ASSERT_EQUAL_INT32(65536, audio_volume_db_to_q16(0.0f));    // unity
}
void test_db_clamp_out_of_range(void){
    TEST_ASSERT_EQUAL_INT32(65536, audio_volume_db_to_q16(6.0f));    // >0 clamps to unity
    TEST_ASSERT_EQUAL_INT32(2072,  audio_volume_db_to_q16(-50.0f));  // <-30 clamps to -30
    TEST_ASSERT_EQUAL_INT32(0,     audio_volume_db_to_q16(-200.0f)); // below sentinel = mute
}
void test_apply_unity_is_identity(void){
    int16_t b[4] = {20000, -20000, 32767, -32768};
    audio_volume_apply(b, 4, AUDIO_VOL_UNITY);
    TEST_ASSERT_EQUAL_INT16(20000, b[0]);
    TEST_ASSERT_EQUAL_INT16(-20000, b[1]);
    TEST_ASSERT_EQUAL_INT16(32767, b[2]);
    TEST_ASSERT_EQUAL_INT16(-32768, b[3]);
}
void test_apply_gain_vectors(void){
    int16_t b0 = 20000; audio_volume_apply(&b0, 1, audio_volume_db_to_q16(0.0f));
    TEST_ASSERT_EQUAL_INT16(20000, b0);
    int16_t b6 = 20000; audio_volume_apply(&b6, 1, audio_volume_db_to_q16(-6.0f));
    TEST_ASSERT_EQUAL_INT16(10024, b6);
    int16_t b30 = 20000; audio_volume_apply(&b30, 1, audio_volume_db_to_q16(-30.0f));
    TEST_ASSERT_EQUAL_INT16(632, b30);
    int16_t bm = 20000; audio_volume_apply(&bm, 1, 0 /*mute*/);
    TEST_ASSERT_EQUAL_INT16(0, bm);
}
void test_apply_clamp_and_symmetry(void){
    // full-scale at unity stays in range (never overflows)
    int16_t hi = 32767, lo = -32768;
    audio_volume_apply(&hi, 1, AUDIO_VOL_UNITY); TEST_ASSERT_EQUAL_INT16(32767, hi);
    audio_volume_apply(&lo, 1, AUDIO_VOL_UNITY); TEST_ASSERT_EQUAL_INT16(-32768, lo);
    // negative sample attenuates symmetrically to the positive one
    int16_t np = 20000, nn = -20000; int32_t q = audio_volume_db_to_q16(-6.0f);
    audio_volume_apply(&np, 1, q); audio_volume_apply(&nn, 1, q);
    TEST_ASSERT_EQUAL_INT16(-np, nn);   // 10024 vs -10024
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_db_to_q16_vectors); RUN_TEST(test_db_clamp_out_of_range);
    RUN_TEST(test_apply_unity_is_identity); RUN_TEST(test_apply_gain_vectors);
    RUN_TEST(test_apply_clamp_and_symmetry);
    return UNITY_END(); }
