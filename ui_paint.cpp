#include "ui_paint.h"

#include "app.h"
#include "audio_port.h"
#include "font8x8_basic.h"
#include "panel_7b.h"
#include "ui_port.h"
#include "usb_uac.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "ic_ui";

static constexpr uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static constexpr uint16_t COL_BG = rgb565(0x14, 0x11, 0x0E);
static constexpr uint16_t COL_SURFACE = rgb565(0x22, 0x1C, 0x17);
static constexpr uint16_t COL_SURFACE2 = rgb565(0x2C, 0x25, 0x1E);
static constexpr uint16_t COL_LINE = rgb565(0x4A, 0x3E, 0x34);
static constexpr uint16_t COL_TEXT = rgb565(0xF6, 0xF1, 0xE8);
static constexpr uint16_t COL_MUTED = rgb565(0xA8, 0x98, 0x88);
static constexpr uint16_t COL_AMBER = rgb565(0xE4, 0xA0, 0x4A);
static constexpr uint16_t COL_INK = rgb565(0x2A, 0x1C, 0x0A);
static constexpr uint16_t COL_GREEN = rgb565(0x8F, 0xB5, 0x73);
static constexpr uint16_t COL_RED = rgb565(0xD4, 0x65, 0x4E);
static constexpr uint16_t COL_TEAL = rgb565(0x7E, 0xAE, 0xA4);

enum {
    ACT_SEL = 1,
    ACT_CALL,
    ACT_END,
    ACT_MIC,
    ACT_PAGE,
    ACT_ALL,
    ACT_VOL,
    ACT_GAIN,
    ACT_AEC,
    ACT_SETUP,
    ACT_POWER,
    ACT_BACK,
    ACT_TEST
};

enum { NAV_AUTO = 0, NAV_SETUP = 1, NAV_POWER = 2 };

enum {
    SCR_SPLASH = 0,
    SCR_HOME,
    SCR_CALLING,
    SCR_CALL,
    SCR_PAGE,
    SCR_RX,
    SCR_BUSY,
    SCR_LINK,
    SCR_AUDIO,
    SCR_SETUP,
    SCR_POWER
};

struct hit {
    int x, y, w, h, act, arg;
};

struct snap {
    ic_view_t view;
    ui_link_t link;
    ui_battery_t bat;
    uint32_t age_ms;
    uint32_t rms;
    int aec_ms;
    int nlegs;
    bool key_default;
    bool headset;
    bool tone;
    char ssid[33];
    char ip[16];
};

struct paint_sig {
    int screen;
    int age_sec;
    ic_state_t state;
    uint16_t self_id;
    uint16_t selected;
    uint16_t remote;
    uint8_t volume;
    uint8_t mic_gain;
    uint8_t mic_armed;
    uint8_t mic_tx;
    uint8_t link_up;
    uint8_t audio_ok;
    uint8_t online[4];
    int bars;
    int rssi;
    int bat_pct;
    int bat_mv;
    int aec_ms;
    int nlegs;
    uint8_t headset;
    uint8_t tone;
    uint8_t key_default;
    uint8_t splash;
    char status[IC_STATUS_LEN];
    char remote_name[IC_NAME_LEN];
    char ssid[33];
    char ip[16];
};

static uint16_t *s_px;
static hit s_hits[24];
static int s_nhit;
static int s_nav;
static bool s_splash;
static bool s_skip;
static bool s_dismiss;
static bool s_force;
static bool s_have;
static bool s_finger;
static int s_up_run;
static uint32_t s_t0;
static uint32_t s_hit_ms;
static uint32_t s_drawn_ms;
static paint_sig s_sig;

static void clear_screen(uint16_t c) {
    uint32_t d = ((uint32_t)c << 16) | c;
    uint32_t *p = (uint32_t *)s_px;
    uint32_t *end = p + (PANEL_W * PANEL_H) / 2;
    while (p < end) {
        *p++ = d;
    }
}

static void fill(int x, int y, int w, int h, uint16_t c) {
    int row;
    int i;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0 || x >= PANEL_W || y >= PANEL_H) {
        return;
    }
    if (x + w > PANEL_W) {
        w = PANEL_W - x;
    }
    if (y + h > PANEL_H) {
        h = PANEL_H - y;
    }
    for (row = y; row < y + h; row++) {
        uint16_t *p = s_px + row * PANEL_W + x;
        for (i = 0; i < w; i++) {
            p[i] = c;
        }
    }
}

static void glyph(int x, int y, int ch, int scale, uint16_t col) {
    const uint8_t *g;
    int row;
    int colb;
    if (scale < 1) {
        return;
    }
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    g = font8x8_basic[ch];
    for (row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        int sy;
        for (colb = 0; colb < 8; colb++) {
            int x0;
            int y0;
            if ((bits & (1u << colb)) == 0) {
                continue;
            }
            x0 = x + colb * scale;
            y0 = y + row * scale;
            for (sy = 0; sy < scale; sy++) {
                int sx;
                uint16_t *p;
                if ((unsigned)(y0 + sy) >= PANEL_H) {
                    continue;
                }
                p = s_px + (y0 + sy) * PANEL_W;
                for (sx = 0; sx < scale; sx++) {
                    int xx = x0 + sx;
                    if ((unsigned)xx < PANEL_W) {
                        p[xx] = col;
                    }
                }
            }
        }
    }
}

static void text_at(int x, int y, const char *s, int scale, uint16_t col) {
    if (!s) {
        return;
    }
    for (; *s; s++) {
        glyph(x, y, (unsigned char)*s, scale, col);
        x += 8 * scale;
    }
}

static int text_px(const char *s, int scale) {
    if (!s) {
        return 0;
    }
    return (int)strlen(s) * 8 * scale;
}

static void text_center(int x, int y, int w, const char *s, int scale, uint16_t col) {
    int tw = text_px(s, scale);
    int tx = x + (w - tw) / 2;
    if (tx < 0) {
        tx = 0;
    }
    text_at(tx, y, s, scale, col);
}

static void text_fit(int x, int y, int max_w, const char *s, int scale, uint16_t col) {
    char buf[24];
    int n = 0;
    int maxc;
    if (!s || scale < 1) {
        return;
    }
    maxc = max_w / (8 * scale);
    if (maxc > (int)sizeof(buf) - 1) {
        maxc = (int)sizeof(buf) - 1;
    }
    if (maxc < 1) {
        return;
    }
    while (s[n] && n < maxc) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = 0;
    text_at(x, y, buf, scale, col);
}

static void hits_clear(void) {
    s_nhit = 0;
}

static void hit_add(int x, int y, int w, int h, int act, int arg) {
    hit *b;
    if (s_nhit >= (int)(sizeof(s_hits) / sizeof(s_hits[0]))) {
        return;
    }
    b = &s_hits[s_nhit++];
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    b->act = act;
    b->arg = arg;
}

static void draw_btn(int x, int y, int w, int h, const char *label, const char *sub, int style,
                     int act, int arg) {
    uint16_t bg = COL_SURFACE2;
    uint16_t fg = COL_TEXT;
    uint16_t subc = COL_MUTED;
    int scale = 3;
    int block;
    int ty;
    if (style == 1) {
        bg = COL_AMBER;
        fg = COL_INK;
        subc = COL_INK;
    } else if (style == 2) {
        bg = COL_RED;
        fg = COL_TEXT;
        subc = COL_TEXT;
    } else if (style == 3) {
        bg = COL_SURFACE;
        fg = COL_MUTED;
    } else if (style == 4) {
        bg = COL_SURFACE;
        fg = COL_LINE;
        subc = COL_LINE;
    }
    fill(x, y, w, h, bg);
    if (label && text_px(label, 3) > w - 16) {
        scale = 2;
    }
    block = 8 * scale + (sub ? 28 : 0);
    ty = y + (h - block) / 2;
    if (ty < y + 4) {
        ty = y + 4;
    }
    text_center(x, ty, w, label ? label : "", scale, fg);
    if (sub) {
        text_center(x, ty + 8 * scale + 6, w, sub, 2, subc);
    }
    if (style != 4 && act != 0) {
        hit_add(x, y, w, h, act, arg);
    }
}

static void station_name(const snap *s, uint16_t id, char *dst, size_t cap) {
    int i = (int)id - IC_STATION_MIN;
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    if (cap <= IC_NAME_LEN || i < 0 || i >= IC_STATION_COUNT) {
        snprintf(dst, cap, "%u", (unsigned)id);
        return;
    }
    memcpy(dst, s->view.dir[i].name, IC_NAME_LEN);
    dst[IC_NAME_LEN] = 0;
    if (dst[0] == 0) {
        snprintf(dst, cap, "%u", (unsigned)id);
    }
}

static void take_snap(uint32_t now, snap *s) {
    int i;
    memset(s, 0, sizeof(*s));
    app_lock();
    ic_core_view(&g_app.core, now, &s->view);
    s->view.status[IC_STATUS_LEN - 1] = 0;
    s->view.remote_name[IC_NAME_LEN - 1] = 0;
    for (i = 0; i < 3; i++) {
        if (g_app.core.legs[i].state != IC_LEG_EMPTY && g_app.core.legs[i].start_ms != 0) {
            s->age_ms = now - g_app.core.legs[i].start_ms;
            break;
        }
    }
    s->aec_ms = (int)g_app.cfg.aec_samples / 8;
    s->nlegs = ic_core_legs(&g_app.core);
    s->key_default = ic_config_key_is_default(&g_app.cfg);
    memcpy(s->ssid, g_app.cfg.ssid, sizeof(s->ssid));
    s->ssid[sizeof(s->ssid) - 1] = 0;
    ic_format_ip(g_app.cfg.ip, s->ip, sizeof(s->ip));
    app_unlock();
    s->rms = g_app.mic_rms;
    s->headset = usb_uac_streaming();
    s->tone = g_app.tone_frames > 0;
    ui_link_read(&s->link);
    ui_battery_read(&s->bat);
}

static void save_cfg(void) {
    ic_config_t copy;
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    if (!ic_config_save(&copy)) {
        ESP_LOGW(TAG, "panel level was not saved");
    }
}

static void step_volume(int delta) {
    app_lock();
    ic_core_ui_volume_step(&g_app.core, delta);
    g_app.cfg.volume = ic_core_volume(&g_app.core);
    app_unlock();
    save_cfg();
}

static void step_gain(int delta) {
    int g;
    app_lock();
    g = (int)ic_core_mic_gain(&g_app.core) + delta;
    if (g < 0) {
        g = 0;
    }
    if (g > 100) {
        g = 100;
    }
    ic_core_set_mic_gain(&g_app.core, (uint8_t)g);
    g_app.cfg.mic_gain = (uint8_t)g;
    app_unlock();
    save_cfg();
}

static void step_aec(int delta_ms) {
    int ms;
    ic_state_t st;
    app_lock();
    ms = (int)g_app.cfg.aec_samples / 8 + delta_ms;
    if (ms < 0) {
        ms = 0;
    }
    if (ms > 180) {
        ms = 180;
    }
    g_app.cfg.aec_samples = (uint16_t)(ms * 8);
    st = ic_core_state(&g_app.core);
    app_unlock();
    if (st == IC_ST_IDLE && g_app.audio_ready) {
        audio_set_aec_delay(ms * 8);
    }
    save_cfg();
}

static void do_test(void) {
    ic_state_t st;
    app_lock();
    st = ic_core_state(&g_app.core);
    app_unlock();
    if (!g_app.audio_ready || !usb_uac_streaming() || st != IC_ST_IDLE) {
        return;
    }
    g_app.tone_frames = 150;
}

static void dispatch(int x, int y) {
    int i;
    ic_state_t st;
    for (i = 0; i < s_nhit; i++) {
        hit *b = &s_hits[i];
        if (x < b->x || y < b->y || x >= b->x + b->w || y >= b->y + b->h) {
            continue;
        }
        s_force = true;
        switch (b->act) {
        case ACT_SEL:
            ui_select((uint16_t)b->arg);
            s_dismiss = true;
            break;
        case ACT_CALL:
            ui_call();
            break;
        case ACT_END:
            ui_end();
            break;
        case ACT_MIC:
            ui_mic();
            break;
        case ACT_PAGE:
            ui_page(false);
            break;
        case ACT_ALL:
            ui_page(true);
            break;
        case ACT_VOL:
            step_volume(b->arg);
            break;
        case ACT_GAIN:
            step_gain(b->arg);
            break;
        case ACT_AEC:
            step_aec(b->arg);
            break;
        case ACT_SETUP:
            app_lock();
            st = ic_core_state(&g_app.core);
            app_unlock();
            if (st == IC_ST_IDLE) {
                s_nav = NAV_SETUP;
            }
            break;
        case ACT_POWER:
            s_nav = NAV_POWER;
            break;
        case ACT_BACK:
            if (b->arg == 1) {
                s_nav = NAV_SETUP;
            } else {
                s_nav = NAV_AUTO;
                s_dismiss = true;
            }
            break;
        case ACT_TEST:
            do_test();
            break;
        default:
            break;
        }
        return;
    }
}

static const char *pill_of(const snap *s) {
    if (!s->view.link_up) {
        return "No Wi-Fi";
    }
    if (!s->view.audio_ok && s->view.state == IC_ST_IDLE) {
        return "Headset";
    }
    if (s->view.state == IC_ST_CALLING) {
        return "Calling";
    }
    if (s->view.state == IC_ST_CONNECTED) {
        return s->view.mic_tx ? "Live" : "In call";
    }
    if (s->view.state == IC_ST_PAGING) {
        return "Paging";
    }
    if (s->view.state == IC_ST_PAGE_RX) {
        return "Page";
    }
    if (strcmp(s->view.status, "Busy") == 0) {
        return "Busy";
    }
    if (strcmp(s->view.status, "Mic armed") == 0) {
        return "Mic armed";
    }
    return "Idle";
}

static void draw_wifi(int x, int y, const snap *s) {
    int bars = s->link.up ? s->link.bars : 0;
    uint16_t col = s->link.up ? COL_TEXT : COL_RED;
    int i;
    char buf[12];
    for (i = 0; i < 4; i++) {
        int bh = 6 + i * 5;
        fill(x + i * 12, y + 22 - bh, 8, bh, i < bars ? col : COL_LINE);
    }
    if (!s->link.up) {
        snprintf(buf, sizeof(buf), "down");
    } else {
        snprintf(buf, sizeof(buf), "%d", s->link.rssi_dbm);
    }
    text_at(x + 52, y + 4, buf, 2, col);
}

static void draw_batt(int x, int y, const snap *s) {
    bool low = s->bat.present && s->bat.percent >= 0 && s->bat.percent < 15;
    uint16_t col = low ? COL_RED : COL_TEXT;
    int pct = s->bat.present ? s->bat.percent : 0;
    char buf[8];
    fill(x, y + 8, 34, 18, col);
    fill(x + 2, y + 10, 30, 14, COL_BG);
    fill(x + 34, y + 13, 4, 8, col);
    if (s->bat.present && pct > 0) {
        int inner = pct * 26 / 100;
        if (inner < 2) {
            inner = 2;
        }
        fill(x + 4, y + 12, inner, 10, col);
    }
    if (!s->bat.present) {
        snprintf(buf, sizeof(buf), "USB");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", s->bat.percent);
    }
    text_at(x + 44, y + 6, buf, 2, col);
}

static void chrome(const snap *s) {
    char left[40];
    char nb[IC_NAME_LEN + 1];
    const char *pill = pill_of(s);
    uint16_t bg = COL_SURFACE2;
    uint16_t fg = COL_TEXT;
    int tw;
    int pw;
    station_name(s, s->view.self_id, nb, sizeof(nb));
    fill(0, 0, PANEL_W, 70, COL_SURFACE);
    fill(0, 70, PANEL_W, 2, COL_LINE);
    snprintf(left, sizeof(left), "%s  %u", nb, (unsigned)s->view.self_id);
    text_fit(20, 24, 360, left, 2, COL_TEXT);
    if (!s->view.link_up || s->view.mic_tx || strcmp(pill, "Busy") == 0 || strcmp(pill, "Headset") == 0) {
        bg = COL_RED;
        fg = COL_TEXT;
    } else if (s->view.state == IC_ST_CALLING || strcmp(pill, "Mic armed") == 0) {
        bg = COL_AMBER;
        fg = COL_INK;
    } else if (s->view.state == IC_ST_PAGING || s->view.state == IC_ST_PAGE_RX) {
        bg = COL_TEAL;
        fg = COL_INK;
    } else if (s->view.state == IC_ST_CONNECTED) {
        bg = COL_GREEN;
        fg = COL_INK;
    }
    tw = text_px(pill, 2);
    pw = tw + 28;
    if (pw > 220) {
        pw = 220;
    }
    fill(390, 16, pw, 38, bg);
    text_fit(404, 24, pw - 20, pill, 2, fg);
    draw_wifi(690, 16, s);
    draw_batt(880, 14, s);
}

static void meter(int x, int y, int w, int h, uint32_t rms) {
    int inner = (int)w - 4;
    int level = rms >= 4000u ? inner : (int)((rms * (uint32_t)inner) / 4000u);
    if (inner < 1) {
        return;
    }
    fill(x, y, w, h, COL_SURFACE);
    fill(x + 2, y + 2, inner, h - 4, COL_BG);
    if (level > 0) {
        fill(x + 2, y + 2, level, h - 4, COL_GREEN);
    }
}

static void draw_cards(const snap *s, int y) {
    static const int xs[4] = {24, 272, 520, 768};
    int i;
    for (i = 0; i < 4; i++) {
        const ic_peer_view_t *p = &s->view.dir[i];
        bool self = p->id == s->view.self_id;
        bool sel = !self && p->id != 0 && p->id == s->view.selected;
        char nb[IC_NAME_LEN + 1];
        char idb[8];
        int x = xs[i];
        fill(x, y, 232, 168, sel ? COL_AMBER : COL_LINE);
        fill(x + 3, y + 3, 226, 162, COL_SURFACE);
        memcpy(nb, p->name, IC_NAME_LEN);
        nb[IC_NAME_LEN] = 0;
        text_fit(x + 16, y + 28, 200, nb[0] ? nb : "Station", 3, COL_TEXT);
        snprintf(idb, sizeof(idb), "%u", (unsigned)p->id);
        text_at(x + 16, y + 70, idb, 2, COL_MUTED);
        if (self) {
            text_at(x + 16, y + 112, "This station", 2, COL_AMBER);
        } else if (p->online) {
            text_at(x + 16, y + 112, "Online", 2, COL_GREEN);
        } else {
            text_at(x + 16, y + 112, "Offline", 2, COL_MUTED);
        }
        hit_add(x, y, 232, 168, ACT_SEL, (int)p->id);
    }
}

static void draw_home(const snap *s) {
    bool idle = s->view.state == IC_ST_IDLE;
    bool reception = s->view.self_id == IC_STATION_MIN;
    int mic_style = s->view.mic_tx ? 2 : (s->view.mic_armed ? 1 : 0);
    const char *mic = s->view.mic_tx ? "Mute" : (s->view.mic_armed ? "Armed" : "Mic off");
    char level[24];
    char line[IC_STATUS_LEN + 16];
    chrome(s);
    draw_cards(s, 88);
    draw_btn(40, 274, 176, 96, "Call", NULL, 1, ACT_CALL, 0);
    draw_btn(232, 274, 168, 96, mic, NULL, mic_style, ACT_MIC, 0);
    draw_btn(416, 274, 168, 96, "Page", reception ? NULL : "Locked", reception ? 0 : 3, ACT_PAGE, 0);
    draw_btn(600, 274, 200, 96, "Page all", reception ? NULL : "Locked", reception ? 0 : 3, ACT_ALL, 0);
    draw_btn(816, 274, 168, 96, "End", NULL, idle ? 4 : 2, idle ? 0 : ACT_END, 0);
    if (s->tone && idle) {
        snprintf(line, sizeof(line), "Local tone");
    } else {
        snprintf(line, sizeof(line), "%s", s->view.status);
    }
    text_fit(40, 390, 940, line, 2, COL_MUTED);
    snprintf(level, sizeof(level), "Speaker %u", (unsigned)s->view.volume);
    draw_btn(40, 448, 88, 72, "-", NULL, 0, ACT_VOL, -10);
    text_at(156, 470, level, 3, COL_TEXT);
    draw_btn(460, 448, 88, 72, "+", NULL, 0, ACT_VOL, 10);
    if (idle) {
        draw_btn(784, 448, 200, 72, "Setup", NULL, 0, ACT_SETUP, 0);
    }
}

static void draw_talk_end(const snap *s, const char *title, const char *who, bool mic_locked) {
    char clockb[16];
    unsigned sec = (unsigned)(s->age_ms / 1000u);
    chrome(s);
    text_center(0, 110, PANEL_W, title, 3, COL_AMBER);
    text_center(0, 170, PANEL_W, who, 4, COL_TEXT);
    snprintf(clockb, sizeof(clockb), "%u:%02u", sec / 60u, sec % 60u);
    text_center(0, 230, PANEL_W, clockb, 3, COL_MUTED);
    text_fit(48, 280, 920, s->view.status, 2, COL_MUTED);
    if (mic_locked) {
        draw_btn(48, 330, 440, 120, "Mic locked", NULL, 4, 0, 0);
    } else if (s->view.mic_tx) {
        draw_btn(48, 330, 440, 120, "Mute", NULL, 2, ACT_MIC, 0);
    } else {
        draw_btn(48, 330, 440, 120, "Talk", NULL, 1, ACT_MIC, 0);
    }
    draw_btn(536, 330, 440, 120, mic_locked ? "Leave" : "End", NULL, 2, ACT_END, 0);
    if (!mic_locked && (s->view.mic_tx || s->view.mic_armed)) {
        meter(48, 470, 440, 28, s->rms);
    }
    {
        char level[20];
        snprintf(level, sizeof(level), "Speaker %u", (unsigned)s->view.volume);
        draw_btn(620, 510, 80, 64, "-", NULL, 0, ACT_VOL, -10);
        text_at(720, 528, level, 2, COL_TEXT);
        draw_btn(900, 510, 80, 64, "+", NULL, 0, ACT_VOL, 10);
    }
}

static void who_label(const snap *s, char *dst, size_t cap) {
    char nb[IC_NAME_LEN + 1];
    uint16_t id = s->view.remote ? s->view.remote : s->view.selected;
    if (s->nlegs > 1 && s->view.state == IC_ST_PAGING) {
        snprintf(dst, cap, "All floors");
        return;
    }
    if (s->view.remote_name[0]) {
        snprintf(dst, cap, "%s", s->view.remote_name);
        return;
    }
    station_name(s, id, nb, sizeof(nb));
    snprintf(dst, cap, "%s", nb);
}

static void draw_splash(const snap *s, uint32_t now) {
    char sub[48];
    char nb[IC_NAME_LEN + 1];
    char wifi[48];
    char pack[32];
    int prog;
    int bw;
    uint16_t wifi_col = s->link.up ? COL_TEXT : COL_MUTED;
    uint16_t bat_col = (s->bat.present && s->bat.percent >= 0 && s->bat.percent < 15) ? COL_RED : COL_TEXT;
    station_name(s, s->view.self_id, nb, sizeof(nb));
    snprintf(sub, sizeof(sub), "%s   %u   %s", nb, (unsigned)s->view.self_id, s->ip);
    if (!s->link.up) {
        if (s->ssid[0]) {
            snprintf(wifi, sizeof(wifi), "joining  %s", s->ssid);
        } else {
            snprintf(wifi, sizeof(wifi), "not set");
        }
    } else {
        snprintf(wifi, sizeof(wifi), "%s   %d dBm", s->ssid[0] ? s->ssid : "up", s->link.rssi_dbm);
    }
    if (!s->bat.present) {
        snprintf(pack, sizeof(pack), "no pack");
    } else {
        snprintf(pack, sizeof(pack), "%d%%   %d.%02d V", s->bat.percent, s->bat.millivolts / 1000,
                 (s->bat.millivolts % 1000) / 10);
    }
    prog = 0;
    if (s_t0 != 0 && now >= s_t0) {
        prog = (int)((now - s_t0) * 100u / 4500u);
    }
    if (prog > 100) {
        prog = 100;
    }
    text_center(0, 64, PANEL_W, "LAN Intercom", 4, COL_TEXT);
    text_center(0, 124, PANEL_W, sub, 2, COL_MUTED);
    text_at(150, 210, "Panel", 3, COL_MUTED);
    text_at(460, 210, "ready", 3, COL_GREEN);
    text_at(150, 270, "Headset", 3, COL_MUTED);
    text_at(460, 270, (s->headset || s->view.audio_ok) ? "ready" : "waiting", 3,
            (s->headset || s->view.audio_ok) ? COL_GREEN : COL_AMBER);
    text_at(150, 330, "Wi-Fi", 3, COL_MUTED);
    text_fit(460, 330, 500, wifi, 3, wifi_col);
    text_at(150, 390, "Battery", 3, COL_MUTED);
    text_at(460, 390, pack, 3, bat_col);
    fill(150, 470, 724, 18, COL_SURFACE);
    bw = 724 * prog / 100;
    if (bw > 0) {
        fill(150, 470, bw, 18, COL_AMBER);
    }
    text_center(0, 520, PANEL_W, "Tap to continue", 2, COL_MUTED);
}

static void draw_setup(const snap *s) {
    char nb[IC_NAME_LEN + 1];
    char vbuf[16];
    char gbuf[16];
    char abuf[16];
    char idb[64];
    bool can_test = s->view.state == IC_ST_IDLE && s->headset && g_app.audio_ready;
    chrome(s);
    text_at(36, 96, "Speaker", 2, COL_TEXT);
    snprintf(vbuf, sizeof(vbuf), "%u", (unsigned)s->view.volume);
    text_at(360, 90, vbuf, 3, COL_AMBER);
    draw_btn(620, 84, 110, 64, "-", NULL, 0, ACT_VOL, -10);
    draw_btn(760, 84, 110, 64, "+", NULL, 0, ACT_VOL, 10);

    text_at(36, 176, "Mic gain", 2, COL_TEXT);
    snprintf(gbuf, sizeof(gbuf), "%u", (unsigned)s->view.mic_gain);
    text_at(360, 170, gbuf, 3, COL_AMBER);
    draw_btn(620, 164, 110, 64, "-", NULL, 0, ACT_GAIN, -5);
    draw_btn(760, 164, 110, 64, "+", NULL, 0, ACT_GAIN, 5);
    text_at(360, 214, "25 is about unity", 2, COL_MUTED);

    text_at(36, 268, "Echo delay", 2, COL_TEXT);
    snprintf(abuf, sizeof(abuf), "%d ms", s->aec_ms);
    text_at(360, 262, abuf, 3, COL_AMBER);
    draw_btn(620, 256, 110, 64, "-", NULL, 0, ACT_AEC, -20);
    draw_btn(760, 256, 110, 64, "+", NULL, 0, ACT_AEC, 20);

    text_at(36, 360, "Mic level", 2, COL_TEXT);
    meter(360, 352, 620, 32, s->rms);

    draw_btn(36, 420, 280, 72, s->tone ? "Playing" : "Audio test", NULL, can_test ? 1 : 4,
             can_test ? ACT_TEST : 0, 0);
    draw_btn(340, 420, 220, 72, "Power", NULL, 0, ACT_POWER, 0);
    draw_btn(760, 420, 200, 72, "Back", NULL, 0, ACT_BACK, 0);

    station_name(s, s->view.self_id, nb, sizeof(nb));
    snprintf(idb, sizeof(idb), "%s   %u   %s", nb, (unsigned)s->view.self_id, s->ip);
    text_fit(36, 512, 960, idb, 2, COL_MUTED);
    if (s->key_default) {
        text_fit(36, 548, 960, "Change the shared key on the console before real use.", 2, COL_AMBER);
    }
}

static void draw_power(const snap *s) {
    char big[16];
    char volt[24];
    char radio[80];
    bool low = s->bat.present && s->bat.percent >= 0 && s->bat.percent < 15;
    int pct = s->bat.present ? s->bat.percent : 0;
    chrome(s);
    text_center(0, 100, PANEL_W, "Power", 3, COL_MUTED);
    if (!s->bat.present) {
        text_center(0, 180, PANEL_W, "USB", 8, COL_TEXT);
        text_center(0, 280, PANEL_W, "No pack on the battery header", 2, COL_MUTED);
    } else {
        snprintf(big, sizeof(big), "%d%%", pct);
        text_center(0, 170, PANEL_W, big, 8, low ? COL_RED : COL_AMBER);
        snprintf(volt, sizeof(volt), "%d.%02d V", s->bat.millivolts / 1000, (s->bat.millivolts % 1000) / 10);
        text_center(0, 270, PANEL_W, volt, 3, COL_TEXT);
        fill(180, 340, 664, 24, COL_SURFACE);
        if (pct > 0) {
            fill(180, 340, 664 * pct / 100, 24, low ? COL_RED : COL_AMBER);
        }
    }
    if (!s->link.up) {
        snprintf(radio, sizeof(radio), "Wi-Fi down");
    } else {
        snprintf(radio, sizeof(radio), "%s    %d dBm    %d bars", s->ssid[0] ? s->ssid : "Wi-Fi",
                 s->link.rssi_dbm, s->link.bars);
    }
    text_center(0, 400, PANEL_W, radio, 2, s->link.up ? COL_TEXT : COL_RED);
    draw_btn(392, 480, 240, 72, "Back", NULL, 0, ACT_BACK, 1);
}

static void draw_link(const snap *s) {
    char line[64];
    chrome(s);
    text_center(0, 100, PANEL_W, "No Wi-Fi", 4, COL_RED);
    if (s->ssid[0]) {
        snprintf(line, sizeof(line), "Joining %s", s->ssid);
    } else {
        snprintf(line, sizeof(line), "No network name stored");
    }
    text_center(0, 170, PANEL_W, line, 2, COL_TEXT);
    text_center(0, 214, PANEL_W, "Check the access point. Calls are stopped.", 2, COL_MUTED);
    draw_cards(s, 260);
    draw_btn(784, 460, 200, 72, "Setup", NULL, 0, ACT_SETUP, 0);
}

static void draw_audio(const snap *s) {
    chrome(s);
    text_center(0, 150, PANEL_W, "Headset not ready", 4, COL_RED);
    text_center(0, 230, PANEL_W, "Plug the adapter into the USB socket, not UART.", 2, COL_TEXT);
    text_center(0, 270, PANEL_W, "Calls stay blocked until the headset streams.", 2, COL_MUTED);
    draw_btn(392, 360, 240, 80, "Setup", NULL, 0, ACT_SETUP, 0);
}

static void draw_busy(const snap *s) {
    char nb[IC_NAME_LEN + 1];
    char line[48];
    uint16_t id = s->view.selected ? s->view.selected : s->view.remote;
    chrome(s);
    text_center(0, 88, PANEL_W, "Busy", 4, COL_RED);
    station_name(s, id, nb, sizeof(nb));
    snprintf(line, sizeof(line), "%s did not take the call", nb);
    text_center(0, 150, PANEL_W, line, 2, COL_TEXT);
    draw_cards(s, 200);
    draw_btn(392, 460, 240, 72, "Back", NULL, 0, ACT_BACK, 0);
}

static int pick_screen(const snap *s) {
    if (s_splash) {
        return SCR_SPLASH;
    }
    if (s->view.state != IC_ST_IDLE) {
        s_nav = NAV_AUTO;
    }
    if (s->view.state == IC_ST_CALLING) {
        return SCR_CALLING;
    }
    if (s->view.state == IC_ST_CONNECTED) {
        return SCR_CALL;
    }
    if (s->view.state == IC_ST_PAGING) {
        return SCR_PAGE;
    }
    if (s->view.state == IC_ST_PAGE_RX) {
        return SCR_RX;
    }
    if (s_nav == NAV_SETUP) {
        return SCR_SETUP;
    }
    if (s_nav == NAV_POWER) {
        return SCR_POWER;
    }
    if (!s->view.link_up) {
        return SCR_LINK;
    }
    if (!s->view.audio_ok) {
        return SCR_AUDIO;
    }
    if (!s_dismiss && strcmp(s->view.status, "Busy") == 0) {
        return SCR_BUSY;
    }
    return SCR_HOME;
}

static void splash_tick(uint32_t now, const snap *s) {
    uint32_t age;
    if (!s_splash || s_t0 == 0) {
        return;
    }
    age = now - s_t0;
    if (age >= 4500u) {
        s_splash = false;
    } else if (s_skip && age >= 700u) {
        s_splash = false;
    } else if (age >= 2000u && s->view.link_up) {
        s_splash = false;
    }
}

static void fill_sig(const snap *s, int screen, paint_sig *sig) {
    int i;
    memset(sig, 0, sizeof(*sig));
    sig->screen = screen;
    sig->age_sec = (int)(s->age_ms / 1000u);
    sig->state = s->view.state;
    sig->self_id = s->view.self_id;
    sig->selected = s->view.selected;
    sig->remote = s->view.remote;
    sig->volume = s->view.volume;
    sig->mic_gain = s->view.mic_gain;
    sig->mic_armed = s->view.mic_armed ? 1 : 0;
    sig->mic_tx = s->view.mic_tx ? 1 : 0;
    sig->link_up = s->view.link_up ? 1 : 0;
    sig->audio_ok = s->view.audio_ok ? 1 : 0;
    for (i = 0; i < 4; i++) {
        sig->online[i] = s->view.dir[i].online ? 1 : 0;
    }
    sig->bars = s->link.bars;
    sig->rssi = s->link.rssi_dbm;
    sig->bat_pct = s->bat.percent;
    sig->bat_mv = s->bat.millivolts;
    sig->aec_ms = s->aec_ms;
    sig->nlegs = s->nlegs;
    sig->headset = s->headset ? 1 : 0;
    sig->tone = s->tone ? 1 : 0;
    sig->key_default = s->key_default ? 1 : 0;
    sig->splash = s_splash ? 1 : 0;
    memcpy(sig->status, s->view.status, IC_STATUS_LEN);
    memcpy(sig->remote_name, s->view.remote_name, IC_NAME_LEN);
    memcpy(sig->ssid, s->ssid, sizeof(sig->ssid));
    memcpy(sig->ip, s->ip, sizeof(sig->ip));
}

static void paint(const snap *s, int screen, uint32_t now) {
    char who[IC_NAME_LEN + 8];
    s_px = panel_7b_canvas();
    if (!s_px) {
        return;
    }
    hits_clear();
    clear_screen(COL_BG);
    switch (screen) {
    case SCR_SPLASH:
        draw_splash(s, now);
        break;
    case SCR_CALLING:
        who_label(s, who, sizeof(who));
        chrome(s);
        text_center(0, 140, PANEL_W, "Calling", 3, COL_AMBER);
        text_center(0, 210, PANEL_W, who, 4, COL_TEXT);
        {
            char clockb[16];
            unsigned sec = (unsigned)(s->age_ms / 1000u);
            snprintf(clockb, sizeof(clockb), "%u s", sec);
            text_center(0, 280, PANEL_W, clockb, 3, COL_MUTED);
        }
        text_fit(80, 340, 860, s->view.status, 2, COL_MUTED);
        draw_btn(312, 400, 400, 110, "End", NULL, 2, ACT_END, 0);
        break;
    case SCR_CALL:
        who_label(s, who, sizeof(who));
        draw_talk_end(s, "In call", who, false);
        break;
    case SCR_PAGE:
        who_label(s, who, sizeof(who));
        draw_talk_end(s, "Paging", who, false);
        break;
    case SCR_RX:
        who_label(s, who, sizeof(who));
        draw_talk_end(s, "Page", who, true);
        break;
    case SCR_BUSY:
        draw_busy(s);
        break;
    case SCR_LINK:
        draw_link(s);
        break;
    case SCR_AUDIO:
        draw_audio(s);
        break;
    case SCR_SETUP:
        draw_setup(s);
        break;
    case SCR_POWER:
        draw_power(s);
        break;
    default:
        draw_home(s);
        break;
    }
    panel_7b_present();
}

static bool animate(int screen, const snap *s) {
    if (s_splash || screen == SCR_SETUP) {
        return true;
    }
    if (screen == SCR_CALL && s->view.mic_tx) {
        return true;
    }
    return false;
}

static void render(uint32_t now) {
    snap cur;
    paint_sig sig;
    int screen;
    bool changed;
    if (!panel_7b_ok()) {
        return;
    }
    take_snap(now, &cur);
    if (strcmp(cur.view.status, "Busy") != 0) {
        s_dismiss = false;
    }
    splash_tick(now, &cur);
    screen = pick_screen(&cur);
    fill_sig(&cur, screen, &sig);
    changed = !s_have || memcmp(&sig, &s_sig, sizeof(sig)) != 0;
    if (!s_force && !changed && !(animate(screen, &cur) && (now - s_drawn_ms) >= 250u)) {
        return;
    }
    paint(&cur, screen, now);
    s_sig = sig;
    s_have = true;
    s_force = false;
    s_drawn_ms = now;
}

void ui_paint_banner(void) {
    if (!panel_7b_ok()) {
        return;
    }
    s_px = panel_7b_canvas();
    if (!s_px) {
        return;
    }
    clear_screen(COL_BG);
    text_center(0, 220, PANEL_W, "LAN Intercom", 4, COL_TEXT);
    text_center(0, 300, PANEL_W, "Starting", 3, COL_AMBER);
    panel_7b_present();
}

void ui_paint_boot(void) {
    if (!panel_7b_ok()) {
        return;
    }
    s_t0 = app_now_ms();
    s_splash = true;
    s_skip = false;
    s_nav = NAV_AUTO;
    s_force = true;
    render(s_t0);
}

void ui_paint_poll(uint32_t now_ms) {
    int x = 0;
    int y = 0;
    bool down;
    if (!panel_7b_ok()) {
        return;
    }
    if (s_t0 == 0) {
        s_t0 = now_ms;
        s_splash = true;
        s_force = true;
    }
    down = panel_7b_touch(&x, &y);
    if (down) {
        s_up_run = 0;
        if (!s_finger) {
            s_finger = true;
            if ((uint32_t)(now_ms - s_hit_ms) > 200u) {
                s_hit_ms = now_ms;
                if (s_splash) {
                    s_skip = true;
                    s_force = true;
                } else {
                    dispatch(x, y);
                }
            }
        }
    } else if (s_finger) {
        s_up_run++;
        if (s_up_run >= 6) {
            s_finger = false;
        }
    }
    render(now_ms);
}
