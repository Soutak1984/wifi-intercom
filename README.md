# LAN intercom for the Arduino IDE

This folder is the same four-station office intercom as the ESP-IDF project in the repository root. It is a separate sketch. It does not replace that tree.

Open `lan_intercom.ino`. The sketch folder name and the `.ino` name match, which is what the Arduino IDE requires.

An Arduino station and an ESP-IDF station can call each other. Both use UDP port 47000 for signalling and UDP port 47010 for µ-law audio, the same shared key, and the same station numbers. The call rules are the same source file (`session.c`).

Copies of the project notes are in this sketch folder:

- [docs/PROCEDURE.md](docs/PROCEDURE.md) — how to set up a station, call, and page
- [docs/NETWORK.md](docs/NETWORK.md) — addresses and provisioning
- [docs/HARDWARE.md](docs/HARDWARE.md) — wiring and GPIO
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — call rules
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- [docs/LIMITATIONS.md](docs/LIMITATIONS.md)
- [docs/TESTING.md](docs/TESTING.md)
- [docs/BUILD.md](docs/BUILD.md) — ESP-IDF build; the Arduino board settings are below
- [docs/FEASIBILITY.md](docs/FEASIBILITY.md)

This sketch has not been compiled here. The command runner on this machine fails before a compiler starts (Windows error 216). Do not treat the sketch as a finished, tested build until Arduino IDE, or `arduino-cli`, compiles it and the call behaviour has been tried on hardware.

## Board package

Install **esp32 by Espressif Systems**, version **3.3.0 or newer**, from the Boards Manager. The calls in this sketch were checked against the headers in Arduino-ESP32 3.3.6. The USB headset host uses the FIFO configuration in that core. Core 2.x cannot build this sketch.

Board settings for the Waveshare ESP32-S3-Touch-LCD-7B:

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| USB Mode | Hardware CDC and JTAG. Do not select USB-OTG (TinyUSB); that holds the USB peripheral and the headset host cannot start |
| USB CDC On Boot | Disabled. The USB Type-C socket is the headset adapter, not a serial port |
| Flash size | 16MB |
| Partition scheme | Huge APP (3MB No OTA), if the default scheme reports that the sketch is too large |
| PSRAM | OPI PSRAM |
| Upload speed | 921600, or slower if the upload fails |

The board is a Waveshare ESP32-S3-Touch-LCD-7B (ESP32-S3-WROOM-1 N16R8). The 1024×600 framebuffer does not fit in internal RAM, so PSRAM must be OPI. With PSRAM left disabled the log says `OPI PSRAM required` and the glass stays dark. The sketch links with ESP32 3.3.11 when Tools → PSRAM is OPI PSRAM. It has not been flashed or seen on a board here.

Upload and the `ic>` console use the Type-C port marked UART. That port is a USB-UART chip on GPIO 43 (TX) and GPIO 44 (RX) at 115200. It is not USB CDC. The Type-C port marked USB is only for the Portronics adapter. Leave USB CDC On Boot disabled so that port stays free. If the UART port does not appear, hold BOOT, tap RESET, then release BOOT. The web page is the other way to change settings.

This sketch joins a 2.4 GHz access point with `WiFi.begin()`. It does not use a W5500, `ETH.h`, or an Ethernet shield library. The ESP32-S3 radio is 2.4 GHz only.

## Identity

Edit `board_config.h` before the first upload if you want a station other than 100. A blank board boots as station 100, name Reception, address 192.168.1.100. The other defaults are 101 at .101, 102 at .102, and 103 at .103. Gateway 192.168.1.1, mask 255.255.255.0.

The NVS namespace is `ic`, with the same key names and types as the ESP-IDF firmware (`ver`, `sid`, `name`, `ip`, `gw`, `mask`, `k`, `vol`, `gain`, `pin`, `p0`–`p3`, `aec`). Uploading this sketch without erasing flash keeps an identity that the ESP-IDF build already saved. Wi-Fi is stored beside that record as `ssid`, `wpw`, and `wop`, so an older identity still loads. `ver` stays 1.

Erase All Flash Before Sketch Upload returns the board to `board_config.h`. Provision one erased board at a time, or every erased board will boot as station 100.

The shared key must be exactly 16 characters and the same on every station. The default is `office-intercom!`. The web user is `admin` and the default PIN is `1234`. Change both before real use. The web page and the serial console can store a new key and PIN. They cannot place a call or turn the microphone on.

Serial prompt is `ic>` on the UART Type-C port. Commands: `status`, `id`, `name`, `ip`, `ssid`, `pass`, `gw`, `mask`, `peer`, `key`, `pin`, `vol`, `gain`, `aec`, `save`, `reboot`, `audiotest`, `miclevel`, `help`. `id`, `name`, addresses, the Wi-Fi name and password, `key`, and `pin` apply after `save` and `reboot`. `pass open` joins a network with no password. The password is not printed. Volume and gain apply immediately. `audiotest` plays a local tone only while idle and does not transmit. A blank board has no SSID until you set one, unless `IC_BUILD_SSID` in `board_config.h` is filled in before upload.

The status page is `http://<station-ip>/` on port 80, HTTP basic authentication, realm `intercom`.

## Wiring

Same map as `main/pins.h` and `docs/HARDWARE.md`.

| Function | GPIO |
| --- | --- |
| Onboard USB D-, D+ | 19, 20 |
| IO expander and touch SDA, SCL | 8, 9 |
| UART Type-C console TX, RX | 43, 44 |
| Panel touch interrupt | 4 |

The 1024×600 panel owns the RGB bus, including the pins an earlier sketch used for buttons and LEDs. This sketch does not read those buttons or drive those LEDs. `panel_7b.cpp` starts the ST7262, turns the backlight on through expander EXIO2, and reads the GT911. `ui_lvgl.cpp` draws the intercom screens into that panel with LVGL 8.4. Calls, paging, the microphone, volume, Wi-Fi strength, battery level, and the Wi-Fi name and password are on the touchscreen. Each frame in `design/LAN-Intercom.desigma` is its own screen: splash, reception home, calling, muted call, live call, page one floor, page all, page receive, busy, link down, setup, power, floor idle, audio fault, and the Wi-Fi keyboard. The on-glass type is Montserrat. The shared key and the web PIN stay off the glass. Audio in and out is the Portronics iKonnect C Pro plugged into the board USB Type-C socket. Plug a TRRS headset into the adapter. At boot the sketch sets expander pin EXIO5 low so that socket is USB rather than CAN, and it keeps that pin low while it drives the backlight and the LCD reset. There is no second USB connector and no Ethernet module. Wiring is in `docs/HARDWARE.md`.

## Behaviour that matches the ESP-IDF firmware

Any station can call any other station. An incoming call is accepted and the speaker is enabled. The microphone stays off until Talk is tapped during a connected call or while this station is the page source. The screen shows the microphone as live only while audio is actually being sent. End hangs up both sides and forces the mic off. A busy station rejects the new call. Reception (station 100) can page one floor or all floors. A page does not interrupt a private call, and receivers keep their microphones off. One active call per station.

Signalling is unicast UDP with AES-128-CTR and a 16-byte truncated HMAC-SHA256. There is no SIP server, registrar, or cloud service.

## Differences from the ESP-IDF build

Wi-Fi station mode uses the on-chip radio. The stored static address, gateway, and mask are applied before `WiFi.begin()`, and modem sleep is turned off so 20 ms audio frames are not held for a power-save window. Link up is “got an IP”. Link down is a disconnect or a lost address. Those edges are applied on the UI task, so a duplicate event does not reset hello timing. The MAC inside each authenticated packet is still the ESP32 base MAC from `esp_read_mac(ESP_MAC_BASE)`. An ESP-IDF station that still uses a W5500 can call this board when that wire and this access point are the same subnet and the static addresses match. The access point must forward client-to-client traffic.

UDP uses `lwip_socket`, `lwip_recvfrom`, and `lwip_sendto`. Arduino-ESP32 3.3.6 builds lwIP with `LWIP_COMPAT_SOCKETS` set to 0, so the plain BSD names are not the lwIP functions. The sockets are set non-blocking with `lwip_ioctl` and `FIONBIO`.

The web page is `WebServer` on its own task. The pages, fields, and basic-auth check are the same. A blank key or PIN keeps the current value. Saving still reboots.

Audio is USB Audio Class 1.0 in both directions. The ESP32-S3 is the host. Samples are converted to 8 kHz mono for the same µ-law, jitter buffer, and NLMS canceller. The headphone output is digital silence unless a call, a received page, or `audiotest` should be heard.

If the crypto self-test fails, the network stays down and the UART log says `crypto self-test failed`. That loop resets the Arduino task watchdog so the board does not reboot over the fault.

## What this does not prove

No compiler was run. Latency, echo, packet loss, heap, and long-call stability have not been measured on this sketch. The echo canceller is the same NLMS filter as the ESP-IDF build and still needs a hardware trial on the real speaker. The shared key is the whole trust boundary: anyone who has it can speak as a station, and a captured HELLO can still change the recorded boot id. See `docs/LIMITATIONS.md` in the repository root.
