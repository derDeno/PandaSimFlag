#pragma once

#include <stdint.h>

// Logical origin: top-left. Wire origin: bottom-right, alternating vertical columns.
constexpr uint8_t matrixWireIndex(uint8_t row, uint8_t column) {
  return (7 - column) * 8 + (column % 2 == 0 ? row : 7 - row);
}

constexpr bool validMatrixRotation(int degrees) {
  return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

inline uint8_t matrixWireIndex(uint8_t row, uint8_t column, uint16_t rotation) {
  switch (rotation) {
    case 90: return matrixWireIndex(column, 7 - row);
    case 180: return matrixWireIndex(7 - row, 7 - column);
    case 270: return matrixWireIndex(7 - column, row);
    default: return matrixWireIndex(row, column);
  }
}
