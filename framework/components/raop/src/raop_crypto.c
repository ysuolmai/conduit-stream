#include "raop_crypto.h"

#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "raop_crypto";
static mbedtls_svc_key_id_t s_sign_key;  // usage SIGN_HASH, alg PKCS1V15_SIGN_RAW
static mbedtls_svc_key_id_t s_dec_key;   // usage DECRYPT|ENCRYPT, alg RSA_OAEP(SHA_1)

// Import the parsed PEM into a PSA key with the given usage + algorithm policy.
// mbedtls_pk_get_psa_attributes seeds the RSA type/bit-size (we hand it SIGN_HASH
// only for that), then we pin the exact RAOP usage+algorithm policy.
static esp_err_t import_key(mbedtls_pk_context *pk, psa_key_usage_t usage,
                            psa_algorithm_t alg, mbedtls_svc_key_id_t *out) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    if (mbedtls_pk_get_psa_attributes(pk, PSA_KEY_USAGE_SIGN_HASH, &attr) != 0) {
        psa_reset_key_attributes(&attr);
        return ESP_FAIL;
    }
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);
    int rc = mbedtls_pk_import_into_psa(pk, &attr, out);
    psa_reset_key_attributes(&attr);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t raop_crypto_init(void) {
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init");
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    // mbedTLS 4.0 signature: (ctx, key, keylen, pwd, pwdlen). keylen INCLUDES the NUL for PEM.
    int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)raop_key_pem, raop_key_pem_len,
                                  NULL, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_parse_key: -0x%04x (embedded key corrupt?)", (unsigned)(-rc));
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    esp_err_t e1 = import_key(&pk, PSA_KEY_USAGE_SIGN_HASH,
                              PSA_ALG_RSA_PKCS1V15_SIGN_RAW, &s_sign_key);
    // Decrypt key also gets ENCRYPT so the self-test can round-trip with the public part.
    esp_err_t e2 = import_key(&pk, PSA_KEY_USAGE_DECRYPT | PSA_KEY_USAGE_ENCRYPT,
                              PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), &s_dec_key);
    mbedtls_pk_free(&pk);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "import_into_psa failed");
        return ESP_FAIL;
    }

    // --- Self-test (fails loud on a corrupted key; no real sender needed) ---
    // (a) sign a fixed 32-byte buffer -> expect a 256-byte signature.
    uint8_t buf32[32] = {0}, sig[256];
    size_t siglen = 0;
    if (raop_crypto_sign_challenge(buf32, sig, sizeof(sig), &siglen) != 0 || siglen != 256) {
        ESP_LOGE(TAG, "self-test sign failed (siglen=%u)", (unsigned)siglen);
        return ESP_FAIL;
    }
    // (b) OAEP encrypt-then-decrypt a known 16-byte AES key -> expect it back.
    const uint8_t sample[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t ct[256], pt[16];
    size_t ctlen = 0;
    if (psa_asymmetric_encrypt(s_dec_key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1),
                               sample, 16, NULL, 0, ct, sizeof(ct), &ctlen) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "self-test encrypt failed");
        return ESP_FAIL;
    }
    if (raop_crypto_decrypt_aeskey(ct, ctlen, pt) != 0 || memcmp(pt, sample, 16) != 0) {
        ESP_LOGE(TAG, "self-test decrypt round-trip mismatch");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RAOP key loaded; sign(256)+OAEP round-trip OK");
    return ESP_OK;
}

int raop_crypto_sign_challenge(const uint8_t buf32[32],
                               uint8_t *sig, size_t sig_cap, size_t *sig_len) {
    psa_status_t s = psa_sign_hash(s_sign_key, PSA_ALG_RSA_PKCS1V15_SIGN_RAW,
                                   buf32, 32, sig, sig_cap, sig_len);
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_sign_hash: %d", (int)s);
        return -1;
    }
    return 0;
}

int raop_crypto_decrypt_aeskey(const uint8_t *in, size_t in_len, uint8_t out16[16]) {
    uint8_t tmp[32];
    size_t olen = 0;
    psa_status_t s = psa_asymmetric_decrypt(s_dec_key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1),
                                            in, in_len, NULL, 0, tmp, sizeof(tmp), &olen);
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_asymmetric_decrypt: %d", (int)s);
        return -1;
    }
    if (olen != 16) {
        ESP_LOGE(TAG, "AES key len %u != 16", (unsigned)olen);
        return -1;
    }
    memcpy(out16, tmp, 16);
    return 0;
}
