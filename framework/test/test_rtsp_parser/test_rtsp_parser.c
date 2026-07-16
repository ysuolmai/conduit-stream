#include <unity.h>
#include <string.h>
#include "rtsp_parser.c"  // compile the pure unit directly into the test TU

void setUp(void) {}
void tearDown(void) {}

static const char kOptions[] =
    "OPTIONS * RTSP/1.0\r\n"
    "CSeq: 1\r\n"
    "Apple-Challenge: Sj/xrb+m1nBUdX+d2llW0Q\r\n"
    "User-Agent: AirPlay/409.16\r\n"
    "\r\n";

void test_parses_request_line_and_headers(void) {
    rtsp_request_t r;
    TEST_ASSERT_TRUE(rtsp_parse_request(kOptions, strlen(kOptions), &r));
    TEST_ASSERT_EQUAL_STRING("OPTIONS", r.method);
    TEST_ASSERT_EQUAL_STRING("*", r.uri);
    TEST_ASSERT_EQUAL_STRING("RTSP/1.0", r.version);
    TEST_ASSERT_EQUAL_INT(1, rtsp_cseq(&r));
    TEST_ASSERT_EQUAL_STRING("Sj/xrb+m1nBUdX+d2llW0Q", rtsp_header_get(&r, "Apple-Challenge"));
}

void test_header_lookup_is_case_insensitive(void) {
    rtsp_request_t r;
    rtsp_parse_request(kOptions, strlen(kOptions), &r);
    TEST_ASSERT_EQUAL_STRING("1", rtsp_header_get(&r, "cseq"));
    TEST_ASSERT_EQUAL_STRING("1", rtsp_header_get(&r, "CSEQ"));
    TEST_ASSERT_NULL(rtsp_header_get(&r, "Nonexistent"));
}

void test_body_clamped_to_content_length(void) {
    static const char req[] =
        "ANNOUNCE rtsp://x RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
    rtsp_request_t r;
    TEST_ASSERT_TRUE(rtsp_parse_request(req, strlen(req), &r));
    TEST_ASSERT_EQUAL_INT(5, (int)r.body_len);
    TEST_ASSERT_EQUAL_MEMORY("hello", r.body, 5);
    TEST_ASSERT_EQUAL_INT(5, (int)rtsp_content_length(&r));
}

void test_incomplete_request_returns_false(void) {
    static const char partial[] = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n"; // no blank line yet
    rtsp_request_t r;
    TEST_ASSERT_FALSE(rtsp_parse_request(partial, strlen(partial), &r));
}

void test_incomplete_body_returns_false(void) {
    // Blank line present, but Content-Length exceeds the bytes that have arrived.
    static const char partial[] =
        "ANNOUNCE rtsp://x RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 20\r\n\r\nonly-ten!!";
    rtsp_request_t r;
    TEST_ASSERT_FALSE(rtsp_parse_request(partial, strlen(partial), &r));
}

void test_transport_port_extraction(void) {
    const char *t = "RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002";
    TEST_ASSERT_EQUAL_INT(6001, rtsp_transport_port(t, "control_port"));
    TEST_ASSERT_EQUAL_INT(6002, rtsp_transport_port(t, "timing_port"));
    TEST_ASSERT_EQUAL_INT(-1,   rtsp_transport_port(t, "server_port"));
}

void test_build_response_echoes_cseq(void) {
    char out[256];
    int n = rtsp_build_response(out, sizeof(out), 200, "OK", 1,
                                "Apple-Response: abc\r\n", NULL, 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "RTSP/1.0 200 OK\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "CSeq: 1\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Apple-Response: abc\r\n"));
    TEST_ASSERT_EQUAL_CHAR('\n', out[n - 1]);  // ends with the blank-line CRLF
}

void test_build_response_with_body_sets_content_length(void) {
    char out[256];
    const char *body = "volume: -20.0\r\n";
    rtsp_build_response(out, sizeof(out), 200, "OK", 7, NULL, body, strlen(body));
    TEST_ASSERT_NOT_NULL(strstr(out, "Content-Length: 15\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\r\n\r\nvolume: -20.0\r\n"));
}

void test_build_response_overflow_returns_neg1(void) {
    char tiny[8];
    TEST_ASSERT_EQUAL_INT(-1, rtsp_build_response(tiny, sizeof(tiny), 200, "OK", 1, NULL, NULL, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_request_line_and_headers);
    RUN_TEST(test_header_lookup_is_case_insensitive);
    RUN_TEST(test_body_clamped_to_content_length);
    RUN_TEST(test_incomplete_request_returns_false);
    RUN_TEST(test_incomplete_body_returns_false);
    RUN_TEST(test_transport_port_extraction);
    RUN_TEST(test_build_response_echoes_cseq);
    RUN_TEST(test_build_response_with_body_sets_content_length);
    RUN_TEST(test_build_response_overflow_returns_neg1);
    return UNITY_END();
}
