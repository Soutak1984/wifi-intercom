# Hardware

One station uses the parts below. Build two before building four.

## Bill of materials (one station)

| Qty | Part | Notes |
| --- | --- | --- |
| 1 | Waveshare ESP32-S3-Touch-LCD-7B | ESP32-S3-WROOM-1-N16R8. Two Type-C sockets: UART for upload and the console, USB for the headset. The on-chip 2.4 GHz radio is the network. The Arduino sketch needs OPI PSRAM so the 1024×600 panel can start. |
| 1 | 2.4 GHz access point | Same LAN for every station. The ESP32-S3 does not join a 5 GHz-only network. Client isolation must be off. |
| 1 | Portronics iKonnect C Pro, or another USB-C headset adapter with a real DAC | USB Audio Class 1.0, headphone out and microphone in. Plugs into the board USB Type-C socket. A passive analogue USB-C adapter will not enumerate. |
| 1 | TRRS headset (CTIA) | Headphones plus mic. A TRS headphone plug has no microphone. |
| 1 | 5 V supply, 1 A is enough | Powers the board. The USB socket's VBUS is the board 5 V rail, so the adapter is powered from the board. |
| 1 | Single-cell 3.7 V lithium battery, optional | PH2.0 header. The panel shows its level. Do not parallel packs. |

Calls, paging, and the microphone are on the 1024 × 600 touchscreen. Do not wire buttons or LEDs to the RGB pins. Those pins are the panel.

## Power

- ESP32-S3 GPIO is 3.3 V.
- Headset adapter VBUS: 5 V from the station supply. Do not take that 5 V from an ESP32 GPIO.
- Do not connect 5 V to an ESP32 GPIO.

The headphone is quiet while idle because the firmware sends digital silence. It sends audio during a connected call, a received page, or `audiotest`. There is no amplifier shutdown pin.

## Wiring

```
Board USB Type-C                 Portronics iKonnect C Pro
(silk "USB", GPIO 19 D- and GPIO 20 D+)

Board UART Type-C                PC, for upload and the ic> console
(silk "UART", CH343, GPIO 43 TX and GPIO 44 RX, 115200)

On-chip 2.4 GHz radio            Access point for the four stations
```

The adapter plugs into the USB socket that is already on the board. Do not add another USB connector. At boot the sketch writes the IO expander (I2C address 0x24, GPIO 8 SDA, GPIO 9 SCL) so EXIO5 is low. That connects GPIO 19 and GPIO 20 to the USB socket. EXIO5 high would connect those pins to the CAN transceiver instead.

USB CDC On Boot stays Disabled. The USB socket is not a serial port and is not used for settings. Settings are the UART Type-C port and the web page.

To upload, use the UART Type-C port. USB Mode is "Hardware CDC and JTAG", and USB CDC On Boot is Disabled. Hold BOOT and tap RESET if the COM port does not appear. The headset can stay plugged into the USB socket while you upload.

Do not use GPIO 0, 1, 2, 3, 5, 7, 10, 14, 17, 18, 21, 38–42, or 45–48 as ordinary GPIO. Those are the RGB panel. GPIO 43 and 44 are the UART Type-C console. GPIO 19 and 20 are the onboard USB socket. GPIO 8 and 9 are the IO expander and the touch controller. GPIO 4 is the touch interrupt.

## GPIO map

| Function | GPIO | Direction |
| --- | --- | --- |
| USB D- | 19 | onboard USB Type-C, headset host |
| USB D+ | 20 | onboard USB Type-C, headset host |
| IO expander SDA | 8 | I2C, EXIO5 selects USB or CAN |
| IO expander SCL | 9 | I2C, EXIO5 selects USB or CAN |
| UART0 TX | 43 | UART Type-C console output, 115200 |
| UART0 RX | 44 | UART Type-C console input, 115200 |
| Touch interrupt | 4 | GT911, polled by the UI task |
| Battery sense | expander ADC 0x06 | single-cell pack on the PH2.0 header |

GPIO 8 and 9 are the expander and the GT911. EXIO1 resets the touch controller, EXIO2 is the backlight, EXIO3 resets the LCD, and EXIO5 stays low for the USB socket. There is no SPI Ethernet wiring. The touchscreen layout is `design/LAN-Intercom.desigma`. The sketch draws that layout.

## USB audio

The adapter must speak USB Audio Class 1.0 and expose both a headphone stream and a microphone stream. The Portronics iKonnect C Pro is sold as a DAC that also answers calls, which is that shape: the 3.5 mm jack is for a CTIA headset. UAC2-only devices are ignored. This Arduino core reads at most 256 bytes of configuration descriptor; a headset whose descriptor is longer will not enumerate.

The host accepts 16-bit or 24-bit PCM, mono or stereo, and prefers 16 kHz, then 8 kHz, 32 kHz, and 48 kHz. Full-speed packets have to fit the USB FIFO. A 48 kHz 16-bit stereo stream is 192 bytes each millisecond, which the firmware asks for with a custom FIFO split. Capture and playback are resampled to 8 kHz mono before G.711. The host counts samples at that nominal rate. It does not read an asynchronous feedback endpoint, so a DAC with its own clock can drift and click on a long call. A headset that uses one clock for the microphone and the headphone stays much closer. `status` prints the rates the adapter actually opened, for example `usb spk 48000Hz/2ch mic 48000Hz/1ch`.

For echo, set `aec` to 20 or 40 and place a new call after each change. The stored default is 40 ms. A headset path is shorter than a loudspeaker in the room.
