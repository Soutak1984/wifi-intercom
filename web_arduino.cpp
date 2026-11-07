#include "web_arduino.h"

#include "app.h"
#include "ui_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Network.h>
#include <WebServer.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ic_web";
static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static WebServer server(80);
static char s_page[5120];

static void page_set(const char *text) {
    size_t n = strlen(text);
    if (n >= sizeof(s_page)) {
        n = sizeof(s_page) - 1;
    }
    memcpy(s_page, text, n);
    s_page[n] = 0;
}

static void page_add(const char *text) {
    size_t used = strlen(s_page);
    size_t n = strlen(text);
    if (used + n >= sizeof(s_page)) {
        n = sizeof(s_page) - 1 - used;
    }
    if ((int)n <= 0) {
        return;
    }
    memcpy(s_page + used, text, n);
    s_page[used + n] = 0;
}

static void page_addf(const char *fmt, ...) {
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    page_add(line);
}

static void page_esc(const char *text) {
    char one[2];
    if (!text) {
        return;
    }
    one[1] = 0;
    for (; *text; text++) {
        if (*text == '<') {
            page_add("&lt;");
        } else if (*text == '>') {
            page_add("&gt;");
        } else if (*text == '&') {
            page_add("&amp;");
        } else if (*text == '"') {
            page_add("&quot;");
        } else {
            one[0] = *text;
            page_add(one);
        }
    }
}

static int b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    size_t i = 0;
    size_t o = 0;
    while (i < len) {
        uint32_t v;
        int n = 1;
        if (o + 5 > cap) {
            return -1;
        }
        v = (uint32_t)in[i++] << 16;
        if (i < len) {
            v |= (uint32_t)in[i++] << 8;
            n++;
        }
        if (i < len) {
            v |= in[i++];
            n++;
        }
        out[o++] = k_b64[(v >> 18) & 63];
        out[o++] = k_b64[(v >> 12) & 63];
        out[o++] = (n >= 2) ? k_b64[(v >> 6) & 63] : '=';
        out[o++] = (n >= 3) ? k_b64[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

static bool ct_equal(const char *a, const char *b) {
    size_t i;
    unsigned diff = 0;
    size_t na = strlen(a);
    size_t nb = strlen(b);
    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

static bool authorized(void) {
    char pin[IC_PIN_LEN + 1];
    char userpass[24];
    char expect_b64[40];
    char expect[56];
    char header[80];

    app_lock();
    snprintf(pin, sizeof(pin), "%s", g_app.cfg.pin);
    app_unlock();
    snprintf(userpass, sizeof(userpass), "admin:%s", pin);
    if (b64_encode((const uint8_t *)userpass, strlen(userpass), expect_b64, sizeof(expect_b64)) < 0) {
        return false;
    }
    snprintf(expect, sizeof(expect), "Basic %s", expect_b64);
    if (!server.hasHeader("Authorization")) {
        return false;
    }
    String got = server.header("Authorization");
    if (got.length() == 0 || got.length() >= sizeof(header)) {
        return false;
    }
    memcpy(header, got.c_str(), got.length());
    header[got.length()] = 0;
    return ct_equal(header, expect);
}

static void send_auth(void) {
    server.sendHeader("WWW-Authenticate", "Basic realm=\"intercom\"");
    server.send(401, String("text/plain"), String("Authentication required\n"));
}

static void send_html(void) {
    server.send(200, String("text/html; charset=utf-8"), String(s_page));
}

static void send_text(int code, const char *body) {
    server.send(code, String("text/plain"), String(body));
}

static const char *state_text(ic_state_t state) {
    switch (state) {
    case IC_ST_CALLING:
        return "calling";
    case IC_ST_CONNECTED:
        return "connected";
    case IC_ST_PAGING:
        return "paging";
    case IC_ST_PAGE_RX:
        return "receiving page";
    default:
        return "idle";
    }
}

static void add_style(void) {
    page_add("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
    page_add("<style>body{font-family:sans-serif;margin:1.2rem;background:#111;color:#eee}");
    page_add("a,button{color:#9cf}label{display:block;margin-top:.6rem}");
    page_add(".mic{font-size:2.4rem;font-weight:700;padding:1rem;border-radius:8px}");
    page_add(".live{background:#7a1f1f}.armed{background:#6a4a10}.off{background:#145214}");
    page_add(".warn{background:#6a4a10;padding:.6rem}</style>");
}

static void build_status(const ic_view_t *view, const ic_config_t *cfg) {
    const char *cls = "off";
    const char *mic = "MICROPHONE OFF";
    int i;
    char ip[16];
    page_set("");
    add_style();
    page_add("<meta http-equiv=\"refresh\" content=\"3\"><title>Intercom</title></head><body>");
    if (ic_config_key_is_default(cfg) || ic_config_pin_is_default(cfg)) {
        page_add("<p class=\"warn\">Change the default shared key and web PIN before real use.</p>");
    }
    page_addf("<h1>Station %u ", (unsigned)view->self_id);
    page_esc(cfg->name);
    page_add("</h1>");
    if (view->mic_tx) {
        cls = "live";
        mic = "MICROPHONE LIVE";
    } else if (view->mic_armed) {
        cls = "armed";
        mic = "MICROPHONE ARMED, NOT TRANSMITTING";
    }
    page_addf("<p class=\"mic %s\">%s</p>", cls, mic);
    page_add("<p>Status: ");
    page_esc(view->status);
    page_addf("</p><p>State: %s. Link: %s. Speaker: %s. Volume: %u. Mic gain: %u.</p>",
              state_text(view->state), view->link_up ? "up" : "down",
              view->speaker ? "on" : "off", (unsigned)view->volume, (unsigned)view->mic_gain);
    page_add("<p>Wi-Fi: ");
    if (cfg->ssid[0] == 0) {
        page_add("SSID is not set");
    } else {
        page_esc(cfg->ssid);
        page_add(cfg->wifi_pass[0] != 0 ? " (password set)" : " (open network)");
    }
    page_add(".</p>");
    page_add("<h2>Directory</h2><ul>");
    for (i = 0; i < IC_STATION_COUNT; i++) {
        ic_format_ip(view->dir[i].ip, ip, sizeof(ip));
        page_addf("<li>%u ", (unsigned)view->dir[i].id);
        page_esc(view->dir[i].name);
        page_addf(" %s %s</li>", ip,
                  view->dir[i].id == view->self_id ? "this station" :
                  (view->dir[i].online ? "online" : "offline"));
    }
    page_add("</ul>");
    {
        ui_link_t link;
        ui_battery_t bat;
        ui_link_read(&link);
        ui_battery_read(&bat);
        page_addf("<p>Wi-Fi signal: %d bars", link.bars);
        if (link.up) {
            page_addf(", %d dBm.", link.rssi_dbm);
        } else {
            page_add(", down.");
        }
        page_add("</p><p>Battery: ");
        if (!bat.present) {
            page_add("no pack, USB supply.");
        } else {
            page_addf("%d%%, %d.%03d V.", bat.percent, bat.millivolts / 1000,
                      bat.millivolts % 1000);
        }
        page_add("</p>");
    }
    page_add("<p><a href=\"/config\">Configuration</a></p>");
    page_add("<p>Calls, paging, and the microphone are controlled from the touchscreen.</p>");
    page_add("</body></html>");
}

static void build_config(const ic_config_t *cfg) {
    char ip[16];
    char gw[16];
    char mask[16];
    char peer[4][16];
    int i;
    page_set("");
    add_style();
    page_add("<title>Intercom config</title></head><body><h1>Configuration</h1>");
    page_add("<p>Saving reboots the station. The key and PIN boxes stay blank so this page does not reveal them.</p>");
    page_add("<form method=\"post\" action=\"/save\">");
    page_addf("<label>Station id <input name=\"sid\" value=\"%u\"></label>", (unsigned)cfg->station_id);
    page_add("<label>Name <input name=\"name\" maxlength=\"15\" value=\"");
    page_esc(cfg->name);
    page_add("\"></label>");
    page_add("<label>Wi-Fi SSID <input name=\"ssid\" maxlength=\"32\" value=\"");
    page_esc(cfg->ssid);
    page_add("\"></label>");
    page_add("<label>Wi-Fi password, blank keeps the current one, or type open for an open network <input name=\"wpass\" type=\"password\" maxlength=\"63\" value=\"\"></label>");
    ic_format_ip(cfg->ip, ip, sizeof(ip));
    ic_format_ip(cfg->gateway, gw, sizeof(gw));
    ic_format_ip(cfg->netmask, mask, sizeof(mask));
    page_addf("<label>IP <input name=\"ip\" value=\"%s\"></label>", ip);
    page_addf("<label>Gateway <input name=\"gw\" value=\"%s\"></label>", gw);
    page_addf("<label>Mask <input name=\"mask\" value=\"%s\"></label>", mask);
    for (i = 0; i < IC_STATION_COUNT; i++) {
        ic_format_ip(cfg->peer_ip[i], peer[i], sizeof(peer[i]));
        page_addf("<label>Station %u IP <input name=\"p%u\" value=\"%s\"></label>",
                  100 + i, 100 + i, peer[i]);
    }
    page_addf("<label>Volume 0-100 <input name=\"vol\" value=\"%u\"></label>", (unsigned)cfg->volume);
    page_addf("<label>Mic gain 0-100 <input name=\"gain\" value=\"%u\"></label>", (unsigned)cfg->mic_gain);
    page_addf("<label>AEC delay ms <input name=\"aec\" value=\"%u\"></label>",
              (unsigned)(cfg->aec_samples / 8));
    page_add("<label>New shared key, 16 characters, blank keeps the current key <input name=\"key\" value=\"\"></label>");
    page_add("<label>New web PIN, 4-8 letters or digits, blank keeps the current PIN <input name=\"pin\" value=\"\"></label>");
    page_add("<p><button type=\"submit\">Save and reboot</button></p></form>");
    page_add("<p><a href=\"/\">Status</a></p></body></html>");
}

static void on_root(void) {
    ic_view_t view;
    ic_config_t cfg;
    if (!authorized()) {
        send_auth();
        return;
    }
    app_lock();
    ic_core_view(&g_app.core, app_now_ms(), &view);
    cfg = g_app.cfg;
    app_unlock();
    build_status(&view, &cfg);
    send_html();
}

static void on_config(void) {
    ic_config_t cfg;
    if (!authorized()) {
        send_auth();
        return;
    }
    app_lock();
    cfg = g_app.cfg;
    app_unlock();
    build_config(&cfg);
    send_html();
}

static bool arg_copy(const char *name, char *out, size_t cap) {
    if (!server.hasArg(name)) {
        out[0] = 0;
        return false;
    }
    String v = server.arg(name);
    if (v.length() >= cap) {
        out[0] = 0;
        return false;
    }
    memcpy(out, v.c_str(), v.length());
    out[v.length()] = 0;
    return true;
}

static bool parse_u8(const char *text, uint8_t *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || end == text || *end != 0 || v < 0 || v > 100) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static void on_save(void) {
    ic_config_t cfg;
    char field[40];
    char wpass[64];
    char peers[4][40];
    char key[8];
    int i;
    long sid;
    long aec_ms;
    char *end = NULL;
    if (!authorized()) {
        send_auth();
        return;
    }
    app_lock();
    cfg = g_app.cfg;
    app_unlock();

    if (!arg_copy("sid", field, sizeof(field))) {
        send_text(400, "Missing station id\n");
        return;
    }
    sid = strtol(field, &end, 10);
    if (end == field || *end != 0 || sid < IC_STATION_MIN || sid > IC_STATION_MAX) {
        send_text(400, "Station id must be 100 to 103\n");
        return;
    }
    cfg.station_id = (uint16_t)sid;
    if (!arg_copy("name", field, sizeof(field)) || !ic_config_name_ok(field)) {
        send_text(400, "Name must be 1 to 15 visible characters\n");
        return;
    }
    snprintf(cfg.name, sizeof(cfg.name), "%s", field);
    if (!arg_copy("ssid", field, sizeof(field)) || !ic_config_ssid_ok(field)) {
        send_text(400, "SSID must be 1 to 32 visible characters\n");
        return;
    }
    snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", field);
    if (!arg_copy("wpass", wpass, sizeof(wpass))) {
        send_text(400, "Bad Wi-Fi password\n");
        return;
    }
    if (strcmp(wpass, "open") == 0) {
        cfg.wifi_pass[0] = 0;
    } else if (wpass[0] != 0) {
        if (!ic_config_wifi_pass_ok(wpass)) {
            send_text(400, "Wi-Fi password must be 8 to 63 visible characters, or open\n");
            return;
        }
        snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", wpass);
    }
    if (!arg_copy("ip", field, sizeof(field)) || !ic_parse_ip(field, &cfg.ip) || cfg.ip == 0) {
        send_text(400, "Bad IP\n");
        return;
    }
    if (!arg_copy("gw", field, sizeof(field)) || !ic_parse_ip(field, &cfg.gateway)) {
        send_text(400, "Bad gateway\n");
        return;
    }
    if (!arg_copy("mask", field, sizeof(field)) || !ic_parse_ip(field, &cfg.netmask) || cfg.netmask == 0) {
        send_text(400, "Bad netmask\n");
        return;
    }
    for (i = 0; i < IC_STATION_COUNT; i++) {
        snprintf(key, sizeof(key), "p%u", 100 + i);
        if (!arg_copy(key, peers[i], sizeof(peers[i])) ||
            !ic_parse_ip(peers[i], &cfg.peer_ip[i]) || cfg.peer_ip[i] == 0) {
            send_text(400, "Bad station IP\n");
            return;
        }
    }
    cfg.peer_ip[cfg.station_id - IC_STATION_MIN] = cfg.ip;
    if (!arg_copy("vol", field, sizeof(field)) || !parse_u8(field, &cfg.volume)) {
        send_text(400, "Volume must be 0 to 100\n");
        return;
    }
    if (!arg_copy("gain", field, sizeof(field)) || !parse_u8(field, &cfg.mic_gain)) {
        send_text(400, "Gain must be 0 to 100\n");
        return;
    }
    if (!arg_copy("aec", field, sizeof(field))) {
        send_text(400, "Missing AEC delay\n");
        return;
    }
    aec_ms = strtol(field, &end, 10);
    if (end == field || *end != 0 || aec_ms < 0 || aec_ms > 180) {
        send_text(400, "AEC delay must be 0 to 180 ms\n");
        return;
    }
    cfg.aec_samples = (uint16_t)(aec_ms * 8);
    if (arg_copy("key", field, sizeof(field)) && field[0] != 0) {
        if (!ic_config_key_ok(field)) {
            send_text(400, "Key must be exactly 16 visible characters\n");
            return;
        }
        memcpy(cfg.key, field, IC_KEY_LEN);
    }
    if (arg_copy("pin", field, sizeof(field)) && field[0] != 0) {
        if (!ic_config_pin_ok(field)) {
            send_text(400, "PIN must be 4 to 8 letters or digits\n");
            return;
        }
        snprintf(cfg.pin, sizeof(cfg.pin), "%s", field);
    }
    if (!ic_config_save(&cfg)) {
        send_text(500, "Save failed\n");
        return;
    }
    app_lock();
    g_app.cfg = cfg;
    app_unlock();
    send_text(200, "Saved. Rebooting.\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void web_task(void *arg) {
    bool watch = false;
    (void)arg;
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        watch = true;
    }
    while (1) {
        if (watch) {
            esp_task_wdt_reset();
        }
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int web_start(void) {
    char check[32];
    if (b64_encode((const uint8_t *)"admin:1234", 10, check, sizeof(check)) < 0 ||
        strcmp(check, "YWRtaW46MTIzNA==") != 0) {
        ESP_LOGE(TAG, "base64 self-check failed");
        return -1;
    }
    server.on("/", HTTP_GET, on_root);
    server.on("/config", HTTP_GET, on_config);
    server.on("/save", HTTP_POST, on_save);
    /* Wi-Fi brings lwIP up. server.begin() asserts if that stack never started. */
    if (!Network.begin()) {
        ESP_LOGE(TAG, "network stack did not start");
        return -1;
    }
    server.begin();
    if (xTaskCreate(web_task, "ic_web", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "web task create failed");
        return -1;
    }
    ESP_LOGI(TAG, "status page on port 80");
    return 0;
}
