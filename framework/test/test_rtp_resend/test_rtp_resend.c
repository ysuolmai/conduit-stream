// Host unit tests for the pure RAOP resend-request builder + 0xd6 response unwrap.
#include <unity.h>
#include <string.h>
#include "rtp_resend.c"
void setUp(void){} void tearDown(void){}

void test_build(void){
    uint8_t r[RESEND_REQ_LEN];
    rtp_resend_build(r, 0x1234, 3);
    TEST_ASSERT_EQUAL_UINT8(0x80, r[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD5, r[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, r[2]); TEST_ASSERT_EQUAL_UINT8(0x01, r[3]); // htons(1)
    TEST_ASSERT_EQUAL_UINT8(0x12, r[4]); TEST_ASSERT_EQUAL_UINT8(0x34, r[5]); // first BE
    TEST_ASSERT_EQUAL_UINT8(0x00, r[6]); TEST_ASSERT_EQUAL_UINT8(0x03, r[7]); // count BE
}
void test_unwrap_ok(void){
    // 0xd6 wrapper + a 16-byte inner audio packet (12 hdr + 4 payload)
    uint8_t p[4+16] = {0x80,0xD6,0x00,0x00,
        0x80,0x60,0xAB,0xCD, 0,0,0,0, 0,0,0,0, 0xDE,0xAD,0xBE,0xEF};
    const uint8_t *in; size_t il;
    TEST_ASSERT_EQUAL_INT(0, rtp_resend_unwrap(p, sizeof p, &in, &il));
    TEST_ASSERT_EQUAL_PTR(p+4, in);
    TEST_ASSERT_EQUAL_size_t(16, il);
    TEST_ASSERT_EQUAL_UINT8(0x60, in[1]);   // inner is a normal audio packet
}
void test_unwrap_rejects_wrong_type(void){
    uint8_t p[20] = {0x80,0xD4}; const uint8_t *in; size_t il;   // 0xd4 sync, not resend
    TEST_ASSERT_EQUAL_INT(-1, rtp_resend_unwrap(p, sizeof p, &in, &il));
}
void test_unwrap_rejects_short(void){
    uint8_t p[15] = {0x80,0xD6}; const uint8_t *in; size_t il;   // < 4 + 12
    TEST_ASSERT_EQUAL_INT(-1, rtp_resend_unwrap(p, sizeof p, &in, &il));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_build); RUN_TEST(test_unwrap_ok);
    RUN_TEST(test_unwrap_rejects_wrong_type); RUN_TEST(test_unwrap_rejects_short);
    return UNITY_END(); }
