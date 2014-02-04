#ifndef	_RANGE_H
#define	_RANGE_H

typedef struct range RANGE;
struct	range {
	char	*rstart, *rstop; /* endpoints from command line */
	u32	isdate:1;	 /* contains date range */
	u32	isrev:1;	 /* contains rev range */
};

#define RANGE_ENDPOINTS	0x10	/* just return s->rstart s->rstop */
#define	RANGE_SET	0x20	/* return S_SET */

#define	WR_BOTH		0x01	/* keep RED and BLUE; callback on both */
#define	WR_GCA		0x02	/* Callback only on the gca deltas */
#define	WR_STOP		0x04	/* Callback controls stopping point (gca) */

/*
 *  1.1 -- 1.2 -- 1.3 -- 1.4 -- 1.5
 *	\                      /
 *	 \                    /
 *	  - 1.1.1.1----------/
 *
 * With RANGE_SET:
 *
 *	-r1.3..1.5
 *	     s->state |= S_SET
 *	     marks 1.1.1.1,1.4,1.5 with D_SET
 *	     sets s->rstart = 1.1.1.1
 *	     sets s->rstop  = 1.5
 *
 *     (rstart/rstop are just the first/last serials where D_SET might be set)
 *
 * With RANGE_ENDPOINTS:
 *
 *	-r1.3..1.5
 *	     sets s->rstart = 1.3
 *	     sets s->rstop = 1.5
 */

int	range_process(char *me, sccs *s, u32 flags, RANGE *rargs);
int	range_addArg(RANGE *rargs, char *arg, int isdate);
int	range_urlArg(RANGE *rargs, char *url);

void	range_cset(sccs *s, ser_t d);
time_t	range_cutoff(char *spec);
void	range_markMeta(sccs *s);
int	range_gone(sccs *s, ser_t d, u32 dflags);
void	range_unrange(sccs *s, ser_t *left, ser_t *right, int all);

int	range_walkrevs(sccs *s, ser_t from, ser_t *fromlist, ser_t to,
    int flags, int (*fcn)(sccs *s, ser_t d, void *token), void *token);
int	walkrevs_setFlags(sccs *s, ser_t d, void *token);
int	walkrevs_clrFlags(sccs *s, ser_t d, void *token);
int	walkrevs_printkey(sccs *s, ser_t d, void *token);
int	walkrevs_printmd5key(sccs *s, ser_t d, void *token);
int	walkrevs_addSer(sccs *s, ser_t d, void *token);
int	walkrevs_countIfDSET(sccs *s, ser_t d, void *token);

#endif
