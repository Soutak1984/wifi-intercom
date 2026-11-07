# Operating procedure

Each board is one station on the same 2.4 GHz access point. Station 100 is Reception at 192.168.1.100, 101 is Floor 1 at 192.168.1.101, 102 is Floor 2 at 192.168.1.102, and 103 is Floor 3 at 192.168.1.103. Calls, paging, and the microphone are the touchscreen. The web page and the UART Type-C console can change settings. They cannot place a call or turn the microphone on. USB CDC is not used. The panel art is `design/LAN-Intercom.desigma`, and this sketch draws those screens on the 7B. Arduino Tools → PSRAM must be OPI PSRAM.

The same steps apply to the ESP-IDF image and to the Arduino sketch in `arduino/lan_intercom/`. Both use the same call rules.

## First setup

1. Put all four boards on the same 2.4 GHz access point, gateway 192.168.1.1, mask 255.255.255.0. No internet or SIP server is required. The addresses above are static, so the access point must not be the only source of those addresses through DHCP. Client-to-client traffic must be allowed.
2. A blank board boots as Reception, 192.168.1.100, with no Wi-Fi name unless `board_config.h` has one. Set the other three before they share the network. On the UART Type-C port (GPIO 43 TX, GPIO 44 RX, 115200) the prompt is `ic>`. The USB Type-C port is the headset, not this console.
3. For Floor 1 type `id 101`, `name Floor 1`, `ip 192.168.1.101`, then `save` and `reboot`. Use 102 / Floor 2 / 192.168.1.102 and 103 / Floor 3 / 192.168.1.103 the same way. `id`, `name`, and `ip` apply only after reboot. Set the Wi-Fi name and password on the panel from Setup on the reception home or the floor idle screen, then Power, then Set Wi-Fi. The link-down screen also has Set Wi-Fi. The keyboard shows the password while it is typed. Save joins that network without a reboot. The console can still do it with `ssid <network name>`, `pass <password>` or `pass open`, then `save` and `reboot`. `pass open` is for a network with no password. The shared key and the web PIN are not on the glass.
4. Put the same 16-character key on every station: `key <16 characters>`, then `save` and `reboot`. The factory key is `office-intercom!`. Change it before real use.
5. The status page is `http://192.168.1.100/` (or that station's address). Sign in as `admin` with PIN `1234`, then set a new PIN of 4 to 8 letters or digits and save. Saving reboots the board. Leave the key or PIN box blank to keep the current secret.

When Wi-Fi has its address, the status pill says Idle and the Wi-Fi bars show the RSSI. No bars means the station is not associated or has no address. The battery percent stays visible either way. Stations announce themselves every 2 seconds and treat a peer as online after a valid hello within 7 seconds.

## Private call

1. Tap the destination: Reception, Floor 1, Floor 2, or Floor 3. This station's own tile is not a destination.
2. Tap Call. The status pill says Calling. The far station answers by itself and turns its speaker on. Its microphone stays off.
3. On the station that should be heard, tap Talk. The control stays live only while that station is actually sending voice. Tap Mute to stop. The speaker stays on.
4. Tap End on either station. Both sides drop, both speakers shut down, and both microphones turn off.

Tapping Mic while idle only arms the microphone for the next call. Nothing is sent until a call is connected. After End the arm is cleared, so the next call starts with the microphone off again.

A busy station rejects the new call. The caller does not cut in. If nobody accepts within 3 seconds, the caller returns to idle with the status "No answer". If both stations press Call toward each other at the same time, the lower station number keeps its outgoing call and the higher number answers it.

## Paging

Only Reception can page. On a floor station, Page stays dim. A tap still reaches the core, and the status line says "Paging is reception only".

1. Reception must be idle. A page does not interrupt a private call.
2. Tap one floor, then Page, to speak to that floor. Tap Page all to speak to Floor 1, Floor 2, and Floor 3 together. Each floor gets its own stream.
3. Each free floor turns its speaker on and locks its microphone. Mic on a floor that is receiving a page does nothing.
4. Reception taps Talk to be heard. Until then the floors hear silence.
5. A floor that is already in a private call is skipped. The other floors still get the page.
6. Reception taps End to stop the page. Floor speakers shut down and their microphones stay off.

## Status bar

| Pill | Meaning |
| --- | --- |
| Idle | Linked, no call |
| Calling | Outgoing invite |
| In call, Page, or the live mic fill | A call or a page is up |
| No Wi-Fi | Not associated, or no address |

The microphone control is filled only while this station is transmitting. Armed-but-idle, muted, and receiving a page leave it off. Wi-Fi bars and the battery percent stay in the bar on every screen after the splash. Under 15% the battery turns red.

## Sound level

Volume, microphone gain, and echo delay are on Setup. A change on the panel is applied and saved. On the console, `vol 0` to `100` and `gain 0` to `100` apply immediately. `gain 25` is about unity. The defaults are volume 60 and gain 40. Type `save` if a console change should survive a reboot. The same fields are on the web config page.
