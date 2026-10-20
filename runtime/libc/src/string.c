/* Minimal string.h implementation for Scry. */
#include <stddef.h>
#include <string.h>

/* The copy and fill routines work eight bytes at a time once the pointers
   are aligned (the simulator requires an access to be aligned to its size),
   four bytes when only that alignment is shared, and unroll the word loops
   by four: on Scry a loop iteration costs its pads, deliveries, induction
   update, compare and jump on top of the body, so a byte loop is about ten
   instructions per byte and a plain word loop about five. */
typedef unsigned long long dword_t;
typedef unsigned int word_t;
#define ALIGNED(p, a) (((size_t)(p) & ((a) - 1)) == 0)
#define MISALIGN(p, a) ((size_t)(p) & ((a) - 1))

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (n >= 4 * sizeof(dword_t) && MISALIGN(d, sizeof(dword_t)) == MISALIGN(s, sizeof(dword_t))) {
    while (!ALIGNED(d, sizeof(dword_t))) {
      *d++ = *s++;
      n--;
    }
    dword_t *dw = (dword_t *)d;
    const dword_t *sw = (const dword_t *)s;
    while (n >= 4 * sizeof(dword_t)) {
      dw[0] = sw[0];
      dw[1] = sw[1];
      dw[2] = sw[2];
      dw[3] = sw[3];
      dw += 4;
      sw += 4;
      n -= 4 * sizeof(dword_t);
    }
    while (n >= sizeof(dword_t)) {
      *dw++ = *sw++;
      n -= sizeof(dword_t);
    }
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  } else if (n >= 2 * sizeof(word_t) &&
             MISALIGN(d, sizeof(word_t)) == MISALIGN(s, sizeof(word_t))) {
    while (!ALIGNED(d, sizeof(word_t))) {
      *d++ = *s++;
      n--;
    }
    word_t *dw = (word_t *)d;
    const word_t *sw = (const word_t *)s;
    while (n >= 4 * sizeof(word_t)) {
      dw[0] = sw[0];
      dw[1] = sw[1];
      dw[2] = sw[2];
      dw[3] = sw[3];
      dw += 4;
      sw += 4;
      n -= 4 * sizeof(word_t);
    }
    while (n >= sizeof(word_t)) {
      *dw++ = *sw++;
      n -= sizeof(word_t);
    }
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  }
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d == s || n == 0)
    return dst;
  if (d < s || d >= s + n)
    return memcpy(dst, src, n);
  d += n;
  s += n;
  if (n >= 4 * sizeof(dword_t) && MISALIGN(d, sizeof(dword_t)) == MISALIGN(s, sizeof(dword_t))) {
    while (!ALIGNED(d, sizeof(dword_t))) {
      *--d = *--s;
      n--;
    }
    dword_t *dw = (dword_t *)d;
    const dword_t *sw = (const dword_t *)s;
    while (n >= 4 * sizeof(dword_t)) {
      dw -= 4;
      sw -= 4;
      dw[3] = sw[3];
      dw[2] = sw[2];
      dw[1] = sw[1];
      dw[0] = sw[0];
      n -= 4 * sizeof(dword_t);
    }
    while (n >= sizeof(dword_t)) {
      *--dw = *--sw;
      n -= sizeof(dword_t);
    }
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  } else if (n >= 2 * sizeof(word_t) &&
             MISALIGN(d, sizeof(word_t)) == MISALIGN(s, sizeof(word_t))) {
    while (!ALIGNED(d, sizeof(word_t))) {
      *--d = *--s;
      n--;
    }
    word_t *dw = (word_t *)d;
    const word_t *sw = (const word_t *)s;
    while (n >= sizeof(word_t)) {
      *--dw = *--sw;
      n -= sizeof(word_t);
    }
    d = (unsigned char *)dw;
    s = (const unsigned char *)sw;
  }
  while (n--)
    *--d = *--s;
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  unsigned char b = (unsigned char)c;
  if (n >= 2 * sizeof(dword_t)) {
    while (!ALIGNED(d, sizeof(dword_t))) {
      *d++ = b;
      n--;
    }
    word_t w = b;
    w |= w << 8;
    w |= w << 16;
    dword_t ww = ((dword_t)w << 32) | w;
    dword_t *dw = (dword_t *)d;
    while (n >= 4 * sizeof(dword_t)) {
      dw[0] = ww;
      dw[1] = ww;
      dw[2] = ww;
      dw[3] = ww;
      dw += 4;
      n -= 4 * sizeof(dword_t);
    }
    while (n >= sizeof(dword_t)) {
      *dw++ = ww;
      n -= sizeof(dword_t);
    }
    d = (unsigned char *)dw;
  }
  while (n--)
    *d++ = b;
  return dst;
}

#define WORD_ALIGNED(p) ALIGNED(p, sizeof(word_t))

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  if (n >= 2 * sizeof(word_t) && WORD_ALIGNED(x) && WORD_ALIGNED(y)) {
    const word_t *xw = (const word_t *)x;
    const word_t *yw = (const word_t *)y;
    while (n >= sizeof(word_t) && *xw == *yw) {
      xw++;
      yw++;
      n -= sizeof(word_t);
    }
    x = (const unsigned char *)xw;
    y = (const unsigned char *)yw;
  }
  for (; n; n--, x++, y++)
    if (*x != *y)
      return (int)*x - (int)*y;
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  for (; n; n--, p++)
    if (*p == (unsigned char)c)
      return (void *)p;
  return NULL;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

size_t strnlen(const char *s, size_t maxlen) {
  size_t n = 0;
  while (n < maxlen && s[n])
    n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b)
      return (int)(unsigned char)*a - (int)(unsigned char)*b;
    if (!*a)
      return 0;
  }
  return 0;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++))
    ;
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++)
    dst[i] = src[i];
  for (; i < n; i++)
    dst[i] = 0;
  return dst;
}

char *strcat(char *dst, const char *src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
  char *d = dst + strlen(dst);
  while (n-- && *src)
    *d++ = *src++;
  *d = 0;
  return dst;
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return NULL;
  }
}

char *strrchr(const char *s, int c) {
  const char *last = NULL;
  for (;; s++) {
    if (*s == (char)c)
      last = s;
    if (!*s)
      return (char *)last;
  }
}

char *strstr(const char *h, const char *n) {
  size_t nl = strlen(n);
  if (!nl)
    return (char *)h;
  for (; *h; h++)
    if (strncmp(h, n, nl) == 0)
      return (char *)h;
  return NULL;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  for (; s[n] && strchr(accept, s[n]); n++)
    ;
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  for (; s[n] && !strchr(reject, s[n]); n++)
    ;
  return n;
}

char *strtok(char *s, const char *delim) {
  static char *save;
  if (!s)
    s = save;
  if (!s)
    return NULL;
  s += strspn(s, delim);
  if (!*s) {
    save = NULL;
    return NULL;
  }
  char *tok = s;
  s += strcspn(s, delim);
  if (*s) {
    *s = 0;
    save = s + 1;
  } else {
    save = NULL;
  }
  return tok;
}

extern void *malloc(size_t);

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = (char *)malloc(n);
  if (d)
    memcpy(d, s, n);
  return d;
}
