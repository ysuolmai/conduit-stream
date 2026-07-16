// Host unit tests for the pure RAOP AES-CBC frame split (block vs plaintext tail).
#include <unity.h>
#include "aes_frame.c"

void setUp(void) {}
void tearDown(void) {}

static void chk(size_t n, size_t cipher, size_t plain) {
    aes_frame_split_t s;
    aes_frame_split(n, &s);
    TEST_ASSERT_EQUAL_size_t(cipher, s.cipher_len);
    TEST_ASSERT_EQUAL_size_t(cipher, s.plain_off);
    TEST_ASSERT_EQUAL_size_t(plain,  s.plain_len);
    // Invariant: cipher + plain reconstructs the whole payload.
    TEST_ASSERT_EQUAL_size_t(n, s.cipher_len + s.plain_len);
}

void test_boundaries(void) {
    chk(0, 0, 0);
    chk(15, 0, 15);
    chk(16, 16, 0);
    chk(17, 16, 1);
    chk(31, 16, 15);
    chk(32, 32, 0);
}

void test_real_frame(void) {
    // A ~1416-byte ALAC ciphertext -> 1408 decrypted + 8 plaintext tail.
    chk(1416, 1408, 8);
}

void test_aligned_no_tail(void) {
    chk(1408, 1408, 0);
    chk(352, 352, 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_boundaries);
    RUN_TEST(test_real_frame);
    RUN_TEST(test_aligned_no_tail);
    return UNITY_END();
}
