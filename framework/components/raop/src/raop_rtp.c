// Phase 3 RTP audio receive path — GLUE (sockets + PSA crypto + FreeRTOS + the
// vendored ALAC decoder). The host-testable logic it relies on lives in the pure
// units rtp_parser.c, aes_frame.c and alac_config.c; this file only wires them to
// the network, the crypto and the audio ring.
//
// Wire behavior follows shairport-sync rtp.c / player.c EXACTLY (see
// research-phase3-5.md agent 3, critic corrections G1/G2 applied):
//   * dispatch on packet[1] & 0x7f; Phase 3 handles only the audio type (0x60).
//   * decrypt ONLY the largest multiple of 16 payload bytes; the trailing
//     (payload_len % 16) bytes are plaintext and copied verbatim (aes_frame).
//   * the AES-CBC IV is RESET to the constant session IV for EVERY packet — CBC
//     chains within a packet, NEVER across packets.
//   * the decrypted+tail bytes are one raw ALAC frame -> alac_decode_frame ->
//     interleaved LE int16 stereo PCM -> audio_play_pcm (arrival order; free-run,
//     no jitter buffer / timing / sync / retransmit in Phase 3).
//
// Untrusted LAN input (spec §9): the datagram size is capped, the RTP length is
// bounded before use, and every downstream length is checked against a buffer.

#include "raop_rtp.h"
#include "rtp_parser.h"
#include "aes_frame.h"
#include "alac.h"
#include "alac_config.h"
#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "raop_rtp";

// RAOP audio datagrams are ~1.4 KB; 2048 caps a hostile oversize packet (§9).
#define RTP_RX_CAP   2048
// Worst-case decoded PCM: frameLength * numChannels * int16. Bounds the decoder
// output for up to 4096-sample stereo (16 KB); the RAOP default 352 uses 1408 B.
#define PCM_MAX_INT16 (4096 * 2)

// ---- module state (single session, spec §8) ------------------------------
static alac_file          *s_alac        = NULL;
static mbedtls_svc_key_id_t s_aes_key;
static bool                 s_aes_key_ok  = false;
static uint8_t              s_iv[16];             // constant session IV (per-packet reset)
static uint8_t              s_num_channels = 2;

static int                  s_audio_fd    = -1;
static TaskHandle_t         s_task        = NULL;
static volatile bool        s_run         = false;
static volatile bool        s_done        = false;

// static (off the task stack); single-instance task -> not reentrant.
static uint8_t              s_rx[RTP_RX_CAP];
static uint8_t              s_plain[RTP_RX_CAP];
static int16_t              s_pcm[PCM_MAX_INT16];

// AES-128-CBC decrypt of `len` (multiple-of-16) ciphertext bytes with a fresh IV
// per call. Returns 0 and *out_len on success, -1 on any PSA error.
static int aes_cbc_decrypt(const uint8_t *in, size_t len, const uint8_t iv[16],
                           uint8_t *out, size_t out_cap, size_t *out_len) {
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t o1 = 0, o2 = 0;
    psa_status_t s;
    if ((s = psa_cipher_decrypt_setup(&op, s_aes_key, PSA_ALG_CBC_NO_PADDING)) != PSA_SUCCESS) goto fail;
    if ((s = psa_cipher_set_iv(&op, iv, 16)) != PSA_SUCCESS) goto fail;         // reset IV every packet
    if ((s = psa_cipher_update(&op, in, len, out, out_cap, &o1)) != PSA_SUCCESS) goto fail;
    if ((s = psa_cipher_finish(&op, out + o1, out_cap - o1, &o2)) != PSA_SUCCESS) goto fail;
    *out_len = o1 + o2;
    return 0;
fail:
    psa_cipher_abort(&op);
    ESP_LOGW(TAG, "AES-CBC decrypt failed: %d", (int)s);
    return -1;
}

static void process_packet(const uint8_t *buf, size_t n) {
    rtp_header_t h;
    if (rtp_parse(buf, n, &h) != 0) return;
    if (h.payload_type != RTP_PT_AUDIO) return;   // sync/timing/resend are Phase 4+
    if (h.payload_len == 0 || h.payload_len > sizeof(s_plain)) return;

    // Split payload into the AES-CBC ciphertext prefix and plaintext tail.
    aes_frame_split_t sp;
    aes_frame_split(h.payload_len, &sp);

    size_t decoded = 0;
    if (sp.cipher_len > 0) {
        if (aes_cbc_decrypt(h.payload, sp.cipher_len, s_iv,
                            s_plain, sizeof(s_plain), &decoded) != 0) {
            return;
        }
    }
    // Trailing plaintext bytes are copied verbatim after the decrypted prefix.
    if (sp.plain_len > 0) {
        memcpy(s_plain + sp.plain_off, h.payload + sp.plain_off, sp.plain_len);
    }

    // One raw ALAC frame -> interleaved LE int16 PCM. *outsize MUST be preset to
    // the buffer capacity (bytes); on return it is the count of PCM bytes written.
    int outsize = (int)sizeof(s_pcm);
    alac_decode_frame(s_alac, s_plain, s_pcm, &outsize);
    if (outsize <= 0) return;

    size_t frames = (size_t)outsize / ((size_t)s_num_channels * sizeof(int16_t));
    if (frames == 0) return;

    // Feed with backpressure (a full ring yields+retries, like the diag tone) so
    // samples are never dropped mid-stream.
    size_t off = 0;
    while (off < frames && s_run) {
        off += audio_play_pcm(s_pcm + off * s_num_channels, frames - off);
        if (off < frames) vTaskDelay(1);
    }
}

static void rtp_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "RTP audio receive task up (core %d)", audio_producer_core());
    while (s_run) {
        int n = recvfrom(s_audio_fd, s_rx, sizeof(s_rx), 0, NULL, NULL);
        if (n <= 0) continue;   // timeout (SO_RCVTIMEO) or transient error -> re-check s_run
        process_packet(s_rx, (size_t)n);
    }
    audio_producer_release();
    s_task = NULL;
    s_done = true;              // publish AFTER releasing: stop() waits on this
    vTaskDelete(NULL);
}

// Build the decoder from the SDP fmtp and set the setinfo_* fields directly
// (matches shairport-sync init_alac_decoder). Returns 0 on success.
static int build_decoder(const char *fmtp) {
    alac_cfg_t c;
    if (alac_cfg_from_fmtp(fmtp, &c) != 0) {
        ESP_LOGE(TAG, "fmtp parse failed: \"%s\"", fmtp ? fmtp : "(null)");
        return -1;
    }
    // The audio ring is 16-bit stereo; bound the decoder to that contract and to
    // the PCM buffer size.
    if (c.bit_depth != 16 || c.num_channels != 2) {
        ESP_LOGE(TAG, "unsupported ALAC: bit_depth=%u channels=%u (want 16/2)",
                 c.bit_depth, c.num_channels);
        return -1;
    }
    if (c.frame_length == 0 ||
        (size_t)c.frame_length * c.num_channels > PCM_MAX_INT16) {
        ESP_LOGE(TAG, "ALAC frame_length %u out of range", (unsigned)c.frame_length);
        return -1;
    }

    s_alac = alac_create(c.bit_depth, c.num_channels);
    if (s_alac == NULL) {
        ESP_LOGE(TAG, "alac_create failed");
        return -1;
    }
    s_alac->setinfo_max_samples_per_frame = c.frame_length;
    s_alac->setinfo_7a                    = c.compat_version;
    s_alac->setinfo_sample_size           = c.bit_depth;
    s_alac->setinfo_rice_historymult      = c.pb;
    s_alac->setinfo_rice_initialhistory   = c.mb;
    s_alac->setinfo_rice_kmodifier        = c.kb;
    s_alac->setinfo_7f                    = c.num_channels;
    s_alac->setinfo_80                    = c.max_run;
    s_alac->setinfo_82                    = c.max_frame_bytes;
    s_alac->setinfo_86                    = c.avg_bitrate;
    s_alac->setinfo_8a_rate               = c.sample_rate;
    alac_allocate_buffers(s_alac);

    s_num_channels = c.num_channels;
    ESP_LOGI(TAG, "ALAC decoder: frameLength=%u %u-bit %uch @%uHz",
             (unsigned)c.frame_length, c.bit_depth, c.num_channels, (unsigned)c.sample_rate);
    return 0;
}

static int import_aes_key(const uint8_t key[16]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_status_t s = psa_import_key(&attr, key, 16, &s_aes_key);
    psa_reset_key_attributes(&attr);
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key(AES): %d", (int)s);
        return -1;
    }
    s_aes_key_ok = true;
    return 0;
}

static void teardown_crypto_decoder(void) {
    if (s_aes_key_ok) {
        psa_destroy_key(s_aes_key);
        s_aes_key_ok = false;
    }
    if (s_alac) {
        alac_free(s_alac);
        s_alac = NULL;
    }
}

int raop_rtp_start(raop_session_t *s) {
    if (s_task != NULL) {
        ESP_LOGW(TAG, "raop_rtp_start: already running");
        return -1;
    }
    if (!s->have_key || s->audio_fd < 0) {
        ESP_LOGE(TAG, "raop_rtp_start: missing key or unbound audio socket");
        return -1;
    }
    if (build_decoder(s->fmtp) != 0) return -1;
    if (import_aes_key(s->aeskey) != 0) { teardown_crypto_decoder(); return -1; }

    memcpy(s_iv, s->aesiv, sizeof(s_iv));
    s_audio_fd = s->audio_fd;

    // Bound recvfrom so the task periodically re-checks s_run and can be stopped.
    struct timeval rcv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    setsockopt(s_audio_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));

    // Claim the single-producer slot BEFORE the task starts (the caller has
    // already stopped the diag tone, so the slot is free). Prio above the diag
    // tone; pinned to the audio core so the SPSC ring stays synchronized.
    if (!audio_producer_acquire("raop_rtp")) {
        teardown_crypto_decoder();
        return -1;
    }
    s_run  = true;
    s_done = false;
    if (xTaskCreatePinnedToCore(rtp_task, "raop_rtp", 8192, NULL, 6,
                                &s_task, audio_producer_core()) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(raop_rtp) failed");
        audio_producer_release();
        s_run = false;
        s_task = NULL;
        teardown_crypto_decoder();
        return -1;
    }
    ESP_LOGI(TAG, "RTP receive started (audio_fd=%d)", s_audio_fd);
    return 0;
}

bool raop_rtp_stop(void) {
    if (s_task == NULL) {
        // Task not running; still clean up any decoder/key left from a failed start.
        teardown_crypto_decoder();
        return false;
    }
    s_run = false;
    while (!s_done) vTaskDelay(1);   // block until the task released the producer
    teardown_crypto_decoder();
    s_audio_fd = -1;
    ESP_LOGI(TAG, "RTP receive stopped");
    return true;
}
