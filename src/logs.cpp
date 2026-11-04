#include "logs.h"

#include <cstdarg>
#include <cstdio>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace pandasimflag { namespace logs {
namespace {
Buffer buffer;
portMUX_TYPE bufferLock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t previousOutput = nullptr;
uint32_t boot = 0;

void append(const char *text, size_t length) {
  portENTER_CRITICAL(&bufferLock);
  buffer.append(text, length);
  portEXIT_CRITICAL(&bufferLock);
}

int capture(const char *format, va_list arguments) {
  char text[512];
  va_list copy;
  va_copy(copy, arguments);
  const int length = std::vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (length > 0) {
    const size_t size = static_cast<size_t>(length) < sizeof(text) ? length : sizeof(text) - 1;
    // shortcut: cap SDK log fragments at 511 bytes; increase if diagnostics need longer messages.
    if (static_cast<size_t>(length) >= sizeof(text)) text[size - 1] = '\n';
    append(text, size);
  }
  return previousOutput(format, arguments);
}
}  // namespace

void start() {
  if (previousOutput) return;
  boot = esp_random();
  previousOutput = &std::vprintf;
  previousOutput = esp_log_set_vprintf(capture);
}

void write(const char *format, ...) {
  char text[512];
  const int prefix = std::snprintf(text, sizeof(text), "I (%lu) PandaSimFlag: ",
      static_cast<unsigned long>(esp_timer_get_time() / 1000));
  va_list arguments;
  va_start(arguments, format);
  const int length = std::vsnprintf(text + prefix, sizeof(text) - prefix, format, arguments);
  va_end(arguments);
  if (length < 0) return;
  const size_t size = static_cast<size_t>(prefix + length) < sizeof(text) - 1
      ? prefix + length : sizeof(text) - 2;
  text[size] = '\n';
  append(text, size + 1);
}

uint32_t boot_id() { return boot; }

Snapshot read(uint64_t since, char *output) {
  portENTER_CRITICAL(&bufferLock);
  const Snapshot snapshot = buffer.read(since, output);
  portEXIT_CRITICAL(&bufferLock);
  return snapshot;
}
}}  // namespace pandasimflag::logs
