// Operand ordering, values with many consumers, long straight-line blocks
// and constant arguments: everything the in-flight forwarding must get
// right.
// expect-exit: 0
#include <stdint.h>

volatile int va = 7, vb = 3, vc = 100;
volatile unsigned vu = 0x80000000u;
volatile int64_t vl = -5000000000LL;

static int sub3(int a, int b, int c) { return a - b - c; }
static int pick3(int c, int a, int b) { return c ? a : b; }
static int sum5(int a, int b, int c, int d, int e) { return a + 2 * b + 3 * c + 4 * d + 5 * e; }

int main(void) {
  int a = va, b = vb, c = vc;
  // Non-commutative operations with operands produced in both orders.
  if (a - b != 4) return 1;
  if (b - a != -4) return 2;
  if ((c << b) != 800) return 3;
  if ((c >> b) != 12) return 4;
  if ((b << a) != 384) return 5;
  if (c / b != 33) return 6;
  if (c % a != 2) return 7;
  if (vu >> 31 != 1) return 8;
  if ((int)(vu >> 1) != 0x40000000) return 9;
  if (vl >> 1 != -2500000000LL) return 10;
  if (sub3(a, b, c) != -96) return 11;
  if (sub3(c, a, b) != 90) return 12;
  if (sub3(b, c, a) != -104) return 13;
  if (sub3(1, 2, 3) != -4) return 14;
  if (sub3(a, 2, 3) != 2) return 15;
  if (sub3(1, b, 3) != -5) return 16;
  if (sub3(1, 2, c) != -101) return 17;
  if (pick3(a, b, c) != 3) return 18;
  if (pick3(0, b, c) != 100) return 19;
  if (sum5(a, b, c, 1, 2) != 327) return 20;
  // A value with many consumers.
  int s = a * a + a * b + a * c + a - b + c * a + (a ^ b) + (a | b) + (a & c);
  if (s != 1489) return 21;
  // A long straight-line block keeping early values live to the end.
  int x0 = a + 1, x1 = x0 + b, x2 = x1 * 2, x3 = x2 - c, x4 = x3 ^ 5, x5 = x4 + x0;
  int y0 = x5 * 3, y1 = y0 + x1, y2 = y1 - x2, y3 = y2 + x3, y4 = y3 * x4, y5 = y4 + x5;
  int z0 = y5 + y0, z1 = z0 - y1, z2 = z1 + y2, z3 = z2 ^ y3, z4 = z3 + y4, z5 = z4 - y5;
  int w0 = z5 + z0 - z1 + z2 - z3 + z4 - x0 + x1 - x2 + x3 - x4 + x5 + a - b + c;
  int w1 = w0 * 7 + z0 + z1 + z2 + z3 + z4 + z5 + y0 + y1 + y2 + y3 + y4 + y5;
  volatile int sink = w1;
  if (sink != w0 * 7 + z0 + z1 + z2 + z3 + z4 + z5 + y0 + y1 + y2 + y3 + y4 + y5) return 22;
  // Stores with the value and the address produced in both orders.
  int arr[4] = {0, 0, 0, 0};
  int *p = arr + (b & 3);
  *p = a;
  arr[a & 1] = b;
  if (arr[3] != 7 || arr[1] != 3) return 23;
  int64_t l = vl;
  int64_t m = l * 3 - (l >> 2) + (int64_t)a;
  if (m != -15000000000LL + 1250000000LL + 7) return 24;
  return 0;
}
