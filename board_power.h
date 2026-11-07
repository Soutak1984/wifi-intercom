#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the single-cell pack on the PH2.0 header through the IO expander ADC.
 * millivolts is the estimated cell voltage. percent is 0-100, or -1 when the
 * reading is too low to be a pack (USB supply only). Returns false if the
 * expander does not answer. The percent is a straight map from 3.30 V to 4.20 V,
 * not a fuel-gauge curve. */
bool board_battery_sample(int *millivolts, int *percent);

#ifdef __cplusplus
}
#endif
