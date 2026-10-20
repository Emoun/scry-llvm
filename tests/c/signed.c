// Signed compares against constants and against values known to be
// non-negative (no signed cast needed for those operands), and right shifts
// by one (a one-input division by two in the ISA, floor division).
// expect-exit: 0
#include <stdint.h>

volatile int32_t a = -5;
volatile int32_t b = 7;
volatile int32_t big = 0x7fffffff;
volatile int32_t neg = (int32_t)0x80000000;
volatile uint8_t u8 = 200;
volatile uint16_t u16 = 60000;
volatile uint32_t u32 = 0xC0000000u;
volatile int8_t s8 = -3;
volatile int16_t s16 = -3;
volatile int64_t s64 = -3;
volatile uint64_t u64 = 0xFFFFFFFFFFFFFFFFull;
volatile unsigned sh = 1;

int main(void) {
  // Constants on either side.
  if (!(a < 0)) return 1;
  if (!(a < 3)) return 2;
  if (!(a > -10)) return 3;
  if (!(-10 < a)) return 4;
  if (!(b >= 7)) return 5;
  if (!(neg < 0)) return 6;
  if (!(big > 0)) return 7;
  if (!(neg < big)) return 8;
  if (!(a <= -5)) return 9;
  // One operand known non-negative (widened, masked, shifted, divided).
  if (!(a < (int32_t)u8)) return 10;          // -5 < 200
  if (!((int32_t)u16 > a)) return 11;         // 60000 > -5
  if (!(a < (int32_t)(u32 >> 1))) return 12;  // -5 < 0x60000000
  if (!((int32_t)(u32 & 0x7fffffff) > a)) return 13;
  if (!((int32_t)(u8 / 3) > a)) return 14;
  if (!((int32_t)(u8 % 7) >= 0)) return 15;
  if ((int32_t)u8 < a) return 16;
  // Values with the top bit set must still compare as negative.
  if (!((int32_t)u32 < 0)) return 17;         // 0xC0000000 as int is negative
  if (!((int32_t)u32 < (int32_t)u8)) return 18;
  if ((int32_t)u32 > b) return 19;
  // Narrow signed compares.
  if (!(s8 < 0)) return 20;
  if (!(s8 < (int8_t)1)) return 21;
  if (!(s16 < 0)) return 22;
  if (!(s64 < 0)) return 23;
  if (!(s64 < (int64_t)u64)) return 24;       // wait: u64 as int64 is -1; -3 < -1
  // Right shifts by one: floor division.
  if ((a >> 1) != -3) return 25;              // -5 >> 1 == -3
  if ((s8 >> 1) != -2) return 26;             // -3 >> 1 == -2
  if ((s16 >> 1) != -2) return 27;
  if ((s64 >> 1) != -2) return 28;
  if ((u8 >> 1) != 100) return 29;
  if ((u32 >> 1) != 0x60000000u) return 30;
  if ((u64 >> 1) != 0x7FFFFFFFFFFFFFFFull) return 31;
  if ((b >> 1) != 3) return 32;
  if ((neg >> 1) != (int32_t)0xC0000000) return 33;
  if (((uint32_t)(uint8_t)(s8 >> 1)) != 254) return 34;
  if ((a >> sh) != -3) return 35;             // variable amount: the shift path
  return 0;
}
