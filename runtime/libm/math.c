/* A small libm for Scry.
 *
 * Scry has no floating-point hardware; all arithmetic here is performed by
 * the compiler-rt soft-float routines.  Accuracy is adequate for benchmarks
 * and tests, not for a numerics library.
 */
#include <math.h>
#include <stdint.h>

double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }

double trunc(double x) {
  if (x != x || x > 9.2e18 || x < -9.2e18)
    return x;
  long long i = (long long)x;
  return (double)i;
}

double floor(double x) {
  double t = trunc(x);
  return (t > x) ? t - 1.0 : t;
}
float floorf(float x) { return (float)floor(x); }

double ceil(double x) {
  double t = trunc(x);
  return (t < x) ? t + 1.0 : t;
}
float ceilf(float x) { return (float)ceil(x); }

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

double fmod(double x, double y) {
  if (y == 0.0 || x != x || y != y)
    return NAN;
  double q = trunc(x / y);
  return x - q * y;
}

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }

double modf(double x, double *ip) {
  *ip = trunc(x);
  return x - *ip;
}

double ldexp(double x, int e) {
  while (e > 0) {
    x *= 2.0;
    e--;
  }
  while (e < 0) {
    x *= 0.5;
    e++;
  }
  return x;
}

double frexp(double x, int *e) {
  *e = 0;
  if (x == 0.0 || x != x)
    return x;
  double a = fabs(x);
  while (a >= 1.0) {
    a *= 0.5;
    (*e)++;
  }
  while (a < 0.5) {
    a *= 2.0;
    (*e)--;
  }
  return x < 0 ? -a : a;
}

double sqrt(double x) {
  if (x < 0)
    return NAN;
  if (x == 0.0 || x != x)
    return x;
  /* Newton-Raphson from a frexp-based estimate. */
  int e;
  double m = frexp(x, &e);
  double y = ldexp(m, (e + 1) / 2);
  for (int i = 0; i < 30; i++)
    y = 0.5 * (y + x / y);
  return y;
}
float sqrtf(float x) { return (float)sqrt(x); }

double cbrt(double x) {
  if (x == 0.0)
    return 0.0;
  double a = fabs(x);
  int e;
  double m = frexp(a, &e);
  double y = ldexp(m, e / 3 + 1);
  for (int i = 0; i < 40; i++)
    y = (2.0 * y + a / (y * y)) / 3.0;
  return x < 0 ? -y : y;
}

/* Reduce to [-pi, pi] and evaluate Taylor series. */
static double sin_core(double x) {
  double x2 = x * x;
  double term = x, sum = x;
  for (int n = 1; n < 12; n++) {
    term *= -x2 / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

static double cos_core(double x) {
  double x2 = x * x;
  double term = 1.0, sum = 1.0;
  for (int n = 1; n < 12; n++) {
    term *= -x2 / ((2.0 * n - 1.0) * (2.0 * n));
    sum += term;
  }
  return sum;
}

static double reduce_2pi(double x) {
  double k = floor(x / (2.0 * M_PI) + 0.5);
  return x - k * 2.0 * M_PI;
}

double sin(double x) { return sin_core(reduce_2pi(x)); }
double cos(double x) { return cos_core(reduce_2pi(x)); }
double tan(double x) { return sin(x) / cos(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }

double atan(double x) {
  /* Argument reduction: atan(x) = pi/2 - atan(1/x) for |x| > 1, then
     atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) to shrink the argument. */
  int invert = 0, neg = 0;
  if (x < 0) {
    neg = 1;
    x = -x;
  }
  if (x > 1.0) {
    invert = 1;
    x = 1.0 / x;
  }
  int halvings = 0;
  while (x > 0.25) {
    x = x / (1.0 + sqrt(1.0 + x * x));
    halvings++;
  }
  double x2 = x * x, term = x, sum = x;
  for (int n = 1; n < 20; n++) {
    term *= -x2;
    sum += term / (2.0 * n + 1.0);
  }
  while (halvings--)
    sum *= 2.0;
  if (invert)
    sum = M_PI_2 - sum;
  return neg ? -sum : sum;
}

double atan2(double y, double x) {
  if (x > 0)
    return atan(y / x);
  if (x < 0)
    return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
  if (y > 0)
    return M_PI_2;
  if (y < 0)
    return -M_PI_2;
  return 0.0;
}

double exp(double x) {
  if (x != x)
    return x;
  if (x > 709.0)
    return INFINITY;
  if (x < -745.0)
    return 0.0;
  /* exp(x) = 2^k * exp(r), r = x - k ln2 in [-ln2/2, ln2/2]. */
  double k = floor(x / M_LN2 + 0.5);
  double r = x - k * M_LN2;
  double term = 1.0, sum = 1.0;
  for (int n = 1; n < 20; n++) {
    term *= r / n;
    sum += term;
  }
  return ldexp(sum, (int)k);
}
float expf(float x) { return (float)exp(x); }

double log(double x) {
  if (x < 0 || x != x)
    return NAN;
  if (x == 0.0)
    return -INFINITY;
  int e;
  double m = frexp(x, &e); /* x = m * 2^e, m in [0.5, 1) */
  m *= 2.0;
  e -= 1; /* m in [1, 2) */
  /* log(m) = 2 atanh((m-1)/(m+1)) */
  double t = (m - 1.0) / (m + 1.0), t2 = t * t, term = t, sum = t;
  for (int n = 1; n < 30; n++) {
    term *= t2;
    sum += term / (2.0 * n + 1.0);
  }
  return 2.0 * sum + e * M_LN2;
}
float logf(float x) { return (float)log(x); }
double log2(double x) { return log(x) / M_LN2; }
double log10(double x) { return log(x) / 2.30258509299404568402; }

double pow(double x, double y) {
  if (y == 0.0)
    return 1.0;
  if (x == 0.0)
    return y > 0 ? 0.0 : INFINITY;
  if (y == trunc(y) && fabs(y) < 1e9) {
    long long n = (long long)y;
    int neg = n < 0;
    if (neg)
      n = -n;
    double r = 1.0, b = x;
    while (n) {
      if (n & 1)
        r *= b;
      b *= b;
      n >>= 1;
    }
    return neg ? 1.0 / r : r;
  }
  if (x < 0)
    return NAN;
  return exp(y * log(x));
}
float powf(float x, float y) { return (float)pow(x, y); }
