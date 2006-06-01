/* Copyright (c) 1997 L.W.McVoy */
#include "system.h"
#include "sccs.h"
#include "range.h"

int
prs_main(int ac, char **av)
{
	return (log_main(ac, av));
}

int
log_main(int ac, char **av)
{
	sccs	*s;
	delta	*e;
	int	log = streq(av[0], "log");
	int	reverse = 0, doheader = !log;
	int	init_flags = INIT_NOCKSUM;
	int	flags = 0, sf_flags = 0;
	int	opposite = 0;
	int	rc = 0, c;
	char	*name, *xrev = 0;
	char	*cset = 0, *tip = 0;
	int	noisy = 0;
	int	expand = 1;
	int	want_parent = 0;
	pid_t	pid = 0;	/* pager */
	char	*dspec = getenv("BK_LOG_DSPEC");
	RANGE_DECL;

	unless (dspec) dspec = log ? ":LOG:" : ":PRS:";

	while ((c = getopt(ac, av, "abc;C;d:DfhMnopr|t|x:vY")) != -1) {
		switch (c) {
		    case 'a':					/* doc 2.0 */
			/* think: -Ma, the -M set expand already */
			if (expand < 2) expand = 2;
			flags |= PRS_ALL;
			break;
		    case 'D': break;	/* obsoleted in 4.0 */
		    case 'f':					/* doc */
		    case 'b': reverse++; break;			/* undoc */
		    case 'C': cset = optarg; break;		/* doc 2.0 */
		    case 'd': dspec = optarg; break;		/* doc 2.0 */
		    case 'h': doheader = 0; break;		/* doc 2.0 */
		    case 'M': expand = 3; break;		/* doc 2.0 */
		    case 'n': flags |= PRS_LF; break;		/* doc 2.0 */
		    case 'o': opposite = 1; doheader = 0; break; /* doc 2.0 */
		    case 'p': want_parent = 1; break;
		    case 't': tip = optarg; break;
		    case 'x': xrev = optarg; break;		/* doc 2.0 */
		    case 'v': noisy = 1; break;			/* doc 2.0 */
		    case 'Y': 	/* for backward compat, undoc 2.0 */
			      break;
		    RANGE_OPTS('c', 'r');			/* doc 2.0 */
		    default:
usage:			system("bk help -s log");
			return (1);
		}
	}

	if (things && (cset || tip)) {
		fprintf(stderr, "log: -r, -C, -t are mutually exclusive.\n");
		exit(1);
	}
	if (log) pid = mkpager();
	for (name = sfileFirst("log", &av[optind], sf_flags);
	    name; name = sfileNext()) {
		unless (s = sccs_init(name, init_flags)) continue;
		unless (HASGRAPH(s)) goto next;
		if (cset) {
			delta	*d = sccs_findrev(s, cset);

			if (!d) {
				rc = 1;
				goto next;
			}
			rangeCset(s, d);
		} else if (tip) {
			unless (e = sccs_findrev(s, tip)) {
				unless (noisy) goto next;
				fprintf(stderr,
				    "log: can't find %s|%s\n", s->gfile, tip);
				goto next;
			}
			sccs_color(s, e);
			if (CSET(s) && (flags & PRS_ALL)) sccs_tagcolor(s, e);
			for (e = s->table; e; e = e->next) {
				unless (e->flags & (D_RED|D_BLUE)) continue;
				unless (s->rstop) s->rstop = e;
				s->rstart = e;
				e->flags |=D_SET;
			}
			s->state |= S_SET;
		} else {
			if (flags & PRS_ALL) s->state |= S_SET;
			RANGE("log", s, expand, noisy);
			/* happens when we have only 1.0 delta */
			unless (s->rstart) goto next;
		}
		assert(s->rstop);
		if (flags & PRS_ALL) {
			assert(s->state & S_SET);
			sccs_markMeta(s);
		}
		if (doheader) {
			printf("======== %s %s%s%s",
			    s->gfile,
			    opposite ? "!" : "",
			    s->rstart->rev,
			    (xrev && streq(xrev, "1st")) ? "+" : "");
			if (s->rstop != s->rstart) {
				printf("..%s", s->rstop->rev);
			}
			printf(" ========\n");
		}
		if (opposite) {
			for (e = s->table; e; e = e->next) {
				if (e->flags & D_SET) {
					e->flags &= ~D_SET;
				} else {
					e->flags |= D_SET;
				}
			}
		}
		if (xrev) {
			unless (s->state & S_SET) { 
				int	check = strcmp(xrev, "1st");

				for (e = s->rstop; e; e = e->next) {
					unless (check && streq(xrev, e->rev)) {
						e->flags |= D_SET;
					}
					if (e == s->rstart) break;
				}
				s->state |= S_SET;
				unless (check) s->rstart->flags &= ~D_SET;
			} else {
				if (streq(xrev, "1st")) {
					s->rstart->flags &= ~D_SET;
				} else {
					e = sccs_findrev(s, xrev);
					if (e) e->flags &= ~D_SET;
				}
			}
		}

		if (want_parent) {
			unless (SET(s)) {
				for (e = s->rstop; e; e = e->next) {
					e->flags |= D_SET;
					if (e == s->rstart) break;
				}
				s->state |= S_SET;
			}
			for (e = s->table; e; e = e->next) {
				if (e->flags & D_SET) {
					e->flags &= ~D_SET;
					if (e->parent) {
						e->parent->flags |= D_RED;
					} else {
						fprintf(stderr,
					      "Warning: %s: %s has no parent\n",
						    s->gfile, e->rev);
					}
				}
			}
			for (e = s->table; e; e = e->next) {
				if (e->flags & D_RED) {
					e->flags |= D_SET;
					e->flags &= ~D_RED;
				}
			}
		}
		sccs_prs(s, flags, reverse, dspec, stdout);
		sccs_free(s);
		continue;
		
next:		rc = 1;
		sccs_free(s);
	}
	if (sfileDone()) rc = 1;
	if (log && (pid > 0)) {
		fclose(stdout);
		waitpid(pid, 0, 0);
	}
	return (rc);
}
