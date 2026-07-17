// Host unit tests for the pure DMAP/DAAP TLV walker (incl. truncated/malformed).
#include <unity.h>
#include <string.h>
#include "dmap.c"
void setUp(void){} void tearDown(void){}

void test_flat_minm(void){
    const uint8_t b[] = {'m','i','n','m',0,0,0,2,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_TRUE(m.has_title);
    TEST_ASSERT_EQUAL_STRING("Hi", m.title);
}
void test_flat_all_three(void){
    const uint8_t b[] = {
        'm','i','n','m',0,0,0,2,'H','i',
        'a','s','a','r',0,0,0,2,'A','B',
        'a','s','a','l',0,0,0,3,'X','Y','Z'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("Hi",  m.title);
    TEST_ASSERT_EQUAL_STRING("AB",  m.artist);
    TEST_ASSERT_EQUAL_STRING("XYZ", m.album);
}
void test_mlit_wrapped(void){
    // mlit container (len=10) holding one minm "Hi"
    const uint8_t b[] = {'m','l','i','t',0,0,0,10,
                         'm','i','n','m',0,0,0,2,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Hi", m.title);
}
void test_unknown_tags_skipped(void){
    const uint8_t b[] = {'a','s','g','n',0,0,0,3,'p','o','p',   // genre: ignored
                         'm','i','n','m',0,0,0,1,'Q'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("Q", m.title);
}
void test_truncated_header(void){          // only 6 bytes: can't read code+len
    const uint8_t b[] = {'m','i','n','m',0,0};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_FALSE(m.has_title);
}
void test_length_overruns_body(void){      // claims len=200 but body short
    const uint8_t b[] = {'m','i','n','m',0,0,0,200,'H','i'};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(0, n);           // overrun guard fires, no read past buf
    TEST_ASSERT_FALSE(m.has_title);
}
void test_empty_value_clears(void){        // vlen==0 = field cleared
    const uint8_t b[] = {'m','i','n','m',0,0,0,0};
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_TRUE(m.has_title);
    TEST_ASSERT_EQUAL_STRING("", m.title);
}
void test_long_value_bounded(void){        // > DMAP_STR_MAX truncates, no overflow
    uint8_t b[8 + 300];
    const uint8_t hdr[8] = {'m','i','n','m',0,0,1,44};   // BE32 0x0000012C = 300
    memcpy(b, hdr, 8);
    for (int i = 0; i < 300; i++) b[8 + i] = 'A';
    dmap_meta_t m; int n = dmap_parse(b, sizeof(b), &m);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(DMAP_STR_MAX - 1, (int)strlen(m.title));  // bounded + NUL
}
void test_empty_body(void){
    dmap_meta_t m; TEST_ASSERT_EQUAL_INT(0, dmap_parse((const uint8_t*)"", 0, &m));
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_flat_minm); RUN_TEST(test_flat_all_three); RUN_TEST(test_mlit_wrapped);
    RUN_TEST(test_unknown_tags_skipped); RUN_TEST(test_truncated_header);
    RUN_TEST(test_length_overruns_body); RUN_TEST(test_empty_value_clears);
    RUN_TEST(test_long_value_bounded); RUN_TEST(test_empty_body);
    return UNITY_END(); }
