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

/* init a new bigint */
int
mp_init (mp_int * a)
{

  /* allocate ram required and clear it */
  a->dp = calloc (sizeof (mp_digit), MP_PREC);
  if (a->dp == NULL) {
    return MP_MEM;
  }

  /* set the used to zero, allocated digit to the default precision
   * and sign to positive */
  a->used = 0;
  a->alloc = MP_PREC;
  a->sign = MP_ZPOS;

  return MP_OKAY;
}
