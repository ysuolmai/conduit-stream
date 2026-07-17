// Host unit tests for the pure status-LED state -> (r,g,b) colour map.
#include <unity.h>
#include <string.h>
#include "led_state.c"
void setUp(void){} void tearDown(void){}

void test_colors_distinct(void){
    led_rgb_t nc = led_state_color(LED_ST_NEEDS_CREDS);
    led_rgb_t wc = led_state_color(LED_ST_WIFI_CONNECTING);
    led_rgb_t ci = led_state_color(LED_ST_CONNECTED_IDLE);
    led_rgb_t st = led_state_color(LED_ST_STREAMING);
    TEST_ASSERT_EQUAL_UINT8(16, nc.r); TEST_ASSERT_EQUAL_UINT8(0, nc.g);
    TEST_ASSERT_EQUAL_UINT8(16, ci.b); TEST_ASSERT_EQUAL_UINT8(0, ci.r);
    TEST_ASSERT_EQUAL_UINT8(16, st.g); TEST_ASSERT_EQUAL_UINT8(0, st.b);
    TEST_ASSERT_EQUAL_UINT8(16, wc.r); TEST_ASSERT_EQUAL_UINT8(6, wc.g);
    // all four differ pairwise
    TEST_ASSERT_TRUE(memcmp(&nc,&wc,sizeof nc) && memcmp(&nc,&ci,sizeof nc) &&
                     memcmp(&nc,&st,sizeof nc) && memcmp(&wc,&ci,sizeof nc) &&
                     memcmp(&wc,&st,sizeof nc) && memcmp(&ci,&st,sizeof nc));
}
void test_out_of_range_off(void){
    led_rgb_t o = led_state_color((system_led_state_t)99);
    TEST_ASSERT_EQUAL_UINT8(0, o.r); TEST_ASSERT_EQUAL_UINT8(0, o.g); TEST_ASSERT_EQUAL_UINT8(0, o.b);
}
int main(void){ UNITY_BEGIN();
    RUN_TEST(test_colors_distinct); RUN_TEST(test_out_of_range_off);
    return UNITY_END(); }
