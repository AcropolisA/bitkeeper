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

/* computes Y == G^X mod P, HAC pp.616, Algorithm 14.85
 *
 * Uses a left-to-right k-ary sliding window to compute the modular exponentiation.
 * The value of k changes based on the size of the exponent.
 *
 * Uses Montgomery reduction 
 */
int
mp_exptmod_fast (mp_int * G, mp_int * X, mp_int * P, mp_int * Y, int redmode)
{
  mp_int  M[256], res;
  mp_digit buf, mp;
  int     err, bitbuf, bitcpy, bitcnt, mode, digidx, x, y, winsize;
  int     (*redux)(mp_int*,mp_int*,mp_digit);
  

  /* find window size */
  x = mp_count_bits (X);
  if (x <= 7) {
    winsize = 2;
  } else if (x <= 36) {
    winsize = 3;
  } else if (x <= 140) {
    winsize = 4;
  } else if (x <= 450) {
    winsize = 5;
  } else if (x <= 1303) {
    winsize = 6;
  } else if (x <= 3529) {
    winsize = 7;
  } else {
    winsize = 8;
  }

  /* init G array */
  for (x = 0; x < (1 << winsize); x++) {
    if ((err = mp_init (&M[x])) != MP_OKAY) {
      for (y = 0; y < x; y++) {
	mp_clear (&M[y]);
      }
      return err;
    }
  }
  
  if (redmode == 0) {
     /* now setup montgomery  */
     if ((err = mp_montgomery_setup (P, &mp)) != MP_OKAY) {
        goto __M;
     }
     redux = mp_montgomery_reduce;
  } else {
     /* setup DR reduction */
     mp_dr_setup(P, &mp);
     redux = mp_dr_reduce;
  }

  /* setup result */
  if ((err = mp_init (&res)) != MP_OKAY) {
    goto __RES;
  }

  /* create M table
   *
   * The M table contains powers of the input base, e.g. M[x] = G^x mod P
   *
   * The first half of the table is not computed though accept for M[0] and M[1]
   */

  if (redmode == 0) {
     /* now we need R mod m */
     if ((err = mp_montgomery_calc_normalization (&res, P)) != MP_OKAY) {
       goto __RES;
     }

     /* now set M[1] to G * R mod m */
     if ((err = mp_mulmod (G, &res, P, &M[1])) != MP_OKAY) {
       goto __RES;
     }
  } else {
     mp_set(&res, 1);
     if ((err = mp_mod(G, P, &M[1])) != MP_OKAY) {
        goto __RES;
     }
  }
  
  /* compute the value at M[1<<(winsize-1)] by squaring M[1] (winsize-1) times */
  if ((err = mp_copy (&M[1], &M[1 << (winsize - 1)])) != MP_OKAY) {
    goto __RES;
  }

  for (x = 0; x < (winsize - 1); x++) {
    if ((err = mp_sqr (&M[1 << (winsize - 1)], &M[1 << (winsize - 1)])) != MP_OKAY) {
      goto __RES;
    }
    if ((err = redux (&M[1 << (winsize - 1)], P, mp)) != MP_OKAY) {
      goto __RES;
    }
  }

  /* create upper table */
  for (x = (1 << (winsize - 1)) + 1; x < (1 << winsize); x++) {
    if ((err = mp_mul (&M[x - 1], &M[1], &M[x])) != MP_OKAY) {
      goto __RES;
    }
    if ((err = redux (&M[x], P, mp)) != MP_OKAY) {
      goto __RES;
    }
  }

  /* set initial mode and bit cnt */
  mode = 0;
  bitcnt = 0;
  buf = 0;
  digidx = X->used - 1;
  bitcpy = bitbuf = 0;

  bitcnt = 1;
  for (;;) {
    /* grab next digit as required */
    if (--bitcnt == 0) {
      if (digidx == -1) {
	break;
      }
      buf = X->dp[digidx--];
      bitcnt = (int) DIGIT_BIT;
    }

    /* grab the next msb from the exponent */
    y = (buf >> (DIGIT_BIT - 1)) & 1;
    buf <<= 1;

    /* if the bit is zero and mode == 0 then we ignore it
     * These represent the leading zero bits before the first 1 bit
     * in the exponent.  Technically this opt is not required but it
     * does lower the # of trivial squaring/reductions used
     */
    if (mode == 0 && y == 0)
      continue;

    /* if the bit is zero and mode == 1 then we square */
    if (mode == 1 && y == 0) {
      if ((err = mp_sqr (&res, &res)) != MP_OKAY) {
	goto __RES;
      }
      if ((err = redux (&res, P, mp)) != MP_OKAY) {
	goto __RES;
      }
      continue;
    }

    /* else we add it to the window */
    bitbuf |= (y << (winsize - ++bitcpy));
    mode = 2;

    if (bitcpy == winsize) {
      /* ok window is filled so square as required and multiply multiply */
      /* square first */
      for (x = 0; x < winsize; x++) {
	if ((err = mp_sqr (&res, &res)) != MP_OKAY) {
	  goto __RES;
	}
	if ((err = redux (&res, P, mp)) != MP_OKAY) {
	  goto __RES;
	}
      }

      /* then multiply */
      if ((err = mp_mul (&res, &M[bitbuf], &res)) != MP_OKAY) {
	goto __RES;
      }
      if ((err = redux (&res, P, mp)) != MP_OKAY) {
	goto __RES;
      }

      /* empty window and reset */
      bitcpy = bitbuf = 0;
      mode = 1;
    }
  }

  /* if bits remain then square/multiply */
  if (mode == 2 && bitcpy > 0) {
    /* square then multiply if the bit is set */
    for (x = 0; x < bitcpy; x++) {
      if ((err = mp_sqr (&res, &res)) != MP_OKAY) {
	goto __RES;
      }
      if ((err = redux (&res, P, mp)) != MP_OKAY) {
	goto __RES;
      }

      bitbuf <<= 1;
      if ((bitbuf & (1 << winsize)) != 0) {
	/* then multiply */
	if ((err = mp_mul (&res, &M[1], &res)) != MP_OKAY) {
	  goto __RES;
	}
	if ((err = redux (&res, P, mp)) != MP_OKAY) {
	  goto __RES;
	}
      }
    }
  }

  if (redmode == 0) {
     /* fixup result */
     if ((err = mp_montgomery_reduce (&res, P, mp)) != MP_OKAY) {
       goto __RES;
     }
  }     

  mp_exch (&res, Y);
  err = MP_OKAY;
__RES:mp_clear (&res);
__M:
  for (x = 0; x < (1 << winsize); x++) {
    mp_clear (&M[x]);
  }
  return err;
}
