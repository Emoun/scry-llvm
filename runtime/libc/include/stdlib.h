#ifndef _SCRY_STDLIB_H
#define _SCRY_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

/* Terminates the simulation.  There is no way to unwind the Scry call stack,
   so exit() records the status in __scry_exit_status and executes a trap. */
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
int atexit(void (*func)(void));
extern volatile int __scry_exit_status;

int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double atof(const char *s);
double strtod(const char *s, char **end);

int rand(void);
void srand(unsigned seed);

void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));

char *getenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _SCRY_STDLIB_H */
