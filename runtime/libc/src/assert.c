/* assert() support for Scry. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func) {
  printf("%s:%u: %s: Assertion `%s' failed.\n", file, line, func, expr);
  abort();
}
