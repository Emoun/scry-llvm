// A small "benchmark-like" program: sorting and checksumming.
// expect-exit: 0
#include <stdint.h>

#define N 200
static uint32_t data[N];

static uint32_t lcg(uint32_t *s) {
  *s = *s * 1664525u + 1013904223u;
  return *s;
}

static void insertion_sort(uint32_t *a, int n) {
  for (int i = 1; i < n; i++) {
    uint32_t v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) {
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = v;
  }
}

static uint32_t crc32(const uint8_t *p, int n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (int i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

int main(void) {
  uint32_t seed = 12345;
  for (int i = 0; i < N; i++)
    data[i] = lcg(&seed) % 1000;
  insertion_sort(data, N);
  for (int i = 1; i < N; i++)
    if (data[i - 1] > data[i])
      return 1;
  uint32_t sum = 0;
  for (int i = 0; i < N; i++)
    sum += data[i];
  if (sum != 105684) return 2;
  if (crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43) !=
      0x414FA339u)
    return 3;
  return 0;
}
