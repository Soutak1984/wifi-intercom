# Architecture

Four identical stations share one firmware image. Identity, addresses, the shared key, volume, microphone gain, the web PIN, and the echo-canceller delay live in NVS. Kconfig supplies those values only on a blank NVS.

There is no process that must stay up for calls to work. An access point only forwards frames between the stations. DNS is not used. The Arduino sketch sends those frames on Wi-Fi.

## Modules

| Path | Role |
| --- | --- |
| `components/ic_core` | Crypto, G.711, jitter, NLMS, packet codec, call state. No ESP-IDF calls. Host tests compile this directory. |
| `net_arduino.cpp` | Wi-Fi station, static IPv4, signalling socket, signalling task |
| `main/audio_io.c` | Headset audio, jitter playout, AEC, media socket. This sketch uses `audio_port.c` and `usb_uac.cpp` |
| `ui_port.c` | Touch commands, protocol tick, Wi-Fi bars, battery |
| `panel_7b.cpp` | ST7262 RGB panel, backlight, GT911 |
| `ui_lvgl.cpp` | One LVGL view per Desigma frame, including floor idle and the Wi-Fi keyboard |
| `main/web_ui.c` | Status and configuration only |
| `main/diag.c` | UART0 console provisioning. This sketch uses `console_arduino.cpp` |
| `main/config_nvs.c` | NVS load and save |

The core object is static. Call-path audio buffers are static. lwIP and the HTTP server allocate at start, not per call.

## Tasks

- `ic_audio` reads 20 ms of microphone audio, plays the jitter buffer, and sends media. Priority 10.
- `ic_sig` receives signalling and transmits the outbound queue. Priority 8.
- `ic_ui` applies touch commands, draws the panel, reads the GT911, and runs the protocol tick every 50 ms. Priority 5.
- The console REPL and the HTTP server use their own tasks.

A mutex covers the core. A task copies the outbound signalling list, releases the mutex, then queues the packets. The audio task does not hold the mutex across USB audio or `sendto`.

The watchdog is subscribed by the audio, signalling, and UI tasks. A failed crypto self-test never starts the network. The UART log is the fault indication. The call and microphone lamps are on the touchscreen, not on GPIO.

## Signalling

UDP port 47000. Magic `INC1`, version 1, 36-byte header, payload, 16-byte HMAC-SHA256. Payloads are AES-128-CTR. The nonce is session, sequence, boot id, and a domain byte so signalling and media do not reuse a keystream. Little-endian explicit bytes, not a packed struct.

Message types: HELLO, INVITE, ACCEPT, REJECT, ACK, BYE, BYE_ACK, KEEPALIVE, PAGE_START, PAGE_ACCEPT, PAGE_END, PAGE_END_ACK.

HELLO every 2 s. A peer is online if a valid HELLO arrived within 7 s. INVITE and PAGE_START retransmit every 300 ms and give up after 3 s. An up call sends a keepalive every 1 s and ends if nothing arrives for 5 s. A link-down event ends the local call immediately and attempts BYE.

Each station has one private call, or up to three page legs. A second private call is rejected busy. If both sides invite each other, the lower station id keeps its outgoing session and the higher id accepts the lower id's session. A page does not start unless the station is idle, so it does not cut off a private call. A busy page target is dropped; the other floors still receive the page.

Every valid INVITE or PAGE_START is accepted at once. Accept turns the speaker on for a private call or a received page. It does not arm the microphone. A received page forces the microphone off and ignores Enable Mic.

## Microphone

`mic_is_live()` is the only transmit decision: link up, audio healthy, mic armed, state connected or paging, and at least one leg up. The touchscreen shows the microphone as live from that bit. Armed while idle does not transmit. While idle, the status line says `Mic armed` or `Mic off`. During calling, a call, or paging, the status line stays on that call. The web page still shows MICROPHONE ARMED or MICROPHONE LIVE from the same flags.

Ending a call, losing the link, an audio fault, a remote reboot, or entering page-receive clears the latch. No signalling packet, web request, or console command sets the latch. Only the touchscreen Mic control toggles it.

While a private call or an outgoing page is up, the station still sends 20 ms frames. If the microphone is off, those frames are marked mute and filled with µ-law silence (`0xFF`). The receiver plays digital silence and does not decode the payload.

## Media

UDP port 47010. Magic `MEC1`, 28-byte header, 160 µ-law samples (20 ms at 8 kHz), 16-byte HMAC. The USB headset is resampled to 8 kHz mono, then DC-blocked, gained, and passed through NLMS. A noise gate attenuates frames whose post-AEC RMS is under 180. The headphone path is volume, then the AEC reference, then resampled to the adapter's rate. Idle and muted calls send digital silence, so the headphones stay quiet without a shutdown pin.

The jitter buffer holds 8 frames and starts playback at 4 frames (80 ms). A missing frame repeats the previous frame scaled by 7/8. A new session resets the jitter buffer and the canceller.

Gain 25 is about unity. The default gain is 40 (about 1.6×). Default volume is 60. Default AEC bulk delay is 320 samples, which is 40 ms at 8 kHz. That delay is a model of speaker-to-microphone latency, not an extra buffer inserted on purpose.

One-way latency budget, not a measurement: 20 ms frame + 80 ms jitter target + 20–40 ms of DMA + under 1 ms on a local switch. Expect roughly 120–160 ms before acoustic delay. The canceller is NLMS with 128 taps. It is not claimed to remove echo on a ceiling speaker until that room is measured.

## Security

Stations 100–103 share a 16-byte key. Packets are encrypt-then-MAC. A bad HMAC gets no reply. After eight bad HMACs from one address in a second, that address is ignored for 30 s. More than 40 signalling packets per second from one address are dropped. Sequence numbers are per boot. A replay or a duplicate does not create a new call. Ended sessions stay in a tombstone list for 30 s. Media from a tombstoned session is discarded.

HELLO may refresh a peer's address from the UDP source after the HMAC checks. Every other message must come from the directory address. A station id outside 100–103 with a valid HMAC gets REJECT and no state change.

Limits are listed in [LIMITATIONS.md](LIMITATIONS.md). The short version: the shared key is the whole trust boundary, it is stored in plaintext NVS, there is no perfect forward secrecy, and a captured HELLO can still move a peer's boot id.

## Group paging

Only station 100 may page. PAGE sends to the selected floor. PAGE ALL sends a separate unicast stream to 101, 102, and 103. Receivers auto-accept, turn the speaker on, and keep their microphones off. There is no multicast.

## Touch panel

The panel is 1024 × 600. `panel_7b.cpp` starts the ST7262 with the Waveshare 7B timings, keeps expander EXIO5 low so the headset socket stays USB, releases the LCD reset on EXIO3, and turns the backlight on through EXIO2. The GT911 is polled on the same I2C bus. Touch actions enter through `ui_select`, `ui_call`, `ui_end`, `ui_mic`, `ui_page`, and `ui_volume_step`. The UI task applies them and `ui_lvgl.cpp` draws the screen with LVGL 8.4. Each frame in `design/LAN-Intercom.desigma` is one LVGL view: splash, reception home, calling, call muted, call live, page one floor, page all, page receive, busy, link down, setup, power, floor idle, audio fault, and the Wi-Fi keyboard. Reception idles on the home screen. Any other station idles on the floor screen. The Wi-Fi keyboard opens from Setup on either idle screen, then Power, then Set Wi-Fi, and also from Set Wi-Fi on the link-down screen. The on-glass letters are Montserrat. The shared key and the web PIN are not on the glass. The framebuffer needs OPI PSRAM. Without it the log says `OPI PSRAM required` and the glass stays dark.

- A station tile selects 100, 101, 102, or 103. This station's own tile is not a destination.
- Call places the selected call.
- End sends BYE or PAGE_END, stops local audio, and returns to idle. If the peer is unreachable, the local side still clears. End while idle sends nothing.
- Mic toggles the local latch. On a connected call the control reads Talk or Mute.
- Page and Page all are on the reception home. On a floor, Page stays dim. A tap still reaches the core, which answers that paging is reception only.

The status bar shows the call state, Wi-Fi bars with RSSI, and the battery percent. There is no call LED and no microphone LED. The microphone control is filled only while audio is actually being transmitted. Wi-Fi down and a pack under 15% are drawn in red. Boot is a splash that lists the panel, the headset, the Wi-Fi reading, and the battery reading.

Volume, microphone gain, and the echo delay are on the Setup screen. A change there is applied and saved. The same values still apply from the console and the web page. The console still needs `save` before a reboot keeps a console change. The shared key and the web PIN are not on the panel.

## Web and console

HTTP port 80, basic authentication, user `admin`. The page shows status and saves configuration. It cannot place a call or arm the microphone. Blank key and PIN fields keep the current secret. A successful save reboots.

Console commands: `status`, `id`, `name`, `ip`, `ssid`, `pass`, `gw`, `mask`, `peer`, `key`, `pin`, `vol`, `gain`, `aec`, `save`, `reboot`, `audiotest`, `miclevel`. `audiotest` plays a local tone only while idle and does not transmit it. `miclevel` prints a local RMS and does not arm the microphone. Address, name, id, Wi-Fi, key, and PIN changes apply on the next boot. Volume and gain apply immediately and are kept if `save` is used. A blank Wi-Fi password field on the web page keeps the current password.
