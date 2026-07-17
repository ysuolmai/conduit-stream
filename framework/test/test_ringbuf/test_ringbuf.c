#include <unity.h>
#include <string.h>
#include "audio_ringbuf.c"  // compile the pure unit directly into the test TU

static audio_ringbuf_t rb;
static int16_t storage[8 * 2];  // 8-frame capacity (7 usable)

void setUp(void)    { audio_ringbuf_init(&rb, storage, 8); }
void tearDown(void) {}

static void fill_frames(int16_t *buf, size_t n, int16_t base) {
    for (size_t i = 0; i < n; i++) { buf[2*i] = base + (int16_t)i; buf[2*i+1] = -(base + (int16_t)i); }
}

void test_starts_empty(void) {
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_available(&rb));
    TEST_ASSERT_EQUAL_UINT(7, audio_ringbuf_free_space(&rb));
}

void test_write_then_read_roundtrip(void) {
    int16_t in[3*2], out[3*2];
    fill_frames(in, 3, 100);
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_write(&rb, in, 3));
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_available(&rb));
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_read(&rb, out, 3));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 3*2);
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_available(&rb));
}

void test_write_saturates_at_capacity(void) {
    int16_t in[10*2];
    fill_frames(in, 10, 1);
    // 7 usable slots -> only 7 accepted.
    TEST_ASSERT_EQUAL_UINT(7, audio_ringbuf_write(&rb, in, 10));
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_free_space(&rb));
}

void test_read_underrun_returns_partial(void) {
    int16_t in[2*2], out[5*2];
    fill_frames(in, 2, 50);
    audio_ringbuf_write(&rb, in, 2);
    TEST_ASSERT_EQUAL_UINT(2, audio_ringbuf_read(&rb, out, 5));  // only 2 available
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_read(&rb, out, 5));  // now empty
}

void test_wraparound_preserves_order(void) {
    int16_t in[5*2], out[5*2];
    // Advance head/tail near the end, then wrap.
    fill_frames(in, 5, 10);
    audio_ringbuf_write(&rb, in, 5);
    audio_ringbuf_read(&rb, out, 5);          // head=tail=5
    fill_frames(in, 5, 200);
    TEST_ASSERT_EQUAL_UINT(5, audio_ringbuf_write(&rb, in, 5));  // wraps past index 8
    TEST_ASSERT_EQUAL_UINT(5, audio_ringbuf_read(&rb, out, 5));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 5*2);
}

void test_drop_advances_tail(void) {
    int16_t in[5*2], out[5*2];
    fill_frames(in, 5, 300);
    audio_ringbuf_write(&rb, in, 5);
    TEST_ASSERT_EQUAL_UINT(2, audio_ringbuf_drop(&rb, 2));      // drop 2 (drift DROP)
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_available(&rb));
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_read(&rb, out, 3));
    // The remaining 3 are frames 2,3,4 of `in` (offset by 2 dropped).
    TEST_ASSERT_EQUAL_INT16_ARRAY(in + 2*2, out, 3*2);
}

void test_drop_clamps_to_available(void) {
    int16_t in[2*2];
    fill_frames(in, 2, 7);
    audio_ringbuf_write(&rb, in, 2);
    TEST_ASSERT_EQUAL_UINT(2, audio_ringbuf_drop(&rb, 9));      // only 2 available
    TEST_ASSERT_EQUAL_UINT(0, audio_ringbuf_available(&rb));
}

void test_first_frame_empty_is_false(void) {
    int16_t f[2];
    TEST_ASSERT_FALSE(audio_ringbuf_first_frame(&rb, f));       // empty -> false
}

void test_first_frame_returns_next_to_play(void) {
    int16_t in[3*2], out[1*2], f[2];
    fill_frames(in, 3, 500);   // frames (500,-500)(501,-501)(502,-502)
    audio_ringbuf_write(&rb, in, 3);
    // DUP source is the OLDEST readable frame (at tail / next to play), NOT head-1.
    TEST_ASSERT_TRUE(audio_ringbuf_first_frame(&rb, f));
    TEST_ASSERT_EQUAL_INT16(500, f[0]);
    TEST_ASSERT_EQUAL_INT16(-500, f[1]);
    TEST_ASSERT_EQUAL_UINT(3, audio_ringbuf_available(&rb));    // did NOT advance tail
    // Tracks tail: after one frame is played the next-to-play frame advances.
    audio_ringbuf_read(&rb, out, 1);
    TEST_ASSERT_TRUE(audio_ringbuf_first_frame(&rb, f));
    TEST_ASSERT_EQUAL_INT16(501, f[0]);
    TEST_ASSERT_EQUAL_INT16(-501, f[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_write_then_read_roundtrip);
    RUN_TEST(test_write_saturates_at_capacity);
    RUN_TEST(test_read_underrun_returns_partial);
    RUN_TEST(test_wraparound_preserves_order);
    RUN_TEST(test_drop_advances_tail);
    RUN_TEST(test_drop_clamps_to_available);
    RUN_TEST(test_first_frame_empty_is_false);
    RUN_TEST(test_first_frame_returns_next_to_play);
    return UNITY_END();
}
