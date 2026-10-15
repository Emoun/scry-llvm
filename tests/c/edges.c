// Values carried in flight across block edges: more than four values live
// across branches and loop back-edges, loops with several exits, values
// live through blocks that do not use them, and predecessors of different
// lengths.
// expect-exit: 0
#include <stdint.h>

volatile int vn = 20, vk = 3;
volatile int64_t vl = 1;

static int many_live(int n, int k) {
  int a = n + 1, b = n * 2, c = n ^ 7, d = n - k, e = k * 5, f = n + k, g = n | 9;
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (i & 1)
      s += a + b - c;       // a, b, c live into both arms
    else
      s += d * e + f - g;   // d, e, f, g too
    s ^= i;
  }
  return s + a + b + c + d + e + f + g;
}

static int64_t mixed(int n) {
  int64_t x = vl, y = 2 * vl;
  int i = 0, j = n;
  while (i < j) {
    x = x * 3 + y;
    y = y - x / 7;
    if (x > 1000000)
      break;
    i++;
    j--;
  }
  return x + y + i - j;
}

static int passthrough(int n) {
  int keep = n * 11;        // not used in the loop body, needed after it
  int acc = 0;
  for (int i = 0; i < n; i++) {
    int t = i * i;
    if (t % 5 == 0)
      acc += t;
    else if (t % 3 == 0)
      acc -= t;
    else
      acc += 1;
  }
  return acc + keep;
}

static int nested(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    int row = i * 3;
    for (int j = 0; j < i; j++) {
      int cell = row + j;
      if (cell & 4)
        total += cell;
      else
        total -= j;
    }
    total += row;
  }
  return total;
}

int main(void) {
  int n = vn, k = vk;
  if (many_live(n, k) != 2916) return 1;
  if (mixed(n) != 53223) return 2;
  if (passthrough(n) != -13) return 3;
  if (nested(n) != 4200) return 4;
  return 0;
}
