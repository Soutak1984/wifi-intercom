#include "console_arduino.h"

#include "app.h"
#include "audio_port.h"
#include "ui_port.h"
#include "usb_uac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <WiFi.h>

#include "esp_system.h"

static const char *state_text(ic_state_t state) {
    switch (state) {
    case IC_ST_CALLING:
        return "calling";
    case IC_ST_CONNECTED:
        return "connected";
    case IC_ST_PAGING:
        return "paging";
    case IC_ST_PAGE_RX:
        return "page-rx";
    default:
        return "idle";
    }
}

static void join_args(int argc, char **argv, char *out, size_t cap) {
    int i;
    size_t n = 0;
    out[0] = 0;
    for (i = 1; i < argc; i++) {
        size_t k;
        if (i > 1 && n + 1 < cap) {
            out[n++] = ' ';
        }
        k = strlen(argv[i]);
        if (n + k >= cap) {
            k = cap - 1 - n;
        }
        memcpy(out + n, argv[i], k);
        n += k;
        out[n] = 0;
    }
}

static int split_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    return argc;
}

static void cmd_status(void) {
    ic_view_t view;
    ic_stats_t stats;
    int i;
    char ip[16];
    app_lock();
    ic_core_view(&g_app.core, app_now_ms(), &view);
    ic_core_stats(&g_app.core, &stats);
    app_unlock();
    ic_format_ip(g_app.cfg.ip, ip, sizeof(ip));
    Serial0.printf("station %u %s ip %s\n", (unsigned)view.self_id, g_app.cfg.name, ip);
    if (g_app.cfg.ssid[0] == 0) {
        Serial0.println("wifi ssid not set");
    } else if (view.link_up) {
        Serial0.printf("wifi %s rssi %d %s\n", g_app.cfg.ssid, (int)WiFi.RSSI(),
                      g_app.cfg.wifi_pass[0] != 0 ? "password set" : "open");
    } else {
        Serial0.printf("wifi %s down %s\n", g_app.cfg.ssid,
                      g_app.cfg.wifi_pass[0] != 0 ? "password set" : "open");
    }
    Serial0.printf("state %s status %s\n", state_text(view.state), view.status);
    Serial0.printf("mic_tx %d mic_armed %d speaker %d link %d audio %d\n",
                  view.mic_tx ? 1 : 0, view.mic_armed ? 1 : 0, view.speaker ? 1 : 0,
                  view.link_up ? 1 : 0, view.audio_ok ? 1 : 0);
    Serial0.printf("volume %u gain %u rms %u\n", (unsigned)view.volume, (unsigned)view.mic_gain,
                  (unsigned)g_app.mic_rms);
    {
        char usb[120];
        usb_uac_status(usb, sizeof(usb));
        Serial0.println(usb);
    }
    Serial0.printf("selected %u remote %u %s\n", (unsigned)view.selected, (unsigned)view.remote,
                  view.remote_name);
    {
        ui_link_t link;
        ui_battery_t bat;
        ui_link_read(&link);
        ui_battery_read(&bat);
        Serial0.printf("wifi bars %d rssi %d\n", link.bars, link.rssi_dbm);
        if (!bat.present) {
            Serial0.printf("battery none mv %d\n", bat.millivolts);
        } else {
            Serial0.printf("battery %d%% %d mV\n", bat.percent, bat.millivolts);
        }
    }
    for (i = 0; i < IC_STATION_COUNT; i++) {
        ic_format_ip(view.dir[i].ip, ip, sizeof(ip));
        Serial0.printf("dir %u %s %s %s\n", (unsigned)view.dir[i].id, view.dir[i].name, ip,
                      view.dir[i].id == view.self_id ? "self" : (view.dir[i].online ? "online" : "offline"));
    }
    Serial0.printf("calls out %u in %u pages out %u in %u\n", stats.calls_out, stats.calls_in,
                  stats.pages_out, stats.pages_in);
    Serial0.printf("bad_hmac %u malformed %u replay %u stale %u rate %u busy %u\n",
                  stats.bad_hmac, stats.malformed, stats.replay, stats.stale, stats.rate_limited,
                  stats.rejected_busy);
    Serial0.printf("usb_rx %u usb_tx %u media_tx %u media_rx %u media_drop %u plc %u sig_drop %u sig_tx_err %u\n",
                  (unsigned)g_app.usb_rx_err, (unsigned)g_app.usb_tx_err, (unsigned)g_app.media_tx,
                  (unsigned)g_app.media_rx, (unsigned)g_app.media_drop, (unsigned)g_app.plc_frames,
                  (unsigned)g_app.sig_drop, (unsigned)g_app.sig_tx_err);
    Serial0.printf("heap %u\n", (unsigned)esp_get_free_heap_size());
    Serial0.println("Console and web cannot enable microphone transmission.");
}

static void cmd_id(int argc, char **argv) {
    long v;
    char *end = NULL;
    if (argc != 2) {
        Serial0.println("usage: id <100-103>");
        return;
    }
    v = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != 0 || v < IC_STATION_MIN || v > IC_STATION_MAX) {
        Serial0.println("station id must be 100 to 103");
        return;
    }
    app_lock();
    g_app.cfg.station_id = (uint16_t)v;
    app_unlock();
    Serial0.printf("stored %ld. save and reboot to apply.\n", v);
}

static void cmd_name(int argc, char **argv) {
    char name[IC_NAME_LEN];
    if (argc < 2) {
        Serial0.println("usage: name <station name>");
        return;
    }
    join_args(argc, argv, name, sizeof(name));
    if (!ic_config_name_ok(name)) {
        Serial0.println("name must be 1 to 15 visible characters");
        return;
    }
    app_lock();
    snprintf(g_app.cfg.name, sizeof(g_app.cfg.name), "%s", name);
    app_unlock();
    Serial0.println("stored. save and reboot to apply.");
}

static void set_addr(int argc, char **argv, uint32_t *slot, const char *what) {
    uint32_t ip;
    if (argc != 2 || !ic_parse_ip(argv[1], &ip)) {
        Serial0.printf("usage: %s <a.b.c.d>\n", what);
        return;
    }
    app_lock();
    *slot = ip;
    app_unlock();
    Serial0.println("stored. save and reboot to apply.");
}

static void cmd_mask(int argc, char **argv) {
    uint32_t ip;
    if (argc != 2 || !ic_parse_ip(argv[1], &ip) || ip == 0) {
        Serial0.println("usage: mask <a.b.c.d>");
        return;
    }
    app_lock();
    g_app.cfg.netmask = ip;
    app_unlock();
    Serial0.println("stored. save and reboot to apply.");
}

static void cmd_peer(int argc, char **argv) {
    long id;
    uint32_t ip;
    char *end = NULL;
    if (argc != 3) {
        Serial0.println("usage: peer <100-103> <a.b.c.d>");
        return;
    }
    id = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != 0 || id < IC_STATION_MIN || id > IC_STATION_MAX ||
        !ic_parse_ip(argv[2], &ip) || ip == 0) {
        Serial0.println("peer arguments are not valid");
        return;
    }
    app_lock();
    g_app.cfg.peer_ip[id - IC_STATION_MIN] = ip;
    app_unlock();
    Serial0.println("stored. save and reboot to apply.");
}

static void cmd_key(int argc, char **argv) {
    if (argc != 2 || !ic_config_key_ok(argv[1])) {
        Serial0.println("usage: key <16 visible characters>");
        return;
    }
    app_lock();
    memcpy(g_app.cfg.key, argv[1], IC_KEY_LEN);
    app_unlock();
    Serial0.println("stored. save and reboot to apply. The key is not printed.");
}

static void cmd_pin(int argc, char **argv) {
    if (argc != 2 || !ic_config_pin_ok(argv[1])) {
        Serial0.println("usage: pin <4-8 letters or digits>");
        return;
    }
    app_lock();
    snprintf(g_app.cfg.pin, sizeof(g_app.cfg.pin), "%s", argv[1]);
    app_unlock();
    Serial0.println("stored. save and reboot to apply. The PIN is not printed.");
}

static int parse_0_100(const char *text, uint8_t *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || end == text || *end != 0 || v < 0 || v > 100) {
        return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static void cmd_vol(int argc, char **argv) {
    uint8_t v;
    if (argc != 2 || parse_0_100(argv[1], &v) != 0) {
        Serial0.println("usage: vol <0-100>");
        return;
    }
    app_lock();
    g_app.cfg.volume = v;
    ic_core_set_volume(&g_app.core, v);
    app_unlock();
    Serial0.printf("volume %u applied. save to keep it.\n", (unsigned)v);
}

static void cmd_gain(int argc, char **argv) {
    uint8_t v;
    if (argc != 2 || parse_0_100(argv[1], &v) != 0) {
        Serial0.println("usage: gain <0-100>");
        return;
    }
    app_lock();
    g_app.cfg.mic_gain = v;
    ic_core_set_mic_gain(&g_app.core, v);
    app_unlock();
    Serial0.printf("gain %u applied. 25 is about unity. save to keep it.\n", (unsigned)v);
}

static void cmd_aec(int argc, char **argv) {
    char *end = NULL;
    long ms;
    ic_state_t state;
    if (argc != 2) {
        Serial0.println("usage: aec <milliseconds 0-180>");
        return;
    }
    ms = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != 0 || ms < 0 || ms > 180) {
        Serial0.println("AEC delay must be 0 to 180 ms");
        return;
    }
    app_lock();
    g_app.cfg.aec_samples = (uint16_t)(ms * 8);
    state = ic_core_state(&g_app.core);
    app_unlock();
    if (state == IC_ST_IDLE && g_app.audio_ready) {
        audio_set_aec_delay((int)(ms * 8));
        Serial0.printf("AEC delay %ld ms applied while idle. save to keep it.\n", ms);
    } else {
        Serial0.printf("stored %ld ms. save and reboot to apply.\n", ms);
    }
}

static void cmd_ssid(int argc, char **argv) {
    char ssid[33];
    if (argc < 2) {
        Serial0.println("usage: ssid <network name>");
        return;
    }
    join_args(argc, argv, ssid, sizeof(ssid));
    if (!ic_config_ssid_ok(ssid)) {
        Serial0.println("SSID must be 1 to 32 visible characters");
        return;
    }
    app_lock();
    snprintf(g_app.cfg.ssid, sizeof(g_app.cfg.ssid), "%s", ssid);
    app_unlock();
    Serial0.println("stored. save and reboot to apply.");
}

static void cmd_pass(int argc, char **argv) {
    char pass[64];
    if (argc < 2) {
        Serial0.println("usage: pass <8-63 characters>    or    pass open");
        return;
    }
    if (argc == 2 && strcmp(argv[1], "open") == 0) {
        app_lock();
        g_app.cfg.wifi_pass[0] = 0;
        app_unlock();
        Serial0.println("stored an open network. save and reboot to apply.");
        return;
    }
    join_args(argc, argv, pass, sizeof(pass));
    if (!ic_config_wifi_pass_ok(pass) || pass[0] == 0) {
        Serial0.println("password must be 8 to 63 visible characters, or pass open");
        return;
    }
    app_lock();
    snprintf(g_app.cfg.wifi_pass, sizeof(g_app.cfg.wifi_pass), "%s", pass);
    app_unlock();
    Serial0.println("stored. save and reboot to apply. The password is not printed.");
}

static void cmd_save(void) {
    ic_config_t copy;
    app_lock();
    g_app.cfg.peer_ip[g_app.cfg.station_id - IC_STATION_MIN] = g_app.cfg.ip;
    copy = g_app.cfg;
    app_unlock();
    if (!ic_config_save(&copy)) {
        Serial0.println("save failed");
        return;
    }
    Serial0.println("saved. Reboot to apply id, name, addresses, Wi-Fi, key, and PIN.");
}

static void cmd_reboot(void) {
    Serial0.println("rebooting");
    Serial0.flush();
    esp_restart();
}

static void cmd_audiotest(void) {
    ic_state_t state;
    app_lock();
    state = ic_core_state(&g_app.core);
    app_unlock();
    if (!g_app.audio_ready) {
        Serial0.println("audio is not running");
        return;
    }
    if (!usb_uac_streaming()) {
        Serial0.println("headset adapter is not streaming");
        return;
    }
    if (state != IC_ST_IDLE) {
        Serial0.println("busy");
        return;
    }
    g_app.tone_frames = 150;
    Serial0.println("local tone for about 3 seconds. It is not transmitted.");
}

static void cmd_miclevel(void) {
    ic_view_t view;
    app_lock();
    ic_core_view(&g_app.core, app_now_ms(), &view);
    app_unlock();
    Serial0.printf("rms %u mic_tx %d mic_armed %d\n", (unsigned)g_app.mic_rms,
                  view.mic_tx ? 1 : 0, view.mic_armed ? 1 : 0);
    Serial0.println("This reading is local. It does not arm transmission.");
}

static void cmd_help(void) {
    Serial0.println("status");
    Serial0.println("id <100-103>");
    Serial0.println("name <station name>");
    Serial0.println("ip <a.b.c.d>");
    Serial0.println("ssid <network name>");
    Serial0.println("pass <8-63 characters>");
    Serial0.println("pass open");
    Serial0.println("gw <a.b.c.d>");
    Serial0.println("mask <a.b.c.d>");
    Serial0.println("peer <100-103> <a.b.c.d>");
    Serial0.println("key <16 chars>");
    Serial0.println("pin <4-8 chars>");
    Serial0.println("vol <0-100>");
    Serial0.println("gain <0-100>");
    Serial0.println("aec <0-180>");
    Serial0.println("save");
    Serial0.println("reboot");
    Serial0.println("audiotest");
    Serial0.println("miclevel");
    Serial0.println("The touchscreen controls calls and the microphone.");
}

static void dispatch(char *line) {
    char *argv[16];
    int argc = split_args(line, argv, 16);
    if (argc == 0) {
        return;
    }
    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "status") == 0) {
        cmd_status();
    } else if (strcmp(argv[0], "id") == 0) {
        cmd_id(argc, argv);
    } else if (strcmp(argv[0], "name") == 0) {
        cmd_name(argc, argv);
    } else if (strcmp(argv[0], "ip") == 0) {
        set_addr(argc, argv, &g_app.cfg.ip, "ip");
    } else if (strcmp(argv[0], "ssid") == 0) {
        cmd_ssid(argc, argv);
    } else if (strcmp(argv[0], "pass") == 0) {
        cmd_pass(argc, argv);
    } else if (strcmp(argv[0], "gw") == 0) {
        set_addr(argc, argv, &g_app.cfg.gateway, "gw");
    } else if (strcmp(argv[0], "mask") == 0) {
        cmd_mask(argc, argv);
    } else if (strcmp(argv[0], "peer") == 0) {
        cmd_peer(argc, argv);
    } else if (strcmp(argv[0], "key") == 0) {
        cmd_key(argc, argv);
    } else if (strcmp(argv[0], "pin") == 0) {
        cmd_pin(argc, argv);
    } else if (strcmp(argv[0], "vol") == 0) {
        cmd_vol(argc, argv);
    } else if (strcmp(argv[0], "gain") == 0) {
        cmd_gain(argc, argv);
    } else if (strcmp(argv[0], "aec") == 0) {
        cmd_aec(argc, argv);
    } else if (strcmp(argv[0], "save") == 0) {
        cmd_save();
    } else if (strcmp(argv[0], "reboot") == 0) {
        cmd_reboot();
    } else if (strcmp(argv[0], "audiotest") == 0) {
        cmd_audiotest();
    } else if (strcmp(argv[0], "miclevel") == 0) {
        cmd_miclevel();
    } else {
        Serial0.println("unknown command");
    }
}

static char s_line[128];
static size_t s_len;

static void print_banner(void) {
    Serial0.println();
    Serial0.println("LAN intercom console. The touchscreen controls calls and the microphone.");
    Serial0.println("Type help for commands. Change the default key and PIN, then save and reboot.");
    Serial0.print("ic> ");
    Serial0.flush();
}

void console_start(void) {
    print_banner();
}

void console_poll(void) {
    while (Serial0.available() > 0) {
        char c = (char)Serial0.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            s_line[s_len] = 0;
            s_len = 0;
            dispatch(s_line);
            Serial0.print("ic> ");
            continue;
        }
        if (c == 8 || c == 127) {
            if (s_len > 0) {
                s_len--;
            }
            continue;
        }
        if (s_len + 1 < sizeof(s_line)) {
            s_line[s_len++] = c;
        }
    }
}
