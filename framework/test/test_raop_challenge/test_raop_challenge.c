#include <unity.h>
#include <string.h>
#include "raop_challenge.c"

void setUp(void) {}
void tearDown(void) {}

void test_layout_challenge_ip_mac_padding(void) {
    uint8_t chal[16]; for (int i = 0; i < 16; i++) chal[i] = (uint8_t)(0x10 + i);
    uint8_t ip4[4]   = {10, 0, 0, 9};
    uint8_t mac[6]   = {0xE8, 0x3D, 0xC1, 0xF2, 0xAC, 0x6C};
    uint8_t out[RAOP_CHALLENGE_BUF_LEN];
    TEST_ASSERT_EQUAL_INT(26, raop_challenge_assemble(chal, 16, ip4, mac, out));
    TEST_ASSERT_EQUAL_MEMORY(chal, out, 16);          // challenge
    TEST_ASSERT_EQUAL_MEMORY(ip4, out + 16, 4);        // IPv4
    TEST_ASSERT_EQUAL_MEMORY(mac, out + 20, 6);        // MAC
    for (int i = 26; i < 32; i++) TEST_ASSERT_EQUAL_UINT8(0, out[i]);  // padding
}

void test_rejects_wrong_challenge_length(void) {
    uint8_t chal[8] = {0};
    uint8_t ip4[4]  = {0}, mac[6] = {0}, out[RAOP_CHALLENGE_BUF_LEN];
    TEST_ASSERT_EQUAL_INT(-1, raop_challenge_assemble(chal, 8, ip4, mac, out));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_layout_challenge_ip_mac_padding);
    RUN_TEST(test_rejects_wrong_challenge_length);
    return UNITY_END();
}
