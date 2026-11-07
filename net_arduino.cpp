#include "net_arduino.h"

#include "app.h"

#include <errno.h>
#include <string.h>

#include <WiFi.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "ic_net";

/* -1 link down, 1 link up. The event callback only stores the request. */
static volatile int s_link_want = -1;
static int s_link_is = -1;
static volatile int s_disc_reason = 0;

static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_disc_reason = 0;
        s_link_want = 1;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        s_disc_reason = (int)info.wifi_sta_disconnected.reason;
        s_link_want = -1;
    } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP || event == ARDUINO_EVENT_WIFI_STA_STOP) {
        s_link_want = -1;
    }
}

static IPAddress ip_from_host(uint32_t host) {
    return IPAddress((uint8_t)((host >> 24) & 255u),
                     (uint8_t)((host >> 16) & 255u),
                     (uint8_t)((host >> 8) & 255u),
                     (uint8_t)(host & 255u));
}

static int udp_bind(uint16_t port) {
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket %u failed", (unsigned)port);
        return -1;
    }
    lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %u failed errno %d", (unsigned)port, errno);
        lwip_close(fd);
        return -1;
    }
    /* Newlib's O_NONBLOCK is 0x4000. The prebuilt lwIP fcntl only accepts its
     * own flag value of 1, so a header that defines O_NONBLOCK first leaves
     * the socket blocking. FIONBIO is the command lwip_ioctl implements. */
    {
        int nb = 1;
        if (lwip_ioctl(fd, FIONBIO, &nb) != 0) {
            ESP_LOGE(TAG, "nonblock %u failed errno %d", (unsigned)port, errno);
            lwip_close(fd);
            return -1;
        }
    }
    return fd;
}

static void send_sig_pkt(const ic_tx_pkt_t *pkt) {
    struct sockaddr_in to;
    int n;
    if (!pkt || pkt->len == 0 || pkt->dst_ip == 0 || g_app.sig_fd < 0) {
        return;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(IC_SIG_PORT);
    to.sin_addr.s_addr = htonl(pkt->dst_ip);
    n = lwip_sendto(g_app.sig_fd, pkt->pkt, pkt->len, 0, (struct sockaddr *)&to, sizeof(to));
    if (n < 0) {
        g_app.sig_tx_err++;
    }
}

static void signal_task(void *arg) {
    uint8_t buf[IC_SIG_MAX_PACKET];
    bool watch = false;
    (void)arg;
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        watch = true;
    }
    while (1) {
        struct sockaddr_in from;
        socklen_t flen;
        int n;
        ic_tx_pkt_t queued;

        if (watch) {
            esp_task_wdt_reset();
        }
        flen = sizeof(from);
        n = lwip_recvfrom(g_app.sig_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n > 0) {
            ic_out_t out;
            app_lock();
            ic_core_on_sig(&g_app.core, buf, (size_t)n, ntohl(from.sin_addr.s_addr), app_now_ms(), &out);
            app_unlock();
            app_submit_sig(&out);
        }
        while (xQueueReceive(g_app.sig_q, &queued, 0) == pdTRUE) {
            send_sig_pkt(&queued);
        }
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            taskYIELD();
        }
    }
}

void net_apply_link(void) {
    int want = s_link_want;
    ic_out_t out;
    if (want == s_link_is) {
        return;
    }
    s_link_is = want;
    if (want == 1) {
        IPAddress got = WiFi.localIP();
        uint32_t want_ip = g_app.cfg.ip;
        ESP_LOGI(TAG, "wifi up %u.%u.%u.%u rssi %d", (unsigned)got[0], (unsigned)got[1],
                 (unsigned)got[2], (unsigned)got[3], (int)WiFi.RSSI());
        if (got[0] != (uint8_t)((want_ip >> 24) & 255u) ||
            got[1] != (uint8_t)((want_ip >> 16) & 255u) ||
            got[2] != (uint8_t)((want_ip >> 8) & 255u) ||
            got[3] != (uint8_t)(want_ip & 255u)) {
            ESP_LOGW(TAG, "address on the air does not match the stored static address");
        }
    } else {
        int reason = s_disc_reason;
        ESP_LOGW(TAG, "wifi down %s",
                 reason != 0 ? WiFi.disconnectReasonName((wifi_err_reason_t)reason) : "lost");
    }
    app_lock();
    ic_core_set_link(&g_app.core, want == 1, app_now_ms(), &out);
    app_unlock();
    app_submit_sig(&out);
}

int net_start(const ic_config_t *cfg) {
    IPAddress local = ip_from_host(cfg->ip);
    IPAddress gateway = ip_from_host(cfg->gateway);
    IPAddress mask = ip_from_host(cfg->netmask);
    IPAddress dns((uint32_t)0);
    int sig_fd;
    int media_fd;

    if (!ic_config_ssid_ok(cfg->ssid)) {
        ESP_LOGE(TAG, "Wi-Fi SSID is not set");
        return -1;
    }
    if (!ic_config_wifi_pass_ok(cfg->wifi_pass)) {
        ESP_LOGE(TAG, "Wi-Fi password is not valid");
        return -1;
    }
    /* A mask that does not start with 255 makes WiFi.config swap its arguments. */
    if (((cfg->netmask >> 24) & 255u) != 255u) {
        ESP_LOGE(TAG, "netmask must start with 255");
        return -1;
    }

    WiFi.persistent(false);
    WiFi.onEvent(on_wifi_event);
    /* Before the interface starts. A later setSleep(false) stores the mode
     * and leaves the radio in the previous sleep setting. */
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setMinSecurity(cfg->wifi_pass[0] == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK);
    if (!WiFi.config(local, gateway, mask, dns)) {
        ESP_LOGE(TAG, "static IP was not accepted");
        return -1;
    }
    if (WiFi.begin(cfg->ssid, cfg->wifi_pass[0] != 0 ? cfg->wifi_pass : NULL) == WL_CONNECT_FAILED) {
        ESP_LOGE(TAG, "Wi-Fi did not start");
        return -1;
    }
    ESP_LOGI(TAG, "wifi join %s", cfg->ssid);

    sig_fd = udp_bind(IC_SIG_PORT);
    media_fd = udp_bind(IC_MED_PORT);
    if (sig_fd < 0 || media_fd < 0) {
        if (sig_fd >= 0) {
            lwip_close(sig_fd);
        }
        if (media_fd >= 0) {
            lwip_close(media_fd);
        }
        return -1;
    }
    g_app.sig_fd = sig_fd;
    g_app.media_fd = media_fd;
    if (xTaskCreate(signal_task, "ic_sig", 8192, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "signal task create failed");
        g_app.sig_fd = -1;
        g_app.media_fd = -1;
        lwip_close(sig_fd);
        lwip_close(media_fd);
        return -1;
    }
    ESP_LOGI(TAG, "wifi UDP %u/%u", (unsigned)IC_SIG_PORT, (unsigned)IC_MED_PORT);
    return 0;
}

int net_join_wifi(const ic_config_t *cfg) {
    if (!cfg) {
        return -1;
    }
    if (g_app.sig_fd < 0) {
        return net_start(cfg);
    }
    if (!ic_config_ssid_ok(cfg->ssid) || !ic_config_wifi_pass_ok(cfg->wifi_pass)) {
        return -1;
    }
    WiFi.setMinSecurity(cfg->wifi_pass[0] == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK);
    WiFi.disconnect(false);
    if (WiFi.begin(cfg->ssid, cfg->wifi_pass[0] != 0 ? cfg->wifi_pass : NULL) == WL_CONNECT_FAILED) {
        ESP_LOGE(TAG, "Wi-Fi did not restart");
        return -1;
    }
    ESP_LOGI(TAG, "wifi join %s", cfg->ssid);
    return 0;
}

int net_rssi_dbm(void) {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    return (int)WiFi.RSSI();
}
