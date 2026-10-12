/* Minimal stdio.h implementation for Scry.
 *
 * Output goes to the in-memory console ring buffer; input always hits EOF.
 * The formatter supports %d %i %u %x %X %o %c %s %p %% with the h/hh/l/ll/z
 * length modifiers, '-', '0', width and precision.  Floating point
 * conversions (%f %e %g) are rendered with a simple fixed-point routine.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

char __scry_console[SCRY_CONSOLE_SIZE];
unsigned __scry_console_len;

struct __scry_FILE {
  int id;
};
static FILE __scry_files[3] = {{0}, {1}, {2}};
FILE *stdin = &__scry_files[0];
FILE *stdout = &__scry_files[1];
FILE *stderr = &__scry_files[2];

static void console_put(char c) {
  __scry_console[__scry_console_len % SCRY_CONSOLE_SIZE] = c;
  __scry_console_len++;
}

int putchar(int c) {
  console_put((char)c);
  return (unsigned char)c;
}

int fputc(int c, FILE *f) {
  (void)f;
  return putchar(c);
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
  (void)f;
  while (*s)
    console_put(*s++);
  return 0;
}

int puts(const char *s) {
  fputs(s, stdout);
  console_put('\n');
  return 0;
}

int fflush(FILE *f) {
  (void)f;
  return 0;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f) {
  (void)f;
  const char *c = (const char *)p;
  size_t n = size * nmemb;
  for (size_t i = 0; i < n; i++)
    console_put(c[i]);
  return nmemb;
}

int getchar(void) { return EOF; }
int fgetc(FILE *f) {
  (void)f;
  return EOF;
}
char *fgets(char *s, int n, FILE *f) {
  (void)s;
  (void)n;
  (void)f;
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

struct out {
  char *buf;    /* NULL: console */
  size_t cap;   /* capacity of buf including the terminator */
  size_t len;   /* characters produced so far */
};

static void out_char(struct out *o, char c) {
  if (o->buf) {
    if (o->len + 1 < o->cap)
      o->buf[o->len] = c;
  } else {
    console_put(c);
  }
  o->len++;
}

static void out_pad(struct out *o, int n, char c) {
  while (n-- > 0)
    out_char(o, c);
}

static void out_field(struct out *o, const char *digits, int ndigits,
                      const char *prefix, int width, int precision,
                      int left, int zero) {
  int plen = (int)strlen(prefix);
  int zeros = precision > ndigits ? precision - ndigits : 0;
  int total = plen + zeros + ndigits;
  if (!left && !zero)
    out_pad(o, width - total, ' ');
  for (int i = 0; i < plen; i++)
    out_char(o, prefix[i]);
  if (!left && zero && precision < 0)
    out_pad(o, width - total, '0');
  out_pad(o, zeros, '0');
  for (int i = 0; i < ndigits; i++)
    out_char(o, digits[i]);
  if (left)
    out_pad(o, width - total, ' ');
}

static int fmt_unsigned(char *tmp, unsigned long long v, unsigned base,
                        int upper) {
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char rev[32];
  int n = 0;
  do {
    rev[n++] = dig[v % base];
    v /= base;
  } while (v);
  for (int i = 0; i < n; i++)
    tmp[i] = rev[n - 1 - i];
  return n;
}

static int fmt_double(char *tmp, double v, int precision) {
  int n = 0;
  if (v != v) {
    memcpy(tmp, "nan", 3);
    return 3;
  }
  if (v > 1e18 || v < -1e18) {
    memcpy(tmp, "inf", 3);
    return 3;
  }
  if (precision < 0)
    precision = 6;
  if (v < 0) {
    tmp[n++] = '-';
    v = -v;
  }
  unsigned long long ip = (unsigned long long)v;
  double frac = v - (double)ip;
  n += fmt_unsigned(tmp + n, ip, 10, 0);
  if (precision > 0) {
    tmp[n++] = '.';
    for (int i = 0; i < precision && n < 60; i++) {
      frac *= 10;
      int d = (int)frac;
      tmp[n++] = (char)('0' + d);
      frac -= d;
    }
  }
  return n;
}

static void vformat(struct out *o, const char *fmt, va_list ap) {
  char tmp[64];
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      out_char(o, *fmt);
      continue;
    }
    fmt++;
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
    for (;; fmt++) {
      if (*fmt == '-')
        left = 1;
      else if (*fmt == '0')
        zero = 1;
      else if (*fmt == '+')
        plus = 1;
      else if (*fmt == ' ')
        space = 1;
      else if (*fmt == '#')
        alt = 1;
      else
        break;
    }
    int width = 0;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        left = 1;
        width = -width;
      }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
    }
    int precision = -1;
    if (*fmt == '.') {
      fmt++;
      precision = 0;
      if (*fmt == '*') {
        precision = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9')
          precision = precision * 10 + (*fmt++ - '0');
      }
    }
    int lmod = 0; /* 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t */
    for (;; fmt++) {
      if (*fmt == 'l')
        lmod = lmod == 1 ? 2 : 1;
      else if (*fmt == 'h')
        lmod = lmod == -1 ? -2 : -1;
      else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j')
        lmod = *fmt == 'j' ? 2 : 3;
      else
        break;
    }
    char conv = *fmt;
    if (!conv)
      break;
    switch (conv) {
    case '%':
      out_char(o, '%');
      break;
    case 'c':
      out_pad(o, left ? 0 : width - 1, ' ');
      out_char(o, (char)va_arg(ap, int));
      out_pad(o, left ? width - 1 : 0, ' ');
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int n = (int)strlen(s);
      if (precision >= 0 && n > precision)
        n = precision;
      out_field(o, s, n, "", width, -1, left, 0);
      break;
    }
    case 'd':
    case 'i': {
      long long v;
      if (lmod == 2)
        v = va_arg(ap, long long);
      else if (lmod == 1)
        v = va_arg(ap, long);
      else if (lmod == 3)
        v = (long long)va_arg(ap, ptrdiff_t);
      else
        v = va_arg(ap, int);
      if (lmod == -1)
        v = (short)v;
      else if (lmod == -2)
        v = (signed char)v;
      const char *prefix = "";
      unsigned long long u;
      if (v < 0) {
        prefix = "-";
        u = (unsigned long long)0 - (unsigned long long)v;
      } else {
        u = (unsigned long long)v;
        if (plus)
          prefix = "+";
        else if (space)
          prefix = " ";
      }
      int n = fmt_unsigned(tmp, u, 10, 0);
      out_field(o, tmp, n, prefix, width, precision, left, zero);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o':
    case 'p': {
      unsigned long long v;
      if (conv == 'p')
        v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
      else if (lmod == 2)
        v = va_arg(ap, unsigned long long);
      else if (lmod == 1)
        v = va_arg(ap, unsigned long);
      else if (lmod == 3)
        v = va_arg(ap, size_t);
      else
        v = va_arg(ap, unsigned);
      if (lmod == -1)
        v = (unsigned short)v;
      else if (lmod == -2)
        v = (unsigned char)v;
      unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
      const char *prefix = "";
      if (conv == 'p')
        prefix = "0x";
      else if (alt && v != 0)
        prefix = conv == 'x' ? "0x" : conv == 'X' ? "0X" : conv == 'o' ? "0" : "";
      int n = fmt_unsigned(tmp, v, base, conv == 'X');
      out_field(o, tmp, n, prefix, width, precision, left, zero);
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
      double v = va_arg(ap, double);
      int n = fmt_double(tmp, v, precision);
      out_field(o, tmp, n, "", width, -1, left, zero);
      break;
    }
    default:
      out_char(o, '%');
      out_char(o, conv);
      break;
    }
  }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
  struct out o = {buf, n, 0};
  vformat(&o, fmt, ap);
  if (buf && n)
    buf[o.len < n ? o.len : n - 1] = 0;
  return (int)o.len;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
  return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

int sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, (size_t)-1, fmt, ap);
  va_end(ap);
  return r;
}

int vprintf(const char *fmt, va_list ap) {
  struct out o = {NULL, 0, 0};
  vformat(&o, fmt, ap);
  return (int)o.len;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vprintf(fmt, ap);
  va_end(ap);
  return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  (void)f;
  return vprintf(fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}
