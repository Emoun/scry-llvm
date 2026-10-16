/* Minimal string.h implementation for Scry. */
#include <stddef.h>
#include <string.h>

/* The copy and fill routines work a word at a time once the pointers are
   aligned: a byte loop costs about ten instructions per byte on Scry. */
typedef unsigned int word_t;
#define WORD_ALIGNED(p) (((size_t)(p) & (sizeof(word_t) - 1)) == 0)

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (n >= 2 * sizeof(word_t) &&
      ((size_t)d & (sizeof(word_t) - 1)) == ((size_t)s & (sizeof(word_t) - 1))) {
    while (!WORD_ALIGNED(d)) {
      *d++ = *s++;
      n--;
    }
    word_t *dw = (word_t *)d;
    const word_t *sw = (const word_t *)s;
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
  if (n >= 2 * sizeof(word_t) &&
      ((size_t)d & (sizeof(word_t) - 1)) == ((size_t)s & (sizeof(word_t) - 1))) {
    while (!WORD_ALIGNED(d)) {
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
  if (n >= 2 * sizeof(word_t)) {
    while (!WORD_ALIGNED(d)) {
      *d++ = b;
      n--;
    }
    word_t w = b;
    w |= w << 8;
    w |= w << 16;
    word_t *dw = (word_t *)d;
    while (n >= sizeof(word_t)) {
      *dw++ = w;
      n -= sizeof(word_t);
    }
    d = (unsigned char *)dw;
  }
  while (n--)
    *d++ = b;
  return dst;
}

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
