#include "ui_lvgl.h"

#include "app.h"
#include "audio_port.h"
#include "config_store.h"
#include "lvgl_port.h"
#include "net_arduino.h"
#include "panel_7b.h"
#include "ui_port.h"
#include "usb_uac.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"

/* One LVGL view per frame in design/screens, including 15-wifi.svg.
 * The shared key and the web PIN are not on this panel. */

static const char *TAG = "ic_lvgl";

static const uint32_t C_BG = 0x14110E;
static const uint32_t C_BAR = 0x100E0C;
static const uint32_t C_SURF = 0x221C17;
static const uint32_t C_SURF2 = 0x2C251E;
static const uint32_t C_LINE = 0x4A3E34;
static const uint32_t C_TEXT = 0xF6F1E8;
static const uint32_t C_MUTED = 0xA89888;
static const uint32_t C_AMBER = 0xE4A04A;
static const uint32_t C_INK = 0x2A1C0A;
static const uint32_t C_GREEN = 0x8FB573;
static const uint32_t C_RED = 0xD4654E;
static const uint32_t C_TEAL = 0x7EAEA4;
static const uint32_t C_PRESS = 0x3A312A;
static const uint32_t C_TEAL_BG = 0x1C2C2A;
static const uint32_t C_CALL_BG = 0x243024;
static const uint32_t C_WARM = 0x3A2C18;
static const uint32_t C_ALERT = 0x3A221C;
static const uint32_t C_INK_RED = 0x2A100C;
static const uint32_t C_SUB_RED = 0x6A2E24;
static const uint32_t C_SUB_AMBER = 0x6A4A16;
static const uint32_t C_OFF = 0x6A5C50;
static const uint32_t C_DIM = 0x5A4E44;
static const uint32_t C_TEAL_INK = 0x4E726C;
static const uint32_t C_CREAM = 0xE6D7C3;
static const uint32_t C_KEY = 0x2C251E;
static const uint32_t C_KEY_INK = 0xF6F1E8;

enum {
    SCR_SPLASH = 0,
    SCR_HOME,
    SCR_FLOOR,
    SCR_CALLING,
    SCR_MUTED,
    SCR_LIVE,
    SCR_PAGE,
    SCR_PAGEALL,
    SCR_RX,
    SCR_BUSY,
    SCR_LINK,
    SCR_AUDIO,
    SCR_SETUP,
    SCR_POWER,
    SCR_WIFI,
    SCR_NET,
    SCR_COUNT
};

enum { NAV_AUTO = 0, NAV_SETUP, NAV_POWER, NAV_WIFI, NAV_NET };
enum { NET_NAME = 0, NET_IP, NET_GW, NET_MASK, NET_P0, NET_P1, NET_P2, NET_P3, NET_N };
enum { T_SURFACE = 0, T_AMBER, T_RED, T_LOCKED, T_DISABLED, T_DIM, T_TEAL };

struct Snap {
    ic_view_t view;
    uint32_t age_ms;
    uint32_t rms;
    int aec_ms;
    int nlegs;
    bool key_default;
    bool pin_default;
    bool pass_set;
    bool headset;
    bool tone;
    bool audio_ready;
    char ssid[33];
    char ip[16];
    char gw[16];
    char mask[16];
    ui_link_t link;
    ui_battery_t bat;
};

struct Sig {
    int screen;
    int age_sec;
    int state;
    int self_id;
    int selected;
    int remote;
    int volume;
    int gain;
    int aec_ms;
    int bars;
    int rssi;
    int bat_pct;
    int bat_mv;
    int nlegs;
    int online[4];
    uint8_t mic_armed;
    uint8_t mic_tx;
    uint8_t link_up;
    uint8_t audio_ok;
    uint8_t headset;
    uint8_t tone;
    uint8_t key_default;
    uint8_t pin_default;
    uint8_t pass_set;
    uint8_t speaker;
    char status[IC_STATUS_LEN];
    char remote_name[IC_NAME_LEN + 1];
    char ssid[33];
    char ip[16];
    char gw[16];
    char mask[16];
    char names[4][IC_NAME_LEN + 1];
    char ips[4][16];
};

struct Btn {
    lv_obj_t *btn;
    lv_obj_t *title;
    lv_obj_t *sub;
};

struct Card {
    lv_obj_t *btn;
    lv_obj_t *dot;
    lv_obj_t *tag;
    lv_obj_t *name;
    lv_obj_t *ident;
    lv_obj_t *sub;
};

static bool s_built = false;
static bool s_booted = false;
static bool s_splash = true;
static bool s_skip = false;
static bool s_dismiss = false;
static bool s_have = false;
static bool s_on_app = false;
static bool s_quiet = false;
static bool s_wifi_filled = false;
static bool s_net_filled = false;
static bool s_pass_hide = false;
static int s_edit_sid = IC_STATION_MIN;
static int s_net_active = -1;
static int s_net_paint_active = -2;
static int s_net_paint_sid = -1;
static char s_net_draft[NET_N][40];
static int s_nav = NAV_AUTO;
static int s_cur = -1;
static uint32_t s_t0 = 0;
static uint32_t s_anim = 0;
static uint16_t s_alt_id[2];
static Sig s_sig;

static lv_obj_t *s_splash_scr;
static lv_obj_t *s_app;
static lv_obj_t *s_views[SCR_COUNT];
static lv_obj_t *s_name;
static lv_obj_t *s_idlbl;
static lv_obj_t *s_pill;
static lv_obj_t *s_pill_lbl;
static lv_obj_t *s_bars[4];
static lv_obj_t *s_rssi;
static lv_obj_t *s_bat_fill;
static lv_obj_t *s_bat_lbl;

static lv_obj_t *s_splash_sub;
static lv_obj_t *s_splash_kick;
static lv_obj_t *s_splash_key[4];
static lv_obj_t *s_splash_val[4];
static lv_obj_t *s_splash_fill;
static lv_obj_t *s_ring[3];

static Card s_home_card[4];
static Card s_floor_card[4];
static Btn s_call_b, s_mic_b, s_page_b, s_all_b, s_end_b;
static Btn s_fcall, s_fmic, s_fpage, s_fend;
static lv_obj_t *s_home_slider;
static lv_obj_t *s_home_vol;
static lv_obj_t *s_home_status;
static lv_obj_t *s_home_headset;
static lv_obj_t *s_floor_title;
static lv_obj_t *s_floor_bat;

static lv_obj_t *s_call_name;
static lv_obj_t *s_call_sub;
static lv_obj_t *s_call_radio;
static Btn s_call_end;

static lv_obj_t *s_mu_name;
static lv_obj_t *s_mu_clock;
static lv_obj_t *s_mu_hear;
static lv_obj_t *s_mu_vol;
static lv_obj_t *s_mu_slider;
static lv_obj_t *s_mu_gain;
static Btn s_mu_talk;
static Btn s_mu_end;

static lv_obj_t *s_live_name;
static lv_obj_t *s_live_clock;
static lv_obj_t *s_live_bar[16];
static Btn s_live_mic;
static Btn s_live_end;

static lv_obj_t *s_pg_title;
static Card s_pg_card[3];
static Btn s_pg_talk;
static Btn s_pg_end;
static Btn s_pg_all;
static lv_obj_t *s_pg_foot;

static lv_obj_t *s_pa_title;
static Card s_pa_card[3];
static Btn s_pa_talk;
static Btn s_pa_end;
static lv_obj_t *s_pa_foot;

static lv_obj_t *s_rx_title;
static lv_obj_t *s_rx_sub;
static lv_obj_t *s_rx_foot;
static Btn s_rx_lock;

static lv_obj_t *s_busy_title;
static lv_obj_t *s_busy_line;
static Btn s_busy_alt[2];

static lv_obj_t *s_link_ssid;
static lv_obj_t *s_link_msg;
static lv_obj_t *s_link_foot;
static lv_obj_t *s_link_name[4];
static lv_obj_t *s_link_sub[4];

static lv_obj_t *s_au_foot;

static lv_obj_t *s_setup_vol;
static lv_obj_t *s_setup_gain;
static lv_obj_t *s_setup_slider_v;
static lv_obj_t *s_setup_slider_g;
static lv_obj_t *s_aec_lbl;
static lv_obj_t *s_aec_side;
static lv_obj_t *s_setup_meter;
static lv_obj_t *s_setup_kb;
static lv_obj_t *s_setup_active_ta;
static lv_obj_t *s_id_name;
static lv_obj_t *s_id_num;
static lv_obj_t *s_id_ip;
static lv_obj_t *s_id_gw;
static lv_obj_t *s_id_mask;
static lv_obj_t *s_warn;

static lv_obj_t *s_pwr_pct;
static lv_obj_t *s_pwr_volt;
static lv_obj_t *s_pwr_fill;
static lv_obj_t *s_pwr_ssid;
static lv_obj_t *s_pwr_rssi;
static lv_obj_t *s_pwr_pass;
static lv_obj_t *s_pwr_name[4];
static lv_obj_t *s_pwr_ip[4];
static lv_obj_t *s_pwr_state[4];

static lv_obj_t *s_ssid_ta;
static lv_obj_t *s_pass_ta;
static lv_obj_t *s_kb;
static lv_obj_t *s_wifi_err;
static lv_obj_t *s_hide_lbl;
static lv_obj_t *s_net_ta;
static lv_obj_t *s_net_kb;
static lv_obj_t *s_net_cap;
static lv_obj_t *s_net_err;
static Btn s_net_field[NET_N];
static Btn s_net_sid[4];

static void set_text(lv_obj_t *lb, const char *text) {
    const char *cur;
    if (!lb) {
        return;
    }
    if (!text) {
        text = "";
    }
    cur = lv_label_get_text(lb);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(lb, text);
}

static void name_of(const char raw[IC_NAME_LEN], char *dst, size_t cap) {
    size_t n = 0;
    if (!dst || cap == 0) {
        return;
    }
    if (!raw) {
        dst[0] = 0;
        return;
    }
    while (n < IC_NAME_LEN && n + 1 < cap && raw[n] != 0) {
        n++;
    }
    memcpy(dst, raw, n);
    dst[n] = 0;
}

static void upper_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    for (; src[i] && i + 1 < cap; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        dst[i] = c;
    }
    dst[i] = 0;
}

static void plain(lv_obj_t *o) {
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(o, 0, LV_STATE_FOCUS_KEY);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *label_at(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color,
                          const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void clip_label(lv_obj_t *l, int w) {
    if (!l) {
        return;
    }
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

static void show(lv_obj_t *o, bool on) {
    if (!o) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *card_at(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    plain(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    if (radius >= 16) {
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_set_style_shadow_color(o, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_ofs_y(o, 4, 0);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void stroke(lv_obj_t *o, uint32_t color, int width) {
    if (!o) {
        return;
    }
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}

static void style_slider(lv_obj_t *sl) {
    lv_slider_set_range(sl, 0, 100);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_PRESS), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 8, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_ver(sl, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(sl, 0, 0);
}

static void tone(const Btn *b, int kind) {
    uint32_t bg = C_SURF2;
    uint32_t fg = C_TEXT;
    uint32_t sg = C_MUTED;
    uint32_t press = C_PRESS;
    uint32_t border = C_LINE;
    int width = 1;
    bool click = true;
    if (!b || !b->btn) {
        return;
    }
    if (kind == T_AMBER) {
        bg = C_AMBER;
        fg = C_INK;
        sg = C_SUB_AMBER;
        press = 0xC88436;
        border = C_AMBER;
    } else if (kind == T_RED) {
        bg = C_RED;
        fg = C_INK_RED;
        sg = C_SUB_RED;
        press = 0xA94D3C;
        border = C_RED;
    } else if (kind == T_LOCKED) {
        bg = C_SURF;
        fg = C_MUTED;
        sg = C_OFF;
        press = C_PRESS;
        border = C_LINE;
    } else if (kind == T_DISABLED) {
        bg = C_SURF;
        fg = C_OFF;
        sg = C_DIM;
        press = C_SURF;
        border = C_LINE;
        click = false;
    } else if (kind == T_DIM) {
        bg = C_SURF;
        fg = C_OFF;
        sg = C_DIM;
        press = C_PRESS;
        border = C_LINE;
    } else if (kind == T_TEAL) {
        bg = C_TEAL_BG;
        fg = C_TEAL;
        sg = C_TEAL_INK;
        press = 0x243834;
        border = C_TEAL;
        width = 2;
    }
    lv_obj_set_style_bg_color(b->btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b->btn, lv_color_hex(press), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b->btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b->btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b->btn, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(b->btn, width, 0);
    if (b->title) {
        lv_obj_set_style_text_color(b->title, lv_color_hex(fg), 0);
    }
    if (b->sub) {
        lv_obj_set_style_text_color(b->sub, lv_color_hex(sg), 0);
    }
    if (click) {
        lv_obj_add_flag(b->btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(b->btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void relabel(const Btn *b, const char *title, const char *sub) {
    if (!b) {
        return;
    }
    set_text(b->title, title ? title : "");
    set_text(b->sub, sub ? sub : "");
    if (b->sub) {
        show(b->sub, sub && sub[0]);
    }
}

static Btn make_btn(lv_obj_t *parent, int x, int y, int w, int h, const char *title, const char *sub,
                    lv_event_cb_t cb, void *user) {
    Btn b;
    b.btn = lv_btn_create(parent);
    lv_obj_set_pos(b.btn, x, y);
    lv_obj_set_size(b.btn, w, h);
    plain(b.btn);
    lv_obj_set_style_radius(b.btn, 16, 0);
    lv_obj_set_style_bg_opa(b.btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b.btn, 0, 0);
    lv_obj_set_style_shadow_color(b.btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(b.btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_ofs_y(b.btn, 3, 0);
    lv_obj_set_flex_flow(b.btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b.btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b.btn, 2, 0);
    b.title = lv_label_create(b.btn);
    lv_obj_set_style_text_font(b.title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(b.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(b.title, title ? title : "");
    lv_obj_set_width(b.title, w - 16);
    lv_label_set_long_mode(b.title, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(b.title, LV_OBJ_FLAG_CLICKABLE);
    b.sub = lv_label_create(b.btn);
    lv_obj_set_style_text_font(b.sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(b.sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(b.sub, sub ? sub : "");
    lv_obj_set_width(b.sub, w - 16);
    lv_label_set_long_mode(b.sub, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(b.sub, LV_OBJ_FLAG_CLICKABLE);
    if (!sub || sub[0] == 0) {
        lv_obj_add_flag(b.sub, LV_OBJ_FLAG_HIDDEN);
    }
    tone(&b, T_SURFACE);
    if (cb) {
        lv_obj_add_event_cb(b.btn, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

static lv_obj_t *dot_at(lv_obj_t *parent, int x, int y, int s) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, s, s);
    plain(d);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    return d;
}

static Card make_station(lv_obj_t *parent, int x, int y) {
    Card c;
    memset(&c, 0, sizeof(c));
    c.btn = lv_btn_create(parent);
    lv_obj_set_pos(c.btn, x, y);
    lv_obj_set_size(c.btn, 232, 188);
    plain(c.btn);
    lv_obj_set_style_radius(c.btn, 18, 0);
    lv_obj_set_style_bg_opa(c.btn, LV_OPA_COVER, 0);
    c.dot = dot_at(c.btn, 20, 22, 10);
    c.tag = label_at(c.btn, 38, 16, &lv_font_montserrat_14, C_GREEN, "ONLINE");
    lv_obj_set_style_text_letter_space(c.tag, 1, 0);
    c.name = label_at(c.btn, 20, 56, &lv_font_montserrat_28, C_TEXT, "Station");
    clip_label(c.name, 192);
    c.ident = label_at(c.btn, 20, 100, &lv_font_montserrat_16, C_MUTED, "100");
    c.sub = label_at(c.btn, 20, 136, &lv_font_montserrat_14, C_MUTED, "");
    clip_label(c.sub, 192);
    return c;
}

static void bind_station(Card *c, uint16_t id, lv_event_cb_t cb) {
    if (c && c->btn && cb) {
        lv_obj_add_event_cb(c->btn, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    }
}

static Card make_info_card(lv_obj_t *parent, int x, int y, int w, int h) {
    Card c;
    memset(&c, 0, sizeof(c));
    c.btn = card_at(parent, x, y, w, h, C_SURF, 18);
    stroke(c.btn, C_LINE, 1);
    c.dot = dot_at(c.btn, 20, 22, 10);
    c.tag = label_at(c.btn, 38, 19, &lv_font_montserrat_14, C_GREEN, "ONLINE");
    lv_obj_set_style_text_letter_space(c.tag, 1, 0);
    c.name = label_at(c.btn, 20, 58, &lv_font_montserrat_28, C_TEXT, "");
    clip_label(c.name, w - 40);
    c.ident = label_at(c.btn, 20, 106, &lv_font_montserrat_16, C_MUTED, "");
    c.sub = label_at(c.btn, 20, 137, &lv_font_montserrat_14, C_MUTED, "");
    clip_label(c.sub, w - 40);
    return c;
}

static void sync_slider(lv_obj_t *sl, int value) {
    if (!sl || lv_obj_has_state(sl, LV_STATE_PRESSED)) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    if (lv_slider_get_value(sl) == value) {
        return;
    }
    s_quiet = true;
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    s_quiet = false;
}

static void save_cfg(void) {
    ic_config_t copy;
    if (!g_app.lock) {
        return;
    }
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    if (!ic_config_save(&copy)) {
        ESP_LOGW(TAG, "panel level was not saved");
    }
}

static void apply_volume(int value, bool save) {
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    if (!g_app.lock) {
        return;
    }
    app_lock();
    ic_core_set_volume(&g_app.core, (uint8_t)value);
    g_app.cfg.volume = (uint8_t)value;
    app_unlock();
    if (save) {
        save_cfg();
    }
}

static void apply_gain(int value, bool save) {
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    if (!g_app.lock) {
        return;
    }
    app_lock();
    ic_core_set_mic_gain(&g_app.core, (uint8_t)value);
    g_app.cfg.mic_gain = (uint8_t)value;
    app_unlock();
    if (save) {
        save_cfg();
    }
}

static void apply_aec(int ms) {
    ic_state_t st;
    if (ms < 0) {
        ms = 0;
    }
    if (ms > 180) {
        ms = 180;
    }
    ms = (ms / 20) * 20;
    if (!g_app.lock) {
        return;
    }
    app_lock();
    g_app.cfg.aec_samples = (uint16_t)(ms * 8);
    st = ic_core_state(&g_app.core);
    app_unlock();
    if (st == IC_ST_IDLE && g_app.audio_ready) {
        audio_set_aec_delay(ms * 8);
    }
    save_cfg();
}

static void on_select(lv_event_t *e) {
    uintptr_t id;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    id = (uintptr_t)lv_event_get_user_data(e);
    ui_select((uint16_t)id);
    s_dismiss = true;
}

static void on_call(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_call();
}

static void on_end(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_end();
}

static void on_mic(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_mic();
}

static void on_page(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_page(false);
}

static void on_page_all(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_page(true);
}

static void on_setup(lv_event_t *e) {
    ic_state_t st;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !g_app.lock) {
        return;
    }
    app_lock();
    st = ic_core_state(&g_app.core);
    app_unlock();
    if (st == IC_ST_IDLE) {
        s_nav = NAV_SETUP;
    }
}

static void on_power(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_nav = NAV_POWER;
}

static void on_wifi_nav(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_nav = NAV_WIFI;
}

static void on_net_nav(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_nav = NAV_NET;
}

static void on_back_setup(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_nav = NAV_SETUP;
    s_wifi_filled = false;
    s_net_filled = false;
    s_net_active = -1;
}

static void on_back_home(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_nav = NAV_AUTO;
    s_dismiss = true;
}

static void on_busy_pick(lv_event_t *e) {
    int idx;
    uint16_t id;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 1) {
        return;
    }
    id = s_alt_id[idx];
    if (id < IC_STATION_MIN || id > IC_STATION_MAX) {
        return;
    }
    ui_select(id);
    s_dismiss = true;
}

static void on_test(lv_event_t *e) {
    ic_state_t st;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !g_app.lock) {
        return;
    }
    app_lock();
    st = ic_core_state(&g_app.core);
    app_unlock();
    if (!g_app.audio_ready || !usb_uac_streaming() || st != IC_ST_IDLE) {
        return;
    }
    g_app.tone_frames = 150;
}

static void on_aec_step(lv_event_t *e) {
    int ms;
    intptr_t delta;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !g_app.lock) {
        return;
    }
    delta = (intptr_t)lv_event_get_user_data(e);
    app_lock();
    ms = (int)g_app.cfg.aec_samples / 8;
    app_unlock();
    apply_aec(ms + (int)delta);
}

static void setup_field_text(lv_obj_t *field, const char *value) {
    if (field && field != s_setup_active_ta) {
        lv_textarea_set_text(field, value ? value : "");
    }
}

static void vol_labels(int value) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    set_text(s_home_vol, buf);
    setup_field_text(s_setup_vol, buf);
    set_text(s_mu_vol, buf);
}

static void gain_label(int value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", value);
    setup_field_text(s_setup_gain, buf);
    snprintf(buf, sizeof(buf), "MIC GAIN %d", value);
    set_text(s_mu_gain, buf);
}

static void aec_label(int ms) {
    char value[12];
    char caption[24];
    snprintf(value, sizeof(value), "%d", ms);
    snprintf(caption, sizeof(caption), "%d ms", ms);
    setup_field_text(s_aec_lbl, value);
    set_text(s_aec_side, caption);
}

static void close_setup_keyboard(void) {
    int volume;
    int gain;
    int aec_ms;
    show(s_setup_kb, false);
    s_setup_active_ta = NULL;
    if (!g_app.lock) {
        return;
    }
    app_lock();
    volume = g_app.cfg.volume;
    gain = g_app.cfg.mic_gain;
    aec_ms = (int)g_app.cfg.aec_samples / 8;
    app_unlock();
    vol_labels(volume);
    gain_label(gain);
    aec_label(aec_ms);
}

static void on_setup_field(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!ta || (code != LV_EVENT_CLICKED && code != LV_EVENT_FOCUSED) || !s_setup_kb) {
        return;
    }
    s_setup_active_ta = ta;
    lv_keyboard_set_mode(s_setup_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(s_setup_kb, ta);
    show(s_setup_kb, true);
    lv_obj_move_foreground(s_setup_kb);
}

static void on_setup_keyboard(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = s_setup_active_ta;
    int value;
    if (code == LV_EVENT_CANCEL) {
        close_setup_keyboard();
        return;
    }
    if (code != LV_EVENT_READY || !ta) {
        return;
    }
    value = atoi(lv_textarea_get_text(ta));
    show(s_setup_kb, false);
    s_setup_active_ta = NULL;
    if (ta == s_setup_vol) {
        if (value > 100) value = 100;
        if (value < 0) value = 0;
        apply_volume(value, true);
        sync_slider(s_setup_slider_v, value);
        sync_slider(s_home_slider, value);
        sync_slider(s_mu_slider, value);
        vol_labels(value);
    } else if (ta == s_setup_gain) {
        if (value > 100) value = 100;
        if (value < 0) value = 0;
        apply_gain(value, true);
        sync_slider(s_setup_slider_g, value);
        gain_label(value);
    } else if (ta == s_aec_lbl) {
        if (value > 180) value = 180;
        if (value < 0) value = 0;
        apply_aec(value);
        aec_label((value / 20) * 20);
    }
}

static void on_volume(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int value;
    if (s_quiet) {
        return;
    }
    value = lv_slider_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_VALUE_CHANGED) {
        apply_volume(value, false);
        vol_labels(value);
    } else if (code == LV_EVENT_RELEASED) {
        apply_volume(value, true);
    }
}

static void on_gain(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int value;
    if (s_quiet) {
        return;
    }
    value = lv_slider_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_VALUE_CHANGED) {
        apply_gain(value, false);
        gain_label(value);
    } else if (code == LV_EVENT_RELEASED) {
        apply_gain(value, true);
    }
}

static void style_keyboard(lv_obj_t *kb) {
    if (!kb) {
        return;
    }
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x100E0C), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_row(kb, 4, 0);
    lv_obj_set_style_pad_column(kb, 4, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(C_KEY_INK), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x4A3E34),
                             (lv_style_selector_t)((unsigned)LV_PART_ITEMS | (unsigned)LV_STATE_PRESSED));
    lv_obj_set_style_bg_color(kb, lv_color_hex(C_AMBER),
                             (lv_style_selector_t)((unsigned)LV_PART_ITEMS | (unsigned)LV_STATE_CHECKED));
    lv_obj_set_style_text_color(kb, lv_color_hex(C_INK),
                               (lv_style_selector_t)((unsigned)LV_PART_ITEMS | (unsigned)LV_STATE_CHECKED));
    lv_keyboard_set_popovers(kb, true);
}

static void style_ta(lv_obj_t *ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x2C251E), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_pad_ver(ta, 12, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_TEXT), LV_PART_CURSOR);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_outline_width(ta, 0, 0);
}

static void wifi_error(const char *text, uint32_t color) {
    set_text(s_wifi_err, text ? text : "");
    if (s_wifi_err) {
        lv_obj_set_style_text_color(s_wifi_err, lv_color_hex(color), 0);
    }
}

static void wifi_commit(void) {
    ic_config_t copy;
    char ssid[33];
    char pass[64];
    const char *raw_ssid;
    const char *raw_pass;
    if (!s_ssid_ta || !s_pass_ta || !g_app.lock) {
        return;
    }
    raw_ssid = lv_textarea_get_text(s_ssid_ta);
    raw_pass = lv_textarea_get_text(s_pass_ta);
    snprintf(ssid, sizeof(ssid), "%s", raw_ssid ? raw_ssid : "");
    snprintf(pass, sizeof(pass), "%s", raw_pass ? raw_pass : "");
    if (!ic_config_ssid_ok(ssid)) {
        wifi_error("Name must be 1 to 32 characters.", C_RED);
        return;
    }
    if (!ic_config_wifi_pass_ok(pass)) {
        wifi_error("Password must be empty, or 8 to 63 characters.", C_RED);
        return;
    }
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    snprintf(copy.ssid, sizeof(copy.ssid), "%s", ssid);
    snprintf(copy.wifi_pass, sizeof(copy.wifi_pass), "%s", pass);
    if (!ic_config_save(&copy)) {
        wifi_error("Could not save.", C_RED);
        return;
    }
    app_lock();
    memcpy(g_app.cfg.ssid, copy.ssid, sizeof(g_app.cfg.ssid));
    memcpy(g_app.cfg.wifi_pass, copy.wifi_pass, sizeof(g_app.cfg.wifi_pass));
    app_unlock();
    if (net_join_wifi(&copy) != 0) {
        wifi_error("Saved, but Wi-Fi did not restart.", C_RED);
        return;
    }
    wifi_error("Saved. Joining.", C_GREEN);
}

static void on_wifi_save(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    wifi_commit();
}

static void on_hide(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_pass_ta) {
        return;
    }
    s_pass_hide = !s_pass_hide;
    lv_textarea_set_password_mode(s_pass_ta, s_pass_hide);
    set_text(s_hide_lbl, s_pass_hide ? "Show" : "Hide");
}

static const char *const k_net_title[NET_N] = {
    "Name", "IP", "Gateway", "Mask", "100", "101", "102", "103"
};
static const char *const k_net_cap[NET_N] = {
    "Station name", "This station IP", "Gateway", "Netmask",
    "Station 100 IP", "Station 101 IP", "Station 102 IP", "Station 103 IP"
};
static const char *const k_role[4] = {"Reception", "Floor 1", "Floor 2", "Floor 3"};

static void net_error(const char *text, uint32_t color) {
    set_text(s_net_err, text ? text : "");
    if (s_net_err) {
        lv_obj_set_style_text_color(s_net_err, lv_color_hex(color), 0);
    }
}

static void store_active_field(void) {
    const char *text;
    if (!s_net_ta || s_net_active < 0 || s_net_active >= NET_N) {
        return;
    }
    text = lv_textarea_get_text(s_net_ta);
    snprintf(s_net_draft[s_net_active], sizeof(s_net_draft[0]), "%s", text ? text : "");
}

static void restyle_net(void) {
    int i;
    for (i = 0; i < NET_N; i++) {
        tone(&s_net_field[i], i == s_net_active ? T_AMBER : T_SURFACE);
        relabel(&s_net_field[i], k_net_title[i], s_net_draft[i]);
    }
    for (i = 0; i < 4; i++) {
        tone(&s_net_sid[i], (IC_STATION_MIN + i) == s_edit_sid ? T_AMBER : T_TEAL);
        relabel(&s_net_sid[i], k_net_title[NET_P0 + i], k_role[i]);
    }
    s_net_paint_active = s_net_active;
    s_net_paint_sid = s_edit_sid;
}

static void show_net_field(int idx) {
    store_active_field();
    if (idx < 0 || idx >= NET_N) {
        idx = NET_NAME;
    }
    s_net_active = idx;
    if (s_net_kb) {
        show(s_net_kb, false);
    }
    if (s_net_ta) {
        lv_textarea_set_accepted_chars(s_net_ta, idx == NET_NAME ? NULL : "0123456789.");
        lv_textarea_set_max_length(s_net_ta, idx == NET_NAME ? (IC_NAME_LEN - 1) : 15);
        lv_textarea_set_text(s_net_ta, s_net_draft[idx]);
        lv_obj_set_style_border_color(s_net_ta, lv_color_hex(C_AMBER), 0);
    }
    set_text(s_net_cap, k_net_cap[idx]);
    if (s_net_kb && s_net_ta) {
        lv_keyboard_set_mode(s_net_kb, idx == NET_NAME ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(s_net_kb, s_net_ta);
    }
    restyle_net();
}

static void net_commit(void);

static void on_net_field(lv_event_t *e) {
    int idx;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_net_field(idx);
    if (s_net_kb) {
        show(s_net_kb, true);
        lv_obj_move_foreground(s_net_kb);
    }
}

static void on_net_sid(lv_event_t *e) {
    int id;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < IC_STATION_MIN || id > IC_STATION_MAX) {
        return;
    }
    s_edit_sid = id;
    restyle_net();
}

static void on_net_save(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    net_commit();
}

static void net_commit(void) {
    ic_config_t copy;
    uint32_t ip = 0;
    uint32_t gw = 0;
    uint32_t mask = 0;
    uint32_t peer[4];
    int i;
    store_active_field();
    if (s_edit_sid < IC_STATION_MIN || s_edit_sid > IC_STATION_MAX) {
        net_error("Station must be 100 to 103.", C_RED);
        return;
    }
    if (!ic_config_name_ok(s_net_draft[NET_NAME])) {
        net_error("Name must be 1 to 15 characters.", C_RED);
        show_net_field(NET_NAME);
        return;
    }
    if (!ic_parse_ip(s_net_draft[NET_IP], &ip) || ip == 0) {
        net_error("IP must look like 192.168.1.101.", C_RED);
        show_net_field(NET_IP);
        return;
    }
    if (!ic_parse_ip(s_net_draft[NET_GW], &gw)) {
        net_error("Gateway must be four numbers.", C_RED);
        show_net_field(NET_GW);
        return;
    }
    if (!ic_parse_ip(s_net_draft[NET_MASK], &mask) || ((mask >> 24) & 255u) != 255u) {
        net_error("Netmask must start with 255.", C_RED);
        show_net_field(NET_MASK);
        return;
    }
    for (i = 0; i < 4; i++) {
        if (!ic_parse_ip(s_net_draft[NET_P0 + i], &peer[i]) || peer[i] == 0) {
            net_error("Each station needs an IP.", C_RED);
            show_net_field(NET_P0 + i);
            return;
        }
    }
    peer[s_edit_sid - IC_STATION_MIN] = ip;
    if (!g_app.lock) {
        return;
    }
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    copy.station_id = (uint16_t)s_edit_sid;
    snprintf(copy.name, sizeof(copy.name), "%s", s_net_draft[NET_NAME]);
    copy.ip = ip;
    copy.gateway = gw;
    copy.netmask = mask;
    for (i = 0; i < 4; i++) {
        copy.peer_ip[i] = peer[i];
    }
    if (!ic_config_save(&copy)) {
        net_error("Could not save.", C_RED);
        return;
    }
    app_lock();
    g_app.cfg = copy;
    app_unlock();
    net_error("Saved. Rebooting.", C_GREEN);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static void enter_net(void) {
    ic_config_t copy;
    char name[IC_NAME_LEN + 1];
    int i;
    if (s_net_filled || !s_net_ta || !g_app.lock) {
        return;
    }
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    s_edit_sid = (int)copy.station_id;
    if (s_edit_sid < IC_STATION_MIN || s_edit_sid > IC_STATION_MAX) {
        s_edit_sid = IC_STATION_MIN;
    }
    name_of(copy.name, name, sizeof(name));
    snprintf(s_net_draft[NET_NAME], sizeof(s_net_draft[0]), "%s", name);
    ic_format_ip(copy.ip, s_net_draft[NET_IP], sizeof(s_net_draft[0]));
    ic_format_ip(copy.gateway, s_net_draft[NET_GW], sizeof(s_net_draft[0]));
    ic_format_ip(copy.netmask, s_net_draft[NET_MASK], sizeof(s_net_draft[0]));
    for (i = 0; i < 4; i++) {
        ic_format_ip(copy.peer_ip[i], s_net_draft[NET_P0 + i], sizeof(s_net_draft[0]));
    }
    s_net_active = -1;
    s_net_filled = true;
    net_error("Key and PIN stay on the web page.", C_MUTED);
    show_net_field(NET_NAME);
}

static void paint_net(void) {
    int i;
    store_active_field();
    for (i = 0; i < NET_N; i++) {
        relabel(&s_net_field[i], k_net_title[i], s_net_draft[i]);
    }
    if (s_net_paint_active != s_net_active || s_net_paint_sid != s_edit_sid) {
        restyle_net();
    }
}

static void on_input_keyboard(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
        show(lv_event_get_target(e), false);
    }
}

static void on_ta(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *other;
    if (!ta) {
        return;
    }
    if (ta == s_net_ta) {
        if ((code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) && s_net_kb) {
            lv_keyboard_set_textarea(s_net_kb, ta);
            show(s_net_kb, true);
            lv_obj_move_foreground(s_net_kb);
        } else if (code == LV_EVENT_READY) {
            if (s_net_active + 1 < NET_N) {
                show_net_field(s_net_active + 1);
                show(s_net_kb, true);
                lv_obj_move_foreground(s_net_kb);
            } else {
                net_commit();
            }
        }
        return;
    }
    if (!s_kb) {
        return;
    }
    other = (ta == s_ssid_ta) ? s_pass_ta : s_ssid_ta;
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(s_kb, ta);
        show(s_kb, true);
        lv_obj_move_foreground(s_kb);
        lv_obj_set_style_border_color(ta, lv_color_hex(C_AMBER), 0);
        if (other) {
            lv_obj_set_style_border_color(other, lv_color_hex(C_LINE), 0);
        }
    } else if (code == LV_EVENT_READY) {
        if (ta == s_ssid_ta && s_pass_ta) {
            lv_keyboard_set_textarea(s_kb, s_pass_ta);
            lv_obj_set_style_border_color(s_ssid_ta, lv_color_hex(C_LINE), 0);
            lv_obj_set_style_border_color(s_pass_ta, lv_color_hex(C_AMBER), 0);
        } else {
            wifi_commit();
        }
    }
}

static void on_splash_tap(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_skip = true;
    }
}

static void enter_wifi(void) {
    ic_config_t copy;
    if (s_wifi_filled || !s_ssid_ta || !g_app.lock) {
        return;
    }
    app_lock();
    copy = g_app.cfg;
    app_unlock();
    lv_textarea_set_text(s_ssid_ta, copy.ssid);
    lv_textarea_set_text(s_pass_ta, copy.wifi_pass);
    s_pass_hide = false;
    lv_textarea_set_password_mode(s_pass_ta, false);
    set_text(s_hide_lbl, "Hide");
    wifi_error("", C_MUTED);
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, s_ssid_ta);
        show(s_kb, false);
    }
    lv_obj_set_style_border_color(s_ssid_ta, lv_color_hex(C_AMBER), 0);
    if (s_pass_ta) {
        lv_obj_set_style_border_color(s_pass_ta, lv_color_hex(C_LINE), 0);
    }
    s_wifi_filled = true;
}

static void take_snap(uint32_t now, Snap *s) {
    int i;
    memset(s, 0, sizeof(*s));
    app_lock();
    ic_core_view(&g_app.core, now, &s->view);
    s->view.status[IC_STATUS_LEN - 1] = 0;
    for (i = 0; i < 3; i++) {
        if (g_app.core.legs[i].state != IC_LEG_EMPTY && g_app.core.legs[i].start_ms != 0) {
            s->age_ms = now - g_app.core.legs[i].start_ms;
            break;
        }
    }
    s->aec_ms = (int)g_app.cfg.aec_samples / 8;
    s->nlegs = ic_core_legs(&g_app.core);
    s->key_default = ic_config_key_is_default(&g_app.cfg);
    s->pin_default = ic_config_pin_is_default(&g_app.cfg);
    memcpy(s->ssid, g_app.cfg.ssid, sizeof(s->ssid) - 1);
    s->pass_set = g_app.cfg.wifi_pass[0] != 0;
    ic_format_ip(g_app.cfg.ip, s->ip, sizeof(s->ip));
    ic_format_ip(g_app.cfg.gateway, s->gw, sizeof(s->gw));
    ic_format_ip(g_app.cfg.netmask, s->mask, sizeof(s->mask));
    app_unlock();
    s->rms = g_app.mic_rms;
    s->headset = usb_uac_streaming();
    s->tone = g_app.tone_frames > 0;
    s->audio_ready = g_app.audio_ready;
    ui_link_read(&s->link);
    ui_battery_read(&s->bat);
}

static void peer_name(const Snap *s, int index, char *dst, size_t cap) {
    if (!dst || cap == 0) {
        return;
    }
    dst[0] = 0;
    if (index < 0 || index >= IC_STATION_COUNT) {
        return;
    }
    name_of(s->view.dir[index].name, dst, cap);
    if (dst[0] == 0) {
        snprintf(dst, cap, "%u", (unsigned)s->view.dir[index].id);
    }
}

static void who_label(const Snap *s, char *dst, size_t cap) {
    char nb[IC_NAME_LEN + 1];
    uint16_t id = s->view.remote ? s->view.remote : s->view.selected;
    name_of(s->view.remote_name, nb, sizeof(nb));
    if (nb[0]) {
        snprintf(dst, cap, "%s", nb);
        return;
    }
    peer_name(s, (int)id - IC_STATION_MIN, dst, cap);
    if (dst[0] == 0) {
        snprintf(dst, cap, "Station");
    }
}

static void fmt_clock(char *dst, size_t cap, uint32_t age_ms, bool speaker) {
    unsigned sec = (unsigned)(age_ms / 1000u);
    snprintf(dst, cap, "%u:%02u   -   speaker %s", sec / 60u, sec % 60u, speaker ? "on" : "off");
}

static void fmt_bat(const Snap *s, char *dst, size_t cap) {
    if (!s->bat.present) {
        snprintf(dst, cap, "USB");
        return;
    }
    snprintf(dst, cap, "%d%%  -  %d.%02d V", s->bat.percent, s->bat.millivolts / 1000,
             (s->bat.millivolts % 1000) / 10);
}

static void fmt_radio(const Snap *s, char *dst, size_t cap) {
    char bat[32];
    fmt_bat(s, bat, sizeof(bat));
    if (!s->link.up) {
        snprintf(dst, cap, "Wi-Fi down   -   Battery %s", bat);
    } else {
        snprintf(dst, cap, "Wi-Fi %d dBm   -   Battery %s", s->link.rssi_dbm, bat);
    }
}

static int pill_w(const char *text) {
    int n = 0;
    int w;
    while (text && text[n]) {
        n++;
    }
    w = 64 + n * 16;
    if (w < 148) {
        w = 148;
    }
    if (w > 260) {
        w = 260;
    }
    return w;
}

static const char *pill_text(const Snap *s, int screen, uint32_t *bg) {
    *bg = C_SURF2;
    if (screen == SCR_SETUP) {
        return "SETUP";
    }
    if (screen == SCR_POWER) {
        return "POWER";
    }
    if (screen == SCR_WIFI) {
        return "WI-FI";
    }
    if (screen == SCR_LINK || !s->view.link_up) {
        *bg = C_ALERT;
        return "NO WI-FI";
    }
    if (screen == SCR_AUDIO || (!s->view.audio_ok && s->view.state == IC_ST_IDLE)) {
        *bg = C_ALERT;
        return "AUDIO FAULT";
    }
    if (s->view.state == IC_ST_CALLING) {
        *bg = C_WARM;
        return "CALLING";
    }
    if (s->view.state == IC_ST_CONNECTED) {
        if (s->view.mic_tx) {
            *bg = C_RED;
            return "MIC LIVE";
        }
        *bg = C_CALL_BG;
        return "IN CALL";
    }
    if (s->view.state == IC_ST_PAGING) {
        *bg = C_TEAL_BG;
        return s->nlegs > 1 ? "PAGE ALL" : "PAGE";
    }
    if (s->view.state == IC_ST_PAGE_RX) {
        *bg = C_TEAL_BG;
        return "PAGE";
    }
    if (strcmp(s->view.status, "Busy") == 0) {
        *bg = C_WARM;
        return "BUSY";
    }
    if (strcmp(s->view.status, "Mic armed") == 0) {
        *bg = C_WARM;
        return "MIC ARMED";
    }
    return "IDLE";
}

static void paint_chrome(const Snap *s, int screen) {
    char nb[IC_NAME_LEN + 1];
    char up[IC_NAME_LEN + 1];
    char idbuf[8];
    char rssi[16];
    char bat[12];
    uint32_t pill_bg = C_SURF2;
    const char *pill = pill_text(s, screen, &pill_bg);
    int bars = s->link.up ? s->link.bars : 0;
    uint32_t on = s->link.up ? C_CREAM : C_LINE;
    uint32_t off = C_LINE;
    bool low = s->bat.present && s->bat.percent >= 0 && s->bat.percent < 15;
    int pct = (s->bat.present && s->bat.percent > 0) ? s->bat.percent : 0;
    int fill = pct * 22 / 100;
    int i;
    peer_name(s, (int)s->view.self_id - IC_STATION_MIN, nb, sizeof(nb));
    if (nb[0] == 0) {
        snprintf(nb, sizeof(nb), "Station");
    }
    upper_copy(up, sizeof(up), nb);
    set_text(s_name, up);
    snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)s->view.self_id);
    set_text(s_idlbl, idbuf);
    if (s_name && s_idlbl) {
        lv_obj_align_to(s_idlbl, s_name, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    }
    set_text(s_pill_lbl, pill);
    if (s_pill) {
        lv_obj_set_width(s_pill, pill_w(pill));
        lv_obj_set_style_bg_color(s_pill, lv_color_hex(pill_bg), 0);
        lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 18);
    }
    if (s_pill_lbl) {
        lv_obj_center(s_pill_lbl);
    }
    if (!s->link.up) {
        on = C_RED;
        off = C_LINE;
        bars = 0;
    }
    for (i = 0; i < 4; i++) {
        if (s_bars[i]) {
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(i < bars ? on : off), 0);
        }
    }
    if (!s->link.up) {
        snprintf(rssi, sizeof(rssi), "down");
    } else {
        snprintf(rssi, sizeof(rssi), "%d", s->link.rssi_dbm);
    }
    set_text(s_rssi, rssi);
    if (s_rssi) {
        lv_obj_set_style_text_color(s_rssi, lv_color_hex(s->link.up ? C_MUTED : C_RED), 0);
    }
    if (s_bat_fill) {
        if (fill < 2 && pct > 0) {
            fill = 2;
        }
        lv_obj_set_width(s_bat_fill, fill);
        lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(low ? C_RED : C_GREEN), 0);
    }
    if (!s->bat.present) {
        snprintf(bat, sizeof(bat), "USB");
    } else {
        snprintf(bat, sizeof(bat), "%d%%", s->bat.percent);
    }
    set_text(s_bat_lbl, bat);
    if (s_bat_lbl) {
        lv_obj_set_style_text_color(s_bat_lbl, lv_color_hex(low ? C_RED : C_TEXT), 0);
    }
}

static void paint_dir_card(Card *c, const Snap *s, int index) {
    const ic_peer_view_t *p;
    bool self;
    bool sel;
    char nb[IC_NAME_LEN + 1];
    char idbuf[8];
    char ip[16];
    uint32_t bg;
    uint32_t border;
    uint32_t tagc;
    const char *tag;
    if (!c || index < 0 || index >= IC_STATION_COUNT) {
        return;
    }
    p = &s->view.dir[index];
    self = p->id == s->view.self_id;
    sel = !self && p->id != 0 && p->id == s->view.selected;
    bg = C_SURF;
    border = C_LINE;
    if (sel) {
        bg = C_WARM;
        border = C_AMBER;
    } else if (self) {
        bg = C_SURF2;
    }
    tag = "OFFLINE";
    tagc = C_OFF;
    peer_name(s, index, nb, sizeof(nb));
    snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)p->id);
    ic_format_ip(p->ip, ip, sizeof(ip));
    if (self) {
        tag = "THIS STATION";
        tagc = C_GREEN;
    } else if (sel) {
        tag = "SELECTED";
        tagc = C_AMBER;
    } else if (p->online) {
        tag = "ONLINE";
        tagc = C_GREEN;
    }
    if (c->btn) {
        lv_obj_set_style_bg_color(c->btn, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_color(c->btn, lv_color_hex(C_PRESS), LV_STATE_PRESSED);
        stroke(c->btn, border, sel ? 3 : 2);
    }
    if (c->dot) {
        lv_obj_set_style_bg_color(c->dot, lv_color_hex(tagc), 0);
    }
    set_text(c->tag, tag);
    if (c->tag) {
        lv_obj_set_style_text_color(c->tag, lv_color_hex(tagc), 0);
    }
    set_text(c->name, nb);
    if (c->name) {
        lv_obj_set_style_text_color(c->name, lv_color_hex(p->online || self || sel ? C_TEXT : C_MUTED), 0);
    }
    set_text(c->ident, idbuf);
    set_text(c->sub, self ? "Not a destination" : ip);
}

static void paint_home(const Snap *s) {
    char nb[48];
    char line[48];
    int sel = (int)s->view.selected - IC_STATION_MIN;
    int i;
    const char *mic_sub = "Off";
    int mic_tone = T_SURFACE;
    for (i = 0; i < 4; i++) {
        paint_dir_card(&s_home_card[i], s, i);
    }
    if (sel >= 0 && sel < IC_STATION_COUNT && s->view.selected != s->view.self_id) {
        peer_name(s, sel, nb, sizeof(nb));
    } else {
        snprintf(nb, sizeof(nb), "Select a station");
    }
    relabel(&s_call_b, "Call", nb);
    tone(&s_call_b, T_AMBER);
    if (s->view.mic_armed) {
        mic_sub = "Armed";
        mic_tone = T_AMBER;
    }
    relabel(&s_mic_b, "Mic", mic_sub);
    tone(&s_mic_b, mic_tone);
    relabel(&s_page_b, "Page",
            (sel >= 0 && s->view.selected != s->view.self_id) ? nb : "Select a floor");
    tone(&s_page_b, T_TEAL);
    relabel(&s_all_b, "Page all", "101 / 102 / 103");
    tone(&s_all_b, T_SURFACE);
    relabel(&s_end_b, "End", "Idle");
    tone(&s_end_b, T_DISABLED);
    sync_slider(s_home_slider, s->view.volume);
    vol_labels(s->view.volume);
    if (s->tone) {
        snprintf(line, sizeof(line), "Local tone");
    } else if (s->view.mic_armed) {
        snprintf(line, sizeof(line), "Speaker off  /  Mic armed");
    } else {
        snprintf(line, sizeof(line), "Speaker off  /  Mic off");
    }
    set_text(s_home_status, line);
    if (s->headset && s->view.audio_ok) {
        set_text(s_home_headset, "HEADSET READY");
        if (s_home_headset) {
            lv_obj_set_style_text_color(s_home_headset, lv_color_hex(C_GREEN), 0);
        }
    } else {
        set_text(s_home_headset, "HEADSET WAITING");
        if (s_home_headset) {
            lv_obj_set_style_text_color(s_home_headset, lv_color_hex(C_AMBER), 0);
        }
    }
}

static void paint_floor(const Snap *s) {
    char nb[48];
    char bat[40];
    int sel = (int)s->view.selected - IC_STATION_MIN;
    int i;
    const char *mic_sub = "Off";
    int mic_tone = T_SURFACE;
    for (i = 0; i < 4; i++) {
        paint_dir_card(&s_floor_card[i], s, i);
    }
    if (sel >= 0 && sel < IC_STATION_COUNT && s->view.selected != s->view.self_id) {
        peer_name(s, sel, nb, sizeof(nb));
    } else {
        snprintf(nb, sizeof(nb), "Select a station");
    }
    relabel(&s_fcall, "Call", nb);
    tone(&s_fcall, T_AMBER);
    if (s->view.mic_armed) {
        mic_sub = "Armed, not sending";
        mic_tone = T_SURFACE;
        set_text(s_floor_title, "Microphone armed");
    } else {
        set_text(s_floor_title, "Microphone off");
    }
    if (s_floor_title) {
        lv_obj_set_style_text_color(s_floor_title, lv_color_hex(s->view.mic_armed ? C_AMBER : C_TEXT), 0);
    }
    relabel(&s_fmic, "Mic", mic_sub);
    tone(&s_fmic, mic_tone);
    relabel(&s_fpage, "Page", "Reception only");
    tone(&s_fpage, T_DIM);
    relabel(&s_fend, "End", "Idle");
    tone(&s_fend, T_DISABLED);
    fmt_bat(s, bat, sizeof(bat));
    set_text(s_floor_bat, bat);
}

static void paint_calling(const Snap *s) {
    char who[IC_NAME_LEN + 1];
    char line[64];
    char ip[16];
    int index;
    who_label(s, who, sizeof(who));
    set_text(s_call_name, who);
    index = (int)(s->view.remote ? s->view.remote : s->view.selected) - IC_STATION_MIN;
    if (index >= 0 && index < IC_STATION_COUNT) {
        ic_format_ip(s->view.dir[index].ip, ip, sizeof(ip));
        snprintf(line, sizeof(line), "Station %u   -   %s",
                 (unsigned)s->view.dir[index].id, ip);
    } else {
        snprintf(line, sizeof(line), "Waiting for an answer");
    }
    set_text(s_call_sub, line);
    fmt_radio(s, line, sizeof(line));
    set_text(s_call_radio, line);
    relabel(&s_call_end, "End", "Cancel this call");
    tone(&s_call_end, T_RED);
}

static void paint_muted(const Snap *s) {
    char who[IC_NAME_LEN + 1];
    char clockb[48];
    who_label(s, who, sizeof(who));
    set_text(s_mu_name, who);
    fmt_clock(clockb, sizeof(clockb), s->age_ms, s->view.speaker);
    set_text(s_mu_clock, clockb);
    snprintf(clockb, sizeof(clockb), "You can hear %s.", who);
    set_text(s_mu_hear, clockb);
    relabel(&s_mu_talk, "Talk", "Microphone is off");
    tone(&s_mu_talk, T_AMBER);
    relabel(&s_mu_end, "End", "Hang up");
    tone(&s_mu_end, T_RED);
    sync_slider(s_mu_slider, s->view.volume);
    vol_labels(s->view.volume);
    gain_label(s->view.mic_gain);
}

static void paint_live_meter(const Snap *s) {
    int level = s->rms >= 4000u ? 4000 : (int)s->rms;
    int lit = level * 16 / 4000;
    int i;
    if (lit > 16) {
        lit = 16;
    }
    for (i = 0; i < 16; i++) {
        if (s_live_bar[i]) {
            lv_obj_set_style_bg_color(s_live_bar[i], lv_color_hex(i < lit ? C_RED : C_PRESS), 0);
        }
    }
}

static void paint_live(const Snap *s) {
    char who[IC_NAME_LEN + 1];
    char clockb[48];
    who_label(s, who, sizeof(who));
    set_text(s_live_name, who);
    fmt_clock(clockb, sizeof(clockb), s->age_ms, s->view.speaker);
    set_text(s_live_clock, clockb);
    relabel(&s_live_mic, "Mute", "Tap to stop sending");
    tone(&s_live_mic, T_RED);
    if (s_live_mic.title) {
        lv_obj_set_style_text_color(s_live_mic.title, lv_color_hex(C_TEXT), 0);
    }
    if (s_live_mic.sub) {
        lv_obj_set_style_text_color(s_live_mic.sub, lv_color_hex(0xF0C8C0), 0);
    }
    relabel(&s_live_end, "End", "Hang up both sides");
    tone(&s_live_end, T_RED);
    paint_live_meter(s);
}

static void paint_floor_slot(Card *c, const Snap *s, int index, int role) {
    char nb[IC_NAME_LEN + 1];
    char idbuf[8];
    char ip[16];
    uint32_t bg = C_SURF;
    uint32_t border = C_LINE;
    uint32_t tagc = C_OFF;
    uint32_t namec = C_MUTED;
    const char *tag = "OFFLINE";
    const char *sub;
    int bw = 1;
    if (!c || index < 0 || index >= IC_STATION_COUNT) {
        return;
    }
    peer_name(s, index, nb, sizeof(nb));
    snprintf(idbuf, sizeof(idbuf), "%u", (unsigned)s->view.dir[index].id);
    ic_format_ip(s->view.dir[index].ip, ip, sizeof(ip));
    sub = ip;
    if (role == 1) {
        bg = C_TEAL_BG;
        border = C_TEAL;
        tagc = C_TEAL;
        namec = C_TEXT;
        tag = "HEARING";
        bw = 2;
    } else if (role == 2) {
        tagc = C_GREEN;
        namec = C_TEXT;
        tag = "ONLINE";
    } else if (role == 3) {
        tagc = C_AMBER;
        tag = "IN A CALL";
        sub = "Skipped";
    }
    if (c->btn) {
        lv_obj_set_style_bg_color(c->btn, lv_color_hex(bg), 0);
        stroke(c->btn, border, bw);
    }
    if (c->dot) {
        lv_obj_set_style_bg_color(c->dot, lv_color_hex(tagc), 0);
    }
    set_text(c->tag, tag);
    if (c->tag) {
        lv_obj_set_style_text_color(c->tag, lv_color_hex(tagc), 0);
    }
    set_text(c->name, nb);
    if (c->name) {
        lv_obj_set_style_text_color(c->name, lv_color_hex(namec), 0);
    }
    set_text(c->ident, idbuf);
    set_text(c->sub, sub);
}

static void paint_page(const Snap *s, bool all) {
    char who[IC_NAME_LEN + 1];
    char line[96];
    int slots[3] = {1, 2, 3};
    int i;
    uint16_t remote = s->view.remote ? s->view.remote : s->view.selected;
    who_label(s, who, sizeof(who));
    if (all) {
        set_text(s_pa_title, "All free floors");
        relabel(&s_pa_talk, s->view.mic_tx ? "Mute" : "Talk",
                s->view.mic_tx ? "Tap to stop sending" : "Floors hear silence until you do");
        tone(&s_pa_talk, s->view.mic_tx ? T_RED : T_AMBER);
        if (s->view.mic_tx && s_pa_talk.title) {
            lv_obj_set_style_text_color(s_pa_talk.title, lv_color_hex(C_TEXT), 0);
            if (s_pa_talk.sub) {
                lv_obj_set_style_text_color(s_pa_talk.sub, lv_color_hex(0xF0C8C0), 0);
            }
        }
        relabel(&s_pa_end, "End page", "All floors drop");
        tone(&s_pa_end, T_RED);
    } else {
        snprintf(line, sizeof(line), "Paging %s", who);
        set_text(s_pg_title, line);
        relabel(&s_pg_talk, s->view.mic_tx ? "Mute" : "Talk", s->view.mic_tx ? "Tap to stop sending" : "Mic is off");
        tone(&s_pg_talk, s->view.mic_tx ? T_RED : T_AMBER);
        if (s->view.mic_tx && s_pg_talk.title) {
            lv_obj_set_style_text_color(s_pg_talk.title, lv_color_hex(C_TEXT), 0);
            if (s_pg_talk.sub) {
                lv_obj_set_style_text_color(s_pg_talk.sub, lv_color_hex(0xF0C8C0), 0);
            }
        }
        relabel(&s_pg_end, "End page", "Stops this floor");
        tone(&s_pg_end, T_RED);
        relabel(&s_pg_all, "Page all", "Switch to all floors");
        tone(&s_pg_all, T_SURFACE);
    }
    for (i = 0; i < 3; i++) {
        int index = slots[i];
        int role = 0;
        const ic_peer_view_t *p = &s->view.dir[index];
        if (all) {
            role = p->online ? 1 : 0;
        } else if (p->id == remote && p->online) {
            role = 1;
        } else if (p->online) {
            role = 2;
        }
        paint_floor_slot(all ? &s_pa_card[i] : &s_pg_card[i], s, index, role);
    }
    if (all) {
        set_text(s_pa_foot, "Receivers keep their microphones off. Enable Mic on a floor does nothing.");
    } else {
        int ri = (int)remote - IC_STATION_MIN;
        if (ri >= 0 && ri < IC_STATION_COUNT) {
            peer_name(s, ri, who, sizeof(who));
            if (!s->view.dir[ri].online) {
                snprintf(line, sizeof(line), "%s is offline, so it is not in this page.", who);
            } else {
                snprintf(line, sizeof(line), "This page is only %s. Other floors are left alone.", who);
            }
            set_text(s_pg_foot, line);
        }
    }
}

static void paint_rx(const Snap *s) {
    char line[80];
    char who[IC_NAME_LEN + 1];
    who_label(s, who, sizeof(who));
    upper_copy(line, sizeof(line), who);
    set_text(s_rx_title, s->view.speaker ? "Speaker on" : "Speaker off");
    snprintf(line, sizeof(line), "%s   -   this station is %u", s->view.speaker ? "Speaker on" : "Speaker off",
             (unsigned)s->view.self_id);
    set_text(s_rx_sub, line);
    relabel(&s_rx_lock, "Mic locked", "Stays off during a page");
    tone(&s_rx_lock, T_DISABLED);
    fmt_radio(s, line, sizeof(line));
    set_text(s_rx_foot, line);
}

static void paint_busy(const Snap *s) {
    char nb[IC_NAME_LEN + 1];
    char line[72];
    uint16_t busy = s->view.selected;
    int n = 0;
    int i;
    if (busy == 0 || busy == s->view.self_id) {
        busy = s->view.remote;
    }
    peer_name(s, (int)busy - IC_STATION_MIN, nb, sizeof(nb));
    snprintf(line, sizeof(line), "%s is busy", nb[0] ? nb : "That station");
    set_text(s_busy_title, line);
    snprintf(line, sizeof(line), "Choose another station, or wait until %s is idle.", nb[0] ? nb : "they");
    set_text(s_busy_line, line);
    s_alt_id[0] = 0;
    s_alt_id[1] = 0;
    for (i = 0; i < 4 && n < 2; i++) {
        uint16_t id = s->view.dir[i].id;
        char sub[16];
        if (id == 0 || id == s->view.self_id || id == busy) {
            continue;
        }
        s_alt_id[n] = id;
        peer_name(s, i, nb, sizeof(nb));
        snprintf(sub, sizeof(sub), "%s", s->view.dir[i].online ? "Online" : "Offline");
        relabel(&s_busy_alt[n], nb, sub);
        tone(&s_busy_alt[n], s->view.dir[i].online ? T_SURFACE : T_DIM);
        show(s_busy_alt[n].btn, true);
        n++;
    }
    for (i = n; i < 2; i++) {
        show(s_busy_alt[i].btn, false);
    }
}

static void paint_link(const Snap *s) {
    char line[80];
    char bat[40];
    int i;
    if (s->ssid[0] == 0) {
        set_text(s_link_ssid, "Wi-Fi name not set");
    } else {
        set_text(s_link_ssid, s->ssid);
    }
    set_text(s_link_msg, "No address. Calls are cleared and the microphone is off.");
    for (i = 0; i < 4; i++) {
        char nb[IC_NAME_LEN + 1];
        peer_name(s, i, nb, sizeof(nb));
        snprintf(line, sizeof(line), "%u  -  %s", (unsigned)s->view.dir[i].id,
                 s->view.dir[i].online ? "online" : "offline");
        set_text(s_link_name[i], nb);
        set_text(s_link_sub[i], line);
    }
    fmt_bat(s, bat, sizeof(bat));
    snprintf(line, sizeof(line), "Battery %s. Red under 15%%.", bat);
    set_text(s_link_foot, line);
}

static void paint_setup(const Snap *s) {
    char nb[IC_NAME_LEN + 1];
    char line[48];
    int level;
    sync_slider(s_setup_slider_v, s->view.volume);
    sync_slider(s_setup_slider_g, s->view.mic_gain);
    vol_labels(s->view.volume);
    gain_label(s->view.mic_gain);
    aec_label(s->aec_ms);
    peer_name(s, (int)s->view.self_id - IC_STATION_MIN, nb, sizeof(nb));
    set_text(s_id_name, nb);
    snprintf(line, sizeof(line), "%u", (unsigned)s->view.self_id);
    set_text(s_id_num, line);
    set_text(s_id_ip, s->ip);
    snprintf(line, sizeof(line), "Gateway %s", s->gw);
    set_text(s_id_gw, line);
    snprintf(line, sizeof(line), "Mask %s", s->mask);
    set_text(s_id_mask, line);
    show(s_warn, s->key_default || s->pin_default);
    level = s->rms >= 4000u ? 4000 : (int)s->rms;
    if (s_setup_meter) {
        lv_bar_set_value(s_setup_meter, level, LV_ANIM_OFF);
    }
}

static void paint_power(const Snap *s) {
    char buf[48];
    int i;
    bool low = s->bat.present && s->bat.percent >= 0 && s->bat.percent < 15;
    int pct = (s->bat.present && s->bat.percent > 0) ? s->bat.percent : 0;
    int fill = pct * 222 / 100;
    if (!s->bat.present) {
        set_text(s_pwr_pct, "USB");
        set_text(s_pwr_volt, "no pack");
        fill = 0;
    } else {
        snprintf(buf, sizeof(buf), "%d%%", s->bat.percent);
        set_text(s_pwr_pct, buf);
        snprintf(buf, sizeof(buf), "%d.%02d V", s->bat.millivolts / 1000, (s->bat.millivolts % 1000) / 10);
        set_text(s_pwr_volt, buf);
    }
    if (s_pwr_pct) {
        lv_obj_set_style_text_color(s_pwr_pct, lv_color_hex(low ? C_RED : C_TEXT), 0);
    }
    if (s_pwr_fill) {
        if (fill < 4 && pct > 0) {
            fill = 4;
        }
        lv_obj_set_width(s_pwr_fill, fill);
        lv_obj_set_style_bg_color(s_pwr_fill, lv_color_hex(low ? C_RED : C_GREEN), 0);
    }
    set_text(s_pwr_ssid, s->ssid[0] ? s->ssid : "Wi-Fi name not set");
    if (!s->link.up) {
        snprintf(buf, sizeof(buf), "down");
    } else {
        snprintf(buf, sizeof(buf), "%d dBm   /   %d bars", s->link.rssi_dbm, s->link.bars);
    }
    set_text(s_pwr_rssi, buf);
    set_text(s_pwr_pass, s->pass_set ? "Password is set. It is not shown here." : "Open network. No password is set.");
    for (i = 0; i < 4; i++) {
        char nb[IC_NAME_LEN + 1];
        char ip[16];
        uint32_t col = C_OFF;
        peer_name(s, i, nb, sizeof(nb));
        ic_format_ip(s->view.dir[i].ip, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%u %s", (unsigned)s->view.dir[i].id, nb);
        set_text(s_pwr_name[i], buf);
        set_text(s_pwr_ip[i], ip);
        if (s->view.dir[i].id == s->view.self_id) {
            set_text(s_pwr_state[i], "This station");
            col = C_AMBER;
        } else if (s->view.dir[i].online) {
            set_text(s_pwr_state[i], "Online");
            col = C_GREEN;
        } else {
            set_text(s_pwr_state[i], "Offline");
        }
        if (s_pwr_state[i]) {
            lv_obj_set_style_text_color(s_pwr_state[i], lv_color_hex(col), 0);
        }
    }
}

static void paint_audio(const Snap *s) {
    char line[96];
    fmt_radio(s, line, sizeof(line));
    set_text(s_au_foot, line);
}

static void paint_splash(const Snap *s) {
    char nb[IC_NAME_LEN + 1];
    char line[64];
    peer_name(s, (int)s->view.self_id - IC_STATION_MIN, nb, sizeof(nb));
    if (nb[0] == 0) {
        name_of(g_app.cfg.name, nb, sizeof(nb));
    }
    snprintf(line, sizeof(line), "%s  -  station %u", nb[0] ? nb : "Station", (unsigned)s->view.self_id);
    set_text(s_splash_sub, line);
    set_text(s_splash_val[0], "1024 x 600");
    if (s->view.audio_ok) {
        set_text(s_splash_key[1], "Headset");
        set_text(s_splash_val[1], "ready");
        if (s_splash_key[1]) {
            lv_obj_set_style_text_color(s_splash_key[1], lv_color_hex(C_GREEN), 0);
        }
    } else {
        set_text(s_splash_key[1], "Headset");
        set_text(s_splash_val[1], "USB adapter waiting");
        if (s_splash_key[1]) {
            lv_obj_set_style_text_color(s_splash_key[1], lv_color_hex(C_AMBER), 0);
        }
    }
    if (!ic_config_ssid_ok(s->ssid)) {
        set_text(s_splash_val[2], "not set");
    } else if (s->link.up) {
        snprintf(line, sizeof(line), "%s   %d dBm", s->ssid, s->link.rssi_dbm);
        set_text(s_splash_val[2], line);
    } else {
        snprintf(line, sizeof(line), "joining %s", s->ssid);
        set_text(s_splash_val[2], line);
    }
    if (!s->bat.present) {
        set_text(s_splash_val[3], "no pack");
    } else {
        snprintf(line, sizeof(line), "%d.%02d V   %d%%", s->bat.millivolts / 1000,
                 (s->bat.millivolts % 1000) / 10, s->bat.percent);
        set_text(s_splash_val[3], line);
    }
}

static void paint_splash_anim(uint32_t now) {
    static const char *dots[] = {"STARTING", "STARTING.", "STARTING..", "STARTING..."};
    uint32_t age = (s_booted && s_t0 != 0) ? (now - s_t0) : 0;
    int w;
    set_text(s_splash_kick, dots[(now / 250u) % 4u]);
    if (age > 4500u) {
        age = 4500u;
    }
    w = (int)(age * 896u / 4500u);
    if (s_splash_fill) {
        lv_obj_set_width(s_splash_fill, w);
    }
}

static void paint_rings(uint32_t now) {
    static const lv_opa_t base[] = {LV_OPA_90, LV_OPA_60, LV_OPA_40};
    int phase = (int)((now / 250u) % 3u);
    int i;
    for (i = 0; i < 3; i++) {
        lv_opa_t opa = (i == phase) ? LV_OPA_COVER : base[i];
        if (s_ring[i]) {
            lv_obj_set_style_border_opa(s_ring[i], opa, 0);
        }
    }
}

static void show_screen(int screen) {
    int i;
    int prev = s_cur;
    if (screen == SCR_SPLASH) {
        if (s_on_app && s_splash_scr) {
            lv_scr_load_anim(s_splash_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            s_on_app = false;
        }
        s_cur = screen;
        return;
    }
    if (!s_on_app && s_app) {
        lv_scr_load_anim(s_app, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        s_on_app = true;
    }
    for (i = SCR_HOME; i < SCR_COUNT; i++) {
        if (!s_views[i]) {
            continue;
        }
        show(s_views[i], i == screen);
    }
    if (screen != SCR_SETUP && s_setup_kb) {
        close_setup_keyboard();
    }
    if (screen != SCR_WIFI && s_kb) {
        show(s_kb, false);
    }
    if (screen != SCR_NET && s_net_kb) {
        show(s_net_kb, false);
    }
    if (screen == SCR_WIFI && prev != SCR_WIFI) {
        enter_wifi();
    }
    if (screen != SCR_WIFI && prev == SCR_WIFI) {
        s_wifi_filled = false;
        if (s_ssid_ta) {
            lv_textarea_set_text(s_ssid_ta, "");
        }
        if (s_pass_ta) {
            lv_textarea_set_text(s_pass_ta, "");
        }
    }
    if (screen == SCR_NET && prev != SCR_NET) {
        enter_net();
    }
    if (screen != SCR_NET && prev == SCR_NET) {
        s_net_filled = false;
        s_net_active = -1;
    }
    s_cur = screen;
}

static int pick_screen(const Snap *s) {
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
        return s->view.mic_tx ? SCR_LIVE : SCR_MUTED;
    }
    if (s->view.state == IC_ST_PAGING) {
        return s->nlegs > 1 ? SCR_PAGEALL : SCR_PAGE;
    }
    if (s->view.state == IC_ST_PAGE_RX) {
        return SCR_RX;
    }
    if (s_nav == NAV_WIFI) {
        return SCR_WIFI;
    }
    if (s_nav == NAV_NET) {
        return SCR_NET;
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
    if (s->view.self_id == IC_STATION_MIN) {
        return SCR_HOME;
    }
    return SCR_FLOOR;
}

static void splash_tick(uint32_t now, const Snap *s) {
    uint32_t age;
    if (!s_splash || !s_booted || s_t0 == 0) {
        return;
    }
    age = now - s_t0;
    if (s_skip || age >= 4500u || (age >= 2000u && s->view.link_up)) {
        s_splash = false;
        ESP_LOGI(TAG, "splash done");
    }
}

void ui_lvgl_touch(int down) {
    if (down && s_splash) {
        s_skip = true;
    }
}

static void fill_sig(const Snap *s, int screen, Sig *sig) {
    int i;
    memset(sig, 0, sizeof(*sig));
    sig->screen = screen;
    sig->age_sec = (int)(s->age_ms / 1000u);
    sig->state = (int)s->view.state;
    sig->self_id = s->view.self_id;
    sig->selected = s->view.selected;
    sig->remote = s->view.remote;
    sig->volume = s->view.volume;
    sig->gain = s->view.mic_gain;
    sig->aec_ms = s->aec_ms;
    sig->bars = s->link.bars;
    sig->rssi = s->link.rssi_dbm;
    sig->bat_pct = s->bat.percent;
    sig->bat_mv = s->bat.millivolts;
    sig->nlegs = s->nlegs;
    sig->mic_armed = s->view.mic_armed ? 1 : 0;
    sig->mic_tx = s->view.mic_tx ? 1 : 0;
    sig->link_up = s->view.link_up ? 1 : 0;
    sig->audio_ok = s->view.audio_ok ? 1 : 0;
    sig->headset = s->headset ? 1 : 0;
    sig->tone = s->tone ? 1 : 0;
    sig->key_default = s->key_default ? 1 : 0;
    sig->pin_default = s->pin_default ? 1 : 0;
    sig->pass_set = s->pass_set ? 1 : 0;
    sig->speaker = s->view.speaker ? 1 : 0;
    snprintf(sig->status, sizeof(sig->status), "%s", s->view.status);
    name_of(s->view.remote_name, sig->remote_name, sizeof(sig->remote_name));
    snprintf(sig->ssid, sizeof(sig->ssid), "%s", s->ssid);
    snprintf(sig->ip, sizeof(sig->ip), "%s", s->ip);
    snprintf(sig->gw, sizeof(sig->gw), "%s", s->gw);
    snprintf(sig->mask, sizeof(sig->mask), "%s", s->mask);
    for (i = 0; i < 4; i++) {
        sig->online[i] = s->view.dir[i].online ? 1 : 0;
        name_of(s->view.dir[i].name, sig->names[i], sizeof(sig->names[i]));
        ic_format_ip(s->view.dir[i].ip, sig->ips[i], sizeof(sig->ips[i]));
    }
}

static void apply_snap(const Snap *s, int screen, uint32_t now) {
    if (screen != SCR_SPLASH) {
        paint_chrome(s, screen);
    }
    show_screen(screen);
    if (screen == SCR_SPLASH) {
        paint_splash(s);
        paint_splash_anim(now);
    } else if (screen == SCR_HOME) {
        paint_home(s);
    } else if (screen == SCR_FLOOR) {
        paint_floor(s);
    } else if (screen == SCR_CALLING) {
        paint_calling(s);
        paint_rings(now);
    } else if (screen == SCR_MUTED) {
        paint_muted(s);
    } else if (screen == SCR_LIVE) {
        paint_live(s);
    } else if (screen == SCR_PAGE) {
        paint_page(s, false);
    } else if (screen == SCR_PAGEALL) {
        paint_page(s, true);
    } else if (screen == SCR_RX) {
        paint_rx(s);
    } else if (screen == SCR_BUSY) {
        paint_busy(s);
    } else if (screen == SCR_LINK) {
        paint_link(s);
    } else if (screen == SCR_AUDIO) {
        paint_audio(s);
    } else if (screen == SCR_SETUP) {
        paint_setup(s);
    } else if (screen == SCR_POWER) {
        paint_power(s);
    } else if (screen == SCR_NET) {
        paint_net();
    }
}

/* Volume and gain already move their own knob. Repainting the whole
 * screen for that change invalidates every button, which is a full
 * frame on each finger step. */
static bool levels_only(const Sig *a, const Sig *b) {
    Sig left;
    Sig right;
    if (a->screen != b->screen) {
        return false;
    }
    left = *a;
    right = *b;
    left.volume = right.volume = 0;
    left.gain = right.gain = 0;
    left.aec_ms = right.aec_ms = 0;
    return memcmp(&left, &right, sizeof(Sig)) == 0;
}

static void paint_levels(const Snap *s) {
    sync_slider(s_home_slider, s->view.volume);
    sync_slider(s_mu_slider, s->view.volume);
    sync_slider(s_setup_slider_v, s->view.volume);
    sync_slider(s_setup_slider_g, s->view.mic_gain);
    vol_labels(s->view.volume);
    gain_label(s->view.mic_gain);
    aec_label(s->aec_ms);
}

static void refresh(uint32_t now) {
    Snap snap;
    Sig sig;
    int screen;
    bool changed;
    take_snap(now, &snap);
    if (strcmp(snap.view.status, "Busy") != 0) {
        s_dismiss = false;
    }
    splash_tick(now, &snap);
    screen = pick_screen(&snap);
    fill_sig(&snap, screen, &sig);
    changed = !s_have || memcmp(&sig, &s_sig, sizeof(sig)) != 0;
    if (changed) {
        if (s_have && levels_only(&sig, &s_sig)) {
            paint_levels(&snap);
        } else {
            apply_snap(&snap, screen, now);
        }
        s_sig = sig;
        s_have = true;
        s_anim = now;
    } else if ((uint32_t)(now - s_anim) >= 250u) {
        if (screen == SCR_SPLASH) {
            paint_splash_anim(now);
        } else if (screen == SCR_CALLING) {
            paint_rings(now);
        } else if (screen == SCR_LIVE) {
            paint_live_meter(&snap);
        } else if (screen == SCR_SETUP && s_setup_meter) {
            int level = snap.rms >= 4000u ? 4000 : (int)snap.rms;
            lv_bar_set_value(s_setup_meter, level, LV_ANIM_OFF);
        }
        s_anim = now;
    }
}

static lv_obj_t *view_create(void) {
    lv_obj_t *v = lv_obj_create(s_app);
    lv_obj_set_pos(v, 0, 0);
    lv_obj_set_size(v, PANEL_W, PANEL_H);
    plain(v);
    lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    return v;
}

static void style_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    plain(scr);
}

static lv_obj_t *meter_bar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 4000);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PRESS), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    return b;
}

static void build_bar(void) {
    lv_obj_t *bar = lv_obj_create(s_app);
    int i;
    static const int heights[] = {5, 9, 13, 18};
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, PANEL_W, 72);
    plain(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t *mark = lv_obj_create(bar);
        lv_obj_t *line = lv_obj_create(bar);
        lv_obj_set_pos(mark, 24, 28);
        lv_obj_set_size(mark, 10, 10);
        plain(mark);
        lv_obj_set_style_bg_color(mark, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mark, 2, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(line, 0, 71);
        lv_obj_set_size(line, PANEL_W, 1);
        plain(line);
        lv_obj_set_style_bg_color(line, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    s_name = label_at(bar, 44, 22, &lv_font_montserrat_20, C_TEXT, "STATION");
    lv_obj_set_style_text_letter_space(s_name, 1, 0);
    s_idlbl = label_at(bar, 180, 22, &lv_font_montserrat_20, C_MUTED, "100");
    s_pill = lv_obj_create(bar);
    lv_obj_set_size(s_pill, 148, 36);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 18);
    plain(s_pill);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(0x2C251E), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pill, 18, 0);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
    s_pill_lbl = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_pill_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_pill_lbl, 1, 0);
    lv_label_set_text(s_pill_lbl, "IDLE");
    lv_obj_clear_flag(s_pill_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_pill_lbl);
    for (i = 0; i < 4; i++) {
        s_bars[i] = lv_obj_create(bar);
        lv_obj_set_size(s_bars[i], 5, heights[i]);
        lv_obj_set_pos(s_bars[i], 780 + i * 8, 44 - heights[i]);
        plain(s_bars[i]);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(C_LINE), 0);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_bars[i], 1, 0);
        lv_obj_clear_flag(s_bars[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_rssi = label_at(bar, 818, 22, &lv_font_montserrat_16, C_MUTED, "down");
    {
        lv_obj_t *body = lv_obj_create(bar);
        lv_obj_t *nub;
        lv_obj_set_pos(body, 888, 28);
        lv_obj_set_size(body, 28, 14);
        plain(body);
        lv_obj_set_style_bg_color(body, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(body, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_border_width(body, 2, 0);
        lv_obj_set_style_radius(body, 3, 0);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        s_bat_fill = lv_obj_create(body);
        lv_obj_set_pos(s_bat_fill, 3, 3);
        lv_obj_set_size(s_bat_fill, 0, 8);
        plain(s_bat_fill);
        lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(C_GREEN), 0);
        lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_bat_fill, 1, 0);
        lv_obj_clear_flag(s_bat_fill, LV_OBJ_FLAG_CLICKABLE);
        nub = lv_obj_create(bar);
        lv_obj_set_pos(nub, 916, 32);
        lv_obj_set_size(nub, 3, 6);
        plain(nub);
        lv_obj_set_style_bg_color(nub, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(nub, 1, 0);
        lv_obj_clear_flag(nub, LV_OBJ_FLAG_CLICKABLE);
    }
    s_bat_lbl = label_at(bar, 928, 22, &lv_font_montserrat_16, C_TEXT, "USB");
}

static void build_home(lv_obj_t *v) {
    static const int xs[] = {24, 268, 512, 756};
    int i;
    lv_obj_t *strip;
    label_at(v, 24, 78, &lv_font_montserrat_28, C_TEXT, "");
    label_at(v, 24, 284, &lv_font_montserrat_14, C_MUTED, "");
    for (i = 0; i < 4; i++) {
        s_home_card[i] = make_station(v, xs[i], 96);
        bind_station(&s_home_card[i], (uint16_t)(IC_STATION_MIN + i), on_select);
    }
    s_call_b = make_btn(v, 24, 308, 184, 96, "Call", "Select a station", on_call, NULL);
    s_mic_b = make_btn(v, 220, 308, 184, 96, "Mic", "Off", on_mic, NULL);
    s_page_b = make_btn(v, 416, 308, 184, 96, "Page", "Select a floor", on_page, NULL);
    s_all_b = make_btn(v, 612, 308, 184, 96, "Page all", "101 / 102 / 103", on_page_all, NULL);
    s_end_b = make_btn(v, 808, 308, 192, 96, "End", "Idle", on_end, NULL);
    strip = card_at(v, 24, 428, 976, 148, C_SURF, 18);
    label_at(strip, 24, 14, &lv_font_montserrat_14, C_MUTED, "SPEAKER");
    s_home_vol = label_at(strip, 568, 14, &lv_font_montserrat_14, C_TEXT, "60");
    s_home_slider = lv_slider_create(strip);
    lv_obj_set_pos(s_home_slider, 24, 42);
    lv_obj_set_size(s_home_slider, 520, 8);
    style_slider(s_home_slider);
    lv_obj_add_event_cb(s_home_slider, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_home_slider, on_volume, LV_EVENT_RELEASED, NULL);
    s_home_headset = label_at(strip, 600, 26, &lv_font_montserrat_14, C_GREEN, "HEADSET WAITING");
    lv_obj_set_style_text_letter_space(s_home_headset, 1, 0);
    s_home_status = label_at(strip, 600, 58, &lv_font_montserrat_16, C_TEXT, "Speaker off  /  Mic off");
    clip_label(s_home_status, 250);
    label_at(strip, 24, 112, &lv_font_montserrat_14, C_MUTED,
             "Tap a station, then Call. Your own tile is not a destination.");
    make_btn(strip, 836, 48, 116, 64, "Setup", "", on_setup, NULL);
}

static void build_floor(lv_obj_t *v) {
    static const int xs[] = {24, 268, 512, 756};
    int i;
    lv_obj_t *strip;
    for (i = 0; i < 4; i++) {
        s_floor_card[i] = make_station(v, xs[i], 96);
        bind_station(&s_floor_card[i], (uint16_t)(IC_STATION_MIN + i), on_select);
    }
    s_fcall = make_btn(v, 24, 308, 300, 96, "Call", "Select a station", on_call, NULL);
    s_fmic = make_btn(v, 340, 308, 220, 96, "Mic", "Off", on_mic, NULL);
    s_fpage = make_btn(v, 576, 308, 210, 96, "Page", "Reception only", on_page, NULL);
    s_fend = make_btn(v, 802, 308, 198, 96, "End", "Idle", on_end, NULL);
    strip = card_at(v, 24, 428, 976, 148, C_SURF, 18);
    s_floor_title = label_at(strip, 24, 18, &lv_font_montserrat_22, C_AMBER, "Microphone off");
    {
        lv_obj_t *hint = label_at(strip, 24, 64, &lv_font_montserrat_16, C_MUTED,
                                  "Nothing is sent until a call is connected. The armed latch clears on hangup.");
        clip_label(hint, 580);
        hint = label_at(strip, 24, 96, &lv_font_montserrat_16, C_MUTED,
                        "Page stays locked here. Setup opens levels and Wi-Fi.");
        clip_label(hint, 580);
    }
    s_floor_bat = label_at(strip, 636, 62, &lv_font_montserrat_16, C_TEXT, "USB");
    make_btn(strip, 836, 48, 116, 64, "Setup", "", on_setup, NULL);
}

static lv_obj_t *ring_at(lv_obj_t *parent, int r) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_pos(o, 540 - r, 250 - r);
    plain(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_calling(lv_obj_t *v) {
    lv_obj_t *left = card_at(v, 24, 104, 620, 360, C_SURF, 22);
    lv_obj_t *note;
    lv_obj_t *foot;
    lv_obj_t *core;
    int i;
    static const int radii[] = {50, 34, 18};
    label_at(left, 32, 40, &lv_font_montserrat_14, C_AMBER, "CALLING");
    s_call_name = label_at(left, 32, 72, &lv_font_montserrat_48, C_TEXT, "");
    clip_label(s_call_name, 400);
    s_call_sub = label_at(left, 32, 166, &lv_font_montserrat_22, C_MUTED, "");
    clip_label(s_call_sub, 420);
    label_at(left, 32, 252, &lv_font_montserrat_20, C_TEXT, "The far station answers by itself.");
    label_at(left, 32, 288, &lv_font_montserrat_16, C_MUTED, "Its speaker comes on. Its microphone stays off.");
    for (i = 0; i < 3; i++) {
        s_ring[i] = ring_at(v, radii[i]);
    }
    core = dot_at(v, 528, 238, 24);
    lv_obj_set_style_bg_color(core, lv_color_hex(C_AMBER), 0);
    s_call_end = make_btn(v, 668, 104, 332, 220, "End", "Cancel this call", on_end, NULL);
    note = card_at(v, 668, 344, 332, 120, C_SURF, 18);
    label_at(note, 24, 28, &lv_font_montserrat_16, C_TEXT, "Gives up after 3 seconds");
    label_at(note, 24, 60, &lv_font_montserrat_16, C_MUTED, "Status becomes No answer");
    foot = card_at(v, 24, 484, 976, 92, C_SURF, 18);
    label_at(foot, 24, 16, &lv_font_montserrat_20, C_MUTED, "Mic stays off until you tap Talk on a connected call.");
    s_call_radio = label_at(foot, 24, 50, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_call_radio, 900);
}

static void build_muted(lv_obj_t *v) {
    lv_obj_t *left = card_at(v, 24, 96, 470, 300, C_SURF, 22);
    lv_obj_t *vol;
    lv_obj_t *foot;
    lv_obj_t *mark;
    mark = dot_at(left, 24, 28, 10);
    lv_obj_set_style_bg_color(mark, lv_color_hex(C_GREEN), 0);
    label_at(left, 44, 22, &lv_font_montserrat_14, C_GREEN, "CONNECTED");
    s_mu_name = label_at(left, 24, 64, &lv_font_montserrat_48, C_TEXT, "");
    clip_label(s_mu_name, 420);
    s_mu_clock = label_at(left, 24, 140, &lv_font_montserrat_22, C_MUTED, "0:00");
    s_mu_hear = label_at(left, 24, 222, &lv_font_montserrat_22, C_TEXT, "You can hear them.");
    clip_label(s_mu_hear, 420);
    label_at(left, 24, 258, &lv_font_montserrat_16, C_MUTED, "They cannot hear you yet.");
    s_mu_talk = make_btn(v, 514, 96, 486, 180, "Talk", "Microphone is off", on_mic, NULL);
    s_mu_end = make_btn(v, 514, 292, 232, 104, "End", "Hang up", on_end, NULL);
    vol = card_at(v, 762, 292, 238, 104, C_SURF2, 16);
    stroke(vol, C_LINE, 1);
    label_at(vol, 24, 28, &lv_font_montserrat_22, C_TEXT, "Volume");
    s_mu_vol = label_at(vol, 24, 62, &lv_font_montserrat_16, C_MUTED, "60");
    foot = card_at(v, 24, 416, 976, 160, C_SURF, 18);
    label_at(foot, 24, 16, &lv_font_montserrat_14, C_MUTED, "SPEAKER");
    s_mu_slider = lv_slider_create(foot);
    lv_obj_set_pos(s_mu_slider, 24, 42);
    lv_obj_set_size(s_mu_slider, 640, 28);
    style_slider(s_mu_slider);
    lv_obj_add_event_cb(s_mu_slider, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_mu_slider, on_volume, LV_EVENT_RELEASED, NULL);
    s_mu_gain = label_at(foot, 696, 28, &lv_font_montserrat_16, C_MUTED, "MIC GAIN 40");
    label_at(foot, 696, 64, &lv_font_montserrat_14, C_MUTED, "Armed while idle sends nothing.");
    label_at(foot, 696, 92, &lv_font_montserrat_14, C_MUTED, "End clears the mic on both sides.");
}

static void build_live(lv_obj_t *v) {
    lv_obj_t *left = card_at(v, 24, 96, 470, 300, C_ALERT, 22);
    lv_obj_t *foot;
    int i;
    static const int heights[] = {10, 17, 24, 31};
    stroke(left, C_RED, 2);
    label_at(left, 24, 28, &lv_font_montserrat_14, C_RED, "TRANSMITTING");
    s_live_name = label_at(left, 24, 64, &lv_font_montserrat_48, C_TEXT, "");
    clip_label(s_live_name, 420);
    s_live_clock = label_at(left, 24, 140, &lv_font_montserrat_22, C_MUTED, "0:00");
    label_at(left, 24, 200, &lv_font_montserrat_14, C_MUTED, "MIC LEVEL");
    for (i = 0; i < 16; i++) {
        int h = heights[i & 3];
        s_live_bar[i] = lv_obj_create(left);
        lv_obj_set_size(s_live_bar[i], 14, h);
        lv_obj_set_pos(s_live_bar[i], 24 + i * 22, 264 - h);
        plain(s_live_bar[i]);
        lv_obj_set_style_bg_color(s_live_bar[i], lv_color_hex(C_PRESS), 0);
        lv_obj_set_style_bg_opa(s_live_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_live_bar[i], 2, 0);
        lv_obj_clear_flag(s_live_bar[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_live_mic = make_btn(v, 514, 96, 486, 180, "Mute", "Tap to stop sending", on_mic, NULL);
    s_live_end = make_btn(v, 514, 292, 486, 104, "End", "Hang up both sides", on_end, NULL);
    foot = card_at(v, 24, 416, 976, 160, C_SURF, 18);
    label_at(foot, 24, 28, &lv_font_montserrat_22, C_TEXT, "They hear you. Their microphone does not follow yours.");
    label_at(foot, 24, 76, &lv_font_montserrat_16, C_MUTED, "Mute frames are silence, so the far speaker stays quiet without a click.");
    label_at(foot, 24, 108, &lv_font_montserrat_16, C_MUTED, "The live state is the only time voice leaves this station.");
}

static void build_page(lv_obj_t *v) {
    static const int xs[] = {24, 356, 688};
    int i;
    lv_obj_t *foot;
    s_pg_title = label_at(v, 24, 76, &lv_font_montserrat_36, C_TEXT, "Paging");
    clip_label(s_pg_title, 960);
    label_at(v, 24, 124, &lv_font_montserrat_16, C_MUTED, "One floor. A private call is left alone.");
    for (i = 0; i < 3; i++) {
        s_pg_card[i] = make_info_card(v, xs[i], 176, 312, 180);
    }
    s_pg_talk = make_btn(v, 24, 376, 312, 100, "Talk", "Mic is off", on_mic, NULL);
    s_pg_end = make_btn(v, 356, 376, 312, 100, "End page", "Stops this floor", on_end, NULL);
    s_pg_all = make_btn(v, 688, 376, 312, 100, "Page all", "Switch to all floors", on_page_all, NULL);
    foot = card_at(v, 24, 496, 976, 80, C_SURF, 16);
    s_pg_foot = label_at(foot, 24, 20, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_pg_foot, 920);
    label_at(foot, 24, 48, &lv_font_montserrat_14, C_MUTED,
             "Only station 100 can page. A floor that taps Page gets: Paging is reception only.");
}

static void build_page_all(lv_obj_t *v) {
    static const int xs[] = {24, 356, 688};
    int i;
    lv_obj_t *foot;
    s_pa_title = label_at(v, 24, 76, &lv_font_montserrat_36, C_TEXT, "All free floors");
    label_at(v, 24, 124, &lv_font_montserrat_16, C_MUTED, "Each floor gets its own stream. Busy floors are skipped.");
    for (i = 0; i < 3; i++) {
        s_pa_card[i] = make_info_card(v, xs[i], 176, 312, 180);
    }
    s_pa_talk = make_btn(v, 24, 376, 480, 100, "Talk", "Floors hear silence until you do", on_mic, NULL);
    s_pa_end = make_btn(v, 520, 376, 480, 100, "End page", "All floors drop", on_end, NULL);
    foot = card_at(v, 24, 496, 976, 80, C_SURF, 16);
    label_at(foot, 24, 20, &lv_font_montserrat_16, C_TEXT, "A floor already in a private call does not hear this page.");
    s_pa_foot = label_at(foot, 24, 48, &lv_font_montserrat_14, C_MUTED, "");
    clip_label(s_pa_foot, 920);
}

static void build_rx(lv_obj_t *v) {
    lv_obj_t *box = card_at(v, 24, 104, 976, 250, C_TEAL_BG, 22);
    lv_obj_t *foot;
    Btn leave;
    stroke(box, C_TEAL, 2);
    label_at(box, 32, 48, &lv_font_montserrat_16, C_TEAL, "RECEPTION IS PAGING");
    s_rx_title = label_at(box, 32, 80, &lv_font_montserrat_48, C_TEXT, "Speaker on");
    s_rx_sub = label_at(box, 32, 174, &lv_font_montserrat_22, C_MUTED, "");
    clip_label(s_rx_sub, 900);
    s_rx_lock = make_btn(v, 24, 376, 480, 120, "Mic locked", "Stays off during a page", NULL, NULL);
    tone(&s_rx_lock, T_DISABLED);
    leave = make_btn(v, 520, 376, 480, 120, "Leave", "Drops this page", on_end, NULL);
    tone(&leave, T_RED);
    foot = card_at(v, 24, 516, 976, 60, C_SURF, 14);
    s_rx_foot = label_at(foot, 24, 18, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_rx_foot, 920);
}

static void build_busy(lv_obj_t *v) {
    lv_obj_t *box = card_at(v, 24, 104, 976, 280, C_SURF, 22);
    lv_obj_t *foot;
    Btn back;
    s_busy_title = label_at(box, 32, 20, &lv_font_montserrat_48, C_TEXT, "Busy");
    clip_label(s_busy_title, 900);
    label_at(box, 32, 112, &lv_font_montserrat_22, C_MUTED, "The call was not started. Their conversation stays up.");
    s_busy_line = label_at(box, 32, 196, &lv_font_montserrat_20, C_TEXT, "");
    clip_label(s_busy_line, 900);
    s_busy_alt[0] = make_btn(v, 24, 412, 312, 100, "Station", "Online", on_busy_pick, (void *)(intptr_t)0);
    s_busy_alt[1] = make_btn(v, 356, 412, 312, 100, "Station", "Offline", on_busy_pick, (void *)(intptr_t)1);
    back = make_btn(v, 688, 412, 312, 100, "Back", "Stay idle", on_back_home, NULL);
    tone(&back, T_AMBER);
    foot = card_at(v, 24, 532, 976, 44, C_SURF, 12);
    label_at(foot, 24, 12, &lv_font_montserrat_16, C_MUTED, "A second private call is rejected. A page also skips a busy floor.");
}

static void build_link(lv_obj_t *v) {
    lv_obj_t *box = card_at(v, 24, 104, 976, 250, C_ALERT, 22);
    lv_obj_t *foot;
    int i;
    static const int xs[] = {24, 272, 520, 768};
    stroke(box, C_RED, 2);
    label_at(box, 32, 48, &lv_font_montserrat_16, C_RED, "WI-FI DOWN");
    s_link_ssid = label_at(box, 32, 76, &lv_font_montserrat_48, C_TEXT, "Wi-Fi name not set");
    clip_label(s_link_ssid, 880);
    s_link_msg = label_at(box, 32, 168, &lv_font_montserrat_20, C_MUTED, "");
    clip_label(s_link_msg, 880);
    label_at(v, 800, 210, &lv_font_montserrat_16, C_RED, "No signal");
    for (i = 0; i < 4; i++) {
        lv_obj_t *chip = card_at(v, xs[i], 376, 232, 110, C_SURF, 16);
        s_link_name[i] = label_at(chip, 20, 22, &lv_font_montserrat_20, C_MUTED, "");
        clip_label(s_link_name[i], 192);
        s_link_sub[i] = label_at(chip, 20, 58, &lv_font_montserrat_14, C_OFF, "offline");
    }
    foot = card_at(v, 24, 506, 680, 70, C_SURF, 14);
    label_at(foot, 24, 10, &lv_font_montserrat_16, C_TEXT, "Battery is separate from the radio.");
    s_link_foot = label_at(foot, 24, 36, &lv_font_montserrat_14, C_MUTED, "");
    clip_label(s_link_foot, 630);
    {
        Btn wifib = make_btn(v, 720, 506, 280, 70, "Set Wi-Fi", "Open network keyboard", on_wifi_nav, NULL);
        tone(&wifib, T_AMBER);
    }
}

static void build_audio(lv_obj_t *v) {
    lv_obj_t *box = card_at(v, 24, 104, 976, 280, C_ALERT, 22);
    lv_obj_t *foot;
    Btn blocked;
    Btn test;
    stroke(box, C_RED, 2);
    label_at(box, 32, 48, &lv_font_montserrat_16, C_RED, "HEADSET");
    label_at(box, 32, 76, &lv_font_montserrat_48, C_TEXT, "No headset");
    label_at(box, 32, 168, &lv_font_montserrat_22, C_MUTED, "Plug the adapter into the USB socket, not the UART socket.");
    label_at(box, 32, 208, &lv_font_montserrat_16, C_TEXT, "Calls and pages stay closed until both mic and headphone streams are up.");
    blocked = make_btn(v, 24, 408, 312, 100, "Call", "Blocked", NULL, NULL);
    tone(&blocked, T_DISABLED);
    test = make_btn(v, 356, 408, 312, 100, "Audio test", "Needs a stream", on_test, NULL);
    tone(&test, T_DIM);
    make_btn(v, 688, 408, 312, 100, "Setup", "Levels stay editable", on_setup, NULL);
    foot = card_at(v, 24, 528, 976, 48, C_SURF, 12);
    s_au_foot = label_at(foot, 24, 14, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_au_foot, 920);
}

static void build_setup(lv_obj_t *v) {
    Btn back;
    lv_obj_t *identity;
    lv_obj_t *edit_hint;
    card_at(v, 24, 92, 620, 484, C_SURF, 20);
    label_at(v, 52, 109, &lv_font_montserrat_14, C_MUTED, "SPEAKER VOLUME");
    s_setup_vol = lv_textarea_create(v);
    lv_obj_set_pos(s_setup_vol, 520, 90);
    lv_obj_set_size(s_setup_vol, 104, 38);
    style_ta(s_setup_vol);
    lv_obj_set_style_pad_hor(s_setup_vol, 8, 0);
    lv_obj_set_style_pad_ver(s_setup_vol, 6, 0);
    lv_textarea_set_one_line(s_setup_vol, true);
    lv_textarea_set_accepted_chars(s_setup_vol, "0123456789");
    lv_textarea_set_max_length(s_setup_vol, 3);
    lv_textarea_set_text(s_setup_vol, "60");
    lv_obj_add_event_cb(s_setup_vol, on_setup_field, LV_EVENT_ALL, NULL);
    s_setup_slider_v = lv_slider_create(v);
    lv_obj_set_pos(s_setup_slider_v, 52, 128);
    lv_obj_set_size(s_setup_slider_v, 540, 28);
    style_slider(s_setup_slider_v);
    lv_obj_add_event_cb(s_setup_slider_v, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_setup_slider_v, on_volume, LV_EVENT_RELEASED, NULL);
    label_at(v, 52, 175, &lv_font_montserrat_14, C_MUTED, "MIC GAIN");
    s_setup_gain = lv_textarea_create(v);
    lv_obj_set_pos(s_setup_gain, 520, 158);
    lv_obj_set_size(s_setup_gain, 104, 38);
    style_ta(s_setup_gain);
    lv_obj_set_style_pad_hor(s_setup_gain, 8, 0);
    lv_obj_set_style_pad_ver(s_setup_gain, 6, 0);
    lv_textarea_set_one_line(s_setup_gain, true);
    lv_textarea_set_accepted_chars(s_setup_gain, "0123456789");
    lv_textarea_set_max_length(s_setup_gain, 3);
    lv_textarea_set_text(s_setup_gain, "40");
    lv_obj_add_event_cb(s_setup_gain, on_setup_field, LV_EVENT_ALL, NULL);
    s_setup_slider_g = lv_slider_create(v);
    lv_obj_set_pos(s_setup_slider_g, 52, 196);
    lv_obj_set_size(s_setup_slider_g, 540, 28);
    style_slider(s_setup_slider_g);
    lv_obj_add_event_cb(s_setup_slider_g, on_gain, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_setup_slider_g, on_gain, LV_EVENT_RELEASED, NULL);
    label_at(v, 52, 255, &lv_font_montserrat_14, C_MUTED, "ECHO DELAY");
    s_aec_side = label_at(v, 500, 255, &lv_font_montserrat_14, C_TEXT, "40 ms");
    make_btn(v, 52, 290, 80, 64, "-", "", on_aec_step, (void *)(intptr_t)-20);
    s_aec_lbl = lv_textarea_create(v);
    lv_obj_set_pos(s_aec_lbl, 148, 290);
    lv_obj_set_size(s_aec_lbl, 280, 64);
    style_ta(s_aec_lbl);
    lv_obj_set_style_pad_hor(s_aec_lbl, 12, 0);
    lv_obj_set_style_pad_ver(s_aec_lbl, 8, 0);
    lv_textarea_set_one_line(s_aec_lbl, true);
    lv_textarea_set_accepted_chars(s_aec_lbl, "0123456789");
    lv_textarea_set_max_length(s_aec_lbl, 3);
    lv_textarea_set_text(s_aec_lbl, "40");
    lv_obj_add_event_cb(s_aec_lbl, on_setup_field, LV_EVENT_ALL, NULL);
    label_at(v, 228, 320, &lv_font_montserrat_14, C_MUTED, "Applies while idle");
    make_btn(v, 444, 290, 80, 64, "+", "", on_aec_step, (void *)(intptr_t)20);
    label_at(v, 52, 376, &lv_font_montserrat_14, C_MUTED, "MIC LEVEL");
    s_setup_meter = meter_bar(v, 52, 406, 540, 16, C_TEAL);
    label_at(v, 52, 436, &lv_font_montserrat_14, C_MUTED, "Local only. The meter does not arm the microphone.");
    {
        Btn testb = make_btn(v, 52, 478, 250, 72, "Audio test", "Idle tone, not sent", on_test, NULL);
        tone(&testb, T_TEAL);
    }
    {
        Btn powerb = make_btn(v, 318, 478, 250, 72, "Power", "Wi-Fi and battery", on_power, NULL);
        tone(&powerb, T_SURFACE);
    }
    identity = card_at(v, 664, 92, 336, 300, C_SURF, 20);
    label_at(v, 688, 123, &lv_font_montserrat_14, C_AMBER, "THIS STATION");
    s_id_name = label_at(v, 688, 152, &lv_font_montserrat_28, C_TEXT, "");
    clip_label(s_id_name, 280);
    s_id_num = label_at(v, 688, 196, &lv_font_montserrat_20, C_MUTED, "");
    s_id_ip = label_at(v, 688, 240, &lv_font_montserrat_20, C_TEXT, "");
    s_id_gw = label_at(v, 688, 280, &lv_font_montserrat_16, C_MUTED, "");
    s_id_mask = label_at(v, 688, 308, &lv_font_montserrat_16, C_MUTED, "");
    edit_hint = label_at(v, 688, 345, &lv_font_montserrat_14, C_AMBER, "Edit station name and IP");
    lv_obj_add_flag(edit_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(edit_hint, on_net_nav, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(identity, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(identity, on_net_nav, LV_EVENT_CLICKED, NULL);
    back = make_btn(v, 860, 108, 116, 40, "Back", "", on_back_home, NULL);
    lv_obj_set_style_text_font(back.title, &lv_font_montserrat_16, 0);
    s_warn = card_at(v, 664, 408, 336, 168, C_WARM, 20);
    s_setup_kb = lv_keyboard_create(v);
    lv_obj_set_pos(s_setup_kb, 0, 380);
    lv_obj_set_size(s_setup_kb, PANEL_W, 220);
    style_keyboard(s_setup_kb);
    lv_keyboard_set_mode(s_setup_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(s_setup_kb, on_setup_keyboard, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_setup_kb, on_setup_keyboard, LV_EVENT_CANCEL, NULL);
    show(s_setup_kb, false);
    label_at(s_warn, 24, 32, &lv_font_montserrat_14, C_AMBER, "DEFAULT KEY");
    label_at(s_warn, 24, 62, &lv_font_montserrat_16, C_TEXT, "Change the shared key");
    label_at(s_warn, 24, 90, &lv_font_montserrat_16, C_TEXT, "and the web PIN before");
    label_at(s_warn, 24, 118, &lv_font_montserrat_16, C_TEXT, "this station is used.");
}

static void build_power(lv_obj_t *v) {
    int i;
    static const int xs[] = {48, 288, 528, 768};
    lv_obj_t *bat;
    lv_obj_t *icon;
    lv_obj_t *nub;
    card_at(v, 24, 92, 470, 320, C_SURF, 22);
    label_at(v, 48, 118, &lv_font_montserrat_14, C_AMBER, "BATTERY");
    icon = lv_obj_create(v);
    lv_obj_set_pos(icon, 48, 160);
    lv_obj_set_size(icon, 250, 110);
    plain(icon);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_border_width(icon, 3, 0);
    lv_obj_set_style_radius(icon, 16, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_fill = lv_obj_create(icon);
    lv_obj_set_pos(s_pwr_fill, 14, 14);
    lv_obj_set_size(s_pwr_fill, 0, 82);
    plain(s_pwr_fill);
    lv_obj_set_style_bg_color(s_pwr_fill, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_bg_opa(s_pwr_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pwr_fill, 10, 0);
    lv_obj_clear_flag(s_pwr_fill, LV_OBJ_FLAG_CLICKABLE);
    nub = lv_obj_create(v);
    lv_obj_set_pos(nub, 298, 196);
    lv_obj_set_size(nub, 16, 38);
    plain(nub);
    lv_obj_set_style_bg_color(nub, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nub, 4, 0);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_pct = label_at(v, 48, 258, &lv_font_montserrat_48, C_TEXT, "USB");
    s_pwr_volt = label_at(v, 230, 292, &lv_font_montserrat_28, C_MUTED, "no pack");
    label_at(v, 48, 362, &lv_font_montserrat_16, C_MUTED, "PH2.0 3.7 V cell.");
    make_btn(v, 300, 348, 160, 48, "Back", "", on_back_setup, NULL);
    card_at(v, 514, 92, 486, 320, C_SURF, 22);
    label_at(v, 538, 118, &lv_font_montserrat_14, C_AMBER, "WI-FI");
    s_pwr_ssid = label_at(v, 538, 150, &lv_font_montserrat_36, C_TEXT, "");
    clip_label(s_pwr_ssid, 430);
    s_pwr_rssi = label_at(v, 538, 218, &lv_font_montserrat_16, C_MUTED, "down");
    s_pwr_pass = label_at(v, 538, 274, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_pwr_pass, 430);
    label_at(v, 538, 310, &lv_font_montserrat_14, C_MUTED, "2.4 GHz only. Client isolation must be off.");
    {
        Btn wifib = make_btn(v, 538, 344, 240, 52, "Set Wi-Fi", "Change network", on_wifi_nav, NULL);
        tone(&wifib, T_AMBER);
    }
    bat = card_at(v, 24, 428, 976, 148, C_SURF, 18);
    (void)bat;
    for (i = 0; i < 4; i++) {
        s_pwr_name[i] = label_at(v, xs[i], 454, &lv_font_montserrat_16, C_TEXT, "");
        clip_label(s_pwr_name[i], 210);
        s_pwr_ip[i] = label_at(v, xs[i], 482, &lv_font_montserrat_14, C_MUTED, "");
        s_pwr_state[i] = label_at(v, xs[i], 512, &lv_font_montserrat_14, C_MUTED, "");
    }
}

static void build_wifi(lv_obj_t *v) {
    Btn hide;
    Btn save;
    label_at(v, 48, 84, &lv_font_montserrat_36, C_TEXT, "Wi-Fi");
    label_at(v, 220, 112, &lv_font_montserrat_16, C_MUTED,
             "Tap a box. Empty password means an open network.");
    label_at(v, 48, 156, &lv_font_montserrat_14, C_TEAL, "NETWORK NAME");
    s_ssid_ta = lv_textarea_create(v);
    lv_obj_set_pos(s_ssid_ta, 48, 184);
    lv_obj_set_size(s_ssid_ta, 928, 52);
    style_ta(s_ssid_ta);
    lv_textarea_set_one_line(s_ssid_ta, true);
    lv_textarea_set_max_length(s_ssid_ta, 32);
    lv_textarea_set_placeholder_text(s_ssid_ta, "SSID");
    lv_obj_add_event_cb(s_ssid_ta, on_ta, LV_EVENT_ALL, NULL);
    label_at(v, 48, 244, &lv_font_montserrat_14, C_TEAL, "PASSWORD");
    s_pass_ta = lv_textarea_create(v);
    lv_obj_set_pos(s_pass_ta, 48, 274);
    lv_obj_set_size(s_pass_ta, 760, 52);
    style_ta(s_pass_ta);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_max_length(s_pass_ta, 63);
    lv_textarea_set_placeholder_text(s_pass_ta, "Password");
    lv_obj_add_event_cb(s_pass_ta, on_ta, LV_EVENT_ALL, NULL);
    hide = make_btn(v, 824, 274, 152, 52, "Hide", "", on_hide, NULL);
    s_hide_lbl = hide.title;
    s_wifi_err = label_at(v, 48, 332, &lv_font_montserrat_16, C_MUTED, "");
    clip_label(s_wifi_err, 490);
    save = make_btn(v, 560, 328, 200, 52, "Save", "", on_wifi_save, NULL);
    tone(&save, T_AMBER);
    make_btn(v, 776, 328, 200, 52, "Back", "", on_back_setup, NULL);
    s_kb = lv_keyboard_create(v);
    lv_obj_set_pos(s_kb, 0, 392);
    lv_obj_set_size(s_kb, PANEL_W, 208);
    style_keyboard(s_kb);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_ssid_ta);
    lv_obj_add_event_cb(s_kb, on_input_keyboard, LV_EVENT_CANCEL, NULL);
    show(s_kb, false);
}

static void build_net(lv_obj_t *v) {
    Btn save;
    int i;
    static const int xs[] = {24, 271, 518, 765};
    label_at(v, 24, 78, &lv_font_montserrat_32, C_TEXT, "Station");
    save = make_btn(v, 620, 74, 180, 48, "Save", "", on_net_save, NULL);
    tone(&save, T_AMBER);
    make_btn(v, 816, 74, 180, 48, "Back", "", on_back_setup, NULL);
    for (i = 0; i < 4; i++) {
        s_net_sid[i] = make_btn(v, xs[i], 132, 230, 52, k_net_title[NET_P0 + i], k_role[i], on_net_sid,
                                (void *)(intptr_t)(IC_STATION_MIN + i));
    }
    for (i = 0; i < NET_N; i++) {
        int col = i % 4;
        int row = i / 4;
        s_net_field[i] = make_btn(v, xs[col], 196 + row * 64, 230, 56, k_net_title[i], "", on_net_field,
                                  (void *)(intptr_t)i);
    }
    s_net_cap = label_at(v, 24, 330, &lv_font_montserrat_14, C_TEAL, "Station name");
    s_net_ta = lv_textarea_create(v);
    lv_obj_set_pos(s_net_ta, 24, 350);
    lv_obj_set_size(s_net_ta, 640, 44);
    style_ta(s_net_ta);
    lv_textarea_set_one_line(s_net_ta, true);
    lv_textarea_set_max_length(s_net_ta, IC_NAME_LEN - 1);
    lv_obj_add_event_cb(s_net_ta, on_ta, LV_EVENT_ALL, NULL);
    s_net_err = label_at(v, 680, 360, &lv_font_montserrat_14, C_MUTED, "");
    clip_label(s_net_err, 320);
    s_net_kb = lv_keyboard_create(v);
    lv_obj_set_pos(s_net_kb, 0, 408);
    lv_obj_set_size(s_net_kb, PANEL_W, 192);
    style_keyboard(s_net_kb);
    lv_keyboard_set_textarea(s_net_kb, s_net_ta);
    lv_obj_add_event_cb(s_net_kb, on_input_keyboard, LV_EVENT_CANCEL, NULL);
    show(s_net_kb, false);
}

static void build_splash(void) {
    int i;
    static const int ys[] = {340, 380, 420, 460};
    static const char *keys[] = {"Panel", "Headset", "Wi-Fi", "Battery"};
    static const uint32_t cols[] = {C_GREEN, C_AMBER, C_TEXT, C_TEXT};
    lv_obj_t *mark;
    lv_obj_t *track;
    s_splash_scr = lv_obj_create(NULL);
    style_screen(s_splash_scr);
    lv_obj_add_flag(s_splash_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_splash_scr, on_splash_tap, LV_EVENT_CLICKED, NULL);
    mark = lv_obj_create(s_splash_scr);
    lv_obj_set_pos(mark, 48, 164);
    lv_obj_set_size(mark, 4, 272);
    plain(mark);
    lv_obj_set_style_bg_color(mark, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    label_at(s_splash_scr, 80, 164, &lv_font_montserrat_48, C_TEXT, "LAN INTERCOM");
    s_splash_sub = label_at(s_splash_scr, 80, 232, &lv_font_montserrat_22, C_MUTED, "Starting");
    s_splash_kick = label_at(s_splash_scr, 80, 292, &lv_font_montserrat_14, C_AMBER, "STARTING");
    lv_obj_set_style_text_letter_space(s_splash_kick, 2, 0);
    for (i = 0; i < 4; i++) {
        s_splash_key[i] = label_at(s_splash_scr, 80, ys[i], &lv_font_montserrat_20, cols[i], keys[i]);
        s_splash_val[i] = label_at(s_splash_scr, 260, ys[i], &lv_font_montserrat_20, C_MUTED, "");
        clip_label(s_splash_val[i], 640);
    }
    track = lv_obj_create(s_splash_scr);
    lv_obj_set_pos(track, 80, 530);
    lv_obj_set_size(track, 864, 6);
    plain(track);
    lv_obj_set_style_bg_color(track, lv_color_hex(C_PRESS), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    s_splash_fill = lv_obj_create(s_splash_scr);
    lv_obj_set_pos(s_splash_fill, 80, 530);
    lv_obj_set_size(s_splash_fill, 0, 6);
    plain(s_splash_fill);
    lv_obj_set_style_bg_color(s_splash_fill, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_bg_opa(s_splash_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_splash_fill, 3, 0);
    lv_obj_clear_flag(s_splash_fill, LV_OBJ_FLAG_CLICKABLE);
    label_at(s_splash_scr, 80, 548, &lv_font_montserrat_16, C_MUTED,
             "Radio, audio, and the touch panel come up before the directory.");
}

void ui_lvgl_build(void) {
    int i;
    if (s_built || !lvgl_port_ready()) {
        return;
    }
    build_splash();
    s_app = lv_obj_create(NULL);
    style_screen(s_app);
    for (i = SCR_HOME; i < SCR_COUNT; i++) {
        s_views[i] = view_create();
    }
    build_home(s_views[SCR_HOME]);
    build_floor(s_views[SCR_FLOOR]);
    build_calling(s_views[SCR_CALLING]);
    build_muted(s_views[SCR_MUTED]);
    build_live(s_views[SCR_LIVE]);
    build_page(s_views[SCR_PAGE]);
    build_page_all(s_views[SCR_PAGEALL]);
    build_rx(s_views[SCR_RX]);
    build_busy(s_views[SCR_BUSY]);
    build_link(s_views[SCR_LINK]);
    build_audio(s_views[SCR_AUDIO]);
    build_setup(s_views[SCR_SETUP]);
    build_power(s_views[SCR_POWER]);
    build_wifi(s_views[SCR_WIFI]);
    build_net(s_views[SCR_NET]);
    build_bar();
    lv_scr_load(s_splash_scr);
    s_on_app = false;
    s_splash = true;
    s_booted = false;
    s_built = true;
    {
        char nb[IC_NAME_LEN + 1];
        char line[64];
        name_of(g_app.cfg.name, nb, sizeof(nb));
        snprintf(line, sizeof(line), "%s  -  station %u", nb[0] ? nb : "Station",
                 (unsigned)g_app.cfg.station_id);
        set_text(s_splash_sub, line);
        set_text(s_splash_kick, "STARTING");
    }
}

void ui_lvgl_boot(uint32_t now_ms) {
    if (!s_built) {
        return;
    }
    s_t0 = now_ms;
    s_booted = true;
    s_splash = true;
    s_skip = false;
    s_nav = NAV_AUTO;
    s_have = false;
    if (g_app.lock) {
        refresh(now_ms);
    }
}

void ui_lvgl_poll(uint32_t now_ms) {
    if (!s_built || !lvgl_port_ready()) {
        return;
    }
    lvgl_port_lock();
    if (s_booted && g_app.lock) {
        refresh(now_ms);
    } else if ((uint32_t)(now_ms - s_anim) >= 250u) {
        paint_splash_anim(now_ms);
        s_anim = now_ms;
    }
    (void)lv_timer_handler();
    lvgl_port_unlock();
}
