// Host unit tests for the pure seq-indexed RTP reorder / jitter buffer. Compiles
// the unit directly. Exercises in-order, reorder, dup, too-old/too-new, badlen,
// gap detection, wait->recover, conceal-on-hold-exceeded, and 16-bit wraparound.
#include <unity.h>
#include <string.h>
#include "rtp_reorder.c"
void setUp(void){} void tearDown(void){}

#define W 8
#define HOLD 6
static rtp_reorder_slot_t g_slots[W];
static rtp_reorder_t rb;
static void reset(uint16_t anchor){
    memset(g_slots,0,sizeof g_slots);
    rtp_reorder_init(&rb,g_slots,W,HOLD);
    rtp_reorder_anchor(&rb,anchor);
}
static rtp_ins_t ins(uint16_t seq){ uint8_t b[4]={ (uint8_t)seq,0,0,0 };
    return rtp_reorder_insert(&rb,seq,b,sizeof b); }
static rtp_pop_t pop(uint16_t *seq){ uint8_t o[16]; size_t ol=0; uint16_t s=0;
    rtp_pop_t r=rtp_reorder_pop(&rb,o,sizeof o,&ol,&s); if(seq)*seq=s; return r; }

void test_inorder(void){ reset(100);
    TEST_ASSERT_EQUAL_INT(RTP_INS_STORED, ins(100));
    TEST_ASSERT_EQUAL_INT(RTP_INS_STORED, ins(101));
    uint16_t s; TEST_ASSERT_EQUAL_INT(RTP_POP_OK, pop(&s)); TEST_ASSERT_EQUAL_UINT16(100,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK, pop(&s)); TEST_ASSERT_EQUAL_UINT16(101,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_EMPTY, pop(&s)); }

void test_reorders(void){ reset(0);
    ins(2); ins(0); ins(1);              // arrive out of order
    uint16_t s; TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(1,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(2,s); }

void test_dup(void){ reset(0); TEST_ASSERT_EQUAL_INT(RTP_INS_STORED,ins(0));
    TEST_ASSERT_EQUAL_INT(RTP_INS_DUP,ins(0)); TEST_ASSERT_EQUAL_size_t(1,rtp_reorder_count(&rb)); }

void test_too_old(void){ reset(10); ins(10); pop(NULL);   // next now 11
    TEST_ASSERT_EQUAL_INT(RTP_INS_TOO_OLD, ins(9)); }

void test_too_new(void){ reset(0);
    TEST_ASSERT_EQUAL_INT(RTP_INS_STORED, ins(W-1));      // last in-window slot
    TEST_ASSERT_EQUAL_INT(RTP_INS_TOO_NEW, ins(W)); }      // one past the window

void test_badlen(void){ reset(0);
    uint8_t b[1]; TEST_ASSERT_EQUAL_INT(RTP_INS_BADLEN, rtp_reorder_insert(&rb,0,b,0)); }

void test_gap_report(void){ reset(0);
    ins(3);                               // hole at 0,1,2; 3 present
    uint16_t f=999,c=999;
    TEST_ASSERT_TRUE(rtp_reorder_gap(&rb,&f,&c));
    TEST_ASSERT_EQUAL_UINT16(0,f); TEST_ASSERT_EQUAL_UINT16(3,c);  // missing 0,1,2
    ins(0); ins(1); ins(2);
    TEST_ASSERT_FALSE(rtp_reorder_gap(&rb,&f,&c)); }

void test_wait_then_recover(void){ reset(0);
    ins(1);                               // hole at 0, only 1 buffered, hold not exceeded
    TEST_ASSERT_EQUAL_INT(RTP_POP_WAIT, pop(NULL));       // cursor holds at 0
    ins(0);                               // recovered (e.g. via resend)
    uint16_t s; TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(1,s); }

void test_conceal_when_hold_exceeded(void){ reset(0);
    // Leave 0 missing; fill enough newer packets that head - next > hold.
    for(uint16_t s=1;s<=HOLD;s++) ins(s);                // span = HOLD+1 > hold
    TEST_ASSERT_EQUAL_INT(RTP_POP_CONCEAL, pop(NULL));   // give up on 0, advance
    uint16_t s; TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(1,s); }

void test_wraparound(void){ reset(0xFFFE);
    ins(0xFFFF); ins(0x0000); ins(0xFFFE); ins(0x0001);  // straddles the 16-bit wrap
    uint16_t s;
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0xFFFE,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0xFFFF,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0x0000,s);
    TEST_ASSERT_EQUAL_INT(RTP_POP_OK,pop(&s)); TEST_ASSERT_EQUAL_UINT16(0x0001,s); }

void test_gap_wraparound(void){ reset(0xFFFF);
    ins(0x0001);                          // hole at 0xFFFF, 0x0000
    uint16_t f=0,c=0; TEST_ASSERT_TRUE(rtp_reorder_gap(&rb,&f,&c));
    TEST_ASSERT_EQUAL_UINT16(0xFFFF,f); TEST_ASSERT_EQUAL_UINT16(2,c); }

int main(void){ UNITY_BEGIN();
    RUN_TEST(test_inorder); RUN_TEST(test_reorders); RUN_TEST(test_dup);
    RUN_TEST(test_too_old); RUN_TEST(test_too_new); RUN_TEST(test_badlen);
    RUN_TEST(test_gap_report); RUN_TEST(test_wait_then_recover);
    RUN_TEST(test_conceal_when_hold_exceeded); RUN_TEST(test_wraparound);
    RUN_TEST(test_gap_wraparound);
    return UNITY_END(); }
