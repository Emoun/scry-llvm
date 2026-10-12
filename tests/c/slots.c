// Frame-slot sharing: values whose live ranges overlap must never share a
// slot, including arguments that are first used late in the function (the
// scheduler may sink the ARG pseudos below other instructions).
// expect-exit: 0
__attribute__((noinline)) static int late_first_use(int a, int b) {
  int sb = b >> 31;
  unsigned bu = (unsigned)(b ^ sb) + (unsigned)(-sb);
  int sa = a >> 31;
  unsigned au = (unsigned)(a ^ sa) + (unsigned)(-sa);
  int s = sa ^ sb;
  return (int)((au / bu) ^ s) - s;
}

__attribute__((noinline)) static long long many_live(int n) {
  int v[16];
  for (int i = 0; i < 16; i++)
    v[i] = n * (i + 1);
  long long acc = 0;
  for (int i = 0; i < 16; i++)
    acc += (long long)v[i] * v[15 - i];
  return acc;
}

volatile int a = 1000, b = -7, n = 3;

int main(void) {
  if (late_first_use(a, b) != -142) return 1;
  if (late_first_use(b, a) != 0) return 2;
  if (late_first_use(-a, -b) != -142) return 3;
  if (many_live(n) != 7344) return 4;
  return 0;
}
