# Limitations and later work

## Not done yet

- Host tests in `tests/test_core.c` have not been executed here.
- `idf.py build` has not been run against ESP-IDF 6.0.2 in this session.
- No board was wired, so latency, echo, loss, heap, and soak are unmeasured.
- The product is not ready to install until those three gaps are closed. See [TESTING.md](TESTING.md).

## Security

- One 16-byte key is shared by all four stations. Whoever can read flash or NVS can impersonate a station and send mute or audio frames. The key is not encrypted at rest.
- There is no certificate, no per-call key, and no forward secrecy. A recorded call can be decrypted later with the same key.
- Signalling headers (type, station ids, sequence, boot id) are authenticated but the header fields are not encrypted. The audio payload and the short signalling payload are encrypted.
- A captured HELLO that still has a valid HMAC can change the stored boot id for that station. A captured INVITE cannot, while HELLOs from the live boot are still arriving.
- HTTP basic authentication has no TLS. The PIN crosses the LAN in a reversible encoding. The web server cannot arm the microphone, but anyone who learns the PIN can change the key or the address.
- Rate limits are per source address in a small table. They are not a defence against a switched-LAN attacker who already has the key.
- Wi-Fi retries drop some frames before they look like bad HMACs. The HMAC is for authenticity, not for a noisy radio. A 2.4 GHz network with client isolation, or a 5 GHz-only access point, cannot carry these calls.

## Protocol

- One active private call per station. A new call is rejected. There is no call waiting and no barge-in.
- Paging is unicast from station 100 only, to at most three floors. A floor that is already in a call does not hear the page.
- Setup gives up after 3 seconds. A peer that stops sending media or keepalives is dropped after 5 seconds. The dropped side shows `Call ended`.
- The jitter window is 8 frames. A burst deeper than that is concealed, then the buffer restarts.
- G.711 µ-law at 8 kHz is the only codec. There is no Opus and no G.722 in this image.
- The NLMS canceller is a single 40 ms bulk delay plus 128 taps (16 ms). It will not track a delay that jumps when the DMA or the jitter buffer restarts, and it will not cancel a strong acoustic path by itself. Adaptation pauses when the microphone is much louder than the far-end reference, which is the usual double-talk compromise, not a full double-talk detector.

## Hardware and operations

- Audio is a USB Audio Class 1.0 headset adapter on the ESP32-S3 full-speed host port. The level is the headphone output of that adapter, not a ceiling-speaker amplifier. Volume in the firmware is digital and comes before the adapter.
- The adapter has to expose both a microphone and a headphone stream. Playback-only DAC dongles, passive analogue USB-C adapters, and UAC2-only devices do not run this firmware.
- The Arduino USB host reads 256 bytes of configuration descriptor. A longer headset descriptor fails enumeration in this core.
- Playback uses the nominal sample rate. There is no asynchronous feedback endpoint, so a DAC with its own clock can drift and click on a long call.
- GPIO 19 and GPIO 20 are the onboard USB Type-C socket. USB CDC is left off, so that socket is not used for settings. The console is the UART Type-C port. EXIO5 must stay low or the same pins are the CAN transceiver.
- The RGB pins are the panel. Do not add buttons or LEDs on that bus. The sketch starts that panel only when OPI PSRAM is enabled. The on-glass letters are Montserrat, drawn by LVGL 8.4. Each Desigma frame is its own screen, including the floor idle screen and the Wi-Fi keyboard. Wi-Fi credentials can be typed on the glass. The shared key and the web PIN cannot. The sketch links with the ESP32 3.3.11 toolchain when PSRAM is OPI. It has not been flashed or tried on a board.
- GPIO 48 is a green data bit on this panel.
- All four boards ship as station 100 until NVS is written. That is safe only if they are provisioned one at a time.
- The Arduino sketch uses the on-chip Wi-Fi radio and does not download `espressif/w5500`. Modem sleep is off while the station runs. A busy 2.4 GHz channel can still add jitter to the 20 ms audio frames.

## Possible later work

- Per-station keys or a short pairing step, still with no central server.
- A measured AEC delay per room, stored in NVS after a local impulse test.
- A line-level or speaker output if a headset jack is not the installation, and a hardware volume knob if the web control is not acceptable on the wall.
- A small display for the directory. The protocol already carries the station name.
- Four-board soak and a written latency number to replace the budget in the architecture note.
