#ifndef _SCRY_ASSERT_H
#define _SCRY_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

void __assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* _SCRY_ASSERT_H */

/* assert() may be redefined on every inclusion, as the standard requires. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e)                                                              \
  ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
