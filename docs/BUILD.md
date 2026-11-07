# Build and flash

This file is the ESP-IDF image at `C:\Users\conta\Music\epbx`. The Arduino sketch in this folder does not use it and does not use a W5500. The sketch joins Wi-Fi. Its board settings are in the README.

The ESP-IDF firmware is written for ESP-IDF 6.0.2 and the component `espressif/w5500` version 2.x. The W5500 driver is not inside ESP-IDF itself from 6.0 onward. The first configure step downloads it. That download needs the Internet on the build PC. The stations do not need the Internet afterward.

This workspace has ESP-IDF at `C:\esp\v6.0.2\esp-idf` when the Espressif installer has been used. The project has not been compiled in this session. Run the commands below and keep the log.

## ESP-IDF

In PowerShell, from the project directory:

```
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
cd C:\Users\conta\Music\epbx
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

Replace `COM5` with the USB-Serial-JTAG port. The console is USB Serial/JTAG, not UART0. GPIO 43 and 44 stay free.

`idf.py set-target esp32s3` is required once. `sdkconfig.defaults` already selects the S3, a 4 MB flash image, the custom partition table, SPI Ethernet, the USB console, a 1000 Hz tick, and a 10 s task watchdog.

## Flash size

The partition table fits in 4 MB, and a 4 MB image setting also flashes onto an 8 MB module. If `idf.py flash` reports that the chip is smaller than the configured size, the sdkconfig flash size is too large. For an 8 MB module you may set `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` in `sdkconfig`. Do not do that on a 4 MB module.

Do not enable PSRAM on this ESP-IDF image. GPIO 26–37 are reserved for flash and, on octal modules, PSRAM. This pin map avoids them. Forcing octal PSRAM on an N8R2 board will not boot. The Arduino sketch on the Waveshare 7B is the other way around: Tools → PSRAM must be OPI PSRAM, or the panel framebuffer cannot be allocated.

## Host tests

The protocol tests do not need a board or ESP-IDF:

```
cd tests
make test
```

`make` uses `gcc -std=c11 -Wall -Wextra -Werror`. On a machine with only the ESP-IDF clang, point `CC` at that clang and add a host target if the compiler does not default to Windows x64. The tests were not run here because the command runner exited with Windows error 216 before any compiler started.

## What the build produces

`build/lan_intercom.bin` is the application. The factory partition starts at `0x10000` and is 1.5 MB. NVS is the `nvs` partition and survives a normal flash. `idf.py erase-flash` wipes it.

If the serial log says `crypto self-test failed`, the image is bad. Do not ship that binary. Reflash a build that passes the self-test. The network is not started in that state.
