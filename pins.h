#pragma once

/* Waveshare ESP32-S3-Touch-LCD-7B. The 1024×600 panel uses the RGB bus,
 * so those pins are not buttons or LEDs.
 * GPIO 4 is the GT911 interrupt. GPIO 8 (SDA) and GPIO 9 (SCL) are the
 * expander and the touch controller. GPIO 19 and GPIO 20 are the USB
 * Type-C headset socket (EXIO5 low selects USB, high selects CAN).
 * GPIO 43 (TX) and GPIO 44 (RX) are UART0 on the upload socket.
 * GPIO 11-13 are the TF slot. GPIO 15 and 16 are RS485.
 * Calls, paging, and the microphone are the touchscreen, not GPIO.
 *
 * Expander 0x24, once the panel driver is up:
 *   EXIO1 touch reset, EXIO2 backlight, EXIO3 LCD reset, EXIO5 USB/CAN,
 *   EXIO6 panel power (drive high).
 * EXIO5 stays low so the headset socket stays on USB.
 */

#define PIN_IOEXP_SDA 8
#define PIN_IOEXP_SCL 9

#define PIN_USB_DM 19
#define PIN_USB_DP 20

#define PIN_TOUCH_INT 4

#define PIN_LCD_VSYNC 3
#define PIN_LCD_DE 5
#define PIN_LCD_PCLK 7
#define PIN_LCD_HSYNC 46

/* RGB565 on the wire, least significant bit first: B3..B7, G2..G7, R3..R7. */
#define PIN_LCD_D0 14
#define PIN_LCD_D1 38
#define PIN_LCD_D2 18
#define PIN_LCD_D3 17
#define PIN_LCD_D4 10
#define PIN_LCD_D5 39
#define PIN_LCD_D6 0
#define PIN_LCD_D7 45
#define PIN_LCD_D8 48
#define PIN_LCD_D9 47
#define PIN_LCD_D10 21
#define PIN_LCD_D11 1
#define PIN_LCD_D12 2
#define PIN_LCD_D13 42
#define PIN_LCD_D14 41
#define PIN_LCD_D15 40
