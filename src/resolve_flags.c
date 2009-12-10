/*
 * resolve_flags.c - auto resolver for file flags
 */
#include "resolve.h"

/* add the local modes to the remote file */
private int
f_local(resolve *rs)
{
	delta	*l = sccs_findrev(rs->s, rs->revs->local);
	delta	*r = sccs_findrev(rs->s, rs->revs->remote);

	sccs_close(rs->s); /* for win32 */
	flags_delta(rs, rs->s->sfile,
	    r, sccs_xflags(rs->s, l), sccs_Xfile(rs->s, 'r'), REMOTE);
	return (1);
}

/* add the remote modes to the local file */
private int
f_remote(resolve *rs)
{
	delta	*l = sccs_findrev(rs->s, rs->revs->local);
	delta	*r = sccs_findrev(rs->s, rs->revs->remote);

	sccs_close(rs->s); /* for win32 */
	flags_delta(rs, rs->s->sfile,
	    l, sccs_xflags(rs->s, r), sccs_Xfile(rs->s, 'r'), LOCAL);
	return (1);
}

/*
 * auto resolve flags according to the following table.
 * 
 * key: <gca val><local val><remote val> = <new val>
 *
 *  000 -> 0
 *  001 -> 1
 *  010 -> 1
 *  011 -> 1
 *  100 -> 0
 *  101 -> 0
 *  110 -> 0
 *  111 -> 1
 *
 *   out = ((L ^ G) | (R ^ G)) ^ G
 *   out = (L & R) | ~G & (~L & R | L & ~R)
 *   out = (L & R) | ~G & (L ^ R)
 */
int
resolve_flags(resolve *rs)
{
	delta	*l = sccs_findrev(rs->s, rs->revs->local);
	delta	*r = sccs_findrev(rs->s, rs->revs->remote);
	delta	*g = sccs_findrev(rs->s, rs->revs->gca);
	int	lf, rf, gf, newflags;

        if (rs->opts->debug) {
		fprintf(stderr, "resolve_flags: ");
		resolve_dump(rs);
	}
	lf = sccs_xflags(rs->s, l);
	rf = sccs_xflags(rs->s, r);
	gf = sccs_xflags(rs->s, g);

	newflags = (lf & rf) | ~gf & (lf ^ rf);

	if (newflags == lf) {
		f_local(rs);
		unless (rs->opts->quiet) {
			fprintf(stderr,
			    "automerge OK.  Using flags from local copy.\n");
		}
	} else if (newflags == rf) {
		f_remote(rs);
		unless (rs->opts->quiet) {
			fprintf(stderr,
			    "automerge OK.  Using flags from remote copy.\n");
		}
	} else {
		/* add the new modes to the local file */
		sccs_close(rs->s); /* for win32 */
		flags_delta(rs, rs->s->sfile, 
		    l, newflags, sccs_Xfile(rs->s, 'r'), LOCAL);
		/* remove delta must be refetched after previous delta */
		r = sccs_findrev(rs->s, rs->revs->remote);
		sccs_close(rs->s); /*
				    * for win32, have to close it again
				    * becuase the previous flags_delta()
				    * called sccs_init()
				    */
		flags_delta(rs, rs->s->sfile, 
		    r, newflags, sccs_Xfile(rs->s, 'r'), REMOTE);
		unless (rs->opts->quiet) {
			fprintf(stderr,
			    "automerge OK. Added merged flags "
			    "to both local and remote versions.\n");
		}
	}
	return (1);
}
