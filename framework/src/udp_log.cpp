// DEBUG-ONLY: mirror every esp_log line to a UDP collector on the dev machine.
// The USB-Serial/JTAG console stalls under load on this board, so we cannot see
// the live RTSP->RECORD->decode sequence over serial. This ships each log line
// over Wi-Fi (reliable, the board is on the LAN) to UDP_LOG_IP:UDP_LOG_PORT while
// still printing to the console. Kept in-tree as a diagnostic aid.
//
// Compiled as C++ (not .c) so the "main" component stays single-language — mixing
// a .c and .cpp here makes PlatformIO cross-apply C/C++-only flags under -Werror.
#include "udp_log.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define UDP_LOG_IP   "192.168.50.112"   // this Mac (en0)
#define UDP_LOG_PORT 9999

static int                s_sock = -1;
static struct sockaddr_in s_dst;
static vprintf_like_t     s_orig = nullptr;
static volatile int       s_busy = 0;   // crude re-entrancy guard (debug only)

static int udp_log_vprintf(const char *fmt, va_list ap) {
    if (s_sock >= 0 && !s_busy) {
        s_busy = 1;
        char buf[320];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
            sendto(s_sock, buf, len, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
        }
        s_busy = 0;
    }
    return s_orig ? s_orig(fmt, ap) : 0;
}

void udp_log_init(void) {
    if (s_sock >= 0) return;
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;
    int fl = fcntl(sk, F_GETFL, 0);
    fcntl(sk, F_SETFL, fl | O_NONBLOCK);
    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(UDP_LOG_PORT);
    s_dst.sin_addr.s_addr = inet_addr(UDP_LOG_IP);
    s_sock = sk;
    s_orig = esp_log_set_vprintf(udp_log_vprintf);
    ESP_LOGI("udp_log", "UDP log mirror -> %s:%d", UDP_LOG_IP, UDP_LOG_PORT);
}
