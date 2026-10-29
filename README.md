# PandaSimFlag

PandaSimFlag is a compact, USB-C-powered **8×8 RGB sim-racing
flag/display** based on the **ESP32-S3** and a **5 V addressable LED
matrix**.

The current firmware supports two selectable input modes:

1.  **USB mode:** connect it to a PC and send LED data using SimHub's
    standard serial protocol over native USB CDC.
2.  **Wi-Fi mode:** connect it to a network for supported game telemetry
    or the JSON telemetry API.

The hardware is designed to leave room for future functionality beyond
simple racing flags.

## Current Capabilities

The revision 1.0 board and current firmware provide:

-   Drive a **64-pixel 8×8 5 V addressable RGB LED matrix**.
-   Be powered from a single **USB-C connection**.
-   Communicate with a PC through the ESP32-S3's **native USB**
    interface.
-   Use the same USB-C port for **firmware flashing** and normal USB
    communication.
-   SimHub serial LED input over native USB CDC (manual device selection;
    branded-device auto-discovery is not supported).
-   Wi-Fi telemetry decoders for **F1 2024**, **F1 2025**, and
    **Gran Turismo 7**.
-   A JSON telemetry API over HTTP and UDP for compatible senders.
-   USB-C current-advertisement detection and software LED power limiting.
-   A device web interface, persistent settings, and firmware/filesystem
    OTA updates.
-   I²C, UART, BOOT, and RESET development/recovery connections.

## Operating Modes

### USB / PC Mode

``` text
Game
  |
  v
SimHub / PC software
  |
  | USB
  v
ESP32-S3
  |
  v
8×8 LED Matrix
```

The ESP32-S3 uses native USB on **GPIO19 (D-)** and **GPIO20 (D+)**. The
same physical USB connection can therefore be used for firmware flashing
and normal host communication.

The firmware implements SimHub's standard serial LED protocol. Select
USB as the input mode and select the device's USB serial port in SimHub;
the native USB CDC identity does not support SimHub's branded-device
auto-discovery workflow.

### Wi-Fi / Standalone Mode

For consoles and games that expose telemetry over the network:

``` text
Console / Game
      |
      | UDP / network telemetry
      v
    Wi-Fi
      |
      v
 ESP32-S3
      |
      v
8×8 LED Matrix
```

The current native Wi-Fi game decoders are F1 2024, F1 2025, and Gran
Turismo 7. F1 decodes the player's gear, car flags, and race-control
events. GT7 sends gear telemetry after the device is configured with the
PlayStation's IP address; it does not currently decode race flags.

Other telemetry sources can use the JSON API, but there are no native
Forza or other game-specific decoders at present.

## Input and Display Flow

Select USB or Wi-Fi in the device settings. In Wi-Fi mode, select F1
2024, F1 2025, or Gran Turismo 7, or send JSON telemetry to the API.
Input is rendered to the matrix and constrained by the software power
limit.

``` text
  USB + SimHub ------+
                     +--> Display rendering --> Power limit --> 8×8 matrix
  Wi-Fi + F1/GT7/JSON+
```

In Wi-Fi mode, choose one of the supported games in the device settings.
The device does not automatically switch between USB and Wi-Fi inputs.

# Hardware

## MCU --- ESP32-S3-MINI-1-N8

The ESP32-S3 was selected because it combines:

-   Wi-Fi
-   Bluetooth capability
-   Native USB
-   USB OTG capability
-   Sufficient GPIO
-   I²C
-   UART
-   Suitable peripherals for addressable LEDs

Native USB means PandaSimFlag does **not** require a CH340, CP2102, or
similar USB-UART bridge.

### Current GPIO Allocation

  |Function | GPIO |
  |---------|------|
  |USB D- | GPIO19 |
  |USB D+ | GPIO20 |
  |I²C SCL | GPIO2 |
  |I²C SDA | GPIO1 |
  |Matrix data | GPIO16 |
  |Status LED | GPIO40 |
  |UART TX (TXD0) | GPIO43 |
  |UART RX (RXD0) | GPIO44 |
  |Boot | GPIO0 |
  |Reset | EN |


## LED Matrix

The display is a **5 V, 8×8, 64-pixel addressable RGB matrix** using a
WS2812-compatible single-wire data interface.

The matrix connector provides:

``` text
+5V
DATA
GND
```

A 64-LED matrix can theoretically consume several amps when all RGB
channels operate at full brightness. PandaSimFlag therefore does not
assume that full-brightness white is always available. LED output is
subject to a configurable current/brightness limit.

## LED Logic-Level Translation

The ESP32-S3 uses 3.3 V logic while the matrix operates from 5 V.

A **Nexperia 74AHCT1G125GV,125** buffer translates the data signal:

``` text
ESP32 GPIO16
     |
     | 3.3 V logic
     v
74AHCT1G125
     |
     | 5 V logic
     v
   330 ohm
     |
     v
Matrix DIN
```

The buffer is powered from 5 V, `/OE` is tied low, and a **100 nF
ceramic capacitor** provides local decoupling.

# USB-C

USB-C provides both power and USB data.

``` text
USB-C D- -> ESD protection -> 22 ohm -> GPIO19
USB-C D+ -> ESD protection -> 22 ohm -> GPIO20
```

USB data protection is provided by a **USBLC6-2SC6**.

# USB-C Power Detection

PandaSimFlag uses a **TUSB320LAIRWBR** USB Type-C configuration-channel
controller.

The controller monitors CC1/CC2 and lets the ESP32 determine the
source-current advertisement:

-   Default USB current
-   1.5 A
-   3.0 A

The ESP32 communicates with the TUSB320 over I²C at 7-bit address **0x47**
(ADDR tied to GND), using GPIO1 for SDA and GPIO2 for SCL.

Current configuration:

``` text
CC1       -> USB-C CC1
CC2       -> USB-C CC2
PORT      -> GND
VBUS_DET  -> VBUS through 900 kohm
ADDR      -> GND
INT_N     -> unused / optional
SDA       -> ESP32 GPIO1
SCL       -> ESP32 GPIO2
ID        -> NC
EN_N      -> GND
VDD       -> 3.3 V
GND       -> GND
```

A **100 nF capacitor** provides local VDD decoupling. The shared I²C bus
has pull-ups to 3.3 V.

## Power-Aware LED Limiting

The firmware reads the TUSB320 Type-C current advertisement and sets the
FastLED output power limit as follows:

``` text
USB capability
      |
      +-- Default / unattached / detector unavailable -> 200 mA
      +-- 1.5 A advertisement                         -> 800 mA
      +-- 3.0 A advertisement                         -> 2000 mA
```

Current must always be reserved for the ESP32-S3, regulator, TUSB320,
level shifter, and other board electronics.

The user-configurable brightness setting (default 25%) is applied in
addition to this software power cap. The TUSB320 reports the **USB
Type-C current advertisement**; it does not measure actual current or
verify the source's real capability. Sources without a recognized
advertisement use the conservative 200 mA limit.

# Power Supply

USB VBUS directly supplies the 5 V LED rail:

``` text
USB VBUS
    |
    +---------------- 5 V LED Matrix
    |
    v
TPS62162
    |
    v
   3.3 V
    |
    +-- ESP32-S3
    +-- TUSB320
    +-- other 3.3 V circuitry
```

## 5 V to 3.3 V Regulator

A **TPS62162DSGR** buck regulator generates 3.3 V from USB 5 V.

The regulator section includes:

-   3.3 µH inductor
-   Input bulk/decoupling capacitance
-   Output capacitance
-   Required grounding and feedback connections

The Power Good output is currently unused.

# I²C

An I²C header is provided for development and future expansion. The bus
is also used internally by the TUSB320.

The board includes:

-   4.7 kΩ SDA pull-up to 3.3 V
-   4.7 kΩ SCL pull-up to 3.3 V

Header pinout (by PCB pin number):

``` text
1: GND
2: 3.3V
3: SCL (GPIO2)
4: SDA (GPIO1)
```

# UART / Development Header

A development header provides the following connections (by PCB pin number):

``` text
1: GND
2: UART TX (TXD0 / GPIO43)
3: UART RX (RXD0 / GPIO44)
4: 3.3V
5: GPIO0 / BOOT
6: EN / RESET
```

Normal firmware flashing should be possible directly through native
USB-C. The header remains useful for development, recovery, and
debugging, or can be used with the **PandaFlasher**.


# Status LED

A programmable status LED is connected to **GPIO40**.

The current firmware blinks it at a faster rate while the setup access
point is active and a slower rate otherwise.

# Current Firmware

The ESP32-S3 firmware provides:

-   An open setup access point on first boot, named `PandaSimFlag-XXXX`,
    with the configuration page at `http://192.168.4.1`.
-   Wi-Fi station mode with configurable hostname and mDNS address, plus
    USB mode for native USB CDC and SimHub serial LED messages.
-   A device web UI for Wi-Fi onboarding, device and game settings,
    status, local matrix preview, manual flag preview, reset, and OTA.
-   Settings stored in NVS, including hostname, connection mode, selected
    game, display option, LED power, and normal-flag blink behavior.
-   Native Wi-Fi decoders for F1 2024, F1 2025, and Gran Turismo 7. GT7
    requires the console's IP address for the telemetry heartbeat.
-   Display modes: **Flag**, **Gear**, **Flag & gear**, **Throttle &
    brake**, and **Tyre temperatures** for F1 and JSON telemetry; GT7
    currently offers **Gear**. The JSON renderer also accepts RPM and
    proximity data.
-   JSON telemetry through HTTP `POST /api/game` and UDP port `20777`.
-   A 30-second Wi-Fi telemetry timeout that clears the display to idle
    when valid game telemetry stops arriving.
-   Animated flag patterns, optional blinking for normal flags, a local
    matrix preview, and a configurable brightness setting.
-   TUSB320-based LED power limiting and separate firmware and LittleFS
    OTA uploads. The filesystem version is read from
    `firmware/data/version.txt`.

F1 decodes gear, player car flags, red/checkered events, penalties, and
safety-car events. GT7's current settings expose the gear display; other
display data is available through the JSON API.

# Project Philosophy

PandaSimFlag is intended to be more than a hard-coded 8×8 flag display.

The hardware is deliberately built around a capable ESP32-S3 so the same
PCB can support:

-   Simple USB SimHub operation
-   Standalone console operation
-   Network telemetry
-   Future USB functionality
-   Firmware expansion

The core principle is to keep **telemetry acquisition, game-specific
decoding, display rendering, and hardware control separated**. This
allows new games and features to be added without redesigning the PCB or
restructuring the entire firmware.

## Current Core Components

  Function                     Component
  ---------------------------- --------------------------------
  MCU                          ESP32-S3-MINI-1-N8
  LED display                  5 V 8×8 addressable RGB matrix
  LED level translation        74AHCT1G125GV,125
  USB-C CC detection           TUSB320LAIRWBR
  5 V -\> 3.3 V regulator      TPS62162DSGR
  USB data ESD protection      USBLC6-2SC6
  LED data series resistor     330 Ω
  USB D+/D- series resistors   22 Ω
  I²C pull-ups                 4.7 kΩ
  TUSB320 VBUS_DET resistor    900 kΩ

# Current Status

Hardware revision 1.0 and firmware version 0.1.0 are in place. The
firmware includes the web UI, USB/SimHub input, the listed Wi-Fi decoders,
power detection and LED limiting, NVS configuration, and separate OTA
paths for firmware and filesystem images. This remains bring-up software;
validate telemetry behavior and OTA recovery on the target hardware
before treating it as production-ready. Additional game decoders and
broader hardware validation remain future work.

