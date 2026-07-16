// Host unit tests for the pure free-run drift watermark decision.
#include <unity.h>
#include "audio_drift.c"
void setUp(void){} void tearDown(void){}
static const audio_drift_cfg_t CFG = { .high = 80000, .low = 8000 };

void test_above_high_drops(void){
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DROP, audio_drift_decide(80000, &CFG));
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DROP, audio_drift_decide(90000, &CFG));
}
void test_below_low_dups(void){
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DUP, audio_drift_decide(8000, &CFG));
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DUP, audio_drift_decide(1, &CFG));
}
void test_empty_is_none(void){   // underrun path, not drift
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(0, &CFG));
}
void test_midband_none(void){
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(40000, &CFG));
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(8001, &CFG));   // just above low
    TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(79999, &CFG));  // just below high
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_above_high_drops); RUN_TEST(test_below_low_dups);
    RUN_TEST(test_empty_is_none); RUN_TEST(test_midband_none);
    return UNITY_END(); }
