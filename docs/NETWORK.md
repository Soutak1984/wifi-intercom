# Network and station setup

The stations join one 2.4 GHz access point. They do not need the Internet, a DNS server, or a SIP server. The access point must allow clients to talk to each other. A 5 GHz-only network will not associate. Do not put two unprovisioned boards on the LAN at the same time: a blank board is station 100 at 192.168.1.100.

## Defaults

| Item | Value |
| --- | --- |
| Station 100 | 192.168.1.100 Reception |
| Station 101 | 192.168.1.101 Floor 1 |
| Station 102 | 192.168.1.102 Floor 2 |
| Station 103 | 192.168.1.103 Floor 3 |
| Gateway | 192.168.1.1 |
| Netmask | 255.255.255.0 |
| Shared key | `office-intercom!` (16 characters, change this) |
| Web user | `admin` |
| Web PIN | `1234` (change this) |
| Signalling | UDP 47000 |
| Media | UDP 47010 |
| Web | TCP 80 |

Addresses are static. The station does not take a DHCP lease. The access point's LAN has to be 192.168.1.0/24, or you change `ip`, `gw`, and `mask` to that LAN before the stations share it. The same 16-character key must be stored on every station. The Wi-Fi name and password are per station and are usually the same. The web PIN can differ per station. Set the Wi-Fi name with `ssid` and the password with `pass`, then `save` and `reboot`. `pass open` is for a network with no password.

HELLO updates a peer's address from the packet source after the authenticator matches, so a moved station is learned once it can still be reached at the previous directory address long enough to exchange HELLO, or after you set `peer` and reboot. Other messages are accepted only from the address currently in the directory.

## Provision one board

1. Flash a single board. Leave the others powered off or away from the access point.
2. Open the UART Type-C console at 115200. The prompt is `ic>`. That is the CH343 port, not the USB socket used by the headset, and not USB CDC.
3. Confirm `status`. It should show station 100 until you change it.
4. For Floor 1:

```
id 101
name Floor 1
ssid <2.4 GHz network name>
pass <8 to 63 characters>
ip 192.168.1.101
peer 100 192.168.1.100
peer 101 192.168.1.101
peer 102 192.168.1.102
peer 103 192.168.1.103
key <same 16 characters on every station>
pin <4 to 8 letters or digits>
save
reboot
```

5. Repeat for 102 and 103. Leave reception as 100, but still change `key` and `pin`, then `save` and `reboot`.
6. Let a board join the access point only after it has a unique address.
7. `status` on each board should list the others as online within about 7 seconds.

`idf.py flash` does not erase NVS, so a later firmware update keeps the station id. `idf.py erase-flash` clears NVS and the board returns to station 100. Erase only one board at a time, and keep it off the LAN until it is provisioned again.

Volume and gain can be changed live:

```
vol 50
gain 40
save
```

`gain 25` is about unity. The default 40 is about 1.6×. `aec 40` stores a 40 ms canceller delay. It applies immediately only while the station is idle; otherwise it applies on reboot. `save` keeps it.

## Web

Open `http://192.168.1.100/` from a computer on the same subnet. Sign in as `admin` with the PIN. The status page refreshes every 3 seconds and shows the microphone as LIVE, ARMED (not transmitting), or OFF in large text. Configuration is at `/config`. Saving writes NVS and reboots. The form cannot enable the microphone or start a call.

The page is HTTP, not HTTPS. Use it on a trusted access point. A computer on that same subnet can open the page. The password box on the configuration form stays blank; leave it blank to keep the current password, or type `open` for a network with no password.

## Calls

1. Tap the destination (Reception, Floor 1, Floor 2, or Floor 3).
2. Tap Call.
3. The far station answers by itself. Both speakers unmute. Both microphones stay off.
4. Tap Talk to speak. The control is live only while that station is transmitting. Tap Mute to stop. The other station's microphone does not follow.
5. Tap End on either station. Both sides go idle, both speakers shut down, and both microphones disarm.

Paging: on reception, select a floor and tap Page, or tap Page all. Floors that are already in a private call stay in that call and do not hear the page. A floor that is free plays the announcement and does not turn its microphone on.

End while idle does nothing on the wire. Mic while idle can arm the station, but no audio is sent until a call or an outgoing page is up. The panel and the web page both show that armed latch.
