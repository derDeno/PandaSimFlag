# PandaSimFlag firmware

Arduino firmware for the ESP32-S3-MINI-1-N8 board. It starts a setup hotspot on first boot (`PandaSimFlag-XXXX`, open network), serves the setup page at `http://192.168.4.1`, and stores the selected Wi-Fi network before rebooting. If joining that network fails, setup mode returns. On the network, the device advertises `pandasimflag.local`; if mDNS startup reports a conflict it tries `pandasimflag-XXXX.local`. The setup and telemetry HTTP APIs have no authentication in this bring-up build, so use it only on a trusted network.

## Build and flash

Install PlatformIO, open this directory as the project, and run:

```sh
pio run
pio run -t upload
pio run -t uploadfs
```

The partition table reserves two OTA application slots (`app0`/`app1`), OTA metadata, a 2 MiB LittleFS partition, and a coredump partition. The Settings page has separate uploads for application firmware and LittleFS images. Firmware uploads restart into the updated app slot; filesystem uploads replace the LittleFS image and reboot. Filesystem uploads update the single filesystem partition in place, so keep the device powered until it restarts; recover an interrupted filesystem update with `pio run -t uploadfs` over USB.

Build the LittleFS image with `pio run -t buildfs`; upload `.pio/build/pandasimflag/littlefs.bin` from the Settings page. Set the filesystem version in `data/version.txt` before building. The Home page reports the installed firmware and filesystem versions separately.

## Inputs

- Matrix: WS2812-compatible 8×8 matrix on GPIO5; status LED on GPIO40.
- Power: the TUSB320 at I²C address `0x60` is polled over GPIO2 (SDA) / GPIO1 (SCL). The default/unavailable and charge-accessory modes cap LEDs at 200 mA; 1.5 A mode caps them at 800 mA; 3 A mode caps them at 2000 mA, reserving the remainder for board electronics. The UI reports the detected mode and active LED cap.
- USB: native USB CDC serial at 115200 baud. SimHub's standard serial LED messages are accepted: `SIMHUB_1.0` protocol query, 64 LED query, and the `sleds` RGB frame command. In SimHub, enable Custom Serial Devices and configure its serial protocol for the selected COM port; native USB CDC does not have the dedicated VID/PID needed for automatic branded-device discovery.
- Network: HTTP `POST /api/game` accepts JSON. UDP port `20777` accepts the same JSON payloads and native F1 24 format `2024` and F1 25 format `2025` packets (player-car FIA flags and gear). GT7 telemetry is received on UDP `33740`; the firmware sends the game's `A` heartbeat to the configured PlayStation IP on UDP `33739`, then uses GT7 gear and RPM data. The JSON mapping accepts `game`, `display`, `flag` (or `marshalFlag`/`raceFlag`), `gear`, `proximity`, `rpm`, `maxRpm`, `animation`, and `period`. Example: `{"game":"F1 25","flag":"yellow","animation":"blink","period":250}`.
- Configure F1 24 to send UDP telemetry to the device IP on port `20777`, using UDP format `2024`. Configure F1 25 with format `2025`. For GT7, select the game and enter the PlayStation's IP address in Game settings. GT7 does not transmit race-control flags; its native display data is gear and RPM.
- UI: `GET /api/state` returns device info and the 64 RGB pixels currently rendered. The Settings page stores hostname, input mode (Wi-Fi API or USB SimHub), display type (flag/gear/flag & gear), and LED brightness in NVS. The device page also provides AP-mode Wi-Fi onboarding, live matrix preview, manual flag/animation controls, and application firmware OTA upload.

Supported displays include the racing flags, the divided black-and-white flag, `gear-1` through `gear-9`, `gear-N`, and `gear-R`. Standard displays support `none` and `blink`; double yellow alternates its diagonal colors automatically, while checkered and divided black-and-white alternate their inverted patterns automatically.

`Flag & gear` display mode shows the selected game's gear until an active F1 25 flag arrives, then returns to the gear when the flag clears. GT7 does not report race flags, so this mode displays its gear while connected to GT7.

This is bring-up firmware: only the listed native game telemetry fields are decoded, and network filesystem updates remain future work.
