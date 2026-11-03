#include <assert.h>
#include "../include/matrix_layout.h"

// Positions measured on the prototype, viewed from the front.
static_assert(matrixWireIndex(7, 7) == 0, "LED 0: bottom-right");
static_assert(matrixWireIndex(6, 7) == 1, "LED 1: one row above LED 0");
static_assert(matrixWireIndex(0, 6) == 8, "LED 8: top, second column from right");
static_assert(matrixWireIndex(0, 7) == 7, "Top-right");
static_assert(matrixWireIndex(0, 0) == 56, "Top-left");
static_assert(matrixWireIndex(7, 0) == 63, "Bottom-left");

int main() {
  const int rotations[] = {0, 90, 180, 270};
  const uint8_t topLeft[] = {56, 7, 0, 63};
  const uint8_t nextToTopLeft[] = {55, 6, 15, 62};
  for (uint8_t turn = 0; turn < 4; ++turn) {
    assert(validMatrixRotation(rotations[turn]));
    assert(matrixWireIndex(0, 0, rotations[turn]) == topLeft[turn]);
    assert(matrixWireIndex(0, 1, rotations[turn]) == nextToTopLeft[turn]);
    bool seen[64] = {};
    for (uint8_t row = 0; row < 8; ++row) {
      for (uint8_t column = 0; column < 8; ++column) {
        const uint8_t index = matrixWireIndex(row, column, rotations[turn]);
        assert(index < 64 && !seen[index]);
        seen[index] = true;
      }
    }
    for (bool mapped : seen) assert(mapped);
  }
  const int invalidRotations[] = {-90, -1, 1, 45, 89, 360, 450};
  for (int invalid : invalidRotations) assert(!validMatrixRotation(invalid));
}
