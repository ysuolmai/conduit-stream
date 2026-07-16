#include <unity.h>
#include <string.h>
#include "sdp.c"

void setUp(void) {}
void tearDown(void) {}

static const char kSdp[] =
    "v=0\r\n"
    "o=iTunes 3641928563 0 IN IP4 10.0.0.5\r\n"
    "s=iTunes\r\n"
    "c=IN IP4 10.0.0.9\r\n"
    "t=0 0\r\n"
    "m=audio 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 AppleLossless\r\n"
    "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"
    "a=rsaaeskey:VjVbxWcmYgbBbhwBNlCh3K0CMNtWoB844BuiHGUJT51xg\r\n"
    "a=aesiv:5b7b4d43a53a34dccd2ed845b8e00e7f\r\n";

void test_extracts_all_three_fields(void) {
    sdp_media_t m;
    TEST_ASSERT_TRUE(sdp_parse(kSdp, strlen(kSdp), &m));
    TEST_ASSERT_TRUE(m.has_fmtp);
    TEST_ASSERT_TRUE(m.has_rsaaeskey);
    TEST_ASSERT_TRUE(m.has_aesiv);
    TEST_ASSERT_EQUAL_STRING("96 352 0 16 40 10 14 2 255 0 0 44100", m.fmtp);
    TEST_ASSERT_EQUAL_STRING("VjVbxWcmYgbBbhwBNlCh3K0CMNtWoB844BuiHGUJT51xg", m.rsaaeskey);
    TEST_ASSERT_EQUAL_STRING("5b7b4d43a53a34dccd2ed845b8e00e7f", m.aesiv);
}

void test_missing_fields_flagged_absent(void) {
    static const char sdp[] = "v=0\r\na=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n";
    sdp_media_t m;
    TEST_ASSERT_TRUE(sdp_parse(sdp, strlen(sdp), &m));
    TEST_ASSERT_TRUE(m.has_fmtp);
    TEST_ASSERT_FALSE(m.has_rsaaeskey);
    TEST_ASSERT_FALSE(m.has_aesiv);
}

void test_bare_lf_line_endings(void) {  // some senders omit the CR
    static const char sdp[] = "v=0\na=aesiv:abcd\n";
    sdp_media_t m;
    TEST_ASSERT_TRUE(sdp_parse(sdp, strlen(sdp), &m));
    TEST_ASSERT_TRUE(m.has_aesiv);
    TEST_ASSERT_EQUAL_STRING("abcd", m.aesiv);
}

void test_empty_body_returns_false(void) {
    sdp_media_t m;
    TEST_ASSERT_FALSE(sdp_parse("", 0, &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_extracts_all_three_fields);
    RUN_TEST(test_missing_fields_flagged_absent);
    RUN_TEST(test_bare_lf_line_endings);
    RUN_TEST(test_empty_body_returns_false);
    return UNITY_END();
}
