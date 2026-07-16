// Host unit tests for the pure fmtp -> ALAC-config mapper. Compiles the unit
// directly (no ESP-IDF/alac.h deps). Canonical line comes straight from the
// RAOP SDP: "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100".
#include <unity.h>
#include "alac_config.c"

void setUp(void) {}
void tearDown(void) {}

void test_canonical_with_pt(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(0, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100", &c));
    TEST_ASSERT_EQUAL_UINT32(352,   c.frame_length);
    TEST_ASSERT_EQUAL_UINT8(0,      c.compat_version);
    TEST_ASSERT_EQUAL_UINT8(16,     c.bit_depth);
    TEST_ASSERT_EQUAL_UINT8(40,     c.pb);
    TEST_ASSERT_EQUAL_UINT8(10,     c.mb);
    TEST_ASSERT_EQUAL_UINT8(14,     c.kb);
    TEST_ASSERT_EQUAL_UINT8(2,      c.num_channels);
    TEST_ASSERT_EQUAL_UINT16(255,   c.max_run);
    TEST_ASSERT_EQUAL_UINT32(0,     c.max_frame_bytes);
    TEST_ASSERT_EQUAL_UINT32(0,     c.avg_bitrate);
    TEST_ASSERT_EQUAL_UINT32(44100, c.sample_rate);
}

void test_accepts_afmtp_prefix(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(0, alac_cfg_from_fmtp("a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100", &c));
    TEST_ASSERT_EQUAL_UINT32(352, c.frame_length);
    TEST_ASSERT_EQUAL_UINT8(2,    c.num_channels);
    TEST_ASSERT_EQUAL_UINT32(44100, c.sample_rate);
}

void test_tolerates_trailing_whitespace(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(0, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100\r\n", &c));
    TEST_ASSERT_EQUAL_UINT32(352, c.frame_length);
}

void test_rejects_short(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0", &c));
}

void test_rejects_trailing_junk(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("96 352 0 16 40 10 14 2 255 0 0 44100 extra", &c));
}

void test_rejects_garbage(void) {
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("", &c));
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("hello world", &c));
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp(NULL, &c));
}

void test_rejects_out_of_range_channels(void) {
    // num_channels lives in a uint8_t field; 300 overflows it -> reject.
    alac_cfg_t c;
    TEST_ASSERT_EQUAL_INT(-1, alac_cfg_from_fmtp("96 352 0 16 40 10 14 300 255 0 0 44100", &c));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canonical_with_pt);
    RUN_TEST(test_accepts_afmtp_prefix);
    RUN_TEST(test_tolerates_trailing_whitespace);
    RUN_TEST(test_rejects_short);
    RUN_TEST(test_rejects_trailing_junk);
    RUN_TEST(test_rejects_garbage);
    RUN_TEST(test_rejects_out_of_range_channels);
    return UNITY_END();
}
