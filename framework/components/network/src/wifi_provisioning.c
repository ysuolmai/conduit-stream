#include "wifi.h"
#include "ota_update.h"
#include "system_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DNS_PORT 53
#define MAX_SCAN_RESULTS 16
#define FORM_BODY_MAX 384
#define OTA_BUFFER_SIZE 4096

#ifdef CONFIG_CONDUIT_MONO_OUTPUT
#define AUDIO_TARGET "MAX98357A"
#define OTA_FILENAME "minispeaker-esp32s3-n4r2-max98357a-ota.bin"
#else
#define AUDIO_TARGET "PCM5102A"
#define OTA_FILENAME "minispeaker-esp32s3-n4r2-pcm5102a-ota.bin"
#endif

static const char *TAG = "wifi_setup";
static wifi_ap_record_t s_scan_results[MAX_SCAN_RESULTS];
static uint16_t s_scan_count;

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>MiniSpeaker setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#f4f6f8;color:#182026;"
    "font:16px system-ui,-apple-system,sans-serif}main{max-width:480px;margin:0 auto;"
    "padding:32px 20px}h1{font-size:26px;margin:0 0 8px}p{color:#53606b;"
    "line-height:1.45;margin:0 0 24px}label{display:block;font-weight:600;"
    "margin:18px 0 7px}select,input{width:100%;height:46px;border:1px solid #aeb8c2;"
    "border-radius:6px;background:#fff;padding:0 12px;font:inherit}button{width:100%;"
    "height:48px;margin-top:24px;border:0;border-radius:6px;background:#1769aa;"
    "color:#fff;font:600 16px system-ui;cursor:pointer}button:disabled{opacity:.55;cursor:default}"
    "small{display:block;color:#697782;margin-top:7px}.name{font-weight:700;color:#182026}"
    "section{border-top:1px solid #d8dee4;margin-top:32px;padding-top:26px}h2{font-size:20px;"
    "margin:0 0 8px}input[type=file]{height:auto;padding:11px}progress{width:100%;height:12px;"
    "margin-top:14px}.result{min-height:24px;margin-top:10px;color:#33414d}</style></head><body><main>";

static const char PAGE_WIFI_FORM_END[] =
    "</select><label for=manual>Other network</label>"
    "<input id=manual name=manual maxlength=32 autocomplete=off>"
    "<small>Use this only when the network is hidden or not listed.</small>"
    "<label for=pass>Wi-Fi password</label>"
    "<input id=pass name=pass type=password maxlength=63 autocomplete=current-password>"
    "<button type=submit>Save and restart</button></form>"
    "<section><h2>Firmware update</h2>";

static const char PAGE_OTA_CONTROLS[] =
    "<input id=firmware type=file accept=\".bin,application/octet-stream\">"
    "<button id=upload type=button onclick=uploadFirmware()>Upload and restart</button>"
    "<progress id=progress value=0 max=100 hidden></progress><div id=result class=result></div>"
    "<script>function uploadFirmware(){const f=document.getElementById('firmware').files[0],"
    "b=document.getElementById('upload'),p=document.getElementById('progress'),"
    "r=document.getElementById('result');if(!f){r.textContent='Choose an OTA firmware file.';return;}"
    "b.disabled=true;p.hidden=false;p.value=0;r.textContent='Uploading...';const x=new XMLHttpRequest();"
    "x.open('POST','/update');x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.upload.onprogress=e=>{if(e.lengthComputable)p.value=Math.round(e.loaded*100/e.total)};"
    "x.onload=()=>{r.textContent=x.responseText;if(x.status!==200)b.disabled=false};"
    "x.onerror=()=>{r.textContent='Upload failed.';b.disabled=false};x.send(f)}</script>"
    "</section></main></body></html>";

static esp_err_t send_text(httpd_req_t *req, const char *text) {
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static void html_escape(const char *src, char *dst, size_t dst_size) {
    size_t used = 0;
    while (*src && used + 1 < dst_size) {
        const char *replacement = NULL;
        switch (*src) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement) {
            size_t len = strlen(replacement);
            if (used + len >= dst_size) break;
            memcpy(dst + used, replacement, len);
            used += len;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    dst[used] = '\0';
}

static bool scan_result_is_duplicate(uint16_t index) {
    for (uint16_t i = 0; i < index; i++) {
        if (strcmp((const char *)s_scan_results[i].ssid,
                   (const char *)s_scan_results[index].ssid) == 0) return true;
    }
    return false;
}

static esp_err_t portal_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (send_text(req, PAGE_HEAD) != ESP_OK) return ESP_FAIL;

    char intro[384];
    snprintf(intro, sizeof(intro),
             "<h1>%s</h1><p>Connect this speaker to a 2.4 GHz Wi-Fi network. "
             "Its AirPlay name will remain <span class=name>%s</span>.</p>"
             "<form method=post action=/save><label for=ssid>Wi-Fi network</label>"
             "<select id=ssid name=ssid><option value=\"\">Select a network</option>",
             system_config_get_name(), system_config_get_name());
    if (send_text(req, intro) != ESP_OK) return ESP_FAIL;

    for (uint16_t i = 0; i < s_scan_count; i++) {
        if (s_scan_results[i].ssid[0] == '\0' || scan_result_is_duplicate(i)) continue;
        char escaped[180];
        char option[420];
        html_escape((const char *)s_scan_results[i].ssid, escaped, sizeof(escaped));
        snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>",
                 escaped, escaped, (int)s_scan_results[i].rssi);
        if (send_text(req, option) != ESP_OK) return ESP_FAIL;
    }

    if (send_text(req, PAGE_WIFI_FORM_END) != ESP_OK) return ESP_FAIL;
    char ota_details[384];
    snprintf(ota_details, sizeof(ota_details),
             "<p>This unit uses <span class=name>%s</span>. Select the matching "
             "<span class=name>%s</span> release file. Wi-Fi settings are preserved.</p>",
             AUDIO_TARGET, OTA_FILENAME);
    if (send_text(req, ota_details) != ESP_OK ||
        send_text(req, PAGE_OTA_CONTROLS) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool hex_value(char c, unsigned char *value) {
    if (c >= '0' && c <= '9') *value = (unsigned char)(c - '0');
    else if (c >= 'a' && c <= 'f') *value = (unsigned char)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') *value = (unsigned char)(c - 'A' + 10);
    else return false;
    return true;
}

static bool url_decode(const char *src, char *dst, size_t dst_size) {
    size_t used = 0;
    while (*src) {
        if (used + 1 >= dst_size) return false;
        if (*src == '+') {
            dst[used++] = ' ';
            src++;
        } else if (*src == '%') {
            unsigned char hi, lo;
            if (!src[1] || !src[2] || !hex_value(src[1], &hi) || !hex_value(src[2], &lo)) {
                return false;
            }
            dst[used++] = (char)((hi << 4) | lo);
            src += 3;
        } else {
            dst[used++] = *src++;
        }
    }
    dst[used] = '\0';
    return true;
}

static bool form_value(const char *body, const char *key, char *out, size_t out_size) {
    char encoded[192];
    if (httpd_query_key_value(body, key, encoded, sizeof(encoded)) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return url_decode(encoded, out, out_size);
}

static esp_err_t send_error(httpd_req_t *req, const char *message) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t send_update_error(httpd_req_t *req, const char *status,
                                   const char *message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > FORM_BODY_MAX) {
        return send_error(req, "Invalid form data.");
    }

    char body[FORM_BODY_MAX + 1];
    size_t received = 0;
    while (received < req->content_len) {
        int result = httpd_req_recv(req, body + received, req->content_len - received);
        if (result <= 0) return ESP_FAIL;
        received += result;
    }
    body[received] = '\0';

    char selected[33], manual[33], password[64];
    if (!form_value(body, "ssid", selected, sizeof(selected)) ||
        !form_value(body, "manual", manual, sizeof(manual)) ||
        !form_value(body, "pass", password, sizeof(password))) {
        return send_error(req, "Invalid encoded form data.");
    }
    const char *ssid = manual[0] ? manual : selected;
    size_t pass_len = strlen(password);
    if (!ssid[0]) return send_error(req, "Choose or enter a Wi-Fi network.");
    if (pass_len > 0 && pass_len < 8) {
        return send_error(req, "Wi-Fi passwords must be empty or at least 8 characters.");
    }

    esp_err_t err = system_config_save_wifi(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving credentials failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Could not save settings.");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Saved</title><style>body{font:18px system-ui;margin:40px;color:#182026}</style>"
        "<h1>Settings saved</h1><p>MiniSpeaker is restarting and will join your Wi-Fi.</p>");
    xTaskCreate(restart_task, "setup_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static int receive_upload_data(httpd_req_t *req, uint8_t *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        int result = httpd_req_recv(req, (char *)buffer + received, length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (result <= 0) return result;
        received += (size_t)result;
    }
    return (int)received;
}

static esp_err_t update_post_handler(httpd_req_t *req) {
    size_t image_size = req->content_len;
    size_t max_size = ota_update_max_size();
    if (image_size == 0 || max_size == 0 || image_size > max_size) {
        return send_update_error(req, "400 Bad Request",
                                 "Firmware is empty or too large for the OTA slot.");
    }

    uint8_t *buffer = malloc(OTA_BUFFER_SIZE);
    if (!buffer) {
        return send_update_error(req, "500 Internal Server Error",
                                 "Not enough memory to start the update.");
    }

    size_t first_length = image_size < OTA_BUFFER_SIZE ? image_size : OTA_BUFFER_SIZE;
    if (receive_upload_data(req, buffer, first_length) <= 0) {
        free(buffer);
        return ESP_FAIL;
    }
    esp_err_t err = ota_update_validate_image_header(buffer, first_length);
    if (err != ESP_OK) {
        free(buffer);
        return send_update_error(req, "400 Bad Request",
                                 "Invalid OTA image. Upload the matching *-ota.bin release file.");
    }

    ota_update_t update = {0};
    err = ota_update_begin(&update, image_size);
    if (err == ESP_OK) err = ota_update_write(&update, buffer, first_length);
    size_t received = first_length;

    while (err == ESP_OK && received < image_size) {
        size_t chunk = image_size - received;
        if (chunk > OTA_BUFFER_SIZE) chunk = OTA_BUFFER_SIZE;
        int result = receive_upload_data(req, buffer, chunk);
        if (result <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = ota_update_write(&update, buffer, (size_t)result);
        received += (size_t)result;
    }
    free(buffer);

    if (err == ESP_OK) err = ota_update_finish(&update);
    if (err != ESP_OK) {
        ota_update_abort(&update);
        ESP_LOGE(TAG, "OTA update failed after %u/%u bytes: %s", (unsigned)received,
                 (unsigned)image_size, esp_err_to_name(err));
        return send_update_error(req, "500 Internal Server Error",
                                 "Firmware update failed; the current firmware is unchanged.");
    }

    ESP_LOGI(TAG, "OTA update complete (%u bytes); restarting", (unsigned)image_size);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Update complete. MiniSpeaker is restarting.");
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(DNS_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t query[300], response[320];
    while (true) {
        struct sockaddr_in source = {0};
        socklen_t source_len = sizeof(source);
        int len = recvfrom(sock, query, sizeof(query), 0,
                           (struct sockaddr *)&source, &source_len);
        if (len < 17 || query[4] != 0 || query[5] != 1) continue;

        size_t question_end = 12;
        while (question_end < (size_t)len && query[question_end] != 0) {
            uint8_t label_len = query[question_end];
            if ((label_len & 0xc0) != 0 || question_end + label_len + 1 >= (size_t)len) {
                question_end = (size_t)len;
                break;
            }
            question_end += (size_t)label_len + 1;
        }
        if (question_end + 5 > (size_t)len) continue;
        question_end += 5;
        if (question_end + 16 > sizeof(response)) continue;

        memcpy(response, query, question_end);
        response[2] = (uint8_t)(0x80 | (query[2] & 0x01));
        response[3] = 0x80;
        response[6] = 0; response[7] = 1;
        response[8] = 0; response[9] = 0;
        response[10] = 0; response[11] = 0;

        size_t pos = question_end;
        const uint8_t answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x3c, 0x00, 0x04, 192, 168, 4, 1
        };
        memcpy(response + pos, answer, sizeof(answer));
        pos += sizeof(answer);
        sendto(sock, response, pos, 0, (struct sockaddr *)&source, source_len);
    }
}

static void scan_networks(void) {
    wifi_scan_config_t scan = {0};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return;
    }
    s_scan_count = MAX_SCAN_RESULTS;
    err = esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_results);
    if (err != ESP_OK) {
        s_scan_count = 0;
        ESP_LOGW(TAG, "reading scan results failed: %s", esp_err_to_name(err));
    }
}

static void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 5;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL
    };
    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL
    };
    const httpd_uri_t update = {
        .uri = "/update", .method = HTTP_POST, .handler = update_post_handler, .user_ctx = NULL
    };
    const httpd_uri_t catch_all = {
        .uri = "/*", .method = HTTP_GET, .handler = portal_get_handler, .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &catch_all));
}

void wifi_start_provisioning(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s",
             system_config_get_name());
    ap_config.ap.ssid_len = (uint8_t)strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    scan_networks();
    xTaskCreate(dns_task, "captive_dns", 4096, NULL, 4, NULL);
    start_web_server();
    ESP_LOGI(TAG, "setup AP '%s' ready at http://192.168.4.1/",
             system_config_get_name());
}
