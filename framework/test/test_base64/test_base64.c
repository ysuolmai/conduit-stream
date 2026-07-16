#include <unity.h>
#include <string.h>
#include "base64.c"  // compile the pure unit directly into the test TU

void setUp(void) {}
void tearDown(void) {}

// RFC 4648 §10 vectors.
void test_encode_rfc4648_vectors(void) {
    char out[16];
    TEST_ASSERT_EQUAL_INT(0, base64_encode((const uint8_t *)"", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    base64_encode((const uint8_t *)"f", 1, out, sizeof(out));      TEST_ASSERT_EQUAL_STRING("Zg==", out);
    base64_encode((const uint8_t *)"fo", 2, out, sizeof(out));     TEST_ASSERT_EQUAL_STRING("Zm8=", out);
    base64_encode((const uint8_t *)"foo", 3, out, sizeof(out));    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
    base64_encode((const uint8_t *)"foob", 4, out, sizeof(out));   TEST_ASSERT_EQUAL_STRING("Zm9vYg==", out);
    base64_encode((const uint8_t *)"fooba", 5, out, sizeof(out));  TEST_ASSERT_EQUAL_STRING("Zm9vYmE=", out);
    base64_encode((const uint8_t *)"foobar", 6, out, sizeof(out)); TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", out);
}

void test_decode_roundtrip_and_padding(void) {
    uint8_t out[16];
    TEST_ASSERT_EQUAL_INT(6, base64_decode("Zm9vYmFy", 8, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
    TEST_ASSERT_EQUAL_INT(1, base64_decode("Zg==", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("f", out, 1);
    TEST_ASSERT_EQUAL_INT(2, base64_decode("Zm8=", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("fo", out, 2);
}

void test_decode_skips_whitespace(void) {  // RTSP/SDP wrap base64 across lines
    uint8_t out[16];
    TEST_ASSERT_EQUAL_INT(6, base64_decode("Zm9v\r\nYmFy", 10, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("foobar", out, 6);
}

void test_decode_rejects_bad_char_and_overflow(void) {
    uint8_t out[2];
    TEST_ASSERT_EQUAL_INT(-1, base64_decode("****", 4, out, sizeof(out)));       // invalid char
    TEST_ASSERT_EQUAL_INT(-1, base64_decode("Zm9vYmFy", 8, out, sizeof(out)));   // 6 bytes > out_cap 2
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_encode_rfc4648_vectors);
    RUN_TEST(test_decode_roundtrip_and_padding);
    RUN_TEST(test_decode_skips_whitespace);
    RUN_TEST(test_decode_rejects_bad_char_and_overflow);
    return UNITY_END();
}
