#include "config_store.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>

#include <Preferences.h>

#include "esp_log.h"

static const char *TAG = "ic_cfg";
static const char *NVS_NS = "ic";

static const char *k_names[IC_STATION_COUNT] = {
    "Reception", "Floor 1", "Floor 2", "Floor 3"
};

static const char k_default_key[IC_KEY_LEN] = {
    'o', 'f', 'f', 'i', 'c', 'e', '-', 'i', 'n', 't', 'e', 'r', 'c', 'o', 'm', '!'
};

static const char *k_peer_keys[IC_STATION_COUNT] = {"p0", "p1", "p2", "p3"};

static void copy_key(uint8_t dst[IC_KEY_LEN]) {
    const char *k = IC_BUILD_KEY;
    if (strlen(k) != IC_KEY_LEN) {
        memcpy(dst, k_default_key, IC_KEY_LEN);
        return;
    }
    memcpy(dst, k, IC_KEY_LEN);
}

static void copy_pin(char dst[IC_PIN_LEN + 1]) {
    const char *p = IC_BUILD_PIN;
    if (!ic_config_pin_ok(p)) {
        snprintf(dst, IC_PIN_LEN + 1, "1234");
        return;
    }
    snprintf(dst, IC_PIN_LEN + 1, "%s", p);
}

static bool visible_text(const char *text, size_t min_len, size_t max_len) {
    size_t n;
    size_t i;
    if (!text) {
        return false;
    }
    n = strlen(text);
    if (n < min_len || n > max_len) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static void copy_build_wifi(ic_config_t *cfg) {
    const char *ssid = IC_BUILD_SSID;
    const char *pass = IC_BUILD_WIFI_PASS;
    cfg->ssid[0] = 0;
    cfg->wifi_pass[0] = 0;
    if (visible_text(ssid, 1, 32)) {
        snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", ssid);
    }
    if (pass[0] == 0 || visible_text(pass, 8, 63)) {
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", pass);
    }
}

void ic_config_defaults(ic_config_t *cfg) {
    int self;
    int i;
    memset(cfg, 0, sizeof(*cfg));
    cfg->station_id = (uint16_t)IC_BUILD_STATION;
    if (cfg->station_id < IC_STATION_MIN || cfg->station_id > IC_STATION_MAX) {
        cfg->station_id = IC_STATION_MIN;
    }
    self = (int)(cfg->station_id - IC_STATION_MIN);
    snprintf(cfg->name, sizeof(cfg->name), "%s", k_names[self]);
    cfg->ip = 0xC0A80100u + cfg->station_id;
    cfg->gateway = 0xC0A80101u;
    cfg->netmask = 0xFFFFFF00u;
    copy_key(cfg->key);
    cfg->volume = 60;
    cfg->mic_gain = 40;
    copy_pin(cfg->pin);
    for (i = 0; i < IC_STATION_COUNT; i++) {
        cfg->peer_ip[i] = 0xC0A80100u + (uint32_t)(IC_STATION_MIN + i);
    }
    cfg->peer_ip[self] = cfg->ip;
    cfg->aec_samples = 320;
    copy_build_wifi(cfg);
}

bool ic_parse_ip(const char *text, uint32_t *host_order) {
    unsigned a, b, c, d;
    char tail;
    if (!text || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    *host_order = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

void ic_format_ip(uint32_t host_order, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u",
             (unsigned)((host_order >> 24) & 255u),
             (unsigned)((host_order >> 16) & 255u),
             (unsigned)((host_order >> 8) & 255u),
             (unsigned)(host_order & 255u));
}

bool ic_config_name_ok(const char *name) {
    size_t n;
    size_t i;
    if (!name) {
        return false;
    }
    n = strlen(name);
    if (n < 1 || n >= IC_NAME_LEN) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
        if (c == '<' || c == '>' || c == '&' || c == '"' || c == '\'') {
            return false;
        }
    }
    return true;
}

bool ic_config_pin_ok(const char *pin) {
    size_t n;
    size_t i;
    if (!pin) {
        return false;
    }
    n = strlen(pin);
    if (n < 4 || n > IC_PIN_LEN) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = pin[i];
        int digit = (c >= '0' && c <= '9');
        int upper = (c >= 'A' && c <= 'Z');
        int lower = (c >= 'a' && c <= 'z');
        if (!digit && !upper && !lower) {
            return false;
        }
    }
    return true;
}

bool ic_config_ssid_ok(const char *ssid) {
    return visible_text(ssid, 1, 32);
}

bool ic_config_wifi_pass_ok(const char *pass) {
    if (!pass) {
        return false;
    }
    if (pass[0] == 0) {
        return true;
    }
    return visible_text(pass, 8, 63);
}

bool ic_config_key_ok(const char *key) {
    size_t i;
    if (!key || strlen(key) != IC_KEY_LEN) {
        return false;
    }
    for (i = 0; i < IC_KEY_LEN; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

bool ic_config_key_is_default(const ic_config_t *cfg) {
    return memcmp(cfg->key, k_default_key, IC_KEY_LEN) == 0;
}

bool ic_config_pin_is_default(const ic_config_t *cfg) {
    return strcmp(cfg->pin, "1234") == 0;
}

static bool read_all(Preferences &prefs, ic_config_t *cfg) {
    uint16_t sid;
    uint16_t aec;
    uint8_t vol;
    uint8_t gain;
    char name[IC_NAME_LEN];
    char pin[IC_PIN_LEN + 1];
    uint8_t key[IC_KEY_LEN];
    uint32_t ip, gw, mask, peers[IC_STATION_COUNT];
    int i;

    if (!prefs.isKey("ver") || prefs.getUInt("ver", 0) != IC_CFG_VER) {
        return false;
    }
    if (!prefs.isKey("sid")) {
        return false;
    }
    sid = prefs.getUShort("sid", 0);
    if (sid < IC_STATION_MIN || sid > IC_STATION_MAX) {
        return false;
    }
    if (!prefs.isKey("name") || prefs.getString("name", name, sizeof(name)) == 0 ||
        !ic_config_name_ok(name)) {
        return false;
    }
    if (!prefs.isKey("ip") || !prefs.isKey("gw") || !prefs.isKey("mask")) {
        return false;
    }
    ip = prefs.getUInt("ip", 0);
    gw = prefs.getUInt("gw", 0);
    mask = prefs.getUInt("mask", 0);
    if (!prefs.isKey("k") || prefs.getBytesLength("k") != IC_KEY_LEN ||
        prefs.getBytes("k", key, IC_KEY_LEN) != IC_KEY_LEN) {
        return false;
    }
    if (!prefs.isKey("vol") || !prefs.isKey("gain")) {
        return false;
    }
    vol = prefs.getUChar("vol", 255);
    gain = prefs.getUChar("gain", 255);
    if (vol > 100 || gain > 100) {
        return false;
    }
    if (!prefs.isKey("pin") || prefs.getString("pin", pin, sizeof(pin)) == 0 ||
        !ic_config_pin_ok(pin)) {
        return false;
    }
    if (!prefs.isKey("aec")) {
        return false;
    }
    aec = prefs.getUShort("aec", 0);
    if (aec > 1500) {
        return false;
    }
    for (i = 0; i < IC_STATION_COUNT; i++) {
        if (!prefs.isKey(k_peer_keys[i])) {
            return false;
        }
        peers[i] = prefs.getUInt(k_peer_keys[i], 0);
        if (peers[i] == 0) {
            return false;
        }
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->station_id = sid;
    snprintf(cfg->name, sizeof(cfg->name), "%s", name);
    cfg->ip = ip;
    cfg->gateway = gw;
    cfg->netmask = mask;
    memcpy(cfg->key, key, IC_KEY_LEN);
    cfg->volume = vol;
    cfg->mic_gain = gain;
    snprintf(cfg->pin, sizeof(cfg->pin), "%s", pin);
    cfg->aec_samples = aec;
    for (i = 0; i < IC_STATION_COUNT; i++) {
        cfg->peer_ip[i] = peers[i];
    }
    cfg->peer_ip[sid - IC_STATION_MIN] = cfg->ip;
    return true;
}

/* SSID and password are extra keys. A record saved before Wi-Fi still loads.
 * Missing keys use board_config.h. wop 1 means an open network on purpose.
 */
static void load_wifi(Preferences &prefs, ic_config_t *cfg) {
    char ssid[33];
    char pass[64];

    copy_build_wifi(cfg);
    if (prefs.isKey("ssid") && prefs.getString("ssid", ssid, sizeof(ssid)) > 0 &&
        ic_config_ssid_ok(ssid)) {
        snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", ssid);
    }
    if (prefs.isKey("wop") && prefs.getUChar("wop", 0) == 1) {
        cfg->wifi_pass[0] = 0;
    } else if (prefs.isKey("wpw") && prefs.getString("wpw", pass, sizeof(pass)) > 0 &&
               ic_config_wifi_pass_ok(pass)) {
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", pass);
    }
}

bool ic_config_load(ic_config_t *cfg) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) {
        ESP_LOGW(TAG, "no saved config");
        ic_config_defaults(cfg);
        return false;
    }
    if (!read_all(prefs, cfg)) {
        prefs.end();
        ESP_LOGW(TAG, "NVS config incomplete, using defaults");
        ic_config_defaults(cfg);
        return false;
    }
    load_wifi(prefs, cfg);
    prefs.end();
    return true;
}

bool ic_config_save(const ic_config_t *cfg) {
    Preferences prefs;
    ic_config_t copy = *cfg;
    bool ok = true;
    int i;

    copy.peer_ip[copy.station_id - IC_STATION_MIN] = copy.ip;
    if (!prefs.begin(NVS_NS, false)) {
        ESP_LOGE(TAG, "nvs open failed");
        return false;
    }
    ok = prefs.putUShort("sid", copy.station_id) == sizeof(uint16_t) && ok;
    ok = prefs.putString("name", copy.name) > 0 && ok;
    ok = prefs.putUInt("ip", copy.ip) == sizeof(uint32_t) && ok;
    ok = prefs.putUInt("gw", copy.gateway) == sizeof(uint32_t) && ok;
    ok = prefs.putUInt("mask", copy.netmask) == sizeof(uint32_t) && ok;
    ok = prefs.putBytes("k", copy.key, IC_KEY_LEN) == IC_KEY_LEN && ok;
    ok = prefs.putUChar("vol", copy.volume) == sizeof(uint8_t) && ok;
    ok = prefs.putUChar("gain", copy.mic_gain) == sizeof(uint8_t) && ok;
    ok = prefs.putString("pin", copy.pin) > 0 && ok;
    ok = prefs.putUShort("aec", copy.aec_samples) == sizeof(uint16_t) && ok;
    for (i = 0; i < IC_STATION_COUNT; i++) {
        ok = prefs.putUInt(k_peer_keys[i], copy.peer_ip[i]) == sizeof(uint32_t) && ok;
    }
    if (copy.ssid[0] == 0) {
        if (prefs.isKey("ssid")) {
            ok = prefs.remove("ssid") && ok;
        }
    } else {
        ok = prefs.putString("ssid", copy.ssid) == strlen(copy.ssid) && ok;
    }
    if (copy.wifi_pass[0] == 0) {
        ok = prefs.putUChar("wop", 1) == sizeof(uint8_t) && ok;
        if (prefs.isKey("wpw")) {
            ok = prefs.remove("wpw") && ok;
        }
    } else {
        ok = prefs.putUChar("wop", 0) == sizeof(uint8_t) && ok;
        ok = prefs.putString("wpw", copy.wifi_pass) == strlen(copy.wifi_pass) && ok;
    }
    ok = prefs.putUInt("ver", IC_CFG_VER) == sizeof(uint32_t) && ok;
    prefs.end();
    if (!ok) {
        ESP_LOGE(TAG, "nvs write failed");
    }
    return ok;
}
