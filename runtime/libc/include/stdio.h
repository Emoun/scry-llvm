#ifndef _SCRY_STDIO_H
#define _SCRY_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The simulator has no I/O.  Everything written to stdout/stderr is appended
   to the in-memory console buffer __scry_console (a ring buffer of
   SCRY_CONSOLE_SIZE bytes), which can be inspected in a memory dump. */
#define SCRY_CONSOLE_SIZE 4096
extern char __scry_console[SCRY_CONSOLE_SIZE];
extern unsigned __scry_console_len; /* total bytes ever written */

#define EOF (-1)
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct __scry_FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int putchar(int c);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fflush(FILE *f);
size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f);
int getchar(void);
int fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* _SCRY_STDIO_H */
