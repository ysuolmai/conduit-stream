// Host unit tests for the pure "volume:"/"progress:" text/parameters line parsers.
#include <unity.h>
#include <string.h>
#include "raop_volume.c"
void setUp(void){} void tearDown(void){}

void test_volume_basic(void){
    const char *b = "volume: -14.500000\r\n"; float db = 99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -14.5f, db);
}
void test_volume_mute_sentinel(void){
    const char *b = "volume: -144.000000\r\n"; float db = 0;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -144.0f, db);
}
void test_volume_full(void){
    const char *b = "volume: 0.000000\r\n"; float db = 99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, db);
}
void test_volume_no_crlf(void){                       // tolerate missing terminator
    const char *b = "volume: -6.0"; float db = 99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -6.0f, db);
}
void test_volume_absent(void){
    const char *b = "progress: 1/2/3\r\n"; float db = 99;
    TEST_ASSERT_FALSE(raop_parse_volume(b, strlen(b), &db));
}
void test_volume_not_bare_prefix(void){               // "volumes:" must not match
    const char *b = "volumes: 5\r\n"; float db = 99;
    TEST_ASSERT_FALSE(raop_parse_volume(b, strlen(b), &db));
}
void test_volume_second_line(void){                   // find it past another line
    const char *b = "something: 1\r\nvolume: -3.0\r\n"; float db = 99;
    TEST_ASSERT_TRUE(raop_parse_volume(b, strlen(b), &db));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -3.0f, db);
}
void test_progress_parse(void){
    const char *b = "progress: 3839844000/3839863488/3856224000\r\n";
    uint32_t s = 0, c = 0, e = 0;
    TEST_ASSERT_TRUE(raop_parse_progress(b, strlen(b), &s, &c, &e));
    TEST_ASSERT_EQUAL_UINT32(3839844000u, s);
    TEST_ASSERT_EQUAL_UINT32(3839863488u, c);
    TEST_ASSERT_EQUAL_UINT32(3856224000u, e);
}
void test_progress_absent(void){
    const char *b = "volume: 0.0\r\n"; uint32_t s, c, e;
    TEST_ASSERT_FALSE(raop_parse_progress(b, strlen(b), &s, &c, &e));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_volume_basic); RUN_TEST(test_volume_mute_sentinel);
    RUN_TEST(test_volume_full); RUN_TEST(test_volume_no_crlf);
    RUN_TEST(test_volume_absent); RUN_TEST(test_volume_not_bare_prefix);
    RUN_TEST(test_volume_second_line);
    RUN_TEST(test_progress_parse); RUN_TEST(test_progress_absent);
    return UNITY_END(); }
