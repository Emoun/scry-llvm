// Integer arithmetic of all widths, signed and unsigned.
// expect-exit: 0
#include <stdint.h>

volatile int32_t a = 1000, b = -7, c = 3;
volatile uint32_t ua = 4000000000u, ub = 13;
volatile int64_t la = -123456789012LL, lb = 1000003;
volatile uint64_t ula = 0xFFFFFFFFFFFFFFF0ull, ulb = 7;
volatile int8_t sa = -100, sb = 3;
volatile uint8_t ba = 250, bb = 10;
volatile int16_t ha = -30000, hb = 7;
volatile uint16_t uha = 65000, uhb = 9;

int main(void) {
  if (a + b != 993) return 1;
  if (a - b != 1007) return 2;
  if (a * b != -7000) return 3;
  if (a / b != -142) return 4;      // truncating division
  if (a % b != 6) return 5;
  if (b / c != -2) return 6;
  if (b % c != -1) return 7;
  if (ua / ub != 307692307u) return 8;
  if (ua % ub != 9) return 9;
  if (ua + ub != 4000000013u) return 10;
  if (ua * ub != 460392448u) return 11;
  if (la / lb != -123456) return 12;
  if (la % lb != -418644) return 13;
  if (la * 3 != -370370367036LL) return 14;
  if (ula / ulb != 2635249153387078800ull) return 15;
  if (ula % ulb != 0) return 16;
  if (ula + ulb != 0xFFFFFFFFFFFFFFF7ull) return 17;
  if ((int)(sa + sb) != -97) return 18;
  if ((int)(sa * sb) != -300) return 19;
  if ((int)(int8_t)(sa * sb) != -44) return 20;
  if (ba + bb != 260) return 21;
  if ((uint8_t)(ba + bb) != 4) return 22;
  if (ha / hb != -4285) return 23;
  if (ha % hb != -5) return 24;
  if (uha / uhb != 7222) return 25;
  if ((uint16_t)(uha * uhb) != 60712) return 26;
  if (-a != -1000) return 27;
  if (~ua != 294967295u) return 28;
  return 0;
}
