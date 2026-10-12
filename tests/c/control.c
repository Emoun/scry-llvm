// Loops, switches, nested control flow and recursion.
// expect-exit: 0
#include <stdint.h>

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int collatz(unsigned n) {
  int steps = 0;
  while (n != 1) {
    n = (n & 1) ? 3 * n + 1 : n / 2;
    steps++;
  }
  return steps;
}

static int classify(int x) {
  switch (x) {
  case 0: return 100;
  case 1: return 101;
  case 2: return 102;
  case 5: return 105;
  case 10: return 110;
  case 100: return 200;
  default: return -1;
  }
}

static int gcd(int a, int b) {
  while (b) {
    int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

volatile int ten = 10;

int main(void) {
  if (fib(15) != 610) return 1;
  if (collatz(27) != 111) return 2;
  if (classify(5) != 105) return 3;
  if (classify(100) != 200) return 4;
  if (classify(7) != -1) return 5;
  if (gcd(1071, 462) != 21) return 6;
  int sum = 0;
  for (int i = 0; i < ten; i++)
    for (int j = 0; j <= i; j++)
      if ((i + j) % 3 == 0)
        sum += i * j;
  if (sum != 405) return 7;
  int count = 0, k = 0;
  do {
    k += 7;
    if (k % 2 == 0)
      continue;
    count++;
    if (count > 50)
      break;
  } while (k < 1000);
  if (count != 51) return 8;
  // Loop with 64-bit counter and early exit.
  uint64_t acc = 1;
  int iters = 0;
  while (acc < 1000000000000ull) {
    acc *= 3;
    iters++;
  }
  if (iters != 26) return 9;
  return 0;
}
