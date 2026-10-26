// Soft-float ABI shims for libm and the few libc calls that pass doubles in
// registers. Variadic functions (printf family) already use the base ABI.

#include "bridges.h"

#include <math.h>
#include <stdlib.h>

#define D1(n) SOFTFP double n##_sfp(double x) { return n(x); }
#define F1(n) SOFTFP float n##_sfp(float x) { return n(x); }
#define D2(n) SOFTFP double n##_sfp(double x, double y) { return n(x, y); }
#define F2(n) SOFTFP float n##_sfp(float x, float y) { return n(x, y); }

D1(acos) D1(asin) D1(atan) D1(ceil) D1(cos) D1(cosh) D1(exp) D1(floor) D1(log) D1(log10) D1(rint)
D1(round) D1(sin) D1(sinh) D1(sqrt) D1(tan) D1(tanh) D1(trunc)
F1(acosf) F1(asinf) F1(atanf) F1(ceilf) F1(cosf) F1(exp2f) F1(expf) F1(floorf) F1(log10f) F1(logf)
F1(sinf) F1(sqrtf) F1(tanf)
D2(atan2) D2(fmod) D2(pow)
F2(atan2f) F2(fmodf) F2(powf)

SOFTFP double frexp_sfp(double x, int *e) { return frexp(x, e); }
SOFTFP double ldexp_sfp(double x, int e) { return ldexp(x, e); }
SOFTFP double modf_sfp(double x, double *i) { return modf(x, i); }
SOFTFP float modff_sfp(float x, float *i) { return modff(x, i); }
SOFTFP int __fpclassifyf_sfp(float x) {
  // Bionic numbering: FP_INFINITE=1, FP_NAN=2, FP_NORMAL=4, FP_SUBNORMAL=8, FP_ZERO=16
  switch (fpclassify(x)) {
  case FP_INFINITE: return 0x01;
  case FP_NAN: return 0x02;
  case FP_NORMAL: return 0x04;
  case FP_SUBNORMAL: return 0x08;
  default: return 0x10;
  }
}
SOFTFP int isnan_sfp(double x) { return isnan(x); }
SOFTFP double strtod_sfp(const char *s, char **end) { return strtod(s, end); }
