#include <unity.h>
#include <string.h>
#include "raop_txt.c"  // compile the pure unit directly into the test TU

static raop_txt_item_t items[RAOP_TXT_COUNT];

void setUp(void)    { memset(items, 0, sizeof(items)); }
void tearDown(void) {}

// Look a key up in the built array; NULL if absent.
static const char *find(const char *key) {
    for (size_t i = 0; i < RAOP_TXT_COUNT; i++)
        if (items[i].key && strcmp(items[i].key, key) == 0) return items[i].value;
    return NULL;
}

void test_builds_full_record_count(void) {
    TEST_ASSERT_EQUAL_UINT(RAOP_TXT_COUNT, raop_txt_build(items, RAOP_TXT_COUNT));
}

void test_exact_capability_values(void) {
    raop_txt_build(items, RAOP_TXT_COUNT);
    TEST_ASSERT_EQUAL_STRING("UDP",   find("tp"));
    TEST_ASSERT_EQUAL_STRING("44100", find("sr"));
    TEST_ASSERT_EQUAL_STRING("16",    find("ss"));
    TEST_ASSERT_EQUAL_STRING("2",     find("ch"));
    TEST_ASSERT_EQUAL_STRING("1",     find("cn"));   // ALAC
    TEST_ASSERT_EQUAL_STRING("0,1",   find("et"));   // none + RSA/AES
    TEST_ASSERT_EQUAL_STRING("false", find("sv"));
    TEST_ASSERT_EQUAL_STRING("true",  find("da"));
    TEST_ASSERT_EQUAL_STRING("3",     find("vn"));
    TEST_ASSERT_EQUAL_STRING("0,1,2", find("md"));   // text metadata honored
}

void test_respects_max_items_bound(void) {
    // Never writes past the caller's array.
    TEST_ASSERT_EQUAL_UINT(3, raop_txt_build(items, 3));
    TEST_ASSERT_NOT_NULL(items[2].key);
    TEST_ASSERT_NULL(items[3].key);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_builds_full_record_count);
    RUN_TEST(test_exact_capability_values);
    RUN_TEST(test_respects_max_items_bound);
    return UNITY_END();
}
