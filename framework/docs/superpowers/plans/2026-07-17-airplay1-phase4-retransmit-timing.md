# AirPlay 1 (RAOP) — Phase 4: seq-indexed jitter/reorder + RTP retransmit + timing channel + drift safeguard

**Goal (spec §5c + §6c + §6f + phasing row 4):** "Retransmit + underrun/drift safeguards → **clean
audio under real Wi-Fi packet loss.**" Replace the Phase-3 arrival-order decode with a **seq-indexed
reorder/jitter buffer** that slots late/out-of-order/retransmitted packets into RTP-sequence order
*before* decode; on a detected gap, send a **resend request** on the control socket and inject the
recovered packet by seq; service the **timing channel** the way real senders expect (receiver-initiated
0xd2 requests) so a sender never tears us down; and add the **drift drop/dup** watermark to the ring drain
so a multi-hour session never slowly underruns from sender/DAC ppm mismatch.

**Branch:** `feat/airplay-phase1-wifi-mdns` (phases stacked here).

**Architecture:** four new **pure** units — three in `components/raop`
(`rtp_reorder`, `rtp_resend`, `rtp_timing`) and one in `components/audio` (`audio_drift`) — each with
zero ESP-IDF/PSA/lwip includes and full Unity host tests. The Phase-3 glue `raop_rtp.c` is rewritten from
a single `recvfrom(audio_fd)` loop into a `select()` loop over **all three** UDP sockets
(audio / control / timing) that (a) inserts encrypted audio packets into the reorder buffer by seq,
(b) drains the buffer in seq order → decrypt → decode → `audio_play_pcm`, (c) requests resends on gaps,
(d) injects resend responses, and (e) periodically emits timing requests. The audio drain
(`audio_playback.c`) grows a drift drop/dup step driven by `audio_drift`.

**Tech stack:** ESP-IDF 6.0.1, PlatformIO, C11, Unity host tests. AES-128-CBC via **PSA Crypto**
(unchanged from Phase 3). Reorder-buffer storage lives in **PSRAM** (spec §6c), caller-owned like the
existing PCM ring.

**Keep intact:** Phases 0–3 — RTSP handshake, RSA challenge, AES key exchange, UDP bind, ANNOUNCE/SETUP,
the first-audio decrypt+decode path, the single-producer diag-tone handoff. The existing **47** host tests
MUST stay green; the firmware MUST build green (`-e esp32-s3-n16r8`).

---

## ⚠️ Interop-critical wire format — exactly what we implement (cited)

**Sources:** shairport-sync (Mike Brady) `rtp.c` — fetched & quoted 2026-07-17
(`rtp_request_resend`, `rtp_timing_sender`, `rtp_timing_receiver`, the `packet[1]==0xd6` resend-response
path); philippe44 `RAOP-Player` `raop_client.c`; the verified research capture in
`/private/tmp/claude-501/-Users-uziiuzair-ooozzy-conduit-stream/9e1f7bb1-1393-4743-b79a-d21eb5991c91/scratchpad/research-phase3-5.md`
(agent 3 "RTP audio + retransmit + timing", **critic corrections G1/G2/G3 applied**).

All RAOP UDP control packets share the 4-byte-ish RTP prefix `packet[0]=0x80`, `packet[1]=0x80|type`;
dispatch on `type = packet[1] & 0x7f`. Audio is `0x60` (high bit clear); control types carry the 0x80 bit.

### RESEND REQUEST — receiver → sender **control** port — 8 bytes

Verbatim from shairport `rtp_request_resend()`:
```
req[0] = 0x80;
req[1] = 0x55 | 0x80;                       // = 0xD5
*(uint16_t*)(req+2) = htons(1);             // our seqno, constant 1
*(uint16_t*)(req+4) = htons(first);         // first missing audio seqno   (BE16)
*(uint16_t*)(req+6) = htons(count);         // number of consecutive pkts  (BE16)
// sendto(control_socket, req, 8, ...)  → the SENDER's control port
```
```
off size field
 0   1   0x80
 1   1   0xD5   (0x55 | 0x80)
 2   2   0x0001 (BE)  our seqno (constant)
 4   2   first_missing_seq (BE)
 6   2   count             (BE)
```

### RESEND RESPONSE — sender → receiver **control** port

From shairport's control-socket handler: `packet[1] == 0xd6` (`0x56|0x80`). The wrapper is **4 bytes**;
the original, complete audio RTP packet (its own 12-byte header + encrypted ALAC) begins at **offset 4**:
```
pktp = packet + 4;  plen -= 4;             // strip the 4-byte 0xd6 wrapper
// pktp now points at a normal audio packet:
//   pktp[0]=0x80 pktp[1]=0x60  seq@pktp+2  ts@pktp+4  ssrc@pktp+8  ALAC@pktp+12
// inner ALAC payload begins at packet offset 4+12 = 16
```
```
off size field
 0   1   0x80
 1   1   0xD6   (0x56 | 0x80)
 2   2   seq echo (BE)   (ignored; we trust the inner header)
 4  ..   FULL original audio packet → parse with rtp_parse(packet+4, len-4, …)
```

### TIMING CHANNEL — **receiver is the initiator** (task premise was backwards; verified)

> ⚠️ **The task brief said "recv 0x53 request / send 0x52 response." That is inverted.** shairport
> `rtp_timing_sender()` **sends** `packet[1]=0xd2` (request) to the sender's timing port and
> `rtp_timing_receiver()` **receives** `packet[1]=0xd3` (response). The sender **never** sends a timing
> request that the receiver must answer — so there is nothing to "reply to." The minimum that keeps a real
> sender from tearing us down is to **periodically emit well-formed 0xd2 requests and consume the 0xd3
> responses** (discarding the clock data — we free-run, spec §5c). This is exactly what shairport and the
> esp-airsync ESP32 receiver do. **Decision:** implement the receiver-initiated sender as the primary path.
> As a harmless defensive belt we also answer an inbound 0xd2 with a 0xd3 (some non-standard senders); this
> is documented, not relied upon.

Both timing packets are **32 bytes**, three NTP-64 timestamps (each = BE u32 seconds-since-1900 :: BE u32
fraction). From shairport `rtp_timing_sender` / `rtp_timing_receiver`:
```
TIMING REQUEST  (0xd2)  receiver → sender timing port, 32 bytes
 off size field
  0   1   0x80
  1   1   0xD2   (0x52 | 0x80)
  2   2   0x0007 (BE)   seqno (constant 7, per shairport)
  4   4   0            filler
  8   8   origin   NTP64 = 0
 16   8   receive  NTP64 = 0
 24   8   transmit NTP64 = our clock at send (t1)   ← may be 0 for pure free-run

TIMING RESPONSE (0xd3)  sender → receiver timing port, 32 bytes
  0   1   0x80
  1   1   0xD3   (0x53 | 0x80)
  2   2   seqno (BE)
  8   8   origin   NTP64 = echo of our transmit t1
 16   8   receive  NTP64 = sender receive time  t2   (shairport: nctohl(&packet[16]) sec, [20] frac)
 24   8   transmit NTP64 = sender transmit time t3   (shairport: nctohl(&packet[24]) sec, [28] frac)
```
**Free-run consumption:** we parse the 0xd3 but do **not** clock-discipline (no resample). Sending the
0xd2 at a steady cadence (~every 3 s, matching shairport's ~3 s interval) is what satisfies the sender.

### SYNC (0xd4) — ignored

Sync packets (`packet[1]=0xd4`) arrive on the control socket; a free-run receiver ignores them (spec §5c).
Logged-and-dropped.

---

## KEY ARCHITECTURAL DECISION — reorder **before** decode (encrypted-by-seq)

Spec §6c: "ring indexed by **RTP sequence**." Spec §5c: "slot the recovered packet back in by sequence
**before decrypt**." Both point the same way, and it is the correct choice:

- Each RAOP audio packet is **one independently-decodable ALAC frame** (no cross-frame decoder state), so
  decode order is free — we may buffer the *encrypted* packets by seq and decode at pop time.
- Retransmit **responses are encrypted RTP audio packets** (0xd6 wraps a raw 0x60 packet). Buffering
  encrypted-by-seq lets a recovered packet drop into its slot and flow through the **single** decode path,
  with no separate "decode a recovered packet" branch and no duplicate decrypt/decode code.
- The reorder buffer therefore stores raw encrypted payloads keyed by seq; the drain pops in seq order and
  runs the existing Phase-3 decrypt→decode→`audio_play_pcm` pipeline per packet.

The reorder buffer is the **network-side** jitter/reorder window (holds during loss so retransmit has
time to land). The existing 2-s PCM ring is the **playback-side** buffer. In steady state the drain empties
the reorder buffer as fast as the backpressured PCM ring accepts, so the reorder buffer sits near-empty and
adds no latency; it only fills when a gap forces a hold. Reorder-buffer storage: `window` slots in PSRAM,
caller-owned (like `audio_ringbuf`).

---

## File structure

```
components/raop/
  src/rtp_reorder.h / rtp_reorder.c    # CREATE pure: seq-indexed reorder + gap detect + conceal (16-bit wrap)
  src/rtp_resend.h  / rtp_resend.c     # CREATE pure: 8-byte resend-request builder + 0xd6 response unwrap
  src/rtp_timing.h  / rtp_timing.c     # CREATE pure: 0xd2/0xd3 timing codec + NTP64 pack/unpack
  src/raop_rtp.c                       # EDIT glue: select() over 3 sockets; seq-ordered decode; resend; timing sender
  src/raop_rtp.h                       # EDIT: raop_rtp_start now takes the whole session (control/timing fds + peer)
  CMakeLists.txt                       # EDIT: add rtp_reorder.c rtp_resend.c rtp_timing.c

components/audio/
  src/audio_drift.h / audio_drift.c    # CREATE pure: high/low watermark drop/dup decision
  src/audio_ringbuf.h / audio_ringbuf.c# EDIT: add audio_ringbuf_drop() + audio_ringbuf_last_frame() for drift
  src/audio_playback.c                 # EDIT glue: apply drift drop/dup once per drain cycle
  CMakeLists.txt                       # EDIT: add audio_drift.c

test/test_rtp_reorder/test_rtp_reorder.c   # CREATE Unity host tests
test/test_rtp_resend/test_rtp_resend.c     # CREATE Unity host tests
test/test_rtp_timing/test_rtp_timing.c     # CREATE Unity host tests
test/test_audio_drift/test_audio_drift.c   # CREATE Unity host tests

platformio.ini                         # (native -I already covers components/raop/src + components/audio/src)
```

**Pure vs glue (house rule):** `rtp_reorder`, `rtp_resend`, `rtp_timing`, `audio_drift` are pure C,
host-tested by `#include`-ing the `.c` directly (as `test_sdp.c`/`test_rtp_parser.c` do). `raop_rtp.c` and
`audio_playback.c` are target-only glue. No new `-I` needed (both src dirs are already on `[env:native]`).

---

## Task 1: `rtp_reorder` — pure seq-indexed reorder / gap / conceal (host-tested)

**Files:** create `components/raop/src/rtp_reorder.{h,c}`, `test/test_rtp_reorder/test_rtp_reorder.c`.

The heart of Phase 4. A fixed `window` of slots, keyed by `seq % window`, caller-owned storage (PSRAM on
target). Tracks a 16-bit **play cursor** `next` (the next seq to emit) and a 16-bit **write head**
(highest seq+1 seen). All seq math is mod-2¹⁶ so it survives wraparound. The unit is the *state machine
only* — it copies packet bytes in/out; it does no crypto/decode/socket work.

- [ ] **Step 1: Header** `rtp_reorder.h`:
  ```c
  // Pure seq-indexed RTP reorder / jitter buffer. Holds ENCRYPTED audio payloads
  // keyed by 16-bit RTP sequence so late / out-of-order / retransmitted packets
  // slot into play order BEFORE decode (spec §6c/§5c). No ESP-IDF/crypto/socket
  // deps — host-unit-testable. Storage is caller-owned (target: PSRAM); this file
  // is allocator-free, mirroring audio_ringbuf.
  //
  // 16-bit seq wraparound is handled everywhere via modular comparison:
  //   seq_lt(a,b)   == (int16_t)(a - b) < 0        (a strictly "before" b)
  //   seq_diff(a,b) == (uint16_t)(a - b)           (packets from b up to a)
  #pragma once
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>

  #define RTP_REORDER_MAX_PKT 1536   // max stored encrypted audio payload (bytes)

  typedef struct {
      bool     valid;                 // slot holds an unread packet
      uint16_t seq;                   // its RTP sequence
      uint16_t len;                   // payload byte count (<= RTP_REORDER_MAX_PKT)
      uint8_t  data[RTP_REORDER_MAX_PKT];
  } rtp_reorder_slot_t;

  typedef struct {
      rtp_reorder_slot_t *slots;      // caller-owned, `window` entries
      uint16_t window;                // ring size in packets (target latency budget)
      uint16_t hold;                  // conceal threshold: max packets to hold at a gap
      uint16_t next;                  // next seq to pop (play cursor)
      uint16_t head;                  // highest seq seen + 1 (write head)
      uint16_t count;                 // packets currently buffered
      bool     started;               // false until anchored
  } rtp_reorder_t;

  typedef enum {
      RTP_INS_STORED = 0,  // accepted into its slot
      RTP_INS_DUP,         // slot already held this seq (retransmit of a pkt we have)
      RTP_INS_TOO_OLD,     // seq < next: already played/concealed — dropped
      RTP_INS_TOO_NEW,     // seq >= next+window: outside the window — dropped
      RTP_INS_BADLEN,      // len 0 or > RTP_REORDER_MAX_PKT — dropped
  } rtp_ins_t;

  typedef enum {
      RTP_POP_OK = 0,      // *out holds the next in-order packet; cursor advanced
      RTP_POP_WAIT,        // next seq missing but still within the hold window — caller
                           //   should wait / (re)request; cursor NOT advanced
      RTP_POP_CONCEAL,     // next seq missing AND hold exceeded — cursor advanced past
                           //   the hole; caller emits one concealment (silence) frame
      RTP_POP_EMPTY,       // nothing buffered / not started
  } rtp_pop_t;

  // Anchor the play cursor to the RECORD RTP-Info seq before the first insert.
  // Optional: if never called, the first insert anchors to its own seq.
  void      rtp_reorder_init(rtp_reorder_t *rb, rtp_reorder_slot_t *slots,
                             uint16_t window, uint16_t hold);
  void      rtp_reorder_anchor(rtp_reorder_t *rb, uint16_t first_seq);

  // Copy one encrypted audio payload into its seq slot. Returns a classification.
  rtp_ins_t rtp_reorder_insert(rtp_reorder_t *rb, uint16_t seq,
                               const uint8_t *data, size_t len);

  // Pop the next packet in seq order (see rtp_pop_t). On RTP_POP_OK, *out_len is set
  // and up to out_cap bytes are copied to out.
  rtp_pop_t rtp_reorder_pop(rtp_reorder_t *rb, uint8_t *out, size_t out_cap,
                            size_t *out_len, uint16_t *out_seq);

  // If the play cursor is stalled on a gap (next missing but a later seq is present),
  // report the resend range: *first = next, *count = consecutive missing from next up
  // to the first buffered seq (clamped to `window`). Returns true if a resend is due.
  bool      rtp_reorder_gap(const rtp_reorder_t *rb, uint16_t *first, uint16_t *count);

  static inline size_t rtp_reorder_count(const rtp_reorder_t *rb) { return rb->count; }
  ```

- [ ] **Step 2: Failing tests** `test/test_rtp_reorder/test_rtp_reorder.c` (`#include "rtp_reorder.c"`).
  Use a small window so the struct fits on the host; give it a static slot array.
  ```c
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
  ```

- [ ] **Step 3: Implement** `rtp_reorder.c`:
  ```c
  #include "rtp_reorder.h"
  #include <string.h>

  static inline bool     seq_lt(uint16_t a, uint16_t b){ return (int16_t)(a - b) < 0; }
  static inline uint16_t seq_diff(uint16_t a, uint16_t b){ return (uint16_t)(a - b); }

  void rtp_reorder_init(rtp_reorder_t *rb, rtp_reorder_slot_t *slots,
                        uint16_t window, uint16_t hold){
      rb->slots = slots; rb->window = window;
      rb->hold = (hold && hold < window) ? hold : (uint16_t)(window - 1);
      rb->next = rb->head = rb->count = 0; rb->started = false;
      for (uint16_t i = 0; i < window; i++) slots[i].valid = false;
  }
  void rtp_reorder_anchor(rtp_reorder_t *rb, uint16_t first_seq){
      rb->next = rb->head = first_seq; rb->started = true;
  }

  rtp_ins_t rtp_reorder_insert(rtp_reorder_t *rb, uint16_t seq,
                               const uint8_t *data, size_t len){
      if (len == 0 || len > RTP_REORDER_MAX_PKT) return RTP_INS_BADLEN;
      if (!rb->started){ rb->next = rb->head = seq; rb->started = true; }
      if (seq_lt(seq, rb->next)) return RTP_INS_TOO_OLD;      // already past it
      if (seq_diff(seq, rb->next) >= rb->window) return RTP_INS_TOO_NEW;
      rtp_reorder_slot_t *sl = &rb->slots[seq % rb->window];
      if (sl->valid && sl->seq == seq) return RTP_INS_DUP;    // retransmit we already hold
      if (!sl->valid) rb->count++;
      sl->valid = true; sl->seq = seq; sl->len = (uint16_t)len;
      memcpy(sl->data, data, len);
      if (seq_lt(rb->head, (uint16_t)(seq + 1))) rb->head = (uint16_t)(seq + 1);
      return RTP_INS_STORED;
  }

  rtp_pop_t rtp_reorder_pop(rtp_reorder_t *rb, uint8_t *out, size_t out_cap,
                            size_t *out_len, uint16_t *out_seq){
      if (!rb->started || rb->count == 0) return RTP_POP_EMPTY;
      rtp_reorder_slot_t *sl = &rb->slots[rb->next % rb->window];
      if (sl->valid && sl->seq == rb->next){
          size_t n = sl->len; if (n > out_cap) n = out_cap;
          memcpy(out, sl->data, n);
          if (out_len) *out_len = n; if (out_seq) *out_seq = rb->next;
          sl->valid = false; rb->count--; rb->next++;
          return RTP_POP_OK;
      }
      // Gap at `next`. Hold unless the write head has run past the hold window.
      if (seq_diff(rb->head, rb->next) > rb->hold){
          rb->next++;                      // give up on the missing packet
          return RTP_POP_CONCEAL;
      }
      return RTP_POP_WAIT;
  }

  bool rtp_reorder_gap(const rtp_reorder_t *rb, uint16_t *first, uint16_t *count){
      if (!rb->started || rb->count == 0) return false;
      const rtp_reorder_slot_t *sl = &rb->slots[rb->next % rb->window];
      if (sl->valid && sl->seq == rb->next) return false;     // front is present
      uint16_t span = seq_diff(rb->head, rb->next);           // 1.. up to window
      uint16_t miss = 0;
      for (uint16_t i = 0; i < span && i < rb->window; i++){
          uint16_t s = (uint16_t)(rb->next + i);
          const rtp_reorder_slot_t *p = &rb->slots[s % rb->window];
          if (p->valid && p->seq == s) break;                 // first present seq ends the run
          miss++;
      }
      if (miss == 0) return false;
      if (first) *first = rb->next; if (count) *count = miss;
      return true;
  }
  ```

- [ ] **Step 4:** `~/.platformio/penv/bin/pio test -e native` — new suite green, prior 47 green.
- [ ] **Step 5: Commit** `feat(raop): pure seq-indexed RTP reorder/jitter buffer with gap+conceal (16-bit wrap)`.

## Task 2: `rtp_resend` — pure resend-request builder + response unwrap (host-tested)

**Files:** create `components/raop/src/rtp_resend.{h,c}`, `test/test_rtp_resend/test_rtp_resend.c`.

- [ ] **Step 1: Header** `rtp_resend.h`:
  ```c
  // Pure builders/parsers for the RAOP retransmit control packets (shairport rtp.c
  // rtp_request_resend + the packet[1]==0xd6 resend-response path). No ESP-IDF deps.
  #pragma once
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>

  #define RESEND_REQ_LEN 8
  #define RESEND_RESP_TYPE 0x56   // packet[1] & 0x7f for a resend response (wire 0xd6)
  #define RESEND_RESP_HDR  4      // 4-byte 0xd6 wrapper before the inner audio packet

  // Build the 8-byte resend REQUEST (receiver -> sender control port):
  //   [0]=0x80 [1]=0xD5 [2..3]=htons(1) [4..5]=htons(first) [6..7]=htons(count)
  void rtp_resend_build(uint8_t out[RESEND_REQ_LEN], uint16_t first_missing, uint16_t count);

  // Validate an incoming resend RESPONSE (control socket) and expose the wrapped
  // original audio packet. Returns 0 and sets *inner/*inner_len (= pkt+4 / len-4) when
  // pkt[1]==0xd6 and len is big enough to hold a wrapped 12-byte RTP header; -1 else.
  int  rtp_resend_unwrap(const uint8_t *pkt, size_t len,
                         const uint8_t **inner, size_t *inner_len);
  ```

- [ ] **Step 2: Failing tests** (`#include "rtp_resend.c"`):
  ```c
  #include <unity.h>
  #include <string.h>
  #include "rtp_resend.c"
  void setUp(void){} void tearDown(void){}

  void test_build(void){
      uint8_t r[RESEND_REQ_LEN];
      rtp_resend_build(r, 0x1234, 3);
      TEST_ASSERT_EQUAL_UINT8(0x80, r[0]);
      TEST_ASSERT_EQUAL_UINT8(0xD5, r[1]);
      TEST_ASSERT_EQUAL_UINT8(0x00, r[2]); TEST_ASSERT_EQUAL_UINT8(0x01, r[3]); // htons(1)
      TEST_ASSERT_EQUAL_UINT8(0x12, r[4]); TEST_ASSERT_EQUAL_UINT8(0x34, r[5]); // first BE
      TEST_ASSERT_EQUAL_UINT8(0x00, r[6]); TEST_ASSERT_EQUAL_UINT8(0x03, r[7]); // count BE
  }
  void test_unwrap_ok(void){
      // 0xd6 wrapper + a 16-byte inner audio packet (12 hdr + 4 payload)
      uint8_t p[4+16] = {0x80,0xD6,0x00,0x00,
          0x80,0x60,0xAB,0xCD, 0,0,0,0, 0,0,0,0, 0xDE,0xAD,0xBE,0xEF};
      const uint8_t *in; size_t il;
      TEST_ASSERT_EQUAL_INT(0, rtp_resend_unwrap(p, sizeof p, &in, &il));
      TEST_ASSERT_EQUAL_PTR(p+4, in);
      TEST_ASSERT_EQUAL_size_t(16, il);
      TEST_ASSERT_EQUAL_UINT8(0x60, in[1]);   // inner is a normal audio packet
  }
  void test_unwrap_rejects_wrong_type(void){
      uint8_t p[20] = {0x80,0xD4}; const uint8_t *in; size_t il;   // 0xd4 sync, not resend
      TEST_ASSERT_EQUAL_INT(-1, rtp_resend_unwrap(p, sizeof p, &in, &il));
  }
  void test_unwrap_rejects_short(void){
      uint8_t p[15] = {0x80,0xD6}; const uint8_t *in; size_t il;   // < 4 + 12
      TEST_ASSERT_EQUAL_INT(-1, rtp_resend_unwrap(p, sizeof p, &in, &il));
  }
  int main(void){ UNITY_BEGIN();
      RUN_TEST(test_build); RUN_TEST(test_unwrap_ok);
      RUN_TEST(test_unwrap_rejects_wrong_type); RUN_TEST(test_unwrap_rejects_short);
      return UNITY_END(); }
  ```

- [ ] **Step 3: Implement** `rtp_resend.c` — big-endian writes via explicit shifts (no `htons`, keeps it
  host-pure):
  ```c
  #include "rtp_resend.h"
  #define RTP_HDR_MIN 12
  void rtp_resend_build(uint8_t out[RESEND_REQ_LEN], uint16_t first_missing, uint16_t count){
      out[0]=0x80; out[1]=0xD5;                 // 0x55 | 0x80
      out[2]=0x00; out[3]=0x01;                 // htons(1)
      out[4]=(uint8_t)(first_missing>>8); out[5]=(uint8_t)first_missing;
      out[6]=(uint8_t)(count>>8);         out[7]=(uint8_t)count;
  }
  int rtp_resend_unwrap(const uint8_t *pkt, size_t len,
                        const uint8_t **inner, size_t *inner_len){
      if (pkt == NULL || len < (size_t)(RESEND_RESP_HDR + RTP_HDR_MIN)) return -1;
      if ((pkt[1] & 0x7f) != RESEND_RESP_TYPE) return -1;   // require 0xd6
      if (inner) *inner = pkt + RESEND_RESP_HDR;
      if (inner_len) *inner_len = len - RESEND_RESP_HDR;
      return 0;
  }
  ```

- [ ] **Step 4:** `pio test -e native` green.
- [ ] **Step 5: Commit** `feat(raop): pure RTP resend-request builder + 0xd6 response unwrap with host tests`.

## Task 3: `rtp_timing` — pure 0xd2/0xd3 timing codec + NTP64 (host-tested)

**Files:** create `components/raop/src/rtp_timing.{h,c}`, `test/test_rtp_timing/test_rtp_timing.c`.

Receiver-initiated: we **build** 0xd2 requests and **parse** 0xd3 responses (proven direction, §wire
format). We also provide the mirror pair (parse request / build response) for the documented defensive
belt only. NTP64 is a plain `uint64_t` (sec<<32 | frac) packed big-endian.

- [ ] **Step 1: Header** `rtp_timing.h`:
  ```c
  // Pure codec for the RAOP timing channel (shairport rtp_timing_sender / _receiver).
  // The RECEIVER is the initiator: it SENDS 0xd2 requests and CONSUMES 0xd3 responses
  // (verified against shairport rtp.c 2026-07-17 — the task's "reply to requests"
  // premise was inverted). We free-run: responses are parsed but NOT used for clock
  // discipline; sending requests at a steady cadence keeps the sender from tearing us
  // down. The request-parse / response-build pair exists ONLY for the documented
  // defensive belt (answer a non-standard sender's 0xd2). No ESP-IDF deps.
  #pragma once
  #include <stdint.h>
  #include <stddef.h>

  #define TIMING_PKT_LEN   32
  #define TIMING_REQ_TYPE  0x52   // (pkt[1] & 0x7f); wire 0xd2
  #define TIMING_RESP_TYPE 0x53   // (pkt[1] & 0x7f); wire 0xd3

  typedef struct {                 // three NTP-64 stamps (sec<<32 | frac)
      uint64_t origin;             // bytes  8..15
      uint64_t receive;            // bytes 16..23
      uint64_t transmit;           // bytes 24..31
  } timing_stamps_t;

  // Build a 0xd2 REQUEST: transmit = our clock at send (t1); origin/receive = 0.
  void rtp_timing_build_request(uint8_t out[TIMING_PKT_LEN], uint64_t transmit_ntp);

  // Parse a 32-byte datagram; sets *out_type to TIMING_REQ_TYPE / TIMING_RESP_TYPE
  // and fills *st. Returns 0 on success, -1 if len<32 or pkt[1]&0x7f is neither type.
  int  rtp_timing_parse(const uint8_t *pkt, size_t len,
                        uint8_t *out_type, timing_stamps_t *st);

  // Defensive belt only: build a 0xd3 RESPONSE echoing a client's transmit as origin,
  // and stamping our receive/transmit clocks.
  void rtp_timing_build_response(uint8_t out[TIMING_PKT_LEN],
                                 uint64_t origin_ntp, uint64_t receive_ntp,
                                 uint64_t transmit_ntp);
  ```

- [ ] **Step 2: Failing tests** (`#include "rtp_timing.c"`):
  ```c
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
  ```

- [ ] **Step 3: Implement** `rtp_timing.c`:
  ```c
  #include "rtp_timing.h"
  static void wr_be64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i)); }
  static uint64_t rd_be64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v; }

  void rtp_timing_build_request(uint8_t out[TIMING_PKT_LEN], uint64_t transmit_ntp){
      for (int i=0;i<TIMING_PKT_LEN;i++) out[i]=0;
      out[0]=0x80; out[1]=0xD2; out[2]=0x00; out[3]=0x07;   // 0x52|0x80, htons(7)
      wr_be64(out+24, transmit_ntp);                        // t1
  }
  void rtp_timing_build_response(uint8_t out[TIMING_PKT_LEN],
                                 uint64_t origin_ntp, uint64_t receive_ntp,
                                 uint64_t transmit_ntp){
      for (int i=0;i<TIMING_PKT_LEN;i++) out[i]=0;
      out[0]=0x80; out[1]=0xD3; out[2]=0x00; out[3]=0x07;   // 0x53|0x80, htons(7)
      wr_be64(out+8,  origin_ntp);
      wr_be64(out+16, receive_ntp);
      wr_be64(out+24, transmit_ntp);
  }
  int rtp_timing_parse(const uint8_t *pkt, size_t len,
                       uint8_t *out_type, timing_stamps_t *st){
      if (pkt == NULL || len < TIMING_PKT_LEN) return -1;
      uint8_t t = pkt[1] & 0x7f;
      if (t != TIMING_REQ_TYPE && t != TIMING_RESP_TYPE) return -1;
      if (out_type) *out_type = t;
      if (st){ st->origin=rd_be64(pkt+8); st->receive=rd_be64(pkt+16); st->transmit=rd_be64(pkt+24); }
      return 0;
  }
  ```

- [ ] **Step 4:** `pio test -e native` green.
- [ ] **Step 5: Commit** `feat(raop): pure RAOP timing 0xd2/0xd3 codec + NTP64 (receiver-initiated) with host tests`.

## Task 4: `audio_drift` — pure watermark drop/dup decision (host-tested)

**Files:** create `components/audio/src/audio_drift.{h,c}`, `test/test_audio_drift/test_audio_drift.c`.

Spec §6f free-run drift. Pure function of the ring's current fill vs a high/low band around the target
depth. Drop one frame (~23 µs) above high (sender faster / buffer filling), duplicate one below low
(sender slower / buffer draining) — but **never** at `avail == 0` (that is a true underrun → the existing
silence path owns it). One action per drain cycle; the buffer self-limits back into the band.

- [ ] **Step 1: Header** `audio_drift.h`:
  ```c
  // Pure free-run drift safeguard (spec §6f). Over a multi-hour session the sender
  // and DAC clocks differ by a few ppm; the PCM ring slowly fills or drains. When
  // it crosses a high/low watermark we drop or duplicate ONE frame (~23 µs @44.1k,
  // inaudible) to hold the buffer near its target depth. No ESP-IDF deps.
  #pragma once
  #include <stddef.h>

  typedef enum {
      AUDIO_DRIFT_NONE = 0,
      AUDIO_DRIFT_DROP,   // buffer above high watermark: discard one frame
      AUDIO_DRIFT_DUP,    // buffer below low watermark (and non-empty): repeat one frame
  } audio_drift_action_t;

  typedef struct {
      size_t high;        // drop when avail >= high
      size_t low;         // dup  when 0 < avail <= low
  } audio_drift_cfg_t;

  // Decide at most one correction for this drain cycle from the current readable
  // frame count. avail==0 always yields NONE (true underrun → silence path owns it).
  audio_drift_action_t audio_drift_decide(size_t avail, const audio_drift_cfg_t *cfg);
  ```

- [ ] **Step 2: Failing tests** (`#include "audio_drift.c"`):
  ```c
  #include <unity.h>
  #include "audio_drift.c"
  void setUp(void){} void tearDown(void){}
  static const audio_drift_cfg_t CFG = { .high = 80000, .low = 8000 };

  void test_above_high_drops(void){
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DROP, audio_drift_decide(80000, &CFG));
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DROP, audio_drift_decide(90000, &CFG));
  }
  void test_below_low_dups(void){
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DUP, audio_drift_decide(8000, &CFG));
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_DUP, audio_drift_decide(1, &CFG));
  }
  void test_empty_is_none(void){   // underrun path, not drift
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(0, &CFG));
  }
  void test_midband_none(void){
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(40000, &CFG));
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(8001, &CFG));   // just above low
      TEST_ASSERT_EQUAL_INT(AUDIO_DRIFT_NONE, audio_drift_decide(79999, &CFG));  // just below high
  }
  int main(void){ UNITY_BEGIN();
      RUN_TEST(test_above_high_drops); RUN_TEST(test_below_low_dups);
      RUN_TEST(test_empty_is_none); RUN_TEST(test_midband_none);
      return UNITY_END(); }
  ```

- [ ] **Step 3: Implement** `audio_drift.c`:
  ```c
  #include "audio_drift.h"
  audio_drift_action_t audio_drift_decide(size_t avail, const audio_drift_cfg_t *cfg){
      if (avail == 0) return AUDIO_DRIFT_NONE;             // underrun → silence path
      if (avail >= cfg->high) return AUDIO_DRIFT_DROP;
      if (avail <= cfg->low)  return AUDIO_DRIFT_DUP;
      return AUDIO_DRIFT_NONE;
  }
  ```

- [ ] **Step 4:** `pio test -e native` green (prior 47 + reorder + resend + timing + drift).
- [ ] **Step 5: Commit** `feat(audio): pure free-run drift drop/dup watermark decision with host tests`.

## Task 5: Ring drop/dup primitives + drift in the drain (audio glue)

**Files:** edit `components/audio/src/audio_ringbuf.{h,c}`, `components/audio/src/audio_playback.c`,
`components/audio/CMakeLists.txt`. Extends the SPSC ring with the two consumer-side primitives the drift
step needs, then applies `audio_drift_decide` once per drain cycle. Pure-ring tests
(`test/test_ringbuf`) must stay green; add ring-drop/last-frame cases there.

- [ ] **Step 1 (ring primitives):** add to `audio_ringbuf.h/.c` (consumer-side only, so the SPSC contract
  is preserved — only the drain task calls these):
  ```c
  // Discard up to n readable frames without emitting them (drift DROP). Returns
  // frames dropped (< n on underrun). Consumer-side: advances tail only.
  size_t audio_ringbuf_drop(audio_ringbuf_t *rb, size_t n);
  // Copy the most recently readable frame into out[2] (drift DUP source). Returns
  // false if empty. Does NOT advance tail.
  bool   audio_ringbuf_last_frame(const audio_ringbuf_t *rb, int16_t out[2]);
  ```
  Implement `drop` by advancing `tail` (mod capacity) up to `available`; `last_frame` reads the frame at
  `(head - 1)` mod capacity when non-empty. Add a couple of cases to `test/test_ringbuf/test_ringbuf.c`
  (drop past-available clamps; last_frame on empty returns false; last_frame returns the newest write).
- [ ] **Step 2 (drain applies drift):** in `audio_playback.c`, compute a `audio_drift_cfg_t` from the ring
  capacity once (target ≈ 50 % depth, `high = cap*3/4`, `low = cap*1/4` in frames — generous band so drift
  correction is rare and never fights normal fill), and once per `for(;;)` cycle **before** the read:
  ```c
  switch (audio_drift_decide(audio_ringbuf_available(ring), &drift_cfg)) {
      case AUDIO_DRIFT_DROP: audio_ringbuf_drop(ring, 1); break;      // shed ~23µs
      case AUDIO_DRIFT_DUP: {                                          // pad ~23µs
          int16_t f[2];
          if (audio_ringbuf_last_frame(ring, f)) audio_i2s_write(f, 1);
          break;
      }
      default: break;
  }
  ```
  Keep the existing read/silence logic unchanged after the switch. Note in a comment: the drop/dup is a
  single frame per cycle; equilibrium parks `avail` just inside the band, so corrections are inaudible and
  infrequent. DUP writes the repeated frame straight to I2S (it does not re-enter the ring).
- [ ] **Step 3: CMakeLists** — add `src/audio_drift.c` to the `audio` component SRCS.
- [ ] **Step 4:** `pio run -e esp32-s3-n16r8` green; `pio test -e native` all green.
- [ ] **Step 5: Commit** `feat(audio): ring drop/dup primitives + drift watermark in playback drain`.

## Task 6: `raop_rtp` rewrite — seq-ordered decode + control/timing select loop (glue)

**Files:** edit `components/raop/src/raop_rtp.{h,c}`. This is the integration point; no host tests
(correctness rests on the four pure units + build). Rewrite the single-socket arrival-order loop into a
`select()` loop over `audio_fd`, `control_fd`, `timing_fd` that keeps **one** task feeding
`audio_play_pcm` (single-producer contract intact).

- [ ] **Step 1: State** — allocate the reorder buffer in PSRAM at start, free at stop:
  ```c
  #define REORDER_WINDOW 256      // ~2 s @ 352 samples/pkt; retransmit hold window
  #define REORDER_HOLD   192      // conceal after holding ~1.5 s (retransmit RTT ≪ this)
  static rtp_reorder_t         s_reorder;
  static rtp_reorder_slot_t   *s_slots;         // heap_caps_malloc(SPIRAM), WINDOW entries
  static struct sockaddr_in    s_peer;          // learned from the first audio recvfrom
  static bool                  s_peer_known = false;
  static int                   s_control_fd = -1, s_timing_fd = -1;
  static int                   s_client_control_port = 0, s_client_timing_port = 0;
  ```
  `raop_rtp_start(raop_session_t *s)` now also stashes `s->control_fd`, `s->timing_fd`,
  `s->client_control_port`, `s->client_timing_port`, allocates
  `s_slots = heap_caps_malloc(REORDER_WINDOW*sizeof(rtp_reorder_slot_t), MALLOC_CAP_SPIRAM)`
  (~395 KB — abort/fail-clean if NULL), and `rtp_reorder_init(&s_reorder, s_slots, REORDER_WINDOW,
  REORDER_HOLD)`. The header comment already documents the whole-session dependency; keep the signature
  `int raop_rtp_start(raop_session_t *s)` / `bool raop_rtp_stop(void)`.
- [ ] **Step 2: Learn the peer + anchor** — on the **first** audio datagram, capture the source address
  from `recvfrom(...&src...)` into `s_peer` (set `s_peer.sin_port` per-destination below) and
  `rtp_reorder_anchor(&s_reorder, first_seq)` using that packet's seq (the RECORD `RTP-Info: seq=` is the
  authoritative anchor; if `raop.c` already parses it into the session, prefer that — otherwise the first
  packet's seq is equivalent for a free-run receiver). Resend requests go to `(s_peer.sin_addr,
  htons(s_client_control_port))`; timing requests to `(s_peer.sin_addr, htons(s_client_timing_port))`.
- [ ] **Step 3: select() loop** (replaces the `recvfrom`-only `rtp_task`):
  ```
  while (s_run):
    FD_SET audio_fd, control_fd, timing_fd; timeout = 250 ms
    select(maxfd+1, &rd, ...)
    if audio_fd ready:
        n = recvfrom(audio_fd, rx, cap, &src)
        learn s_peer + anchor on first packet
        rtp_parse(rx,n,&h); if audio: rtp_reorder_insert(&s_reorder, h.seq, h.payload, h.payload_len)
        drain_decode()                       # pop-decode as far as in-order data allows
        maybe_request_resend()               # rtp_reorder_gap → rtp_resend_build → sendto(control_fd,peer:ctrl)
    if control_fd ready:
        n = recvfrom(control_fd, rx, cap, NULL)
        type = rx[1]&0x7f
        if type==RESEND_RESP_TYPE(0x56): rtp_resend_unwrap → rtp_parse(inner) →
                                         rtp_reorder_insert(seq,payload) ; drain_decode()
        else: log+drop (0x54 sync ignored — free-run)
    if timing_fd ready:
        n = recvfrom(timing_fd, rx, cap, &src)
        rtp_timing_parse(rx,n,&type,&st)
        if type==TIMING_REQ_TYPE(0x52):      # defensive belt only
            rtp_timing_build_response(...) ; sendto(timing_fd, resp, 32, &src)
        # TIMING_RESP_TYPE(0x53): consumed, NOT used (free-run) — just drop
    # periodic timing request (receiver-initiated; keeps the sender happy):
    if s_peer_known and now - last_timing >= 3 s:
        rtp_timing_build_request(req, now_ntp64()) ; sendto(timing_fd, req, 32, peer:timing) ; last_timing=now
  ```
  `drain_decode()` loops:
  ```
  for (;;):
     r = rtp_reorder_pop(&s_reorder, s_plain_enc, cap, &len, &seq)
     if r==RTP_POP_OK:      decrypt(s_plain_enc,len) → alac_decode → feed_pcm()   # existing Phase-3 body
     if r==RTP_POP_CONCEAL: feed_silence(frameLength)   # push one frame of zeros to hold timing
     if r==RTP_POP_WAIT || RTP_POP_EMPTY: break         # stop; wait for more / retransmit
  ```
  Reuse the Phase-3 `aes_cbc_decrypt` + `aes_frame_split` + `alac_decode_frame` + backpressured
  `audio_play_pcm` exactly — only the **source** of the encrypted payload changes (reorder pop instead of
  the raw datagram). `feed_silence` pushes `frameLength` zero frames through the same backpressured feed so
  a concealed gap preserves stream duration instead of time-compressing.
- [ ] **Step 4: Resend throttle** — track the last-requested `(first,count)` and a small min-interval
  (e.g. don't re-request the same `first` more than every ~30 ms) so a persistent gap doesn't flood the
  control channel while a retransmit is in flight. `rtp_reorder_gap` gives the current front-gap each call;
  guard with a timestamp + last-first memo.
- [ ] **Step 5: Interop comments** to embed: reorder-before-decode rationale; per-packet IV reset
  unchanged; **receiver-initiated** timing (cite the shairport verification + that the task premise was
  inverted); sync 0xd4 ignored (free-run); conceal-with-silence on unrecoverable gaps; peer learned from
  first audio packet (shairport does the same). Stop path: `s_run=false`, join task, free `s_slots`,
  `teardown_crypto_decoder()`, reset peer/fds.
- [ ] **Step 6:** `rm -rf framework/.pio/build/esp32-s3-n16r8` (REQUIRES unchanged, but the reorder PSRAM
  alloc + new sockets are new glue — a clean build is cheap insurance); `pio run -e esp32-s3-n16r8` green.
- [ ] **Step 7: Commit** `feat(raop): seq-ordered decode + retransmit + timing over a 3-socket select loop`.

## Task 7: Wire CMake + session plumbing + README

**Files:** edit `components/raop/CMakeLists.txt`, `components/raop/README.md`, spot-check `raop.c`.

- [ ] **Step 1: CMakeLists** — add `src/rtp_reorder.c src/rtp_resend.c src/rtp_timing.c` to the `raop`
  SRCS. (`PRIV_REQUIRES` already has `alac audio lwip mbedtls` — no REQUIRES change, so no forced wipe;
  Task 6 already wiped.)
- [ ] **Step 2: raop.c** — confirm RECORD still calls `audio_diag_tone_stop()` then `raop_rtp_start(&s_session)`
  and TEARDOWN/reclaim still call `raop_rtp_stop()` + `audio_diag_tone_start()` (Phase 3 handoff unchanged).
  If `raop.c` parses the RECORD `RTP-Info: seq=`/`rtptime=`, pass the seq into the session so
  `raop_rtp_start` can anchor precisely; otherwise document that the first-packet anchor is used.
  FLUSH still must NOT tear the decoder down — but it SHOULD reset the reorder buffer's cursor on the next
  packet; simplest: leave the free-run anchor-on-first-packet logic to re-anchor after a FLUSH gap (note
  this as acceptable Phase-4 behavior; a precise FLUSH-reanchor is a follow-up).
- [ ] **Step 3: README** — document the Phase-4 receive pipeline (reorder-before-decode, resend on gap,
  receiver-initiated timing, drift drop/dup), the exact wire formats (link this plan), and the corrected
  timing direction.
- [ ] **Step 4:** `pio run -e esp32-s3-n16r8` green; `pio test -e native` — all green
  (47 prior + `test_rtp_reorder` + `test_rtp_resend` + `test_rtp_timing` + `test_audio_drift`).
- [ ] **Step 5: Commit** `docs(raop): Phase 4 README — retransmit, timing, drift; wire CMake`.

---

## Success criteria

- `~/.platformio/penv/bin/pio run -e esp32-s3-n16r8` builds green (fresh `.pio/build` after Task 6).
- `~/.platformio/penv/bin/pio test -e native` passes: the prior **47** tests **plus** `test_rtp_reorder`,
  `test_rtp_resend`, `test_rtp_timing`, `test_audio_drift` — all pure, zero ESP-IDF/PSA/lwip includes.
- Static review confirms wire behavior matches the cited framing: resend request `0x80 0xD5 htons(1)
  htons(first) htons(count)`; resend response `0xD6` wrapper stripped 4 bytes then parsed as a normal audio
  packet; timing **receiver-initiated** (`0xD2` request / `0xD3` response, seqno 7, NTP64 at 8/16/24);
  reorder dispatch/gap/conceal handles 16-bit seq wraparound; drift DROP≥high / DUP≤low / NONE at 0.
- Phases 0–3 intact: RTSP handshake, RSA/AES key path, first-audio decrypt+decode, single-producer
  diag-tone handoff on RECORD/TEARDOWN — all unchanged in behavior.
- Reorder buffer holds during loss and slots retransmitted/late packets into seq order before decode; an
  unfillable gap conceals (silence) and moves on — never stalls forever.

## Risks / interop honesty (no real sender here)

- **No hardware / no live sender.** Verification is build + host tests only. The retransmit and timing wire
  formats are matched **byte-for-byte** to shairport-sync `rtp.c` (fetched & quoted 2026-07-17); nothing is
  stubbed.
- **Timing direction was inverted in the task brief.** Verified against shairport `rtp_timing_sender`
  (sends `0xd2`) / `rtp_timing_receiver` (receives `0xd3`): the **receiver initiates**. We implement the
  receiver-initiated sender (the proven path) + a documented defensive `0xd2`→`0xd3` responder. If a
  specific sender still tears down, the fallback is tuning the request cadence — not adding a responder the
  sender never queries.
- **Free-run, no clock discipline (spec §5c).** Timing responses are parsed but discarded; drift is handled
  only by the §6f drop/dup watermark. Long-session ppm mismatch is corrected in ~23 µs steps.
- **Reorder window vs latency.** `REORDER_WINDOW=256` (~2 s) bounds how long a gap is held before conceal;
  in steady state the drain keeps it near-empty so it adds no latency, only a hold budget for retransmit.
- **FLUSH re-anchor** is left to the free-run first-packet re-anchor after the gap; a precise
  seq/rtptime re-anchor from the FLUSH `RTP-Info` is a noted follow-up, not required for "clean audio under
  loss."
```
