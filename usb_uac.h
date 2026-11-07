#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB Audio Class 1.0 headset on the ESP32-S3 host port.
 * read/write are 8 kHz mono PCM, the same rate the rest of the intercom uses.
 */

bool usb_uac_start(void);
bool usb_uac_installed(void);
bool usb_uac_streaming(void);
bool usb_uac_read(int16_t *mono, int n, int timeout_ms);
bool usb_uac_write(const int16_t *mono, int n, int timeout_ms);
void usb_uac_status(char *dst, size_t cap);
/* callbacks is how many microphone URBs completed. bytes is payload received.
 * status is the last URB status, or -1 before the first completion. */
void usb_uac_mic_io(int *callbacks, int *bytes, int *status);

#ifdef __cplusplus
}
#endif
