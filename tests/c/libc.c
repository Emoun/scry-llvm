// Library functions: string.h, stdlib.h, stdio.h formatting.
// expect-exit: 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

int main(void) {
  char buf[64];
  if (strcmp("abc", "abd") >= 0) return 1;
  if (strncmp("abcdef", "abcxyz", 3) != 0) return 2;
  strcpy(buf, "hello");
  strcat(buf, " world");
  if (strlen(buf) != 11) return 3;
  if (strchr(buf, 'w') != buf + 6) return 4;
  if (strstr(buf, "lo w") != buf + 3) return 5;
  if (memcmp(buf, "hello", 5) != 0) return 6;
  memmove(buf + 2, buf, 5);
  if (memcmp(buf, "hehello", 7) != 0) return 7;
  if (atoi("-1234") != -1234) return 8;
  if (strtoul("0xFF", 0, 0) != 255) return 9;
  if (abs(-9) != 9) return 10;
  int arr[8] = {5, 3, 9, 1, 7, 2, 8, 4};
  qsort(arr, 8, sizeof(int), cmp_int);
  static const int sorted[8] = {1, 2, 3, 4, 5, 7, 8, 9};
  for (int i = 0; i < 8; i++)
    if (arr[i] != sorted[i])
      return 11;
  int key = 7;
  int *found = bsearch(&key, arr, 8, sizeof(int), cmp_int);
  if (!found || *found != 7) return 12;
  int *m = malloc(10 * sizeof(int));
  if (!m) return 13;
  for (int i = 0; i < 10; i++)
    m[i] = i;
  m = realloc(m, 20 * sizeof(int));
  if (!m || m[9] != 9) return 14;
  free(m);
  snprintf(buf, sizeof buf, "%d|%u|%x|%s|%c|%05d|%-4d|%lld|%%", -42, 42u, 255,
           "str", 'z', 7, 3, -1234567890123LL);
  if (strcmp(buf, "-42|42|ff|str|z|00007|3   |-1234567890123|%") != 0)
    return 15;
  printf("printf works: %d\n", 1);
  if (__scry_console_len != 16) return 16;
  srand(1);
  int r1 = rand(), r2 = rand();
  if (r1 == r2) return 17;
  return 0;
}
