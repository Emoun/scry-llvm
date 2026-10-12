// Shifts, bitwise operations, rotations and bit counts.
// expect-exit: 0
#include <stdint.h>

volatile uint32_t x = 0x80000001u;
volatile int32_t sx = -16;
volatile uint64_t y = 0x8000000000000001ull;
volatile int64_t sy = -256;
volatile unsigned n = 3;
volatile uint8_t b8 = 0xF0;
volatile int8_t s8 = -128;
volatile uint16_t h16 = 0x8001;

int main(void) {
  if ((x << n) != 0x8) return 1;
  if ((x >> n) != 0x10000000u) return 2;
  if ((sx >> n) != -2) return 3;
  if ((sx << n) != -128) return 4;
  if ((y << n) != 0x8ull) return 5;
  if ((y >> n) != 0x1000000000000000ull) return 6;
  if ((sy >> n) != -32) return 7;
  if ((x & 0xFF) != 1) return 8;
  if ((x | 0xFF) != 0x800000FFu) return 9;
  if ((x ^ 0xFFFFFFFFu) != 0x7FFFFFFEu) return 10;
  if ((b8 >> 4) != 0xF) return 11;
  if ((uint8_t)(b8 << 1) != 0xE0) return 12;
  if ((s8 >> 7) != -1) return 13;
  if ((h16 >> 15) != 1) return 14;
  if ((uint16_t)(h16 << 1) != 2) return 15;
  if (__builtin_popcount(x) != 2) return 16;
  if (__builtin_clz(x) != 0) return 17;
  if (__builtin_ctz(x) != 0) return 18;
  if (__builtin_clz(n) != 30) return 19;
  if (__builtin_popcountll(y) != 2) return 20;
  if (__builtin_bswap32(x) != 0x01000080u) return 21;
  if (((x << 4) | (x >> 28)) != 0x18u) return 22; // rotate
  if ((x >> 31) != 1) return 23;
  if ((y >> 63) != 1) return 24;
  if ((x >> 0) != x) return 25;
  return 0;
}
