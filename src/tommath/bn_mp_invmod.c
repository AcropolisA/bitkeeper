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

int
mp_invmod (mp_int * a, mp_int * b, mp_int * c)
{
  mp_int  x, y, u, v, A, B, C, D;
  int     res;

  /* b cannot be negative */
  if (b->sign == MP_NEG) {
    return MP_VAL;
  }

  /* if the modulus is odd we can use a faster routine instead */
  if (mp_iseven (b) == 0) {
    return fast_mp_invmod (a, b, c);
  }

  if ((res = mp_init (&x)) != MP_OKAY) {
    goto __ERR;
  }

  if ((res = mp_init (&y)) != MP_OKAY) {
    goto __X;
  }

  if ((res = mp_init (&u)) != MP_OKAY) {
    goto __Y;
  }

  if ((res = mp_init (&v)) != MP_OKAY) {
    goto __U;
  }

  if ((res = mp_init (&A)) != MP_OKAY) {
    goto __V;
  }

  if ((res = mp_init (&B)) != MP_OKAY) {
    goto __A;
  }

  if ((res = mp_init (&C)) != MP_OKAY) {
    goto __B;
  }

  if ((res = mp_init (&D)) != MP_OKAY) {
    goto __C;
  }

  /* x = a, y = b */
  if ((res = mp_copy (a, &x)) != MP_OKAY) {
    goto __D;
  }
  if ((res = mp_copy (b, &y)) != MP_OKAY) {
    goto __D;
  }

  if ((res = mp_abs (&x, &x)) != MP_OKAY) {
    goto __D;
  }

  /* 2. [modified] if x,y are both even then return an error! */
  if (mp_iseven (&x) == 1 && mp_iseven (&y) == 1) {
    res = MP_VAL;
    goto __D;
  }

  /* 3. u=x, v=y, A=1, B=0, C=0,D=1 */
  if ((res = mp_copy (&x, &u)) != MP_OKAY) {
    goto __D;
  }
  if ((res = mp_copy (&y, &v)) != MP_OKAY) {
    goto __D;
  }
  mp_set (&A, 1);
  mp_set (&D, 1);


top:
  /* 4.  while u is even do */
  while (mp_iseven (&u) == 1) {
    /* 4.1 u = u/2 */
    if ((res = mp_div_2 (&u, &u)) != MP_OKAY) {
      goto __D;
    }
    /* 4.2 if A or B is odd then */
    if (mp_iseven (&A) == 0 || mp_iseven (&B) == 0) {
      /* A = (A+y)/2, B = (B-x)/2 */
      if ((res = mp_add (&A, &y, &A)) != MP_OKAY) {
	goto __D;
      }
      if ((res = mp_sub (&B, &x, &B)) != MP_OKAY) {
	goto __D;
      }
    }
    /* A = A/2, B = B/2 */
    if ((res = mp_div_2 (&A, &A)) != MP_OKAY) {
      goto __D;
    }
    if ((res = mp_div_2 (&B, &B)) != MP_OKAY) {
      goto __D;
    }
  }


  /* 5.  while v is even do */
  while (mp_iseven (&v) == 1) {
    /* 5.1 v = v/2 */
    if ((res = mp_div_2 (&v, &v)) != MP_OKAY) {
      goto __D;
    }
    /* 5.2 if C,D are even then */
    if (mp_iseven (&C) == 0 || mp_iseven (&D) == 0) {
      /* C = (C+y)/2, D = (D-x)/2 */
      if ((res = mp_add (&C, &y, &C)) != MP_OKAY) {
	goto __D;
      }
      if ((res = mp_sub (&D, &x, &D)) != MP_OKAY) {
	goto __D;
      }
    }
    /* C = C/2, D = D/2 */
    if ((res = mp_div_2 (&C, &C)) != MP_OKAY) {
      goto __D;
    }
    if ((res = mp_div_2 (&D, &D)) != MP_OKAY) {
      goto __D;
    }
  }

  /* 6.  if u >= v then */
  if (mp_cmp (&u, &v) != MP_LT) {
    /* u = u - v, A = A - C, B = B - D */
    if ((res = mp_sub (&u, &v, &u)) != MP_OKAY) {
      goto __D;
    }

    if ((res = mp_sub (&A, &C, &A)) != MP_OKAY) {
      goto __D;
    }

    if ((res = mp_sub (&B, &D, &B)) != MP_OKAY) {
      goto __D;
    }
  } else {
    /* v - v - u, C = C - A, D = D - B */
    if ((res = mp_sub (&v, &u, &v)) != MP_OKAY) {
      goto __D;
    }

    if ((res = mp_sub (&C, &A, &C)) != MP_OKAY) {
      goto __D;
    }

    if ((res = mp_sub (&D, &B, &D)) != MP_OKAY) {
      goto __D;
    }
  }

  /* if not zero goto step 4 */
  if (mp_iszero (&u) == 0)
    goto top;

  /* now a = C, b = D, gcd == g*v */

  /* if v != 1 then there is no inverse */
  if (mp_cmp_d (&v, 1) != MP_EQ) {
    res = MP_VAL;
    goto __D;
  }

  /* a is now the inverse */
  mp_exch (&C, c);
  res = MP_OKAY;

__D:mp_clear (&D);
__C:mp_clear (&C);
__B:mp_clear (&B);
__A:mp_clear (&A);
__V:mp_clear (&v);
__U:mp_clear (&u);
__Y:mp_clear (&y);
__X:mp_clear (&x);
__ERR:
  return res;
}
