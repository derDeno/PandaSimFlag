#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <inttypes.h>
#include <memory>
#include <new>
#include "logs.h"
#include "matrix_layout.h"

namespace {
constexpr uint8_t MATRIX_PIN = 16;
constexpr uint8_t STATUS_PIN = 40;
constexpr uint8_t STATUS_PWM_CHANNEL = 0;
constexpr uint16_t LED_COUNT = 64;
constexpr uint16_t GAME_UDP_PORT = 20777;
constexpr uint16_t GT7_UDP_PORT = 33740;
constexpr uint16_t GT7_HEARTBEAT_PORT = 33739;
constexpr uint32_t WIFI_TELEMETRY_TIMEOUT_MS = 30000;
constexpr uint32_t USB_TELEMETRY_TIMEOUT_MS = 2000;
constexpr uint32_t ORIENTATION_TIMEOUT_MS = 15000;
constexpr uint8_t TUSB320_ADDRESS = 0x47;
constexpr uint16_t SAFE_LED_LIMIT_MA = 200;
constexpr uint16_t MEDIUM_LED_LIMIT_MA = 800;
constexpr uint16_t HIGH_LED_LIMIT_MA = 2000;
constexpr char VERSION[] = "0.1.9";
constexpr BaseType_t CONTROL_CORE = 0;
constexpr BaseType_t MATRIX_CORE = 1;
constexpr size_t MAX_TELEMETRY_BYTES = 1472;
static_assert(FASTLED_ESP32_HAS_RMT == 1 && FASTLED_RMT5 == 0,
              "Matrix requires the ESP-IDF 4 RMT driver; software timing is unsupported");
static_assert(ARDUINO_RUNNING_CORE == CONTROL_CORE, "Control loop must run on core 0");
static_assert(ARDUINO_EVENT_RUNNING_CORE == CONTROL_CORE, "Arduino events must run on core 0");
static_assert(CONFIG_LWIP_TCPIP_TASK_AFFINITY == CONTROL_CORE, "TCP/IP task must run on core 0");
static_assert(CONFIG_MDNS_TASK_AFFINITY == CONTROL_CORE, "mDNS task must run on core 0");
static_assert(CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_0, "Wi-Fi driver must run on core 0");

// Core 0 owns requested settings; queues copy plain data to/from core 1.
struct MatrixSettings {
  char inputMode[5] = "wifi";
  char wifiGame[8] = "";
  char gt7ConsoleIp[16] = "";
  char displayType[12] = "flag";
  uint8_t ledPowerPercent = 10;
  uint16_t matrixRotation = 0;
  bool orientationActive = false;
  uint16_t orientationRotation = 0;
  uint32_t orientationLastContact = 0;
  bool normalFlagsBlink = false;
  uint16_t normalFlagBlinkPeriod = 250;
  uint16_t powerLimitMa = SAFE_LED_LIMIT_MA;
};

struct MatrixSnapshot {
  uint8_t pixels[LED_COUNT * 3] = {};
  char game[33] = "Not reported";
  char connectionMode[32] = "Standby";
  char flag[24] = "idle";
  char animation[16] = "none";
  uint16_t animationPeriod = 250;
  uint8_t animationPhase = 0;
  uint32_t nextAnimation = 0;
  bool gameConnected = false;
  bool gameSyncPaused = false;
  bool orientationActive = false;
  uint8_t outputBrightness = 0;
  int8_t core = -1;
  uint32_t frames = 0;
  uint32_t stackFreeBytes = 0;
};

struct TelemetryCommand {
  char json[MAX_TELEMETRY_BYTES + 1];
};

MatrixSettings controlMatrixSettings;
uint32_t orientationSession = 0;
QueueHandle_t matrixSettingsQueue = nullptr;
QueueHandle_t matrixSnapshotQueue = nullptr;
QueueHandle_t telemetryQueue = nullptr;

CRGB pixels[LED_COUNT];
CRGB frame[LED_COUNT];
CRGB wirePixels[LED_COUNT];
bool matrixDirty = false;
uint32_t matrixFrames = 0;
bool gameSyncPaused = false;
uint16_t matrixPowerLimitMa = SAFE_LED_LIMIT_MA;
uint8_t matrixOutputBrightness = 0;
WebServer server(80);
DNSServer dns;
WiFiUDP gameUdp;
WiFiUDP gt7Udp;
Preferences preferences;
String suffix;
String hostName = "pandasimflag";
String configuredHostname = "pandasimflag";
bool mdnsStarted = false;
String inputMode = "wifi";
String wifiGame;
String gt7ConsoleIp;
String displayType = "flag";
uint8_t ledPowerPercent = 10;
uint16_t matrixRotation = 0;
uint16_t savedMatrixRotation = 0;
bool orientationActive = false;
uint32_t orientationLastContact = 0;
bool powerLedEnabled = true;
uint8_t powerLedBrightnessPercent = 20;
bool normalFlagsBlink = false;
uint16_t normalFlagBlinkPeriod = 250;
String apName;
String wifiSsid;
String wifiPassword;
String currentGame = "Not reported";
String currentConnectionMode = "Standby";
String currentF1Flag;
String currentF1Gear;
String currentF1EventFlag;
uint32_t f1CheckeredStarted = 0;
bool f1SafetyCarActive = false;
bool f1PenaltyFlagActive = false;
uint32_t f1PenaltyFlagStarted = 0;
bool accessPoint = false;
bool animationOff = false;
String animation = "none";
String activeFlag = "idle";
uint16_t animationPeriod = 250;
uint8_t rainOffset = 0;
uint32_t nextAnimation = 0;
uint32_t nextPowerPoll = 0;
uint32_t nextGt7Heartbeat = 0;
uint32_t lastWifiTelemetry = 0;
bool wifiTelemetrySeen = false;
uint32_t lastUsbTelemetry = 0;
bool usbTelemetrySeen = false;
uint16_t ledLimitMa = SAFE_LED_LIMIT_MA;
uint8_t detectedCurrentMode = 0;
uint8_t attachedState = 0;
bool tusb320Available = false;
bool filesystemMounted = false;
bool updateStarted = false;
bool updateSucceeded = false;
bool updateFilesystem = false;
String updateError;
String filesystemVersion = "unknown";

enum class UsbState : uint8_t { Prefix, Command, LedData, LedEnd };
UsbState usbState = UsbState::Prefix;
uint8_t prefixLength = 0;
char usbCommand[6] = {};
uint8_t commandLength = 0;
uint8_t dataLength = 0;
uint8_t endLength = 0;
uint8_t endBytes[3] = {};
CRGB incoming[LED_COUNT];
uint32_t commandStarted = 0;

void showFrame() {
  // Coalesce incoming changes; only the matrix task transmits LED data.
  matrixDirty = true;
}

void publishMatrixSettings() {
  if (matrixSettingsQueue) xQueueOverwrite(matrixSettingsQueue, &controlMatrixSettings);
}

MatrixSnapshot readMatrixSnapshot() {
  MatrixSnapshot snapshot;
  if (matrixSnapshotQueue) xQueuePeek(matrixSnapshotQueue, &snapshot, 0);
  return snapshot;
}

bool gameConnected() {
  const uint32_t now = millis();
  return inputMode == "wifi"
      ? wifiTelemetrySeen && now - lastWifiTelemetry < WIFI_TELEMETRY_TIMEOUT_MS
      : usbTelemetrySeen && now - lastUsbTelemetry < USB_TELEMETRY_TIMEOUT_MS;
}

void updateStatusLed() {
  uint8_t duty = powerLedEnabled ? (uint16_t)powerLedBrightnessPercent * 255 / 100 : 0;
  if (accessPoint) {
    if ((millis() / 250) % 2 == 0) duty = 0;
  } else if (duty && !readMatrixSnapshot().gameConnected) {
    const float phase = (millis() % 3000) * (2.0f * PI / 3000.0f);
    duty = duty * (1.0f - cosf(phase)) * 0.5f;
  }
  static int previousDuty = -1;
  if (duty != previousDuty) {
    ledcWrite(STATUS_PWM_CHANNEL, duty);
    previousDuty = duty;
  }
}

bool readTusbRegister(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(TUSB320_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(TUSB320_ADDRESS, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

void updatePowerDetection() {
  if (millis() < nextPowerPoll) return;
  nextPowerPoll = millis() + 250;

  uint8_t attachment;
  if (!readTusbRegister(0x09, attachment)) {
    tusb320Available = false;
    attachedState = 0;
    detectedCurrentMode = 0;
    ledLimitMa = SAFE_LED_LIMIT_MA;
  } else {
    tusb320Available = true;
    attachedState = (attachment >> 6) & 0x03;
    uint8_t current = 0;
    if (attachedState == 2 && readTusbRegister(0x08, current)) {
      detectedCurrentMode = (current >> 4) & 0x03;
      ledLimitMa = detectedCurrentMode == 1 ? MEDIUM_LED_LIMIT_MA : detectedCurrentMode == 3 ? HIGH_LED_LIMIT_MA : SAFE_LED_LIMIT_MA;
    } else {
      detectedCurrentMode = 0;
      ledLimitMa = SAFE_LED_LIMIT_MA;
    }
  }
  if (controlMatrixSettings.powerLimitMa != ledLimitMa) {
    controlMatrixSettings.powerLimitMa = ledLimitMa;
    publishMatrixSettings();
  }
}

void drawRain(uint8_t offset) {
  fill_solid(pixels, LED_COUNT, CRGB::Black);
  const uint8_t dropX[] = {0, 5, 2};
  const uint8_t dropY[] = {0, 0, 4};
  const uint8_t dropRows[] = {0, 1, 1, 1, 2, 2, 2};
  const uint8_t dropCols[] = {1, 0, 1, 2, 0, 1, 2};
  for (uint8_t drop = 0; drop < 3; ++drop) for (uint8_t point = 0; point < 7; ++point) {
    const uint8_t row = (dropY[drop] + offset + dropRows[point]) % 8;
    const uint8_t col = dropX[drop] + dropCols[point];
    pixels[row * 8 + col] = CRGB(0, 150, 255);
  }
}

void fillFlag(const String& flag) {
  const bool continueRain = flag == "rain" && animation == "rain";
  activeFlag = flag;
  animationOff = false;
  if (!continueRain) animation = "none";
  fill_solid(pixels, LED_COUNT, CRGB::Black);
  if (flag == "rain") {
    if (!continueRain) rainOffset = 0;
    drawRain(rainOffset);
  }
  else if (flag == "yellow") fill_solid(pixels, LED_COUNT, CRGB::Yellow);
  else if (flag == "green") fill_solid(pixels, LED_COUNT, CRGB::Green);
  else if (flag == "blue") fill_solid(pixels, LED_COUNT, CRGB::Blue);
  else if (flag == "red") fill_solid(pixels, LED_COUNT, CRGB::Red);
  else if (flag == "white") fill_solid(pixels, LED_COUNT, CRGB::White);
  else if (flag == "black") fill_solid(pixels, LED_COUNT, CRGB(48, 0, 0));
  else if (flag == "checkered") {
    for (uint8_t i = 0; i < LED_COUNT; ++i) pixels[i] = ((i / 8 + i % 8) % 2) ? CRGB::Black : CRGB::White;
  } else if (flag == "blackwhite") {
    for (uint8_t i = 0; i < LED_COUNT; ++i) pixels[i] = i / 8 + i % 8 < 7 ? CRGB::Black : CRGB::White;
  } else if (flag == "doubleyellow") {
    for (uint8_t i = 0; i < LED_COUNT; ++i) if (i / 8 + i % 8 < 7) pixels[i] = CRGB::Yellow;
  }
  ::memcpy(frame, pixels, sizeof(frame));
  showFrame();
}

bool fillGear(String gear) {
  gear.toUpperCase();
  const char* names[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "N", "R"};
  const char* patterns[] = {
    "00011000" "00111000" "00011000" "00011000" "00011000" "00011000" "00111100" "00111100",
    "00111100" "01100110" "00000110" "00011100" "00110000" "01100000" "01111110" "01111110",
    "00111100" "01100110" "00000110" "00011100" "00000110" "01100110" "00111100" "00111100",
    "00011000" "00111000" "01101000" "01001000" "01111110" "00001000" "00001000" "00001000",
    "01111110" "01100000" "01111100" "00000110" "00000110" "01100110" "00111100" "00111100",
    "00111100" "01100000" "01100000" "01111100" "01100110" "01100110" "00111100" "00111100",
    "01111110" "01111110" "00000110" "00001100" "00011000" "00110000" "00110000" "00110000",
    "00111100" "01100110" "01100110" "00111100" "01100110" "01100110" "00111100" "00111100",
    "00111100" "01100110" "01100110" "00111110" "00000110" "00000110" "01100110" "00111100",
    "01100011" "01110011" "01111011" "01101111" "01100111" "01100011" "01100011" "01100011",
    "01111100" "01100110" "01100110" "01111100" "01111000" "01101100" "01100110" "01100011"
  };
  for (uint8_t glyph = 0; glyph < 11; ++glyph) {
    if (gear != names[glyph]) continue;
    for (uint8_t pixel = 0; pixel < LED_COUNT; ++pixel) pixels[pixel] = patterns[glyph][pixel] == '1' ? CRGB::White : CRGB::Black;
    activeFlag = "gear-" + gear;
    animation = "none";
    ::memcpy(frame, pixels, sizeof(frame));
    showFrame();
    return true;
  }
  return false;
}

void fillPedals(float throttle, float brake) {
  if (!isfinite(throttle) || !isfinite(brake)) return;
  const uint8_t throttleLeds = lroundf(constrain(throttle, 0.0f, 100.0f) * 32.0f / 100.0f);
  const uint8_t brakeLeds = lroundf(constrain(brake, 0.0f, 100.0f) * 32.0f / 100.0f);
  fill_solid(pixels, LED_COUNT, CRGB::Black);
  for (uint8_t led = 0; led < 32; ++led) {
    const uint8_t row = 7 - led / 4;
    if (led < throttleLeds) pixels[row * 8 + led % 4] = CRGB::Green;
    if (led < brakeLeds) pixels[row * 8 + 4 + led % 4] = CRGB::Red;
  }
  activeFlag = "pedals";
  animation = "none";
  ::memcpy(frame, pixels, sizeof(frame));
  showFrame();
}

void fillTyres(const float temperatures[4]) {
  fill_solid(pixels, LED_COUNT, CRGB::Black);
  for (uint8_t tyre = 0; tyre < 4; ++tyre) {
    const float temperature = temperatures[tyre];
    // shortcut: fixed 85–105 °C band, adjust if compound-specific ranges are needed.
    const CRGB color = !isfinite(temperature) ? CRGB::Black : temperature < 85 ? CRGB::Blue : temperature <= 105 ? CRGB::Green : CRGB::Red;
    const uint8_t rowOffset = tyre >= 2 ? 4 : 0;
    const uint8_t colOffset = (tyre % 2) * 4;
    for (uint8_t row = 0; row < 4; ++row) {
      for (uint8_t col = 0; col < 4; ++col) pixels[(row + rowOffset) * 8 + col + colOffset] = color;
    }
  }
  activeFlag = "tyres";
  animation = "none";
  ::memcpy(frame, pixels, sizeof(frame));
  showFrame();
}

void setAnimation(const String& mode, uint16_t period) {
  if (mode != "none" && mode != "blink" && mode != "invert" && mode != "doubleyellow" && mode != "rain") return;
  const bool startRain = mode == "rain" && animation != "rain";
  animation = mode;
  animationPeriod = mode == "blink" ? constrain(period, 20, 2000) : constrain(period, 250, 5000);
  animationOff = false;
  if (startRain) rainOffset = 0;
  ::memcpy(frame, pixels, sizeof(frame));
  showFrame();
}

bool isNormalFlag(const String& display) {
  return display == "green" || display == "yellow" || display == "blue" ||
         display == "red" || display == "white" || display == "black";
}

String selectedGameName() {
  if (wifiGame == "gt7") return "Gran Turismo 7";
  if (wifiGame == "f1-2024") return "F1 2024";
  if (wifiGame == "f1-2025") return "F1 2025";
  return "Not reported";
}

void resetGameTelemetry() {
  wifiTelemetrySeen = false;
  usbTelemetrySeen = false;
  currentF1Flag = "";
  currentF1Gear = "";
  currentF1EventFlag = "";
  f1SafetyCarActive = false;
  f1PenaltyFlagActive = false;
  currentGame = selectedGameName();
  currentConnectionMode = gameSyncPaused ? "Web UI / manual" : "Standby";
}

void applyTelemetry(JsonDocument& doc) {
  if (orientationActive) return;
  const bool webUiControl = doc["source"] == "webui";
  String display = doc["display"] | "";
  display.toLowerCase();
  if (webUiControl) {
    if (display == "idle") {
      gameSyncPaused = false;
      resetGameTelemetry();
      fillFlag("idle");
      return;
    }
    const bool gear = display.length() == 6 && display.startsWith("gear-") &&
        ((display[5] >= '1' && display[5] <= '9') || display[5] == 'n' || display[5] == 'r');
    if (!gear && !isNormalFlag(display) && display != "doubleyellow" &&
        display != "blackwhite" && display != "checkered" && display != "rain") return;
    gameSyncPaused = true;
  } else if (inputMode != "wifi" || gameSyncPaused) return;
  currentConnectionMode = webUiControl ? "Web UI / manual" : "Wi-Fi / API";
  if (doc["game"].is<const char*>()) {
    currentGame = doc["game"].as<String>();
    currentGame.trim();
    if (currentGame.length() > 32) currentGame.remove(32);
    if (!currentGame.length()) currentGame = "Not reported";
  } else currentGame = selectedGameName();
  String flag = doc["flag"] | doc["marshalFlag"] | doc["raceFlag"] | "";
  flag.toLowerCase();
  flag.replace("_", "");
  flag.replace(" ", "");
  const bool hasActiveFlag = flag == "green" || flag == "yellow" || flag == "doubleyellow" || flag == "blue" || flag == "red" || flag == "white" || flag == "black" || flag == "blackwhite" || flag == "checkered" || flag == "rain";
  if (displayType == "flaggear" && hasActiveFlag) fillFlag(flag);
  else if (display.startsWith("gear-")) fillGear(display.substring(5));
  else if (display == "green" || display == "yellow" || display == "doubleyellow" || display == "blue" || display == "red" || display == "white" || display == "black" || display == "blackwhite" || display == "checkered" || display == "rain" || display == "idle") fillFlag(display);
  else if (hasActiveFlag && displayType != "gear" && displayType != "flaggear") fillFlag(flag);
  else if (displayType == "flaggear" && doc["gear"].is<const char*>()) fillGear(doc["gear"].as<String>());
  else if (displayType == "flaggear" && doc["gear"].is<int>()) fillGear(String(doc["gear"].as<int>()));

  const String selectedDisplay = display.length() ? display : flag;
  if (doc["animation"].is<const char*>() || (normalFlagsBlink && isNormalFlag(selectedDisplay))) {
    String requestedAnimation = normalFlagsBlink && isNormalFlag(selectedDisplay)
        ? "blink" : doc["animation"] | "none";
    if (requestedAnimation != "none") {
      if (display == "doubleyellow" || flag == "doubleyellow") requestedAnimation = "doubleyellow";
      else if (display == "checkered" || flag == "checkered" || display == "blackwhite" || flag == "blackwhite") requestedAnimation = "invert";
      else if (requestedAnimation == "invert" || requestedAnimation == "doubleyellow") requestedAnimation = "none";
    }
    const uint16_t defaultPeriod = isNormalFlag(selectedDisplay) ? normalFlagBlinkPeriod : animationPeriod;
    setAnimation(requestedAnimation, doc["period"] | defaultPeriod);
  } else if (display == "doubleyellow" || flag == "doubleyellow") {
    setAnimation("doubleyellow", doc["period"] | animationPeriod);
  } else if (display == "checkered" || flag == "checkered" || display == "blackwhite" || flag == "blackwhite") {
    setAnimation("invert", doc["period"] | animationPeriod);
  } else if (display == "rain" || flag == "rain") {
    setAnimation("rain", doc["period"] | animationPeriod);
  }
  if (doc["proximity"].is<const char*>()) {
    String side = doc["proximity"].as<String>();
    if (side != "left" && side != "right" && side != "both") return;
    fill_solid(pixels, LED_COUNT, CRGB::Black);
    if (side == "left" || side == "both") for (uint8_t row = 0; row < 8; ++row) pixels[row * 8] = CRGB::Blue;
    if (side == "right" || side == "both") for (uint8_t row = 0; row < 8; ++row) pixels[row * 8 + 7] = CRGB::Blue;
    activeFlag = "proximity-" + side;
    animation = "none";
    ::memcpy(frame, pixels, sizeof(frame));
    showFrame();
  }
  if (displayType != "flag" && displayType != "flaggear" && doc["rpm"].is<uint32_t>()) {
    const uint16_t rpm = doc["rpm"].as<uint16_t>();
    const uint16_t maxRpm = max<uint16_t>(1, doc["maxRpm"] | 8000);
    const uint8_t lit = min<uint16_t>(64, (uint32_t)rpm * 64 / maxRpm);
    activeFlag = "rpm";
    animation = "none";
    for (uint8_t i = 0; i < LED_COUNT; ++i) pixels[i] = i < lit ? (i > 55 ? CRGB::Red : i > 47 ? CRGB::Orange : CRGB::Green) : CRGB::Black;
    ::memcpy(frame, pixels, sizeof(frame));
    showFrame();
  }
  const bool hasThrottle = doc["throttle"].is<float>() || doc["throttle"].is<int>() || doc["throttle"].is<unsigned int>();
  const bool hasBrake = doc["brake"].is<float>() || doc["brake"].is<int>() || doc["brake"].is<unsigned int>();
  if (displayType == "pedals" && hasThrottle && hasBrake) fillPedals(doc["throttle"].as<float>(), doc["brake"].as<float>());
  if (displayType == "tyres" && doc["tyreTemps"].is<JsonArray>() && doc["tyreTemps"].as<JsonArray>().size() == 4) {
    JsonArray values = doc["tyreTemps"].as<JsonArray>();
    float temperatures[4];
    bool valid = true;
    for (uint8_t i = 0; i < 4; ++i) {
      const bool numeric = values[i].is<float>() || values[i].is<int>() || values[i].is<unsigned int>();
      if (!numeric) { valid = false; break; }
      temperatures[i] = values[i].as<float>();
    }
    if (valid) fillTyres(temperatures);
  }
}

uint32_t readLe32(const uint8_t* bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

uint32_t rotateLeft(uint32_t value, uint8_t count) {
  return (value << count) | (value >> (32 - count));
}

void decryptGt7Packet(uint8_t* packet, size_t length) {
  static const char key[] = "Simulator Interface Packet GT7 ver 0.0";
  const uint32_t iv1 = readLe32(packet + 0x40);
  const uint32_t nonce[2] = {iv1 ^ 0xDEADBEAF, iv1};
  uint64_t counter = 0;
  for (size_t offset = 0; offset < length; offset += 64, ++counter) {
    uint32_t state[16] = {
      0x61707865, readLe32((const uint8_t*)key), readLe32((const uint8_t*)key + 4), readLe32((const uint8_t*)key + 8), readLe32((const uint8_t*)key + 12),
      0x3320646e, nonce[0], nonce[1], (uint32_t)counter, (uint32_t)(counter >> 32), 0x79622d32,
      readLe32((const uint8_t*)key + 16), readLe32((const uint8_t*)key + 20), readLe32((const uint8_t*)key + 24), readLe32((const uint8_t*)key + 28), 0x6b206574
    };
    uint32_t work[16];
    memcpy(work, state, sizeof(work));
    for (uint8_t round = 0; round < 10; ++round) {
      work[4] ^= rotateLeft(work[0] + work[12], 7); work[8] ^= rotateLeft(work[4] + work[0], 9); work[12] ^= rotateLeft(work[8] + work[4], 13); work[0] ^= rotateLeft(work[12] + work[8], 18);
      work[9] ^= rotateLeft(work[5] + work[1], 7); work[13] ^= rotateLeft(work[9] + work[5], 9); work[1] ^= rotateLeft(work[13] + work[9], 13); work[5] ^= rotateLeft(work[1] + work[13], 18);
      work[14] ^= rotateLeft(work[10] + work[6], 7); work[2] ^= rotateLeft(work[14] + work[10], 9); work[6] ^= rotateLeft(work[2] + work[14], 13); work[10] ^= rotateLeft(work[6] + work[2], 18);
      work[3] ^= rotateLeft(work[15] + work[11], 7); work[7] ^= rotateLeft(work[3] + work[15], 9); work[11] ^= rotateLeft(work[7] + work[3], 13); work[15] ^= rotateLeft(work[11] + work[7], 18);
      work[1] ^= rotateLeft(work[0] + work[3], 7); work[2] ^= rotateLeft(work[1] + work[0], 9); work[3] ^= rotateLeft(work[2] + work[1], 13); work[0] ^= rotateLeft(work[3] + work[2], 18);
      work[6] ^= rotateLeft(work[5] + work[4], 7); work[7] ^= rotateLeft(work[6] + work[5], 9); work[4] ^= rotateLeft(work[7] + work[6], 13); work[5] ^= rotateLeft(work[4] + work[7], 18);
      work[11] ^= rotateLeft(work[10] + work[9], 7); work[8] ^= rotateLeft(work[11] + work[10], 9); work[9] ^= rotateLeft(work[8] + work[11], 13); work[10] ^= rotateLeft(work[9] + work[8], 18);
      work[12] ^= rotateLeft(work[15] + work[14], 7); work[13] ^= rotateLeft(work[12] + work[15], 9); work[14] ^= rotateLeft(work[13] + work[12], 13); work[15] ^= rotateLeft(work[14] + work[13], 18);
    }
    uint8_t stream[64];
    for (uint8_t word = 0; word < 16; ++word) {
      const uint32_t value = work[word] + state[word];
      stream[word * 4] = value; stream[word * 4 + 1] = value >> 8; stream[word * 4 + 2] = value >> 16; stream[word * 4 + 3] = value >> 24;
    }
    for (size_t i = 0; i < min((size_t)64, length - offset); ++i) packet[offset + i] ^= stream[i];
  }
}

void applyNativeTelemetry(const char* display) {
  JsonDocument doc;
  doc["game"] = selectedGameName();
  doc["display"] = display;
  applyTelemetry(doc);
  currentConnectionMode = wifiGame == "f1-2024" ? "Wi-Fi / F1 24 UDP" : "Wi-Fi / F1 25 UDP";
}

void applyF1Display() {
  if (displayType == "gear") return;
  if (currentF1EventFlag == "checkered" && millis() - f1CheckeredStarted >= 10000) currentF1EventFlag = "";
  if (f1PenaltyFlagActive && millis() - f1PenaltyFlagStarted >= 3000) f1PenaltyFlagActive = false;
  const char* display = currentF1EventFlag.length() ? currentF1EventFlag.c_str() :
                        f1PenaltyFlagActive ? "blackwhite" :
                        f1SafetyCarActive ? "doubleyellow" :
                        currentF1Flag.length() ? currentF1Flag.c_str() : "idle";
  if (displayType == "flaggear" && !currentF1EventFlag.length() && !f1PenaltyFlagActive && !f1SafetyCarActive && !currentF1Flag.length() && currentF1Gear.length()) {
    fillGear(currentF1Gear);
  } else {
    applyNativeTelemetry(display);
  }
}

void markWifiTelemetry() {
  lastWifiTelemetry = millis();
  wifiTelemetrySeen = true;
}

void handleF1Packet(const uint8_t* packet, size_t length) {
  if (inputMode != "wifi" || length < 29) return;
  const uint16_t packetFormat = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8);
  const bool f1Packet = (wifiGame == "f1-2024" && packetFormat == 2024 && packet[2] == 24) ||
                        (wifiGame == "f1-2025" && packetFormat == 2025 && packet[2] == 25);
  if (!f1Packet) return;
  if (gameSyncPaused) { markWifiTelemetry(); return; }
  const uint8_t packetId = packet[6];
  const uint8_t player = packet[27];
  if (packetId == 3) {
    if (length < 45) return;
    markWifiTelemetry();
    currentGame = selectedGameName();
    currentConnectionMode = wifiGame == "f1-2024" ? "Wi-Fi / F1 24 UDP" : "Wi-Fi / F1 25 UDP";
    const uint8_t* event = packet + 29;
    if (memcmp(event, "SSTA", 4) == 0) {
      currentF1Flag = "";
      currentF1EventFlag = "";
      f1SafetyCarActive = false;
      f1PenaltyFlagActive = false;
    } else if (memcmp(event, "RDFL", 4) == 0) {
      currentF1EventFlag = "red";
    } else if (memcmp(event, "CHQF", 4) == 0) {
      currentF1EventFlag = "checkered";
      f1CheckeredStarted = millis();
    } else if (memcmp(event, "PENA", 4) == 0) {
      f1PenaltyFlagActive = true;
      f1PenaltyFlagStarted = millis();
    } else if (memcmp(event, "SCAR", 4) == 0) {
      const uint8_t safetyCarType = packet[33];
      const uint8_t eventType = packet[34];
      if ((safetyCarType == 1 || safetyCarType == 2) && eventType == 0) f1SafetyCarActive = true;
      else if (eventType == 2 || eventType == 3) f1SafetyCarActive = false;
    }
    applyF1Display();
    return;
  }
  if (player >= 22) return;
  markWifiTelemetry();
  currentGame = selectedGameName();
  currentConnectionMode = wifiGame == "f1-2024" ? "Wi-Fi / F1 24 UDP" : "Wi-Fi / F1 25 UDP";
  if (packetId == 7) {
    constexpr size_t CAR_STATUS_SIZE = 55;
    const size_t flagOffset = 29 + (size_t)player * CAR_STATUS_SIZE + 28;
    if (length <= flagOffset) return;
    const int8_t flag = (int8_t)packet[flagOffset];
    if (flag == 0) {
      currentF1Flag = "";
    } else if (flag == 1 || flag == 2 || flag == 3) {
      currentF1Flag = flag == 1 ? "green" : flag == 2 ? "blue" : "yellow";
      if (flag == 1 && currentF1EventFlag == "red") currentF1EventFlag = "";
    }
    applyF1Display();
  } else if (packetId == 6 && length >= 29 + ((size_t)player + 1) * 60) {
    const size_t carOffset = 29 + (size_t)player * 60;
    const int8_t gear = (int8_t)packet[carOffset + 15];
    currentF1Gear = gear < 0 ? "R" : gear == 0 ? "N" : String(gear);
    if (displayType == "pedals") {
      float throttle, brake;
      memcpy(&throttle, packet + carOffset + 2, sizeof(throttle));
      memcpy(&brake, packet + carOffset + 10, sizeof(brake));
      fillPedals(throttle * 100.0f, brake * 100.0f);
    } else if (displayType == "tyres") {
      const float temperatures[] = {packet[carOffset + 32], packet[carOffset + 33], packet[carOffset + 30], packet[carOffset + 31]};
      fillTyres(temperatures);
    } else if (displayType == "gear" || (displayType == "flaggear" && !currentF1Flag.length() && !currentF1EventFlag.length() && !f1PenaltyFlagActive && !f1SafetyCarActive)) fillGear(currentF1Gear);
  }
}

void handleGt7Packet(uint8_t* packet, size_t length) {
  if (length != 296 || inputMode != "wifi" || wifiGame != "gt7") return;
  decryptGt7Packet(packet, length);
  if (readLe32(packet) != 0x47375330) return;
  markWifiTelemetry();
  if (gameSyncPaused) return;
  currentGame = "Gran Turismo 7";
  currentConnectionMode = "Wi-Fi / GT7 UDP";
  if (displayType == "flag") return;
  float rpm;
  memcpy(&rpm, packet + 0x3C, sizeof(rpm));
  const int rawGear = packet[0x90] & 0x0F;
  const int gear = rawGear - 1;
  if (displayType == "gear" || displayType == "flaggear") fillGear(gear < 0 ? "R" : gear == 0 ? "N" : String(gear));
  else if (isfinite(rpm) && rpm >= 0 && rpm <= 30000) {
    JsonDocument doc;
    doc["game"] = "Gran Turismo 7";
    doc["rpm"] = (uint16_t)rpm;
    doc["maxRpm"] = max<uint16_t>(1, readLe32(packet + 0x8A) & 0xFFFF);
    applyTelemetry(doc);
    currentConnectionMode = "Wi-Fi / GT7 UDP";
  }
}

void replyJson(JsonDocument& doc) {
  String response;
  serializeJson(doc, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void sendLogs() {
  uint64_t since = 0, boot = 0;
  if (server.args()) {
    if (server.args() != 2 || !server.hasArg("since") || !server.hasArg("boot") ||
        !pandasimflag::logs::parseCursor(server.arg("since").c_str(), UINT64_MAX, since) ||
        !pandasimflag::logs::parseCursor(server.arg("boot").c_str(), UINT32_MAX, boot)) {
      server.send(400, "application/json", "{\"error\":\"invalid_log_cursor\"}");
      return;
    }
    if (boot != pandasimflag::logs::boot_id()) since = 0;
  }
  // Snapshot under the logging lock; socket writes happen after releasing it.
  std::unique_ptr<char[]> text(new (std::nothrow) char[pandasimflag::logs::kCapacity]);
  if (!text) {
    server.send(503, "application/json", "{\"error\":\"log_buffer_unavailable\"}");
    return;
  }
  const auto snapshot = pandasimflag::logs::read(since, text.get());
  char cursor[21];
  snprintf(cursor, sizeof(cursor), "%" PRIu64, snapshot.next);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Log-Cursor", cursor);
  server.sendHeader("X-Log-Boot", String(pandasimflag::logs::boot_id()));
  server.sendHeader("X-Log-Dropped", snapshot.dropped ? "1" : "0");
  server.setContentLength(snapshot.size);
  server.send(200, "text/plain; charset=utf-8", "");
  if (snapshot.size) server.sendContent(text.get(), snapshot.size);
}

void sendInfo() {
  const MatrixSnapshot snapshot = readMatrixSnapshot();
  JsonDocument doc;
  doc["target"] = "esp32s3";
  doc["pcb_revision"] = "1.0";
  doc["chip_revision"] = ESP.getChipRevision();
  doc["cpu_cores"] = ESP.getChipCores();
  doc["cpu_mhz"] = ESP.getCpuFreqMHz();
  doc["flash_bytes"] = ESP.getFlashChipSize();
  doc["sdk_version"] = ESP.getSdkVersion();
  doc["build_date"] = __DATE__;
  doc["build_time"] = __TIME__;
  doc["reset_reason"] = esp_reset_reason();
  doc["heap_free_bytes"] = ESP.getFreeHeap();
  doc["heap_min_free_bytes"] = ESP.getMinFreeHeap();
  doc["task_count"] = uxTaskGetNumberOfTasks();
  doc["log_capacity_bytes"] = pandasimflag::logs::kCapacity;
  doc["control_core"] = xPortGetCoreID();
  doc["matrix_core"] = snapshot.core;
  doc["matrix_frames"] = snapshot.frames;
  doc["matrix_stack_free_bytes"] = snapshot.stackFreeBytes;
  replyJson(doc);
}

String readFilesystemVersion() {
  File file = LittleFS.open("/version.txt", "r");
  if (!file) return "unknown";
  String version = file.readStringUntil('\n');
  file.close();
  version.trim();
  return version.length() ? version : "unknown";
}

void sendState() {
  const MatrixSnapshot snapshot = readMatrixSnapshot();
  JsonDocument doc;
  doc["ssid"] = accessPoint ? apName : WiFi.SSID();
  doc["ip"] = (accessPoint ? WiFi.softAPIP() : WiFi.localIP()).toString();
  doc["hostname"] = hostName + ".local";
  doc["mac"] = WiFi.macAddress();
  doc["mode"] = accessPoint ? "ap" : "station";
  doc["connectionMode"] = accessPoint ? "Setup AP" : snapshot.connectionMode;
  doc["inputMode"] = controlMatrixSettings.inputMode;
  doc["wifiGame"] = controlMatrixSettings.wifiGame;
  doc["gt7ConsoleIp"] = controlMatrixSettings.gt7ConsoleIp;
  doc["displayType"] = controlMatrixSettings.displayType;
  doc["ledPowerPercent"] = controlMatrixSettings.ledPowerPercent;
  doc["matrixRotation"] = controlMatrixSettings.matrixRotation;
  doc["orientationActive"] = snapshot.orientationActive;
  doc["powerLedEnabled"] = powerLedEnabled;
  doc["powerLedBrightness"] = powerLedBrightnessPercent;
  doc["gameConnected"] = snapshot.gameConnected;
  doc["gameSyncPaused"] = snapshot.gameSyncPaused;
  doc["outputBrightness"] = snapshot.outputBrightness;
  doc["normalFlagsBlink"] = controlMatrixSettings.normalFlagsBlink;
  doc["normalFlagBlinkPeriod"] = controlMatrixSettings.normalFlagBlinkPeriod;
  doc["deviceHostname"] = configuredHostname;
  doc["networkName"] = apName;
  doc["connected"] = accessPoint || WiFi.status() == WL_CONNECTED;
  doc["rssi"] = accessPoint ? 0 : WiFi.RSSI();
  doc["game"] = snapshot.game;
  doc["flag"] = snapshot.flag;
  doc["animation"] = snapshot.animation;
  doc["controlCore"] = xPortGetCoreID();
  doc["matrixCore"] = snapshot.core;
  doc["matrixFrames"] = snapshot.frames;
  doc["matrixStackFreeBytes"] = snapshot.stackFreeBytes;
  doc["version"] = VERSION;
  doc["filesystemVersion"] = filesystemVersion;
  doc["uptime"] = millis() / 1000;
  doc["powerController"] = tusb320Available ? "TUSB320" : "not detected";
  doc["usbAttached"] = attachedState == 2;
  doc["usbCurrentMode"] = attachedState != 2 ? "unattached" : detectedCurrentMode == 1 ? "1.5A" : detectedCurrentMode == 3 ? "3A" : detectedCurrentMode == 2 ? "charge accessory" : "default current";
  doc["ledLimitMa"] = ledLimitMa;
  JsonArray rgb = doc["pixels"].to<JsonArray>();
  for (uint8_t value : snapshot.pixels) rgb.add(value);
  replyJson(doc);
}

void sendMatrixState() {
  const MatrixSnapshot snapshot = readMatrixSnapshot();
  JsonDocument doc;
  doc["game"] = snapshot.game;
  doc["connectionMode"] = accessPoint ? "Setup AP" : snapshot.connectionMode;
  doc["gameConnected"] = snapshot.gameConnected;
  doc["gameSyncPaused"] = snapshot.gameSyncPaused;
  doc["orientationActive"] = snapshot.orientationActive;
  doc["flag"] = snapshot.flag;
  doc["animation"] = snapshot.animation;
  doc["period"] = snapshot.animationPeriod;
  doc["phase"] = snapshot.animationPhase;
  const int32_t remaining = snapshot.nextAnimation - millis();
  doc["remaining"] = remaining > 0 ? remaining : 0;
  if (strcmp(snapshot.flag, "simhub-usb") == 0) {
    JsonArray rgb = doc["pixels"].to<JsonArray>();
    for (uint8_t value : snapshot.pixels) rgb.add(value);
  } else if (strcmp(snapshot.flag, "pedals") == 0 || strcmp(snapshot.flag, "rpm") == 0) {
    uint8_t left = 0, right = 0;
    for (uint8_t i = 0; i < LED_COUNT; ++i) {
      const bool lit = snapshot.pixels[i * 3] || snapshot.pixels[i * 3 + 1] || snapshot.pixels[i * 3 + 2];
      if (lit) (i % 8 < 4 ? left : right)++;
    }
    JsonArray values = doc["values"].to<JsonArray>();
    if (strcmp(snapshot.flag, "rpm") == 0) values.add(left + right);
    else { values.add(left); values.add(right); }
  } else if (strcmp(snapshot.flag, "tyres") == 0) {
    JsonArray values = doc["values"].to<JsonArray>();
    for (uint8_t i : {0, 4, 32, 36}) {
      for (uint8_t channel = 0; channel < 3; ++channel) values.add(snapshot.pixels[i * 3 + channel]);
    }
  }
  replyJson(doc);
}

void startMdns(bool useMacSuffix = false) {
  MDNS.end();
  hostName = configuredHostname;
  hostName.toLowerCase();
  mdnsStarted = MDNS.begin(hostName.c_str());
  if (mdnsStarted) {
    MDNS.setInstanceName(apName);
    if (configuredHostname.equalsIgnoreCase(apName) && !useMacSuffix) {
      // Query before claiming the short name; begin() does not report conflicts.
      mdns_result_t* results = nullptr;
      const bool available = mdns_query("pandasimflag", nullptr, nullptr, MDNS_TYPE_ANY, 1000, 1, &results) == ESP_OK && results == nullptr;
      mdns_query_results_free(results);
      if (available) {
        hostName = "pandasimflag";
        mdnsStarted = MDNS.begin(hostName.c_str());
      }
    }
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
  }
  pandasimflag::logs::write("mDNS %s: %s.local", mdnsStarted ? "started" : "failed", hostName.c_str());
}

void handleMdnsConflict() {
  // The mDNS stack renames its local host after a probe/defence conflict.
  if (mdnsStarted && hostName == "pandasimflag" && !mdns_hostname_exists(hostName.c_str())) startMdns(true);
}

bool validHostname(const String& value) {
  if (!value.length() || value.length() > 30 || value[0] == '-' || value[value.length() - 1] == '-') return false;
  for (char c : value) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

void expireOrientationAssistant() {
  if (!controlMatrixSettings.orientationActive ||
      millis() - controlMatrixSettings.orientationLastContact < ORIENTATION_TIMEOUT_MS) return;
  controlMatrixSettings.orientationActive = false;
  orientationSession = 0;
  publishMatrixSettings();
}

void handleOrientation() {
  expireOrientationAssistant();
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  const String action = doc["action"] | "";
  if (action != "start" && action != "preview" && action != "keepalive" && action != "save" && action != "cancel") {
    server.send(400, "application/json", "{\"error\":\"Unknown orientation action\"}");
    return;
  }
  if (action == "start") {
    if (controlMatrixSettings.orientationActive) {
      server.send(409, "application/json", "{\"error\":\"An orientation assistant is already open. Close it or wait a few seconds and retry.\"}");
      return;
    }
    orientationSession = esp_random();
    if (!orientationSession) orientationSession = 1;
    controlMatrixSettings.orientationActive = true;
    controlMatrixSettings.orientationRotation = controlMatrixSettings.matrixRotation;
  } else {
    if (!controlMatrixSettings.orientationActive || !doc["session"].is<uint32_t>() ||
        doc["session"].as<uint32_t>() != orientationSession) {
      server.send(409, "application/json", "{\"error\":\"Orientation assistant expired. Close and reopen it.\"}");
      return;
    }
    if (action == "preview") {
      if (!doc["rotation"].is<int>() || !validMatrixRotation(doc["rotation"].as<int>())) {
        server.send(400, "application/json", "{\"error\":\"Matrix rotation must be 0, 90, 180, or 270 degrees\"}");
        return;
      }
      controlMatrixSettings.orientationRotation = doc["rotation"].as<uint16_t>();
    } else if (action == "save" || action == "cancel") {
      if (action == "save") {
        preferences.begin("device", false);
        const bool stored = preferences.putUShort("rotation", controlMatrixSettings.orientationRotation) == 2;
        preferences.end();
        if (!stored) {
          server.send(500, "application/json", "{\"error\":\"Could not save orientation. Please retry.\"}");
          return;
        }
        controlMatrixSettings.matrixRotation = controlMatrixSettings.orientationRotation;
      }
      controlMatrixSettings.orientationActive = false;
      orientationSession = 0;
    }
  }
  controlMatrixSettings.orientationLastContact = millis();
  publishMatrixSettings();
  JsonDocument response;
  response["session"] = orientationSession;
  response["rotation"] = controlMatrixSettings.orientationActive
      ? controlMatrixSettings.orientationRotation : controlMatrixSettings.matrixRotation;
  if (action != "keepalive") pandasimflag::logs::write("Orientation assistant: %s", action.c_str());
  replyJson(response);
}

void handleSettingsSave() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  String hostname = doc["hostname"] | "";
  hostname.trim();
  hostname.toLowerCase();
  const String requestedInputMode = doc["connectionMode"] | "";
  const String requestedWifiGame = doc["wifiGame"].is<const char*>() ? doc["wifiGame"].as<String>() : String(controlMatrixSettings.wifiGame);
  const String requestedGt7ConsoleIp = doc["gt7ConsoleIp"].is<const char*>() ? doc["gt7ConsoleIp"].as<String>() : String(controlMatrixSettings.gt7ConsoleIp);
  const int requestedLedPower = doc["ledPower"] | -1;
  const int requestedMatrixRotation = doc["matrixRotation"] | static_cast<int>(controlMatrixSettings.matrixRotation);
  if ((!doc["matrixRotation"].isUnbound() && !doc["matrixRotation"].is<int>()) ||
      !validMatrixRotation(requestedMatrixRotation)) {
    server.send(400, "application/json", "{\"error\":\"Matrix rotation must be 0, 90, 180, or 270 degrees\"}");
    return;
  }
  const bool requestedPowerLedEnabled = doc["powerLedEnabled"] | powerLedEnabled;
  const int requestedPowerLedBrightness = doc["powerLedBrightness"] | static_cast<int>(powerLedBrightnessPercent);
  if ((!doc["powerLedEnabled"].isUnbound() && !doc["powerLedEnabled"].is<bool>()) ||
      (!doc["powerLedBrightness"].isUnbound() && !doc["powerLedBrightness"].is<int>()) ||
      requestedPowerLedBrightness < 0 || requestedPowerLedBrightness > 100) {
    server.send(400, "application/json", "{\"error\":\"Power LED must be enabled or disabled, with brightness from 0 to 100\"}");
    return;
  }
  IPAddress parsedGt7Ip;
  if (!validHostname(hostname) || (requestedInputMode != "wifi" && requestedInputMode != "usb") ||
      (requestedWifiGame != "" && requestedWifiGame != "gt7" && requestedWifiGame != "f1-2024" && requestedWifiGame != "f1-2025") ||
      (requestedInputMode == "wifi" && requestedWifiGame == "gt7" && (!parsedGt7Ip.fromString(requestedGt7ConsoleIp) || parsedGt7Ip == IPAddress(0, 0, 0, 0))) ||
      (requestedGt7ConsoleIp.length() && !parsedGt7Ip.fromString(requestedGt7ConsoleIp)) ||
      requestedLedPower < 0 || requestedLedPower > 100) {
    server.send(400, "application/json", "{\"error\":\"Check the device settings and enter a valid PlayStation IP address for GT7\"}");
    return;
  }
  preferences.begin("device", false);
  bool stored = preferences.putString("hostname", hostname) == hostname.length();
  stored &= preferences.putString("input", requestedInputMode) == requestedInputMode.length();
  stored &= preferences.putString("wifiGame", requestedWifiGame) == requestedWifiGame.length();
  stored &= preferences.putString("gt7Ip", requestedGt7ConsoleIp) == requestedGt7ConsoleIp.length();
  stored &= preferences.putUChar("ledPower", requestedLedPower) == 1;
  stored &= preferences.putUShort("rotation", requestedMatrixRotation) == 2;
  stored &= preferences.putBool("powerLedOn", requestedPowerLedEnabled) == 1;
  stored &= preferences.putUChar("powerLedLevel", requestedPowerLedBrightness) == 1;
  preferences.end();
  if (!stored) {
    server.send(500, "application/json", "{\"error\":\"Could not save settings to NVS\"}");
    return;
  }
  const bool hostnameChanged = !configuredHostname.equalsIgnoreCase(hostname);
  configuredHostname = hostname;
  strlcpy(controlMatrixSettings.inputMode, requestedInputMode.c_str(), sizeof(controlMatrixSettings.inputMode));
  strlcpy(controlMatrixSettings.wifiGame, requestedWifiGame.c_str(), sizeof(controlMatrixSettings.wifiGame));
  strlcpy(controlMatrixSettings.gt7ConsoleIp, requestedGt7ConsoleIp.c_str(), sizeof(controlMatrixSettings.gt7ConsoleIp));
  controlMatrixSettings.ledPowerPercent = requestedLedPower;
  controlMatrixSettings.matrixRotation = requestedMatrixRotation;
  publishMatrixSettings();
  powerLedEnabled = requestedPowerLedEnabled;
  powerLedBrightnessPercent = requestedPowerLedBrightness;
  updateStatusLed();
  if (hostnameChanged) startMdns();
  pandasimflag::logs::write("Device settings saved; input %s", controlMatrixSettings.inputMode);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGameSettingsSave() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  const String requestedWifiGame = doc["wifiGame"] | "";
  const String requestedGt7ConsoleIp = doc["gt7ConsoleIp"] | "";
  const String requestedDisplayType = doc["displayType"] | controlMatrixSettings.displayType;
  const bool requestedNormalFlagsBlink = doc["normalFlagsBlink"] | controlMatrixSettings.normalFlagsBlink;
  const int requestedNormalFlagBlinkPeriod = doc["normalFlagBlinkPeriod"] | static_cast<int>(controlMatrixSettings.normalFlagBlinkPeriod);
  const bool validDisplayType = requestedWifiGame == "gt7"
      ? requestedDisplayType == "gear"
      : requestedDisplayType == "gear" || requestedDisplayType == "flag" || requestedDisplayType == "flaggear" || requestedDisplayType == "pedals" || requestedDisplayType == "tyres";
  IPAddress parsedGt7Ip;
  if ((requestedWifiGame != "" && requestedWifiGame != "gt7" && requestedWifiGame != "f1-2024" && requestedWifiGame != "f1-2025") ||
      (requestedWifiGame == "gt7" && (!parsedGt7Ip.fromString(requestedGt7ConsoleIp) || parsedGt7Ip == IPAddress(0, 0, 0, 0))) ||
      (requestedGt7ConsoleIp.length() && !parsedGt7Ip.fromString(requestedGt7ConsoleIp)) ||
      !validDisplayType || requestedNormalFlagBlinkPeriod < 20 || requestedNormalFlagBlinkPeriod > 2000) {
    server.send(400, "application/json", "{\"error\":\"Select a supported game and display type, and enter a valid PlayStation IP address for GT7\"}");
    return;
  }
  preferences.begin("device", false);
  bool stored = preferences.putString("wifiGame", requestedWifiGame) == requestedWifiGame.length();
  stored &= preferences.putString("gt7Ip", requestedGt7ConsoleIp) == requestedGt7ConsoleIp.length();
  stored &= preferences.putString("display", requestedDisplayType) == requestedDisplayType.length();
  stored &= preferences.putBool("normalBlink", requestedNormalFlagsBlink);
  stored &= preferences.putUShort("blinkPeriod", requestedNormalFlagBlinkPeriod) == 2;
  preferences.end();
  if (!stored) {
    server.send(500, "application/json", "{\"error\":\"Could not save game settings to NVS\"}");
    return;
  }
  strlcpy(controlMatrixSettings.wifiGame, requestedWifiGame.c_str(), sizeof(controlMatrixSettings.wifiGame));
  strlcpy(controlMatrixSettings.gt7ConsoleIp, requestedGt7ConsoleIp.c_str(), sizeof(controlMatrixSettings.gt7ConsoleIp));
  strlcpy(controlMatrixSettings.displayType, requestedDisplayType.c_str(), sizeof(controlMatrixSettings.displayType));
  controlMatrixSettings.normalFlagsBlink = requestedNormalFlagsBlink;
  controlMatrixSettings.normalFlagBlinkPeriod = requestedNormalFlagBlinkPeriod;
  publishMatrixSettings();
  pandasimflag::logs::write("Game settings saved; game %s, display %s", controlMatrixSettings.wifiGame, controlMatrixSettings.displayType);
  server.send(200, "application/json", "{\"ok\":true}");
}

void startAccessPoint() {
  accessPoint = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  dns.setTTL(0);
  dns.start(53, "*", WiFi.softAPIP());
  startMdns();
  pandasimflag::logs::write("Setup access point started at %s", WiFi.softAPIP().toString().c_str());
}

void handleWifiSave() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  if (ssid.length() == 0 || ssid.length() > 32 || (password.length() != 0 && (password.length() < 8 || password.length() > 63))) {
    server.send(400, "application/json", "{\"error\":\"SSID must be 1-32 characters; password must be empty or 8-63 characters\"}");
    return;
  }
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  pandasimflag::logs::write("Wi-Fi settings saved; restarting");
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(250);
  ESP.restart();
}

void handleReboot() {
  pandasimflag::logs::write("Restart requested from WebUI");
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleReset() {
  const esp_err_t result = nvs_flash_erase();
  if (result != ESP_OK) {
    pandasimflag::logs::write("Could not erase settings: %d", result);
    server.send(500, "application/json", "{\"error\":\"Could not erase NVS settings\"}");
    return;
  }
  pandasimflag::logs::write("Settings erased; restarting into setup mode");
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleGameInput() {
  const String body = server.arg("plain");
  if (body.length() > MAX_TELEMETRY_BYTES) {
    server.send(413, "application/json", "{\"error\":\"Telemetry exceeds 1472 bytes\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  if (controlMatrixSettings.orientationActive && doc["source"] == "webui") {
    server.send(409, "application/json", "{\"error\":\"Close the orientation assistant before starting a demo\"}");
    return;
  }
  TelemetryCommand command{};
  strlcpy(command.json, body.c_str(), sizeof(command.json));
  if (xQueueSend(telemetryQueue, &command, 0) != pdPASS) {
    server.send(503, "application/json", "{\"error\":\"Telemetry queue is full; retry shortly\"}");
    return;
  }
  server.send(204);
}

void handleUpdateUpload() {
  if (!server.header("Content-Type").startsWith("multipart/form-data")) {
    updateStarted = false;
    updateSucceeded = false;
    updateError = "Update uploads must use multipart/form-data";
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    updateFilesystem = server.uri() == "/api/update/filesystem";
    pandasimflag::logs::write("Starting %s update", updateFilesystem ? "filesystem" : "firmware");
    updateStarted = false;
    updateSucceeded = false;
    updateError = "";
    if (updateFilesystem && filesystemMounted) {
      LittleFS.end();
      filesystemMounted = false;
    }
    if (Update.begin(UPDATE_SIZE_UNKNOWN, updateFilesystem ? U_SPIFFS : U_FLASH)) updateStarted = true;
    else {
      updateError = Update.errorString();
      if (updateFilesystem) filesystemMounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && updateStarted) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      updateError = Update.errorString();
      Update.abort();
      updateStarted = false;
      if (updateFilesystem) filesystemMounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
    }
  } else if (upload.status == UPLOAD_FILE_END && updateStarted) {
    updateSucceeded = Update.end(true);
    if (!updateSucceeded) updateError = Update.errorString();
    updateStarted = false;
    if (updateFilesystem) {
      filesystemMounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
      filesystemVersion = filesystemMounted ? readFilesystemVersion() : "unknown";
      if (!filesystemMounted || filesystemVersion == "unknown" || !LittleFS.exists("/index.html")) {
        updateSucceeded = false;
        updateError = "Filesystem image is invalid or missing index.html/version.txt";
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (updateStarted) Update.abort();
    updateStarted = false;
    updateError = "Update upload aborted";
    if (updateFilesystem && !filesystemMounted) filesystemMounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
  }
}

void handleUpdateComplete() {
  if (!updateSucceeded) {
    pandasimflag::logs::write("Update failed: %s", updateError.c_str());
    JsonDocument doc;
    doc["error"] = updateError.length() ? updateError : "Firmware update failed";
    String response;
    serializeJson(doc, response);
    server.send(400, "application/json", response);
    return;
  }
  pandasimflag::logs::write("Update installed; restarting");
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500);
  ESP.restart();
}

void routeRequest() {
  String path = server.uri();
  if (accessPoint) {
    const String portalHost = WiFi.softAPIP().toString();
    const String requestHost = server.hostHeader();
    // Redirect connectivity probes and foreign hosts to the setup page.
    if ((requestHost != portalHost && requestHost != portalHost + ":80") ||
        (path != "/" && !LittleFS.exists(path))) {
      server.sendHeader("Location", "http://" + portalHost + "/", true);
      server.sendHeader("Cache-Control", "no-store");
      server.send(302, "text/plain", "");
      return;
    }
  }
  if (path == "/") path = "/index.html";
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, path.endsWith(".js") ? "text/javascript" : "text/html");
    file.close();
  } else if (LittleFS.exists("/index.html")) {
    File file = LittleFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else server.send(404, "text/plain", "PandaSimFlag web files missing");
}

void setupWeb() {
  const char* headers[] = {"Content-Type"};
  server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, routeRequest);
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/matrix", HTTP_GET, sendMatrixState);
  server.on("/api/info", HTTP_GET, sendInfo);
  server.on("/api/logs", HTTP_GET, sendLogs);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/orientation", HTTP_POST, handleOrientation);
  server.on("/api/game-settings", HTTP_POST, handleGameSettingsSave);
  server.on("/api/game", HTTP_POST, handleGameInput);
  server.on("/api/update/firmware", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
  server.on("/api/update/filesystem", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
  server.on("/generate_204", HTTP_GET, routeRequest);
  server.on("/hotspot-detect.html", HTTP_GET, routeRequest);
  server.onNotFound(routeRequest);
  server.begin();
}

void handleUsb() {
  for (size_t bytes = 0; bytes < 1024 && Serial.available(); ++bytes) {
    const uint8_t value = Serial.read();
    if (usbState == UsbState::Prefix) {
      if (value == 0xFF) {
        if (++prefixLength == 6) { usbState = UsbState::Command; commandLength = 0; commandStarted = millis(); }
      } else prefixLength = 0;
    } else if (usbState == UsbState::Command) {
      if (value == '\n' || value == '\r') continue;
      if (commandLength < sizeof(usbCommand) - 1) usbCommand[commandLength++] = static_cast<char>(value);
      if (commandLength == 5) {
        usbCommand[5] = '\0';
        if (strcmp(usbCommand, "sleds") == 0) { usbState = UsbState::LedData; dataLength = 0; }
        else {
          if (strcmp(usbCommand, "proto") == 0) Serial.println("SIMHUB_1.0");
          else if (strcmp(usbCommand, "ledsc") == 0) Serial.println(LED_COUNT);
          usbState = UsbState::Prefix;
          prefixLength = 0;
        }
      }
    } else if (usbState == UsbState::LedData) {
      const uint8_t index = dataLength / 3;
      const uint8_t channel = dataLength % 3;
      if (channel == 0) incoming[index].r = value;
      else if (channel == 1) incoming[index].g = value;
      else incoming[index].b = value;
      if (++dataLength == LED_COUNT * 3) { usbState = UsbState::LedEnd; endLength = 0; }
    } else {
      endBytes[endLength++] = value;
      if (endLength == 3) {
        if (endBytes[0] == 0xFF && endBytes[1] == 0xFE && endBytes[2] == 0xFD) {
          if (inputMode == "usb") {
            lastUsbTelemetry = millis();
            usbTelemetrySeen = true;
            if (!gameSyncPaused) {
              ::memcpy(pixels, incoming, sizeof(pixels));
              ::memcpy(frame, pixels, sizeof(frame));
              animation = "none";
              activeFlag = "simhub-usb";
              currentConnectionMode = "USB / SimHub";
              currentGame = "Not reported";
              showFrame();
            }
          }
        }
        usbState = UsbState::Prefix;
        prefixLength = 0;
      }
    }
  }
  if (usbState == UsbState::Command && millis() - commandStarted > 100) {
    if (commandLength == 4 && strncmp(usbCommand, "proto", 4) == 0) Serial.println("SIMHUB_1.0");
    usbState = UsbState::Prefix;
    prefixLength = 0;
  }
}

void handleGameUdp() {
  const int packetSize = gameUdp.parsePacket();
  if (!packetSize) return;
  if (packetSize > MAX_TELEMETRY_BYTES) { gameUdp.flush(); return; }
  uint8_t buffer[MAX_TELEMETRY_BYTES + 1];
  const int count = gameUdp.read(buffer, min(packetSize, static_cast<int>(MAX_TELEMETRY_BYTES)));
  if (count <= 0) return;
  if (count >= 7 && ((wifiGame == "f1-2024" && buffer[0] == 0xE8 && buffer[1] == 0x07) ||
                     (wifiGame == "f1-2025" && buffer[0] == 0xE9 && buffer[1] == 0x07))) {
    handleF1Packet(buffer, count);
    return;
  }
  buffer[count] = '\0';
  JsonDocument doc;
  if (!deserializeJson(doc, (const char*)buffer)) {
    if (inputMode == "wifi" && doc["source"] != "webui") markWifiTelemetry();
    applyTelemetry(doc);
  }
}

void handleGt7Udp() {
  const int packetSize = gt7Udp.parsePacket();
  if (!packetSize) return;
  uint8_t packet[296];
  if (packetSize != sizeof(packet)) {
    gt7Udp.flush();
    return;
  }
  if (gt7Udp.read(packet, sizeof(packet)) == sizeof(packet)) handleGt7Packet(packet, sizeof(packet));
}

void sendGt7Heartbeat() {
  if (inputMode != "wifi" || wifiGame != "gt7" || millis() < nextGt7Heartbeat) return;
  nextGt7Heartbeat = millis() + 1000;
  IPAddress consoleIp;
  if (!consoleIp.fromString(gt7ConsoleIp)) return;
  gt7Udp.beginPacket(consoleIp, GT7_HEARTBEAT_PORT);
  gt7Udp.write('A');
  gt7Udp.endPacket();
}

void animate() {
  if (animation == "none" || millis() < nextAnimation) return;
  nextAnimation = millis() + animationPeriod;
  if (animation == "rain") {
    rainOffset = (rainOffset + 1) % 8;
    drawRain(rainOffset);
    ::memcpy(frame, pixels, sizeof(frame));
    showFrame();
    return;
  }
  animationOff = !animationOff;
      if (animation == "blink" && animationOff) fill_solid(pixels, LED_COUNT, CRGB::Black);
  else if (animation == "invert" && animationOff) {
    for (uint8_t i = 0; i < LED_COUNT; ++i) pixels[i] = CRGB(255 - frame[i].r, 255 - frame[i].g, 255 - frame[i].b);
      } else if (animation == "doubleyellow" && animationOff) {
        for (uint8_t i = 0; i < LED_COUNT; ++i) pixels[i] = frame[i] == CRGB::Yellow ? CRGB::Black : CRGB::Yellow;
  } else ::memcpy(pixels, frame, sizeof(frame));
  showFrame();
}

void clearTimedOutDisplay() {
  if (gameSyncPaused || gameConnected() ||
      !(inputMode == "wifi" ? wifiTelemetrySeen : usbTelemetrySeen)) return;
  resetGameTelemetry();
  fillFlag("idle");
}

void applyQueuedMatrixSettings() {
  MatrixSettings settings;
  if (xQueueReceive(matrixSettingsQueue, &settings, 0) != pdPASS) return;
  const bool sourceChanged = inputMode != settings.inputMode || wifiGame != settings.wifiGame || gt7ConsoleIp != settings.gt7ConsoleIp;
  inputMode = settings.inputMode;
  wifiGame = settings.wifiGame;
  gt7ConsoleIp = settings.gt7ConsoleIp;
  displayType = settings.displayType;
  ledPowerPercent = settings.ledPowerPercent;
  savedMatrixRotation = settings.matrixRotation;
  matrixRotation = settings.orientationActive ? settings.orientationRotation : savedMatrixRotation;
  const bool orientationEnded = orientationActive && !settings.orientationActive;
  orientationActive = settings.orientationActive;
  orientationLastContact = settings.orientationLastContact;
  normalFlagsBlink = settings.normalFlagsBlink;
  normalFlagBlinkPeriod = settings.normalFlagBlinkPeriod;
  // Keep the calibration dot visible even when normal matrix output is off.
  FastLED.setBrightness((uint16_t)(orientationActive ? max<uint8_t>(10, ledPowerPercent) : ledPowerPercent) * 255 / 100);
  matrixPowerLimitMa = settings.powerLimitMa;
  FastLED.setMaxPowerInVoltsAndMilliamps(5, matrixPowerLimitMa);
  if (sourceChanged) {
    resetGameTelemetry();
    nextGt7Heartbeat = 0;
    usbState = UsbState::Prefix;
    prefixLength = 0;
    if (!gameSyncPaused) fillFlag("idle");
  }
  if (orientationActive) {
    gameSyncPaused = true;
    currentConnectionMode = "Orientation assistant";
    fillFlag("orientation");
    pixels[0] = CRGB::Red;
    ::memcpy(frame, pixels, sizeof(frame));
  } else if (orientationEnded) {
    gameSyncPaused = false;
    resetGameTelemetry();
    fillFlag("idle");
  }
  showFrame();
}

void expireOrientationPreview() {
  if (!orientationActive || millis() - orientationLastContact < ORIENTATION_TIMEOUT_MS) return;
  orientationActive = false;
  matrixRotation = savedMatrixRotation;
  FastLED.setBrightness((uint16_t)ledPowerPercent * 255 / 100);
  gameSyncPaused = false;
  resetGameTelemetry();
  fillFlag("idle");
}

void handleQueuedTelemetry() {
  TelemetryCommand command;
  if (xQueueReceive(telemetryQueue, &command, 0) != pdPASS) return;
  JsonDocument doc;
  if (deserializeJson(doc, command.json)) return;
  if (inputMode == "wifi" && doc["source"] != "webui") markWifiTelemetry();
  applyTelemetry(doc);
}

void publishMatrixSnapshot() {
  MatrixSnapshot snapshot;
  for (size_t i = 0; i < LED_COUNT; ++i) {
    snapshot.pixels[i * 3] = pixels[i].r;
    snapshot.pixels[i * 3 + 1] = pixels[i].g;
    snapshot.pixels[i * 3 + 2] = pixels[i].b;
  }
  strlcpy(snapshot.game, currentGame.c_str(), sizeof(snapshot.game));
  strlcpy(snapshot.connectionMode, currentConnectionMode.c_str(), sizeof(snapshot.connectionMode));
  strlcpy(snapshot.flag, activeFlag.c_str(), sizeof(snapshot.flag));
  strlcpy(snapshot.animation, animation.c_str(), sizeof(snapshot.animation));
  snapshot.animationPeriod = animationPeriod;
  snapshot.animationPhase = animation == "rain" || activeFlag == "rain" ? rainOffset : animationOff;
  snapshot.nextAnimation = nextAnimation;
  snapshot.gameConnected = gameConnected();
  snapshot.gameSyncPaused = gameSyncPaused;
  snapshot.orientationActive = orientationActive;
  snapshot.outputBrightness = matrixOutputBrightness;
  snapshot.core = xPortGetCoreID();
  snapshot.frames = matrixFrames;
  snapshot.stackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
  xQueueOverwrite(matrixSnapshotQueue, &snapshot);
}

void matrixTask(void*) {
  // Initialize RMT/FastLED here so its interrupts are allocated on core 1 too.
  FastLED.addLeds<WS2812B, MATRIX_PIN, GRB>(wirePixels, LED_COUNT);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, SAFE_LED_LIMIT_MA);
  fillFlag("idle");
  if (!gameUdp.begin(GAME_UDP_PORT) || !gt7Udp.begin(GT7_UDP_PORT)) abort();
  uint32_t lastSnapshot = millis() - 20;
  for (;;) {
    applyQueuedMatrixSettings();
    expireOrientationPreview();
    handleUsb();
    handleGameUdp();
    handleGt7Udp();
    handleQueuedTelemetry();
    sendGt7Heartbeat();
    clearTimedOutDisplay();
    animate();
    if (matrixDirty) {
      for (uint8_t i = 0; i < LED_COUNT; ++i) {
        wirePixels[matrixWireIndex(i / 8, i % 8, matrixRotation)] = pixels[i];
      }
      matrixOutputBrightness = calculate_max_brightness_for_power_mW(
          FastLED.getBrightness(), 5UL * matrixPowerLimitMa);
      FastLED.show();
      matrixDirty = false;
      ++matrixFrames;
    }
    if (millis() - lastSnapshot >= 20) {
      publishMatrixSnapshot();
      lastSnapshot = millis();
    }
    // Bound each iteration and leave time for idle/system tasks on this core.
    vTaskDelay(1);
  }
}
}

void setup() {
  pandasimflag::logs::start();
  pandasimflag::logs::write("Booting firmware %s; reset reason %d", VERSION, esp_reset_reason());
  matrixSettingsQueue = xQueueCreate(1, sizeof(MatrixSettings));
  matrixSnapshotQueue = xQueueCreate(1, sizeof(MatrixSnapshot));
  telemetryQueue = xQueueCreate(4, sizeof(TelemetryCommand));
  if (!matrixSettingsQueue || !matrixSnapshotQueue || !telemetryQueue) abort();
  Serial.begin(115200);
  ledcSetup(STATUS_PWM_CHANNEL, 5000, 8);
  ledcAttachPin(STATUS_PIN, STATUS_PWM_CHANNEL);
  ledcWrite(STATUS_PWM_CHANNEL, 0);
  Wire.begin(1, 2); // SDA, SCL
  updatePowerDetection();
  filesystemMounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
  if (filesystemMounted) filesystemVersion = readFilesystemVersion();
  pandasimflag::logs::write("WebUI filesystem %s; version %s", filesystemMounted ? "mounted" : "unavailable", filesystemVersion.c_str());

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macSuffix[5];
  snprintf(macSuffix, sizeof(macSuffix), "%02X%02X", mac[4], mac[5]);
  suffix = macSuffix;
  apName = "PandaSimFlag-" + suffix;

  preferences.begin("device", true);
  configuredHostname = preferences.getString("hostname", apName);
  if (configuredHostname == "pandasimflag" || configuredHostname.equalsIgnoreCase(apName)) configuredHostname = apName;
  strlcpy(controlMatrixSettings.inputMode, preferences.getString("input", "wifi").c_str(), sizeof(controlMatrixSettings.inputMode));
  strlcpy(controlMatrixSettings.wifiGame, preferences.getString("wifiGame", "").c_str(), sizeof(controlMatrixSettings.wifiGame));
  strlcpy(controlMatrixSettings.gt7ConsoleIp, preferences.getString("gt7Ip", "").c_str(), sizeof(controlMatrixSettings.gt7ConsoleIp));
  strlcpy(controlMatrixSettings.displayType, preferences.getString("display", "flag").c_str(), sizeof(controlMatrixSettings.displayType));
  if (strcmp(controlMatrixSettings.displayType, "auto") == 0) strlcpy(controlMatrixSettings.displayType, "flag", sizeof(controlMatrixSettings.displayType));
  if (strcmp(controlMatrixSettings.wifiGame, "gt7") == 0) strlcpy(controlMatrixSettings.displayType, "gear", sizeof(controlMatrixSettings.displayType));
  controlMatrixSettings.ledPowerPercent = preferences.getUChar("ledPower", 10);
  controlMatrixSettings.matrixRotation = preferences.getUShort("rotation", 0);
  if (!validMatrixRotation(controlMatrixSettings.matrixRotation)) controlMatrixSettings.matrixRotation = 0;
  powerLedEnabled = preferences.getBool("powerLedOn", true);
  powerLedBrightnessPercent = preferences.getUChar("powerLedLevel", 20);
  if (powerLedBrightnessPercent > 100) powerLedBrightnessPercent = 20;
  controlMatrixSettings.normalFlagsBlink = preferences.getBool("normalBlink", false);
  controlMatrixSettings.normalFlagBlinkPeriod = preferences.getUShort("blinkPeriod", 250);
  preferences.end();

  preferences.begin("wifi", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();

  WiFi.setHostname(apName.c_str());
  WiFi.mode(wifiSsid.length() ? WIFI_STA : WIFI_AP);
  publishMatrixSettings();
  if (xTaskCreatePinnedToCore(matrixTask, "matrix", 12288, nullptr, 2, nullptr, MATRIX_CORE) != pdPASS) abort();
  if (wifiSsid.length()) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
      pandasimflag::logs::write("Wi-Fi connected at %s", WiFi.localIP().toString().c_str());
      startMdns();
    }
    else startAccessPoint();
  } else startAccessPoint();
  setupWeb();
  pandasimflag::logs::write("WebUI ready on core %d; matrix task on core %d", CONTROL_CORE, MATRIX_CORE);
}

void loop() {
  expireOrientationAssistant();
  handleMdnsConflict();
  if (accessPoint) dns.processNextRequest();
  server.handleClient();
  updatePowerDetection();
  updateStatusLed();
  delay(1);
}
