/* LibTomMath, multiple-precision integer library -- Tom St Denis
 *
 * LibTomMath is library that provides for multiple-precision
 * integer arithmetic as well as number theoretic functionality.
 *
 * The library is designed directly after the MPI library by
 * Michael Fromberger but has been written from scratch with
 * additional optimizations in place.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 *
 * Tom St Denis, tomstdenis@iahu.ca, http://math.libtomcrypt.org
 */
#include <tommath.h>

/* setups the montgomery reduction stuff */
int
mp_montgomery_setup (mp_int * a, mp_digit * mp)
{
  unsigned long x, b;

/* fast inversion mod 2^32 
 *
 * Based on the fact that 
 *
 * XA = 1 (mod 2^n)  =>  (X(2-XA)) A = 1 (mod 2^2n)
 *                   =>  2*X*A - X*X*A*A = 1
 *                   =>  2*(1) - (1)     = 1
 */
  b = a->dp[0];

  if ((b & 1) == 0) {
    return MP_VAL;
  }

  x = (((b + 2) & 4) << 1) + b;	/* here x*a==1 mod 2^4 */
  x *= 2 - b * x;		/* here x*a==1 mod 2^8 */
  x *= 2 - b * x;		/* here x*a==1 mod 2^16; each step doubles the nb of bits */
  x *= 2 - b * x;		/* here x*a==1 mod 2^32 */

  /* t = -1/m mod b */
  *mp = ((mp_digit) 1 << ((mp_digit) DIGIT_BIT)) - (x & MP_MASK);

  return MP_OKAY;
}
