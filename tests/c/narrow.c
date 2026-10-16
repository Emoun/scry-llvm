// Narrow (8/16-bit) values widened before arithmetic: the lowering may drop
// the widening cast when the consumer is indifferent to the width, but not
// when the operation would then run at the narrow width (an implicit operand
// takes the single input's type) or when the value carries a signed tag.
// expect-exit: 0
#include <stdint.h>

volatile uint8_t b = 255;
volatile uint8_t c = 3;
volatile uint16_t h = 0xFFFF;
volatile int8_t s = -4;
volatile uint32_t w = 1000;
volatile int32_t sw = -8;
volatile uint32_t mask = 0xFFFFFFFFu;

int main(void) {
  // Implicit second operands: add/sub/and by 1, or/xor by all ones, div by 2.
  if ((uint32_t)b + 1 != 256) return 1;
  if ((uint32_t)b - 1 != 254) return 2;
  if ((uint32_t)h + 1 != 65536) return 3;
  if (((uint32_t)b | 0xFFFFFFFFu) != 0xFFFFFFFFu) return 4;
  if (((uint32_t)b ^ 0xFFFFFFFFu) != 0xFFFFFF00u) return 5;
  if ((uint32_t)b / 2 != 127) return 6;
  if ((uint32_t)b * 2 != 510) return 7;
  if ((uint32_t)b * (uint32_t)c != 765) return 8;
  // Wide operands: the narrow value may be used as it is.
  if ((uint32_t)b + w != 1255) return 9;
  if ((w - (uint32_t)b) != 745) return 10;
  if (((uint32_t)b & mask) != 255) return 11;
  if ((uint32_t)b < w) { } else return 12;
  // A value from an arithmetic shift is tagged signed; widening it must
  // zero-extend.
  if ((uint32_t)(uint8_t)(s >> 1) != 254) return 13;
  if ((uint32_t)(uint8_t)(s >> 1) + w != 1254) return 14;
  if ((uint64_t)(uint32_t)(sw >> 1) != 0xFFFFFFFCull) return 15;
  if ((uint64_t)(uint32_t)(sw >> 1) + 1 != 0xFFFFFFFDull) return 16;
  // Shift amounts and indices from narrow values.
  if ((w << c) != 8000) return 17;
  if ((w >> c) != 125) return 18;
  return 0;
}
