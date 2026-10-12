// Floating point via compiler-rt soft-float routines and the small libm.
// expect-exit: 0
#include <math.h>

volatile double a = 1.5, b = -2.25, c = 1e10;
volatile float fa = 0.5f, fb = 3.0f;
volatile int i = 7;
volatile unsigned u = 4000000000u;
volatile long long ll = -1234567890123LL;

static int close(double x, double y) {
  double d = x - y;
  if (d < 0) d = -d;
  double m = x < 0 ? -x : x;
  if (m < 1) m = 1;
  return d <= 1e-9 * m;
}

int main(void) {
  if (a + b != -0.75) return 1;
  if (a * b != -3.375) return 2;
  if (a / b != -0.6666666666666666) return 3;
  if (!(a > b)) return 4;
  if (a == b) return 5;
  if ((int)b != -2) return 6;
  if ((double)i != 7.0) return 7;
  if ((double)u != 4000000000.0) return 8;
  if ((long long)(c * 2) != 20000000000LL) return 9;
  if ((double)ll != -1234567890123.0) return 10;
  if (fa + fb != 3.5f) return 11;
  if ((double)fa != 0.5) return 12;
  if ((float)a != 1.5f) return 13;
  if ((int)(fb * fa) != 1) return 14;
  if (fabs(b) != 2.25) return 15;
  if (!close(sqrt(2.0), 1.4142135623730951)) return 16;
  if (!close(cos(0.0), 1.0)) return 17;
  if (!close(sin(M_PI / 2), 1.0)) return 18;
  if (!close(exp(1.0), M_E)) return 19;
  if (!close(log(M_E), 1.0)) return 20;
  if (!close(pow(2.0, 10.0), 1024.0)) return 21;
  if (floor(b) != -3.0) return 22;
  if (ceil(b) != -2.0) return 23;
  if (fmod(7.5, 2.0) != 1.5) return 24;
  unsigned long long big = 18000000000000000000ull;
  if ((double)big != 1.8e19) return 25;
  if ((unsigned long long)(c * 1.5) != 15000000000ull) return 26;
  return 0;
}
