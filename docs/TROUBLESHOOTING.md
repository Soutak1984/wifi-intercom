# Troubleshooting

## The LCD stays dark

The sketch does drive the 7B. A dark glass means the panel did not finish starting.

- Arduino Tools → PSRAM must be **OPI PSRAM**. The IDE default is Disabled, and that build keeps the glass dark. QSPI PSRAM is the wrong chip. Flash Size 16MB. Flash Mode stays **QIO**, not the OPI flash-mode entry. USB CDC On Boot Disabled. USB Mode Hardware CDC and JTAG. The log line `OPI PSRAM required` means the picture cannot be allocated.
- `panel: expander up, USB selected` means the backlight pin is under sketch control. `panel: ST7262 up, backlight on` means the RGB driver started and EXIO2 was set high. EXIO5 is held low so the headset socket stays USB.
- `panel: IO expander did not answer` means the chip at I2C 0x24 did not ack. The backlight and the LCD reset both go through that chip.
- `panel: ST7262 init failed` means the RGB driver returned an error after PSRAM was present. The timings are the Waveshare 7B 1024×600 set, not the 800×480 panel.
- `GT911 did not answer` leaves the picture up and the touch dead. Power-cycle once. The reset is expander EXIO1 and the interrupt is GPIO 4.

## The old picture stays in the corners

Pale edges and a shadow of the previous screen in the corners come from the backlight boost and from drawing into the buffer the panel is scanning.

The expander PWM register is active-low. Duty 1 is full bright. Duty 0 or 255 drops the boost converter, which washes the four edges and leaves a ghost of the last frame. The sketch now writes duty 1, the same value the Fire Alarm panel uses on this glass.

The picture is drawn into the framebuffer that is not on screen, then handed over with `draw_bitmap`. The bounce buffer invalidates its cache, matching that panel. Both framebuffers are cleared before the first frame, so a swap cannot show an unwritten corner.

## The log says crypto self-test failed

The SHA-256, HMAC, or AES-128 known-answer check failed. The network is left down so the board cannot talk. Reflash a good image. Do not bypass the check. There is no lamp for this fault.

## The board never appears on the network

- `status` prints `wifi ssid not set` until `ssid`, `pass`, `save`, and `reboot`. The log line `Wi-Fi SSID is not set` means the same thing.
- The radio is 2.4 GHz only. A 5 GHz-only access point never associates. `wifi down` on `status`, and `wifi bars 0`, means there is no association or no address.
- The log line `wifi join <name>` means the stack started. `wifi up` with an address means the static address is in use. `address on the air does not match the stored static address` means the interface did not take `ip`, `gw`, and `mask`.
- `wifi down <reason>` names the last disconnect. A wrong password is an authentication failure. The password is not printed.
- Two boards still at 192.168.1.100 will fight over that address. Provision one at a time.
- An access point that isolates clients will show link up and every peer offline.
- Erasing flash returns a board to station 100 and clears the saved Wi-Fi name. That is expected.

## Wi-Fi bars stay empty

Wi-Fi is down. The call, if any, has already been cleared and the microphone is off. Check the access point, the SSID, and the password. The station does not need a gateway that answers pings. It does need an address on the access point. Battery percent is independent of the radio. `battery none` means the expander saw no pack.

## No audio, link is up

- `status` shows `audio 0` when the headset adapter is missing or its streams stop. Plug it into the board USB Type-C socket, not the UART Type-C socket. Twenty-five failed reads mark audio bad; a later good read marks it good again, but the call has already ended.
- The log line `onboard USB socket selected` means EXIO5 was driven low. If that write fails, GPIO 19 and 20 stay on the CAN transceiver and the adapter never enumerates.
- `status` prints a `usb ...` line. `usb waiting` means nothing has enumerated. `usb unplugged` means it was removed. `no headset mic and headphone` means the device is playback-only or not USB audio. `UAC2` is not supported. A descriptor at the 256-byte core limit can be truncated before both streams are visible.
- USB CDC On Boot must be Disabled. If it is enabled, the USB socket is a serial port and the adapter cannot use it.
- If the log says the USB socket was selected and `status` still stays on `usb waiting`, the adapter is not answering on that socket. Its VBUS is the board 5 V rail. The CC pins on this socket are 5.1 kΩ to ground.
- The microphone is the headset mic on the TRRS plug. `miclevel` should move when you speak even though `mic_tx` stays 0.
- `audiotest` plays a local tone in the headphones only while idle, and it does not transmit. It refuses when the adapter is not streaming.
- The console is the UART Type-C port at 115200 (GPIO 43 TX, GPIO 44 RX). The USB Type-C port is not the console.

## The other station is always offline

- Both keys must be the same 16 characters. A mismatch looks like a dead station because bad HMACs are not answered.
- `peer 101 192.168.1.101` must match the address that station actually uses. `save` and `reboot` after directory edits.
- A station is online only after a HELLO in the last 7 seconds. A call to an offline station fails immediately and sends no INVITE.
- Eight bad HMACs from one address block that address for 30 seconds. A PC sending junk at a station can do this. Wait, or reboot the station.

## A call will not start

- Select the destination, then Call. This station's own tile is not a destination.
- `status` of `Audio fault` means the headset stream is down. Calls are rejected.
- `Busy` means the target is already in a call or a page. The existing call is left alone.
- `No answer` means the invite was not accepted within 3 seconds. A late accept must not bring the call back; if it does, that is a bug.
- Paging from 101, 102, or 103 reports `Paging is reception only`.

## Echo or noise

The canceller is 128-tap NLMS with a configurable bulk delay (`aec`, milliseconds). On this headset path, start at 20 or 40. If the far end hears a strong copy of their own voice, try 20, 40, and 60 while idle and place a new call after each change. This will not fix a headphone that leaks straight into the boom microphone. Lower the volume before raising the gain. The noise gate only attenuates quiet frames; it is not a noise-reduction processor. Clicks that grow over a long call are the adapter's own clock drifting; this host does not follow an asynchronous feedback endpoint.

Do not describe the result as zero echo unless a recording in the real room shows that.

## The web page

- User `admin`, PIN from `pin` or the default `1234` on a blank board.
- HTTP only. A browser warning about the missing lock is expected.
- The key and PIN fields are blank on purpose. An empty field keeps the stored value. A key must be exactly 16 visible characters with no spaces.
- The page cannot arm the microphone. If a web save starts a transmission, the firmware is the wrong image.

## Console

`help` lists commands. `status` is the first check. None of the commands set `mic_armed`. If `miclevel` prints `mic_tx 1` while the station is idle, treat that as a defect and do not use the board.

## Watchdog reset

`status` is unavailable until boot, but the log prints the reset reason. A task that stops resetting the watchdog for 10 seconds reboots the station. The microphone latch is cleared by boot. If this happens during a call, the other station should drop within 5 seconds of losing keepalives, or immediately if it sees the new HELLO boot id.
