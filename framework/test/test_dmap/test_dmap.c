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

// Wrap `payload` in `levels` nested mlit containers (each length encompasses the
// child), placing the result in `dst`. Returns the total encoded length.
static size_t build_nested(uint8_t *dst, int levels,
                           const uint8_t *payload, size_t plen){
    size_t total = (size_t)levels * 8 + plen;
    memcpy(dst + (size_t)levels * 8, payload, plen);
    for (int i = levels - 1; i >= 0; i--){
        size_t h = (size_t)i * 8;
        uint32_t child = (uint32_t)(total - (h + 8));      // value = all bytes after hdr
        dst[h+0]='m'; dst[h+1]='l'; dst[h+2]='i'; dst[h+3]='t';
        dst[h+4]=(child>>24)&0xff; dst[h+5]=(child>>16)&0xff;
        dst[h+6]=(child>>8)&0xff;  dst[h+7]=child&0xff;
    }
    return total;
}
void test_nested_within_cap(void){         // 4 nested mlit: field still reached
    const uint8_t minm[] = {'m','i','n','m',0,0,0,2,'H','i'};
    uint8_t b[64]; size_t n = build_nested(b, 4, minm, sizeof(minm));
    dmap_meta_t m; int c = dmap_parse(b, n, &m);
    TEST_ASSERT_EQUAL_INT(1, c);
    TEST_ASSERT_EQUAL_STRING("Hi", m.title);
}
void test_nested_beyond_cap(void){         // 5 nested mlit: descent stops, field unseen
    const uint8_t minm[] = {'m','i','n','m',0,0,0,2,'H','i'};
    uint8_t b[64]; size_t n = build_nested(b, 5, minm, sizeof(minm));
    dmap_meta_t m; int c = dmap_parse(b, n, &m);
    TEST_ASSERT_EQUAL_INT(0, c);            // capped: no crash, field not extracted
    TEST_ASSERT_FALSE(m.has_title);
}
void test_deep_nesting_no_overflow(void){  // crafted ~250-deep chain must not recurse away
    const uint8_t minm[] = {'m','i','n','m',0,0,0,2,'H','i'};
    static uint8_t b[2100];                 // 250*8+10 = 2010, under RAOP_RX_CAP-ish
    size_t n = build_nested(b, 250, minm, sizeof(minm));
    dmap_meta_t m; int c = dmap_parse(b, n, &m);
    TEST_ASSERT_EQUAL_INT(0, c);            // depth cap => bounded recursion, no overflow
    TEST_ASSERT_FALSE(m.has_title);
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_flat_minm); RUN_TEST(test_flat_all_three); RUN_TEST(test_mlit_wrapped);
    RUN_TEST(test_unknown_tags_skipped); RUN_TEST(test_truncated_header);
    RUN_TEST(test_length_overruns_body); RUN_TEST(test_empty_value_clears);
    RUN_TEST(test_long_value_bounded); RUN_TEST(test_empty_body);
    RUN_TEST(test_nested_within_cap); RUN_TEST(test_nested_beyond_cap);
    RUN_TEST(test_deep_nesting_no_overflow);
    return UNITY_END(); }
