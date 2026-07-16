// Host unit tests for the pure RTP header parser. Compiles the unit directly.
#include <unity.h>
#include <string.h>
#include "rtp_parser.c"

void setUp(void) {}
void tearDown(void) {}

// 0x80 0x60  seq=0x0102  ts=0x0A0B0C0D  ssrc=0x11223344  + 4 payload bytes.
static const uint8_t P[] = {
    0x80, 0x60, 0x01, 0x02, 0x0A, 0x0B, 0x0C, 0x0D,
    0x11, 0x22, 0x33, 0x44, 0xDE, 0xAD, 0xBE, 0xEF
};

void test_parse_audio(void) {
    rtp_header_t h;
    TEST_ASSERT_EQUAL_INT(0, rtp_parse(P, sizeof P, &h));
    TEST_ASSERT_EQUAL_UINT8(RTP_PT_AUDIO, h.payload_type);
    TEST_ASSERT_FALSE(h.marker);
    TEST_ASSERT_EQUAL_UINT16(0x0102, h.seq);
    TEST_ASSERT_EQUAL_UINT32(0x0A0B0C0D, h.timestamp);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, h.ssrc);
    TEST_ASSERT_EQUAL_size_t(4, h.payload_len);
    TEST_ASSERT_EQUAL_UINT8(0xDE, h.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, h.payload[3]);
}

void test_marker_first_packet(void) {
    uint8_t p[16];
    memcpy(p, P, sizeof P);
    p[1] = 0xE0;   // 0x60 | 0x80 marker set on the very first audio packet
    rtp_header_t h;
    TEST_ASSERT_EQUAL_INT(0, rtp_parse(p, sizeof p, &h));
    TEST_ASSERT_TRUE(h.marker);
    TEST_ASSERT_EQUAL_UINT8(RTP_PT_AUDIO, h.payload_type);  // marker stripped
}

void test_control_type_dispatch(void) {
    // A sync packet (0xD4 = 0x54|0x80): marker high bit set, type = 0x54.
    uint8_t p[16];
    memcpy(p, P, sizeof P);
    p[1] = 0xD4;
    rtp_header_t h;
    TEST_ASSERT_EQUAL_INT(0, rtp_parse(p, sizeof p, &h));
    TEST_ASSERT_TRUE(h.marker);
    TEST_ASSERT_EQUAL_UINT8(0x54, h.payload_type);
}

void test_timestamp_cadence_plus_352(void) {
    // Two consecutive audio packets differ by exactly frameLength=352 in ts.
    uint8_t a[16], b[16];
    memcpy(a, P, sizeof P);
    memcpy(b, P, sizeof P);
    uint32_t ts0 = 0x00010000;
    a[4] = (ts0 >> 24) & 0xFF; a[5] = (ts0 >> 16) & 0xFF;
    a[6] = (ts0 >> 8)  & 0xFF; a[7] = ts0 & 0xFF;
    uint32_t ts1 = ts0 + 352;
    b[4] = (ts1 >> 24) & 0xFF; b[5] = (ts1 >> 16) & 0xFF;
    b[6] = (ts1 >> 8)  & 0xFF; b[7] = ts1 & 0xFF;
    rtp_header_t ha, hb;
    rtp_parse(a, sizeof a, &ha);
    rtp_parse(b, sizeof b, &hb);
    TEST_ASSERT_EQUAL_UINT32(352, hb.timestamp - ha.timestamp);
}

void test_rejects_short(void) {
    rtp_header_t h;
    TEST_ASSERT_EQUAL_INT(-1, rtp_parse(P, 11, &h));
    TEST_ASSERT_EQUAL_INT(-1, rtp_parse(NULL, 16, &h));
}

void test_zero_len_payload_ok(void) {
    rtp_header_t h;
    TEST_ASSERT_EQUAL_INT(0, rtp_parse(P, RTP_HEADER_LEN, &h));
    TEST_ASSERT_EQUAL_size_t(0, h.payload_len);
    TEST_ASSERT_NULL(h.payload);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_audio);
    RUN_TEST(test_marker_first_packet);
    RUN_TEST(test_control_type_dispatch);
    RUN_TEST(test_timestamp_cadence_plus_352);
    RUN_TEST(test_rejects_short);
    RUN_TEST(test_zero_len_payload_ok);
    return UNITY_END();
}
