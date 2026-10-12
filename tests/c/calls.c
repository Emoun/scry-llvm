// Calling convention: many arguments, stack-passed arguments, 64-bit
// arguments, function pointers, varargs, struct passing and returns.
// expect-exit: 0
#include <stdarg.h>
#include <stdint.h>

static int add6(int a, int b, int c, int d, int e, int f) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

static int64_t mix(int8_t a, int64_t b, uint16_t c, int32_t d, int64_t e,
                   uint8_t f, int64_t g) {
  return a + b + c + d + e + f + g;
}

static int sum_var(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static int64_t sum_var64(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int64_t s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int64_t);
  va_end(ap);
  return s;
}

struct pair {
  int a;
  int b;
};

struct big {
  int v[10];
};

static struct pair make_pair(int a, int b) {
  struct pair p = {a, b};
  return p;
}

static int pair_sum(struct pair p) { return p.a + p.b; }

static int big_sum(struct big b) {
  int s = 0;
  for (int i = 0; i < 10; i++)
    s += b.v[i];
  return s;
}

static int apply(int (*f)(int, int), int x, int y) { return f(x, y); }
static int mul(int x, int y) { return x * y; }
static int sub(int x, int y) { return x - y; }

static int depth(int n) { return n == 0 ? 0 : 1 + depth(n - 1); }

static void swap(int *a, int *b) {
  int t = *a;
  *a = *b;
  *b = t;
}

int main(void) {
  if (add6(1, 2, 3, 4, 5, 6) != 91) return 1;
  if (mix(-1, 1000000000000LL, 60000, -5, -2000000000000LL, 200, 7) !=
      -999999939799LL)
    return 2;
  if (sum_var(5, 1, 2, 3, 4, 5) != 15) return 3;
  if (sum_var(0) != 0) return 4;
  if (sum_var64(3, 1LL << 40, 1LL << 41, 7LL) != ((1LL << 40) + (1LL << 41) + 7))
    return 5;
  struct pair p = make_pair(3, 4);
  if (p.a != 3 || p.b != 4) return 6;
  if (pair_sum(p) != 7) return 7;
  struct big b;
  for (int i = 0; i < 10; i++)
    b.v[i] = i;
  if (big_sum(b) != 45) return 8;
  if (apply(mul, 6, 7) != 42) return 9;
  if (apply(sub, 6, 7) != -1) return 10;
  int (*fp)(int, int) = mul;
  if (fp(3, 3) != 9) return 11;
  if (depth(20) != 20) return 12;
  int x = 1, y = 2;
  swap(&x, &y);
  if (x != 2 || y != 1) return 13;
  return 0;
}
