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
 * Tom St Denis, tomstdenis@iahu.ca, http://libtommath.iahu.ca
 */
#include <tommath.h>

/* returns the number of bits in an int */
int
mp_count_bits (mp_int * a)
{
  int       r;
  mp_digit  q;

  if (a->used == 0) {
    return 0;
  }

  r = (a->used - 1) * DIGIT_BIT;
  q = a->dp[a->used - 1];
  while (q > ((mp_digit) 0)) {
    ++r;
    q >>= ((mp_digit) 1);
  }
  return r;
}
