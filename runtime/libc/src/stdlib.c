/* Minimal stdlib.h implementation for Scry. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int errno;
volatile int __scry_exit_status;

/* ------------------------------------------------------------------------ */
/* Process termination                                                      */
/* ------------------------------------------------------------------------ */

void __libc_init(void) {}

void abort(void) {
  __scry_exit_status = 134;
  for (;;)
    __builtin_trap();
}

void _Exit(int status) {
  __scry_exit_status = status;
  for (;;)
    __builtin_trap();
}

void exit(int status) { _Exit(status); }

int atexit(void (*func)(void)) {
  (void)func;
  return 0;
}

char *getenv(const char *name) {
  (void)name;
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Heap: a bump allocator with a size header so realloc can copy.           */
/* ------------------------------------------------------------------------ */

#ifndef SCRY_HEAP_SIZE
#define SCRY_HEAP_SIZE (64 * 1024)
#endif

static unsigned char __scry_heap[SCRY_HEAP_SIZE] __attribute__((aligned(16)));
static size_t __scry_heap_top;

void *malloc(size_t size) {
  size_t total = (size + 16 + 15) & ~(size_t)15; /* header + payload */
  if (__scry_heap_top + total > SCRY_HEAP_SIZE) {
    errno = ENOMEM;
    return NULL;
  }
  unsigned char *block = __scry_heap + __scry_heap_top;
  __scry_heap_top += total;
  *(size_t *)block = size;
  return block + 16;
}

void free(void *ptr) {
  (void)ptr; /* bump allocator: memory is reclaimed at exit only */
}

void *calloc(size_t nmemb, size_t size) {
  size_t n = nmemb * size;
  void *p = malloc(n);
  if (p)
    memset(p, 0, n);
  return p;
}

void *realloc(void *ptr, size_t size) {
  if (!ptr)
    return malloc(size);
  size_t old = *(size_t *)((unsigned char *)ptr - 16);
  if (size <= old)
    return ptr;
  void *p = malloc(size);
  if (p)
    memcpy(p, ptr, old);
  return p;
}

/* ------------------------------------------------------------------------ */
/* Integer helpers                                                          */
/* ------------------------------------------------------------------------ */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

div_t div(int num, int den) {
  div_t r = {num / den, num % den};
  return r;
}
ldiv_t ldiv(long num, long den) {
  ldiv_t r = {num / den, num % den};
  return r;
}
lldiv_t lldiv(long long num, long long den) {
  lldiv_t r = {num / den, num % den};
  return r;
}

static int digit_value(int c, int base) {
  int v;
  if (c >= '0' && c <= '9')
    v = c - '0';
  else if (c >= 'a' && c <= 'z')
    v = c - 'a' + 10;
  else if (c >= 'A' && c <= 'Z')
    v = c - 'A' + 10;
  else
    return -1;
  return v < base ? v : -1;
}

unsigned long long strtoull(const char *s, char **end, int base) {
  const char *p = s;
  while (isspace((unsigned char)*p))
    p++;
  int neg = 0;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
      digit_value(p[2], 16) >= 0) {
    p += 2;
    base = 16;
  } else if (base == 0 && p[0] == '0') {
    base = 8;
  } else if (base == 0) {
    base = 10;
  }
  unsigned long long v = 0;
  int any = 0, d;
  while ((d = digit_value(*p, base)) >= 0) {
    v = v * (unsigned)base + (unsigned)d;
    any = 1;
    p++;
  }
  if (end)
    *end = (char *)(any ? p : s);
  return neg ? (unsigned long long)0 - v : v;
}

long long strtoll(const char *s, char **end, int base) {
  return (long long)strtoull(s, end, base);
}
unsigned long strtoul(const char *s, char **end, int base) {
  return (unsigned long)strtoull(s, end, base);
}
long strtol(const char *s, char **end, int base) {
  return (long)strtoull(s, end, base);
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

double strtod(const char *s, char **end) {
  const char *p = s;
  while (isspace((unsigned char)*p))
    p++;
  int neg = 0;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  double v = 0;
  int any = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    p++;
    any = 1;
  }
  if (*p == '.') {
    p++;
    double scale = 0.1;
    while (*p >= '0' && *p <= '9') {
      v += (*p - '0') * scale;
      scale *= 0.1;
      p++;
      any = 1;
    }
  }
  if (any && (*p == 'e' || *p == 'E')) {
    const char *q = p + 1;
    int eneg = 0;
    if (*q == '+' || *q == '-')
      eneg = *q++ == '-';
    if (*q >= '0' && *q <= '9') {
      int e = 0;
      while (*q >= '0' && *q <= '9')
        e = e * 10 + (*q++ - '0');
      while (e--)
        v = eneg ? v / 10 : v * 10;
      p = q;
    }
  }
  if (end)
    *end = (char *)(any ? p : s);
  return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, NULL); }

/* ------------------------------------------------------------------------ */
/* Pseudo-random numbers (glibc-compatible TYPE_0 generator)                */
/* ------------------------------------------------------------------------ */

static unsigned __scry_rand_state = 1;

int rand(void) {
  __scry_rand_state = __scry_rand_state * 1103515245u + 12345u;
  return (int)((__scry_rand_state >> 1) & 0x7fffffff);
}

void srand(unsigned seed) { __scry_rand_state = seed; }

/* ------------------------------------------------------------------------ */
/* Sorting and searching                                                    */
/* ------------------------------------------------------------------------ */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n) {
  while (n--) {
    unsigned char t = *a;
    *a++ = *b;
    *b++ = t;
  }
}

/* Heap sort: O(n log n), in place, no recursion (the simulator stack is
   small). */
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
  unsigned char *b = (unsigned char *)base;
  if (n < 2)
    return;
  for (size_t start = n / 2; start-- > 0;) {
    size_t root = start;
    for (;;) {
      size_t child = 2 * root + 1;
      if (child >= n)
        break;
      if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size) < 0)
        child++;
      if (cmp(b + root * size, b + child * size) >= 0)
        break;
      swap_bytes(b + root * size, b + child * size, size);
      root = child;
    }
  }
  for (size_t end = n - 1; end > 0; end--) {
    swap_bytes(b, b + end * size, size);
    size_t root = 0;
    for (;;) {
      size_t child = 2 * root + 1;
      if (child >= end)
        break;
      if (child + 1 < end && cmp(b + child * size, b + (child + 1) * size) < 0)
        child++;
      if (cmp(b + root * size, b + child * size) >= 0)
        break;
      swap_bytes(b + root * size, b + child * size, size);
      root = child;
    }
  }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
  const unsigned char *b = (const unsigned char *)base;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = cmp(key, b + mid * size);
    if (c == 0)
      return (void *)(b + mid * size);
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}
