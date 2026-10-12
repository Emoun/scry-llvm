// Comparisons and selects, signed and unsigned, all widths.
// expect-exit: 0
#include <stdint.h>

volatile int32_t a = -5, b = 7;
volatile uint32_t ua = 0xFFFFFFF0u, ub = 16;
volatile int64_t la = -5000000000LL, lb = 1;
volatile uint64_t ula = 0xFFFFFFFFFFFFFFF0ull, ulb = 2;
volatile int8_t sa = -1, sb = 1;
volatile uint8_t ba = 255, bb = 1;
volatile int16_t ha = -300, hb = 300;

static int sel(int c, int x, int y) { return c ? x : y; }

int main(void) {
  if (!(a < b)) return 1;
  if (a > b) return 2;
  if (!(a <= b)) return 3;
  if (a >= b) return 4;
  if (a == b) return 5;
  if (!(a != b)) return 6;
  if (ua < ub) return 7;      // unsigned
  if (!(ua > ub)) return 8;
  if ((int32_t)ua > (int32_t)ub) return 9;  // signed reinterpretation
  if (!(la < lb)) return 10;
  if (ula < ulb) return 11;
  if (!((int64_t)ula < (int64_t)ulb)) return 12;
  if (!(sa < sb)) return 13;
  if (ba < bb) return 14;
  if (!((int8_t)ba < (int8_t)bb)) return 15;
  if (!(ha < hb)) return 16;
  if (ha == hb) return 17;
  if (sel(a < b, 10, 20) != 10) return 18;
  if (sel(a > b, 10, 20) != 20) return 19;
  if ((a < b ? la : lb) != la) return 20;
  if ((ua == 0xFFFFFFF0u) != 1) return 21;
  if ((a != -5) != 0) return 22;
  int64_t m = la < lb ? la : lb;
  if (m != la) return 23;
  uint8_t t = ba > bb ? ba : bb;
  if (t != 255) return 24;
  return 0;
}
