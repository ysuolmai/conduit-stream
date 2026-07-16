// Host unit tests for the pure RAOP timing 0xd2/0xd3 codec + NTP64 pack/unpack.
#include <unity.h>
#include <string.h>
#include "rtp_timing.c"
void setUp(void){} void tearDown(void){}

void test_build_request(void){
    uint8_t p[TIMING_PKT_LEN];
    rtp_timing_build_request(p, 0x0102030405060708ULL);
    TEST_ASSERT_EQUAL_UINT8(0x80, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD2, p[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, p[2]); TEST_ASSERT_EQUAL_UINT8(0x07, p[3]); // htons(7)
    for (int i=8;i<24;i++) TEST_ASSERT_EQUAL_UINT8(0, p[i]);                  // origin+recv 0
    TEST_ASSERT_EQUAL_UINT8(0x01, p[24]); TEST_ASSERT_EQUAL_UINT8(0x08, p[31]); // transmit BE
}
void test_parse_response(void){
    uint8_t p[TIMING_PKT_LEN] = {0};
    p[0]=0x80; p[1]=0xD3;
    // receive t2 = 0x00000002_00000000 at [16..23]; transmit t3 = 0x0000000300000000 at [24..31]
    p[19]=0x02; p[27]=0x03;
    uint8_t type; timing_stamps_t st;
    TEST_ASSERT_EQUAL_INT(0, rtp_timing_parse(p, sizeof p, &type, &st));
    TEST_ASSERT_EQUAL_UINT8(TIMING_RESP_TYPE, type);
    TEST_ASSERT_EQUAL_UINT64(0x0000000200000000ULL, st.receive);
    TEST_ASSERT_EQUAL_UINT64(0x0000000300000000ULL, st.transmit);
}
void test_roundtrip_response(void){
    uint8_t p[TIMING_PKT_LEN];
    rtp_timing_build_response(p, 0x1111111122222222ULL, 0x3333333344444444ULL,
                                 0x5555555566666666ULL);
    uint8_t type; timing_stamps_t st;
    TEST_ASSERT_EQUAL_INT(0, rtp_timing_parse(p, sizeof p, &type, &st));
    TEST_ASSERT_EQUAL_UINT8(TIMING_RESP_TYPE, type);
    TEST_ASSERT_EQUAL_UINT64(0x1111111122222222ULL, st.origin);
    TEST_ASSERT_EQUAL_UINT64(0x3333333344444444ULL, st.receive);
    TEST_ASSERT_EQUAL_UINT64(0x5555555566666666ULL, st.transmit);
}
void test_parse_request_type(void){
    uint8_t p[TIMING_PKT_LEN]={0}; p[0]=0x80; p[1]=0xD2;
    uint8_t type; timing_stamps_t st;
    TEST_ASSERT_EQUAL_INT(0, rtp_timing_parse(p,sizeof p,&type,&st));
    TEST_ASSERT_EQUAL_UINT8(TIMING_REQ_TYPE, type);
}
void test_parse_rejects(void){
    uint8_t p[TIMING_PKT_LEN]={0}; p[0]=0x80; p[1]=0xD4;    // sync, not timing
    uint8_t type; timing_stamps_t st;
    TEST_ASSERT_EQUAL_INT(-1, rtp_timing_parse(p,sizeof p,&type,&st));
    TEST_ASSERT_EQUAL_INT(-1, rtp_timing_parse(p,31,&type,&st)); // short
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_build_request); RUN_TEST(test_parse_response);
    RUN_TEST(test_roundtrip_response); RUN_TEST(test_parse_request_type);
    RUN_TEST(test_parse_rejects);
    return UNITY_END(); }
