#include <cassert>
#include <string>

#include "logs.h"

int main() {
  using namespace pandasimflag::logs;
  uint64_t cursor = 123;
  assert(parseCursor("0", UINT64_MAX, cursor) && cursor == 0);
  assert(parseCursor("18446744073709551615", UINT64_MAX, cursor) && cursor == UINT64_MAX);
  assert(parseCursor("4294967295", UINT32_MAX, cursor) && cursor == UINT32_MAX);
  for (const char *invalid : {"", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616"}) {
    assert(!parseCursor(invalid, UINT64_MAX, cursor));
  }
  assert(!parseCursor("4294967296", UINT32_MAX, cursor));
  Buffer buffer;
  char output[kCapacity];
  auto snapshot = buffer.read(0, output);
  assert(snapshot.next == 0 && snapshot.size == 0 && !snapshot.dropped);

  buffer.append("first\n", 6);
  snapshot = buffer.read(0, output);
  assert(snapshot.next == 6 && !snapshot.dropped);
  assert(std::string(output, snapshot.size) == "first\n");
  buffer.append("second\n", 7);
  snapshot = buffer.read(snapshot.next, output);
  assert(snapshot.next == 13 && !snapshot.dropped);
  assert(std::string(output, snapshot.size) == "second\n");
  snapshot = buffer.read(snapshot.next, output);
  assert(snapshot.size == 0 && !snapshot.dropped);

  std::string history = "first\nsecond\n";
  const uint64_t stale = history.size();
  for (size_t index = 0; index < kCapacity + 123; ++index) {
    const char byte = 'a' + index % 26;
    history += byte;
    buffer.append(&byte, 1);
  }
  snapshot = buffer.read(stale, output);
  assert(snapshot.next == history.size() && snapshot.size == kCapacity && snapshot.dropped);
  assert(std::string(output, snapshot.size) == history.substr(history.size() - kCapacity));
  snapshot = buffer.read(history.size() - 37, output);
  assert(snapshot.size == 37 && !snapshot.dropped);
  assert(std::string(output, snapshot.size) == history.substr(history.size() - 37));
  // A second reader's cursor does not consume or clear the buffer.
  snapshot = buffer.read(0, output);
  assert(std::string(output, snapshot.size) == history.substr(history.size() - kCapacity));

  const std::string large = std::string(kCapacity, 'x') + std::string(kCapacity, 'y') + "tail";
  buffer.append(large.data(), large.size());
  snapshot = buffer.read(0, output);
  assert(snapshot.next == history.size() + large.size() && snapshot.dropped);
  assert(std::string(output, snapshot.size) == large.substr(large.size() - kCapacity));
  snapshot = buffer.read(snapshot.next + 1, output);
  assert(snapshot.size == kCapacity && snapshot.dropped);
}
