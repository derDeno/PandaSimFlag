#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <nvs_flash.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

namespace {
constexpr uint8_t MATRIX_PIN = 5;
constexpr uint8_t STATUS_PIN = 40;
constexpr uint16_t LED_COUNT = 64;
constexpr uint16_t GAME_UDP_PORT = 20777;
constexpr uint16_t GT7_UDP_PORT = 33740;
constexpr uint16_t GT7_HEARTBEAT_PORT = 33739;
constexpr uint32_t WIFI_TELEMETRY_TIMEOUT_MS = 30000;
constexpr uint8_t TUSB320_ADDRESS = 0x60;
constexpr uint16_t SAFE_LED_LIMIT_MA = 200;
constexpr uint16_t MEDIUM_LED_LIMIT_MA = 800;
constexpr uint16_t HIGH_LED_LIMIT_MA = 2000;
constexpr char VERSION[] = "0.1.0";

CRGB pixels[LED_COUNT];
CRGB frame[LED_COUNT];
WebServer server(80);
DNSServer dns;
WiFiUDP gameUdp;
WiFiUDP gt7Udp;
Preferences preferences;
String suffix;
String hostName = "pandasimflag";
String configuredHostname = "pandasimflag";
String inputMode = "wifi";
String wifiGame;
String gt7ConsoleIp;
String displayType = "flag";
uint8_t ledPowerPercent = 25;
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
  FastLED.show();
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
  FastLED.setMaxPowerInVoltsAndMilliamps(5, ledLimitMa);
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

void applyTelemetry(JsonDocument& doc) {
  const bool clearRequest = doc["display"].is<const char*>() && doc["display"] == "idle";
  const bool webUiControl = doc["source"] == "webui";
  if (inputMode != "wifi" && !clearRequest && !webUiControl) return;
  if (inputMode == "wifi") currentConnectionMode = "Wi-Fi / API";
  if (doc["game"].is<const char*>()) {
    currentGame = doc["game"].as<String>();
    currentGame.trim();
    if (currentGame.length() > 32) currentGame.remove(32);
    if (!currentGame.length()) currentGame = "Not reported";
  } else currentGame = selectedGameName();
  String display = doc["display"] | "";
  display.toLowerCase();
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
  server.send(200, "application/json", response);
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
  JsonDocument doc;
  doc["ssid"] = accessPoint ? apName : WiFi.SSID();
  doc["ip"] = (accessPoint ? WiFi.softAPIP() : WiFi.localIP()).toString();
  doc["hostname"] = hostName + ".local";
  doc["mac"] = WiFi.macAddress();
  doc["mode"] = accessPoint ? "ap" : "station";
  doc["connectionMode"] = accessPoint ? "Setup AP" : currentConnectionMode;
  doc["inputMode"] = inputMode;
  doc["wifiGame"] = wifiGame;
  doc["gt7ConsoleIp"] = gt7ConsoleIp;
  doc["displayType"] = displayType;
  doc["ledPowerPercent"] = ledPowerPercent;
  doc["normalFlagsBlink"] = normalFlagsBlink;
  doc["normalFlagBlinkPeriod"] = normalFlagBlinkPeriod;
  doc["deviceHostname"] = configuredHostname;
  doc["connected"] = accessPoint || WiFi.status() == WL_CONNECTED;
  doc["rssi"] = accessPoint ? 0 : WiFi.RSSI();
  doc["game"] = currentGame;
  doc["flag"] = activeFlag;
  doc["animation"] = animation;
  doc["version"] = VERSION;
  doc["filesystemVersion"] = filesystemVersion;
  doc["uptime"] = millis() / 1000;
  doc["powerController"] = tusb320Available ? "TUSB320" : "not detected";
  doc["usbAttached"] = attachedState == 2;
  doc["usbCurrentMode"] = attachedState != 2 ? "unattached" : detectedCurrentMode == 1 ? "1.5A" : detectedCurrentMode == 3 ? "3A" : detectedCurrentMode == 2 ? "charge accessory" : "default current";
  doc["ledLimitMa"] = ledLimitMa;
  JsonArray rgb = doc["pixels"].to<JsonArray>();
  for (const CRGB& pixel : pixels) {
    rgb.add(pixel.r);
    rgb.add(pixel.g);
    rgb.add(pixel.b);
  }
  replyJson(doc);
}

void startMdns() {
  MDNS.end();
  hostName = configuredHostname;
  if (!MDNS.begin(hostName.c_str())) {
    hostName += "-" + suffix;
    MDNS.begin(hostName.c_str());
  }
  MDNS.addService("http", "tcp", 80);
}

bool validHostname(const String& value) {
  if (!value.length() || value.length() > 30 || value[0] == '-' || value[value.length() - 1] == '-') return false;
  for (char c : value) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
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
  const String requestedWifiGame = doc["wifiGame"].is<const char*>() ? doc["wifiGame"].as<String>() : wifiGame;
  const String requestedGt7ConsoleIp = doc["gt7ConsoleIp"].is<const char*>() ? doc["gt7ConsoleIp"].as<String>() : gt7ConsoleIp;
  const int requestedLedPower = doc["ledPower"] | -1;
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
  preferences.end();
  if (!stored) {
    server.send(500, "application/json", "{\"error\":\"Could not save settings to NVS\"}");
    return;
  }
  configuredHostname = hostname;
  inputMode = requestedInputMode;
  wifiGame = requestedWifiGame;
  gt7ConsoleIp = requestedGt7ConsoleIp;
  if (inputMode == "wifi") currentGame = selectedGameName();
  ledPowerPercent = requestedLedPower;
  FastLED.setBrightness((uint16_t)ledPowerPercent * 255 / 100);
  startMdns();
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
  const String requestedDisplayType = doc["displayType"] | displayType;
  const bool requestedNormalFlagsBlink = doc["normalFlagsBlink"] | normalFlagsBlink;
  const int requestedNormalFlagBlinkPeriod = doc["normalFlagBlinkPeriod"] | normalFlagBlinkPeriod;
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
  if (wifiGame != requestedWifiGame) {
    currentF1Flag = "";
    currentF1Gear = "";
    currentF1EventFlag = "";
    f1SafetyCarActive = false;
    f1PenaltyFlagActive = false;
  }
  wifiGame = requestedWifiGame;
  gt7ConsoleIp = requestedGt7ConsoleIp;
  displayType = requestedDisplayType;
  normalFlagsBlink = requestedNormalFlagsBlink;
  normalFlagBlinkPeriod = requestedNormalFlagBlinkPeriod;
  currentGame = selectedGameName();
  currentConnectionMode = "Wi-Fi / API";
  server.send(200, "application/json", "{\"ok\":true}");
}

void startAccessPoint() {
  accessPoint = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  startMdns();
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
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(250);
  ESP.restart();
}

void handleReset() {
  const esp_err_t result = nvs_flash_erase();
  if (result != ESP_OK) {
    server.send(500, "application/json", "{\"error\":\"Could not erase NVS settings\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(300);
  ESP.restart();
}

void handleGameInput() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return;
  }
  if (inputMode == "wifi" && doc["source"] != "webui") markWifiTelemetry();
  applyTelemetry(doc);
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
      if (updateFilesystem) filesystemMounted = LittleFS.begin(false);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && updateStarted) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      updateError = Update.errorString();
      Update.abort();
      updateStarted = false;
      if (updateFilesystem) filesystemMounted = LittleFS.begin(false);
    }
  } else if (upload.status == UPLOAD_FILE_END && updateStarted) {
    updateSucceeded = Update.end(true);
    if (!updateSucceeded) updateError = Update.errorString();
    updateStarted = false;
    if (updateFilesystem) {
      filesystemMounted = LittleFS.begin(false);
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
    if (updateFilesystem && !filesystemMounted) filesystemMounted = LittleFS.begin(false);
  }
}

void handleUpdateComplete() {
  if (!updateSucceeded) {
    JsonDocument doc;
    doc["error"] = updateError.length() ? updateError : "Firmware update failed";
    String response;
    serializeJson(doc, response);
    server.send(400, "application/json", response);
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500);
  ESP.restart();
}

void routeRequest() {
  String path = server.uri();
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
  server.on("/api/wifi", HTTP_POST, handleWifiSave);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/game-settings", HTTP_POST, handleGameSettingsSave);
  server.on("/api/game", HTTP_POST, handleGameInput);
  server.on("/api/update/firmware", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
  server.on("/api/update/filesystem", HTTP_POST, handleUpdateComplete, handleUpdateUpload);
  server.on("/generate_204", HTTP_GET, routeRequest);
  server.on("/hotspot-detect.html", HTTP_GET, routeRequest);
  server.onNotFound(routeRequest);
  server.begin();
  gameUdp.begin(GAME_UDP_PORT);
  gt7Udp.begin(GT7_UDP_PORT);
}

void handleUsb() {
  while (Serial.available()) {
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
            ::memcpy(pixels, incoming, sizeof(pixels));
            ::memcpy(frame, pixels, sizeof(frame));
            animation = "none";
            activeFlag = "simhub-usb";
            currentConnectionMode = "USB / SimHub";
            currentGame = "Not reported";
            showFrame();
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
  if (!packetSize || packetSize > 1472) return;
  uint8_t buffer[1473];
  const int count = gameUdp.read(buffer, min(packetSize, 1472));
  if (count <= 0) return;
  if (count >= 7 && ((wifiGame == "f1-2024" && buffer[0] == 0xE8 && buffer[1] == 0x07) ||
                     (wifiGame == "f1-2025" && buffer[0] == 0xE9 && buffer[1] == 0x07))) {
    handleF1Packet(buffer, count);
    return;
  }
  buffer[count] = '\0';
  JsonDocument doc;
  if (!deserializeJson(doc, (const char*)buffer)) {
    if (inputMode == "wifi") markWifiTelemetry();
    applyTelemetry(doc);
  }
}

void handleGt7Udp() {
  const int packetSize = gt7Udp.parsePacket();
  if (!packetSize) return;
  uint8_t packet[296];
  if (packetSize != sizeof(packet)) {
    while (gt7Udp.available()) gt7Udp.read();
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

void clearTimedOutWifiDisplay() {
  if (inputMode != "wifi" || !wifiTelemetrySeen || millis() - lastWifiTelemetry < WIFI_TELEMETRY_TIMEOUT_MS) return;
  wifiTelemetrySeen = false;
  fillFlag("idle");
}
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_PIN, OUTPUT);
  FastLED.addLeds<WS2812B, MATRIX_PIN, GRB>(pixels, LED_COUNT);
  FastLED.setBrightness(64);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, SAFE_LED_LIMIT_MA);
  FastLED.clear(true);
  Wire.begin(2, 1); // SDA, SCL
  updatePowerDetection();
  filesystemMounted = LittleFS.begin(false);
  if (filesystemMounted) filesystemVersion = readFilesystemVersion();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macSuffix[5];
  snprintf(macSuffix, sizeof(macSuffix), "%02X%02X", mac[4], mac[5]);
  suffix = macSuffix;
  apName = "PandaSimFlag-" + suffix;

  preferences.begin("device", true);
  configuredHostname = preferences.getString("hostname", "pandasimflag");
  inputMode = preferences.getString("input", "wifi");
  wifiGame = preferences.getString("wifiGame", "");
  gt7ConsoleIp = preferences.getString("gt7Ip", "");
  displayType = preferences.getString("display", "flag");
  if (displayType == "auto") displayType = "flag";
  if (wifiGame == "gt7") displayType = "gear";
  ledPowerPercent = preferences.getUChar("ledPower", 25);
  normalFlagsBlink = preferences.getBool("normalBlink", false);
  normalFlagBlinkPeriod = preferences.getUShort("blinkPeriod", 250);
  preferences.end();
  FastLED.setBrightness((uint16_t)ledPowerPercent * 255 / 100);

  preferences.begin("wifi", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();

  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    const uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
    if (WiFi.status() == WL_CONNECTED) startMdns();
    else startAccessPoint();
  } else startAccessPoint();
  setupWeb();
  fillFlag("idle");
}

void loop() {
  if (accessPoint) dns.processNextRequest();
  server.handleClient();
  handleUsb();
  handleGameUdp();
  handleGt7Udp();
  sendGt7Heartbeat();
  updatePowerDetection();
  animate();
  clearTimedOutWifiDisplay();
  digitalWrite(STATUS_PIN, ((millis() / (accessPoint ? 250 : 1000)) % 2) ? HIGH : LOW);
}
