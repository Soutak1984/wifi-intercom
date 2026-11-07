#pragma once

/* First-boot defaults. A saved record in the NVS namespace "ic" replaces
 * these. That is the same namespace the ESP-IDF firmware uses, so a board
 * already provisioned there keeps its identity when this sketch is uploaded
 * without erasing flash.
 *
 * A blank board boots as station 100 at 192.168.1.100. Provision one board
 * at a time: set the id from the serial console, save, and reboot before
 * flashing the next blank board. Erase All Flash Before Sketch Upload
 * returns a board to the values below.
 *
 * The shared key must be exactly 16 characters and identical on all four
 * stations. Change the key and the web PIN before real use.
 */

#ifndef IC_BUILD_STATION
#define IC_BUILD_STATION 101
#endif

#ifndef IC_BUILD_KEY
#define IC_BUILD_KEY "office-intercom!"
#endif

#ifndef IC_BUILD_PIN
#define IC_BUILD_PIN "1234"
#endif

/* Wi-Fi station on the 2.4 GHz radio. Leave both empty and set them from
 * the ic> console (ssid, pass, save, reboot), or fill them in before upload.
 * The password is 8 to 63 characters. An empty password joins an open network.
 * Addresses stay the static values in NVS. This radio does not use 5 GHz.
 */
#ifndef IC_BUILD_SSID
#define IC_BUILD_SSID ""
#endif

#ifndef IC_BUILD_WIFI_PASS
#define IC_BUILD_WIFI_PASS ""
#endif
