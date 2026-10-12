// Globals, arrays, structs, pointers, unaligned and mixed-width accesses.
// expect-exit: 0
#include <stdint.h>
#include <string.h>

struct point {
  int8_t tag;
  int32_t x;
  int16_t y;
  int64_t big;
};

static struct point pts[4];
static uint8_t bytes[64];
static int table[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const char *msg = "hello";
int global_counter;

static int sum_table(const int *t, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += t[i];
  return s;
}

static void bump(void) { global_counter++; }

int main(void) {
  for (int i = 0; i < 4; i++) {
    pts[i].tag = (int8_t)(i - 2);
    pts[i].x = i * 1000;
    pts[i].y = (int16_t)(-i * 100);
    pts[i].big = (int64_t)i << 40;
  }
  if (pts[3].tag != 1) return 1;
  if (pts[2].x != 2000) return 2;
  if (pts[1].y != -100) return 3;
  if (pts[3].big != (3ll << 40)) return 4;
  if (sum_table(table, 16) != 136) return 5;
  if (sum_table(table + 8, 4) != 42) return 6;
  for (int i = 0; i < 64; i++)
    bytes[i] = (uint8_t)i;
  uint32_t w;
  memcpy(&w, bytes + 1, 4); // unaligned copy
  if (w != 0x04030201u) return 7;
  uint32_t *p = (uint32_t *)(bytes + 3);
  if (*p != 0x06050403u) return 8; // unaligned load
  *p = 0xAABBCCDDu;
  if (bytes[3] != 0xDD || bytes[6] != 0xAA) return 9;
  if (strlen(msg) != 5) return 10;
  if (msg[1] != 'e') return 11;
  struct point *q = &pts[2];
  q->x += q->tag;
  if (pts[2].x != 2000) return 12;
  bump();
  bump();
  if (global_counter != 2) return 13;
  int local[8];
  for (int i = 0; i < 8; i++)
    local[i] = i * i;
  int *lp = local + 2;
  if (lp[3] != 25) return 14;
  if (sum_table(local, 8) != 140) return 15;
  memset(local, 0, sizeof local);
  if (sum_table(local, 8) != 0) return 16;
  return 0;
}
