#pragma once

#include "ic_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IC_CFG_VER 1
#define IC_PIN_LEN 8

typedef struct ic_config {
    uint16_t station_id;
    char name[IC_NAME_LEN];
    uint32_t ip;
    uint32_t gateway;
    uint32_t netmask;
    uint8_t key[IC_KEY_LEN];
    uint8_t volume;
    uint8_t mic_gain;
    char pin[IC_PIN_LEN + 1];
    uint32_t peer_ip[IC_STATION_COUNT];
    uint16_t aec_samples;
    char ssid[33];
    char wifi_pass[64];
} ic_config_t;

void ic_config_defaults(ic_config_t *cfg);
bool ic_config_load(ic_config_t *cfg);
bool ic_config_save(const ic_config_t *cfg);

bool ic_parse_ip(const char *text, uint32_t *host_order);
void ic_format_ip(uint32_t host_order, char *out, size_t cap);
bool ic_config_name_ok(const char *name);
bool ic_config_pin_ok(const char *pin);
bool ic_config_key_ok(const char *key);
bool ic_config_ssid_ok(const char *ssid);
bool ic_config_wifi_pass_ok(const char *pass);
bool ic_config_key_is_default(const ic_config_t *cfg);
bool ic_config_pin_is_default(const ic_config_t *cfg);

#ifdef __cplusplus
}
#endif
