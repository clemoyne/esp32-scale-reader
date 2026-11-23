// Types used in function signatures, in a header so they come before the
// prototypes the Arduino builder generates for the .ino.
#pragma once
#include <stdint.h>
#include <time.h>

struct Reading {
  float kg;
  uint32_t atMs;  // millis() when it locked
  time_t epoch;   // 0 until the clock is known
  int rssi;
};

struct Packet {
  uint8_t len;
  uint8_t data[31];
};
