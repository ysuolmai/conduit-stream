#include <unity.h>
#include <string.h>
#include "device_id.c"  // compile the pure unit directly into the test TU

void setUp(void)    {}
void tearDown(void) {}

void test_device_id_format_uppercase_hex(void) {
    const uint8_t mac[6] = {0xE8, 0x3D, 0xC1, 0xF2, 0xAC, 0x6C};
    char out[13];
    device_id_format(mac, out);
    TEST_ASSERT_EQUAL_STRING("E83DC1F2AC6C", out);
}

void test_device_id_format_zero_pads(void) {
    const uint8_t mac[6] = {0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C};
    char out[13];
    device_id_format(mac, out);
    TEST_ASSERT_EQUAL_STRING("000A000B000C", out);
}

void test_instance_name_joins_with_at(void) {
    char out[32];
    device_instance_name_format("E83DC1F2AC6C", "Conduit", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("E83DC1F2AC6C@Conduit", out);
}

void test_instance_name_truncates_safely(void) {
    char out[8];  // too small; must stay NUL-terminated, never overflow
    device_instance_name_format("E83DC1F2AC6C", "Conduit", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(7, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('\0', out[7]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_device_id_format_uppercase_hex);
    RUN_TEST(test_device_id_format_zero_pads);
    RUN_TEST(test_instance_name_joins_with_at);
    RUN_TEST(test_instance_name_truncates_safely);
    return UNITY_END();
}
