// Platform stand-ins; check_telemetry.py compiles the actual firmware functions.
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using std::isfinite;
using std::max;
using std::min;

struct String : std::string {
  using std::string::string;
  using std::string::operator=;
  String(const std::string& value) : std::string(value) {}
  explicit String(int value) : std::string(std::to_string(value)) {}
  void toLowerCase() { for (char& c : *this) c = std::tolower(static_cast<unsigned char>(c)); }
  void toUpperCase() { for (char& c : *this) c = std::toupper(static_cast<unsigned char>(c)); }
  bool startsWith(const char* prefix) const { return find(prefix) == 0; }
  String substring(size_t start) const { return substr(start); }
  void remove(size_t start) { erase(start); }
  void trim() {
    const auto first = find_first_not_of(" \t\r\n");
    *this = first == npos ? "" : substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
  void replace(const char* from, const char* to) {
    size_t position = 0;
    while ((position = find(from, position)) != npos) {
      std::string::replace(position, strlen(from), to);
      position += strlen(to);
    }
  }
};
void convertFromJson(JsonVariantConst value, String& result) { result = value.as<std::string>(); }

struct CRGB {
  uint8_t r, g, b;
  enum Color : uint32_t { Black = 0, Red = 0xff0000, Green = 0x008000, Blue = 0x0000ff,
                          White = 0xffffff, Yellow = 0xffff00, Orange = 0xffa500 };
  CRGB() = default;
  CRGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}
  CRGB(uint32_t value) : r(value >> 16), g(value >> 8), b(value) {}
  bool operator==(const CRGB& other) const { return r == other.r && g == other.g && b == other.b; }
};
void fill_solid(CRGB* values, size_t count, CRGB color) { std::fill_n(values, count, color); }
template <typename T, typename L, typename H> T constrain(T value, L low, H high) {
  return std::max<T>(low, std::min<T>(high, value));
}

constexpr size_t LED_COUNT = 64;
CRGB pixels[LED_COUNT] = {}, frame[LED_COUNT] = {};
String activeFlag, animation = "none", inputMode = "wifi", wifiGame = "f1-2025";
String displayType = "flag", currentGame, currentConnectionMode;
String currentF1Flag, currentF1Gear, currentF1EventFlag;
bool orientationActive = false, gameSyncPaused = false, normalFlagsBlink = true;
bool animationOff = false, f1SafetyCarActive = false, f1PenaltyFlagActive = false;
bool wifiTelemetrySeen = false, usbTelemetrySeen = false;
uint16_t animationPeriod = 250, normalFlagBlinkPeriod = 250;
uint8_t rainOffset = 0;
uint32_t nextAnimation = 0, lastWifiTelemetry = 0, f1CheckeredStarted = 0, f1PenaltyFlagStarted = 0;
uint32_t now = 1000, writes = 0;
uint32_t millis() { return now; }
void showFrame() { ++writes; }

#include "telemetry_under_test.h"

void json(const char* payload) {
  JsonDocument doc;
  assert(!deserializeJson(doc, payload));
  applyTelemetry(doc);
}

void checkAnimation(const char* flag, const char* mode) {
  fillFlag("idle");
  fillFlag(flag);
  setAnimation(mode, 250);
  assert(nextAnimation == now + 250);
  now = nextAnimation;
  animate();
  CRGB saved[LED_COUNT];
  memcpy(saved, pixels, sizeof(saved));
  const auto deadline = nextAnimation;
  const auto before = writes;
  for (int i = 0; i < 20; ++i) {
    ++now;
    fillFlag(flag);
    setAnimation(mode, 250);
    animate();
    assert(writes == before && nextAnimation == deadline);
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);
  }
  now = deadline;
  animate();
  assert(writes == before + 1 && memcmp(saved, pixels, sizeof(saved)) != 0);
  fillFlag("idle");
  assert(animation == "none");
  for (auto pixel : pixels) assert(pixel == CRGB::Black);
}

int main() {
  for (auto pair : {std::make_pair("green", "blink"), {"doubleyellow", "doubleyellow"},
                    {"checkered", "invert"}, {"blackwhite", "invert"}, {"rain", "rain"}}) {
    checkAnimation(pair.first, pair.second);
  }
  json(R"({"display":"yellow"})");
  now = nextAnimation;
  animate();
  assert(animationOff);
  const auto before = writes, deadline = nextAnimation;
  json(R"({"display":"yellow"})");
  assert(animationOff && writes == before && nextAnimation == deadline);
  normalFlagsBlink = false;
  json(R"({"display":"yellow"})");
  assert(animation == "none" && !animationOff);
  for (auto pixel : pixels) assert(pixel == CRGB::Yellow);
  normalFlagsBlink = true;
  json(R"({"display":"blue"})");
  assert(activeFlag == "blue" && !animationOff && nextAnimation == now + 250);

  displayType = "pedals";
  json(R"({"throttle":0,"brake":100})");
  auto unchanged = writes;
  for (int i = 0; i < 20; ++i) json(R"({"throttle":0,"brake":100})");
  assert(writes == unchanged);
  for (int row = 0; row < 8; ++row) for (int col = 0; col < 8; ++col)
    assert(pixels[row * 8 + col] == (col < 4 ? CRGB::Black : CRGB::Red));
  fillPedals(100, 0);
  assert(writes == unchanged + 1);
  fillPedals(NAN, 100);
  assert(writes == unchanged + 1);
  fillFlag("red");
  fillPedals(100, 0);
  assert(activeFlag == "pedals" && pixels[0] == CRGB::Green && pixels[4] == CRGB::Black);
  displayType = "tyres";
  json(R"({"tyreTemps":[80,90,110,100]})");
  unchanged = writes;
  json(R"({"tyreTemps":[81,91,111,101]})");
  assert(writes == unchanged);
  json(R"({"tyreTemps":[90,90,110,100]})");
  assert(writes == unchanged + 1 && pixels[0] == CRGB::Green);

  // Interleave real F1 telemetry, status and event packets for both supported formats.
  for (int year : {2024, 2025}) {
    wifiGame = year == 2024 ? "f1-2024" : "f1-2025";
    uint8_t telemetry[1352] = {}, status[1239] = {}, event[45] = {};
    for (auto packet : {telemetry, status, event}) {
      packet[0] = year & 0xff; packet[1] = year >> 8;
      packet[2] = year % 100; packet[27] = 3;
    }
    telemetry[6] = 6; status[6] = 7; event[6] = 3;
    const size_t car = 29 + 3 * 60;
    telemetry[car + 15] = 5;
    const float brake = 1.0f;
    memcpy(telemetry + car + 10, &brake, sizeof(brake));
    // Surface RL/RR/FL/FR; distinct inner temperatures must not affect the display.
    telemetry[car + 30] = 110; telemetry[car + 31] = 95;
    telemetry[car + 32] = 80; telemetry[car + 33] = 100;
    memset(telemetry + car + 34, 200, 4);
    status[29 + 3 * 55 + 28] = 1;
    for (const char* mode : {"pedals", "tyres"}) {
      displayType = mode;
      handleF1Packet(telemetry, sizeof(telemetry));
      assert(activeFlag == mode && animation == "none");
      if (displayType == "tyres") {
        const CRGB colors[] = {CRGB::Blue, CRGB::Green, CRGB::Red, CRGB::Green};
        for (int row = 0; row < 8; ++row) for (int col = 0; col < 8; ++col)
          assert(pixels[row * 8 + col] == colors[(row / 4) * 2 + col / 4]);
      }
      unchanged = writes;
      for (const char* code : {"SCAR", "RDFL", "PENA", "CHQF", "SSTA"}) {
        memcpy(event + 29, code, 4); event[33] = 1; event[34] = 0;
        handleF1Packet(status, sizeof(status));
        handleF1Packet(event, sizeof(event));
        handleF1Packet(telemetry, sizeof(telemetry));
        assert(activeFlag == mode && animation == "none" && writes == unchanged);
      }
    }
    displayType = "flaggear";
    currentF1Flag = "";
    applyF1Display();
    assert(activeFlag == "gear-5");
    handleF1Packet(status, sizeof(status));
    assert(activeFlag == "green" && animation == "blink");
    now = nextAnimation;
    animate();
    unchanged = writes;
    handleF1Packet(status, sizeof(status));
    assert(animationOff && writes == unchanged);
    status[29 + 3 * 55 + 28] = 0;
    handleF1Packet(status, sizeof(status));
    assert(activeFlag == "gear-5" && animation == "none");
    displayType = "flag";
    applyF1Display();
    assert(activeFlag == "idle");
  }
  puts("Telemetry regression checks passed (flags, pedals, tyres, F1 24/25 and JSON).");
}
