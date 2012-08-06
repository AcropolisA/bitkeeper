/* Copyright (c) 1997 L.W.McVoy */
#include "system.h"
#include "sccs.h"
#include "range.h"
#include "logging.h"	/* lease_check() */

typedef	struct dstat {
	char	*name;		   /* name of the file */
	int	adds, dels, mods;  /* lines added/deleted/modified */
} dstat;

private int	nulldiff(char *name, df_opt *dop);
private	void	printHistogram(dstat *diffstats);

/*
 * diffs - show differences of SCCS revisions.
 *
 * diffs file file file....
 *	for each file that is checked out, diff it against the old version.
 * diffs -r<rev> file
 *	diff the checked out (or TOT) against rev like so
 *	diff rev TOT
 * diffs -r<r1>..<r2> file
 *	diff the two revisions like so
 *	diff r1 r2
 *
 * In a quite inconsistent but (to me) useful fashion, I don't default to
 * all files when there are no arguments.  I want
 *	diffs
 *	diffs -dalpha1
 * to behave differently.
 *
 * Rules for -r option:
 *
 *  diffs file
 *	no gfile or readonly gfile
 *		skip
 *	edited gfile
 *		diff TOT gfile
 *  diffs -r<rev> file   or  echo 'file|<rev>' | bk diffs -
 *	no gfile or readonly gfile
 *		diff <rev> TOT    (don't diff with gfile)
 *	edited gfile
 *		diff <rev> gfile
 *  diffs -r+ file
 *	no gfile
 *		skip
 *	readonly gfile
 *		diff TOT gfile (and do keywork expansion on TOT)
 *	edited gfile
 *		diff TOT gfile
 *  diffs -r<rev1> -r<rev2> file or echo 'file|<rev1>' | bk diffs -r<rev2> -
 *	state of gfile doesn't matter
 *		diff <rev1> <rev2>
 *
 *  XXX - need a -N which makes diffs more like diff -Nr, esp. w/ diffs -r@XXX
 */
int
diffs_main(int ac, char **av)
{
	int	rc, c;
	int	verbose = 0, empty = 0, errors = 0, force = 0;
	u32	flags = SILENT;
	df_opt	dop = {0};
	FILE	*fout = stdout;
	char	*name;
	char	*Rev = 0, *boundaries = 0;
	dstat	*diffstats = 0, *ds;
	int	diffstat_only = 0;
	longopt	lopts[] = {
		{ "stats-only", 340 },
		{ 0, 0 }
	};
	RANGE	rargs = {0};

	dop.out_header = 1;
	while ((c = getopt(ac, av,
		    "@|a;A;bBcC|d;efhHl|nNpr;R|suvw", lopts)) != -1) {
		switch (c) {
		    case 'A':
			flags |= GET_ALIGN;
			/*FALLTHROUGH*/
		    case 'a':
			flags = annotate_args(flags, optarg);
			if (flags == -1) usage();
			break;
		    case 'b': dop.ignore_ws_chg = 1; break;	/* doc 2.0 */
		    case 'B': /* unimplemented */    break;	/* doc 2.0 */
		    case 'c': dop.out_unified = 0;   break;	/* doc 2.0 */
		    case 'C': getMsg("diffs_C", 0, 0, stdout); exit(0);
		    case 'e': empty = 1; break;			/* don't doc */
		    case 'f': force = 1; break;
		    case 'h': dop.out_header = 0; break;	/* doc 2.0 */
		    case 'H': dop.out_comments = 1; break;
		    case 'l': boundaries = optarg; break;	/* doc 2.0 */
		    case 'n': dop.out_rcs = 1;	break;		/* doc 2.0 */
		    case 'N': dop.new_is_null = 1; break;
		    case 'p': dop.out_show_c_func = 1; break;	/* doc 2.0 */
		    case 'R': unless (Rev = optarg) Rev = "-"; break;
		    case 's': dop.sdiff = 1; break;		/* doc 2.0 */
		    case 'u': dop.out_unified = 1; break;	/* doc 2.0 */
		    case 'v': verbose = 1; break;		/* doc 2.0 */
		    case 'w': dop.ignore_all_ws = 1; break;	/* doc 2.0 */
		    case 'd':
			if (range_addArg(&rargs, optarg, 1)) usage();
			break;
		    case 'r':
			if (range_addArg(&rargs, optarg, 0)) usage();
			break;
		    case '@':
			if (range_urlArg(&rargs, optarg)) return (1);
			break;
		    case 340:	/* --stats-only */
			diffstat_only = 1;
			break;
		    default: bk_badArg(c, av);
		}
	}

	if ((rargs.rstart && (boundaries || Rev)) || (boundaries && Rev)) {
		fprintf(stderr, "%s: -R must be alone\n", av[0]);
		return (1);
	}

	if (diffstat_only && (dop.out_unified || !dop.out_header ||
		dop.out_comments || dop.out_rcs || dop.sdiff)) {
		fprintf(stderr, "%s: --stats-only should be alone\n", av[0]);
		return (1);
	}

	if ((dop.out_header && dop.out_unified) || diffstat_only) {
		dop.out_diffstat = 1;
		fout = fmem();
	}

	dop.flags = flags;

	/*
	 * If we specified both revisions then we don't need the gfile.
	 * If we specifed one rev, then the gfile is also optional, we'll
	 * do the parent against that rev if no gfile.
	 * If we specified no revs then there must be a gfile.
	 */
	if ((flags & GET_PREFIX) &&
	    !Rev && (!rargs.rstop || boundaries) && !streq("-", av[ac-1])) {
		fprintf(stderr,
		    "%s: must have both revisions with -A|U|M|O\n", av[0]);
		return (1);
	}

	name = sfileFirst("diffs", &av[optind], 0);
	while (name) {
		sccs	*s = 0;
		ser_t	d;
		char	*r1 = 0, *r2 = 0;

		/*
		 * Unless we are given endpoints, don't diff.
		 * This is a big performance win.
		 * 2005-06: Endpoints meaning extended for diffs -N.
		 */
		unless (force ||
		    rargs.rstart || boundaries || Rev || sfileRev()) {
			char	*gfile = sccs2name(name);

			unless (writable(gfile) ||
			    ((dop.new_is_null) && !exists(name) && exists(gfile))){
				free(gfile);
				goto next;
			}
			free(gfile);
		}
		s = sccs_init(name, SILENT);
		unless (s && HASGRAPH(s)) {
			if (nulldiff(name, &dop) == 2) goto out;
			goto next;
		}
		if (boundaries) {
			unless (d = sccs_findrev(s, boundaries)) {
				fprintf(stderr,
				    "No delta %s in %s\n",
				    boundaries, s->gfile);
				goto next;
			}
			range_cset(s, d);
			if (s->rstart && PARENT(s, s->rstart)) {
				s->rstart = PARENT(s, s->rstart);
			}
		} else if (Rev) {
			/* r1 == r2  means diff against the parent(s)(s)  */
			/* XXX TODO: probably needs to support -R+	  */
			if (streq(Rev, "-")) {
				unless (r1 = r2 = sfileRev()) {
					fprintf(stderr,
					    "diffs: -R- needs file|rev.\n");
					goto next;
				}
			} else {
				r1 = r2 = Rev;
			}
		} else {
			int	restore = 0;

			/*
			 * XXX - if there are other commands which want the
			 * edited file as an arg, we should make this code
			 * be a function.  export/rset/gnupatch use it.
			 */
			// XXX
			if (rargs.rstart && streq(rargs.rstart, ".")) {
				restore = 1;
				if (HAS_GFILE(s) && WRITABLE(s)) {
					rargs.rstart = 0;
				} else {
					rargs.rstart = "+";
				}
			}
			if (range_process("diffs", s, RANGE_ENDPOINTS,&rargs)) {
				unless (empty) goto next;
				s->rstart = TREE(s);
			}
			if (restore) rargs.rstart = ".";
		}
		if (s->rstart) {
			unless (r1 = REV(s, s->rstart)) goto next;
			if ((rargs.rstop) && (s->rstart == s->rstop)) goto next;
			if (s->rstop) r2 = REV(s, s->rstop);
		}

		/*
		 * Optimize out the case where we we are readonly and diffing
		 * TOT.
		 * EDITED() doesn't work because they could have chmod +w
		 * the file.
		 */
		if (!r1 && (!HAS_GFILE(s) || (!force && !WRITABLE(s)))) {
			goto next;
		}

		/*
		 * Optimize out the case where we have a locked file with
		 * no changes at TOT.
		 * XXX: the following doesn't make sense because of PFILE:
		 * EDITED() doesn't work because they could have chmod +w
		 * the file.
		 */
		if (!r1 && WRITABLE(s) && HAS_PFILE(s) && !MONOTONIC(s)) {
			rc = sccs_hasDiffs(s, flags, 1);
			if (BAM(s) && ((rc < 0) || (rc > 1))) {
				errors |= 2;
				goto next;
			}
			unless (rc) goto next;
			if (BAM(s)) {
				if (dop.out_header) {
					printf("===== %s %s vs edited =====\n",
					    s->gfile, REV(s, sccs_top(s)));
				}
				printf("Binary file %s differs\n", s->gfile);
				goto next;
			}
		}
		/*
		 * If diffing against an edited file and tip,
		 * then we need a write lease.
		 * This includes: bk diffs foo; bk diffs -r+ foo; 
		 * but not bk diffs -r1.42 foo if 1.42 is not tip
		 */
		if (!r2 && WRITABLE(s) && HAS_PFILE(s)) {
			if (!r1 || (sccs_top(s) == sccs_findrev(s, r1))) {
				lease_check(s->proj, O_WRONLY, s);
			}
		}

		/*
		 * Errors come back as -1/-2/-3/0
		 * -2/-3 means it couldn't find the rev; ignore.
		 *
		 * XXX - need to catch a request for annotations w/o 2 revs.
		 */
		dop.adds = dop.dels = dop.mods = 0;
		rc = sccs_diffs(s, r1, r2, &dop, fout);
		if (dop.out_diffstat && (dop.adds || dop.dels || dop.mods)) {
			ds = addArray(&diffstats, 0);
			ds->name = strdup(s->gfile);
			ds->adds = dop.adds;
			ds->dels = dop.dels;
			ds->mods = dop.mods;
		}
		switch (rc) {
		    case -1:
			fprintf(stderr,
			    "diffs of %s failed.\n", s->gfile);
			break;
		    case -2:
		    case -3:
			break;
		    case 0:	
			if (verbose) fprintf(stderr, "%s\n", s->gfile);
			break;
		    default:
			fprintf(stderr,
			    "diffs of %s failed.\n", s->gfile);
		}
next:		if (s) {
			sccs_free(s);
			s = 0;
		}
		name = sfileNext();
	}
	if (dop.out_diffstat) {
		char	*data;
		size_t	len;

		if (diffstat_only || (nLines(diffstats) > 1)) {
			printHistogram(diffstats);
			putchar('\n');
		}
		data = fmem_close(fout, &len);
		unless (diffstat_only) fwrite(data, 1, len, stdout);
		FREE(data);
	}
out:	if (sfileDone()) errors |= 4;
	return (errors);
}

private int
ilog10(int i)
{
	int	n = 0;

	while (i /= 10) n++;
	return (n);
}

private void
printHistogram(dstat *diffstats)
{
	int	maxlen, maxdiffs, maxbar, n, m;
	int	adds, dels, mods, files;
	int	i;
	double	factor;
	dstat	*ds;
	char	hist[MAXLINE];

#define	DIGITS(x) ((x) ? ilog10(x) + 1 : 1)

	maxdiffs = maxlen = 0;
	files = adds = dels = mods = 0;
	EACHP(diffstats, ds) {
		n = strlen(ds->name);
		if (n > maxlen) maxlen = n;
		if ((ds->adds + ds->dels + ds->mods) > maxdiffs) {
			maxdiffs = ds->adds + ds->dels + ds->mods;
		}
	}

	maxbar = 80 - maxlen - strlen(" | ") - DIGITS(maxdiffs) - 1;
	if (maxdiffs < maxbar) {
		factor = 1.0;
	} else {
		factor = (double)maxbar / (double)maxdiffs;
	}
	EACHP(diffstats, ds) {
		n = 0;
		m = (int)((double)ds->adds * factor);
		for (i = 0; i < m; i++) hist[n++] = '+';
		m = (int)((double)ds->mods * factor);
		for (i = 0; i < m; i++) hist[n++] = '~';
		m = (int)((double)ds->dels * factor);
		for (i = 0; i < m; i++) hist[n++] = '-';
		hist[n] = 0;
		printf(" %-*.*s | %*d %s\n", maxlen, maxlen,
		    ds->name,
		    DIGITS(maxdiffs),
		    ds->adds + ds->dels + ds->mods, hist);
		files++;
		adds += ds->adds;
		dels += ds->dels;
		mods += ds->mods;
		FREE(ds->name);
	}
	FREE(diffstats);
	printf(" %d files changed", files);
	if (adds) printf(", %d insertions(+)", adds);
	if (mods) printf(", %d modifications(~)", mods);
	if (dels) printf(", %d deletions(-)", dels);
	printf("\n");
}

private int
nulldiff(char *name, df_opt *dop)
{
	int	ret = 0;
	char	*here, *file, *p;

	name = sccs2name(name);
	unless (dop->new_is_null) {
		printf("New file: %s\n", name);
		goto out;
	}
	unless (ascii(name)) {
		fprintf(stderr, "Warning: skipping binary '%s'\n", name);
		goto out;
	}
	if (dop->out_header) {
		printf("===== New file: %s =====\n", name);
		/* diff() uses write, not stdio */
		if (fflush(stdout)) {
			ret = 2;
			goto out;
		}
	}

	/*
	 * Wayne liked this better but I had to work around a nasty bug in
	 * that the chdir() below changes the return from proj_cwd().
	 * Hence the strdup.  I think we want a pushd/popd sort of interface.
	 */
	here = strdup(proj_cwd());
	p = strrchr(here, '/');
	assert(p);
	file = aprintf("%s/%s", p+1, name);
	chdir("..");
	ret = diff_files(DEVNULL_RD, file, dop, 0, "-");
	chdir(here);
	free(here);
	free(file);
out:	free(name);
	return (ret);
}
