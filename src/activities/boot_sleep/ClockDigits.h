#pragma once

#include <cstdint>

// A clock face font: 1-bit row bitmaps (MSB first, rows padded to whole bytes)
// for the characters in CLOCK_DIGIT_CHARS.
struct ClockDigitGlyph {
  uint8_t width;
  uint8_t height;
  uint8_t advance;
  int8_t left;      // pen position to the bitmap's left edge
  int16_t top;      // digit top to the bitmap's top edge (negative for overshoot)
  uint32_t offset;  // into ClockDigitFace::bitmap
};

constexpr const char CLOCK_DIGIT_CHARS[] = "0123456789:-";
constexpr int CLOCK_DIGIT_COLON = 10;
constexpr int CLOCK_DIGIT_DASH = 11;

struct ClockDigitFace {
  int height;  // digit height, overshoot included
  const uint8_t* bitmap;
  ClockDigitGlyph glyphs[sizeof(CLOCK_DIGIT_CHARS) - 1];
};
