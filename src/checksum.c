#include "system.h"
#include "sccs.h"
#include "zlib/zlib.h"
#include "logging.h"

WHATSTR("@(#)%K%");

private	int	do_chksum(int fd, int off, int *sump);
private	int	chksum_sccs(char **files, char *offset);
private	int	do_file(char *file, int off);

/*
 * checksum - check and/or regenerate the checksums associated with a file.
 *
 * Copyright (c) 1999-2001 L.W.McVoy
 */
int
checksum_main(int ac, char **av)
{
	sccs	*s;
	delta	*d;
	int	doit = 0;
	char	*name;
	int	fix = 0, diags = 0, bad = 0, do_sccs = 0, ret = 0, spin = 0;
	int	c;
	char	*off = 0;
	char	*rev = 0;
	project	*proj = 0;

	if (ac > 1 && streq("--help", av[1])) {
		system("bk help checksum");
		return (0);
	}
	while ((c = getopt(ac, av, "cfr;s|v/")) != -1) {
		switch (c) {
		    case 'c': break;	/* obsolete */
		    case 'f': fix = 1; break;			/* doc 2.0 */
		    case 'r': rev = optarg; break;
		    case 's': do_sccs = 1; off = optarg; break;
		    case '/': spin = 1; break;
		    case 'v': diags++; break;			/* doc 2.0 */
		    default:  system("bk help -s checksum");
			      return (1);
		}
	}

	if (do_sccs) return (chksum_sccs(&av[optind], off));

	if (fix ? repository_wrlock() : repository_rdlock()) {
		repository_lockers(0);
		return (1);
	}
	for (name = sfileFirst("checksum", &av[optind], 0);
	    name; name = sfileNext()) {
		s = sccs_init(name, INIT_SAVEPROJ, proj);
		unless (s) continue;
		unless (proj) proj = s->proj;
		unless (HASGRAPH(s)) {
			fprintf(stderr, "%s: can't read SCCS info in \"%s\".\n",
			    av[0], s->sfile);
			continue;
		}
		unless (BITKEEPER(s)) {
			fprintf(stderr,
			    "%s: \"%s\" is not a BitKeeper file, ignored\n",
			    av[0], s->sfile);
			continue;
		}
		doit = bad = 0;
		/* should this be changed to use the range code? */
		if (rev) {
			unless (d = sccs_findrev(s, rev)) {
				fprintf(stderr,
				    "%s: unable to find rev %s in %s\n",
				    av[0], rev, s->gfile);
				continue;
			}
			c = sccs_resum(s, d, diags, fix);
			if (c & 1) doit++;
			if (c & 2) bad++;
		} else {
			if (CSET(s)) {
				doit = bad = cset_resum(s, diags, fix, spin);
			} else {
				for (d = s->table; d; d = d->next) {
					unless (d->type == 'D') continue;
					c = sccs_resum(s, d, diags, fix);
					if (c & 1) doit++;
					if (c & 2) bad++;
				}
			}
		}
		if (diags && fix) {
			fprintf(stderr,
			    "%s: fixed bad metadata in %d deltas\n",
			    s->gfile, doit);
		}
		if (diags && !fix) {
			fprintf(stderr,
			    "%s: bad metadata in %d deltas\n",
			    s->gfile, bad);
		}
		if ((doit || !s->cksumok) && fix) {
			unless (sccs_restart(s)) { perror("restart"); exit(1); }
			if (sccs_admin(
			    s, 0, NEWCKSUM, 0, 0, 0, 0, 0, 0, 0, 0)) {
			    	ret = 2;
				unless (BEEN_WARNED(s)) {
					fprintf(stderr,
					    "admin -z of %s failed.\n",
					    s->sfile);
				}
			}
		}
		sccs_free(s);
	}
	sfileDone();
	if (proj) proj_free(proj);
	fix ? repository_wrunlock(0) : repository_rdunlock(0);
	return (ret ? ret : (doit ? 1 : 0));
}

int
sccs_resum(sccs *s, delta *d, int diags, int fix)
{
	int	i;
	int	err = 0;
	char	before[25];	/* ^As 00000/00000/00000\n == 21 chars */
	char	after[25];	/* ^As 00000/00000/00000\n == 21 chars */

	if (LOGS_ONLY(s)) return (0);

	unless (d) d = sccs_top(s);

	if (S_ISLNK(d->mode)) {
		u8	*t;
		sum_t	sum = 0;
		delta	*e;

		/* don't complain about these, old BK binaries did this */
		e = getSymlnkCksumDelta(s, d);
		if (!fix && !e->sum) return (0);

		for (t = d->symlink; *t; sum += *t++);
		if ((e->flags & D_CKSUM) && (e->sum == sum)) return (0);
		unless (fix) {
			fprintf(stderr, "Bad symlink checksum %d:%d in %s|%s\n",
			    e->sum, sum, s->gfile, d->rev);
			return (2);
		} else {
			if (diags > 1) {
				fprintf(stderr, "Corrected %s:%s %d->%d\n",
				    s->sfile, d->rev, d->sum, sum);
			}
			d->sum = sum;
			d->flags |= D_CKSUM;
			return (1);
		}
	}

	/*
	 * This can rewrite added / deleted / same , so do it first
	 * then check to see if they are none zero, then check checksum
	 */

	if (sccs_get(s,
	    d->rev, 0, 0, 0, GET_SUM|GET_SHUTUP|SILENT|PRINT, "-")) {
		unless (BEEN_WARNED(s)) {
			fprintf(stderr,
			    "get of %s:%s failed, skipping it.\n",
			    s->gfile, d->rev);
		}
		return (4);
	}

	sccs_fitCounters(before, d->added, d->deleted, d->same);
	sccs_fitCounters(after, s->added, s->deleted, s->same);
	unless (streq(before, after)) {
		size_t	n;

		n = strlen(before);
		assert(n > 4 && before[n-1] == '\n');
		before[n-1] = 0;

		n = strlen(after);
		assert(n  > 4 && after[n-1] == '\n');
		after[n-1] = 0;

		unless (fix) {
#if	0
			/* XXX: t.long_line has one line counted as 4
			 * which means repositories out in the world which
			 * are working, would all of a sudden start gakking
			 */
			fprintf(stderr,
		    		"Bad a/d/s %s:%s in %s|%s\n",
		    		&before[3], &after[3], s->gfile, d->rev);
			err = 2;
#endif
		}
		else {
			if (diags > 1) {
				fprintf(stderr, "Corrected a/d/s "
					"%s:%s %s->%s\n",
		    			s->sfile, d->rev,
					&before[3], &after[3]);
			}
			d->added = s->added;
			d->deleted = s->deleted;
			d->same = s->same;
			err = 1;
		}
	}

	/*
	 * If there is no content change, then if no checksum, cons one up
	 * from the data in the delta table.
	 * NOTE: check using newly computed added and deleted (in *s)
	 */
	unless (s->added || s->deleted || d->include || d->exclude) {
		int	new = 0;

		if (d->flags & D_CKSUM) return (err);
		new = adler32(new, d->sdate, strlen(d->sdate));
		new = adler32(new, d->user, strlen(d->user));
		if (d->pathname) {
			new = adler32(new, d->pathname, strlen(d->pathname));
		}
		if (d->hostname) {
			new = adler32(new, d->hostname, strlen(d->hostname));
		}
		EACH(d->comments) {
			new = adler32(new,
			    d->comments[i], strlen(d->comments[i]));
		}
		unless (fix) {
			fprintf(stderr, "%s:%s actual=<none> sum=%d\n",
			    s->gfile, d->rev, new);
			return (2);
		}
		if (diags > 1) {
			fprintf(stderr, "Derived %s:%s -> %d\n",
			    s->sfile, d->rev, (sum_t)new);
		}
		d->sum = (sum_t)new;
		d->flags |= D_CKSUM;
		return (1);
	}

	if ((d->flags & D_CKSUM) && (d->sum == s->dsum)) return (err);
	unless (fix) {
		fprintf(stderr,
		    "Bad checksum %d:%d in %s|%s\n",
		    d->sum, s->dsum, s->gfile, d->rev);
		return (2);
	}
	if (diags > 1) {
		fprintf(stderr, "Corrected %s:%s %d->%d\n",
		    s->sfile, d->rev, d->sum, s->dsum);
	}
	d->sum = s->dsum;
	d->flags |= D_CKSUM;
	return (1);
	assert("Not reached" == 0);
}


/*
 * Calculate the same checksum as is used in BitKeeper.
 */
private int
chksum_sccs(char **files, char *offset)
{
	int	sum, i;
	int	off = 0;
	char	buf[MAXPATH];

	if (offset) off = atoi(offset);
	unless (files[0]) {
		if (do_chksum(0, off, &sum)) return (1);
		printf("%d\n", sum);
	} else if (streq("-", files[0]) && !files[1]) {
		while (fnext(buf, stdin)) {
			chop(buf);
			if (do_file(buf, off)) return (1);
		}
	} else for (i = 0; files[i]; ++i) {
		if (do_file(files[i], off)) return (1);
	}
	return (0);
}

private int
do_file(char *file, int off)
{
	int	sum, fd;

	fd = open(file, 0, 0);
	if (fd == -1) {
		perror(file);
		return (1);
	}
	if (do_chksum(fd, off, &sum)) {
		close(fd);
		return (1);
	}
	close(fd);
	printf("%-20s %d\n", file, sum);
	return (0);
}

private int
do_chksum(int fd, int off, int *sump)
{
	u8	 buf[16<<10];
	register unsigned char *p;
	register int i;
	u16	 sum = 0;

	while (off--) {
		if (read(fd, buf, 1) != 1) return (1);
	}
	while ((i = read(fd, buf, sizeof(buf))) > 0) {
		for (p = buf; i--; sum += *p++);
	}
	*sump = (int)sum;
	return (0);
}

typedef struct serset {
	int	num, size;
	struct _sse {
		ser_t	ser;
		u16	sum;
	} data[0];
} serset;

#define	SS_SIZE  sizeof(serset)
#define	SSE_SIZE sizeof(struct _sse)

private void
add_ins(HASH *h, char *root, int len, ser_t ser, u16 sum)
{
	serset	**ssp, *ss;

	ssp = (serset **)hash_fetchAlloc(h, root, len, sizeof(serset *));
	unless (*ssp) {
		*ssp = malloc(SS_SIZE + 4*SSE_SIZE);
		(*ssp)->num = 0;
		(*ssp)->size = 4;
	} else if ((*ssp)->num == (*ssp)->size - 1) {
		/* realloc 2X */
		ss = malloc(SS_SIZE + (2 * (*ssp)->size)*SSE_SIZE);
		memcpy(ss, *ssp, SS_SIZE + (*ssp)->size*SSE_SIZE);
		ss->size = 2 * (*ssp)->size;
		free(*ssp);
		*ssp = ss;
	}
	ss = *ssp;
	ss->data[ss->num].ser = ser;
	ss->data[ss->num].sum = sum;
	++ss->num;
	ss->data[ss->num].ser = 0;
}

/* same semantics as sccs_resum() except one call for all deltas */
int
cset_resum(sccs *s, int diags, int fix, int spinners)
{
	HASH	*root2map = hash_new();
	ser_t	ins_ser = 0;
	char	*p, *q, *e;
	char	*end = s->mmap + s->size;
	u16	sum;
	int	cnt, i, added, n = 0;
	serset	**map;
	ser_t	*slist;
	struct	_sse *sse;
	delta	*d;
	int	found = 0;
	kvpair	kv;
	char	*spin = "|/-\\";

	if (spinners) fprintf(stderr, "checking checksums ");

	/* build up weave data structure */
	p = s->mmap + s->data;
	while (p < end) {
		if (p[0] == '\001') {
			if (p[1] == 'I') ins_ser = atoi(p + 3);
			e = p;
			while (*e++ != '\n');
		} else {
			q = separator(p);
			sum = 0;
			e = p;
			do {
				sum += *(unsigned char *)e;
			} while (*e++ != '\n');
			add_ins(root2map, p, q-p, ins_ser, sum);
		}
		p = e;
	}
	cnt = 0;
	EACH_HASH(root2map) ++cnt;
	map = malloc(cnt * sizeof(serset *));
	cnt = 0;
	EACH_HASH(root2map) {
		map[cnt] = *(serset **)kv.val.dptr;
		++cnt;
	}
	/* the above is very fast, no need to optimize further */

	/* foreach delta */
	for (d = s->table; d; d = d->next) {
		unless (d->type == 'D') continue;
		unless (d->added || d->include || d->exclude) continue;
		if (SET(s) && !(d->flags & D_SET)) {
			d->flags &= ~D_SET;	/* clean up as we go */
			continue;
		}

		slist = sccs_set(s, d, 0, 0); /* slow */
		if (spinners) {
			static	int sometimes;

			if (++sometimes == 10) {
				fprintf(stderr, "%c\b", spin[n++ % 4]);
				sometimes = 0;
			}
		}
		sum = 0;
		added = 0;
		for (i = 0; i < cnt; i++) {
			ser_t	ser;

			sse = map[i]->data;
			ser = sse->ser;
			while (ser) {
				if ((ser <= d->serial) && slist[ser]) {
					sum += sse->sum;
					if (ser == d->serial) ++added;
					break;
				}
				++sse;
				ser = sse->ser;
			}
		}
		free(slist);

		if ((d->added != added) || d->deleted || (d->same != 1)) {
			/*
			 * We dont report bad counts if we are not fixing.
			 * We have not been consistant about this in the past.
			 */
			if (diags > 1) {
				fprintf(stderr, "%s a/d/s "
					"%s:%s %d/%d/%d->%d/0/1\n",
				    (fix ? "Corrected" : "Bad"),
				    s->sfile, d->rev,
				    d->added, d->deleted, d->same, added);
			}
			if (fix) {
				d->added = added;
				d->deleted = 0;
				d->same = 1;
				++found;
			}
		}

		if (d->sum != sum) {
			if (!fix || (diags > 1)) {
				fprintf(stderr, "%s checksum %d:%d in %s|%s\n",
				    (fix ? "Corrected" : "Bad"),
				    d->sum, sum, s->gfile, d->rev);
			}
			if (fix) {
				d->sum = sum;
				d->flags |= D_CKSUM;
			}
			++found;
		}
	}
	s->state &= ~S_SET;	/* if set, then done with it: clean up */
	for (i = 0; i < cnt; i++) free(map[i]);
	free(map);
	hash_free(root2map);
	return (found);
}

#define	LINUX_ROOTKEY \
"torvalds@athlon.transmeta.com|ChangeSet|20020205173056|16047|c1d11a41ed024864"
#define	BADKEY	"4076e392jtslXUMZRrRU0ZIm6P7uVg"
#define	GOODKEY	"4076e392K8UBsl4E_GBNx5_z2ywdfg"

sccs *
cset_fixLinuxKernelChecksum(sccs *s)
{
	delta	*d;
	char	key[MAXKEY];

	sccs_sdelta(s, sccs_ino(s), key);
	unless (streq(key, LINUX_ROOTKEY)) return (s); /* linux only */
	if (exists(LOG_TREE)) return (s); /* don't fix openlogging */
	unless (d = sccs_findMD5(s, BADKEY)) return (s);

	if (sccs_findMD5(s, GOODKEY)) {
		getMsg("bad-linux-kern-tree", 0, 0, 0, stderr);
		return (0);
	}
	unless (sccs_resum(s, d, 0, 1)) {
		fprintf(stderr, "failed to fix linux checksum error\n");
		return (0);
	}
	sccs_admin(s, 0, NEWCKSUM, 0, 0, 0, 0, 0, 0, 0, 0);
	sccs_restart(s);
	sccs_findKeyDB(s, 0);
	return (s);
}
