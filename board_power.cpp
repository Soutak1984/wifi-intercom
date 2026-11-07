#include "board_power.h"

#include <Arduino.h>
#include <Wire.h>

bool board_battery_sample(int *millivolts, int *percent) {
    int adc;
    int mv;
    int pct;
    uint8_t lo;
    uint8_t hi;
    if (!millivolts || !percent) {
        return false;
    }
    Wire.beginTransmission(0x24);
    Wire.write(0x06);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom((uint8_t)0x24, (uint8_t)2) != 2) {
        return false;
    }
    lo = (uint8_t)Wire.read();
    hi = (uint8_t)Wire.read();
    adc = (int)lo | ((int)hi << 8);
    if (adc < 0) {
        return false;
    }
    if (adc > 1023) {
        adc = 1023;
    }
    /* 10-bit reading, 3.3 V reference, 3:1 divider across the cell. */
    mv = adc * 3300 * 3 / 1023;
    *millivolts = mv;
    if (mv < 2500) {
        *percent = -1;
        return true;
    }
    pct = (mv - 3300) * 100 / 900;
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    *percent = pct;
    return true;
}
