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

/* low level squaring, b = a*a, HAC pp.596-597, Algorithm 14.16 */
int
s_mp_sqr (mp_int * a, mp_int * b)
{
  mp_int    t;
  int       res, ix, iy, pa;
  mp_word   r, u;
  mp_digit  tmpx, *tmpt;

  /* can we use the fast multiplier? */
  if (((a->used * 2 + 1) < 512)
      && a->used <
      (1 << ((CHAR_BIT * sizeof (mp_word)) - (2 * DIGIT_BIT) - 1))) {
    res = fast_s_mp_sqr (a, b);
    return res;
  }

  pa = a->used;
  if ((res = mp_init_size (&t, pa + pa + 1)) != MP_OKAY) {
    return res;
  }
  t.used = pa + pa + 1;

  for (ix = 0; ix < pa; ix++) {
    /* first calculate the digit at 2*ix */
    /* calculate double precision result */
    r =
      ((mp_word) t.dp[ix + ix]) +
      ((mp_word) a->dp[ix]) * ((mp_word) a->dp[ix]);

    /* store lower part in result */
    t.dp[ix + ix] = (mp_digit) (r & ((mp_word) MP_MASK));

    /* get the carry */
    u = (r >> ((mp_word) DIGIT_BIT));

    /* left hand side of A[ix] * A[iy] */
    tmpx = a->dp[ix];

    /* alias for where to store the results */
    tmpt = &(t.dp[ix + ix + 1]);
    for (iy = ix + 1; iy < pa; iy++) {
      /* first calculate the product */
      r = ((mp_word) tmpx) * ((mp_word) a->dp[iy]);

      /* now calculate the double precision result, note we use
       * addition instead of *2 since its easier to optimize
       */
      r = ((mp_word) * tmpt) + r + r + ((mp_word) u);

      /* store lower part */
      *tmpt++ = (mp_digit) (r & ((mp_word) MP_MASK));

      /* get carry */
      u = (r >> ((mp_word) DIGIT_BIT));
    }
    r = ((mp_word) * tmpt) + u;
    *tmpt = (mp_digit) (r & ((mp_word) MP_MASK));
    u = (r >> ((mp_word) DIGIT_BIT));
    /* propagate upwards */
    ++tmpt;
    while (u != ((mp_word) 0)) {
      r = ((mp_word) * tmpt) + ((mp_word) 1);
      *tmpt++ = (mp_digit) (r & ((mp_word) MP_MASK));
      u = (r >> ((mp_word) DIGIT_BIT));
    }
  }

  mp_clamp (&t);
  mp_exch (&t, b);
  mp_clear (&t);
  return MP_OKAY;
}
