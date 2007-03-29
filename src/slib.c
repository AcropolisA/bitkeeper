/*
 * SCCS library - all of SCCS is implemented here.  All the other source is
 * no more than front ends that call entry points into this library.
 * It's one big file so I can hide a bunch of stuff as statics (private).
 *
 * XXX - I don't handle memory allocation failures well.
 *
 * Copyright (c) 1997-2000 Larry McVoy and others.	 All rights reserved.
 */
#include "system.h"
#include "sccs.h"
#include "resolve.h"
#include "bkd.h"
#include "logging.h"
#include "tomcrypt.h"
#include "range.h"

#define	WRITABLE_REG(s)	(WRITABLE(s) && isRegularFile((s)->mode))
private delta	*rfind(sccs *s, char *rev);
private void	dinsert(sccs *s, delta *d, int fixDate);
private int	samebranch(delta *a, delta *b);
private char	*sccsXfile(sccs *sccs, char type);
private int	badcksum(sccs *s, int flags);
private int	printstate(const serlist *state, const ser_t *slist);
private int	delstate(ser_t ser, const serlist *state, const ser_t *slist);
private int	whatstate(const serlist *state);
private int	visitedstate(const serlist *state, const ser_t *slist);
private void	changestate(register serlist *state, char type, int serial);
private serlist *allocstate(serlist *old, int n);
private int	end(sccs *, delta *, FILE *, int, int, int, int);
private void	date(delta *d, time_t tt);
private int	getflags(sccs *s, char *buf);
private sum_t	fputmeta(sccs *s, u8 *buf, FILE *out);
private sum_t	fputdata(sccs *s, u8 *buf, FILE *out);
private int	fflushdata(sccs *s, FILE *out);
private void	putserlist(sccs *sc, ser_t *s, FILE *out);
private ser_t*	getserlist(sccs *sc, int isSer, char *s, int *ep);
private int	hasComments(delta *d);
private int	checkRev(sccs *s, char *file, delta *d, int flags);
private int	checkrevs(sccs *s, int flags);
private int	stripChecks(sccs *s, delta *d, char *who);
private delta*	hashArg(delta *d, char *name);
private delta*	csetFileArg(delta *d, char *name);
private delta*	dateArg(delta *d, char *arg, int defaults);
private delta*	hostArg(delta *d, char *arg);
private delta*	pathArg(delta *d, char *arg);
private delta*	randomArg(delta *d, char *arg);
private delta*	zoneArg(delta *d, char *arg);
private delta*	mergeArg(delta *d, char *arg);
private delta*	sumArg(delta *d, char *arg);
private	void	symArg(sccs *s, delta *d, char *name);
private time_t	getDate(delta *d);
private	int	unlinkGfile(sccs *s);
private int	write_pfile(sccs *s, int flags, delta *d,
		    char *rev, char *iLst, char *i2, char *xLst, char *mRev);
private time_t	date2time(char *asctime, char *z, int roundup);
private char	*time2date(time_t tt);
private	char	*sccsrev(delta *d);
private int	addSym(char *name, sccs *sc, int flags, admin *l, int *ep);
private void	updatePending(sccs *s);
private int	sameFileType(sccs *s, delta *d);
private int	deflate_gfile(sccs *s, char *tmpfile);
private int	isRegularFile(mode_t m);
private void	sccs_freetable(delta *d);
private	delta*	getCksumDelta(sccs *s, delta *d);
private delta	*gca(delta *left, delta *right);
private delta	*gca2(sccs *s, delta *left, delta *right);
private delta	*gca3(sccs *s, delta *left, delta *right, char **i, char **e);
private int	compressmap(sccs *s, delta *d, ser_t *set, int useSer,
			void **i, void **e);
private	void	uniqDelta(sccs *s);
private	void	uniqRoot(sccs *s);
private int	checkGone(sccs *s, int bit, char *who);
private	int	openOutput(sccs*s, int encode, char *file, FILE **op);
private	void	parseConfig(char *buf, MDBM *db);
private	delta	*cset2rev(sccs *s, char *rev);
private	void	taguncolor(sccs *s, delta *d);
private	void	prefix(sccs *s,
		    delta *d, u32 flags, int lines, char *name, FILE *out);

int
emptyDir(char *dir)
{
	char	**d;
	int	i, n = 0;

	unless (d = getdir(dir)) return (0);
	EACH (d) n++;
	freeLines(d, free);
	return (n == 0);
}

/*
 * Convert lrwxrwxrwx -> 0120777, etc.
 */
private mode_t
a2mode(char *mode)
{
	mode_t	m;

	assert(mode && *mode);
	switch (*mode) {
	    case '-': m = S_IFREG; break;
	    case 'd': m = S_IFDIR; break;
	    case 'l': m = S_IFLNK; break;
	    default:
	    	fprintf(stderr, "Unsupported file type: '%c'\n", *mode);
		return (0);
	}
	mode++;
	if (*mode++ == 'r') m |= S_IRUSR;
	if (*mode++ == 'w') m |= S_IWUSR;
	switch (*mode++) {
#ifdef	S_ISUID
	    case 'S': m |= S_ISUID; break;
	    case 's': m |= S_ISUID; /* fall-through */
#endif
	    case 'x': m |= S_IXUSR; break;
	}

	/* group - XXX, inherite these on DOS? */
	if (*mode++ == 'r') m |= S_IRGRP;
	if (*mode++ == 'w') m |= S_IWGRP;
	switch (*mode++) {
#ifdef	S_ISGID
	    case 'S': m |= S_ISGID; break;
	    case 's': m |= S_ISGID; /* fall-through */
#endif
	    case 'x': m |= S_IXGRP; break;
	}

	/* other */
	if (*mode++ == 'r') m |= S_IROTH;
	if (*mode++ == 'w') m |= S_IWOTH;
	if (*mode++ == 'x') m |= S_IXOTH;
	return (m);
}

private mode_t
fixModes(mode_t m)
{
	unless (m & 0200) {
		fprintf(stderr, "Warning: adding owner write permission\n");
		m |= 0200;
	}
	unless (m & 0400) {
		fprintf(stderr, "Warning: adding owner read permission\n");
		m |= 0400;
	}
	return (m);
}

private mode_t
getMode(char *arg)
{
	mode_t	m;

	if (isdigit(*arg)) {
		char	*p = arg;
		for (m = 0; isdigit(*p); m <<= 3, m |= (*p - '0'), p++) {
			unless ((*p >= '0') && (*p <= '7')) {
				fprintf(stderr, "Illegal octal file mode: %s\n",
				    arg);
				return (0);
			}
		}
		if (!S_ISLNK(m) && !S_ISDIR(m)) m |= S_IFREG;
	} else {
		m = a2mode(arg);
	}
	return (fixModes(m));
}

/*
 * chmod [ugoa]+rwxs
 * chmod [ugoa]-rwxs
 * chmod [ugoa]=rwxs
 *
 * Yes, this code knows the values of the bits.  Tough.
 */
private mode_t
newMode(delta *d, char *p)
{
	mode_t	mode, or = 0;
	int	op = 0, setid = 0, u = 0, g = 0, o = 0;

	assert(p && *p);
	if (isdigit(*p) || (strlen(p) == 10)) return (getMode(p));

	for ( ; *p; p++) {
		switch (*p) {
		    case 'u': u = 1; break;
		    case 'g': g = 1; break;
		    case 'o': o = 1; break;
		    case 'a': u = g = o = 1; break;
		    default: goto plusminus;
		}
	}
plusminus:
	unless (u || g || o) u = g = o = 1;
	switch (*p) {
	    case '+': case '-': case '=': op = *p++; break;
	    default: return (0);
	}

	setid = mode = 0;
	while (*p) {
		switch (*p++) {
		    case 'r': or |= 4; break;
		    case 'w': or |= 2; break;
		    case 'x': or |= 1; break;
		    case 's': setid = 1; break;
		    default: return (0);
		}
	}
	if (u) {
		mode |= (or << 6);
		if (setid) mode |= 04000;
	}
	if (g) {
		mode |= (or << 3);
		if (setid) mode |= 02000;
	}
	if (o) mode |= or;
#ifndef	S_ISUID
	mode &= 0777;	/* no setgid if no setuid */
#endif
	switch (op) {
	    case '-':	mode = (d->mode & ~mode); break;
	    case '+':	mode = (d->mode | mode); break;
	    case '=':
		unless (u) mode |= (d->mode & 0700);
		unless (g) mode |= (d->mode & 0070);
		unless (o) mode |= (d->mode & 0007);
		break;
	}
	mode |= (d->mode & S_IFMT);	/* don't lose file type */
	return (fixModes(mode));
}

/* value is overwritten on each call */
char	*
mode2a(mode_t m)
{
	static	char mode[12];
	char	*s = mode;

	if (S_ISLNK(m)) {
		*s++ = 'l';
	} else if (S_ISDIR(m)) {
		*s++ = 'd';
	} else if (S_ISREG(m)) {
		*s++ = '-';
	} else {
	    	fprintf(stderr, "Unsupported mode: '%o'\n", m);
		return ("<bad mode>");
	}
	*s++ = (m & S_IRUSR) ? 'r' : '-';
	*s++ = (m & S_IWUSR) ? 'w' : '-';
#ifndef	S_ISUID
	*s++ = (m & S_IXUSR) ? 'x' : '-';
#else
	*s++ = (m & S_IXUSR) ? ((m & S_ISUID) ? 's' : 'x')
			     : ((m & S_ISUID) ? 'S' : '-');
#endif
	*s++ = (m & S_IRGRP) ? 'r' : '-';
	*s++ = (m & S_IWGRP) ? 'w' : '-';
#ifndef	S_ISGID
	*s++ = (m & S_IXGRP) ? 'x' : '-';
#else
	*s++ = (m & S_IXGRP) ? ((m & S_ISGID) ? 's' : 'x')
			     : ((m & S_ISGID) ? 'S' : '-');
#endif
	*s++ = (m & S_IROTH) ? 'r' : '-';
	*s++ = (m & S_IWOTH) ? 'w' : '-';
	*s++ = (m & S_IXOTH) ? 'x' : '-';
	*s = 0;
	return (mode);
}

char	*
mode2FileType(mode_t m)
{
	if (S_ISREG(m)) {
		return ("FILE");
	} else if (S_ISLNK(m)) {
		return ("SYMLINK");
	} else {
		return ("unsupported file type");
	}
}

/*
 * extract the file type bits from the mode
 */
int
fileType(mode_t m)
{
	return (m & S_IFMT);
}

/*
 * These are the file types we currently suppprt
 * TODO: we may support empty directory & special file someday
 */
int
fileTypeOk(mode_t m)
{
	return ((S_ISREG(m)) || (S_ISLNK(m)));
}

/*
 * Compare up to but not including the newline.
 * They should be newlines or nulls.
 */
#if 0
private int
strnonleq(register char *s, register char *t)
{
	while (*s && *t && (*s == *t) && (*s || (*s != '\n'))) s++, t++;
	return ((!*s || (*s == '\n')) && (!*t || (*t == '\n')));
}
#endif

/*
 * remove last character from string and return it
 * XXX this function is deprecated, pls use chomp() instead.
 */
char
chop(char *s)
{
	char	c;

	assert(s);
	unless (*s) return (0);
	s += strlen(s) - 1;
	c = *s;
	*s = 0;
	return (c);
}


/*
 * Keys are like u@h|path|date|.... whatever
 * We want to skip over any spaces in the path part.
 */
char	*
separator(char *s)
{
	while (s && (*s != '|') && *s) s++;
	unless (s && (*s == '|')) return (0);
	s++;
	while ((*s != '|') && *s) s++;
	unless (*s == '|') return (0);
	return (strchr(s, ' '));
}

/*
 * Convert the pointer into something we can write.
 * We trim the newline since most of the time that's what we want anyway.
 * This pointer points into a readonly mmapping.
 */
char	*
mkline(char *p)
{
	static	char buf[MAXLINE];
	char	*s;

	unless (p) return (0);
	for (s = buf; (*s++ = *p++) != '\n'; );
	s[-1] = 0;
	assert((s - buf) <= MAXLINE);
	return (buf);
}

/*
 * Return the length of the buffer until a newline.
 */
int
linelen(char *s)
{
	char	*t = s;

	while (*t && (*t++ != '\n'));
	return (t-s);
}

#if 0
/*
 * Compare up to and including the newline.  Both have to be on \n to match.
 */
private int
strnleq(register char *s, register char *t)
{
	while (*t == *s) {
		if (!*t || (*t == '\n')) return (1);
		t++, s++;
	}
	return (0);
}
#endif

/*
 * Convert a serial to an ascii string.
 */
void
sertoa(register char *buf, ser_t val)
{
	char	reverse[6];
	int	i, j;

	for (i = 0; val; i++, val /= 10) {
		reverse[i] = '0' + val % 10;
	}
	for (j = 0; i--; ) buf[j++] = reverse[i];
	buf[j] = 0;
}

#define	atoi	myatoi
private int
atoi(register char *s)
{
	register int val = 0;

	if (!s) return (0);
	while (*s && isdigit(*s)) {
		val = val * 10 + *s++ - '0';
	}
	return (val);
}

int
atoi_p(char **sp)
{
	register int val = 0;
	register char *s = *sp;

	if (!s) return (0);
	while (*s && isdigit(*s)) {
		val = val * 10 + *s++ - '0';
	}
	*sp = s;
	return (val);
}

private int
atoiMult_p(char **p)
{
	register int val = 0;
	register char *s = *p;

	if (!s) return (0);
	while (*s && isdigit(*s)) {
		val = val * 10 + *s++ - '0';
	}
	switch (*s) {
	    case 'K': val *= 1000; s++; break;
	    case 'M': val *= 1000000; s++; break;
	    case 'G': val *= 1000000000; s++; break;
	}
	*p = s;
	return (val);
}

/* Free one delta.  */
private void
freedelta(delta *d)
{
	freeLines(d->comments, free);
	freeLines(d->text, free);

	if (d->rev) free(d->rev);
	if (d->user) free(d->user);
	if (d->sdate) free(d->sdate);
	if (d->include) free(d->include);
	if (d->exclude) free(d->exclude);
	if (d->symlink && !(d->flags & D_DUPLINK)) free(d->symlink);
	if (d->hostname && !(d->flags & D_DUPHOST)) free(d->hostname);
	if (d->pathname && !(d->flags & D_DUPPATH)) free(d->pathname);
	if (d->random) free(d->random);
	if (d->zone && !(d->flags & D_DUPZONE)) free(d->zone);
	if (d->csetFile && !(d->flags & D_DUPCSETFILE)) {
		free(d->csetFile);
	}
	if (d->hash) free(d->hash);
	free(d);
}


/*
 * Free the delta tree.
 */
void
sccs_freetree(delta *tree)
{
	if (!tree) return;

	debug((stderr, "freetree(%s %s %d)\n",
	       notnull(tree->rev), notnull(tree->sdate), tree->serial));
	sccs_freetree(tree->siblings);
	sccs_freetree(tree->kid);
	freedelta(tree);
}

/*
 * Free the entire delta table.
 * This follows the ->next pointer and is not recursive.
 */
private void
sccs_freetable(delta *t)
{
	delta *u;

	if (!t) return;

	debug((stderr, "freetable():\n"));
	for(; t; t = u) {
		debug((stderr, "\t%s %s %d\n",
		       notnull(t->rev), notnull(t->sdate), t->serial));
		u = t->next;
		freedelta(t);
	}
}

/*
 * Check a delta for duplicate fields which are normally inherited.
 * Also inherit any fields which are not set in the delta and are set in
 * the parent.
 * Also mark any merged deltas.
 * This routine must be called after the delta is added to a graph which
 * is already correct.	As in dinsert().
 * Make sure to keep this up for all inherited fields.
 */
void
sccs_inherit(sccs *s, delta *d)
{
	delta	*p;

	unless (d) return;
	unless (p = d->parent) {
		DATE(d);
		return;
	}

	/*
	 * For each metadata field, check if not null, dup if null.
	 * If it is already dupped, leave it alone.
	 */
#define	CHK_DUP(field, flag, str) \
	if (d->field) { \
		unless (d->flags & flag) { \
			if (p->field && streq(d->field, p->field)) { \
				free(d->field); \
				d->field = p->field; \
				d->flags |= flag; \
			} \
		} \
	} else if (p->field) { \
		d->field = p->field; \
		d->flags |= flag; \
	}
	CHK_DUP(pathname, D_DUPPATH, "path");
	CHK_DUP(hostname, D_DUPHOST, "host");
	CHK_DUP(zone, D_DUPZONE, "zone");
	CHK_DUP(csetFile, D_DUPCSETFILE, "csetFile");
	if ((p->flags & D_MODE) && !(d->flags & D_MODE)) {
		d->flags |= D_MODE;
		d->mode = p->mode;
		CHK_DUP(symlink, D_DUPLINK, "symlink");
	}
	DATE(d);
	if (d->merge) {
		d = sfind(s, d->merge);
		assert(d);
		d->flags |= D_MERGED;
	}
}

/*
 * Reduplicate the dup information, as it might have changed
 * because of swapping parent and merge pointers (renumber()) or
 * because of D_GONE'ing some interior nodes (csetprune()).
 * The code is here so that any time a new meta data is added that
 * requires dup'ing, that code will be put here as well to unDup it.
 *
 * XXX: this isn't efficient, but is less vulnerable than trying to
 * do this in one step.  Safer to UNDUP in table order, and REDUP
 * in reverse table order.
 */
void
sccs_reDup(sccs *s)
{
	delta	*d, *p;
	int	i;

#define	UNDUP(field, flag, str) \
	if (d->field && (d->flags & flag)) { \
		d->field = strdup(d->field); \
		d->flags &= ~flag; \
	}

	/* undup in forward table order */
	for (d = s->table; d; d = d->next) {
		UNDUP(pathname, D_DUPPATH, "path");
		UNDUP(hostname, D_DUPHOST, "host");
		UNDUP(zone, D_DUPZONE, "zone");
		UNDUP(csetFile, D_DUPCSETFILE, "csetFile");
		UNDUP(symlink, D_DUPLINK, "symlink");
	}
#undef	UNDUP

#define	REDUP(field, flag, str) \
	if (p->field && d->field && streq(p->field, d->field)) { \
		free(d->field); \
		d->field = p->field; \
		d->flags |= flag; \
	}

	/* redup in reverse table order (from sccs_renumber()) */
	for (i = 1; i < s->nextserial; i++) {
		unless (d = sfind(s, i)) continue;
		if (d->flags & D_GONE) continue;
		unless (p = d->parent) continue;

		if (p->flags & D_GONE) {
			/* like an assert, but with more info */
			fprintf(stderr, "Internal error: "
				"Parent %s is GONE "
				"and delta %s is not\n",
				p->rev, d->rev);
			exit (1);
		}
		REDUP(pathname, D_DUPPATH, "path");
		REDUP(hostname, D_DUPHOST, "host");
		REDUP(zone, D_DUPZONE, "zone");
		REDUP(csetFile, D_DUPCSETFILE, "csetFile");
		REDUP(symlink, D_DUPLINK, "symlink");
	}
#undef	REDUP
}

/*
 * Insert the delta in the (ordered) tree.
 * A little weirdness when it comes to removed deltas,
 * we want them off to the side if possible (it makes rfind work better).
 * New in Feb, '99: remove duplicate metadata fields here, maintaining the
 * invariant that a delta in the graph is always correct.
 */
private void
dinsert(sccs *s, delta *d, int fixDate)
{
	delta	*p;

	debug((stderr, "dinsert(%s)", d->rev));
	unless (s->tree) {
		s->tree = d;
		s->lastinsert = d;
		debug((stderr, " -> ROOT\n"));
		if (fixDate) uniqRoot(s);
		return;
	}
	if (d->random) {
		debug((stderr, "GRAFT: %s@%s\n", s->gfile, d->rev));
		s->grafted = 1;
	}
	if (s->lastinsert && (s->lastinsert->serial == d->pserial)) {
		p = s->lastinsert;
		debug((stderr, " (fast)"));
	} else {
		p = sfind(s, d->pserial);
	}
	assert(p);
	s->lastinsert = d;
	d->parent = p;
	assert(!d->kid && !d->siblings);
	if (!p->kid) {
		p->kid = d;
		debug((stderr, " -> %s (kid)\n", p->rev));

	} else if ((p->kid->type == 'D') &&
	    samebranch(p, p->kid)) { /* in right place */
		/*
		 * If there are siblings, add d at the end.
		 */
		if (p->kid->siblings) {
			delta	*l = p->kid->siblings;

			while (l->siblings) l = l->siblings;
			l->siblings = d;
		} else {
			p->kid->siblings = d;
		}
		debug((stderr, " -> %s (sib)\n", p->rev));
	} else {  /* else not in right place, put the new delta there. */
		debug((stderr, "kid type %c, %d.%d.%d.%d vs %d.%d.%d.%d\n",
		    p->kid->type, p->r[0], p->r[1], p->r[2], p->r[3],
		    p->kid->r[0], p->kid->r[1], p->kid->r[2], p->kid->r[3]));
		d->siblings = p->kid;
		p->kid = d;
		debug((stderr, " -> %s (kid, moved sib %s)\n",
		    p->rev, d->siblings->rev));
	}
	sccs_inherit(s, d);
	if (fixDate) {
		uniqDelta(s);
	}
}

/*
 * Find the delta referenced by the serial number.
 */
delta *
sfind(sccs *s, ser_t serial)
{
	delta	*t;

	assert(serial <= s->nextserial);
	if (serial >= s->ser2dsize) goto realloc;
	if (s->ser2delta && s->ser2delta[serial]) return (s->ser2delta[serial]);
	if (s->ser2delta) {
		for (t = s->table; t; t = t->next) {
			if (t->serial == serial) {
				assert(serial < s->ser2dsize);
				s->ser2delta[serial] = t;
				return (t);
			}
		}
		return (0);
	}

realloc:
	if (s->ser2delta) free(s->ser2delta);
	/* We leave a little extra room for sccs_delta. */
	s->ser2dsize = s->nextserial+10;
	s->ser2delta = calloc(s->ser2dsize, sizeof(delta*));
	for (t = s->table; t; t = t->next) {
		if (t->serial >= s->ser2dsize) {
			fprintf(stderr, "%s %d %d\n",
			    s->sfile, t->serial, s->ser2dsize);
		}
		assert(t->serial < s->ser2dsize);
		s->ser2delta[t->serial] = t;
	}
	return (s->ser2delta[serial]);
}

/*
 * An array, indexed by years after 1971, which gives the seconds
 * at the beginning of that year.  Valid up to around 2038.
 * The first value is the time value for 1970-01-01-00:00:00 .
 *
 * Do NOT NOT NOT change this after shipping, even if it is wrong.
 *
 * Bummer.  I made the same mistake in generating this table (passing in
 * 70 intstead of 1970 to the leap your calculation) so all entries
 * after 2000 were wrong.
 * What we'll do is rev the file format so that old binaries
 * won't be able to create deltas with bad time stamps.
 */
static const time_t  yearSecs[] = {
	     0,   31536000,   63072000,   94694400,  126230400,  157766400,
     189302400,  220924800,  252460800,  283996800,  315532800,  347155200,
     378691200,  410227200,  441763200,  473385600,  504921600,  536457600,
     567993600,  599616000,  631152000,  662688000,  694224000,  725846400,
     757382400,  788918400,  820454400,  852076800,  883612800,  915148800,
     946684800,  978307200, 1009843200, 1041379200, 1072915200, 1104537600,
    1136073600, 1167609600, 1199145600, 1230768000, 1262304000, 1293840000,
    1325376000, 1356998400, 1388534400, 1420070400, 1451606400, 1483228800,
    1514764800, 1546300800, 1577836800, 1609459200, 1640995200, 1672531200,
    1704067200, 1735689600, 1767225600, 1798761600, 1830297600, 1861920000,
    1893456000, 1924992000, 1956528000, 1988150400, 2019686400, 2051222400,
    2082758400, 2114380800, 0 };

/*
 * An array, indexed by the month which we are in, which gives the
 * number of seconds in all of the preceeding months of that year.
 * The index is 0 for Jan (which will return 0) and 12 for Dec
 * This is not adjusted for leap years.
 */
#define	DSECS	(24*60*60)
static const int monthSecs[13] = {
    0,		31*DSECS,  59*DSECS,  90*DSECS,
    120*DSECS,	151*DSECS, 181*DSECS, 212*DSECS,
    243*DSECS,	273*DSECS, 304*DSECS, 334*DSECS,
    365*DSECS };

static const char days[13] =
{ 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

int
leapYear(int year)
{
	return ((!((year) % 4) && ((year) % 100)) || !((year) % 400));
}

/*
 * A version of mktime() which uses the fields
 *	year, mon, hour, min, sec
 * and assumes utc time.
 */
time_t
tm2utc(struct tm *tp)
{
	time_t	t;

	if (tp->tm_year < 70) return (1);
	if (tp->tm_year >= 138) {
		fprintf(stderr,
		    "tm2utc: bad time structure: tm_year = %d\n", tp->tm_year);
		return  (0);
	}
	t = yearSecs[tp->tm_year - 70];
	t += monthSecs[tp->tm_mon];
	if ((tp->tm_mon > 1) && leapYear(1900 + tp->tm_year)) {
		t += DSECS;
	}
	t += ((tp->tm_mday - 1)* DSECS);
	t += (tp->tm_hour * 60 * 60);
	t += (tp->tm_min * 60);
	t += tp->tm_sec;
	return (t);
}

struct	tm *
utc2tm(time_t t)
{
	int	leap;
	int	tmp, i;
	static	struct tm tm;

	for (tmp = 70, i = 0; yearSecs[i+1]; i++, tmp++) {
		if (t < yearSecs[i+1]) break;
	}
	bzero(&tm, sizeof(tm));
	tm.tm_year = tmp;
	t -= yearSecs[i];
	leap = leapYear(1900 + tmp);
	/* Jan = 0, Feb = 1, ... */
	for (i = 0; i < 12; ++i) {
		/* [2] means tmp == end of january */
		tmp = monthSecs[i+1];
		if (leap && ((i+1) >= 2)) tmp += DSECS;

		/* if seconds < end of the month */
		if (t < tmp) {
			/* 0 = Jan, 1 = Feb, etc */
			tm.tm_mon = i;
			tmp = monthSecs[i];
			if (leap && (i >= 2)) tmp += DSECS;
			t -= tmp;
			break;
		}
	}
	tm.tm_mday = (t / DSECS) + 1; t -= DSECS * (tm.tm_mday - 1);
	assert(tm.tm_mday > 0);
	tm.tm_hour = t / 3600; t -= tm.tm_hour * 3600;
	tm.tm_min = t / 60; t -= tm.tm_min * 60;
	tm.tm_sec = t;
	return (&tm);
}

private time_t
getDate(delta *d)
{
	if (!d->date) {
		d->date = date2time(d->sdate, d->zone, EXACT);
		if (d->dateFudge) {
			d->date += d->dateFudge;
		}
	}
	CHKDATE(d);
	return (d->date);
}

time_t
sccs_date2time(char *date, char *zone)
{
	return (date2time(date, zone, EXACT));
}

/*
 * The prev pointer is a more recent delta than this one,
 * so make sure that the prev date is > that this one.
 */
private void
fixDates(delta *prev, delta *d)
{
	DATE(d);

	/* recurse forwards first */
	if (d->next) fixDates(d, d->next);

	/* When we get here, we're done. */
	unless (prev) return;

	if (prev->date <= d->date) {
		int	f = (d->date - prev->date) + 1;
		prev->dateFudge += f;
		prev->date += f;
	}
}

void
sccs_fixDates(sccs *s)
{
	fixDates(0, s->table);
}

char	*
age(time_t when, char *space)
{
	int	i;
	static	char buf[100];

#define	DOIT(SMALL, BIG, UNITS) \
	if (when <= 3*BIG) {		/* first 3 BIG as SMALL */	\
		for (i = 0; i <= 3*BIG; i += SMALL) {			\
			if (when <= i) {				\
				sprintf(buf, 				\
				    "%d%s%s", i/SMALL, space, UNITS);	\
				if (i/SMALL == 1) chop(buf);		\
				return (buf);				\
			}						\
		}							\
	}

	DOIT(HOUR, DAY, "hours");		/* first 3 days as hours */
	DOIT(DAY, WEEK, "days");		/* first 3 weeks as days */
	DOIT(WEEK, MONTH, "weeks");		/* first 3 months as days */
	DOIT(MONTH, YEAR, "months");		/* first 3 years as months */
	DOIT(YEAR, DECADE, "years");		/* first 3 decades as years */
	return ("more than 30 years");
}

private void
uniqRoot(sccs *s)
{
	delta	*d;
	char	buf[MAXPATH+100];

	assert(s->tree == s->table);
	d = s->tree;
	DATE(d);

	unless (uniq_open() == 0) return;	// XXX - no error?
	sccs_shortKey(s, sccs_ino(s), buf);
	while (!unique(buf)) {
//fprintf(stderr, "COOL: caught a duplicate root: %s\n", buf);
		d->dateFudge++;
		d->date++;
		sccs_shortKey(s, d, buf);
	}
	uniq_update(buf, d->date);
	uniq_close();
	return;
}

/*
 * Fix the date in a new delta.
 * Make sure date is increasing
 */
private void
uniqDelta(sccs *s)
{
	delta	*next, *d;
	char	buf[MAXPATH+100];

	assert(s->tree != s->table);
	d = s->table;
	next = d->next;
	assert(d != s->tree);
	DATE(d);

	/*
	 * This is kind of a hack.  We aren't in BK mode yet we are fudging.
	 * It keeps BK happy, I guess.
	 */
	unless (BITKEEPER(s)) {
		unless (next = d->next) return;
		if (next->date >= d->date) {
			time_t	tdiff;
			tdiff = next->date - d->date + 1;
			d->date += tdiff;
			d->dateFudge += tdiff;
		}
		return;
	}

	unless (uniq_open() == 0) return;
	CHKDATE(next);
	if (d->date <= next->date) {
		time_t	tdiff;
		tdiff = next->date - d->date + 1;
		d->date += tdiff;
		d->dateFudge += tdiff;
	}

	/*
	 * We want the import convertor to produce the same tree
	 * when ran multiple times. Do not enforce unique key
	 * across different repository; 
	 */
	if (IMPORT(s)) {
		uniq_close();
		return;
	}

	sccs_shortKey(s, d, buf);
	while (!unique(buf)) {
//fprintf(stderr, "COOL: caught a duplicate key: %s\n", buf);
		d->date++;
		d->dateFudge++;
		sccs_shortKey(s, d, buf);
	}
	uniq_update(buf, d->date);
	uniq_close();
}

private int
monthDays(int year, int month)
{
	if (month != 2) return (days[month]);
	if (leapYear(year)) return (29);
	return (28);
}

private void
a2tm(struct tm *tp, char *asctime, char *z, int roundup)
{
	int	i, tmp;

#define	gettime(field) \
	if (isdigit(asctime[1])) { \
		tp->field = ((asctime[0] - '0') * 10 + (asctime[1] - '0')); \
		asctime += 2; \
	} else { \
		tp->field = *asctime++ - '0'; \
	} \
	for (; *asctime && !isdigit(*asctime); asctime++);

	/* this is weird, but all I care about here is up or !up */
	roundup = (roundup == ROUNDUP);
	bzero(tp, sizeof(*tp));
	tp->tm_mday = 1;
	if (roundup) {
		tp->tm_mday = 31;
		tp->tm_mon = 11;
		tp->tm_hour = 23;
		tp->tm_min = tp->tm_sec = 59;
	}

	/*
	 * We're moving towards always having 4 digit years but we still want
	 * to support, for now, 2 digit years and 4 digit years without 
	 * requiring a non-digit separator.  So here's how that works:
	 *
	 * If the first 2 digits are in the 69..18 range then we are going
	 * see that as 1969..2018.  And before 2019 we need to have dropped
	 * support for 2 digit years.  The reason for 1969 is imports which
	 * might have gone back far (and teamware grafted files w/ 1970 dates).
	 * Otherwise it's a 4 digit year.  
	 */
	for (i = tmp = 0; (i < 4) && isdigit(*asctime); i++) {
		/* I want the increment here because of the break below */
		tmp = tmp * 10 + (*asctime++ - '0');

		/* we want 69..99 or 0..18 and this does that */
		if ((i == 1) && ((tmp >= 69) || (tmp <= 18))) break;
    	}
	tp->tm_year = tmp;
	for (; *asctime && !isdigit(*asctime); asctime++);

	/*
	 * We want a value between 69 and 138 which covers the time between
	 * 1969 and 2038
	 */
	if (tp->tm_year < 69) tp->tm_year += 100;	/* we're 2000's */
	if (tp->tm_year >= 1969) tp->tm_year -= 1900;	/* 4 digit year */
	unless (*asctime) goto correct;

	/* tm_mon counts 0..11; ASCII is 1..12 */
	gettime(tm_mon); tp->tm_mon--; unless (*asctime) goto correct;
	gettime(tm_mday); unless (*asctime) goto correct;
	gettime(tm_hour); unless (*asctime) goto correct;
	gettime(tm_min); unless (*asctime) goto correct;
	gettime(tm_sec);
	if (((asctime[-1] == '-') || (asctime[-1] == '+')) && !z) z = --asctime;

correct:
	/* Correct for dates parsed as pre-epoch because of missing tz */
	if (tp->tm_year == 69) {
		bzero(tp, sizeof(*tp));
		z = 0;
	}
	
	/*
	 * Truncate down oversized fields.
	 */
	if (tp->tm_mon > 11) tp->tm_mon = 11;
	i = monthDays(1900 + tp->tm_year, tp->tm_mon + 1);
	if (i < tp->tm_mday) tp->tm_mday = i;
	if (tp->tm_hour > 23) tp->tm_hour = 23;
	if (tp->tm_min > 59) tp->tm_min = 59;
	if (tp->tm_sec > 59) tp->tm_sec = 59;

	if (z) {
		int	sign;

		if ((sign = (*z == '-'))) {
			z++;
			sign = 1;
		} else {
			sign = -1;	/* this is what I want */
		}
		if (*z == '+') z++;
		tp->tm_hour += atoi(z) * sign;
		while (*z++ != ':');
		tp->tm_min += atoi(z) * sign;
	}
}

/*
 * Take 93/07/25 21:14:11 and return a time_t that is useful.  Note that
 * "useful" means useful to BitKeeper, not anyone else.
 * For years < 69, assume 20xx which means add 100.
 *
 * roundup is ROUNDDOWN, EXACT, or ROUNDUP.  Which do the implied
 * adjustments for the unspecified fields.  Fields which are incorrectly
 * specified, i.e., 31 for a month that has 30 days, are truncated back
 * down to legit values.
 *
 * This is a little weird because tm_ does not translate directly from
 * yy/mm/dd hh:mm:ss - see mktime(3) for details.
 */
private time_t
date2time(char *asctime, char *z, int roundup)
{
	struct	tm tm;

	a2tm(&tm, asctime, z, roundup);
#if	0
{	struct  tm tm2 = tm;
	struct  tm *tp;
	fprintf(stderr, "%s%s %04d/%02d/%02d %02d:%02d:%02d = %u = ",
	asctime,
	z ? z : "",
	tm.tm_year + 1900,
	tm.tm_mon + 1,
	tm.tm_mday,
	tm.tm_hour,
	tm.tm_min,
	tm.tm_sec,
	tm2utc(&tm2));
	tp = utc2tm(tm2utc(&tm2));
	fprintf(stderr, "%04d/%02d/%02d %02d:%02d:%02d\n",
	tp->tm_year + 1900,
	tp->tm_mon + 1,
	tp->tm_mday,
	tp->tm_hour,
	tp->tm_min,
	tp->tm_sec);
}
#endif
	return (tm2utc(&tm));
}

/*
 * Set the sfile so that it is older than any checked out associated gfile.
 * The -2 adjustments are because FAT file systems have a 2 second granularity.
 */
int
sccs_setStime(sccs *s, time_t newest)
{
	struct	utimbuf	ut;
	delta	*d;

	/* If we have no deltas we don't know what time it is */
	unless (s && s->table) return (0);

	/*
	 * To prevent the "make" command from doing a "get" due to 
	 * sfile's newer modification time, and then fail due to the
	 * editable gfile, adjust sfile's modification to be just
	 * before that of gfile's.
	 * Note: It is ok to do this, because we've already recorded
	 * the time of the delta in the delta table.
	 * A potential pitfall would be that it may confuse the backup
	 * program to skip the sfile when doing a incremental backup.
	 */
	ut.actime = time(0);

	/*
	 * Use the most recent "real" delta for the timestamp.
	 * We're skipping over TAGs or xflag changes because those don't
	 * modify the gfile.
	 */
	for (d = s->table; d; d = d->next) {
		if (d->merge) break;
		unless (TAG(d) || (d->flags & D_XFLAGS)) break;
	}
	unless (d) d = s->tree;		/* 1.0 has XFLAGS, so its skipped */
	ut.modtime = d->date - d->dateFudge - 2;

	/*
	 * In checkout:edit mode bk delta is like bk delta -l; so it will
	 * not touch the gfile.  So now s->table->date > gtime.
	 * We look for that and adjust: this will only roll back
	 */
	if (s->gtime && (s->gtime < (ut.modtime + 2))) {
		ut.modtime = s->gtime - 2;
	}
	/*
	 * if hard linked clone, and doing a get -T, don't pull the
	 * sfile modification time forward as that may mess things up
	 * in some other hardlinked clone which may have an older gfile.
	 */
	if (newest && (ut.modtime >= newest)) return (0);
	if (ut.modtime > (ut.actime - 2)) {
		ut.modtime = ut.actime - 2;	/* clamp to now */
	}
	return (utime(s->sfile, &ut));
}

/*
 * Diff can give me
 *	10a12, 14	-> 10 a
 *	10, 12d13	-> 10 d
 *	10, 12c12, 14	-> 10 c
 *
 * Diff -n can give me
 *	a10 2		-> 10 b 2
 *	d10 2		-> 10 e 2
 *
 * Mkpatch can give me
 *	I10 2		-> 10 I 2
 *	D10 2		-> 10 D 2
 */
private inline int
scandiff(char *s, int *where, char *what, int *howmany)
{
	if (!isdigit(*s)) { /* look for diff -n and mkpatch format */
		*what = *s;
		if (*s == 'a')
			*what = 'i';
		else if (*s == 'd')
			*what = 'x';
		else unless (*s == 'D' || *s == 'I' || *s == 'N')
			return (-1);
		s++;
		*where = atoi_p(&s);
		unless (*s == ' ')  return (-1);
		s++;
		*howmany = atoi_p(&s);
		return (0);
	}
	*howmany = 0;	/* not used by this part, but used by nonl fixer */
	*where = atoi_p(&s);
	if (*s == ',') {
		s++;
		(void)atoi_p(&s);
	}
	if (*s != 'a' && *s != 'c' && *s != 'd') {
		return (-1);
	}
	*what = *s;
	return (0);
}

/*
 * Convert ascii 1.2.3.4 -> 1, 2, 3, 4
 */
private int
scanrev(char *s, ser_t *a, ser_t *b, ser_t *c, ser_t *d)
{
	char	*p;

	/* make sure it's well formed or take nothing. */
	for (p = s; isdigit(*p) || (*p == '.'); p++);
	if (*p) return (0);

	*a = atoi_p(&s);
	if (b && *s == '.') {
		s++;
		*b = atoi_p(&s);
		if (c && *s == '.') {
			s++;
			*c = atoi_p(&s);
			if (d && *s == '.') {
				s++;
				*d = atoi(s);
				return (4);
			} else return (3);
		} else return (2);
	} else return (1);
}

private void
explode_rev(delta *d)
{
	register char *s = d->rev;
	int	dots = 0;

	while (*s) { if (*s++ == '.') dots++; }
	if (dots > 3) d->flags |= D_ERROR|D_BADFORM;
	switch (scanrev(d->rev, &d->r[0], &d->r[1], &d->r[2], &d->r[3])) {
	    case 1: d->r[1] = 0;	/* fall through */
	    case 2: d->r[2] = 0;	/* fall through */
	    case 3: d->r[3] = 0;
	}
}

private char *
sccsrev(delta *d)
{
	return (d->rev);
}

/*
 * Return true if you can get to p by work upwards from d
 */
private int
ancestor(sccs *s, delta *d, delta *p)
{
	delta	*e;

	unless (d) return (0);
	unless (d->serial >= p->serial) return (0);

	/* try the easy path first */
	for (e = d; e; e = e->parent) {
		if (e == p) return (1);
	}

	/* walk the merge parents */
	for (e = d; e; e = e->parent) {
		if (e->merge && ancestor(s, sfind(s, e->merge), p)) return (1);
	}
	return (0);
}

/*
 * This one assumes SCCS style branch numbering, i.e., x.y.z.d
 */
private int
samebranch(delta *a, delta *b)
{
	if (!a->r[2] && !b->r[2]) return (1);
	return ((a->r[0] == b->r[0]) &&
		(a->r[1] == b->r[1]) &&
		(a->r[2] == b->r[2]));
}

private char *
branchname(delta *d)
{
	ser_t	a1 = 0, a2 = 0, a3 = 0, a4 = 0;
	static	char buf[6];

	scanrev(d->rev, &a1, &a2, &a3, &a4);
	if (a3) {
		sprintf(buf, "%d", a3);
	} else {
		sprintf(buf, "0");
	}
	return (buf);
}

/*
 * XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 * All of this pathname/changeset shit needs to be reworked.
 * XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 */

void
sccs_mkroot(char *path)
{
	char	buf[MAXPATH];

	sprintf(buf, "%s/SCCS", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/etc", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/etc/SCCS", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/deleted", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/deleted/SCCS", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/tmp", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/BitKeeper/log", path);
	if ((mkdir(buf, 0777) == -1) && (errno != EEXIST)) {
		perror(buf);
		exit(1);
	}
}

/*
 * Reverse the effect of sccs_mkroot
 */
void
sccs_unmkroot(char *path)
{
	char	buf[MAXPATH];

	sprintf(buf, "%s/SCCS", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}

	sprintf(buf, "%s/BitKeeper/etc/SCCS", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper/etc", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper/deleted/SCCS", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper/deleted", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper/tmp", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper/log", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
	sprintf(buf, "%s/BitKeeper", path);
	if (rmdir(buf) == -1) {
		perror(buf);
	}
}

/*
 * Return the pathname relative to the first ChangeSet file found.
 *
 * XXX - this causes a 5-10% slowdown, more if the tree is very deep.
 * I need to cache changeset lookups.
 */
char	*
_relativeName(char *gName, int isDir, int mustHaveRmarker, int wantRealName,
    project *proj)
{
	char	*t, *s;
	char	*root;
	int	len;
	project	*freeproj = 0;
	char	tmp[MAXPATH];
	static  char buf2[MAXPATH];

	strcpy(tmp, fullname(gName));
	unless (IsFullPath(tmp)) return (0);
	tmp[strlen(tmp)+1] = 0;	/* for code below */
	t = tmp;

	unless (proj) proj = freeproj = proj_init(t);
	unless (proj) {
		if (mustHaveRmarker) return (0);
		strcpy(buf2, tmp);
		return (buf2);
	}

	root = proj_root(proj);
	len = strlen(root);
	assert(strneq(root, t, len));
	if (freeproj) proj_free(freeproj);
	s = &t[len];
	assert((*s == '\0') || (*s == '/'));

	/*
	 * Must cd to project root before we call getRealName()
	 */
	t[s-t] = 0; /* t now points to project root */
	if (wantRealName && proj_isCaseFoldingFS(proj)) {
		char here[MAXPATH];

		fast_getcwd(here, MAXPATH);
		if (streq(here, t)) {
			getRealName(++s, NULL, buf2);
		} else if (chdir(t) == 0) {
			getRealName(++s, NULL, buf2);
			chdir(here);	/* restore pwd */
		} else {
			/*
			 * If chdir() filed
			 * skip getRealName()
			 */
			strcpy(buf2, ++s);
		}
	} else {
		strcpy(buf2, ++s);
	}

	if (isDir) {
		if (buf2[0] == 0) strcpy(buf2, ".");
		return (buf2);
	}
	return(buf2);
}


/*
 * Trim off the RESYNC/ part of the pathname, that's garbage.
 */
private char	*
relativeName(sccs *sc, int mustHaveRmarker)
{
	char	*s, *g;

	g = sccs2name(sc->sfile);
	s = _relativeName(g, 0, mustHaveRmarker, 1, sc->proj);
	free(g);
	unless (s) return (0);

	if (strncmp("RESYNC/", s, 7) == 0) s += 7;
	return (s);
}

private inline symbol *
findSym(symbol *s, char *name)
{
	symbol	*sym;

	unless (name) return (0);
	for (sym = s; sym; sym = sym->next) {
		if (sym->symname && streq(name, sym->symname)) return (sym);
	}
	return (0);
}

/*
 * Revision number/branching theory of operation:
 *	rfind() does an exact match - use this if you want a specific delta.
 *	findrev() does an inexact match - it will find top of trunk for the
 *		trunk or a branch if it is specified as -r1 or -r1.2.1.
 *		Note that -r1 will not find tot if tot is 2.1; use a null
 *		rev to get that.
 *	sccs_getedit() finds the revision and gets the new revision number for a
 *		new delta.  The find logic is findrev().  The new revision
 *		will be the next available on that line, unless there is
 *		a conflict or the forcebranch flag is set.  In either of
 *		those cases, the new revision will be a branch.
 *		If the revision is on the trunk and they wanted to bump
 *		the release, do so.
 */
private int
name2rev(sccs *s, char **rp)
{
	char	*rev = *rp;
	symbol	*sym;

	if (!rev) return (0);
	if (isdigit(rev[0])) return (0);

again:
	for (sym = s->symbols; sym; sym = sym->next) {
		if (sym->symname && streq(rev, sym->symname)) {
			rev = sym->rev;
			if (isdigit(rev[0])) {
				*rp = rev;
				return (0);
			}
			goto again;
		}
	}
	return (-1);
}

private inline int
samerev(ser_t a[4], ser_t b[4])
{
	return ((a[0] == b[0]) &&
		(a[1] == b[1]) &&
		(a[2] == b[2]) &&
		(a[3] == b[3]));
}

private ser_t	R[4];
/*
 * This one uses the globals set up below.  This hack makes this library
 * !MT safe.  Which it wasn't anyway.
 */
private delta *
_rfind(delta *d)
{
	delta	*t;

	debug((stderr,
	    "\t_rfind(%d.%d.%d.%d)\n", d->r[0], d->r[1], d->r[2], d->r[3]));
	if (samerev(d->r, R) && !TAG(d)) {
		return (d);
	}
	if (d->kid && (t = _rfind(d->kid))) {
		return (t);
	}
	if (d->siblings && (t = _rfind(d->siblings))) {
		return (t);
	}
	return (0);
}

/*
 * Find the delta referenced by rev.  It must be an exact match.
 */
private delta *
rfind(sccs *s, char *rev)
{
	delta	*d;

	debug((stderr, "rfind(%s) ", rev));
	name2rev(s, &rev);
	R[0] = R[1] = R[2] = R[3] = 0;
	scanrev(rev, &R[0], &R[1], &R[2], &R[3]);
	debug((stderr, "aka %d.%d.%d.%d\n", R[0], R[1], R[2], R[3]));
	d = _rfind(s->tree);
	debug((stderr, "rfind(%s) got %s\n", rev, d ? d->rev : "Not found"));
	return (d);
}

private char *
defbranch(sccs *s)
{
	if (BITKEEPER(s)) return ("1");
	if (s->defbranch) return (s->defbranch);
	return ("65535");
}

/*
 * Find the specified delta.  The rev can be
 *	(null)	get top of the default branch
 *	d	get top of trunk for that release
 *	d.d	get that revision or error if not there
 *	d.d.d	get top of branch that matches those three digits
 *	d.d.d.d get that revision or error if not there.
 */
private delta *
findrev(sccs *s, char *rev)
{
	ser_t	a = 0, b = 0, c = 0, d = 0;
	ser_t	max = 0;
	delta	*e = 0, *f = 0;
	char	buf[20];

	debug((stderr,
	    "findrev(%s in %s def=%s)\n",
	    notnull(rev), s->sfile, defbranch(s)));
	unless (HASGRAPH(s)) return (0);
	if (!rev || !*rev || streq("+", rev)) {
		rev = defbranch(s);
	}

	/* 1.0 == s->tree even if s->tree is 1.1 */
	if (streq(rev, "1.0")) return (s->tree);

	if (*rev == '=') {
		e = sfind(s, atoi(++rev));
		unless (e) fprintf(stderr, "Serial %s not found\n", rev);
		return (e);
	}
	if (name2rev(s, &rev)) return (0);
	switch (scanrev(rev, &a, &b, &c, &d)) {
	    case 1:
		/* XXX: what does -r0 mean?? */
		unless (a) {
			fprintf(stderr, "Illegal revision 0\n");
			debug((stderr, " BAD rev %s\n", e->rev));
			return (0);
		}
		/* get max X.Y that is on same branch or tip of biggest */
		for (e = s->table; e; e = e->next) {
			if (e->flags & D_GONE) continue;
			if (e->type != 'D'
			    || (e->r[2] != 0)
			    || (e->r[0] > a))  continue;

			if (e->r[0] == a) break; /* first is it! */

			/* else save max of lesser releases */
			if (e->r[0] > max) {
				f = e;
				max = e->r[0];
			}
		}
		unless (e) e = f;	/* can't find, use max of lesser */
		debug((stderr, "findrev(%s) =  %s\n", rev, e ? e->rev: ""));
		return (e);
	    case 2:
	    case 4:
		e = rfind(s, rev);	/* well formed */
		debug((stderr,
		    "findrev(%s) =  %s\n", rev, e ? e->rev : "Not found"));
		return (e);
	    case 3:
		/* XXX - should this be a while loop, decrementing the
		 * third digit until it find it???  Nahh.
		 */
		sprintf(buf, "%s.1", rev);
		unless (e = rfind(s, buf)) { /* well formed */
			debug((stderr, "findrev(%s) =  Not found\n", rev));
			return (0);
		}
		for (; e->kid && e->kid->type == 'D' && samebranch(e, e->kid);
		    e = e->kid)
			;
		debug((stderr, "findrev(%s) =  %s\n", rev, e->rev));
		return (e);
	    default:
		fprintf(stderr, "Malformed revision: %s\n", rev);
		debug((stderr, " BAD %s\n", e->rev));
		return (0);
	}
}

/*
 * Return the top of the default branch.
 *
 * XXX - this could be much faster in big files by starting at the table
 * end and returning the first real delta which matches the default branch.
 */
delta	*
sccs_top(sccs *s)
{
	return (findrev(s,0));
}

/*
 * Compare two pathnames relative to the root of the repository and
 * return true if they are the same.  Either gfiles or sfiles can
 * be used.
 *
 * This is useful since some old repos have a sfile pathname stored
 * in d->pathname.
 */
int
sccs_patheq(char *file1, char *file2)
{
	int	s1, s2;
	int	ret;

	unless (ret = streq(file1, file2)) {
		/*
		 * if the files don't match check to see if one is a sfile
		 * and the other isn't
		 */
		s1 = (strstr(file1, "SCCS/s.") != 0);
		s2 = (strstr(file2, "SCCS/s.") != 0);
		if (s1 ^ s2) {
			/* normalize the mismatch case */
			file1 = name2sccs(file1);
			file2 = name2sccs(file2);
			ret = streq(file1, file2);
			free(file1);
			free(file2);
		}
	}
	return (ret);
}

delta	*
sccs_findrev(sccs *s, char *rev)
{
	delta	*d;

	unless (rev && *rev) return (findrev(s, 0));
again:	if (rev[0] == '@') {
		if (rev[1] == '@') {
			d = 0;
		} else if (CSET(s)) {
			++rev;
			goto again;
		} else {
			d = cset2rev(s, rev+1);
		}
	} else if (isKey(rev)) {
		d = sccs_findKey(s, rev);
	} else {
		d = findrev(s, rev);
	}
	return (d);
}

/*
 * Find the first delta that is > date.
 *
 * XXX - if there are two children, this finds only one of them.
 */
private delta *
findDate(delta *d, time_t date)
{
	time_t	d2;
	delta	*tmp, *tmp2;

	if (!d) return (0);
	d2 = DATE(d);
	if (d2 >= date) return (d);
	for (tmp = d->kid; tmp; tmp = tmp->siblings) {
		if (samebranch(tmp, d) && (tmp2 = findDate(tmp, date))) {
			return (tmp2);
		}
	}
	return (0);
}

/*
 * Take a date and return the delta closest to that date.
 *
 * The roundup parameter determines how to round inexact dates (ie 2001)
 * and which direction to look for deltas near that date.
 */
delta *
sccs_findDate(sccs *sc, char *s, int roundup)
{
	delta	*tmp, *d = 0;
	time_t	date;
	ser_t	x;

	unless (sc && sc->table) return (0);
	assert(s);
	assert((roundup == ROUNDUP) || (roundup == ROUNDDOWN));

	/*
	 * If it is a symbol, revision, or key,
	 * then just go get that delta and return it.
	 */
	if ((!isdigit(*s) && (*s != '-')) ||	/* not a date */
	    (scanrev(s, &x, &x, &x, &x) > 1) ||
	    isKey(s)) {
		return (sccs_findrev(sc, s));
		/* XXX what if findrev fails... */
	}

	/*
	 * It's a plain date.  Convert it to a number and then go
	 * find the closest delta.  If there is an exact match,
	 * return that.
	 * Depending on which call this, we think a little differently about
	 * endpoints.
	 * The order returned is rstart == 1.1 and rstop == 1.5, i.e.,
	 * oldest delta .. newest.
	 */
	if (*s == '-') {
		date = range_cutoff(s+1);
	} else {
		date = date2time(s, 0, roundup);
	}

	/* Walking the table newest .. oldest order */
	for (tmp = 0, d = sc->table; d; tmp = d, d = d->next) {
		if (TAG(d)) continue;
		if (d->date == date) return (d);
		/*
		 *                v date
		 * big date   1.4   1.3   1.2    little date
		 *             ^tmp  ^d
		 */
		if (d->date < date) break;
	}
	if (roundup == ROUNDDOWN) return (tmp);
	return (d);
}

void
delete_cset_cache(char *rootpath, int save)
{
	char	**files;
	char	**keep = 0;
	int	i;
	char	*p;
	char	buf[MAXPATH];

	/* delete old caches here, keep newest */
	sprintf(buf, "%s/BitKeeper/tmp", rootpath);
	files = getdir(buf);
	p = buf + strlen(buf);
	*p++ = '/';
	EACH (files) {
		if (strneq(files[i], "csetcache.", 10)) {
			struct	stat statbuf;
			strcpy(p, files[i]);
			lstat(buf, &statbuf);
			/* invert time to get newest first */
			keep = addLine(keep,
			    aprintf("%08x %s", (u32)~statbuf.st_atime, buf));
		}
	}
	freeLines(files, free);
	sortLines(keep, 0);
	EACH (keep) {
		if (i > save) {
			p = strchr(keep[i], ' ');
			unlink(p+1);
		}
	}
	freeLines(keep, free);
}

private delta *
cset2rev(sccs *s, char *rev)
{
	static	struct	stat	csetstat = {0};
	char	*rootpath;
	char	*mpath = 0;
	MDBM	*m = 0;
	delta	*ret = 0;
	char	*s_cset = 0;
	char	*deltakey;
	char	rootkey[MAXKEY];

	/*
	 * Sleazy little hack to optimize a common case of most recent
	 * committed delta.
	 */
	if (streq(rev, "+")) {
		for (ret = s->table;
		    ret && (TAG(ret) || !(ret->flags & D_CSET));
		    ret = ret->next);
		return (ret);
	}

	unless (rootpath = proj_root(0)) goto ret;

	/*  stat cset file once per process */
	unless (csetstat.st_mtime) {
		s_cset = aprintf("%s/" CHANGESET, rootpath);
		if (lstat(s_cset, &csetstat)) goto ret;
	}
	mpath = aprintf("%s/BitKeeper/tmp/csetcache.%x", rootpath,
	    (u32)adler32(0, rev, strlen(rev)));
	if (exists(mpath) &&
	    (m = mdbm_open(mpath, O_RDONLY, 0600, 0))) {
		/* validate it still matches cset file */
		char	*x;

		if (!(x = mdbm_fetch_str(m, "STAT")) ||
		    strtoul(x, &x, 16) != (unsigned long)csetstat.st_mtime ||
		    strtoul(x, 0, 16) != (unsigned long)csetstat.st_size ||
		    !(x = mdbm_fetch_str(m, "REV")) ||
		    !streq(x, rev)) {
			mdbm_close(m);
			unlink(mpath);
			m = 0;
		}
	}
	unless (m) {
		sccs	*sc;
		MDBM	*csetm;
		char	buf[20];

		/* fetch MDBM from ChangeSet */
		unless (s_cset) s_cset = aprintf("%s/" CHANGESET, rootpath);
		unless (sc = sccs_init(s_cset, 0)) goto ret;
		if (sccs_get(sc, rev, 0, 0, 0, SILENT|GET_HASHONLY, 0)) {
			csetm = 0;
		} else {
			csetm = sc->mdbm;
			sc->mdbm = 0;
		}
		sccs_free(sc);

		delete_cset_cache(rootpath, 1);	/* save newest */

		/* write new MDBM */
		m = mdbm_open(mpath, O_RDWR|O_CREAT|O_TRUNC, 0666, 0);
		unless (m) {
			if (csetm) mdbm_close(csetm);
			goto ret;
		}
		if (csetm) {
			kvpair	kv;

			EACH_KV (csetm) {
				mdbm_store(m, kv.key, kv.val, MDBM_REPLACE);
			}
			mdbm_close(csetm);
		}
		sprintf(buf, "%lx %lx",
		    (unsigned long)csetstat.st_mtime,
		    (unsigned long)csetstat.st_size);
		mdbm_store_str(m, "STAT", buf, MDBM_REPLACE);
		mdbm_store_str(m, "REV", rev, MDBM_REPLACE);
	}
	sccs_sdelta(s, sccs_ino(s), rootkey);
	deltakey = mdbm_fetch_str(m, rootkey);
	if (deltakey) ret = sccs_findKey(s, deltakey);
	mdbm_close(m);
 ret:
	if (s_cset) free(s_cset);
	if (mpath) free(mpath);
	return (ret);
}

private inline int
isbranch(delta *d)
{
	return (d->r[2]);
}

private inline int
morekids(delta *d)
{
	return (d->kid && (d->kid->type != 'R') && samebranch(d, d->kid));
}

/*
 * Get the delta that is the basis for this edit.
 * Get the revision name of the new delta.
 * Sep 2000 - removed branch, we don't support it.
 */
delta *
sccs_getedit(sccs *s, char **revp)
{
	char	*rev = *revp;
	ser_t	a = 0, b = 0, c = 0, d = 0;
	delta	*e, *t;
	static	char buf[MAXREV];

	debug((stderr, "sccs_getedit(%s, %s)\n", s->gfile, notnull(*revp)));
	/*
	 * use the findrev logic to get to the delta.
	 */
	e = findrev(s, rev);
	debug((stderr, "sccs_getedit(e=%s)\n", e?e->rev:""));

	unless (e) {
		/*
		 * Special case: we're at 1.x and they want 2.1 or 3.1.
		 * We allow that.
		 */
		scanrev(rev, &a, &b, &c, &d);
		if ((c == 0) && (b == 1)) {
			e = findrev(s, 0);
			assert(e);
			if (e->r[0] < a) goto ok;
		}
		return (0);
	}
ok:
	/*
	 * Just continue trunk/branch
	 * Because the kid may be a branch, we have to be extra careful here.
	 */
	unless (morekids(e)) {
		a = e->r[0];
		b = e->r[1];
		c = e->r[2];
		d = e->r[3];
		if (!c) {
			/* Seems weird but makes -e -r2 -> 2.1 when tot is 1.x
			 */
			int	release = rev ? atoi(rev) : 1;

			if (release > a) {
				a = release;
				b = 1;
			} else {
				b++;
			}
			sprintf(buf, "%d.%d", a, b);
		} else {
			sprintf(buf, "%d.%d.%d.%d", a, b, c, d+1);
		}
		debug((stderr, "sccs_getedit1(%s) -> %s\n", notnull(rev), buf));
		*revp = buf;
	}
	else {
		/*
		 * For whatever reason (they asked, or there is a kid in
		 * the way), we need a branch.  Branches are all based,
		 * in their /name/, off of the closest trunk node.  Go
		 * backwards up the tree until we hit the trunk and then
		 * use that rev as a basis.  Because all branches are
		 * below that trunk node, we don't have to search the
		 * whole tree.
		 */

		for (t = e; isbranch(t); t = t->parent);
		R[0] = t->r[0]; R[1] = t->r[1]; R[2] = 1; R[3] = 1;
		while (_rfind(t)) R[2]++;
		sprintf(buf, "%d.%d.%d.%d", R[0], R[1], R[2], R[3]);
		debug((stderr, "sccs_getedit2(%s) -> %s\n", notnull(rev), buf));
		*revp = buf;
	}

	return (e);
}

private inline char *
peek(sccs *s)
{
	if (s->encoding & E_GZIP) return (zpeek(s->zin, 2));
	return (s->where);
}

private off_t
sccstell(sccs *s)
{
	return (s->where - s->mmap);
}

private char	*
fastnext(sccs *s)
{
	register char *t = s->where;
	register char *tmp = s->mmap + s->size;
	register size_t n = tmp - t;

	if (n <= 0) return (0);
	/*
	 * I tried unrolling this a couple of ways and it got worse
	 *
	 * An idea for improvement: read ahead and cache the pointers.
	 * It can be done here but be careful not to screw up s->where, that
	 * needs to stay valid, peekc and others use it.
	 */
	while (n-- && *t++ != '\n');

	tmp = s->where;
	s->where = t;
	return (tmp);
}

/*
 * uncompress data into a local buffer and return a pointer to it.
 * Callers of this interface want a \n terminated buffer, so if we get
 * to the end of the buffer and there isn't enough space, we have to
 * move and decompress some more.
 */
private inline char *
nextdata(sccs *s)
{
	unless (s->encoding & E_GZIP) return (fastnext(s));
	return (zgets(s->zin));
}

/*
 * Look for a bug in the weave that could be created by bitkeeper
 * versions 3.0.1 or older.  It happens when a file is delta'ed on
 * Windows when it ends in a bare \r without a trailing \n.
 *
 * This function gets run from check.c if checksums are being verified
 * right after we have walked the weave to check checksums.  If a
 * 'no-newline' marker is seen in the file then get_reg() sets
 * s->has_nonl.  And we skip this call if s->has_nonl is not set, that
 * way the weave is only extracted twice on a subset of the files.
 *
 * This code is a simple pattern match that looks for a blank line
 * followed by end marker with a no-newline flag.
 *
 * The function should be removed after all customers have upgraded
 * beyond 3.0.1.
 */
int
chk_nlbug(sccs *s)
{
	char	*buf, *p;
	int	sawblank = 0;
	int	ret = 0;

	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
	}
	while (buf = nextdata(s)) {
		if (buf[0] == '\001' && buf[1] == 'E') {
			p = buf + 3;
			while (isdigit(*p)) p++;
			if (*p == 'N' && sawblank) {
				getMsg("saw_blanknonl", s->sfile, 0, stderr);
				ret = 1;
			}
		}
		sawblank = (buf[0] == '\n');
	}
	if ((s->encoding & E_GZIP) && zgets_done(s->zin)) ret = 1;
	s->zin = 0;
	return (ret);
}

/*
 * This does standard SCCS expansion, it's almost 100% here.
 * New stuff added:
 * %@%	user@host
 */
private char *
expand(sccs *s, delta *d, char *l, int *expanded)
{
	char 	*buf;
	char	*t;
	char	*tmp;
	time_t	now = 0;
	struct	tm *tm = 0;
	ser_t	a[4] = {0, 0, 0, 0};
	int hasKeyword = 0, buf_size;
#define EXTRA 1024

	a[0] = a[1] = a[2] = a[3] = 0;
	/* pre scan the line to determine if it needs keyword expansion */
	*expanded = 0;
	for (t = l; *t != '\n'; t++) {
		if (hasKeyword) continue;
		if (t[0] != '%' || t[1] == '\n' || t[2] != '%') continue;
		/* NOTE: this string *must* match the case label below */
		if (strchr("ABCDEFGHIKLMPQRSTUWYZ@", t[1])) hasKeyword = 1;
	}
	unless (hasKeyword) return (l);
	buf_size = t - l + EXTRA; /* get extra memory for keyword expansion */

	/* ok, we need to expand keyword, allocate a new buffer */
	t = buf = malloc(buf_size);
	a[0] = a[1] = a[2] = a[3] = 0;	/* bogus gcc 4 warning */
	while (*l != '\n') {
		if (l[0] != '%' || l[1] == '\n' || l[2] != '%') {
			*t++ = *l++;
			continue;
		}
		switch (l[1]) {
		    case 'A':	/* %Z%%Y% %M% %I%%Z% */
			/* XXX - no tflag */
			strcpy(t, "@(#) "); t += 5;
			tmp = basenm(s->gfile);
			strcpy(t, tmp); t += strlen(tmp); *t++ = ' ';
			strcpy(t, d->rev); t += strlen(d->rev);
			strcpy(t, "@(#)"); t += 4;
			break;

		    case 'B':	/* branch name: XXX */
			tmp = branchname(d); strcpy(t, tmp); t += strlen(tmp);
			break;

		    case 'C':	/* line number - XXX */
			*t++ = '%'; *t++ = 'C'; *t++ = '%';
			break;

		    case 'D':	/* today: 97/06/22 */
			if (!now) { time(&now); tm = localtimez(&now, 0); }
			assert(tm);
			if (YEAR4(s)) {
				int	y = tm->tm_year;

				if (y < 69) y += 2000; else y += 1900;
				sprintf(t, "%4d/%02d/%02d",
				    y, tm->tm_mon+1, tm->tm_mday);
				t += 10;
			} else {
				while (tm->tm_year > 100) tm->tm_year -= 100;
				sprintf(t, "%02d/%02d/%02d",
				    tm->tm_year, tm->tm_mon+1, tm->tm_mday);
				t += 8;
			}
			break;

		    case 'E':	/* most recent delta: 97/06/22 */
			if (YEAR4(s)) {
				if (atoi(d->sdate) > 69) {
					*t++ = '1'; *t++ = '9';
				} else {
					*t++ = '2'; *t++ = '0';
				}
			}
			strncpy(t, d->sdate, 8); t += 8;
			break;

		    case 'F':	/* s.file name */
			strcpy(t, "SCCS/"); t += 5;
			tmp = basenm(s->sfile);
			strcpy(t, tmp); t += strlen(tmp);
			break;

		    case 'G':	/* most recent delta: 06/22/97 */
			*t++ = d->sdate[3]; *t++ = d->sdate[4]; *t++ = '/';
			*t++ = d->sdate[6]; *t++ = d->sdate[7]; *t++ = '/';
			if (YEAR4(s)) {
				if (atoi(d->sdate) > 69) {
					*t++ = '1'; *t++ = '9';
				} else {
					*t++ = '2'; *t++ = '0';
				}
			}
			*t++ = d->sdate[0]; *t++ = d->sdate[1];
			break;

		    case 'H':	/* today: 06/22/97 */
			if (!now) { time(&now); tm = localtimez(&now, 0); }
			assert(tm);
			if (YEAR4(s)) {
				int	y = tm->tm_year;

				if (y < 69) y += 2000; else y += 1900;
				sprintf(t, "%4d/%02d/%02d",
				    y, tm->tm_mon+1, tm->tm_mday);
				sprintf(t, "%02d/%02d/%04d",
				    tm->tm_mon+1, tm->tm_mday, y);
				t += 10;
			} else {
				while (tm->tm_year > 100) tm->tm_year -= 100;
				sprintf(t, "%02d/%02d/%02d",
				    tm->tm_mon+1, tm->tm_mday, tm->tm_year);
				t += 8;
			}
			break;

		    case 'I':	/* name of revision: 1.1 or 1.1.1.1 */
			strcpy(t, d->rev); t += strlen(d->rev);
			break;

		    case 'K':	/* BitKeeper Key */
		    	t += sccs_sdelta(s, d, t);
			break;

		    case 'L':	/* 1.2.3.4 -> 2 */
			scanrev(d->rev, &a[0], &a[1], 0, 0);
			sprintf(t, "%d", a[1]); t += strlen(t);
			break;

		    case 'M':	/* mflag or filename: slib.c */
			tmp = basenm(s->gfile);
			strcpy(t, tmp); t += strlen(tmp);
			break;

		    case 'P':	/* full: /u/lm/smt/sccs/SCCS/s.slib.c */
			tmp = fullname(s->sfile);
			strcpy(t, tmp); t += strlen(tmp);
			break;

		    case 'Q':	/* qflag */
			*t++ = '%'; *t++ = 'Q'; *t++ = '%';
			break;

		    case 'R':	/* release 1.2.3.4 -> 1 */
			scanrev(d->rev, &a[0], 0, 0, 0);
			sprintf(t, "%d", a[0]); t += strlen(t);
			break;

		    case 'S':	/* rev number: 1.2.3.4 -> 4 */
			a[3] = 0;
			scanrev(d->rev, &a[0], &a[1], &a[2], &a[3]);
			sprintf(t, "%d", a[3]); t += strlen(t);
			break;

		    case 'T':	/* time: 23:04:04 */
			if (!now) { time(&now); tm = localtimez(&now, 0); }
			assert(tm);
			sprintf(t, "%02d:%02d:%02d",
			    tm->tm_hour, tm->tm_min, tm->tm_sec);
			t += 8;
			break;

		    case 'U':	/* newest delta: 23:04:04 */
			strcpy(t, &d->sdate[9]); t += 8;
			break;

		    case 'W':	/* @(#)%M% %I%: @(#)slib.c 1.1 */
			strcpy(t, "@(#) "); t += 4;
			tmp = basenm(s->gfile);
			strcpy(t, tmp); t += strlen(tmp); *t++ = ' ';
			strcpy(t, d->rev); t += strlen(d->rev);
			break;

		    case 'Y':	/* tflag */
			*t++ = '%'; *t++ = 'Y'; *t++ = '%';
			break;

		    case 'Z':	/* @(#) */
			strcpy(t, "@(#)"); t += 4;
			break;

		    case '@':	/* user@host */
			strcpy(t, d->user);
			t += strlen(d->user);
			if (d->hostname) {
				*t++ = '@';
				strcpy(t, d->hostname);
				t += strlen(d->hostname);
			}
			break;
			
		    case '#':	/* user */
			strcpy(t, d->user);
			t += strlen(d->user);
			break;

		    default:
			*t++ = *l++;
			continue;
		}
		l += 3;
	}
	*t++ = '\n'; *t = 0;
	assert((t - buf) <= buf_size);
	*expanded = 1;  /* Note: if expanded flag is set	 */
					/* caller must free buffer when done */			
	return (buf);
}

/*
 * This does standard RCS expansion.
 *
 * This code recognizes RCS keywords and expands them.  This code can
 * handle finding a keyword that is already expanded and reexpanding
 * it to the correct value.  This is for when a RCS file gets added
 * with the keywords already expanded.
 *
 * XXX In BitKeeper the contents of the weave and edited files are is
 * normally not expanded. (ie: $Date$) However mistakes can happen.
 * We should unexpand RCS keywords on delta if they happen...
 */
private char *
rcsexpand(sccs *s, delta *d, char *line, int *expanded)
{
	static const struct keys {
		int	len;
		char	*keyword;
		char	*dspec;
	} keys[] = {
		/*   123456789 */
		{6, "Author", ": :USER:@:HOST: "},
		{4, "Date", ": :D: :T::TZ: "},
		{6, "Header", ": :GFILE: :I: :D: :T::TZ: :USER:@:HOST: "},
		{2, "Id", ": :G: :I: :D: :T::TZ: :USER:@:HOST: "},
		{6, "Locker", ": <Not implemented> "},
		{3, "Log", ": <Not implemented> "},
		{4, "Name", ": <Not implemented> "}, /* almost :TAG: */
		{8, "Revision", ": :I: "},
		{7, "RCSfile", ": s.:G: "},
		{6, "Source", ": :SFILE: "},
		{5, "State", ": <unknown> "}
	};
	const	int	keyslen = sizeof(keys)/sizeof(struct keys);
	char	*p, *prs;
	char	*out = 0;
	char	*outend = 0;
	char	*last = line;
	char	*ks;	/* start of keyword. */
	char	*ke;	/* byte after keyword name */
	int	i;

	*expanded = 0;
	p = line;
	while (*p != '\n') {
		/* Look for keyword */
		while (*p != '\n' && *p != '$') p++;
		if (*p == '\n') break;
		ks = ++p;  /* $ */
		while (isalpha(*p)) p++;
		if (*p == '$') {
			ke = p;
		} else if (*p == ':') {
			ke = p;
			while (*p != '\n' && *p != '$') p++;
			if (*p == '\n') break;
		} else {
		        continue;
		}
		/* found something that matches the pattern */
		for (i = 0; i < keyslen; i++) {
			if (keys[i].len == (ke-ks) &&
			    strneq(keys[i].keyword, ks, ke-ks)) {
				break;
			}
		}
		if (i == keyslen) continue; /* try again */

		unless (out) {
			char	*nl = p;
			while (*nl != '\n') nl++;
			outend = out = malloc(nl - line + EXTRA);
		}
		*expanded = 1;
		while (last < ke) *outend++ = *last++;
		prs = sccs_prsbuf(s, d, 0, keys[i].dspec);
		strcpy(outend, prs);
		free(prs);
		while (*outend) outend++;
		last = p;
		++p;
	}
	if (out) {
		while (last <= p) *outend++ = *last++;
		*outend = 0;
	} else {
		out = line;
	}
	return (out);
}

/*
 * We don't consider EPIPE an error since we hit it when we do stuff like
 * get -p | whatever
 * and whatever exits first.
 * XXX - on linux, at least, this doesn't work, so we catch the EPIPE case
 * where we call this function.  There should be only one place in the
 * get_reg() function.
 */
int
flushFILE(FILE *out)
{
	if (fflush(out) && (errno != EPIPE)) {
		return (errno);
	}
	return (ferror(out));
}

/*
 * We couldn't initialize an SCCS file.	 Tell the user why not.
 * Possible reasons we handle:
 *	no s.file
 *	bad checksum
 *	couldn't build the graph
 *
 * XXX - use this for 100% of the error messages.
 */
void
sccs_whynot(char *who, sccs *s)
{
	if (s->io_error && s->io_warned) return;
	if (BEEN_WARNED(s)) return;
	unless (HAS_SFILE(s)) {
		fprintf(stderr, "%s: No such file: %s\n", who, s->sfile);
		return;
	}
	unless (s->cksumok) {
		fprintf(stderr, "%s: bad checksum in %s\n", who, s->sfile);
		return;
	}
	unless (HASGRAPH(s)) {
		fprintf(stderr, "%s: couldn't decipher delta table in %s\n",
		    who, s->sfile);
		return;
	}
	if (HAS_ZFILE(s)) {
		fprintf(stderr, "%s: %s is zlocked\n", who, s->gfile);
		return;
	}
	if (HAS_PFILE(s)) {
		fprintf(stderr, "%s: %s is edited\n", who, s->gfile);
		return;
	}
	if (errno == ENOSPC) {
		fprintf(stderr, "No disk space for %s\n", s->sfile);
		return;
	}
	fprintf(stderr, "%s: %s: unknown error.\n", who, s->sfile);
}

/*
 * Add this meta delta into the symbol graph.
 * Set this symbol's leaf flag and clear the parent's.
 */
void
symGraph(sccs *s, delta *d)
{
	delta	*p;

	if (getenv("_BK_NO_TAG_GRAPH")) {
		assert(!d->symGraph);
		assert(!d->symLeaf);
		assert(!d->ptag);
		assert(!d->mtag);
		return;
	}
	if (d->symGraph) return;
	for (p = s->table; p && !p->symLeaf; p = p->next);
	if (p) {
		d->ptag = p->serial;
		p->symLeaf = 0;
	}
	d->symLeaf = 1;
	d->symGraph = 1;
}

/*
 * For each symbol, go find the real delta and point to it instead of the
 * meta delta.
 */
private void
metaSyms(sccs *sc)
{
	delta	*d;
	symbol	*sym;

	for (sym = sc->symbols; sym; sym = sym->next) {
		assert(sym->d);
		if (sym->d->type == 'D') continue;
		assert(sym->d->parent);
		for (d = sym->d->parent; d->type != 'D'; d = d->parent) {
			assert(d);
		}
		sym->d = d;
		d->flags |= D_SYMBOLS;
	}

}

int
sccs_tagleaves(sccs *s, delta **l1, delta **l2)
{
	delta	*d;
	symbol	*sym;
	int	first = 1;
	char	*arev = 0;
	char	*brev = 0;
	char	*aname, *bname;

	/*
	 * This code used to walk the symbol table list but that doesn't
	 * work because a tag graph node might not have a symbol; tag
	 * automerges do not have symbols.  So there is no entry in the
	 * symbol table.
	 */
	aname = bname = "?";
	for (d = s->table; d; d = d->next) {
		unless (d->symLeaf) continue;
		for (sym = s->symbols; sym; sym = sym->next) {
			if (sym->metad == d) break;
		}
		unless (*l1) {
			arev = d->rev;
			if (sym) aname = sym->symname;
			*l1 = d;
			continue;
		}
		unless (*l2) {
			brev = d->rev;
			if (sym) bname = sym->symname;
			*l2 = d;
			continue;
		}
		if (first) {
			fprintf(stderr,
			    "Unmerged tag tips:\n"
			    "\t%-16s %s\n\t%-16s %s\n\t%-16s %s\n",
			    arev, aname, brev, bname,
			    d->rev, sym ? sym->symname : "<tag merge>");
		    	first = 0;
		} else {
			fprintf(stderr,
			    "\t%-16s %s\n",
			    d->rev, sym ? sym->symname : "<tag merge>");
		}
	}
	return (!first);	/* first == 1 means no errors */
}

/* make a meta delta in the RESYNC directory, storing in csets-in */
private	int
resyncMeta(sccs *s, delta *d, char *buf)
{
	sccs	*s2;
	MMAP	*m;
	FILE	*f;
	char	key[MAXKEY];

	m = mrange(buf, buf + strlen(buf), "");
	/* note: this rewrites the s.file, and does a sccs_close() */
	if (sccs_meta("resolve", s, d, m, 1)) {
		fprintf(stderr, "resolve: failed to make tag merge\n");
		return (1);
	}

	/* If called out of RESYNC, save in csets-in */
	/* XXX: could assert it is in RESYNC */
	unless (proj_isResync(s->proj)) return (0);
	unless (s2 = sccs_init(s->sfile, s->initFlags)) {
err:		if (s2) sccs_free(s2);
		return (1);
	}
	unless (s2->table->symGraph) {
		fprintf(stderr, "resolve: new tip is not a tag merge\n");
		goto err;
	}
	sccs_sdelta(s2, s->table, key);
	sccs_free(s2);
	unless (f = fopen(CSETS_IN, "a")) {
		fprintf(stderr,
		    "resolve: adding tag merge, csets-in did not open\n");
		perror("resolve");
		goto err;
	}
	fprintf(f, "%s\n", key);
	fclose(f);
	return (0);
}

/*
 * Add a merge delta which closes the tag graph.
 */
int
sccs_tagMerge(sccs *s, delta *d, char *tag)
{
	delta	*l1 = 0, *l2 = 0;
	char	*buf;
	char	k1[MAXKEY], k2[MAXKEY];
	time_t	tt = time(0);
	int	len, rc;

	if (sccs_tagleaves(s, &l1, &l2)) assert("too many tag leaves" == 0);
	assert(l1 && l2);
	/*
	 * If we are automerging, then use the later of the two tag tips.
	 */
	unless (d) {
		assert(tag == 0);
		d = (l1->date > l2->date) ? l1 : l2;
	}
	sccs_sdelta(s, l1, k1);
	sccs_sdelta(s, l2, k2);
	len = strlen(k1) + strlen(k1) + 2000;
	if (tag) len += strlen(tag);
	buf = malloc(len);
	/* XXX - if we ever remove |ChangeSet| from the keys fix P below */
	sprintf(buf,
	    "M 0.0 %s%s %s@%s 0 0 0/0/0\nP ChangeSet\n"
	    "%s%s%ss g\ns l\ns %s\ns %s\n%s\n",
	    time2date(tt), sccs_zone(tt), sccs_user(), sccs_host(),
	    tag ? "S " : "",
	    tag ? tag : "",
	    tag ? "\n" : "",
	    k1, k2,
	    "------------------------------------------------");
	assert(strlen(buf) < len);
	rc = resyncMeta(s, d, buf);
	free(buf);
	return (rc);
}

/*
 * Add another tag entry to the delta but do not close the graph, this
 * is what we call when we have multiple tags, the last tag calls the
 * tagMerge.
 */
int
sccs_tagLeaf(sccs *s, delta *d, delta *md, char *tag)
{
	char	*buf;
	char	k1[MAXKEY];
	time_t	tt = time(0);
	int	rc;
	delta	*e, *l1 = 0, *l2 = 0;

	if (sccs_tagleaves(s, &l1, &l2)) assert("too many tag leaves" == 0);
	for (e = s->table; e; e = e->next) e->flags &= ~D_RED;
	sccs_tagcolor(s, l1);
	if (md->flags & D_RED) {
		md = l1;
	} else {
		md = l2;
	}
	assert(md);
	sccs_sdelta(s, md, k1);
	assert(tag);
	buf = aprintf("M 0.0 %s%s %s@%s 0 0 0/0/0\nS %s\ns g\ns l\ns %s\n%s\n",
	    time2date(tt), sccs_zone(tt), sccs_user(), sccs_host(),
	    tag,
	    k1,
	    "------------------------------------------------");
	rc = resyncMeta(s, d, buf);
	free(buf);
	return (rc);
}

/*
 * This is called after sccs_tagcolor() which colors all nodes
 * related to a node.  It colors them RED and BLUE.
 * 
 * This code is used to uncolor the RED nodes that lie in the
 * intersection of histories of 2 different leaves.
 * If it finds a RED and BLUE, it uncolors RED.
 * If it find a BLUE but not RED, then it uncolors the BLUE.
 * For this trick to work, we need to go in table order rather
 * than recurse, because doing this, when we uncolor BLUE, we
 * know that we will not need to use it again.  The same is not
 * true if we did recurse.
 *
 * When calling sccs_tagcolor again, it will only color
 * RED and BLUE the nodes not in the intersection.
 */
private void
taguncolor(sccs *s, delta *d)
{
	assert(d);
	d->flags |= D_BLUE;
	for (; d; d = d->next) {
		unless (d->flags & D_BLUE) continue;
		if (d->flags & D_RED) {
			d->flags &= ~D_RED;
			continue;
		}
		if (d->ptag) sfind(s, d->ptag)->flags |= D_BLUE;
		if (d->mtag) sfind(s, d->mtag)->flags |= D_BLUE;
		d->flags &= ~D_BLUE;
	}
}

/*
 * Return an MDBM ${value} = rev,rev
 * for each value that was added by both the left and the right.
 */
MDBM	*
sccs_tagConflicts(sccs *s)
{
	MDBM	*db = 0;
	delta	*l1 = 0, *l2 = 0;
	char	buf[MAXREV*3];
	symbol	*sy1, *sy2;

	if (sccs_tagleaves(s, &l1, &l2)) assert("too many tag leaves" == 0);
	unless (l2) return (0);

	/* We always return an MDBM even if it is just an automerge case
	 * with nothing to merge.
	 */
	unless (db) db = mdbm_mem();
	sccs_tagcolor(s, l1);
	taguncolor(s, l2);	/* uncolor the intersection */
	for (sy1 = s->symbols; sy1; sy1 = sy1->next) {
		unless (sy1->metad->flags & D_RED) continue;
		sy1->metad->flags &= ~D_RED;
		sy1->left = 1;
	}
	sccs_tagcolor(s, l2);
	for (sy1 = s->symbols; sy1; sy1 = sy1->next) {
		unless (sy1->metad->flags & D_RED) continue;
		sy1->metad->flags &= ~D_RED;
		sy1->right = 1;
	}

	/*
	 * OK, for each symbol only in the left, see if there is one of the
	 * same name only in the right.
	 *
	 * Imagine a tree like so:
	 *	[ 1.5, "foo" ]
	 *	[ 1.6, "foo" ]	[ 1.5.1.1, "foo" ]
	 *	[ 1.7, "foo" ]
	 *
	 * We want to store 1.7,1.5.1.1 as the conflict.
	 */
	for (sy1 = s->symbols; sy1; sy1 = sy1->next) {
		unless (sy1->left && !sy1->right) continue;
		for (sy2 = s->symbols; sy2; sy2 = sy2->next) {
			unless (sy2->right && !sy2->left &&
			    streq(sy1->symname, sy2->symname)) {
			    	continue;
			}
			/*
			 * Quick check to see if they added the same symbol
			 * twice to the same rev.
			 */
			if (streq(sy1->rev, sy2->rev)) continue;
			/*
			 * OK, we really have a conflict, save it.
			 * If it is already there, make sure that our version
			 * has later serials on both sides.
			 */
			sprintf(buf,
			    "%d %d", sy1->metad->serial, sy2->metad->serial);
			if (mdbm_store_str(db, sy1->symname, buf, MDBM_INSERT)) {
				char	*old = mdbm_fetch_str(db, sy1->symname);
				int	a = 0, b = 0;

				assert(old);
				sscanf(old, "%d %d", &a, &b);
				assert(a && b);
				if ((a > sy1->metad->serial) ||
				    (b > sy2->metad->serial)) {
				    	continue;
				}
				mdbm_store_str(db,
				    sy1->symname, buf, MDBM_REPLACE);
		    	}
		}
	}
	return (db);
}

private void
unvisit(sccs *s, delta *d)
{
	unless (d) return;
	unless (d->flags & D_BLUE) return;
	d->flags &= ~D_BLUE;

	if (d->ptag) unvisit(s, sfind(s, d->ptag));
	if (d->mtag) unvisit(s, sfind(s, d->mtag));
}

private delta *
tagwalk(sccs *s, delta *d)
{
	unless (d) return ((delta*)1);	/* this is an error case */

	/* note that the stripdel path has used D_RED */
	if (d->flags & D_BLUE) return(0);
	d->flags |= D_BLUE;

	if (d->ptag) if (tagwalk(s, sfind(s, d->ptag))) return (d);
	if (d->mtag) if (tagwalk(s, sfind(s, d->mtag))) return (d);
	return (0);
}

private int
checktags(sccs *s, delta *leaf, int flags)
{
	delta	*d, *e;

	unless (leaf) return(0);
	unless (d = tagwalk(s, leaf)) {
		unvisit(s, leaf);
		return (0);
	}
	if (d == (delta*)1) {
		verbose((stderr,
		    "Corrupted tag graph in %s\n", s->gfile));
		return (1);
	}
	unless (e = sfind(s, d->ptag)) {
		verbose((stderr,
		    "Cannot find serial %u, tag parent for %s:%u, in %s\n",
		    d->ptag, d->rev, d->serial, s->gfile));
	} else {
		assert(d->mtag);
		verbose((stderr,
		    "Cannot find serial %u, tag parent for %s:%u, in %s\n",
		    d->mtag, d->rev, d->serial, s->gfile));
	}
	return (1);
}

int
sccs_badTag(char *me, char *tag, int flags)
{
	char	*p;

	if (isdigit(*tag)) {
		verbose((stderr,
		    "%s: %s: tags can't start with a digit.\n", me, tag));
		return (1);
	}
	switch (*tag) {
	    case '@':
	    case '=':
	    case '-':
	    case '+':
	    case '.':
		verbose((stderr,
		    "%s: %s: tags can't start with a '%c'.\n", me, tag, *tag));
		return (1);
	}
	if (strstr(tag, "..")) {
		verbose((stderr,
		    "%s: tag %s cannot contain '..'\n", me, tag));
		return (1);
	}
	if (strstr(tag, ".,")) {
		verbose((stderr,
		    "%s: tag %s cannot contain '.,'\n", me, tag));
		return (1);
	}
	if (strstr(tag, ",.")) {
		verbose((stderr,
		    "%s: tag %s cannot contain ',.'\n", me, tag));
		return (1);
	}
	if (strstr(tag, ",,")) {
		verbose((stderr,
		    "%s: tag %s cannot contain ',,'\n", me, tag));
		return (1);
	}
	p = tag;
	while (*p) {
		switch (*p++) {
		    case '\001':
		    case '|':
		    case '\n':
		    case '\r':
			verbose((stderr,
			    "%s: tag %s cannot contain \"^A,|\\n\\r\"\n",
			    me, tag));
			return (1);
		}
	}
	return (0);
}

/*
 * Check tag graph integrity.
 */
private int
checkTags(sccs *s, int flags)
{
	delta	*l1 = 0, *l2 = 0;
	symbol	*sym;
	int	bad = 0;

	/* Nobody else has tags */
	unless (CSET(s)) return (0);

	/* Make sure that tags don't contain weird characters */
	for (sym = s->symbols; sym; sym = sym->next) {
		unless (sym->symname) continue;
		/* XXX - not really "check" all the time */
		if (sccs_badTag("check", sym->symname, flags)) bad = 1;
	}
	if (bad) return (128);

	if (sccs_tagleaves(s, &l1, &l2)) return (128);
	if (checktags(s, l1, flags) || checktags(s, l2, flags)) return (128);

	return (0);
}

/*
 * Dig meta data out of a delta.
 * The buffer looks like ^Ac<T>data where <T> is one character type.
 * A - hash for binpool
 * B - cset file root key
 * C - cset boundry
 * D - dangling delta
 * E - ??
 * F - date fudge
 * H - host name
 * K - delta checksum
 * M - merge parent
 * P - pathname
 * O - permissions.
 * R - random bits (1.0 only)
 * S - pre tag symbol
 * T - text description
 * V - file format version (1.0 delta only)
 * X - Xflags
 * Z - zone (really offset from GMT)
 */
private void
meta(sccs *s, delta *d, char *buf)
{
	if (d->type != 'D') d->flags |= D_META;
	switch (buf[2]) {
	    case 'A':
		hashArg(d, &buf[3]);
		break;
	    case 'B':
		csetFileArg(d, &buf[3]);
		break;
	    case 'C':
		d->flags |= D_CSET;
		break;
	    case 'D':
		d->dangling = 1;
		break;
	    case 'E':
		/* OLD, ignored */
		break;
	    case 'F':
		/* Do not add to date here, done in inherit */
		d->dateFudge = atoi(&buf[3]);
		break;
	    case 'H':
		hostArg(d, &buf[3]);
		break;
	    case 'K':
		sumArg(d, &buf[3]);
		break;
	    case 'M':
		mergeArg(d, &buf[3]);
		break;
	    case 'P':
		pathArg(d, &buf[3]);
		break;
	    case 'O':
		modeArg(d, &buf[3]);
		break;
	    case 'R':
		randomArg(d, &buf[3]);
		break;
	    case 'S':
		symArg(s, d, &buf[3]);
		break;
	    case 'T':
		assert(d);
		d->flags |= D_TEXT;
		if (buf[3] == ' ') {
			d->text = addLine(d->text, strnonldup(&buf[4]));
		}
		break;
	    case 'V':
		s->version = atoi(&buf[3]);
		unless (s->version <= SCCS_VERSION) {
			fprintf(stderr,
			    "Later file format version %d, forcing read only\n",
			    s->version);
			s->state |= S_READ_ONLY;
		}
		break;
	    case 'X':
		assert(d);
		d->xflags = strtol(&buf[3], 0, 0); /* hex or dec */
		/* reenable longkeys for old KEY_FORMAT2 trees */
		if (CSET(s) && (s->xflags & X_LONGKEY)) d->xflags |= X_LONGKEY;
		d->xflags &= ~X_SINGLE; /* clear old single_user bit */
		d->flags |= D_XFLAGS;
		break;
	    case 'Z':
		zoneArg(d, &buf[3]);
		break;
	    default:
		fprintf(stderr, "%s: Ignoring %.5s\n", s->gfile, buf);
		/* got unknown field, force read only mode */
		s->state |= S_READ_ONLY;
		return;
	}
	s->bitkeeper = 1;
}

/*
 * Find the next delta in linear table order.
 * If you pass in 1.10, this should give you 1.11.
 */
delta	*
sccs_next(sccs *s, delta *d)
{
	delta	*e;

	if (!s || !d) return (0);
	if (d == s->table) return (0);
	for (e = d->kid ? d->kid : s->table; e->next != d; e = e->next);
	assert(e && (e->next == d));
	return (e);
}

/*
 * Read in an sfile and build the delta table graph.
 * Scanf caused problems here when there are corrupted files.
 */
private void
mkgraph(sccs *s, int flags)
{
	delta	*d = 0, *t;
	char	rev[100], date[9], time[9], user[100];
	char	*p;
	char	tmp[100];
	int	i;
	int	line = 1;
	char	*expected = "?";
	char	*buf;

	seekto(s, 0);
	fastnext(s);			/* checksum */
	line++;
	debug((stderr, "mkgraph(%s)\n", s->sfile));
	for (;;) {
		unless (buf = fastnext(s)) {
bad:
			fprintf(stderr,
			    "%s: bad delta on line %d, expected `%s'",
			    s->sfile, line, expected);
			if (buf) {
				fprintf(stderr,
				    ", line follows:\n\t``%.*s''\n",
				    linelen(buf)-1, buf);
			} else {
				fprintf(stderr, "\n");
			}
			sccs_freetable(s->table);
			s->table = 0;
			return;
		}
		line++;
		if (strneq(buf, "\001u\n", 3)) break;
		t = calloc(sizeof(*t), 1);
		assert(t);
		s->numdeltas++;
		t->kid = d;
		if (d)
			d->next = t;
		else
			s->table = t;
		d = t;
		/* ^As 00001/00000/00011 */
		if (buf[0] != '\001' || buf[1] != 's' || buf[2] != ' ') {
			expected = "^As ";
			goto bad;
		}
		p = &buf[3];
		d->added = atoiMult_p(&p);
		p++;
		d->deleted = atoiMult_p(&p);
		p++;
		d->same = atoiMult_p(&p);
		/* ^Ad D 1.2.1.1 97/05/15 23:11:46 lm 4 2 */
		/* ^Ad R 1.2.1.1 97/05/15 23:11:46 lm 4 2 */
		buf = fastnext(s);
		line++;
		if (buf[0] != '\001' || buf[1] != 'd' || buf[2] != ' ') {
			expected = "^Ad ";
			goto bad;
		}
	    /* D|R */
		d->type = buf[3];
	    /* 1.1 or 1.2.2.2, etc. */
		p = &buf[5];
		for (i = 0, p = &buf[5]; *p != ' '; rev[i++] = *p++);
		rev[i] = 0;
		if (s->revLen < i) s->revLen = i;
		if (*p != ' ') { expected = "^AD 1.1 "; goto bad; }
	    /* 98/03/17 */
		for (i = 0, p++; *p != ' '; date[i++] = *p++);
		date[i] = 0;
		if (*p != ' ') { expected = "^AD 1.1 98/03/17 "; goto bad; }
	    /* 18:32:39[.mmm] */
		for (i = 0, p++; *p != ' ' && *p != '.'; time[i++] = *p++);
		time[i] = 0;
		if (*p != ' ') {
			expected = "^AD 1.1 98/03/17 18:32:39";
			goto bad;
		}
	    /* user */
		for (i = 0, p++; *p != ' '; user[i++] = *p++);
		user[i] = 0;
		if (s->userLen < i) s->userLen = i;
		if (*p != ' ') {
			expected = "^AD 1.1 98/03/17 18:32:39 user ";
			goto bad;
		}
		p++;
	    /* 10 11 */
		d->serial = atoi_p(&p);
		if (*p != ' ') {
			expected = "^AD 1.1 98/03/17 18:32:39 user 12 ";
			goto bad;
		}
		p++;
		d->pserial = atoi(p);
		if (d->serial >= s->nextserial) s->nextserial = d->serial + 1;
		debug((stderr, "mkgraph(%s)\n", rev));
		d->rev = strdup(rev);
		explode_rev(d);
		if (d->flags & D_BADFORM) {
			expected = "1.2 or 1.2.3.4, too many dots";
			goto bad;
		}
		sprintf(tmp, "%s %s", date, time);
		d->sdate = strdup(tmp);
		d->user = strdup(user);
		for (;;) {
			if (!(buf = fastnext(s)) || buf[0] != '\001') {
				expected = "^A";
				goto bad;
			}
			line++;
			/*
			 * Sun apparently puts empty lists in the file.
			 * Ignore those lines.
			 */
			switch (buf[1]) {
			    case 'i':
			    case 'x':
			    case 'g':
				if ((buf[2] == '\n') || (buf[3] == '\n')) {
					continue;
				}
			}

			switch (buf[1]) {
			    case 'c':
				i = 0; goto comment;
			    case 'i':
				d->include = getserlist(s, 1, &buf[3], 0);
				break;
			    case 'x':
				d->exclude = getserlist(s, 1, &buf[3], 0);
				break;
			    case 'g':
		    		s->state |= S_READ_ONLY;
				fprintf(stderr, "ignore serials unsupported\n");
				break;
			    case 'e':
				goto done;
			    case 'm':
		    		s->state |= S_READ_ONLY;
				fprintf(stderr, "mr records unsupported\n");
				break;
			    default:
				fprintf(stderr, "Bad file format: ");
				expected = "^A{c,i,x,g,e}";
				goto bad;
			}
		}
		/* ^Ac branch. */
		for (;;) {
			if (!(buf = fastnext(s)) || buf[0] != '\001') {
				expected = "^A";
				freeLines(d->comments, free);
				goto bad;
			}
			line++;
comment:		switch (buf[1]) {
			    case 'e': goto done;
			    case 'c':
				if (buf[2] == '_') {	/* strip it */
					;
				} else if (buf[2] != ' ') {
					meta(s, d, buf);
				} else {
					d->comments =
					    addLine(d->comments,
					    strnonldup(&buf[3]));
				}
				break;
			    default:
				expected = "^A{e,c}";
				freeLines(d->comments, free);
				goto bad;
			}
		}
done:		if (CSET(s) && (d->type == 'R') &&
		    !d->symGraph && !(d->flags & D_SYMBOLS)) {
			MK_GONE(s, d);
		}
	}

	/*
	 * Convert the linear delta table into a graph.
	 *
	 * You would think that this is the place to adjust the times,
	 * but it isn't because we used to store the timezones in the flags.
	 * XXX - the above comment is incorrect, we no longer support that.
	 */
	s->tree = d;
	sccs_inherit(s, d);
	d = d->kid;
	s->tree->kid = 0;
	while (d) {
		delta	*therest = d->kid;

		d->kid = 0;
		dinsert(s, d, 0);
		d = therest;
	}
	unless (flags & INIT_WACKGRAPH) {
		if (checkrevs(s, 0)) s->state |= S_BADREVS|S_READ_ONLY;
	}

	/*
	 * For all the metadata nodes, go through and propogate the data up to
	 * the real node.
	 */
	if (CSET(s)) metaSyms(s);

	debug((stderr, "mkgraph() done\n"));
}

/*
 * Read the stuff after the ^au (which has already been read).
 * ^au
 * user
 * user
 * group name (ATT SCCS allows only numbers, maybe we should too)
 * ^aU
 * ^af whatever
 * ^at
 * Descriptive text (pass through).
 * ^aT
 * flags
 */
private int
misc(sccs *s)
{
	char	*buf;

	/* Save the users / groups list */
	for (; (buf = fastnext(s)) && !strneq(buf, "\001U\n", 3); ) {
		if (buf[0] == '\001') {
			fprintf(stderr, "%s: corrupted user section.\n",
			    s->sfile);
			return (-1);
		}
		s->usersgroups = addLine(s->usersgroups, strnonldup(buf));
	}

	/* Save the flags.  Some are handled by the flags routine; those
	 * are not handled here.
	 */
	for (; (buf = fastnext(s)) && !strneq(buf, "\001t\n", 3); ) {
		if (strneq(buf, "\001f &", 4) ||
		    strneq(buf, "\001f z _", 6)) {	/* XXX - obsolete */
			/* We strip these now */
			continue;
		} else if (strneq(buf, "\001f x", 4)) { /* strip it */
			unless (sccs_xflags(sccs_top(s))) {
				/* hex or dec */
				s->tree->xflags =
					strtol(&buf[5], 0, 0) & ~X_SINGLE;
				s->tree->flags |= D_XFLAGS;
			}
			continue;
		} else if (strneq(buf, "\001f e ", 5)) {
			switch (atoi(&buf[5])) {
			    case E_ASCII:
			    case E_ASCII|E_GZIP:
			    case E_UUENCODE:
			    case E_UUENCODE|E_GZIP:
			    case E_BINPOOL:
				s->encoding = atoi(&buf[5]);
				break;
			    default:
				fprintf(stderr,
				    "sccs: don't know encoding %d, "
				    "assuming ascii\n",
				    atoi(&buf[5]));
				s->encoding = E_ASCII;
				return (-1);
			}
			continue;
		}
		if (!getflags(s, buf)) {
			s->flags = addLine(s->flags, strnonldup(&buf[3]));
		}
	}

	/* Save descriptive text. AT&T/teamware might have more after T */
	for (; (buf = fastnext(s)) &&
	    !strneq(buf, "\001T\n", BITKEEPER(s) ? 3 : 2); ) {
		s->text = addLine(s->text, strnonldup(buf));
	}
	s->data = sccstell(s);
	return (0);
}

/*
 * Reads until a space without a preceeding \.
 * XXX - counts on s[-1] being a valid address.
 */
private char *
gettok(char **sp)
{
	char	*t;
	char	*s = *sp;
	static char	copy[MAXLINE];

	t = copy;
	*t = 0;
	while ((*t = *s++)) {
		if (*t == '\n') {
			*t = 0;
			*sp = s;
			return (copy);
		}
		if (*t == ' ') {
			if (t[-1] == '\\') {
				t--;
				*t = ' ';
			} else {
				*t = 0;
				*sp = s;
				return (copy);
			}
		}
		t++;
	}
	*sp = s;
	return (copy);
}

/*
 * Look at the flag and if we handle in this routine, return true.
 * Otherwise, return false so it gets passed through.
 *
 * We handle here:
 *	d	<rev> default branch
 */
private int
getflags(sccs *s, char *buf)
{
	char	*p = &buf[5];
	char	f = buf[3];
	char	*t;
	delta	*d = 0;

	if (buf[0] != '\001' || buf[1] != 'f' || buf[2] != ' ') return (0);
	t = gettok(&p);
	assert(t);
	if ((f != 's') && (f != 'd') &&
	    (!HASGRAPH(s) || !(d = rfind(s, t)))) {
		/* ignore it. but keep it. */
		return (0);
	}
	if (f == 'd') {
		if (s->defbranch) free(s->defbranch);
		s->defbranch = strdup(t);
		return (1);
	}
	return (0);
}

/*
 * Add a symbol to the symbol table.
 * Return 0 if we added it, 1 if it's a dup.
 */
int
addsym(sccs *s, delta *d, delta *metad, int graph, char *rev, char *val)
{
	symbol	*sym, *s2, *s3;

	/* If we can't find it, just pass it through */
	if (!d && !(d = rfind(s, rev))) return (0);

	sym = findSym(s->symbols, val);

	/*
	 * If rev is NULL, it means we have a new delta with
	 * unallocated rev, force add the symbol. Caller
	 * is responsible to run "bk renumber" to fix up
	 * the rev. This is used by the cweave code.
	 */
	if (sym && rev && streq(sym->rev, rev)) {
		return (1);
	} else {
		sym = calloc(1, sizeof(*sym));
		assert(sym);
	}
	sym->rev = rev ? strdup(rev) : NULL;
	sym->symname = strdup(val);
	sym->d = d;
	sym->metad = metad;
	d->flags |= D_SYMBOLS;
	metad->flags |= D_SYMBOLS;
	DATE(d);
	CHKDATE(d);

	/*
	 * Insert in sorted order, most recent first.
	 */
	if (!s->symbols || !s->symbols->d) {
		sym->next = s->symbols;
		s->symbols = sym;
	} else if (metad->date >= s->symbols->d->date) {
		sym->next = s->symbols;
		s->symbols = sym;
	} else {
		for (s3 = 0, s2 = s->symbols; s2; s3 = s2, s2 = s2->next) {
			if (!s2->d || (metad->date >= s2->d->date)) {
				sym->next = s2;
				s3->next = sym;
				break;
			}
		}
		/* insert at end */
		if (!sym->next) {
			if (s3) {
				s3->next = sym;
			} else {
				s->symbols = sym;
			}
		}
	}
	debug((stderr, "Added symbol %s->%s (%d,%d) in %s\n",
	    val, rev, d->serial, metad->serial, s->sfile));
	if (graph) symGraph(s, metad);
	return (0);
}


/* [user][@host][:path] */
private	remote *
pref_parse(char *buf)
{
	remote	*r;
	char	*p;
	int 	want_path = 0, want_host = 0;

	new(r);
	r->rfd = r->wfd = -1;
	unless (*buf) return (r);
	/* user */
	if (p = strchr(buf, '@')) {
		if (buf != p) {
			*p = 0; r->user = strdup(buf);
		}
		buf = p + 1;
		want_host = 1;
	}
	/* host */
	if (p = strchr(buf, ':')) {
		if (buf != p) {
			if (r->user || want_host) {
				*p = 0; r->host = strdup(buf);
				want_host = 0;
			} else {
				*p = 0; r->user = strdup(buf);
			}
		}
		buf = p + 1;
		want_path = 1;
	}
	if (*buf) {
		if (want_path) {
			r->path = strdup(buf);
		} else if (want_host) {
			r->host = strdup(buf);
		} else {
			assert(r->user == NULL);
			r->user = strdup(buf);
		}
	}
	return (r);
}

/*
 * filter: return 1 if match
 */
private int
filter(char *buf)
{
	remote *r;
	char *h;
	char	*root;

	r = pref_parse(buf);
	if ((r->user) && !match_one(sccs_getuser(), r->user, 0)) {
no_match:	remote_free(r);
		return (0);
	}

	if (r->host) {
		h = sccs_gethost();
		unless (h && match_one(h, r->host, 1)) goto no_match;
	}

	if (r->path && (root = proj_root(0))) {
		unless (match_one(root, r->path, !mixedCasePath())) {
			goto no_match;
		}
	}

	remote_free(r);
	return (1);
}

private char	*
filterMatch(char *buf)
{
	char	*end = strchr(buf, ']');

	unless (end) return (0);
	*end = 0;
	unless (filter(++buf)) return (0);
	return (end);
}

private int
parseConfigKV(char *buf, int nofilter, char **kp, char **vp)
{
	char	*p = 0;

	/* trim leading whitespace */
	while (*buf && isspace(*buf)) buf++;

	/* handle leading filters */
	if (*buf == '[') {
		if (nofilter) {
			/* include filter in key */
			unless (p = strchr(buf, ']')) return (0);
			for (p++; isspace(*p); p++);
		} else {
			/* match filter */
			unless (buf = filterMatch(buf)) return (0);
			for (buf++; isspace(*buf); buf++);
		}
	}
	unless (p) p = buf;
	p = strchr(p, ':');
	if ((*buf == '#') || !p) return (0);

	/*
	 * lose all white space on either side of ":"
	 */
	while ((p >= buf) && isspace(p[-1])) --p;
	if (*p != ':') {
		*p = 0;
		for (p++; *p != ':'; p++);
	}
	for (*p++ = 0; isspace(*p); p++);

	if (streq(buf, "logging_ok")) return (0);

	*kp = buf;
	*vp = p;

	/*
	 * Lose trailing whitespace including newline.
	 */
	while (p[1]) p++;
	while (isspace(*p)) *p-- = 0;
//fprintf(stderr, "[%s] -> [%s]\n", *kp, *vp);
	return (1);
}

/*
 * parse a line of config file and insert that line into 'db'.
 */
private void
parseConfig(char *buf, MDBM *db)
{
	char	*k, *v, *p;
	int	flags = MDBM_INSERT;

	unless (parseConfigKV(buf, 0, &k, &v)) return;
	if (*v) {
		for (p = v; p[1]; p++);	/* find end of value */
		if (*p == '!') {
			*p = 0;
			flags = MDBM_REPLACE;
		}
	}
	mdbm_store_str(db, k, v, flags);
}

private void
config2mdbm(MDBM *db, char *config)
{
	FILE	*f;
	char 	buf[MAXLINE];

	if (f = fopen(config, "rt")) {
		while (fnext(buf, f)) parseConfig(buf, db);
		fclose(f);
	}
}

/*
 * Load config file into a MDBM DB
 */
private MDBM *
loadRepoConfig(char *root)
{
	MDBM	*DB = 0;
	sccs	*s = 0;
	char	config[MAXPATH];

	/*
	 * If the config is already checked out, use that.
	 */
	concat_path(config, root, "/BitKeeper/etc/config");
	if (exists(config)) {
		DB = mdbm_mem();
		config2mdbm(DB, config);
		return (DB);
	}

	/*
	 * No g file, so load it directly from the s.file
	 */
	concat_path(config, root, "BitKeeper/etc/SCCS/s.config");
	unless (exists(config)) return (0);

	unless (s = sccs_init(config, SILENT)) return (0);
	s->state |= S_CONFIG; /* This should really be stored on disk */
	if (sccs_get(s, 0, 0, 0, 0, SILENT|GET_HASH|GET_HASHONLY, 0)) {
		sccs_free(s);
		return (0);
	}
	DB = s->mdbm;
	s->mdbm = 0;
	sccs_free(s);
	return (DB);
}

/*
 * "Append" Global config to local config.
 * I.e local field have priority over global field.
 * If local field exists, it masks out the global counter part.
 */
private MDBM *
loadGlobalConfig(MDBM *db)
{
	char 	*config;

	assert(db);
	config = aprintf("%s/BitKeeper/etc/config", globalroot());
	config2mdbm(db, config);
	free(config);
	return(db);
}

/*
 * "Append" bin config to local config.
 * I.e local field have priority over global field.
 * If local field exists, it masks out the global counter part.
 */
MDBM *
loadBinConfig(MDBM *db)
{
	char 	*config;

	assert(db);
	config = aprintf("%s/config", bin);
	config2mdbm(db, config);
	free(config);
	return(db);
}


/*
 * "Append" .bk/config config to local config.
 * I.e local field have priority over global field.
 * If local field exists, it masks out the global counter part.
 */
MDBM *
loadDotBkConfig(MDBM *db)
{
	char 	*config;

	assert(db);
	config = aprintf("%s/config", getDotBk());
	config2mdbm(db, config);
	free(config);
	return(db);
}

/*
 * Override the config db with values from the BK_CONFIG enviromental
 * variable if it exists.
 *
 * BK_CONFIG='var1:value1;var2:values2'
 */
private MDBM *
loadEnvConfig(MDBM *db)
{
	char	*env = getenv("BK_CONFIG");
	char	**values;
	int	i;

	unless (env) return (db);
	assert(db);
	values = splitLine(env, ";", 0);
	EACH (values) parseConfig(values[i], db);
	freeLines(values, free);
	return (db);
}

/*
 * Load both local and global config
 *
 * if forcelocal is set, then we must find a repo config file.
 */
MDBM *
loadConfig(char *root, int forcelocal)
{
	MDBM	*db;

	unless (db = loadRepoConfig(root)) {
		if (forcelocal) return (0);
		db = mdbm_mem();
	}
	loadDotBkConfig(db);
	unless (getenv("BK_REGRESSION")) loadGlobalConfig(db);
	loadBinConfig(db);
	loadEnvConfig(db);
	return (db);
}

private void
printconfig(char *file, MDBM *db, MDBM *cfg)
{
	kvpair	kv;
	int	i, j;
	char	*k, *v1, *v2, *t;
	char	**keys = 0;
	FILE	*f;
	MDBM	*freeme = 0;
	char 	buf[MAXLINE];

	unless (db) {
		db = freeme = mdbm_mem();

		if (f = fopen(file, "rt")) {
			while (fnext(buf, f)) {
				unless (parseConfigKV(buf, 0, &k, &v1)) {
					continue;
				}
				mdbm_store_str(db, k, v1, MDBM_INSERT);
			}
			fclose(f);
		}
	}
	EACH_KV(db) keys = addLine(keys, kv.key.dptr);
	sortLines(keys, 0);
	printf("%s:\n", file);
	EACH(keys) {
		k = keys[i];
		v1 = mdbm_fetch_str(db, k);
		assert(v1);
		v2 = mdbm_fetch_str(cfg, k);
		assert(v2);

		/* mark the config that doesn't get used */
		if (!*v1 || streq(v1, "!")) {
			if (*v2) putchar('#');
		} else {
			t = strdup(v1);
			j = strlen(t) - 1;
			if (t[j] == '!') t[j] = 0;
			unless (streq(t, v2)) putchar('#');
			free(t);
		}
		printf("\t%s:", k);
		if ((j = 15 - strlen(k)) > 0) {
			while (j--) putchar(' ');
		}
		puts(v1);
	}
	freeLines(keys, 0);
	putchar('\n');
	if (freeme) mdbm_close(freeme);
}

/*
 * merge some new config keys into an existing config file.
 * This is used by the installer to add config items embedded in the
 * installer into the `bk bin` config file.
 */
private int
config_merge(char *file1, char *file2)
{
	hash	*config = hash_new(HASH_MEMHASH);
	FILE	*f;
	char	*k, *v;
	char	**keys = 0;
	int	i;
	char	buf[MAXLINE];

	/* load second file into hash */
	if (streq(file2, "-")) {
		f = stdin;
	} else {
		unless (f = fopen(file2, "r")) {
			perror(file2);
			return (1);
		}
	}
	while (fnext(buf, f)) {
		if (parseConfigKV(buf, 1, &k, &v)) {
			hash_storeStr(config, k, v);
		}
	}
	unless (f == stdin) fclose(f);

	/* copy first file and keep track of keys */
	unless (f = fopen(file1, "r")) {
		perror(file1);
		return (1);
	}
	while (fnext(buf, f)) {
		fputs(buf, stdout);
		if (parseConfigKV(buf, 1, &k, &v)) {
			hash_deleteStr(config, k);
		}
	}
	fclose(f);

	/* print any keys remaining from file2 */
	EACH_HASH(config) keys = addLine(keys, config->kptr);
	sortLines(keys, 0);
	EACH(keys) {
		printf("%s: %s\n", keys[i], hash_fetchStr(config, keys[i]));
	}
	freeLines(keys, 0);
	hash_free(config);
	return (0);
}

int
config_main(int ac, char **av)
{
	char	*root, *file, *env;
	MDBM	*db;
	MDBM	*cfg = proj_config(0);
	char	**values = 0;
	char	*k, *v;
	int	i, c;
	int	verbose = 0;
	int	merge = 0;
	kvpair	kv;

	while ((c = getopt(ac, av, "mv")) != -1) {
		switch (c) {
		    case 'm': merge = 1; break;
		    case 'v': verbose = 1; break;
		    default:
usage:			sys("bk", "help", "-s", "config", SYS);
			return (1);
		}
	}
	if (merge) {
		unless ((ac - optind) == 2) goto usage;
		if (verbose) goto usage;
		return (config_merge(av[optind], av[optind+1]));
	}
	unless (verbose) {
		if (av[optind]) {
			if (av[optind+1]) goto usage;
			if (v = mdbm_fetch_str(cfg, av[optind])) {
				puts(v);
				return (0);
			} else {
				return (1);
			}
		}
		EACH_KV(cfg) {
			values = addLine(values,
			    aprintf("%s: %s", kv.key.dptr, kv.val.dptr));
		}
		sortLines(values, 0);
		EACH(values) puts(values[i]);
		freeLines(values, free);
		return (0);
	}
	if (av[optind]) goto usage;

	if (root = proj_root(0)) {
		file = aprintf("%s/BitKeeper/etc/config", root);
		unless (exists(file)) get(file, SILENT|GET_EXPAND, "-");
		printconfig(file, 0, cfg);
		free(file);
	}

	/* dotbk config */
	file = aprintf("%s/config", getDotBk());
	printconfig(file, 0, cfg);
	free(file);

	unless (getenv("BK_REGRESSION")) {
		/* global config */
		file = aprintf("%s/BitKeeper/etc/config", globalroot());
		printconfig(file, 0, cfg);
		free(file);
	}

	/* bin config */
	file = aprintf("%s/config", bin);
	printconfig(file, 0, cfg);
	free(file);

	/* $BK_CONFIG */
	db = mdbm_mem();
	if (env = getenv("BK_CONFIG")) values = splitLine(env, ";", 0);
	EACH (values) {
		unless (parseConfigKV(values[i], 0, &k, &v)) continue;
		mdbm_store_str(db, k, v, MDBM_REPLACE);
	}
	freeLines(values, free);
	printconfig("$BK_CONFIG", db, cfg);
	mdbm_close(db);

	return (0);
}

/*
 * Return 0 for OK, -1 for error.
 */
int
check_gfile(sccs *s, int flags)
{
	struct	stat sbuf;

	if (lstat(s->gfile, &sbuf) == 0) {
		unless ((flags & INIT_NOGCHK) || fileTypeOk(sbuf.st_mode)) {
			verbose((stderr,
			    "unsupported file type: %s (%s) 0%06o\n",
			    s->sfile, s->gfile, sbuf.st_mode & 0177777));
err:			sccs_free(s);
			return (-1);
		}
		s->state |= S_GFILE;
		s->mode = sbuf.st_mode;
		s->gtime = sbuf.st_mtime;
		if (S_ISLNK(sbuf.st_mode)) {
			char link[MAXPATH];
			int len;

			s->mode |= 0777; /* hp11 is weird */
			len = readlink(s->gfile, link, sizeof(link));
			if ((len > 0 )  && (len < sizeof(link))){
				link[len] = 0;
				if (s->symlink) free(s->symlink);
				s->symlink = strdup(link);
			} else {
				verbose((stderr,
				    "cannot read sym link: %s\n", s->gfile));
				goto err;
			}
		}
	} else {
		s->state &= ~S_GFILE;
		s->mode = 0;
	}
	return (0);
}

/*
 * Initialize an SCCS file.  Do this before anything else.
 * If the file doesn't exist, the graph isn't set up.
 * It should be OK to have multiple files open at once.
 * If the project is passed in, use it, else init one if we are in BK mode.
 */
sccs*
sccs_init(char *name, u32 flags)
{
	sccs	*s;
	struct	stat sbuf;
	char	*t;
	int	rc;
	delta	*d;
	int	fixstime = 0;
	static	int _YEAR4;
	static	char *glob = 0;
	static	int show = -1;

	if (show == -1) {
		glob = getenv("BK_SHOWINIT");
		show = glob != 0;
	}
	if (show && match_one(name, glob, 0)) ttyprintf("init(%s)\n", name);

	if (strchr(name, '\n') || strchr(name, '\r')) {
		fprintf(stderr,
		   "bad file name, file name must not contain LF or CR "
		   "character\n");
		return (0);
	}
	localName2bkName(name, name);
	if (sccs_filetype(name) == 's') {
		s = calloc(1, sizeof(*s));
		s->sfile = strdup(name);
		s->gfile = sccs2name(name);
	} else {
		fprintf(stderr, "Not an SCCS file: %s\n", name);
		return (0);
	}

	s->initFlags = flags;
	t = strrchr(s->sfile, '/');
	if (t) {
		*t = 0;
		s->proj = proj_init(s->sfile);
		*t = '/';
	} else {
		s->proj = proj_init(".");
	}

	if (isCsetFile(s->sfile)) {
		s->xflags |= X_HASH;
		s->state |= S_CSET;
	}

	if (flags & INIT_NOSTAT) {
		if ((flags & INIT_HASgFILE) && check_gfile(s, flags)) return 0;
	} else {
		if (check_gfile(s, flags)) return (0);
	}
	rc = lstat(s->sfile, &sbuf);
	if (rc == 0) {
		if (!S_ISREG(sbuf.st_mode)) {
			verbose((stderr, "Not a regular file: %s\n", s->sfile));
 err:			free(s->gfile);
			free(s->sfile);
			proj_free(s->proj);
			free(s);	/* We really mean free, not sccs_free */
			return (0);
		}
		if (sbuf.st_size == 0) {
			verbose((stderr, "Zero length file: %s\n", s->sfile));
			goto err;
		}
		s->state |= S_SFILE;
		s->size = sbuf.st_size;
		s->stime = sbuf.st_mtime;	/* for GET_DTIME */

		/*
		 * Catch any case where we would cause make to barf to
		 * fail if we are BK folks, otherwise fix it.
		 */
		if ((flags & INIT_CHK_STIME) &&
		    s->gtime && (sbuf.st_mtime > s->gtime)) {
			if ((t = getenv("_BK_DEVELOPER")) && *t) {
				fprintf(stderr,
				    "timestamp %s\n\ts\t%u\n\tgtime\t%u\n\t",
				    s->gfile, sbuf.st_mtime, s->gtime);
				if (lstat(s->gfile, &sbuf)) sbuf.st_mtime = 0;
				fprintf(stderr, "g\t%u\n", sbuf.st_mtime);
				exit(1);
			}
			fixstime = 1;
		}
	} else if (CSET(s)) {
		int	bad;
		/* t still points at last slash in s->sfile */
		assert(*t == '/');

		t[1] = 'q';
		bad = exists(s->sfile);
		t[1] = 's';
		if (bad) {
			fprintf(stderr,
"Unable to proceed.  ChangeSet file corrupted.  error=57\n"
"Please run 'bk support' to request assistance.\n");
			goto err;
		}
	}
	s->pfile = strdup(sccsXfile(s, 'p'));
	s->zfile = strdup(sccsXfile(s, 'z'));
	if (flags & INIT_NOSTAT) {
		if (flags & INIT_HASpFILE) s->state |= S_PFILE;
		if (flags & INIT_HASzFILE) s->state |= S_ZFILE;
	} else {
		if (isreg(s->pfile)) s->state |= S_PFILE;
		if (isreg(s->zfile)) s->state |= S_ZFILE;
	}
	debug((stderr, "init(%s) -> %s, %s\n", s->gfile, s->sfile, s->gfile));
	s->nextserial = 1;
	s->fd = -1;
	s->mmap = (caddr_t)-1;
	sccs_open(s, &sbuf);

	if (s->mmap == (caddr_t)-1) {
		if ((errno == ENOENT) || (errno == ENOTDIR)) {
			/* Not an error if the file doesn't exist yet.  */
			debug((stderr, "%s doesn't exist\n", s->sfile));
			s->cksumok = 1;		/* but not done */
			goto out;
		} else {
			fputs("sccs_init: ", stderr);
			perror(s->sfile);
			free(s->sfile);
			free(s->gfile);
			free(s->pfile);
			proj_free(s->proj);
			free(s);	/* We really mean free, not sccs_free */
			return (0);
		}
	}
	debug((stderr, "mapped %s for %d at 0x%p\n",
	    s->sfile, (int)s->size, s->mmap));
	if (((flags&INIT_NOCKSUM) == 0) && badcksum(s, flags)) {
		goto out;
	} else {
		s->cksumok = 1;
	}
	mkgraph(s, flags);
	debug((stderr, "mkgraph found %d deltas\n", s->numdeltas));
	if (HASGRAPH(s)) {
		if (misc(s)) {
			sccs_free(s);
			return (0);
		}

		/*
		 * get the xflags from the delta graph
		 * instead of the sccs flag section
		 */
		s->xflags = sccs_xflags(sccs_top(s));
		unless (BITKEEPER(s)) s->xflags |= X_SCCS;

		/*
		 * Don't allow them to check in a gfile of a different type.
		 */
		if (HAS_GFILE(s)) {
			for (d = s->table; TAG(d); d = d->next);
			assert(d);
			if ((d->flags & D_MODE) &&
			    (fileType(d->mode) != fileType(s->mode))) {
				verbose((stderr,
				    "%s has different file types, treating "
				    "this file as read only.\n", s->gfile));
		    		s->state |= S_READ_ONLY;
			}
		}
	}

	/*
	 * Let them force YEAR4
	 */
	unless (_YEAR4) _YEAR4 = getenv("BK_YEAR2") ? -1 : 1;
	if (_YEAR4 == 1) s->xflags |= X_YEAR4;

	if (sig_ignore() == 0) s->unblock = 1;
	lease_check(s->proj, O_RDONLY, s);
 out:
	if (fixstime) sccs_setStime(s, s->stime); /* only make older */
	return (s);
}

/*
 * Restart an sccs_init because we've changed the state of the file.
 *
 * This does not reread the delta table, if you want that, use sccs_reopen().
 */
sccs*
sccs_restart(sccs *s)
{
	struct	stat sbuf;
	char	*buf;

	assert(s);
	if (check_gfile(s, 0)) return (0);
	bzero(&sbuf, sizeof(sbuf));	/* file may not be there */
	if (lstat(s->sfile, &sbuf) == 0) {
		if (!S_ISREG(sbuf.st_mode)) {
bad:			sccs_free(s);
			return (0);
	}
		if (sbuf.st_size == 0) goto bad;
		s->state |= S_SFILE;
	}
	unless ((s->state & S_SOPEN) && (s->size == sbuf.st_size)) {
		sccs_close(s);
		if (sccs_open(s, &sbuf)) return (s);
		seekto(s, 0);
		while(buf = fastnext(s)) if (strneq(buf, "\001T\n", 3)) break;
		s->data = sccstell(s);
	}
	if (isreg(s->pfile)) {
		s->state |= S_PFILE;
	} else {
		s->state &= ~S_PFILE;
	}
	if (isreg(s->zfile)) {
		s->state |= S_ZFILE;
	} else {
		s->state &= ~S_ZFILE;
	}
	if (s->mdbm) {
		mdbm_close(s->mdbm);
		s->mdbm = 0;
	}
	if (s->findkeydb) {
		mdbm_close(s->findkeydb);
		s->findkeydb = 0;
	}
	return (s);
}

sccs	*
sccs_reopen(sccs *s)
{
	sccs	*s2;

	assert(s);
	sccs_close(s);
	s2 = sccs_init(s->sfile, s->initFlags);
	assert(s2);
	sccs_free(s);
	return (s2);
}

/*
 * open & mmap the file.
 * Use this after an sccs_close() to reopen,
 * use sccs_reopen() if you need to reread the graph.
 * Note: this is used under windows via win32_open().
 */
int
sccs_open(sccs *s, struct stat *sp)
{
	assert(s);
	if (s->state & S_SOPEN) {
		assert(s->fd != -1);
		assert(s->mmap != (caddr_t)-1L);
		return (0);
	} else {
		assert(s->fd == -1);
		assert(s->mmap == (caddr_t)-1);
	}
	if ((s->fd = open(s->sfile, O_RDONLY, 0)) == -1) return (-1);
	if (sp) {
		s->size = sp->st_size;
	} else {
		struct	stat sbuf;

		fstat(s->fd, &sbuf);
		s->size = sbuf.st_size;
	}
	s->mmap = mmap(0, s->size, PROT_READ, MAP_SHARED, s->fd, 0);
#if     defined(hpux)
	if (s->mmap == (caddr_t)-1) {
		/*
		 * HP-UX won't let you have two shared mmap to the same file.
		 */
		debug((stderr,
		       "MAP_SHARED failed, trying MAP_PRIVATE\n"));
		s->mmap =
		    mmap(0, s->size, PROT_READ, MAP_PRIVATE, s->fd, 0);
		s->state |= S_MAPPRIVATE;
	}
#endif
	if (s->mmap == (caddr_t)-1) {
		close(s->fd);
		s->fd = -1;
		return (-1);
	}
	s->state |= S_SOPEN;
	return (0);
}

/*
 * close all open file stuff associated with an sccs structure.
 * Note: this is used under windows via win32_close().
 */
void
sccs_close(sccs *s)
{
	if (s->state & S_SOPEN) {
		assert(s->fd != -1);
		assert(s->mmap != (caddr_t)-1L);
	} else {
		assert(s->fd == -1);
		assert(s->mmap == (caddr_t)-1L);
		return;
	}
	munmap(s->mmap, s->size);
	close(s->fd);
	s->mmap = (caddr_t) -1;
	s->fd = -1;
	s->state &= ~S_SOPEN;
}

/*
 * a) If gfile exists and writable, insist p.file exist
 * b) If p.file exists, insist gfile writable or absent
 * Note 1: "delta -n" and "admin -i" violate  'a'
 * Note 2: "edit -g" may violate 'b'
 */
private void
chk_gmode(sccs *s)
{
	struct	stat sbuf;
	int	pfileExists, gfileExists, gfileWritable;
	char 	*gfile;

	unless (getenv("_BK_GMODE_DEBUG")) return;
	if (!s || !HASGRAPH(s)) return; /* skip new file */

	gfile = sccs2name(s->sfile); /* Don't trust s->gfile, see bk admin -i */
	gfileExists = !lstat(gfile, &sbuf);
	gfileWritable = (gfileExists && (sbuf.st_mode & 0200));
	if (S_ISLNK(sbuf.st_mode)) return; /* skip smylink */
	pfileExists = exists(sccs_Xfile(s, 'p'));

	if (gfileWritable) {
		unless (pfileExists) {
			fprintf(stderr,
			    "ERROR: %s: writable gfile with no p.file\n",
			    gfile);
			assert("writable gfile with no p.file" == 0);
		};
	}

	if (pfileExists) {
		if (gfileExists && !gfileWritable) {
			fprintf(stderr,
			    "ERROR: %s: p.file with read only gfile\n",
			    gfile);
			assert("p.file with read only gfile" == 0);
		}
	}
	free(gfile);
}

/*
 * Free up all resources associated with the file.
 * This is the last thing you do.
 */
void
sccs_free(sccs *s)
{
	symbol	*sym, *t;
	int	unblock;

	unless (s) return;
	if (s->io_error && !s->io_warned) {
		fprintf(stderr, "%s: unreported I/O error\n", s->sfile);
	}
	chk_gmode(s);
	sccsXfile(s, 0);
	if (s->table) sccs_freetable(s->table);
	for (sym = s->symbols; sym; sym = t) {
		t = sym->next;
		if (sym->symname) free(sym->symname);
		if (sym->rev) free(sym->rev);
		free(sym);
	}
	if (s->state & S_SOPEN) sccs_close(s); /* move this up for trace */
	if (s->sfile) free(s->sfile);
	if (s->gfile) free(s->gfile);
	if (s->zfile) free(s->zfile);
	if (s->pfile) free(s->pfile);
	if (s->state & S_CHMOD) {
		struct	stat sbuf;

		if (fstat(s->fd, &sbuf) == 0) {
			sbuf.st_mode &= ~0200;
			chmod(s->sfile, sbuf.st_mode & 0777);
		}
	}
	if (s->defbranch) free(s->defbranch);
	if (s->ser2delta) free(s->ser2delta);
	freeLines(s->usersgroups, free);
	freeLines(s->flags, free);
	freeLines(s->text, free);
	if (s->symlink) free(s->symlink);
	if (s->mdbm) mdbm_close(s->mdbm);
	if (s->findkeydb) mdbm_close(s->findkeydb);
	if (s->locs) free(s->locs);
	if (s->proj) proj_free(s->proj);
	if (s->rrevs) freenames(s->rrevs, 1);
	unblock = s->unblock;
	bzero(s, sizeof(*s));
	free(s);
	if (unblock) sig_default();
}

/*
 * open the ChangeSet file if can be found.  We are not necessarily
 * at the root, nor do we want to go to the root forcefully, as the
 * command line parameters may not be root relative
 */

sccs	*
sccs_csetInit(u32 flags)
{
	char	*rootpath;
	char	csetpath[MAXPATH];
	sccs	*cset = 0;

	unless (rootpath = proj_root(0)) return (0);
	if (samepath(rootpath, ".")) {
		strcpy(csetpath, CHANGESET);
	} else {
		strcpy(csetpath, rootpath);
		strcat(csetpath, "/" CHANGESET);
	}
	debug((stderr, "sccs_csetinit: opening changeset '%s'\n", csetpath));
	cset = sccs_init(csetpath, flags);
	return (cset);
}

/*
 * We want SCCS/s.foo or path/to/SCCS/s.foo
 * ATT allows s.foo or path/to/s.foo.
 * We insist on SCCS/s. unless in ATT compat mode.
 * XXX ATT compat mode sucks - it's really hard to operate on a
 * gfile named s.file .
 *
 * This returns the following:
 *	'c'	this is an SCCS pathname (whatever/SCCS/c.whatever)
 *	'd'	this is an SCCS pathname (whatever/SCCS/d.whatever)
 *	'm'	this is an SCCS pathname (whatever/SCCS/m.whatever)
 *	'p'	this is an SCCS pathname (whatever/SCCS/p.whatever)
 *	'r'	this is an SCCS pathname (whatever/SCCS/r.whatever)
 *	's'	this is an SCCS pathname (whatever/SCCS/s.whatever)
 *	'x'	this is an SCCS pathname (whatever/SCCS/x.whatever)
 *	'z'	this is an SCCS pathname (whatever/SCCS/z.whatever)
 *	0	none of the above
 */
int
sccs_filetype(char *name)
{
	char	*s = rindex(name, '/');

	if (!s) {
#ifdef	ATT_SCCS
		if (name[0] == 's' && name[1] == '.') return ((int)'s');
#endif
		return (0);
	}
	unless (s[1] && (s[2] == '.')) return (0);
	switch (s[1]) {
	    case 'c':	/* comments files */
	    case 'd':	/* delta pending */
	    case 'm':	/* name resolve files */
	    case 'p':	/* lock files */
	    case 'r':	/* content resolve files */
	    case 's':	/* sccs file */
	    case 'x':	/* temp file, about to be s.file */
	    case 'z':	/* lock file */
	    	break;
	    default:  return (0);
	}
#ifdef	ATT_SCCS
	return ((int)s[1]);
#endif
	if ((name <= &s[-4]) && pathneq("SCCS", &s[-4], 4)) {
		return ((int)s[1]);
	}
	return (0);
}

/*
 * remove SCCS/s. or just the s.
 * It's up to the caller to free() the resulting name.
 */
char	*
sccs2name(char *sfile)
{
	char	*s, *t;
	char	*new = strdup(sfile);

	s = rindex(new, '/');
	if (!s) {
		assert(new[0] == 's' && new[1] == '.');
		t = new;
		s = &new[2];
	} else {
		assert(s[1] == 's' && s[2] == '.');
		if ((s >= (new + 4)) &&
		    (s[-1] == 'S') && (s[-2] == 'C') &&
		    (s[-3] == 'C') && (s[-4] == 'S')) {
			t = &s[-4];
			s = &s[3];
		} else {
			t = &s[1];
			s = &s[3];
		}
	}
	while ((*t++ = *s++));
	return (new);
}

/*
 * Make the sccs dir if we need one.
 */
void
mksccsdir(char *sfile)
{
	char	*s = rindex(sfile, '/');

	if (!s) return;
	if ((s >= sfile + 4) &&
	    s[-1] == 'S' && s[-2] == 'C' && s[-3] == 'C' && s[-4] == 'S') {
		*s = 0;
		mkdir(sfile, 0777);
		*s = '/';
	}
}

/*
 * Take a file name such as foo.c and return SCCS/s.foo.c
 * Also works for /full/path/foo.c -> /fullpath/SCCS/s.foo.c.
 * It's up to the caller to free() the resulting name.
 */
char	*
name2sccs(char *name)
{
	int	len = strlen(name);
	char	*s, *newname;

	/* maybe it has the SCCS in it already */
	if (s = rindex(name, '/')) s -= 4;	/* point it at start of SCCS/ */
	unless (s >= name) s = 0;

	/* DIR_WITH_SCCS/GOTTEN screwed us up, this should fix it */
	if (s && strneq(s, "SCCS/", 5) && ((s == name) || (s[-1] == '/'))) {
		unless (sccs_filetype(name)) return (0);
		name = strdup(name);
		s = strrchr(name, '/');
		s[1] = 's';
		return (name);
	}
	newname = malloc(len + 8);
	assert(newname);
	strcpy(newname, name);
	if ((s = rindex(newname, '/'))) {
		s++;
		strcpy(s, "SCCS/s.");
		s += 7;
		strcpy(s, rindex(name, '/') + 1);
	} else {
		strcpy(s = newname, "SCCS/s.");
		s += 7;
		strcpy(s, name);
	}
	return (newname);
}

private int
stale(char *file)
{
	char	buf[300];
	char	*t, *h = sccs_gethost();
	int	n, fd = open(file, 0, 0);

	if (fd == -1) return (0);
	if ((n = read(fd, buf, sizeof(buf))) <= 0) {
		close(fd);
		return (0);
	}
	buf[n-1] = 0;				/* null the newline */
	close(fd);
	unless (t = strchr(buf, ' ')) return (0);
	*t++ = 0;
	unless (streq(t, h)) return (0);	/* different hosts */
	for (n = 0; n < 100; n++) {		/* about a second */
		if (kill(atoi(buf), 0) == -1) {
			unlink(file);
			return (1);
		}
		usleep(10000);
	}
	return (0);
}

/*
 * create SCCS/<type>.foo.c
 */
int
sccs_lock(sccs *s, char type)
{
	char	*t;
	int	lockfd, verbose;

	if (READ_ONLY(s)) return (0);

	verbose = (s->state & SILENT) ? 0 : 1;
	if ((type == 'z') && repository_locked(s->proj)) return (0);

	/* get -e does Z lock so we can skip past the repository locks */
	if (type == 'Z') type = 'z';
	t = sccsXfile(s, type);
again:	lockfd =
	    open(t, O_CREAT|O_WRONLY|O_EXCL, type == 'z' ? 0444 : GROUP_MODE);
	debug((stderr, "lock(%s) = %d\n", s->sfile, lockfd >= 0));
	if ((lockfd == -1) && stale(t)) goto again;
	if (lockfd == -1) {
		return (0);
	}
	if (type == 'z') {
		char	buf[20];
		char	*h = sccs_gethost();

		sprintf(buf, "%u ", getpid());
		write(lockfd, buf, strlen(buf));
		if (h) {
			write(lockfd, h, strlen(h));
		} else {
			write(lockfd, "?", 1);
		}
		write(lockfd, "\n", 1);
		s->state |= S_ZFILE;
	}
	close(lockfd);
	return (1);
}

/*
 * Take SCCS/s.foo.c and unlink SCCS/<type>.foo.c
 */
int
sccs_unlock(sccs *sccs, char type)
{
	char	*s;
	int	failed;

	debug((stderr, "unlock(%s, %c)\n", sccs->sfile, type));
	s = sccsXfile(sccs, type);
	failed  = unlink(s);
	// XXX This seems to a bug, we should only reset S_ZFILE if type == 'z'
	unless (failed) sccs->state &= ~S_ZFILE;
	return (failed);
}

/*
 * Take SCCS/s.foo.c, type and return a temp copy of SCCS/<type>.foo.c
 */
private char *
sccsXfile(sccs *sccs, char type)
{
	static	char	*s = 0;
	static	int	len = 0;
	char	*t;

	if (type == 0) {	/* clean up so purify doesn't barf */
		if (len) free(s);
		len = 0;
		s = 0;
		return (0);
	}
	if (!len) {
		len = strlen(sccs->sfile) + 50;
		s = malloc(len + 50);
		assert(s);
	} else if (len < (int) strlen(sccs->sfile) + 3) {
		free(s);
		len = strlen(sccs->sfile) + 50;
		s = malloc(len);
		assert(s);
	}
	if (!index(sccs->sfile, '/')) {
		strcpy(s, sccs->sfile);
		s[0] = type;
		return (s);
	}
	strcpy(s, sccs->sfile);
	t = rindex(s, '/') + 1;
	*t = type;
	return (s);
}

char	*
sccs_Xfile(sccs *s, char type)
{
	return (sccsXfile(s, type));
}

/*
 * Get the date as YY/MM/DD HH:MM:SS.mmm
 * and get timezone as minutes west of GMT
 */
private void
date(delta *d, time_t tt)
{
	d->sdate = strdup(time2date(tt));
	zoneArg(d, sccs_zone(tt));

	DATE(d);
	if ((d->date - d->dateFudge) != tt) {
		fprintf(stderr, "Date=[%s%s] d->date=%lu tt=%lu\n",
		    d->sdate, d->zone, d->date, tt);
		fprintf(stderr, "Fudge = %d\n", (int)d->dateFudge);
		fprintf(stderr, "Internal error on dates, aborting.\n");
		assert((d->date - d->dateFudge) == tt);
	}
}

char	*
testdate(time_t t)
{
	static char	date[50];
	char	zone[50];

	strcpy(date, time2date(t));
	strcpy(zone, sccs_zone(t));

	if (date2time(date, zone, EXACT) != t) {
		fprintf(stderr, "Internal error on dates, aborting.\n");
		fprintf(stderr, "time_t=%lu vs %lu date=%s zone=%s\n",
		    date2time(date, zone, EXACT), t, date, zone);
		exit(1);
	}
	return (date);
}

/*
 * Returns a date string like 01/08/16 13:54:42 in the current
 * timezone for a time_t.  Should be used with sccs_zone(tt) to
 * get the full time string.
 * Return value is in a staticly allocated buffer.
 */
private char *
time2date(time_t tt)
{
	static	char	tmp[50];

	strftime(tmp, sizeof(tmp), "%y/%m/%d %H:%M:%S",
		 localtimez(&tt, 0));
	return (tmp);
}

/*
 * Save a serial in an array.  If the array is out of space, reallocate it.
 * The size of the array is in array[0].
 * The serial number is stored in ascending order.
 */
ser_t *
addSerial(ser_t *space, ser_t s)
{
	int	i, j, size;
	ser_t	*tmp;

	if (!space) {
		space = calloc(16, sizeof(ser_t));
		assert(space);
		space[0] = (ser_t)16;
		space[1] = s;
		return (space);
	}

	size = (int) space[0];
	if (space[size -1]) {	/* full up, dude */
		tmp = calloc(size*2, sizeof(ser_t));
		assert(tmp);
		if (space[size - 1] < s)  {
			/* s is the largest, stick it at the end */
			memcpy(tmp, space, size * sizeof(ser_t));
			tmp[size] = s;
		} else {
			/* s is not the largest, insert it while we copy */
			for (i = j = 1; i < size;) {
				if (space[i] > s)  { tmp[j++] = s; break; }
				tmp[j++] = space[i++];
			}
			memcpy(&tmp[j], &space[i], (size - i) * sizeof(ser_t));
		}
		tmp[0] = (ser_t)(size * 2);
		free(space);
		return (tmp);
	}

	EACH(space) if (space[i] > s) break;
	if (space[i] > s) {
		/* we have a "insert", move stuff up one slot */
		for (j = i; space[j]; j++);
		assert(j <= (size - 1));
		tmp = &space[i + 1];
		memmove(tmp, &tmp[-1], (j - i) * sizeof(ser_t));
	}
	space[i] = s;
	return (space);
}

/*
 * Parse the list of revisions passed, sort of like strtok().
 * The first call passes in the list, the next passes in 0.
 * Handle rev,rev,rev,rev-rev,rev...
 * "rev" can be a symbol.
 * We're careful to have the list unchanged if we return an error.
 */
private delta *
walkList(sccs *s, char *list, int *errp)
{
	static	char *next;	/* left pointing at next rev */
	static	delta *d;	/* set if we are working up the tree */
	static	delta *stop;	/* where to stop in the tree, inclusive */
	delta	*tmp;
	char	save, *t, *rev;

	if (list) {
		next = list;
		d = stop = 0;
	}
	if (d) {
		tmp = d;
		d = d == stop ? 0 : d->parent;
		return (tmp);
	}
	if (!next) return (0);
	/* XXX - this will screw up if they have symbols w/ - in the name */
	for (t = next; *t && *t != ',' && *t != '-'; t++);
	if (!*t || (*t == ',')) {
		save = *t;
		*t++ = 0;
		rev = next;
		next = save ? t : 0;
		if (streq(rev, "+")) {
			tmp = findrev(s, 0);
		} else {
			name2rev(s, &rev);
			tmp = rfind(s, rev);
		}
		t[-1] = save;
		return (tmp);
	}
	if (*t != '-') {
		*errp = 1;
		return (0);
	}
	/*
	 * OK, it's a range.  Find the bottom/top and and start walking them.
	 * We insist that the ordering is 1.3-1.9, not the other way, and
	 * that there is a path from the second rev to the first through
	 * the parent pointers (i.e., it doesn't go around a corner down
	 * a branch).
	 */
	*t++ = 0;
	rev = next;
	next = t;
	name2rev(s, &rev);
	stop = rfind(s, rev);
	t[-1] = '-';
	for (t = next; *t && *t != ',' && *t != '-'; t++);
	if (*t && (*t != ',')) {
		*errp = 1;
		return (0);
	}
	save = *t;
	*t++ = 0;
	rev = next;
	next = save ? t : 0;
	if (streq(rev, "+")) {
		d = findrev(s, 0);
	} else {
		name2rev(s, &rev);
		d = rfind(s, rev);
	}
	t[-1] = save;

	if (!stop || !d) {
		*errp = 2;
		return (0);
	}
	for (tmp = d; tmp && tmp != stop; tmp = tmp->parent);
	if (tmp != stop) {
		*errp = 2;
		return (0);
	}
	tmp = d;
	d = d == stop ? 0 : d->parent;
	return (tmp);
}

/*
 * Read the list of serials in s and put them in list.
 * S can either be list of serials or a list of revisions.
 * In neither case does this recurse on the list, it just parses the list.
 * This is quite different than serialmap() which uses this routine but
 * stores the results in a different data structure.
 * XXX - this poorly named, it doesn't use "struct serlist".
 */
private ser_t *
getserlist(sccs *sc, int isSer, char *s, int *ep)
{
	delta	*t;
	ser_t	*l = 0;

	debug((stderr, "getserlist(%.*s)\n", linelen(s), s));
	if (isSer) {
		while (*s && *s != '\n') {
			l = addSerial(l, (ser_t) atoi(s));
			while (*s && *s != '\n' && isdigit(*s)) s++;
			while (*s && *s != '\n' && !isdigit(*s)) s++;
		}
		return (l);
	}
	for (t = walkList(sc, s, ep); !*ep && t; t = walkList(sc, 0, ep)) {
		l = addSerial(l, t->serial);
	}
	return (l);
}

/*
 * XXX - this poorly named, it doesn't use "struct serlist".
 */
private void
putserlist(sccs *sc, ser_t *s, FILE *out)
{
	int	first = 1, i;
	char	buf[20];

	if (!s) return;
	EACH(s) {
		sertoa(buf, s[i]);
		if (!first) fputmeta(sc, " ", out);
		fputmeta(sc, buf, out);
		first = 0;
	}
}

/*
 * Generate a list of serials marked with D_SET tag
 */
private ser_t *
setmap(sccs *s, int bit, int all)
{
	ser_t	*slist;
	delta	*t;

	slist = calloc(s->nextserial, sizeof(ser_t));
	assert(slist);

	for (t = s->table; t; t = t->next) {
		unless (all || (t->type == 'D')) continue;
 		assert(t->serial <= s->nextserial);
		if (t->flags & bit) {
			slist[t->serial] = 1;
		}
	}
	return (slist);
}

struct	liststr {
	struct liststr	*next;
	char		*str;
};

private int
insertstr(struct liststr **list, char *str)
{
	struct liststr	*item = malloc(sizeof(*item));

	item->next = *list;
	item->str = str;
	*list = item;
	return (1 + strlen(str));	/* include room for join or term */
}

private char *
buildstr(struct liststr *list, int len)
{
	struct liststr	*p;
	char		*str;
	int		offset = 0;

	unless (len) return (0);
	str = malloc(len + 1);
	for (p = list; p ; p = p->next) {
		offset += sprintf(&str[offset], "%s,", p->str);
	}
	assert(offset == len);
	if (offset) str[offset - 1] = '\0';
	return (str);
}

private void
freestr(struct liststr *list)
{
	struct liststr	*p;

	for ( ; list ; list = p) {
		p = list->next;
		free(list);
	}
}

/* compress a set of serials.  Assume 'd' is basis version and compute
 * include and exclude strings to go with it.  The strings are a
 * comma separated list of numbers
 */

private int
compressmap(sccs *s, delta *d, ser_t *set, int useSer, void **inc, void **exc)
{
	struct	liststr	*inclist = 0, *exclist = 0;
	int	inclen = 0, exclen = 0;
	ser_t	*slist;
	delta	*t;
	int	i;
	int	active;
	ser_t	*incser = 0, *excser = 0;

	assert(d);
	assert(set);

	*exc = *inc = 0;

	slist = calloc(s->nextserial, sizeof(ser_t));
	assert(slist);

	slist[d->serial] = S_PAR;	/* seed the ancestor thread */

	for (t = s->table; t; t = t->next) {
		if (t->type != 'D') continue;

 		assert(t->serial <= s->nextserial);

		/* Set up parent ancestory for this node */
		if ((slist[t->serial] & S_PAR) && t->parent) {
			slist[t->parent->serial] |= S_PAR;
#ifdef MULTIPARENT
			if (t->merge) slist[t->merge] |= S_PAR;
#endif
		}

		/* if a parent and not excluded, or if included */
		active = (((slist[t->serial] & (S_PAR|S_EXCL)) == S_PAR)
		     || slist[t->serial] & S_INC);

		/* exclude if active in delta set and not in desired set */
		if (active && !set[t->serial]) {
			if (useSer) {
				excser = addSerial(excser, t->serial);
			} else {
				exclen += insertstr(&exclist, t->rev);
			}
		}
		unless (set[t->serial])  continue;

		/* include if not active in delta set and in desired set */
		if (!active) {
			if (useSer) {
				incser = addSerial(incser, t->serial);
			} else {
				inclen += insertstr(&inclist, t->rev);
			}
		}
		EACH(t->include) {
			unless(slist[t->include[i]] & (S_INC|S_EXCL))
				slist[t->include[i]] |= S_INC;
		}
		EACH(t->exclude) {
			unless(slist[t->exclude[i]] & (S_INC|S_EXCL))
				slist[t->exclude[i]] |= S_EXCL;
		}
	}

	if (useSer) {
		if (incser) *inc = incser;
		if (excser) *exc = excser;
	} else {
		if (inclen) *inc = buildstr(inclist, inclen);
		if (exclen) *exc = buildstr(exclist, exclen);
	}

	if (slist)   free(slist);
	if (exclist) freestr(exclist);
	if (inclist) freestr(inclist);
	return (0);
}

/*
 * Generate a list of serials to use to get a particular delta and
 * allocate & return space with the list in the space.
 * The 0th entry is contains the maximum used entry.
 * Note that the error pointer is to be used only by walkList, it's null if
 * the lists are null.
 * Note we don't have to worry about growing tables here, the list isn't saved
 * across calls.
 */
private ser_t *
serialmap(sccs *s, delta *d, char *iLst, char *xLst, int *errp)
{
	ser_t	*slist;
	delta	*t, *start = d;
	int	i;

	assert(d);

	slist = calloc(s->nextserial, sizeof(ser_t));
	assert(slist);

	/* initialize with iLst and xLst */
	if (iLst) {
		debug((stderr, "Included:"));
		for (t = walkList(s, iLst, errp);
		    !*errp && t; t = walkList(s, 0, errp)) {
			debug((stderr, " %s", t->rev));
			assert(t->serial <= s->nextserial);
			slist[t->serial] = S_INC;
			if (t->serial > start->serial) start = t;
 		}
		debug((stderr, "\n"));
		if (*errp) goto bad;
	}

	if (xLst) {
		debug((stderr, "Excluded:"));
		for (t = walkList(s, xLst, errp);
		    !*errp && t; t = walkList(s, 0, errp)) {
			assert(t->serial <= s->nextserial);
			debug((stderr, " %s", t->rev));
			if (slist[t->serial] == S_INC)
				*errp = 3;
			else {
				slist[t->serial] = S_EXCL;
			}
			if (t->serial > start->serial) start = t;
 		}
		debug((stderr, "\n"));
		if (*errp) goto bad;
 	}

	/* Use linear list, newest to oldest, looking only at 'D' */

	/* slist is used as temp storage for S_INC and S_EXCL then
	 * replaced with either a 0 or a 1 depending on if in view
	 * XXX clean up use of enum values mixed with 0 and 1
	 * XXX The slist[0] has a ser_t entry ... is it needed?
	 * XXX slist has (besides slist[0]) only one of 5 values:
	 *     0, 1, S_INC, S_EXCL, S_PAR so it doesn't need to be ser_t?
	 */

	/* Seed the graph thread */
	slist[d->serial] |= S_PAR;

	for (t = start; t; t = t->next) {
		if (t->type != 'D') continue;

 		assert(t->serial <= s->nextserial);

		/* Set up parent ancestory for this node */
		if ((slist[t->serial] & S_PAR) && t->parent) {
			slist[t->parent->serial] |= S_PAR;
#ifdef MULTIPARENT
			if (t->merge) slist[t->merge] |= S_PAR;
#endif
		}

		/* if an ancestor and not excluded, or if included */
		if ( ((slist[t->serial] & (S_PAR|S_EXCL)) == S_PAR)
		     || slist[t->serial] & S_INC) {

			/* slist [0] = Max serial that is in slist */
			unless (slist[0])  slist[0] = t->serial;

			slist[t->serial] = 1;
			/* alter only if item hasn't been set yet */
			EACH(t->include) {
				unless(slist[t->include[i]] & (S_INC|S_EXCL))
					slist[t->include[i]] |= S_INC;
			}
			EACH(t->exclude) {
				unless(slist[t->exclude[i]] & (S_INC|S_EXCL))
					slist[t->exclude[i]] |= S_EXCL;
			}
		}
		else
			slist[t->serial] = 0;
	}
	return (slist);
bad:	free(slist);
	return (0);
}

int
sccs_graph(sccs *s, delta *d, ser_t *map, char **inc, char **exc)
{
	return (compressmap(s, d, map, 0, (void **)inc, (void **)exc));
}

ser_t *
sccs_set(sccs *s, delta *d, char *iLst, char *xLst)
{
	int	junk;

	return (serialmap(s, d, iLst, xLst, &junk));
}

private void
changestate(register serlist *state, char type, int serial)
{
	register serlist *p, **pp;
	register serlist *n;

	debug2((stderr, "chg(%c, %d)\n", type, serial));

	/* find place in linked list */
	for (pp = &(state[SLIST].next), p = *pp; p; pp = &(p->next), p = *pp) {
		if (p->serial <= serial) break;
	}

	/*
	 * Delete it if it is an 'E'.
	 */
	if (type == 'E') {	/* free this item */
		assert(p && (p->serial == serial));
		*pp = p->next;
		p->next = state[SFREE].next;
		state[SFREE].next = p;
		return;
	}

	/*
	 * Else a 'D' or an 'I', so insert it in list
	 */

	assert(!p || (p->serial < serial));
	assert(state[SFREE].next || ("Ran out of serial numbers" == 0));

	n = state[SFREE].next;
	state[SFREE].next = n->next;

	*pp = n;
	n->next = p;
	n->serial = serial;
	n->type = type;
	verify(state);
}

/*
 * Allocate (or realocate) a serial list array.	 The way this works is like
 * so:
 * [SLIST] is the allocated list, linked through .next and .prev.
 * [SLIST].serial is the number of allocated nodes.
 * [SFREE] is the free list, linked only through .next.
 * [SFREE].serial is the number of free nodes.
 */
private serlist *
allocstate(serlist *old, int n)
{
	serlist *s;
	int	i;

	n += 2;		/* to account for the accounting overhead */
	n += 10;	/* XXX - I'm not sure I need this but haven't walked
			 * all the paths to be sure I don't.  If I do need it,
			 * it is perhaps due to metadata being added.
			 */
	assert(!old);	/* XXX - realloc not done */
	s = malloc(n * sizeof(serlist));
	assert(s);
	for (i = 2; i < n-1; ++i) {
		s[i].next = &s[i+1];
	}
	s[i].next = 0;
	s[SFREE].next = &s[2];
	s[SFREE].serial = 0;
	s[SLIST].next = 0;
	s[SLIST].serial = 0;
	return (s);
}

private int
delstate(ser_t ser, const serlist *state, const ser_t *slist)
{
	register serlist *s;
	register	 int ok = 0;

	/* To be yes, serial must delete and no others, and first I
	 * must be active.  If any other delete active, return false.
	 */
	assert(slist[ser]);
	for (s = state[SLIST].next; s; s = s->next) {
		if (s->type != 'D') break;
		if (s->serial == ser) {
			ok = 1;
		}
		else if (slist[s->serial]) {
			return (0);
		}
	}

	if (ok && s && slist[s->serial])
		return (s->serial);

	return (0);
}

private int
visitedstate(const serlist *state, const ser_t *slist)
{
	register serlist *s;

	/* Find the first not D and return serial if it is active */
	for (s = state[SLIST].next; s; s = s->next) {
		if (s->type != 'D') break;
	}

	if (s && slist[s->serial])
		return (s->serial);

	return (0);
}

private int
whatstate(const serlist *state)
{
	register serlist *s;

	/* Loop until an I */
	for (s = state[SLIST].next; s; s = s->next) {
		if (s->type == 'I') break;
	}
	return ((s) ? s->serial : 0);
}

/* calculate printstate using where we are (state)
 * and list of active deltas (slist)
 * return either the serial if active, or 0 if not
 */

private int
printstate(const serlist *state, const ser_t *slist)
{
	register serlist *s;

	/* Loop until any I or active D */
	for (s = state[SLIST].next; s; s = s->next) {
		unless (s->type == 'D' && !slist[s->serial])
			break;
	}

	if (s) {
		int ret = (s->type == 'I')
			? (slist[s->serial] ? s->serial : 0)
 			: 0;
		debug2((stderr, "printstate {t %c s %d p %d} = %d\n", \
			s->type, s->serial, slist[s->serial], ret));
		return (ret);
	}

	debug2((stderr, "printstate {} = 0\n"));

	return (0);
}

private inline void
fnnlputs(char *buf, FILE *out)
{
	register char	*t = buf;
	char		fbuf[MAXLINE];
	register char	*p = fbuf;

	while (*t && (*t != '\n')) {
		*p++ = *t++;
		if (p == &fbuf[MAXLINE-1]) {
			*p = 0;
			p = fbuf;
			fputs(fbuf, out);
		}
	}
	if (p != fbuf) {
		*p = 0;
		fputs(fbuf, out);
	}
}

private void
fnlputs(char *buf, FILE *out)
{
	register char	*t = buf;
	char		fbuf[MAXLINE];
	register char	*p = fbuf;

	do {
		*p++ = *t;
		if (p == &fbuf[MAXLINE-1]) {
			*p = 0;
			p = fbuf;
			fputs(fbuf, out);
		}
	} while (*t && (*t++ != '\n'));
	if (p != fbuf) {
		*p = 0;
		fputs(fbuf, out);
	}
}

/*
 * This interface is used for writing the metadata (delta table, flags, etc).
 *
 * N.B. All checksum functions do the intermediate sum in an int variable
 * because 16-bit register arithmetic can be up to 2x slower depending on
 * the platform and compiler.
 */
private sum_t
fputmeta(sccs *s, u8 *buf, FILE *out)
{
	register u8	*t = buf;
	register unsigned int sum = 0;

	for (; *t; t++)	sum += *t;
	s->cksum += (sum_t)sum;
	fputs(buf, out);

	return (sum);
}

/*
 * Data for zputs() to writes it's data.  We currently can only write
 * one sfile at a time.
 */
private	zputbuf	*zput;

private void
gzip_sum(void *data, u8 *buf, int len)
{
	sccs	*s = ((void **)data)[0];
	FILE	*f = ((void **)data)[1];
	u32	sum = 0;
	int	i;

	if (len) {
		for (i = 0; i < len; sum += buf[i++]);
		s->cksum += (sum_t)sum;
		fwrite(buf, 1, len, f);
	} else {
		free(data);
	}
}

private void
sccs_zputs_init(sccs *s, FILE *fout)
{
	void	**data = malloc(2 * sizeof(void *));

	data[0] = s;
	data[1] = fout;
	zput = zputs_init(gzip_sum, data);
	assert(zput);
}

/*
 * If the data isn't a control line, just dump it.
 * Otherwise, put it out there after adjusting the serial.
 */
private sum_t
fputbumpserial(sccs *s, u8 *buf, int inc, FILE *out)
{
	u8	tmp[20];
	u8	*t;
	ser_t	ser, sum;

	unless (buf[0] == '\001') return (fputdata(s, buf, out));
	/* ^AI ddd\n
	 * ^AE ddd\n
	 * ^AE dddN\n
	 */
	tmp[1] = 0;
	tmp[0] = buf[1];
	sum = fputdata(s, "\001", out);
	sum += fputdata(s, tmp, out);
	sum += fputdata(s, " ", out);
	ser = atoi(&buf[3]);
	sprintf(tmp, "%u", ser + inc);
	sum += fputdata(s, tmp, out);
	for (t = &buf[3]; isdigit(*t); t++);
	sum += fputdata(s, t, out);
	return (sum);
}

/*
 * Like fputmeta, but optionally compresses the data stream.
 * This is used for the data section exclusively.
 */
private sum_t
fputdata(sccs *s, u8 *buf, FILE *out)
{
	u32	sum = 0;
	u8	*p = buf;

	while (*p) {
		sum += *p;
		if (*p++ == '\n') break;
	}
	if (s->encoding & E_GZIP) {
		zputs(zput, buf, p - buf);
	} else {
		fwrite(buf, 1, p - buf, out);
		s->cksum += sum;
	}
	return (sum);
}

/* Flush out data buffered by fputdata.  */
private int
fflushdata(sccs *s, FILE *out)
{
	if (s->encoding & E_GZIP) {
		zputs_done(zput);
		zput = 0;
	}
	if (flushFILE(out) && (errno != EPIPE)) {
		perror(s->sfile);
		s->io_error = s->io_warned = 1;
		return(-1);
	}
	return (0);
}

#define	ENC(c)	((((uchar)c) & 0x3f) + ' ')
#define	DEC(c)	((((uchar)c) - ' ') & 0x3f)

private inline int
uuencode1(register uchar *from, register char *to, int n)
{
	int	space[4];
	register int *c = space;
	register int i;
	char	*save = to;

	*to++ = ENC(n);
	for (i = 0; i < n; i += 3) {
		c[0] = from[i] >> 2;
		c[1] = ((from[i]<<4)&0x30) | ((from[i+1]>>4)&0xf);
		c[2] = ((from[i+1]<<2)&0x3c) | ((from[i+2]>>6)&3);
		c[3] = from[i+2] & 0x3f;
		*to++ = ENC(c[0]);
		*to++ = ENC(c[1]);
		*to++ = ENC(c[2]);
		*to++ = ENC(c[3]);
	}
	*to++ = '\n';
	*to = 0;
	return (to - save);
}

private int
uuencode_sum(sccs *s, FILE *in, FILE *out)
{
	uchar	ibuf[450];
	char	obuf[80];
	register uchar *inp;
	register int n;
	register int length;
	int	added = 0;

	while ((length = fread(ibuf, 1, 450, in)) > 0) {
		inp = ibuf;
		while (length > 0) {
			n = (length > 45) ? 45 : length;
			if (n < 45) {
				uchar	*e = &inp[n];
				int	left = 45 - n;

				while (left--) *e++ = 0;
			}
			length -= n;
			uuencode1(inp, obuf, n);
			s->dsum += fputdata(s, obuf, out);
			inp += n;
			added++;
		}
	}
	s->dsum += fputdata(s, " \n", out);
	return (++added);
}

int
uuencode(FILE *in, FILE *out)
{
	uchar	ibuf[450];
	char	obuf[650];
	register uchar *inp;
	register char *outp;
	register int n;
	register int length;
	int	added = 0;

	while ((length = fread(ibuf, 1, 450, in)) > 0) {
		outp = obuf;
		inp = ibuf;
		while (length > 0) {
			n = (length > 45) ? 45 : length;
			if (n < 45) {
				uchar	*e = &inp[n];
				int	left = 45 - n;

				while (left--) *e++ = 0;
			}
			length -= n;
			outp += uuencode1(inp, outp, n);
			added++;
			inp += n;
		}
		*outp = 0;
		fputs((char *)obuf, out);
	}
	fputs(" \n", out);
	return (++added);
}

private inline int
uudecode1(register char *from, register uchar *to)
{
	int	length, save;

	unless (from[0] && from[1] && (length = DEC(*from++))) return (0);
	if (length > 50) {
		fprintf(stderr, "Corrupted data: %.25s\n", from);
		return (0);
	}
	save = length;
	while (length > 0) {
		if (length-- > 0)
			*to++ = (uchar)((DEC(from[0])<<2) | (DEC(from[1])>>4));
		if (length-- > 0)
			*to++ = (uchar)((DEC(from[1])<<4) | (DEC(from[2])>>2));
		if (length-- > 0)
			*to++ = (uchar)((DEC(from[2]) << 6) | DEC(from[3]));
		from += 4;
	}
	return (save);
}

int
uudecode(FILE *in, FILE *out)
{
	uchar	ibuf[650];
	char	obuf[450];
	int	n, moved = 0;

	while (fnext(ibuf, in)) {
		n = uudecode1(ibuf, obuf);
		moved += n;
		fwrite(obuf, n, 1, out);
	}
	return (moved);
}

private int
openOutput(sccs *s, int encode, char *file, FILE **op)
{
	char	*mode = "w";
	int	toStdout = streq(file, "-");

	assert(op);
	debug((stderr, "openOutput(%x, %s, %p)\n", encode, file, op));
	/*
	 * Note: This has no effect when we print to stdout We want
	 * this becuase we want diff_gfile() to diffs file with
	 * normlized to LF.
	 *
	 * Win32 note: t.bkd regression failed if ChangeSet have have
	 * CRLF termination.
	 */
	if (((encode & E_DATAENC) == E_ASCII) &&
	    !CSET(s) && (s->xflags & X_EOLN_NATIVE)) {
		mode = "wt";
	}
	if (toStdout) {
		*op = stdout;
	} else {
		unless (*op = fopen(file, mode)) {
			mkdirf(file);
			*op = fopen(file, mode);
		}
	}
	debug((stderr, "openOutput = %p\n", *op));
	return (0);
}

/*
 * Return a list of revisions from rev to the first gca of base.
 * Null return is an error.
 * An invariant is that no list is returned if the rev is already implied
 * by base..root.
 */
char *
sccs_impliedList(sccs *s, char *who, char *base, char *rev)
{
	delta	*baseRev, *t, *mRev;
	int	active;
	void	*inc = 0, *exc = 0;
	ser_t	*slist = 0;
	int	i;

	/* XXX: This can go away when serialmap does this directly
	 */

	unless (baseRev = findrev(s, base)) {
		fprintf(stderr,
		    "%s: cannot find base rev %s in %s\n",
		    who, base, s->sfile);
err:		s->state |= S_WARNED;
		if (inc) free(inc);
		if (exc) free(exc);
		if (slist) free(slist);
		return (0);
	}
	unless (mRev = findrev(s, rev)) {
		fprintf(stderr,
		    "%s: cannot find merge rev %s in %s\n",
		    who, rev, s->sfile);
		goto err;
	}

	slist = calloc(s->nextserial, sizeof(ser_t));

	slist[baseRev->serial] = S_PAR;
	slist[mRev->serial] = S_PAR;

	for (t = s->table; t; t = t->next) {
		if (t->type != 'D') continue;

 		assert(t->serial < s->nextserial);

		/* Set up parent ancestory for this node */
		if ((slist[t->serial] & S_PAR) && t->parent) {
			slist[t->parent->serial] |= S_PAR;
#ifdef MULTIPARENT
			if (t->merge) slist[t->merge] |= S_PAR;
#endif
		}

		/* if a parent and not excluded, or if included */
		active = (((slist[t->serial] & (S_PAR|S_EXCL)) == S_PAR)
		     || slist[t->serial] & S_INC);

		unless (active) {
			slist[t->serial] = 0;
			continue;
		}
		slist[t->serial] = 1;
		EACH(t->include) {
			unless(slist[t->include[i]] & (S_INC|S_EXCL))
				slist[t->include[i]] |= S_INC;
		}
		EACH(t->exclude) {
			unless(slist[t->exclude[i]] & (S_INC|S_EXCL))
				slist[t->exclude[i]] |= S_EXCL;
		}
	}
	if (compressmap(s, baseRev, slist, 0, &inc, &exc)) {
		fprintf(stderr, "%s: cannot compress merged set\n", who);
		goto err;
	}
	if (exc) {
		fprintf(stderr,
		    "%s: compressed map caused exclude list: %s\n",
		    who, (char *)exc);
		goto err;
	}
	if (slist) free(slist);
	return (inc);
}

/* Make sure the serial map generated by sc and the backup scb are
 * the same (ignoring any marked D_GONE). 'd' is inside of sc.
 * sc has been modified (as far as some parent-child) and
 * scb is the original.
 */

void
sccs_adjustSet(sccs *sc, sccs *scb, delta *d)
{
	int	errp;
	ser_t	*slist;
	delta	*n;
	void	*inc = 0, *exc = 0;

	errp = 0;
	n = sfind(scb, d->serial);	/* get 'd' from backup */
	assert(n);
	slist = serialmap(scb, n, 0, 0, &errp);
	if (errp) {
		fprintf(stderr, "an errp error\n");
		if (inc) free(inc);
		if (exc) free(exc);
		if (slist) free(slist);
		exit(1);
	}
	if (sc->hasgone) {
		for (n = d; n; n = n->next) {
			if (n->flags & D_GONE) slist[n->serial] = 0;
		}
	}
	if (d->include) {
		free(d->include);
		d->include = 0;
	}
	if (d->exclude) {
		free(d->exclude);
		d->exclude = 0;
	}
	if (compressmap(sc, d, slist, 1, &inc, &exc)) {
		assert("cannot compress merged set" == 0);
	}
	if (inc) {
		d->include = inc;
	}
	if (exc) {
		d->exclude = exc;
	}
	if (slist) {
		free(slist);
		slist = 0;
	}
	return;
}

/*
 * Take two strings and concat them into a new strings.
 * Caller frees.
 */
private char *
strconcat(char *a, char *b, char *sep)
{
	char	*tmp;

	if (!a) return (b);
	if (!b) return (a);
	tmp = malloc(strlen(a) + strlen(b) + 2);
	sprintf(tmp, "%s%s%s", a, sep, b);
	return (tmp);
}

private int
write_pfile(sccs *s, int flags, delta *d,
	char *rev, char *iLst, char *i2, char *xLst, char *mRev)
{
	int	fd, len;
	char	*tmp, *tmp2;
	
	if ((WRITABLE_REG(s) ||
		S_ISLNK(s->mode) && HAS_GFILE(s) && HAS_PFILE(s)) && 
	    !(flags & GET_SKIPGET)) {
		verbose((stderr,
		    "Writable %s exists, skipping it.\n", s->gfile));
		s->state |= S_WARNED;
		return (-1);
	}
	unless (sccs_lock(s, 'Z')) {
		fprintf(stderr, "get: can't zlock %s\n", s->gfile);
		repository_lockers(s->proj);
		return (-1);
	}
	fd = open(s->pfile, O_CREAT|O_WRONLY|O_EXCL, GROUP_MODE);
	if (fd == -1) {
		fprintf(stderr, "get: can't plock %s\n", s->gfile);
		sccs_unlock(s, 'z');
		return (-1);
	}
	tmp2 = time2date(time(0));
	assert(sccs_getuser() != 0);
	len = strlen(d->rev)
	    + MAXREV + 2
	    + strlen(rev)
	    + strlen(sccs_getuser())
	    + strlen(tmp2)
	    + (xLst ? strlen(xLst) + 3 : 0)
	    + (mRev ? strlen(mRev) + 3 : 0)
	    + 3 + 1 + 1; /* 3 spaces \n NULL */
	if (i2) {
		len += strlen(i2) + 3;
	} else {
#ifdef CRAZY_WOW
		// XXX: this used to be here, and was removed because
		// mRev can be set with no i2 list because of the
		// elements in the merge, while on another branch,
		// might have been previously included with -i.
		// see the t.merge file for an example of this.
		assert(!mRev);
#endif
		len += (iLst ? strlen(iLst) + 3 : 0);
	}
	tmp = malloc(len);
	sprintf(tmp, "%s %s %s %s", d->rev, rev, sccs_getuser(), tmp2);
	if (i2) {
		strcat(tmp, " -i");
		strcat(tmp, i2);
	} else if (iLst) {
		strcat(tmp, " -i");
		strcat(tmp, iLst);
	}
	if (xLst) {
		strcat(tmp, " -x");
		strcat(tmp, xLst);
	}
	if (mRev) {
		strcat(tmp, " -m");
		strcat(tmp, mRev);
	}
	strcat(tmp, "\n");
	write(fd, tmp, strlen(tmp));
	close(fd);
	free(tmp);
	s->state |= S_PFILE|S_ZFILE;
	return (0);
}

int
sccs_rewrite_pfile(sccs *s, pfile *pf)
{
	int	fd, len;
	char	*tmp;

	/* XXX: Do I need any special locking code? */
	if ((fd = open(s->pfile, O_WRONLY|O_TRUNC, 0666)) == -1) {
		perror("open pfile");
		return (-1);
	}
	len = strlen(pf->oldrev)
	    + MAXREV + 2
	    + strlen(pf->newrev)
	    + strlen(pf->user)
	    + strlen(pf->date)
	    + (pf->iLst ? strlen(pf->iLst) + 3 : 0)
	    + (pf->xLst ? strlen(pf->xLst) + 3 : 0)
	    + (pf->mRev ? strlen(pf->mRev) + 3 : 0)
	    + 3 + 1 + 1; /* 3 spaces \n NULL */
	tmp = malloc(len);
	sprintf(tmp, "%s %s %s %s",
	    pf->oldrev, pf->newrev, pf->user, pf->date);
	if (pf->iLst) {
		strcat(tmp, " -i");
		strcat(tmp, pf->iLst);
	}
	if (pf->xLst) {
		strcat(tmp, " -x");
		strcat(tmp, pf->xLst);
	}
	if (pf->mRev) {
		strcat(tmp, " -m");
		strcat(tmp, pf->mRev);
	}
	strcat(tmp, "\n");
	write(fd, tmp, strlen(tmp));
	close(fd);
	free(tmp);
	return (0);
}

/*
 * Returns: valid address if OK, (char*)-1 if error, 0 if not ok but not error.
 */
private char *
setupOutput(sccs *s, char *printOut, int flags, delta *d)
{
	char *f;

	/*
	 * GET_SUM should always have PRINT as well because otherwise we
	 * may create a gfile when all we wanted was the SUM.  Yucky API.
	 */
	if (flags & (PRINT|GET_SUM)) {
		f = printOut;
	} else {
		/* With -G/somewhere/foo.c we need to check the gfile again */
		if (flags & GET_NOREGET) flags |= SILENT;
		if (WRITABLE_REG(s) && writable(s->gfile)) {
			verbose((stderr, "Writable %s exists\n", s->gfile));
			s->state |= S_WARNED;
			return ((flags & GET_NOREGET) ? 0 : (char*)-1);
		} else if ((flags & GET_NOREGET) && exists(s->gfile) &&
		    (!(flags&GET_EDIT) || !(s->xflags & (X_RCS|X_SCCS)))) {
			if ((flags & GET_EDIT) && !WRITABLE(s)) {
				s->mode |= 0200;
				if (chmod(s->gfile, s->mode)) {
					perror(s->gfile);
					return ((char*)-1);
				}
			}
			return (0);
		}
		f = s->gfile;
		unlinkGfile(s);
	}
	return (f);
}

/*
 * Get the checksum of the first delta found with a real checksum, not
 * a made up one from almostUnique().
 * We depend on the fact that includes/excludes force a data checksum,
 * not an almostUnique() value.
 */
private	delta *
getCksumDelta(sccs *s, delta *d)
{
	delta	*t;

	for (t = d; t; t = t->parent) {
		if (t->include || t->exclude || t->added || t->deleted) {
			return (t);
		}
	}
	return (0);
}

/*
 * Get the checksum of the first delta found with a real checksum, not
 * a made up one from almostUnique().
 * That would be the earliest delta that has the same symlink value as
 * d->symlink. if none exists, the checksum delta is d itself.
 * 
 * Note : A symlink merge node must have its own checksum,
 * because:
 * a) One of the parent delta has became the merge parent. This means we
 *    may not find the right checksum by following the parent pointer.
 *    Recomputing of checksum should be done in the resolver.
 *    The resolver does not have a symlink resolver yet, we
 *    need to re-test this after the symlink resolve is implemented.
 * b) It also possible that conflict is resolved by creating third symlink
 *    target.
 */
delta *
getSymlnkCksumDelta(sccs *s, delta *d)
{
	delta	*t, *p;

	assert(d->symlink);
	if (d->merge) return (d);
	for (t = d; t; t = t->parent) {
		p = t->parent;
		unless (p->symlink) return (t);
		unless (streq(p->symlink, d->symlink)) return (t);
		if (p->merge && streq(p->symlink, d->symlink)) return (p);
	}
	return (d);
}

private sum_t
getKey(MDBM *DB, char *buf, int flags)
{
	char	*k, *v;
	int	len;
	char	data[MAXLINE];

	for (k = buf; *k != '\n'; ++k);
	len = (char *)k - buf;
	assert(len < MAXLINE);
	if (len) strncpy(data, buf, len);
	data[len] = 0;
	if (flags & DB_CONFIG) {
		parseConfig(data, DB);
		return (1);
	} else if (flags & DB_KEYFORMAT) {
		k = data;
		if (v = separator(data)) *v++ = 0;
	} else {
		k = data;
		if (v = strchr(data, ' ')) *v++ = 0;
	}
	unless (v) {
		chomp(data);
		fprintf(stderr, "get hash: no separator in '%s'\n", data);
		return (-1);
	}
	switch (mdbm_store_str(DB, k, v, MDBM_INSERT)) {
	    case 1:	/* key already in DB */
		return (0);
	    case -1:
		return (-1);
	    default:
		return (1);
	}
}

private char	*
get_lineName(sccs *s, ser_t ser, MDBM *db, u32 lnum, char *buf)
{
	datum	k, v;

	/* see if ser is in mdbm, if not, gen an md5 name and stick in buf */
	k.dptr = (void *)&ser;
	k.dsize = sizeof(ser_t);
	v = mdbm_fetch(db, k);
	if (v.dsize) {
		strcpy(buf, v.dptr);
	} else {
		sccs_md5delta(s, sfind(s, ser), buf);
		v.dptr = (void *)buf;
		v.dsize = strlen(buf) + 1;
		if (mdbm_store(db, k, v, MDBM_INSERT)) {
			fprintf(stderr, "lineName cache: insert error\n");
			return (0);
		}
	}
	sprintf(&buf[v.dsize - 1], ".%u", lnum);
	return (buf);
}

private int
get_reg(sccs *s, char *printOut, int flags, delta *d,
		int *ln, char *iLst, char *xLst)
{
	serlist *state = 0;
	ser_t	*slist = 0;
	int	lines = 0, print = 0, error = 0;
	int	seq;
	int	encoding = (flags&GET_ASCII) ? E_ASCII : s->encoding;
	unsigned int sum;
	u32	same, added, deleted, other, *counter;
	FILE 	*out = 0;
	char	*buf, *name = 0, *gfile = 0;
	MDBM	*DB = 0;
	int	hash = 0;
	int	hashFlags = 0;
	int	sccs_expanded, rcs_expanded;
	int	lf_pend = 0;
	char	*eol = "\n";
	ser_t	serial;
	char	align[16];
	char	lnamebuf[MD5LEN+32]; /* md5sum + '.' + linenumber */
	MDBM	*namedb = 0;
	u32	*lnum = 0;

	assert(!BINPOOL(s));
	if (EOLN_WINDOWS(s)) eol = "\r\n";
	slist = d ? serialmap(s, d, iLst, xLst, &error)
		  : setmap(s, D_SET, 0);
	if (error) {
		assert(!slist);
		switch (error) {
		    case 1:
			fprintf(stderr,
			    "Malformed include/exclude list for %s\n",
			    s->sfile);
			break;
		    case 2:
			fprintf(stderr,
			    "Can't find specified rev in include/exclude "
			    "list for %s\n", s->sfile);
			break;
		    case 3:
			fprintf(stderr,
			    "Error in include/exclude:\n"
			    "\tSame revision appears "
			    "in both lists for %s\n", s->sfile);
			break;
		    default:
			fprintf(stderr,
			    "Error in converting version plus include/exclude "
			    "to a set for %s\n", s->sfile);
			break;
		}
		s->state |= S_WARNED;
		return 1;
	}
	if (flags & GET_SUM) {
		flags |= NEWCKSUM;
	} else if (d && BITKEEPER(s) && !iLst && !xLst) {
		flags |= NEWCKSUM;
	}
	/* we're changing the meaning of the file, checksum would be invalid */
	if (HASH(s)) {
		if (flags & GET_NOHASH) flags &= ~NEWCKSUM;
	} else {
		if (flags & GET_HASH) flags &= ~NEWCKSUM;
	}

	if ((HASH(s) && !(flags & GET_NOHASH)) || (flags & GET_HASH)) {
		hash = 1;
		if (CSET(s)) {
			hashFlags = DB_KEYFORMAT;
		} else if (CONFIG(s)) {
			hashFlags = DB_CONFIG;
		}
		unless ((encoding & E_DATAENC) == E_ASCII) {
			fprintf(stderr, "get: has files must be ascii.\n");
			s->state |= S_WARNED;
			goto out;
		}
		unless (DB = mdbm_open(NULL, 0, 0, GOOD_PSIZE)) {
			fprintf(stderr, "get: bad MDBM.\n");
			s->state |= S_WARNED;
			goto out;
		}
		assert(proj_root(s->proj));
	}

	/* Think carefully before changing this */
	if (BINARY(s) || hash) {
		flags &= ~(GET_EXPAND|GET_PREFIX);
	}
	unless (SCCS(s) || RCS(s)) flags &= ~GET_EXPAND;

	if (flags & GET_LINENAME) {
		lnum = calloc(s->nextserial, sizeof(*lnum));
		namedb = mdbm_mem();
	}
	if (flags & GET_MODNAME) {
		name = basenm(d ? d->pathname : s->gfile);
	} else if (flags & GET_RELPATH) {
		name = d ? d->pathname : s->gfile;
	}

	/*
	 * We want the data to start on a tab aligned boundry
	 */
	if ((flags & GET_PREFIX) && (flags & GET_ALIGN)) {
		int	len = 0;

		if (flags&(GET_MODNAME|GET_RELPATH)) len += strlen(name) + 1;
		if (flags&GET_PREFIXDATE) len += YEAR4(s) ? 11 : 9;
		if (flags&GET_USER) len += s->userLen + 1;
		if (flags&GET_REVNUMS) len += s->revLen + 1;
		if (flags&GET_LINENUM) len += 7;
		if (flags&GET_LINENAME) len += 37;
		len += 2;
		align[0] = 0;
		while (len++ % 8) strcat(align, " ");
		strcat(align, "| ");
	}

	state = allocstate(0, s->nextserial);

	unless (flags & (GET_HASHONLY|GET_SUM)) {
		gfile = d ? setupOutput(s, printOut, flags, d) : printOut;
		if ((gfile == (char *) 0) || (gfile == (char *)-1)) {
out:			if (slist) free(slist);
			if (state) free(state);
			if (DB) mdbm_close(DB);
			/*
			 * 0 == OK
			 * 1 == error
			 * 2 == No reget
			 */
			unless (gfile) return (2);
			return (1);
		}
		openOutput(s, encoding, gfile, &out);
		unless (out) {
			fprintf(stderr,
			    "get_reg: Can't open %s for writing\n", gfile);
			perror(gfile);
			fflush(stderr);
			goto out;
		}
	}
	seq = 0;
	sum = 0;
	added = 0;
	deleted = 0;
	same = 0;
	other = 0;
	counter = &other;
	seekto(s, s->data);
	if (s->encoding & E_GZIP) zgets_init(s->where, s->size - s->data);
	while (buf = nextdata(s)) {
		register u8 *e, *e1, *e2;

		e1= e2 = 0;
		if (isData(buf)) {
			++seq;
			(*counter)++;
			if (lnum) { /* count named lines */
				lnum[print ? print
				    : whatstate((const serlist*)state)]++;
			}
			if (buf[0] == CNTLA_ESCAPE) {
				assert((encoding & E_DATAENC) == E_ASCII);
				buf++; /* skip the escape character */
			}
			if (!print) {
				/* if we are skipping data from pending block */
				if (lf_pend &&
				    lf_pend == whatstate((const serlist*)state))
				{
					unless (flags & GET_SUM) {
						fputs(eol, out);
					}
					if (flags & NEWCKSUM) sum += '\n';
					lf_pend = 0;
				}
				continue;
			}
			if (hash) {
				if (getKey(DB, buf, hashFlags|flags) == 1) {
					unless (flags &
					    (GET_HASHONLY|GET_SUM)) {
						fnlputs(buf, out);
					}
					if (flags & NEWCKSUM) {
						for (e = buf;
						    *e != '\n'; sum += *e++);
						sum += '\n';
					}
					lines++;
				}
				continue;
			}
			lines++;
			if (lf_pend) {
				unless (flags & GET_SUM) fputs(eol, out);
				if (flags & NEWCKSUM) sum += '\n';
				lf_pend = 0;
			}
			if (flags & NEWCKSUM) {
				for (e = buf; *e != '\n'; sum += *e++);
				sum += '\n';
			}
			if (flags&GET_SEQ) smerge_saveseq(seq);
			if (flags & GET_PREFIX) {
				delta	*tmp = sfind(s, (ser_t) print);
				char	*p = 0;

				prefix(s, tmp, flags, lines, name, out);

				/* GET_LINENAME must be last for mdiff */
				if (flags & GET_LINENAME) {
					p = get_lineName(s, print,
					    namedb, lnum[print], lnamebuf);
					assert(p &&
					    strlen(p) < sizeof(lnamebuf));
				}
				if (flags & GET_ALIGN) {
					if (p) fprintf(out, "%-36s", p);
					fprintf(out, align);
				} else {
					if (p) fprintf(out, "%s\t", p);
				}
			}

			e = buf;
			sccs_expanded = rcs_expanded = 0;
			unless (flags & GET_EXPAND) goto write;
			if (SCCS(s)) {
				for (e = buf; *e != '%' && *e != '\n'; e++);
				if (*e == '%') {
					e = e1 =
					    expand(s, d, buf, &sccs_expanded);
					if (sccs_expanded && EXPAND1(s)) {
						flags &= ~GET_EXPAND;
					}
				} else {
					e = buf;
				}
			}
			if (RCS(s)) {
				char	*t;

				for (t = e; *t != '$' && *t != '\n'; t++);
				if (*t == '$') {
					e = e2 =
					    rcsexpand(s, d, e, &rcs_expanded);
					if (rcs_expanded && EXPAND1(s)) {
						flags &= ~GET_EXPAND;
					}
				}
			} 

write:
			switch (encoding) {
			    case E_GZIP|E_UUENCODE:
			    case E_UUENCODE: {
				uchar	obuf[50];
				int	n;
				
				unless (flags & GET_SUM) {
					n = uudecode1(e, obuf);
					fwrite(obuf, n, 1, out);
				}
				break;
			    }
			    case E_ASCII:
			    case E_ASCII|E_GZIP:
				unless (flags & GET_SUM) fnnlputs(e, out);
				if (flags & NEWCKSUM) sum -= '\n';
				lf_pend = print;
				if (sccs_expanded) free(e1);
				if (rcs_expanded) free(e2);
				break;
			    default:
				assert(0);
				break;
			}
			continue;
		}

		debug2((stderr, "%.*s", linelen(buf), buf));
		serial = atoi(&buf[3]);
		/* seek out E which closes text block for last line
		 * printed.  serial for that block is in lf_pend.
		 * whatstate returns serial of current text block
		 * This is needed to make sure E isn't closing a D
		 * The whatstate test isn't needed assuming diff
		 * semantics of a replace is a delete followed
		 * by an insert.  We know in that case, the condition
		 * E and lf_pend == serial is enough because if E
		 * is tagged 'N', there can be no 'D' following it.
		 * If E isn't tagged, then lf_pend gets cleared.
		 * In the name of robustness, that someday this
		 * assumption might not be true, the whatstate check
		 * is in there.
		 */
		if (buf[1] == 'E' && lf_pend == serial &&
		    whatstate((const serlist*)state) == serial)
		{
			char	*n = &buf[3];
			while (isdigit(*n)) n++;
			if (*n != 'N') {
				unless (flags & GET_SUM) fputs(eol, out);
				lf_pend = 0;
				if (flags & NEWCKSUM) sum += '\n';
			} else {
				s->has_nonl = 1;
			}
		}
		changestate(state, buf[1], serial);
		if (d) {
			print = printstate((const serlist*)state,
					(const ser_t*)slist);
			unless (flags & NEWCKSUM) {
				/* don't recalc add/del/same unless CKSUM */
			}
			else if (print == d->serial) {
				counter = &added;
			}
			else if (print) {
				counter = &same;
			}
			else if (delstate(d->serial, (const serlist*)state,
					(const ser_t*)slist))
			{
				counter = &deleted;
			}
			else {
				counter = &other;
			}
		}
		else {
			print = visitedstate((const serlist*)state,
					(const ser_t*)slist);
		}
	}
	if (BITKEEPER(s) &&
	    d && (flags & NEWCKSUM) && !(flags&GET_SHUTUP) && lines) {
		delta	*z = getCksumDelta(s, d);

		if (!z || ((sum_t)sum != z->sum)) {
		    fprintf(stderr,
			"get: bad delta cksum %u:%d for %s in %s, %s\n",
			(sum_t)sum, z ? z->sum : -1, d->rev, s->sfile,
			"gotten anyway.");
		}
	}
	/* Try passing back the sum in dsum in case someone wants it */
	s->dsum = sum;
	s->added = added;
	s->deleted = deleted;
	s->same = same;

	if (flags & (GET_HASHONLY|GET_SUM)) {
		error = 0;
	} else {
		if (error = flushFILE(out)) {
			/*
			 * In spite of flushFILE() looking like it catches
			 * EPIPE, it doesn't.  So we look for that case
			 * here.
			 */
			unless ((flags&PRINT) && streq("-", printOut)) {
				perror(s->gfile);
				s->io_error = s->io_warned = 1;
			} else {
				error = 0;
			}
		}
		if (flags & PRINT) {
			unless (streq("-", printOut)) fclose(out);
		} else {
			fclose(out);
		}
	}
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) {
			error = 1;
			s->io_error = s->io_warned = 1;
		}
		s->zin = 0;
	}

	if (error) {
		unless (flags & PRINT) unlink(s->gfile);
		if (DB) mdbm_close(DB);
		return (1);
	}

	/* Win32 restriction, must do this before we chmod to read only */
	if (d && (flags&GET_DTIME)){
		char	*fname = (flags&PRINT) ? printOut : s->gfile;
		int	doit = 1;
		time_t	now;
		struct	utimbuf ut;

		/*
		 * If we are doing a regular SCCS/s.foo -> foo get then
		 * we set the gfile time iff we can set the sfile time.
		 * This keeps make happy.
		 */
		ut.modtime = d->date - d->dateFudge;
		unless (flags & PRINT) {
			now = time(0);
			if (ut.modtime > now) ut.modtime = now;
			s->gtime = ut.modtime;
			if (sccs_setStime(s, s->stime)) doit = 0;
		}
		ut.actime = ut.modtime;
		if (doit && !streq(fname, "-") && (utime(fname, &ut) != 0)) {
			char	*msg;

			msg = aprintf("Cannot set mod time on %s:", fname);
			perror(msg);
			free(msg);
			s->state |= S_WARNED;
			goto out;
		}
	}

	unless (hash && (flags&GET_HASHONLY)) {
		int 	rc = 0;

		if (flags&GET_EDIT) {
			if (d->mode) {
				rc = chmod(s->gfile, d->mode);
			} else {
				rc = chmod(s->gfile, 0666);
			}
		} else if (!(flags&PRINT)) {
			if (d->mode) {
				rc = chmod(s->gfile, d->mode & ~0222);
			} else {
				rc = chmod(s->gfile, 0444);
			}
		}
		if (rc) {
			fprintf(stderr,
				"get_reg: cannot chmod %s\n", s->gfile);
			perror(s->gfile);
		}
	}


#ifdef X_SHELL
	if (SHELL(s) && ((flags & PRINT) == 0)) {
		char	*path = strdup(getenv("PATH"));
		char	*t;
		char	cmd[MAXPATH];

		safe_putenv("PATH=%s", getenv("BK_OLDPATH"));
		t = strrchr(s->gfile, '/');
		if (t) {
			*t = 0;
			sprintf(cmd, "cd '%s'; sh '%s' -o", s->gfile, &t[1]);
			*t = '/';
		} else {
			sprintf(cmd, "sh '%s' -o", s->gfile);
		}
		system(cmd);
		safe_putenv("PATH=%s", path);
		free(path);
	}
#endif
	*ln = lines;
	if (slist) free(slist);
	if (state) free(state);
	if (namedb) mdbm_close(namedb);
	if (lnum) free(lnum);
	if (DB) {
		if (s->mdbm) mdbm_close(s->mdbm);
		s->mdbm = DB;
	}
	return 0;
}

private int
get_bp(sccs *s, char *printOut, int flags, delta *d,
		int *ln, char *iLst, char *xLst)
{
	char	*gfile = 0;
	int	error = 0;

	assert(BINPOOL(s) && BITKEEPER(s));
	assert(d);

	/*
	 * Supported flags are: GET_EDIT (handled elsewhere),
	 * GET_SKIPGET (handled elsewhere)
	 * GET_SHUTUP (dunno)
	 * GET_FORCE (dunno)
	 * GET_DTIME
	 * GET_NOREGET (handled in setupOutput)
	 * GET_SUM
	 */
#define	BAD	(GET_PREFIX|GET_ASCII|GET_ALIGN|GET_HEADER|\
		GET_NOHASH|GET_HASHONLY|GET_DIFFS|GET_BKDIFFS|GET_HASHDIFFS|\
		GET_HASH|GET_SEQ|GET_COMMENTS)
	if (flags & BAD) {
		fprintf(stderr,
		    "get: bad flags on get for %s: %x\n", s->gfile, flags);
		return (1);
	}

	unless (flags & GET_SUM) {
		gfile = setupOutput(s, printOut, flags, d);
		if ((gfile == (char *) 0) || (gfile == (char *)-1)) {
			/*
			 * 1 == error
			 * 2 == No reget
			 */
			unless (gfile) return (2);
			return (1);
		}
	}
	if (error = bp_get(s, d, flags, gfile)) {
		unless (error == EAGAIN) return (1);
		if (flags & GET_NOREMOTE) {
			s->cachemiss = 1;
			return (1);
		} else if (bp_fetch(s, d)) {
			fprintf(stderr,
			    "binpool: fetch failed for %s\n", s->gfile);
			return (1);
		} else if (error = bp_get(s, d, flags, gfile)) {
			fprintf(stderr,
			    "binpool: get after fetch failed for %s\n",
			    s->gfile);
			return (1);
		}
	}
	/* Track get_reg from here on down (mostly) */
	if (BITKEEPER(s) &&
	    (flags & NEWCKSUM) && !(flags & GET_SHUTUP) && s->added) {
		delta	*z = getCksumDelta(s, d);

		if (!z || (s->dsum != z->sum)) {
		    fprintf(stderr,
			"get: bad delta cksum %u:%d for %s in %s, %s\n",
			s->dsum, z ? z->sum : -1, d->rev, s->sfile,
			"gotten anyway.");
		}
	}

	/* Win32 restriction, must do this before we chmod to read only */
	if ((flags & GET_DTIME) && !(flags & PRINT)) {
		time_t	now;
		struct	utimbuf ut;
		char	*msg;

		/*
		 * If we are doing a regular SCCS/s.foo -> foo get then
		 * we set the gfile time iff we can set the sfile time.
		 * This keeps make happy.
		 */
		ut.modtime = d->date - d->dateFudge;
		now = time(0);
		if (ut.modtime > now) ut.modtime = now;
		s->gtime = ut.actime = ut.modtime;
		assert(gfile);
		if ((sccs_setStime(s, s->stime) == 0) &&
		    (utime(gfile, &ut) != 0)) {
			msg = aprintf("Cannot set mod time on %s:", gfile);
			perror(msg);
			free(msg);
			s->state |= S_WARNED;
			error = 1;
		}
	}
	if (ln) *ln = s->added;
	return (error);
}

private int
get_link(sccs *s, char *printOut, int flags, delta *d, int *ln)
{
	char *f = setupOutput(s, printOut, flags, d);
	u8 *t;
	u16 dsum = 0;
	delta	*e;

	unless (f) return 2;

	/*
	 * What we want is to just checksum the symlink.
	 * However due two bugs in old binary, we do not have valid check if:
	 * a) It is a 1.1 delta
	 * b) It is 1.1.* delta (the 1.1 delta got moved after a merge)
	 * c) The recorded checsum is zero.
	 */
	e = getSymlnkCksumDelta(s, d);
	if ((e->flags & D_CKSUM) && (e->sum != 0) &&
	     !streq(e->rev, "1.1") && !strneq(e->rev, "1.1.", 4)) {
		for (t = d->symlink; *t; t++) dsum += *t;
		if (e->sum != dsum) {
			fprintf(stderr,
				"get: bad delta cksum %u:%d for %s in %s, %s\n",
				dsum, d->sum, d->rev, s->sfile,
				"gotten anyway.");
		}
	}
	if (flags & PRINT) {
		int	ret;
		FILE 	*out;

		ret = openOutput(s, E_ASCII, f, &out);
		assert(ret == 0);
		unless (out) {
			fprintf(stderr,
			    "get_link: Can't open %s for writing\n", f);
			fflush(stderr);
			return 1;
		}
		if (flags & GET_PREFIX) {
			char	*name = 0;

			assert(d->pathname);
			if (flags & GET_MODNAME) name = basenm(d->pathname);
			if (flags & GET_RELPATH) name = d->pathname;
			prefix(s, d, flags, 1, name, out);
			if (flags & GET_ALIGN) {
				int	len = 0;

				if (flags&(GET_MODNAME|GET_RELPATH)) {
					len += strlen(name) + 1;
				}
				if (flags&GET_PREFIXDATE) {
					len += YEAR4(s) ? 11 : 9;
				}
				if (flags&GET_USER) len += s->userLen + 1;
				if (flags&GET_REVNUMS) len += s->revLen + 1;
				if (flags&GET_LINENUM) len += 7;
				len += 2;
				while (len++ % 8) fputc(' ', out);
				fputs("| ", out);
			}
			// XXX - no GET_LINENAME (yet)
		}
		fprintf(out, "SYMLINK -> %s\n", d->symlink);
		unless (streq("-", f)) fclose(out);
		*ln = 1;
	} else {
		unless (symlink(d->symlink, f) == 0 ) {
#ifdef WIN32
			getMsg("symlink", s->gfile, '=', stderr);
#else
			perror(f);
#endif
			return 1;
		}
		*ln = 0;
	}
	return 0;
}

/*
 * get the specified revision.
 * The output file is passed in so that callers can redirect it.
 * iLst and xLst are malloced and get() frees them.
 */
int
sccs_get(sccs *s, char *rev,
	char *mRev, char *iLst, char *xLst, u32 flags, char *printOut)
{
	delta	*d;
	int	lines = -1, locked = 0, error;
	char	*i2 = 0;

	debug((stderr, "get(%s, %s, %s, %s, %s, %x, %s)\n",
	    s->sfile, notnull(rev), notnull(mRev),
	    notnull(iLst), notnull(xLst), flags, printOut));
	if (BITKEEPER(s) && !s->tree->pathname) {
		fprintf(stderr, "get: no pathname for %s\n", s->sfile);
		return (-1);
	}
	unless (s->state & S_SOPEN) {
		fprintf(stderr, "get: couldn't open %s\n", s->sfile);
err:		if (i2) free(i2);
		if (locked) {
			sccs_unlock(s, 'p');
			sccs_unlock(s, 'z');
			s->state &= ~(S_PFILE|S_ZFILE);
		}
		return (-1);
	}
	unless (s->cksumok) {
		fprintf(stderr, "get: bad chksum on %s\n", s->sfile);
		goto err;
	}
	unless (HASGRAPH(s)) {
		fprintf(stderr, "get: no/bad delta tree in %s\n", s->sfile);
		goto err;
	}
	if ((s->state & S_BADREVS) && !(flags & GET_FORCE)) {
		fprintf(stderr,
		    "get: bad revisions, run renumber on %s\n", s->sfile);
		s->state |= S_WARNED;
		goto err;
	}
	/* XXX: if 'not in view' file, then ignore: NOT IMPLEMENTED YET */

	/* this has to be above the sccs_getedit() - that changes the rev */
	if (mRev) {
		char *tmp;

		tmp = sccs_impliedList(s, "get", rev, mRev);
#ifdef  CRAZY_WOW
		// XXX: why was this here?  Should revisions that are
		// inline (get -e -R1.7 -M1.5 foo) be an error?
		// Needed to take this out because valid merge set
		// could be empty (see t.merge for example)
		unless (tmp) goto err;
#endif
		/* XXX this is bogus if tmp==0 and iLst is set */
		i2 = strconcat(tmp, iLst, ",");
		if (tmp && i2 != tmp) free(tmp);
	}
	if (rev && streq(rev, "+")) rev = 0;
	if (flags & GET_EDIT) {
		d = sccs_getedit(s, &rev);
		if (!d) {
			fprintf(stderr, "get: can't find revision %s in %s\n",
			    notnull(rev), s->sfile);
			s->state |= S_WARNED;
		}
		if (flags & PRINT) {
			fprintf(stderr, "get: can't combine edit and print\n");
			s->state |= S_WARNED;
			d = 0;
		}
	} else {
		d = sccs_findrev(s, rev ? rev : "+");
		unless (d) {
			verbose((stderr,
			    "get: can't find revision like %s in %s\n",
			rev, s->sfile));
			s->state |= S_WARNED;
		}
	}
	unless (d) goto err;

	if (flags & GET_EDIT) {
		if (write_pfile(s, flags, d, rev, iLst, i2, xLst, mRev)) {
			goto err;
		}
		locked = 1;
	}
	if (flags & GET_SKIPGET) {
		/*
		 * XXX - need to think about this for various file types.
		 * Remove read only files if we are editing.
		 * Do not error if there is a writable gfile, they may have
		 * wanted that.
		 */
		if ((flags & GET_EDIT) && HAS_GFILE(s) && !WRITABLE(s)) {
			unlink(s->gfile);
		}
		goto skip_get;
	}

	/*
	 * Based on the file type,
	 * we call the appropriate function to get the body
 	 */
	switch (fileType(d->mode)) {
	    case 0:		/* uninitialized mode, assume regular file */
	    case S_IFREG:	/* regular file */
		if (BINPOOL(s)) {
			error = get_bp(s, printOut, flags, d, &lines, 0, 0);
			break;
		} 
		error =
		    get_reg(s, printOut, flags, d, &lines, i2? i2 : iLst, xLst);
		break;
	    case S_IFLNK:	/* symlink */
		error = get_link(s, printOut, flags, d, &lines);
		break;
	    default:
		fprintf(stderr, "get unsupported file type %d\n",
			fileType(d->mode));
		error = 1;
	}
	switch (error) {
	    case 0: break;
	    case 1: goto err;
	    case 2: flags |= SILENT; error = 0; break;	/* reget; no get */
	    default:
		assert("bad error return in get" == 0);
	}
	debug((stderr, "GET done\n"));

skip_get:
	if (flags & GET_EDIT) {
		sccs_unlock(s, 'z');
		s->state &= ~S_ZFILE;
	}
	if (!(flags&SILENT)) {
		fprintf(stderr, "%s %s", s->gfile, d->rev);
		if (i2) {
			fprintf(stderr, " inc: %s", i2);
		} else if (iLst) {
			fprintf(stderr, " inc: %s", iLst);
		}
		if (xLst) {
			fprintf(stderr, " exc: %s", xLst);
		}
		if (flags & GET_EDIT) {
			fprintf(stderr, " -> %s", rev);
		}
		unless (flags & GET_SKIPGET) {
			if (lines >= 0) fprintf(stderr, ": %d %s",
			    lines, (BINPOOL(s) ? "bytes" : "lines"));
		}
		fprintf(stderr, "\n");
	}
	if (i2) free(i2);
	return (0);
}

/*
 * XXX - the userLen/revLen should be calculated for the set of serials that
 * we are displaying, not the full set.
 */
private void
prefix(sccs *s, delta *d, u32 flags, int lines, char *name, FILE *out)
{
	if (flags & GET_ALIGN) {
		if (flags&(GET_MODNAME|GET_RELPATH)) fprintf(out, "%s ", name);
		if (flags&GET_PREFIXDATE) {
			if (YEAR4(s)) {
				if (atoi(d->sdate) > 69) {
					fprintf(out, "19");
				} else {
					fprintf(out, "20");
				}
			}
			fprintf(out, "%8.8s ", d->sdate);
		}
		if (flags&GET_USER) fprintf(out, "%-*s ", s->userLen, d->user);
		if (flags&GET_REVNUMS) fprintf(out, "%-*s ", s->revLen, d->rev);
		if (flags&GET_LINENUM) fprintf(out, "%6d ", lines);
	} else {
#if 0
		/* maybe, need to think about it */
		if (flags&(GET_MODNAME|GET_RELPATH)) fprintf(out, "%s|",name);
		if (flags&GET_PREFIXDATE) {
			if (YEAR4(s)) {
				if (atoi(d->sdate) > 69) {
					fprintf(out, "19");
				} else {
					fprintf(out, "20");
				}
			}
			fprintf(out, "%8.8s|", d->sdate);
		}
		if (flags&GET_USER) fprintf(out, "%s|", d->user);
		if (flags&GET_REVNUMS) fprintf(out, "%s|", d->rev);
		if (flags&GET_LINENUM) fprintf(out, "%d|", lines);
#else
		/* tab style */
		if (flags&(GET_MODNAME|GET_RELPATH)) fprintf(out, "%s\t",name);
		if (flags&GET_PREFIXDATE) {
			if (YEAR4(s)) {
				if (atoi(d->sdate) > 69) {
					fprintf(out, "19");
				} else {
					fprintf(out, "20");
				}
			}
			fprintf(out, "%8.8s\t", d->sdate);
		}
		if (flags&GET_USER) fprintf(out, "%s\t", d->user);
		if (flags&GET_REVNUMS) fprintf(out, "%s\t", d->rev);
		if (flags&GET_LINENUM) fprintf(out, "%d\t", lines);
#endif
	}
}

/*
 * cat the delta body formatted according to flags.
 */
int
sccs_cat(sccs *s, u32 flags, char *printOut)
{
	int	lines = 0, error;

	debug((stderr, "annotate(%s, %x, %s)\n",
	    s->sfile, flags, printOut));
	unless (s->state & S_SOPEN) {
		fprintf(stderr, "annotate: couldn't open %s\n", s->sfile);
err:		return (-1);
	}
	unless (s->cksumok) {
		fprintf(stderr, "annotate: bad chksum on %s\n", s->sfile);
		goto err;
	}
	unless (HASGRAPH(s)) {
		fprintf(stderr, "annotate: no delta tree in %s\n", s->sfile);
		goto err;
	}
	if ((s->state & S_BADREVS) && !(flags & GET_FORCE)) {
		fprintf(stderr,
		    "annotate: bad revisions, run renumber on %s\n", s->sfile);
		s->state |= S_WARNED;
		goto err;
	}
	if (BINARY(s)) {
		fprintf(stderr,
		    "annotate: can't annotate binary %s\n", s->gfile);
		s->state |= S_WARNED;
		goto err;
	}

	error = get_reg(s, printOut, flags, 0, &lines, 0, 0);
	if (error) return (-1);

	debug((stderr, "SCCSCAT done\n"));

	return (0);
}

/* These are used to describe 'side' of a diff */
#define	NEITHER 0
#define	LEFT	1
#define	RIGHT	2
#define	BOTH	3

private int
outdiffs(sccs *s, int type, int side, int *left, int *right, int count,
	int no_lf, FILE *in, FILE *out)
{
	char	*prefix;

	unless (count)  return (0);

	if (side == RIGHT) {
		if (type == GET_BKDIFFS) {
			fprintf(out, "%c%d %d\n", no_lf ? 'N' : 'I',
				*left, count);
			*right += count; /* not used, but easy to inc */
			prefix = "";
		} else {
			fprintf(out, "%da%d", *left, *right+1);
			*right += count;
			if (count != 1)
				fprintf(out, ",%d", *right);
			fputc('\n', out);
			prefix = "> ";
		}
	} else {
		assert(side == LEFT);
		if (type == GET_BKDIFFS) {
			fprintf(out, "D%d %d\n", *left+1, count);
			*left += count;
			prefix = "";
		} else {
			fprintf(out, "%d", *left+1);
			*left += count;
			if (count != 1)
				fprintf(out, ",%d", *left);
			fprintf(out, "d%d\n", *right);
			prefix = "< ";
		}
	}

	/* print out text block unless a diff -n type of delete */
	unless (type == GET_BKDIFFS && side == LEFT) {
		fseek(in, 0L, SEEK_SET);
		while (count--) {
			char	buf[MAXLINE];

			fputs(prefix, out);
			if (fnext(buf, in)) {
				/*
				 * We're escaping blank lines and the escape
				 * character itself.  Takepatch wants the
				 * diffs ended with ^\n so blank lines confuse
				 * it.
				 * Note that these are stripped in the delta
				 * routine.
				 */
				if ((buf[0] == '\\') || (buf[0] == '\n')) {
					fputs("\\", out);
				}
				fputs(buf, out);

				/*
				 * This loop is here to handle line
				 * that is longer than the MAXLINE buffer size
				 *
				 * XXX TODO: should mmap the tmp file
				 */
				while (buf[strlen(buf) - 1] != '\n') {
					unless (fnext(buf, in)) {
						fprintf(stderr,
		    "get: getdiffs temp file has no line-feed termination\n");
						fputs(buf, out);
						break;
					}
					fputs(buf, out);
				}
			} else {
				fprintf(stderr,
				    "get: getdiffs temp file early EOF\n");
				s->state |= S_WARNED;
				return (-1);
			}
		}
		fseek(in, 0L, SEEK_SET);
	}
	return (0);
}

/*
 * Get the diffs of the specified revision.
 * The diffs are only in terms of deletes and adds, no changes.
 * The output file is passed in so that callers can redirect it.
 */
int
sccs_getdiffs(sccs *s, char *rev, u32 flags, char *printOut)
{
	int	type = flags & (GET_DIFFS|GET_BKDIFFS|GET_HASHDIFFS);
	serlist *state = 0;
	ser_t	*slist = 0;
	ser_t	old = 0;
	delta	*d;
	int	with = 0, without = 0;
	int	count = 0, left = 0, right = 0;
	FILE	*out = 0;
	int	encoding = (flags&GET_ASCII) ? E_ASCII : s->encoding;
	int	error = 0;
	int	side, nextside;
	char	*buf;
	char	*tmpfile = 0, *tmppat = 0;
	FILE	*lbuf = 0;
	int	no_lf = 0;
	ser_t	serial;
	int	ret = -1;

	unless (s->state & S_SOPEN) {
		fprintf(stderr, "getdiffs: couldn't open %s\n", s->sfile);
		s->state |= S_WARNED;
		return (-1);
	}
	unless (s->cksumok) {
		fprintf(stderr, "getdiffs: bad chksum on %s\n", s->sfile);
		s->state |= S_WARNED;
		return (-1);
	}
	unless (HASGRAPH(s)) {
		fprintf(stderr,
		    "getdiffs: no/bad delta tree in %s\n", s->sfile);
		s->state |= S_WARNED;
		return (-1);
	}
	if (s->state & S_BADREVS) {
		fprintf(stderr,
		    "getdiffs: bad revisions, run renumber on %s\n", s->sfile);
		s->state |= S_WARNED;
		return (-1);
	}
	unless (d = findrev(s, rev)) {
		fprintf(stderr, "get: can't find revision like %s in %s\n",
		    rev, s->sfile);
		s->state |= S_WARNED;
		return (-1);
	}
	tmppat = aprintf("%s-%d", basenm(s->gfile), d->serial);
	tmpfile = bktmp(0, tmppat);
	free(tmppat);
	openOutput(s, encoding, printOut, &out);
	setmode(fileno(out), O_BINARY); /* for win32 EOLN_NATIVE file */
	if (type == GET_HASHDIFFS) {
		int	lines = 0;
		int	f = PRINT;
		int	hash = s->xflags & X_HASH;
		int	set = d->flags & D_SET;
		char	b[MAXLINE];

		s->xflags |= X_HASH;
		d->flags |= D_SET;
		ret = get_reg(s, tmpfile, f|flags, 0, &lines, 0, 0);
		unless (hash) s->xflags &= ~X_HASH;
		unless (set) d->flags &= ~D_SET;
		unless ((ret == 0) && (lines != 0)) {
		    	goto done3;
		}
		unless (lbuf = fopen(tmpfile, "r")) {
			perror(tmpfile);
			ret = -1;
			goto done2;
		}
		/* XXX: NOT YET fprintf(out, "I0 %u\n", lines); */
		fprintf(out, "0a0\n");
		while (fnext(b, lbuf)) {
			fputs("> ", out);
			fputs(b, out);
		}
		fclose(lbuf);
		lbuf = NULL;
		unlink(tmpfile);
		goto done2;
	}
	unless (lbuf = fopen(tmpfile, "w+")) {
		perror(tmpfile);
		fprintf(stderr, "getdiffs: couldn't open %s\n", tmpfile);
		s->state |= S_WARNED;
		goto done2;
	}
	slist = serialmap(s, d, 0, 0, &error);
	state = allocstate(0, s->nextserial);
	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
	}
	side = NEITHER;
	nextside = NEITHER;

	while (buf = nextdata(s)) {
		unless (isData(buf)) {
			debug2((stderr, "%.*s", linelen(buf), buf));
			serial = atoi(&buf[3]);
			if (buf[1] == 'E' && serial == with &&
			    serial == d->serial)
			{
				char	*n = &buf[3];
				while (isdigit(*n)) n++;
				if (*n == 'N') no_lf = 1;
			}
			changestate(state, buf[1], serial);
			with = printstate((const serlist*)state,
					(const ser_t*)slist);
			old = slist[d->serial];
			slist[d->serial] = 0;
			without = printstate((const serlist*)state,
				    	(const ser_t*)slist);
			slist[d->serial] = old;

			nextside = with ? (without ? BOTH : RIGHT)
					: (without ? LEFT : NEITHER);
			continue;
		}
		if (nextside == NEITHER) continue;
		if (count &&
		    nextside != side && (side == LEFT || side == RIGHT)) {
			if (outdiffs(s, type,
			    side, &left, &right, count, no_lf, lbuf, out)) {
				goto done;
			}
			count = 0;
			no_lf = 0;
		}
		side = nextside;
		switch (side) {
		    case LEFT:
		    case RIGHT:
			count++;
			if ((type == GET_DIFFS) || (side == RIGHT)) {
				if (buf[0] == CNTLA_ESCAPE) {
					assert((encoding & E_DATAENC)
					    == E_ASCII);
					buf++; /* skip the escape character */
				}
				fnlputs(buf, lbuf);
			}
			break;
		    case BOTH:
			left++, right++; break;
		}
	}
	if (count) { /* there is something left in the buffer */
		if (outdiffs(s, type, side, &left, &right, count, no_lf,
		    lbuf, out))
		{
			goto done;
		}
		count = 0;
		no_lf = 0;
	}
	ret = 0;
done:   
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) {
			s->io_error = 1;
			ret = -1; /* compression failure */
		}
		s->zin = 0;
	}
done2:	/* for GET_HASHDIFFS, the encoding has been handled in get_reg() */
	if (lbuf) {
		if (flushFILE(lbuf)) {
			s->io_error = 1;
			ret = -1; /* i/o error: no disk space ? */
		}
		fclose(lbuf);
	}
done3:
	if (flushFILE(out)) {
		s->io_error = 1;
		ret = -1; /* i/o error: no disk space ? */
	}
	unless (streq("-", printOut)) fclose(out);
	if (slist) free(slist);
	if (state) free(state);
	if (tmpfile) {
		unlink(tmpfile);
		free(tmpfile);
	}
	return (ret);
}

/*
 * Return true if bad cksum
 */
private int
signed_badcksum(sccs *s, int flags)
{
	register char *t;
	register char *end = s->mmap + s->size;
	register unsigned int sum = 0;
	int	filesum;

	debug((stderr, "Checking sum from %p to %p (%d)\n",
	    s->mmap + 8, end, (char*)end - s->mmap - 8));
	assert(s);
	seekto(s, 0);
	filesum = atoi(&s->mmap[2]);
	debug((stderr, "File says sum is %d\n", filesum));
	t = s->mmap + 8;
	end -= 16;
	while (t < end) {
		sum += t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7] +
		    t[8] + t[9] + t[10] + t[11] + t[12] + t[13] + t[14] + t[15];
		t += 16;
	}
	end += 16;
	while (t < end) sum += *t++;
	if ((sum_t)sum == filesum) {
		s->cksumok = 1;
	} else {
		verbose((stderr,
		    "Bad old style checksum for %s, got %d, wanted %d\n",
		    s->sfile, (sum_t)sum, filesum));
	}
	debug((stderr,
	    "%s has %s cksum\n", s->sfile, s->cksumok ? "OK" : "BAD"));
	return ((sum_t)sum != filesum);
}

/*
 * Return true if bad cksum
 */
private int
badcksum(sccs *s, int flags)
{
	register u8 *t;
	register u8 *end = s->mmap + s->size;
	register unsigned int sum = 0;
	int	filesum;

#ifdef	PURIFY
	assert(size(s->sfile) == s->size);
#endif
	debug((stderr, "Checking sum from %p to %p (%d)\n",
	    s->mmap + 8, end, (char*)end - s->mmap - 8));
	assert(s);
	seekto(s, 0);
	s->cksum = filesum = atoi(&s->mmap[2]);
	s->cksumdone = 1;
	debug((stderr, "File says sum is %d\n", filesum));
	t = s->mmap + 8;
	end -= 16;
	while (t < end) {
		sum += t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + t[6] + t[7] +
		    t[8] + t[9] + t[10] + t[11] + t[12] + t[13] + t[14] + t[15];
		t += 16;
	}
	end += 16;
	while (t < end) sum += *t++;
	debug((stderr, "Calculated sum is %d\n", (sum_t)sum));
	if ((sum_t)sum == filesum) {
		s->cksumok = 1;
	} else {
		if (signed_badcksum(s, flags)) {
			verbose((stderr,
			    "Bad checksum for %s, got %d, wanted %d\n",
			    s->sfile, (sum_t)sum, filesum));
		} else {
			return (0);
		}
	}
	debug((stderr,
	    "%s has %s cksum\n", s->sfile, s->cksumok ? "OK" : "BAD"));
	return ((sum_t)sum != filesum);
}

/*
 * If we are regular files, then return sameness in the modes.
 * Otherwise return sameness in the links.
 */
private int
sameMode(delta *a, delta *b)
{
	assert(a && b);
	if (S_ISREG(a->mode) && S_ISREG(b->mode)) return (a->mode == b->mode);
	unless (S_ISLNK(a->mode) && S_ISLNK(b->mode)) return (0);
	assert(a->symlink && b->symlink);
	return (streq(a->symlink, b->symlink));
}

private int
sameFileType(sccs *s, delta *d)
{
	if (d->flags & D_MODE) {
		return (fileType(s->mode) == fileType(d->mode));
	} else {			/* assume no mode means regular file */
		return (S_ISREG(s->mode));
	}
}

/* Formatting routines used by delta_table.
 * sprintf() is so slow that it accounted for ~20% of execution time
 * of delta(1).
 * LMXXX - I've never believed this to be true, zack claimed it.
 * All of these write the requested data into the string provided
 * and return a pointer one past where they stopped writing, so you
 * can write code like
 * p = fmts(p, "blah");
 * p = fmtd(p, number);
 * ...
 * *p = '\0';
 * None of them null-terminate.
 * The names are all 'fmt' + what you'd put after the % in a sprintf format
 * spec (except "fmttt" which stands for "format time_t").
 */
private inline char *
fmts(register char *p, register char *s)
{
	while (*p++ = *s++);
	return (p - 1);
}

private char *
fmtu(char *p, unsigned int u)
{
	char tmp[10];  /* 4294967295 (2**32) has 10 digits.  */
	char *x = &tmp[10];

	if (u == 0) {
		*p++ = '0';
		return p;
	}

	do {
		*--x = u%10 + '0';
		u /= 10;
	} while (u);

	memcpy(p, x, 10 - (x - tmp));
	return p + 10 - (x - tmp);
}

private char *
fmtd(char *p, int d)
{
	if (d < 0) {
		*p++ = '-';
		d = -d;
	}

	return fmtu(p, d);
}

private char *
fmttt(char *p, time_t d)
{
	char tmp[21];  /* 18446744073709551616 (2**64) has 21 digits.
			* time_t may well be a 64 bit quantity.
			*/
	char *x = &tmp[21];

	/* time_t is signed! */
	if (d < 0) {
		*p++ = '-';
		d = -d;
	}

	if (d == 0) {
		*p++ = '0';
		return p;
	}

	do {
		*--x = d%10 + '0';
		d /= 10;
	} while (d);

	memcpy(p, x, 21 - (x - tmp));
	return p + 21 - (x - tmp);
}

private void
check_removed(sccs *s, delta *d, int strip_tags)
{
	assert(d->type == 'R');
	if (d->flags & D_GONE) return;
	if (strip_tags) {
		MK_GONE(s, d);
		return;
	}

	/*
	 * We don't need no skinkin' removed deltas.
	 */
	unless (d->symGraph || (d->flags & D_SYMBOLS) || d->comments) {
		MK_GONE(s, d);
	}
}

/*
 * The table is all here in order, just print it.
 * New in Feb, '99: remove duplicates of metadata.
 */
int
delta_table(sccs *s, FILE *out, int willfix)
{
	delta	*d;
	int	i;	/* used by EACH */
	int	first = willfix;
	char	buf[MAXLINE];
	char	*p;
	int	bits = 0;
	int	gonechkd = 0;
	int	strip_tags = CSET(s) && getenv("_BK_STRIPTAGS");
	int	version = SCCS_VERSION;
	symbol	*sym;
	int	syms_sorted;

	if (getenv("_BK_SCCS_VERSION")) {
		version = atoi(getenv("_BK_SCCS_VERSION"));
		switch (version) {
		    case SCCS_VERSION:
			break;
		    default:
			fprintf(stderr,
			    "Bad version %d, defaulting to current\n", version);
		}
	}
	assert(!READ_ONLY(s));
	assert(s->state & S_ZFILE);
	fprintf(out, "\001%cXXXXX\n", BITKEEPER(s) ? 'H' : 'h');
	s->cksum = 0;
	assert(sizeof(buf) >= 1024);	/* see comment code */

	/*
	 * Add in default xflags if the 1.0 delta doesn't have them.
	 */
	if (BITKEEPER(s)) {
		unless (s->tree->xflags) {
			s->tree->flags |= D_XFLAGS;
			s->tree->xflags = X_DEFAULT;
		}
		/* for old binaries */
		s->tree->xflags |= X_BITKEEPER|X_CSETMARKED;
		if (CSET(s)) {
			s->tree->xflags &= ~(X_SCCS|X_RCS);
			s->tree->xflags |= X_HASH;
		}
	}

	/*
	 * determine if the s->symbols is in order.
	 * If so we can use an optimization to only walk the list once.
	 * XXX In the future s->symbols should be a liblines array
	 *     and changed to always be in order.
	 */
	syms_sorted = 1;
	for (sym = s->symbols; sym; sym = sym->next) {
		unless (sym->metad) { /* csetprune */
			syms_sorted = 0;
			break;
		}
		unless (sym->next) break;
		if (!sym->next->metad ||
		    (sym->metad->serial < sym->next->metad->serial)) {
			syms_sorted = 0;
			break;
		}
	}
	sym = s->symbols;
	for (d = s->table; d; d = d->next) {
		if ((d->next == NULL) && (s->state & S_FAKE_1_0)) {
			/* If the 1.0 delta is a fake, skip it */
			assert(streq(s->table->rev, "1.0"));
			break;
		}
		if (d->type == 'R') check_removed(s, d, strip_tags);
		if (d->flags & D_GONE) {
			/* This delta has been deleted - it is not to be
			 * written out at all.
			 *
			 * All the children (d->kid) of this delta on
			 * the same branch must also have been deleted.
			 * If this branch was merged, the merge node and
			 * all its children must have been deleted too.
			 *
			 * This check is expensive, but D_GONE nodes should
			 * be extremely rare.
			 */
			unless (!gonechkd) {
				assert(!checkGone(s, D_GONE, "delta_table"));
				gonechkd = 1;
			}

			continue;
		}

		CHKDATE(d);

		/*
		 * XXX Whoa, nelly.  This is wrong, we must allow these if
		 * we are doing a takepatch.
		 */
		if (d->parent && BITKEEPER(s) &&
		    (d->date <= d->parent->date)) {
		    	s->state |= S_READ_ONLY;
			fprintf(stderr,
			    "%s@%s: dates do not increase\n", s->sfile, d->rev);
			return (-1);
		}

		/*
		 * Have to leave this for now.  I changed the code to
		 * handle 1/2/3 instead of 00001/00002/00003 but until
		 * everyone upgrades, we have to leave it.
		 * Fix in 3.0.
		 */
		sprintf(buf,
		    "\001s %05d/%05d/%05d\n", d->added, d->deleted, d->same);
		if (strlen(buf) > 21) {
			unless (BITKEEPER(s)) {
				fprintf(stderr,
				    "%s: file too large\n", s->gfile);
				return (-1);
			}
			sccs_fitCounters(buf, d->added, d->deleted, d->same);
		}
		if (first)
			fputs(buf, out);
		else
			fputmeta(s, buf, out);

		p = fmts(buf, "\001d ");
		*p++ = d->type;
		*p++ = ' ';
		p = fmts(p, sccsrev(d));
		*p++ = ' ';
		p = fmts(p, d->sdate);
		*p++ = ' ';
		p = fmts(p, d->user);
		*p++ = ' ';
		p = fmtd(p, d->serial);
		*p++ = ' ';
		p = fmtd(p, d->pserial);
		*p++ = '\n';
		*p = '\0';
		fputmeta(s, buf, out);
		if (d->include) {
			fputmeta(s, "\001i ", out);
			putserlist(s, d->include, out);
			fputmeta(s, "\n", out);
		}
		if (d->exclude) {
			fputmeta(s, "\001x ", out);
			putserlist(s, d->exclude, out);
			fputmeta(s, "\n", out);
		}
		EACH(d->comments) {
			p = fmts(buf, "\001c ");
			p = fmts(p, d->comments[i]);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}

		unless (BITKEEPER(s)) goto SCCS;

		if (d->hash) {
			p = fmts(buf, "\001cA");
			p = fmts(p, d->hash);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
		if (d->csetFile && !(d->flags & D_DUPCSETFILE)) {
			p = fmts(buf, "\001cB");
			p = fmts(p, d->csetFile);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
		if (d->flags & D_CSET) {
			assert(d->type == 'D');
			fputmeta(s, "\001cC\n", out);
		}
		if (d->dangling) fputmeta(s, "\001cD\n", out);
		if (d->dateFudge) {
			p = fmts(buf, "\001cF");
			p = fmttt(p, d->dateFudge);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}

		if (d->hostname && !(d->flags & D_DUPHOST)) {
			p = fmts(buf, "\001cH");
			p = fmts(p, d->hostname);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}

		if (first) {
			fputmeta(s, "\001cK", out);
			s->sumOff = ftell(out);
			fputs("XXXXX", out);
			fputmeta(s, "\n", out);
		} else if (d->flags & D_CKSUM) {
			/*
			 * It turns out not to be worth to save the
			 * few bytes you might save by not making this
			 * a fixed width field.  85% of the sums are
			 * 5 digits, 97% are 4 or 5.
			 * Leaving this fixed means we can diff the
			 * s.files easily.
			 */
			sprintf(buf, "\001cK%05u\n", d->sum);
			fputmeta(s, buf, out);
		}
		if (d->merge) {
			p = fmts(buf, "\001cM");
			p = fmtd(p, d->merge);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
		if (d->pathname && !(d->flags & D_DUPPATH)) {
			p = fmts(buf, "\001cP");
			p = fmts(p, d->pathname);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
		if (d->flags & D_MODE) {
		    	unless (d->parent && sameMode(d->parent, d)) {
				p = fmts(buf, "\001cO");
				p = fmts(p, mode2a(d->mode));
				if (d->symlink) {
					assert(S_ISLNK(d->mode));

					*p++ = ' ';
					p = fmts(p, d->symlink);
				}
				*p++ = '\n';
				*p   = '\0';
				fputmeta(s, buf, out);
			}
		}
		if (d->random) {
			p = fmts(buf, "\001cR");
			p = fmts(p, d->random);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
		if (d->flags & D_SYMBOLS) {
			unless (syms_sorted) sym = s->symbols;
			for (; sym; sym = sym->next) {
				if (syms_sorted &&
				    (sym->metad->serial < d->serial)) {
					break;
				}
				unless (sym->metad == d) continue;
				if (!strip_tags || 
				    streq(KEY_FORMAT2, sym->symname)) {
					p = fmts(buf, "\001cS");
					p = fmts(p, sym->symname);
					*p++ = '\n';
					*p   = '\0';
					fputmeta(s, buf, out);
				}
			}
		}
		/* automagically strip tag serials from non-csetfiles */
		if (!strip_tags && d->symGraph && CSET(s)) {
			p = fmts(buf, "\001cS");
			if (d->ptag) {
				p = fmtd(p, d->ptag);
				if (d->mtag) {
					*p++ = ' ';
					p = fmtd(p, d->mtag);
				}
			} else {
				p = fmtd(p, 0);
			}
			if (d->symLeaf) {
				*p++ = ' ';
				*p++ = 'l';
			}
			*p++ = '\n';
			*p = 0;
			fputmeta(s, buf, out);
		}
		if (d->flags & D_TEXT) {
			unless (d->text) {
				fputmeta(s, "\001cT\n", out);
			} else {
				EACH(d->text) {
					p = buf;
					p = fmts(p, "\001cT ");
					p = fmts(p, d->text[i]);
					*p++ = '\n';
					*p   = '\0';
					fputmeta(s, buf, out);
				}
			}
		}
		if (!d->next) {
			sprintf(buf, "\001cV%u\n", version);
			fputmeta(s, buf, out);
		}
		if (d->flags & D_XFLAGS) {
			sprintf(buf, "\001cX0x%x\n", d->xflags);
			fputmeta(s, buf, out);
		}
		if (d->zone && !(d->flags & D_DUPZONE)) {
			p = fmts(buf, "\001cZ");
			p = fmts(p, d->zone);
			*p++ = '\n';
			*p   = '\0';
			fputmeta(s, buf, out);
		}
SCCS:
		first = 0;
		fputmeta(s, "\001e\n", out);
	}
	fputmeta(s, "\001u\n", out);
	EACH(s->usersgroups) {
		fputmeta(s, s->usersgroups[i], out);
		fputmeta(s, "\n", out);
	}
	fputmeta(s, "\001U\n", out);
	if (BITKEEPER(s) || (s->encoding != E_ASCII)) {
		p = fmts(buf, "\001f e ");
		p = fmtd(p, s->encoding);
		*p++ = '\n';
		*p   = '\0';
		fputmeta(s, buf, out);
	}
	if (BITKEEPER(s)) {
		bits = sccs_xflags(sccs_top(s));
		if (bits) {
			sprintf(buf, "\001f x 0x%x\n", bits);
			fputmeta(s, buf, out);
		}
	}
	if (s->defbranch) {
		p = fmts(buf, "\001f d ");
		p = fmts(p, s->defbranch);
		*p++ = '\n';
		*p   = '\0';
		fputmeta(s, buf, out);
	}
	EACH(s->flags) {
		p = fmts(buf, "\001f ");
		p = fmts(p, s->flags[i]);
		*p++ = '\n';
		*p   = '\0';
		fputmeta(s, buf, out);
	}
	fputmeta(s, "\001t\n", out);
	EACH(s->text) {
		fputmeta(s, s->text[i], out);
		fputmeta(s, "\n", out);
	}
	fputmeta(s, "\001T\n", out);
	if (flushFILE(out)) {
		perror(s->sfile);
		s->io_warned = 1;
		return (-1);
	}
	return (0);
}

/*
 * If we are trying to compare with expanded strings, do so.
 */
private int
expandnleq(sccs *s, delta *d, MMAP *gbuf, char *fbuf, int *flags)
{
	char	*e = fbuf, *e1 = 0, *e2 = 0;
	int sccs_expanded = 0 , rcs_expanded = 0, rc;

	if (BINARY(s)) return (MCMP_DIFF);
	unless (*flags & GET_EXPAND) return (MCMP_DIFF);
	if (SCCS(s)) {
		e = e1 = expand(s, d, e, &sccs_expanded);
		if (EXPAND1(s) && sccs_expanded) *flags &= ~GET_EXPAND;
	}
	if (RCS(s)) {
		e = e2 = rcsexpand(s, d, e, &rcs_expanded);
		if (EXPAND1(s) && rcs_expanded) *flags &= ~GET_EXPAND;
	}
	rc = mcmp(gbuf, e);
	if (sccs_expanded) free(e1);
	if (rcs_expanded) free(e2);
	return (rc);
}

/*
 * This is an expensive call but not as expensive as running diff.
 * flags is same as get flags.
 */
private int
_hasDiffs(sccs *s, delta *d, u32 flags, int inex, pfile *pf)
{
	MMAP	*gfile = 0;
	MDBM	*ghash = 0;
	MDBM	*shash = 0;
	serlist *state = 0;
	ser_t	*slist = 0;
	int	print = 0, different;
	char	sbuf[MAXLINE];
	char	*name = 0, *mode = "rb";
	int	tmpfile = 0;
	int	mcmprc;
	char	*fbuf;
	int	no_lf = 0;
	int	lf_pend = 0;
	u32	eflags = flags; /* copy because expandnleq destroys bits */
	int	error = 0, serial;

#define	RET(x)	{ different = x; goto out; }

	if (inex && (pf->mRev || pf->iLst || pf->xLst)) RET(2);

	if (S_ISLNK(s->mode)) RET(!streq(s->symlink, d->symlink));

	/* If the path changed, it is a diff */
	if (d->pathname) {
		char *r = _relativeName(s->gfile, 0, 1, 1, s->proj);
		if (r && !streq(d->pathname, r)) RET(1);
	}

	/*
	 * Cannot enforce this assert here, gfile may be ready only
	 * due to  GET_SKIPGET
	 * assert(WRITABLE(s));
	 */
	if (UUENCODE(s)) {
		tmpfile = 1;
		unless (bktmp(sbuf, "getU")) RET(-1);
		name = strdup(sbuf);
		if (deflate_gfile(s, name)) {
			unlink(name);
			free(name);
			RET(-1);
		}
	} else {
		mode = "rt";
		name = strdup(s->gfile);
	}
	if (BINPOOL(s)) {
		RET(bp_diff(s, d, name));
	} else if (HASH(s)) {
		int	flags = CSET(s) ? DB_KEYFORMAT : 0;

		ghash = loadDB(name, 0, flags|DB_USEFIRST);
		shash = mdbm_open(NULL, 0, 0, GOOD_PSIZE);
	}
	else unless (gfile = mopen(name, mode)) {
		verbose((stderr, "can't open %s\n", name));
		RET(-1);
	}
	assert(s->state & S_SOPEN);
	slist = serialmap(s, d, pf->iLst, pf->xLst, &error);
	assert(!error);
	state = allocstate(0, s->nextserial);
	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
	}
	while (fbuf = nextdata(s)) {
		if (isData(fbuf)) {
			if (fbuf[0] == CNTLA_ESCAPE) fbuf++;
			if (!print) {
				/* if we are skipping data from pending block */
				if (lf_pend &&
				    lf_pend == whatstate((const serlist*)state))
				{
					lf_pend = 0;
				}
				continue;
			}
			if (HASH(s)) {
				char	*from, *to, *val;
				/* XXX: hack to use sbuf, but it exists */
				val = CSET(s) ?
				    separator(fbuf) : strchr(fbuf, ' ');
				assert(val && (*val == ' '));
				for (from = fbuf, to = sbuf;
				    from != val;
				    *to++ = *from++) /* null body */;
				*to++ = '\0';
				from++;
				val = to;
				for ( ;
				    *from && *from != '\n';
				    *to++ = *from++) /* null body */;
				assert(*from == '\n');
				*to = '\0';
				/* XXX: could use the MDBM_INSERT fails */
				if (mdbm_fetch_str(shash, sbuf)) continue;
				if (mdbm_store_str(shash, sbuf, "1",
				    MDBM_INSERT))
				{
					fprintf(stderr, "sccs_hasDiffs: "
						"key insert caused conflict "
						"'%s'\n", sbuf);
					RET(-1);
				}
				unless (to = mdbm_fetch_str(ghash, sbuf)) {
#ifdef	WHEN_DELETE_KEY_SUPPORTED
					debug((stderr, "diff because sfile "
						"had key\n"));
					RET(1);
#else
					continue;
#endif
				}
				unless (streq(to, val)) {
					debug((stderr, "diff because same "
						"key diff value\n"));
					RET(1);
				}
				mdbm_delete_str(ghash, sbuf);
				continue;
			}
			mcmprc = mcmp(gfile, fbuf);
			if (mcmprc == MCMP_DIFF) {
				mcmprc = expandnleq(s, d, gfile, fbuf, &eflags);
			}
			no_lf = 0;
			lf_pend = print;
			switch (mcmprc) {
			    case MCMP_ERROR:
			    	fprintf(stderr, "sccs_hasDiffs: mcmp error\n");
				exit(1);
			    case MCMP_MATCH:
			    	break;
			    case MCMP_NOLF:
			    	no_lf = print;
				break;
			    case MCMP_DIFF:
				debug((stderr, "diff because diff data\n"));
				RET(1);
			    case MCMP_SFILE_EOF:
			    case MCMP_BOTH_EOF:
			    	fprintf(stderr, "sccs_hasDiffs: "
				    "sfile has data and is EOF?\n");
				RET(1);
			    case MCMP_GFILE_EOF:
				debug((stderr, "diff because EOF on gfile\n"));
				RET(1);
			    default:
			    	fprintf(stderr,
				    "sccs_hasDiffs: switch with no case %d\n",
				    mcmprc);
				exit(1);
			}
			debug2((stderr, "SAME %.*s", linelen(fbuf), fbuf));
			continue;
		}
		serial = atoi(&fbuf[3]);
		if (fbuf[1] == 'E' && lf_pend == serial &&
		    whatstate((const serlist*)state) == serial)
		{
			char	*n = &fbuf[3];
			while (isdigit(*n)) n++;
			if (*n == 'N') {
				no_lf = (no_lf) ? 0 : 1; /* toggle */
			}
			else if (no_lf) {
				/* sfile has newline when gfile had none */
				debug((stderr, "diff because EOF on gfile\n"));
				RET(1);
			}
			lf_pend = 0;
		}
		changestate(state, fbuf[1], serial);
		debug2((stderr, "%.*s\n", linelen(fbuf), fbuf));
		print = printstate((const serlist*)state, (const ser_t*)slist);
	}
	if (HASH(s)) {
		kvpair	kv;
		kv = mdbm_first(ghash);
		if (kv.key.dsize) {
			debug((stderr, "diff because gfile had key\n"));
			RET(1);
		}
		debug((stderr, "hashes same\n"));
		RET(0);
	}
	if (no_lf) {
		/* gfile has newline when sfile had none */
		debug((stderr, "diff because EOF on sfile\n"));
		RET(1);
	}
	mcmprc = mcmp(gfile, 0);
	if (mcmprc == MCMP_BOTH_EOF) {
		debug((stderr, "same\n"));
		RET(0);
	}
	assert(mcmprc == MCMP_SFILE_EOF);
	debug((stderr, "diff because EOF on sfile\n"));
	RET(1);
out:
	if (s->zin) {
		zgets_done(s->zin);
		s->zin = 0;
	}
	if (gfile) mclose(gfile); /* must close before we unlink */
	if (ghash) mdbm_close(ghash);
	if (shash) mdbm_close(shash);
	if (name) {
		if (tmpfile) unlink(name);
		free(name);
	}
	if (slist) free(slist);
	if (state) free(state);
	return (different);
}

int
sccs_hasDiffs(sccs *s, u32 flags, int inex)
{
	pfile	pf;
	int	ret;
	delta	*d;

	unless (HAS_GFILE(s) && HAS_PFILE(s)) return (0);

	bzero(&pf, sizeof(pf));
	if (sccs_read_pfile("hasDiffs", s, &pf)) return (-1);
	unless (d = findrev(s, pf.oldrev)) {
		verbose((stderr,
		    "diffs: can't find %s in %s\n", pf.oldrev, s->gfile));
		free_pfile(&pf);
		return (-1);
	}
	ret = _hasDiffs(s, d, flags, inex, &pf);
	if ((ret == 1) && MONOTONIC(s) && d->dangling && !s->tree->dangling) {
		while (d->next && (d->dangling || TAG(d))) d = d->next;
		assert(d->next);
		strcpy(pf.oldrev, d->rev);
		ret = _hasDiffs(s, d, flags, inex, &pf);
	}
	free_pfile(&pf);
	return (ret);
}

private inline int
hasComments(delta *d)
{
	int	i;

	EACH(d->comments) return (1);
	return (0);
}

/*
 * Apply the encoding to the gfile and leave it in tmpfile.
 */
private int
deflate_gfile(sccs *s, char *tmpfile)
{
	FILE	*in, *out;
	int	n;

	unless (out = fopen(tmpfile, "w")) return (-1);
	switch (s->encoding & E_DATAENC) {
	    case E_UUENCODE:
		in = fopen(s->gfile, "r");
		n = uuencode(in, out);
		fclose(in);
		fclose(out);
		break;
	    default:
		assert("Bad encoding" == 0);
	}
	return (0);
}


private int
isRegularFile(mode_t m)
{
	return ((m == 0) || S_ISREG(m));
}

/*
 * Check mode/symlink changes
 * Changes in permission are ignored
 * Returns:
 *	0 if no change
 *	1 if file type changed
 *	2 if meta mode field changed (e.g. symlink)
 * 	3 if path changed
 *
 */
private int
diff_gmode(sccs *s, pfile *pf)
{
	delta *d = findrev(s, pf->oldrev);

	/* If the path changed, it is a diff */
	if (d->pathname) {
		char *q, *r = _relativeName(s->sfile, 0, 1, 1, s->proj);

		if (r) {
			q = sccs2name(r);
			if (!streq(d->pathname, q)) {
				free(q);
				return (3);
			}
			free(q);
		}
	}

	if (S_ISLNK(s->mode)) {
		unless (streq(s->symlink, d->symlink)) {
			return (2);
		}
	}
	return (0); /* no  change */
}

/*
 * Load both files into MDBMs and then leave the difference in the tmpfile.
 * case 0:	no diffs
 * case 1:	diffs
 * case 2:	diff ran into problems
 */
int
diffMDBM(sccs *s, char *old, char *new, char *tmpfile)
{
	FILE	*f = fopen(tmpfile, "w");
	int	flags = CSET(s) ? DB_KEYFORMAT : 0;
	MDBM	*o = loadDB(old, 0, flags|DB_USEFIRST);
	MDBM	*n = loadDB(new, 0, flags|DB_USEFIRST);
	/* 'p' is not used as hash, but has simple storage */
	MDBM	*p = mdbm_open(NULL, 0, 0, GOOD_PSIZE);
	kvpair	kv;
	int	items = 0;
	int	ret;
	char	*val;

	unless (n && o && p && f) {
		ret = 2;
		unlink(tmpfile);
		goto out;
	}
	for (kv = mdbm_first(n); kv.key.dsize; kv = mdbm_next(n)) {
		if ((val = mdbm_fetch_str(o, kv.key.dptr)) &&
		    streq(val, kv.val.dptr)) {
		    	continue;
		}
		if (mdbm_store(p, kv.key, kv.val, MDBM_INSERT)) {
			ret = 2;
			fprintf(stderr, "diffMDBM: mdbm store failed\n");
			goto out;
		}
		items++;
	}
	if (items) {
		/* XXX NOT YET: fprintf(f, "I0 %u\n", items); */
		fprintf(f, "0a0\n");
		for (kv = mdbm_first(p); kv.key.dsize; kv = mdbm_next(p)) {
			fputs("> ", f);
			fputs(kv.key.dptr, f);
			fputc(' ', f);
			fputs(kv.val.dptr, f);
			fputc('\n', f);
		}
		ret = 1;
		if (s->mdbm) mdbm_close(s->mdbm);
		s->mdbm = o;
		o = 0;
	} else {
		ret = 0;
	}
out:	if (n) mdbm_close(n);
	if (o) mdbm_close(o);
	if (p) mdbm_close(p);
	if (f) fclose(f);
	return (ret);
}

/*
 * Returns:
 *	-1 for some already bitched about error
 *	0 if there were differences
 *	1 if no differences
 *
 * 	This function is called to determine the difference
 *	in the delta body. Note that, by definition, the delta body
 *	of non-regular file is empty.
 */
int
diff_gfile(sccs *s, pfile *pf, int expandKeyWord, char *tmpfile)
{
	char	old[MAXPATH];	/* the version from the s.file */
	char	new[MAXPATH];	/* the new file, usually s->gfile */
	int	ret, flags;
	delta *d;

	debug((stderr, "diff_gfile(%s, %s)\n", pf->oldrev, s->gfile));
	assert(s->state & S_GFILE);
	/*
	 * set up the "new" file
	 */
	if (isRegularFile(s->mode)) {
		if (UUENCODE(s)) {
			unless (bktmp(new, "getU")) return (-1);
			if (deflate_gfile(s, new)) {
				unlink(new);
				return (-1);
			}
		} else {
			strcpy(new, s->gfile);
		}
	} else { /* non regular file, e.g symlink */
		strcpy(new, DEVNULL_WR);
	}

	/*
	 * set up the "old" file
	 */
	d = findrev(s, pf->oldrev);
	unless (d) {
		fprintf(stderr,
		    "%s: Cannot find base revision \"%s\", bad p.file?\n",
		    s->sfile, pf->oldrev);
		return (-1);
	}
	flags =  GET_ASCII|SILENT|PRINT;
	if (expandKeyWord) flags |= GET_EXPAND;
	if (isRegularFile(d->mode) && !BINPOOL(s)) {
		unless (bktmp(old, "get")) return (-1);
		if (sccs_get(s, pf->oldrev, pf->mRev, pf->iLst, pf->xLst,
		    flags, old)) {
			unlink(old);
			return (-1);
		}
	} else {
		strcpy(old, DEVNULL_WR);
	}

	/*
	 * now we do the diff
	 */
	if (HASH(s)) {
		ret = diffMDBM(s, old, new, tmpfile);
	} else if (BINPOOL(s)) {
		ret = bp_diff(s, d, new);
	} else {
		ret = diff(old, new, DF_DIFF, tmpfile);
	}
	unless (streq(old, DEVNULL_WR)) unlink(old);
	if (!streq(new, s->gfile) && !streq(new, DEVNULL_WR)){
		unlink(new);		/* careful */
	}
	switch (ret) {
	    case 0:	/* no diffs */
		return (1);
	    case 1:	/* diffs */
		return (0);
	    case 2:	/* diff ran into problems */
		fprintf(stderr, "Arrrrg.  Diff errored\n");
		return (-1);
	    default:	/* unknown? */
		fprintf(stderr, "Unknown exit from diff.\n");
		return (-1);
	}
}

/*
 * Check mode changes & changes in delta body.
 * The real work is done in diff_gmode & diff_gfile.
 * Returns:
 *	-1 for some already bitched about error
 *	0 if there were differences
 *	1 if no differences
 *
 */
private int
diff_g(sccs *s, pfile *pf, char **tmpfile)
{
	static char	tmpname[MAXPATH];

	*tmpfile = DEVNULL_WR;
	switch (diff_gmode(s, pf)) {
	    case 0: 		/* no mode change */
		if (!isRegularFile(s->mode)) return 1;
		*tmpfile  = bktmp(tmpname, "diffg1");
		assert(*tmpfile);
		return (diff_gfile(s, pf, 0, *tmpfile));
	    case 2:		/* meta mode field changed */
		return 0;
	    case 3:		/* path changed */
	    case 1:		/* file type changed */
		*tmpfile  = bktmp(tmpname, "diffg2");
		assert(*tmpfile);
		if (diff_gfile(s, pf, 0, *tmpfile) == -1) return (-1);
		return 0;
	    default:
		return -1;
	}
}

private int
unlinkGfile(sccs *s)
{
	if (s->state & S_GFILE) {
		if (unlink(s->gfile)) return (-1);	/* Careful */
	}
	/*
	 * zero out all gfile related field
	 */
	if (s->symlink) free(s->symlink);
	s->symlink = 0;
	s->gtime = s->mode = 0;
	s->state &= ~S_GFILE;
	return (0);
}

private void
pdiffs(char *gfile, char *left, char *right, FILE *diffs)
{
	int	first = 1;
	char	buf[MAXLINE];

	while (fnext(buf, diffs)) {
		if (first) {
			printf("===== %s %s vs %s =====\n", gfile, left, right);
			first = 0;
		}
		fputs(buf, stdout);
	}
	fclose(diffs);
}

void
free_pfile(pfile *pf)
{
	if (pf->iLst) free(pf->iLst);
	if (pf->xLst) free(pf->xLst);
	if (pf->mRev) free(pf->mRev);
	if (pf->user) free(pf->user);
	bzero(pf, sizeof(*pf));
}

/*
 * clean / unedit the specified file.
 *
 * Return codes are passed out to exit() so don't error on warnings.
 * If listing, don't do any cleans.
 */
int
sccs_clean(sccs *s, u32 flags)
{
	pfile	pf;
	char	tmpfile[MAXPATH];
	delta	*d;

	/* don't go removing gfiles without s.files */
	unless (HAS_SFILE(s) && HASGRAPH(s)) {
		verbose((stderr, "%s not under SCCS control\n", s->gfile));
		return (0);
	}

	unless (HAS_PFILE(s)) {
		pfile	dummy = { "+", "?", "?", 0, "?", 0, 0, 0 };
		int	flags = SILENT|GET_EXPAND;

		if (isRegularFile(s->mode) && !WRITABLE(s)) {
			verbose((stderr, "Clean %s\n", s->gfile));
			unless (flags & CLEAN_CHECKONLY) unlinkGfile(s);
			return (0);
		}

		/*
		 * It's likely that they did a chmod +w on the file.
		 * Go look and see if there are any diffs and if not,
		 * clean it.
		 */
		unless (_hasDiffs(s, sccs_top(s), flags, 0, &dummy)) {
			verbose((stderr, "Clean %s\n", s->gfile));
			unless (flags & CLEAN_CHECKONLY) unlinkGfile(s);
			return (0);
		}
		fprintf(stderr,
		    "%s writable, with changes, but not edited.\n", s->gfile);
		unless (flags & PRINT) return (1);
		sccs_diffs(s, 0, 0, DIFF_HEADER|SILENT, DF_DIFF, stdout);
		return (1);
	}

	unless (HAS_GFILE(s)) {
		verbose((stderr, "%s not checked out\n", s->gfile));
		return (0);
	}

	if (sccs_read_pfile("clean", s, &pf)) return (1);
	if (pf.mRev || pf.iLst || pf.xLst) {
		fprintf(stderr,
		    "%s has merge|include|exclude, not cleaned.\n", s->gfile);
		free_pfile(&pf);
		return (1);
	}

	unless (d = findrev(s, pf.oldrev)) {
		free_pfile(&pf);
		return (1);
	}

	unless (sameFileType(s, d)) {
		unless (flags & PRINT) {
			fprintf(stderr,
			    "%s has different file types "
			    "which is unsupported.\n", s->gfile);
                } else {
			printf("===== %s (file type) %s vs edited =====\n",
			    s->gfile, pf.oldrev);
			printf("< %s\n-\n", mode2FileType(d->mode));
			printf("> %s\n", mode2FileType(s->mode));
 		}
		free_pfile(&pf);
		return (2);
	}

	if (BITKEEPER(s)) {
		char *t = relativeName(s, 1);

		unless (t) {
			fprintf(stderr,
			"%s: cannot compute relative path, no project root ?\n",
				s->gfile);
			free_pfile(&pf);
			return (1);
		}
		if (!(flags & CLEAN_SKIPPATH) && (!streq(t, d->pathname))) {
			unless (flags & PRINT) {
				verbose((stderr,
				   "%s has different pathnames: %s, needs delta.\n",
				    s->gfile, t));
			} else {
				printf(
				    "===== %s (pathnames) %s vs edited =====\n",
				    s->gfile, pf.oldrev);
				printf("< %s\n-\n", d->pathname);
				printf("> %s\n", t);
			}
			free_pfile(&pf);
			return (2);
		}
	} 

	if (S_ISLNK(s->mode)) {
		if (streq(s->symlink, d->symlink)) {
			verbose((stderr, "Clean %s\n", s->gfile));
			unless (flags & CLEAN_CHECKONLY) {
				unlink(s->pfile);
				unlinkGfile(s);
			}
			free_pfile(&pf);
			return (0);
		}
		unless (flags & CLEAN_SHUTUP) {
			unless (flags & PRINT) {
				fprintf(stderr,
			    "%s has been modified, needs delta.\n", s->gfile);
			} else {
				printf("===== %s %s vs %s =====\n",
				    s->gfile, pf.oldrev, "edited");
				printf("< SYMLINK -> %s\n-\n", d->symlink);
				printf("> SYMLINK -> %s\n", s->symlink);
			}
		}
		free_pfile(&pf);
		return (2);
	}

	unless (EDITED(s)) {
		if (ASCII(s)) flags |= GET_EXPAND;
	}
	unless (bktmp(tmpfile, "diffg")) return (1);
	/*
	 * hasDiffs() ignores keyword expansion differences.
	 * And it's faster.
	 */
	unless (sccs_hasDiffs(s, flags, 1)) goto nodiffs;
	switch (diff_gfile(s, &pf, 0, tmpfile)) {
	    case 1:		/* no diffs */
nodiffs:	verbose((stderr, "Clean %s\n", s->gfile));
		unless (flags & CLEAN_CHECKONLY) {
			unlink(s->pfile);
			unlinkGfile(s);
		}
		free_pfile(&pf);
		unlink(tmpfile);
		return (0);
	    case 0:		/* diffs */
		unless (flags & CLEAN_SHUTUP) {
			unless (flags & PRINT) {
				fprintf(stderr,
			    "%s has been modified, needs delta.\n", s->gfile);
			} else {
				pdiffs(s->gfile,
				    pf.oldrev, "edited", fopen(tmpfile, "r"));
		}
		}
		free_pfile(&pf);
		unlink(tmpfile);
		return (2);
	    default:		/* error */
		fprintf(stderr,
		    "couldn't compute diffs on %s, skipping", s->gfile);
		free_pfile(&pf);
		unlink(tmpfile);
		return (1);
	}
}

/*
 * unedit the specified file.
 *
 * Return codes are passed out to exit() so don't error on warnings.
 */
int
sccs_unedit(sccs *s, u32 flags)
{
	int	modified = 0;
	int	getFlags;
	int	currState = 0;

	/* don't go removing gfiles without s.files */
	unless (HAS_SFILE(s) && HASGRAPH(s)) {
		verbose((stderr, "%s not under SCCS control\n", s->gfile));
		return (0);
	}

	switch(proj_checkout(s->proj)) {
	    case CO_GET: getFlags = GET_EXPAND; break;
	    case CO_EDIT: getFlags = GET_EDIT; break;
	    default: getFlags = 0; break;
	}
	if (HAS_PFILE(s)) {
		if (!getFlags || sccs_hasDiffs(s, flags, 1)) modified = 1;
		currState = GET_EDIT;
	} else {
		if (WRITABLE(s)) {
			fprintf(stderr, 
			    "%s writable but not edited?\n", s->gfile);
			return (1);
		} else {
			verbose((stderr, "Clean %s\n", s->gfile));
			if (HAS_GFILE(s)) currState = GET_EXPAND;
		}
	}
	unlink(s->pfile);
	if (!modified && getFlags &&
	    (getFlags == currState ||
		(currState != 0 && !(SCCS(s) || RCS(s))))) {
		getFlags |= GET_SKIPGET;
	} else {
		unlinkGfile(s);
	}
	if (getFlags) {
		if (sccs_get(s, 0, 0, 0, 0, SILENT|getFlags, "-")) {
			return (1);
		}
		s = sccs_restart(s);
		fix_gmode(s, getFlags);
	}
	return (0);
}


private void
_print_pfile(sccs *s)
{
	FILE	*f;
	char	buf[MAXPATH];

	sprintf(buf, "%s:", s->gfile);
	printf("%-23s ", buf);
	f = fopen(s->pfile, "r");
	if (fgets(buf, sizeof(buf), f)) {
		char	*s;
		for (s = buf; *s && *s != '\n'; ++s);
		*s = 0;
		printf(buf);
	} else {
		printf("(can't read pfile)\n");
	}
	fclose(f);
}

private void
sccs_infoMsg(sccs *s, char msgCode, u32 flags)
{
	char *gfile = s->gfile;

	if (flags & SINFO_TERSE) {
		printf("%s|%c\n", gfile, msgCode);
		return;
	}
	switch (msgCode) {
	    case 'x':	verbose((stderr, "%s not under SCCS control\n", gfile));
			break;
	    case 'u':	break; /* no long message */
	    case 'p':	/* locked, but has no g file */
			verbose((stderr, "%s not checked out\n", gfile));
			break;
	    case 'm':	_print_pfile(s);
			printf(" (has merge pointer, needs delta)\n");
			break;
	    case 'c':	_print_pfile(s);
			printf(" (modified, needs delta)\n");
			break;
	    case '?':	_print_pfile(s);
			fprintf(stderr,
		"couldn't compute diffs on %s, skipping\n", gfile);
			break;
	    case 'l':	_print_pfile(s);
			printf("\n");
			break;
	}
}

/*
 * provide information about the editing status of a file.
 * XXX - does way too much work for this, shouldn't sccs init.
 *
 * Return codes are passed out to exit() so don't error on warnings.
 */
int
sccs_info(sccs *s, u32 flags)
{
	unless (HAS_SFILE(s)) {
		sccs_infoMsg(s, 'x', flags);
		return (0);
	}
	GOODSCCS(s);
	if (!HAS_PFILE(s)) {
		sccs_infoMsg(s, 'u', flags);
		return (0);
	}
	unless (HAS_GFILE(s)) {
		sccs_infoMsg(s, 'p', flags);
		return (0);
	}

	switch (sccs_hasDiffs(s, flags, 1)) {
	    case 2:
		sccs_infoMsg(s, 'm', flags);
		return (1);
	    case 1:
		sccs_infoMsg(s, 'c', flags);
		return (1);
	    case -1:
		sccs_infoMsg(s, '?', flags);
		return (1);
	    default:
		sccs_infoMsg(s, 'l', flags);
	}
	return (0);
}

/*
 * Work backwards and count all the lines for this delta.
 */
private int
count_lines(delta *d)
{
	if (!d) return (0);
	return (count_lines(d->parent) + d->added - d->deleted);
}

/*
 * Look for files containing binary data that BitKeeper cannot handle,
 * when in text mode.
 * Right now only NUL is unsupported
 * "\n\001" used to cause problem, this has been fixed.
 *
 * XXX Performance warning: This is slow if we get a very large text file
 */
int
ascii(char *file)
{
	MMAP	*m = mopen(file, "b");
	u8	*p, *end;

	if (!m) return (2);
	for (p = (u8*)m->where, end = (u8*)m->end; p < end; ) {
		unless (*p++) { /* null is not allowed */
			mclose(m);
			return (0);
		}
	}
	mclose(m);
	return (1);
}

/*
 * Return the encoding for a binary file.
 */
int
sccs_binaryenc(sccs *s)
{
	if (proj_configbool(s ? s->proj : 0, "binpool")) {
		return (E_BINPOOL);
	} else {
		return (E_UUENCODE);
	}
}

/*
 * Open the input for a checkin/delta.
 * The set of options we have are:
 *	{empty, stdin, file} | {cat, gzip|uuencode}
 */
private int
openInput(sccs *s, int flags, FILE **inp)
{
	char	*file = (flags&DELTA_EMPTY) ? DEVNULL_WR : s->gfile;
	char	*mode = "rb";	/* default mode is binary mode */
	int 	compress, enc;

	unless (flags & DELTA_EMPTY) {
		unless (HAS_GFILE(s)) {
			*inp = NULL;
			return (-1);
		}
	}
	compress = s->encoding & E_GZIP;
	enc = s->encoding & E_DATAENC;
	/* handle auto promoting ascii to binary if needed */
	if ((enc == E_ASCII) && !streq("-", file) && !ascii(file)) {
		enc = sccs_binaryenc(s);
		s->encoding = enc | compress;
		/* binpool doesn't support gzip */
		if (BINPOOL(s)) s->encoding &= ~E_GZIP;
	}
	switch (enc) {
	    case E_ASCII:
		mode = "rt";
		/* fall through, check if we are really ascii */
	    case E_UUENCODE:
		if (streq("-", file)) {
			*inp = stdin;
		} else {
			*inp = fopen(file, mode);
		}
		break;
	    case E_BINPOOL:
		*inp = 0;
		break;
	    default:
		assert(0);
		break;
	}
	return (0);
}

/*
 * Do most of the initialization on a delta.
 */
delta *
sccs_dInit(delta *d, char type, sccs *s, int nodefault)
{
	char	*t;

	unless (d) d = calloc(1, sizeof(*d));
	d->type = type;
	assert(s);
	if (BITKEEPER(s) && (type == 'D')) d->flags |= D_CKSUM;
	unless (d->sdate) {
		if (t = getenv("BK_DATE_TIME_ZONE")) {
			dateArg(d, t, 1);
			assert(!(d->flags & D_ERROR));
		} else if (s->gtime && (s->initFlags & INIT_FIXDTIME)) {
			time_t	now = time(0), use = s->gtime, i;

			if (use > now) {
				fprintf(stderr, "BK limiting the delta time "
				    "to the present time.\n"
				    "Timestamp on \"%s\" is in the future\n",
				    s->gfile);
				use = now;
			}
			date(d, use);

			/*
			 * If gtime is from the past, fudge the date
			 * to current, so the unique() code don't cut us off
			 * too early. This is important for getting unique
			 * root key.
			 */
			if ((i = (now - use)) > 0) {
				d->dateFudge = i;
				d->date += d->dateFudge;
			}
		} else {
			date(d, time(0));
		}
	}
	if (nodefault) {
		unless (d->user) d->user = strdup("Anon");
	} else {
		unless (d->user) d->user = strdup(sccs_user());
		unless (d->hostname && sccs_gethost()) {
			char	*imp, *h;

			if (imp = getenv("BK_IMPORTER")) {
				h = aprintf("%s[%s]", sccs_gethost(), imp);
				hostArg(d, h);
				free(h);
			} else {
				hostArg(d, sccs_host());
			}
		}
		unless (d->pathname && s) {
			char *p, *q;

			/*
			 * Get the relativename of the sfile, _not_ the gfile,
			 * because we cannot trust the gfile name on
			 * win32 case-folding file system.
			 */
			p = _relativeName(s->sfile, 0, 0, 1, s->proj);
			q = sccs2name(p);
			pathArg(d, q);
			free(q);
		}
#ifdef	AUTO_MODE
		assert("no" == 0);
		unless (d->flags & D_MODE) {
			if (s->state & GFILE) {
				d->mode = s->mode;
				d->symlink = s->symlink;
				s->symlink = 0;
				d->flags |= D_MODE;
				assert(!(d->flags & D_DUPLINK));
			} else {
				modeArg(d, "0664");
			}
		}
#endif
	}
	return (d);
}

/*
 * This poorly named function is trying to decide if the files are the
 * same type, and if they are symlinks, are they the same value.
 */
private int
needsMode(sccs *s, delta *p)
{
	unless (p) return (1);
	unless (s->symlink) return (0);
	return (!streq(s->symlink, p->symlink));
}

/*
 * Update the mode field and the symlink field.
 */
private void
updMode(sccs *s, delta *d, delta *dParent)
{
	if ((s->state & S_GFILE) &&
	    !(d->flags & D_MODE) && needsMode(s, dParent)) {
		assert(d->mode == 0);
		d->mode = s->mode;
		if (s->symlink) {
			d->symlink = strdup(s->symlink);
			assert(!(d->flags & D_DUPLINK));
		}
		d->flags |= D_MODE;
	}
}

private void
updatePending(sccs *s)
{
	if (CSET(s)) return;
	touch(sccsXfile(s, 'd'),  GROUP_MODE);
}

/* s/\r+\n$/\n/ */
private void
fix_crnl(register char *s)
{
	char	*p = s;
	while (*p) p++;
	unless (p - s >= 2) return;
	unless (p[-2] == '\r' && p[-1] == '\n') return;
	for (p -= 2; p != s; p--) {
		unless (p[-1] == '\r') break;
	}
	p[0] = '\n';
	p[1] = 0;
}

/*
 * Escape the control-A character in data buffer
 */
private void
fix_cntl_a(sccs *s, char *buf, FILE *out)
{
	char	cntlA_escape[2] = { CNTLA_ESCAPE, 0 };

	if (buf[0] == '\001') fputdata(s, cntlA_escape, out);
}

/*
 * Read the output of diff and determine if the files 
 * are binary.
 */
private int
binaryCheck(MMAP *m)
{
	u8	*p, *end;

	p = m->mmap;
	end = m->end;
	
	/* GNU diff reports binary files */
	if (p + 13 < end && strneq("Binary files ", p, 13)) return (1);
	
	/* no nulls */
	while (p < end) if (*p++ == 0) return (1);
	return (0);
}

/*
 * Check in initial sfile.
 *
 * XXX - need to make sure that they do not check in binary files in
 * gzipped format - we can't handle that yet.
 */
/* ARGSUSED */
private int
checkin(sccs *s,
	int flags, delta *prefilled, int nodefault, MMAP *diffs, char **syms)
{
	FILE	*sfile = 0, *gfile = 0;
	delta	*n0 = 0, *n, *first;
	int	added = 0;
	int	len;
	int	i;
	char	*t;
	char	buf[MAXLINE];
	admin	l[2];
	int	no_lf = 0;
	int	error = 0;
	int	short_key = 0;
	MDBM	*db;
	static	int fixDate = -1;

	if (fixDate == -1) fixDate = (getenv("_BK_NO_UNIQUE") == 0);

	assert(s);
	debug((stderr, "checkin %s %x\n", s->gfile, flags));
	unless (flags & NEWFILE) {
		verbose((stderr,
		    "%s not checked in, use -i flag.\n", s->gfile));
out:		sccs_unlock(s, 'z');
		sccs_unlock(s, 'x');
		if (prefilled) sccs_freetree(prefilled);
		if (sfile) fclose(sfile);
		if (gfile && (gfile != stdin)) fclose(gfile);
		s->state |= S_WARNED;
		return (-1);
	}
	if (diffs) {
		if (binaryCheck(diffs)) {
			fprintf(stderr,
			    "%s has nulls (\\0) in diffs, checkin aborted.\n",
			    s->gfile);
			goto out;
		}
	} else if (isRegularFile(s->mode)) {
		openInput(s, flags, &gfile);
		unless (gfile || BINPOOL(s)) {
			perror(s->gfile);
			goto out;
		}
	} else if (S_ISLNK(s->mode) && BINARY(s)) {
		fprintf(stderr, "%s: symlinks should not use BINARY mode!\n",
		    s->gfile);
		goto out;
	}

	if (HASGRAPH(s)) {
		fprintf(stderr, "delta: %s already exists\n", s->sfile);
		goto out;
	}
	/* This should never happen - the zlock should protect */
	if (exists(s->sfile)) {
		fprintf(stderr, "delta: lost checkin race on %s\n", s->sfile);
		goto out;
	}

	/* pathname, we need this below */
	buf[0] = 0;
	t = relativeName(s, 0);
	assert(t);
	strcpy(buf, t);

	/*
	 * Disallow BK_FS character in file name.
	 * Some day we may allow caller to escape the BK_FS character
	 */
	if (strchr(t, BK_FS)) {
		fprintf(stderr,
			"delta: %s: filename must not contain \"%c\"\n",
			t, BK_FS);
		goto out;
	}

	/*
	 * Disallow BKSKIP
	 */
	t = basenm(s->sfile);
	if (streq(&t[2], BKSKIP)) {
		fprintf(stderr,
			"delta: checking in %s is not allowed\n", BKSKIP);
		goto out;
	}

	sfile = fopen(sccsXfile(s, 'x'), "wb");
	unless (s) {
		perror(sccsXfile(s, 'x'));
		goto out;
	}
	if ((flags & DELTA_PATCH) || proj_root(s->proj)) {
		s->bitkeeper = 1;
		s->xflags |= X_BITKEEPER;
	}
	/*
	 * Do a 1.0 delta unless
	 * a) there is a init file (nodefault), or
	 * b) prefilled->rev is initialized, or
	 * c) the DELTA_EMPTY flag is set
	 */
	if (nodefault ||
	    (flags & DELTA_EMPTY) || (prefilled && prefilled->rev)) {
		first = n = prefilled ? prefilled : calloc(1, sizeof(*n));
	} else {
		first = n0 = calloc(1, sizeof(*n0));
		/*
		 * We don't do modes here.  The modes should be part of the
		 * per LOD state, so each new LOD starting from 1.0 should
		 * have new modes.
		 *
		 * We do do flags here, the initial flags are per file.
		 * XXX - is this the right answer?
		 */
		n0->rev = strdup("1.0");
		explode_rev(n0);
		n0->serial = s->nextserial++;
		n0->next = 0;
		s->table = n0;
		if (buf[0]) pathArg(n0, buf); /* pathname */

		n0 = sccs_dInit(n0, 'D', s, nodefault);
		n0->flags |= D_CKSUM;
		n0->sum = almostUnique();
		dinsert(s, n0, fixDate && !(flags & DELTA_PATCH));

		n = prefilled ? prefilled : calloc(1, sizeof(*n));
		n->pserial = n0->serial;
		n->next = n0;
	}
	assert(n);
	if (!nodefault && buf[0]) pathArg(n, buf); /* pathname */
	n = sccs_dInit(n, 'D', s, nodefault);

	/*
	 * We want the 1.0 and 1.1 keys to naturally not collide so we
	 * we need different dates.
	 * It would probably be more correct to not fudge the 1.1 delta
	 * forward but instead fudge the 1.0 1 second backwards but that
	 * is a royal pain the butt to do in all cases.  So here we are.
	 */
	if (n0) {
		n->date++;
		n->dateFudge++;
	}

	if (s->mode & 0111) s->mode |= 0110;	/* force user/group execute */
	s->mode |= 0220;			/* force user/group write */
	s->mode |= 0440;			/* force user/group read */

	updMode(s, n, 0);
	if (!n->rev) n->rev = n0 ? strdup("1.1") : strdup("1.0");
	explode_rev(n);
	if (nodefault) {
		if (prefilled) s->xflags |= prefilled->xflags;
	} else if (ASCII(s)) {
		unless (CSET(s)) {
			/* check eoln preference */
			s->xflags |= X_DEFAULT;
			if (s->proj) {
				db = proj_config(s->proj);
				if (db) {
					char *p = mdbm_fetch_str(db, "eoln");
					if (p && streq("unix", p)) {
						s->xflags &= ~X_EOLN_NATIVE;
						s->xflags &= ~X_EOLN_WINDOWS;
					}
					if (p && streq("windows", p)) {
						s->xflags &= ~X_EOLN_NATIVE;
						s->xflags |= X_EOLN_WINDOWS;
					}

					if (p = mdbm_fetch_str(db, "keyword")) {
						if (strstr(p, "sccs")) {
							s->xflags |= X_SCCS;
						}
						if (strstr(p, "rcs")) {
							s->xflags |= X_RCS;
						}
						if (strstr(p, "expand1")) {
							s->xflags |= X_EXPAND1;
						}
					}
				}
			}
		}
	}
	n->serial = s->nextserial++;
	s->table = n;
	if (n->flags & D_BADFORM) {
		fprintf(stderr, "checkin: bad revision: %s for %s\n",
		    n->rev, s->sfile);
		goto out;
	} else {
		l[0].flags = 0;
	}
	dinsert(s, n, fixDate && !(flags & DELTA_PATCH));
	s->numdeltas++;
	EACH (syms) {
		addsym(s, n, n, !(flags & DELTA_PATCH), n->rev, syms[i]);
	}
	/* need random set before the call to sccs_sdelta */
	/* XXX: changes n, so must be after syms stuff */
	unless (nodefault || (flags & DELTA_PATCH)) {
		delta	*d = n0 ? n0 : n;

		if (!d->random && !short_key) {
			randomBits(buf);
			assert(buf[0]);
			if (BINPOOL(s)) {
				d->random = malloc(strlen(buf) + 3);
				sprintf(d->random, "B:%s", buf);
			} else {
				d->random = strdup(buf);
			}
		}
		unless (hasComments(n)) {
			if (comments_readcfile(s, 0, n)) {
				if (flags & DELTA_CFILE) {
					fprintf(stderr,
					    "checkin: no comments for %s\n",
					    s->sfile);
					goto out;
				}
			}
		}
		unless (hasComments(n)) {
			sprintf(buf, "BitKeeper file %s", fullname(s->gfile));
			n->comments = addLine(d->comments, strdup(buf));
		}
	}
	if (BITKEEPER(s)) {
		s->version = SCCS_VERSION;
		if (flags & DELTA_HASH) s->xflags |= X_HASH;
		unless (flags & DELTA_PATCH) {
			first->flags |= D_XFLAGS;
			first->xflags = s->xflags;
		}
		if (CSET(s)) {
			unless (first->csetFile) {
				first->sum = almostUnique();
				first->flags |= D_ICKSUM;
				sccs_sdelta(s, first, buf);
				first->csetFile = strdup(buf);
			}
			first->flags |= D_CKSUM;
		} else {
			unless (first->csetFile) {
				char	*c = proj_rootkey(s->proj);

				if (c) first->csetFile = strdup(c);
			}
		}
	}
	if (BINPOOL(s)) {
		assert(n == s->table);
		if (!(flags & DELTA_PATCH) && bp_delta(s, n)) {
			fprintf(stderr,
			    "binpool delta of %s failed\n", s->gfile);
			goto out;
		}
		added = s->added = n->added;
	}
	if (delta_table(s, sfile, 1)) {
		error++;
		goto out;
	}
	if (BINPOOL(s)) goto skip_weave;
	buf[0] = 0;
	if (s->encoding & E_GZIP) sccs_zputs_init(s, sfile);
	if (n0) {
		fputdata(s, "\001I 2\n", sfile);
	} else {
		fputdata(s, "\001I 1\n", sfile);
	}
	s->dsum = 0;
	if (!(flags & DELTA_PATCH) && BINARY(s)) {
		/* XXX - this is incorrect, it needs to do it depending on
		 * what the encoding is.
		 */
		added = uuencode_sum(s, gfile, sfile);
	} else {
		if (diffs) {
			int	off = 0;
			char	*t;

			if ((t = mnext(diffs)) && isdigit(t[0])) off = 2;
			while (t = mnext(diffs)) {
				if ((off == 0) && (t[0] == '\\')) {
					++t;
				} else {
					t = &t[off];
				}
				s->dsum += fputdata(s, t, sfile);
				added++;
			}
			mclose(diffs);
		} else if (gfile) {
			while (fnext(buf, gfile)) {
				fix_crnl(buf);
				fix_cntl_a(s, buf, sfile);
				s->dsum += fputdata(s, buf, sfile);
				added++;
			}
			/*
			 * For ascii files, add missing \n automagically.
			 */
			len = strlen(buf);
			if (len && (buf[len - 1] != '\n')) {
				/* put lf in sfile, but not in dsum */
				fputdata(s, "\n", sfile);
				no_lf = 1;
			}
		} else if (S_ISLNK(s->mode)) {
			u8	*t;

			for (t = s->symlink; t && *t; s->dsum += *t++);
		}
	}
	if (n0) {
		fputdata(s, "\001E 2", sfile);
		if (no_lf) fputdata(s, "N", sfile);
		fputdata(s, "\n", sfile);
		fputdata(s, "\001I 1\n", sfile);
		fputdata(s, "\001E 1\n", sfile);
	} else {
		fputdata(s, "\001E 1", sfile);
		if (no_lf) fputdata(s, "N", sfile);
		fputdata(s, "\n", sfile);
	}
skip_weave:
	error = end(s, n, sfile, flags, added, 0, 0);
	if (gfile && (gfile != stdin)) {
		fclose(gfile);
		gfile = 0;
	}
	if (error) {
		fprintf(stderr, "checkin: cannot construct sfile\n");
		goto out;
	}

	t = sccsXfile(s, 'x');
	if (fclose(sfile)) {
		fprintf(stderr, "checkin: i/o error\n");
		perror(t);
		sfile = 0;
		goto out;
	}
	sfile = 0;
	if (rename(t, s->sfile)) {
		fprintf(stderr,
			 "checkin: can't rename(%s, %s) left in %s\n",
			t, s->sfile, t);
		goto out;
	}
	assert(size(s->sfile) > 0);
	unless (flags & DELTA_SAVEGFILE) unlinkGfile(s);	/* Careful */
	/* Always set the time on the s.file behind the g.file or now */
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	if (BITKEEPER(s)) updatePending(s);
	sccs_unlock(s, 'z');
	return (0);
}

/*
 * Figure out if anything has been used more than once.
 */
private int
checkdups(sccs *s)
{
	MDBM	*db;
	delta	*d;
	datum	k, v;
	char	c = 0;

	db = mdbm_open(NULL, 0, 0, 0);
	v.dsize = sizeof(ser_t);
	for (d = s->table; d; d = d->next) {
		if (d->type == 'R') continue;
		k.dptr = (void*)d->r;
		k.dsize = sizeof(d->r);
		v.dptr = (void*)&d->serial;
		if (mdbm_store(db, k, v, MDBM_INSERT)) {
			v = mdbm_fetch(db, k);
			fprintf(stderr, "%s: %s in use by serial %d and %d.\n",
			    s->sfile, d->rev, d->serial, *(ser_t*)v.dptr);
			c = 1;
		}
	}
	mdbm_close(db);
	return (c);
}

private inline int
isleaf(register sccs *s, register delta *d)
{
	if (d->type != 'D') return (0);
	/*
	 * June 2002: ignore lod stuff, we're removing that feature.
	 * We'll later add back the support for 2.x, 3.x, style numbering
	 * and then we'll need to remove this.
	 */
	assert(d->r[0] == 1);

	if (d->flags & D_MERGED) {
		delta	*t;

		unless (s->hasgone) return (0);

		for (t = s->table; t && t != d; t = t->next) {
			if ((t->merge == d->serial) && !(t->flags & D_GONE)) {
				return (0);
			}
		}
	}

	for (d = d->kid; d; d = d->siblings) {
		if (d->flags & D_GONE) continue;
		if (d->type != 'D') continue;
		return (0);
	}
	return (1);
}

int
sccs_isleaf(sccs *s, delta *d)
{
	return (isleaf(s, d));
}

/*
 * Check open branch
 * XXX: Assumes D_RED is clear ; exits with D_RED clear
 */
private int
checkOpenBranch(sccs *s, int flags)
{
	delta	*d, *m, *tip = 0, *symtip = 0;
	int	ret = 0, tips = 0, symtips = 0;

	for (d = s->table; d; (d->flags &= ~D_RED), d = d->next) {
		/*
		 * This order is important:
		 * Skip 1.0,
		 * check for bad R delta even if it is marked gone so we warn,
		 * then skip the rest if they are GONE.
		 */
		if (streq(d->rev, "1.0")) continue;
		if (CSET(s)) {
			if (!d->added && !d->deleted && !d->same &&
			    !(d->flags & D_SYMBOLS) && !d->symGraph) {
				verbose((stderr,
				    "%s: illegal removed delta %s\n",
				    s->sfile, d->rev));
				ret = 1;
			}
			if (d->symLeaf && !(d->flags & D_GONE)) {
				if (symtips) {
					if (symtips == 1) {
					    verbose((stderr,
			    			"%s: unmerged symleaf %s\n",
						s->sfile, symtip->rev));
					}
					verbose((stderr,
			    		    "%s: unmerged symleaf %s\n",
					    s->sfile, d->rev));
					ret = 1;
				}
				symtip = d;
				symtips++;
			}
		}
		if ((d->flags & D_GONE) || (d->type == 'R')) continue;
		unless (d->flags & D_RED) {
			if (tips) {
				if (tips == 1) {
				    verbose((stderr,
		    			"%s: unmerged leaf %s\n",
					s->sfile, tip->rev));
				}
				verbose((stderr,
		    		    "%s: unmerged leaf %s\n",
				    s->sfile, d->rev));
				ret = 1;
			}
			tip = d;
			tips++;
		}
		if (d->parent) d->parent->flags |= D_RED;
		if (d->merge) {
			m = sfind(s, d->merge);
			assert(m);
			m->flags |= D_RED;
		}
	}
	return (ret);
}


/*
 * Check all the BitKeeper specific stuff such as
 *	. no open branches
 *	. checksums on all deltas
 *	. xflags history
 *	. tag structure
 */
private int
checkInvariants(sccs *s, int flags)
{
	int	error = 0;
	int	xf = (flags & SILENT) ? XF_STATUS : XF_DRYRUN;
	delta	*d;

	error |= checkOpenBranch(s, flags);
	error |= checkTags(s, flags);
	for (d = s->table; d; d = d->next) {
		if ((d->type == 'D') && !(d->flags & D_CKSUM)) {
			verbose((stderr,
			    "%s|%s: no checksum\n", s->gfile, d->rev));
		}
		if (d->xflags && checkXflags(s, d, xf)) {
			extern	int xflags_failed;

			xflags_failed = 1;
			error |= 1;
		}
		if (d->mtag && !sfind(s, d->mtag)) {
			verbose((stderr,
			    "%s|%s: tag merge %u does not exist\n",
			    s->gfile, d->rev, d->mtag));
			error |= 1;
		}
		if (d->ptag && !sfind(s, d->ptag)) {
			verbose((stderr,
			    "%s|%s: tag parent %u does not exist\n",
			    s->gfile, d->rev, d->ptag));
			error |= 1;
		}
	}
	return (error);
}

/*
 * Given a graph with some deltas marked as gone (D_SET|D_GONE),
 * make sure that things will be OK with those deltas gone.
 * This means for each delta that is not gone:
 *	. make sure its parents (d->parent and d->merge) are not gone
 *	. make sure each delta it includes/excludes is not gone
 *
 * Since we are single rooted, then means that getting rid of all
 * the gone will leave us with a consistent tree.
 */
private int
checkGone(sccs *s, int bit, char *who)
{
	ser_t	*slist = setmap(s, bit, 1);
	delta	*d;
	int	i, error = 0;

	for (d = s->table; d; d = d->next) {
		if (d->flags & bit) continue;
		if (d->parent && (d->parent->flags & bit)) {
			error++;
			fprintf(stderr,
			"%s: revision %s not at tip of branch in %s.\n",
			    who, d->parent->rev, s->sfile);
			s->state |= S_WARNED;
		}
		if (d->merge && slist[d->merge]) {
			error++;
			fprintf(stderr,
			"%s: revision %s not at tip of branch in %s.\n",
			    who, sfind(s, d->merge)->rev, s->sfile);
			s->state |= S_WARNED;
		}
		if (d->symGraph && (d->ptag && slist[d->ptag])) {
			error++;
			fprintf(stderr,
			"%s: revision %s not at tip of tag graph in %s.\n",
			    who, sfind(s, d->ptag)->rev, s->sfile);
			s->state |= S_WARNED;
		}
		if (d->symGraph && (d->mtag && slist[d->mtag])) {
			error++;
			fprintf(stderr,
			"%s: revision %s not at tip of tag graph in %s.\n",
			    who, sfind(s, d->mtag)->rev, s->sfile);
			s->state |= S_WARNED;
		}
		EACH(d->include) {
			if (slist[d->include[i]]) {
				fprintf(stderr,
				    "%s: %s:%s includes %s\n", s->sfile,
				    who, d->rev, sfind(s, d->include[i])->rev);
				error++;
				s->state |= S_WARNED;
			}
		}
		EACH (d->exclude) {
			if (slist[d->exclude[i]]) {
				fprintf(stderr,
				    "%s: %s:%s excludes %s\n", s->sfile,
				    who, d->rev, sfind(s, d->exclude[i])->rev);
				error++;
				s->state |= S_WARNED;
			}
		}
	}
	free(slist);
	return (error);
}

private int
checkMisc(sccs *s, int flags)
{
	unless (s->version == SCCS_VERSION) {
	    verbose((stderr, "warning: %s version=%u, current version=%u\n",
		s->gfile, s->version, SCCS_VERSION));
	}
	return (0);
}

private int
checkrevs(sccs *s, int flags)
{
	delta	*d;
	int	e;

	for (e = 0, d = s->table; d; d = d->next) {
		e |= checkRev(s, s->sfile, d, flags);
	}
	return (e);
}

private int
checkRev(sccs *s, char *file, delta *d, int flags)
{
	int	i, error = 0;
	int	badparent;
	delta	*e, *p;

	if ((d->type == 'R') || (d->flags & D_GONE)) return (0);

	if (d->flags & D_BADFORM) {
		fprintf(stderr, "%s: bad rev '%s'\n", file, d->rev);
	}

	/*
	 * Make sure that the revision is well formed.
	 * The random part says that we allow x.y.z.0 if has random bits;
	 * that is for grafted trees.
	 */
	if (!d->r[0] || (!d->r[2] && !d->r[1] && (d->r[0] != 1)) ||
	    (d->r[2] && (!d->r[3] && !d->random)))
	{
		unless (flags & ADMIN_SHUTUP) {
			fprintf(stderr, "%s: bad revision %s (parent = %s)\n",
			    file, d->rev, d->parent?d->parent->rev:"Root");
		}
		error = 1;
	}
	/*
	 * XXX - this should check for BitKeeper files.  If it is not
	 * BitKeeper and the form is 1.0, that is an error.
	 */

	/*
	 * Make sure there is no garbage in the serial list[s].
	 */
	EACH (d->include) {
		if (d->include[i] < d->serial) continue;
		unless (flags & ADMIN_SHUTUP) {
			fprintf(stderr, "%s: %s has bad include serial %d\n",
			    file, d->rev, d->include[i]);
		}
		error = 1;
	}
	EACH (d->exclude) {
		if (d->exclude[i] < d->serial) continue;
		unless (flags & ADMIN_SHUTUP) {
			fprintf(stderr, "%s: %s has bad exclude serial %d\n",
			    file, d->rev, d->exclude[i]);
		}
		error = 1;
	}

	/*
	 * make sure that the parent points at us.
	 */
	if (p = d->parent) {
		for (e = p->kid; e; e = e->siblings) {
			if (e == d) break;
		}
		if (!e) {
			unless (flags & ADMIN_SHUTUP) {
				fprintf(stderr,
				    "%s: parent %s does not point to %s?!?!\n",
				    file, p->rev, d->rev);
			}
			error = 1;
		}
	} else {
		/* no parent */
		if (BITKEEPER(s) &&
		    !(streq(d->rev, "1.0") || streq(d->rev, "1.1"))) {
			unless (flags & ADMIN_SHUTUP) {
				fprintf(stderr,
				    "%s: rev %s not connected to trunk\n",
				    file, d->rev);
			}
			error = 1;
		}
		goto done;
	}

	badparent = 0;
	/*
	 * Two checks here.
	 * If they are on the same branch, is the sequence numbering
	 * correct?  Handle 1.9 -> 2.1 properly.
	 */
	if (d->r[2]) {
		/* If a x.y.z.q release, then it's trunk node should be x.y, */
		if ((p->r[0] != d->r[0]) || (p->r[1] != d->r[1])) {
			badparent = 1;
		}
		if (d->r[3] == 0) badparent = 1;
		if (p->r[2]) {
			if ((d->r[3] > 1) && (d->r[3] != p->r[3]+1)) {
				badparent = 1;
			}
#ifdef	CRAZY_WOW
			// XXX - this should be an option to admin.

			/* if there is a parent, and the parent is a
			 * x.y.z.q, and this is an only child, then
			 * insist that the revs are on the same
			 * branch.
			 */
			if (onlyChild(d) && !samebranch(d, d->parent)) {
				badparent = 1;
			}
#endif
		} else {
			if (d->r[3] != 1) badparent = 1;
		}
	} else {
		if (d->r[0] == p->r[0]) {
			if (d->r[1] != p->r[1]+1) badparent = 1;
		} else {
			/* LOD case needed for _fix_lod1 */
			if (d->r[1] != 1) badparent = 1;
		}
	}
	if (badparent) {
		unless (flags & ADMIN_SHUTUP) {
			fprintf(stderr,
			    "%s: rev %s has incorrect parent %s\n",
			    file, d->rev, p->rev);
		}
		error = 1;
	}

	/* If there is a parent, make sure the dates increase. */
	if (d->date < p->date) {
		if (flags & ADMIN_TIME) {
			fprintf(stderr,
			    "%s: time goes backwards between %s and %s\n",
			    file, d->rev, d->parent->rev);
			fprintf(stderr, "\t%s: %s    %s: %s -> %d seconds\n",
			    d->rev, d->sdate, d->parent->rev, d->parent->sdate,
			    (int)(d->date - d->parent->date));
			error |= 2;
		}
	}
	/* If the dates are identical, check that the keys are sorted */
	if (BITKEEPER(s) && d->parent && (d->date == d->parent->date)) {
		char	me[MAXPATH], parent[MAXPATH];

		sccs_sdelta(s, d, me);
		sccs_sdelta(s, d->parent, parent);
		unless (strcmp(parent, me) < 0) {
			fprintf(stderr,
			    "\t%s: %s,%s have same date and bad key order\n",
			    s->sfile, d->rev, d->parent->rev);
			error |= 2;
		}
	}

	/* Make sure the table order is sorted */
	if (BITKEEPER(s) && d->next) {
		unless (d->next->date <= d->date) {
			unless (flags & ADMIN_SHUTUP) {
				fprintf(stderr,
				    "\t%s: %s,%s dates do not "
				    "increase in table\n",
				    s->sfile, d->rev, d->next->rev);
			}
			error |= 2;
		}
	}

	/* Make sure we have no duplicate keys, assuming table sorted by date */
	if (BITKEEPER(s) &&
	    d->next &&
	    (d->date == d->next->date) &&
	    (d->next != d->parent)) { /* parent already checked above */
		char	me[MAXPATH], next[MAXPATH];

		sccs_sdelta(s, d, me);
		sccs_sdelta(s, d->next, next);
		if (streq(next, me)) {
			fprintf(stderr,
			    "\t%s: %s,%s have same key\n",
			    s->sfile, d->rev, d->next->rev);
			error |= 2;
		}
	}

done:	if (error & 1) d->flags |= D_BADREV;
	return (error);
}

private int
getval(char *arg)
{
	if (!arg || !*arg || !isdigit(*arg)) return (-1);
	return (atoi(arg));
}

/*
 * Accept dates in any of the following forms and convert to the
 * SCCS form, filling out the delta struct.
 *	1998-01-11 20:00:00-08		(RCS -zLT)
 *	1990/01/12 04:00:00		(RCS UTC)
 *	98/01/11 20:00:00.000-8:00	(LMSCCS fully qualified)
 *	98/01/11 20:00:00.000		(LMSCCS localtime)
 *	98/01/11 20:00:00		(SCCS, local time)
 */
private delta *
dateArg(delta *d, char *arg, int defaults)
{
	char	*save = arg;
	char	tmp[50];
	int	year, month, day, hour, minute, second, msec, hwest, mwest;
	char	sign = ' ';
	int	rcs = 0;
	int	gotZone = 0;

	if (!d) d = (delta *)calloc(1, sizeof(*d));
	if (!arg || !*arg) { d->flags = D_ERROR; return (d); }
	year = getval(arg);
	if (year == -1) {
out:		fprintf(stderr, "sccs: can't parse date format %s at %s\n",
		    save, arg && *arg ? arg : "(unknown)");
		d->flags = D_ERROR;
		return (d);
	}
	if (year >= 100) {
		rcs++;
		if (year > 2068 || year < 1969) {
			goto out;
		}
		if (year < 2000) year -= 1900;
		if (year >= 2000) year -= 2000;
	}
/* CSTYLED */
#define	move(a) {while (*a && isdigit(*a)) a++; if (*a && (*a++ == '-')) rcs++;}
#define	getit(what) {move(arg); what = getval(arg); if (what == -1) goto out; }
	getit(month);
	getit(day);
	getit(hour);
	getit(minute);
	getit(second);
	hwest = mwest = 0;
	msec = 0;
	if (*arg && arg[2] == '.') {
		getit(msec);
		if (arg[-1] == '-') {
			gotZone++;
			sign = '-';
			getit(hwest);
			getit(mwest);
		}
	} else if (*arg && arg[2] == '-') {
		gotZone++;
		sign = '-';
		getit(hwest);
		/* I don't know if RCS ever puts in the minutes, but I'll
		 * take 'em if they give 'em.
		 */
		if (*arg && arg[2] == ':') getit(mwest);
	} else if (*arg && arg[2] == '+') {
		gotZone++;
		sign = '+';
		getit(hwest);
		/* I don't know if RCS ever puts in the minutes, but I'll
		 * take 'em if they give 'em.
		 */
		if (*arg && arg[2] == ':') getit(mwest);
	} else if (rcs || defaults) {
		/* This is a bummer because we can't figure out in which
		 * timezone the delta was performed.
		 * So we assume here.
		 * XXX - maybe not the right answer?
		 */
		long	seast;
		time_t	t = time(0);

		gotZone++;
		localtimez(&t, &seast);
		if (seast < 0) {
			seast = -seast;
			sign = '-';
		} else {
			sign = '+';
		}

		hwest = seast / 3600;
		mwest = (seast % 3600) / 60;
	}
	sprintf(tmp, "%02d/%02d/%02d %02d:%02d:%02d",
	    year, month, day, hour, minute, second);
	d->sdate = strdup(tmp);
	if (gotZone) {
		sprintf(tmp, "%c%02d:%02d", sign, hwest, mwest);
		d->zone = strdup(tmp);
	}
	DATE(d);
	return (d);
}
#undef	getit
#undef	move

private delta *
userArg(delta *d, char *arg)
{
	char	*save = arg;

	if (!d) d = (delta *)calloc(1, sizeof(*d));
	if (!arg || !*arg) { d->flags = D_ERROR; return (d); }
	while (*arg && (*arg++ != '@'));
	if (arg[-1] == '@') {
		arg[-1] = 0;
		if (d->hostname && !(d->flags & D_DUPHOST)) free(d->hostname);
		d->hostname = strdup(arg);
	}
	assert(!d->user);
	d->user = strdup(save);		/* has to be after we null the @ */
	return (d);
}

#define	ARG(field, flag, dup) \
	if (!d) d = (delta *)calloc(1, sizeof(*d)); \
	if (!arg || !*arg) { \
		d->flags |= flag; \
	} else { \
		if (d->field && !(d->flags & dup)) free(d->field); \
		d->field = strnonldup(arg); \
		d->flags &= ~(flag); \
	} \
	d->flags &= ~(dup); \
	return (d)

/*
 * Process the various args which we might have to save.
 * Null args are accepted in those with a "0" as arg 2.
 */
private delta *
csetFileArg(delta *d, char *arg) { ARG(csetFile, 0, D_DUPCSETFILE); }

private delta *
hashArg(delta *d, char *arg) { ARG(hash, 0, 0); }

private delta *
hostArg(delta *d, char *arg) { ARG(hostname, D_NOHOST, D_DUPHOST); }

private delta *
pathArg(delta *d, char *arg) { ARG(pathname, D_NOPATH, D_DUPPATH); }

private delta *
randomArg(delta *d, char *arg) { ARG(random, 0, 0); }

/*
 * Handle either 0664 style or -rw-rw-r-- style.
 */
delta *
modeArg(delta *d, char *arg)
{
	unsigned int m;

	if (!d) d = (delta *)calloc(1, sizeof(*d));
	unless (m = getMode(arg)) return (0);
	if (S_ISLNK(m))	 {
		char *p = strchr(arg , ' ');
		
		unless (p) return (0);
		d->symlink = strnonldup(++p);
		assert(!(d->flags & D_DUPLINK));
	}
	if (d->mode = m) d->flags |= D_MODE;
	return (d);
}

private delta *
sumArg(delta *d, char *arg)
{
	char	*p;
	if (!d) d = (delta *)calloc(1, sizeof(*d));
	d->flags |= D_CKSUM;
	d->sum = atoi(arg);
	for (p = arg; isdigit(*p); p++);
	return (d);
}

private delta *
mergeArg(delta *d, char *arg)
{
	if (!d) d = (delta *)calloc(1, sizeof(*d));
	assert(d->merge == 0);
	assert(isdigit(arg[0]));
	d->merge = atoi(arg);
	return (d);
}


private void
symArg(sccs *s, delta *d, char *name)
{
	symbol	*sym;

	assert(d);

	unless (CSET(s)) return;	/* no tags on regular files */

	/*
	 * Stash away the parent (and maybe merge) serial numbers
	 *
	 * Note that if this is a tag merge and there is no symbol on
	 * this node, then it never gets added to the symbol table.
	 * So you can't walk the symbol table looking for metad->symLeaf
	 * to find leaves.  See sccs_tagleaves().
	 */
	if (isdigit(*name)) {
		d->symGraph = 1;
		d->ptag = atoi(name);
		while (isdigit(*name)) name++;
		unless (*name++ == ' ') {
			return;
		}
		if (isdigit(*name)) {
			d->mtag = atoi(name);
			while (isdigit(*name)) name++;
			unless (*name++ == ' ') {
				return;
			}
		}
		assert(*name == 'l');
		d->symLeaf = 1;
		return;
	}

	sym = calloc(1, sizeof(*sym));
	sym->rev = strdup(d->rev);
	sym->symname = strnonldup(name);
	if (!s->symbols) {
		s->symbols = s->symTail = sym;
	} else {
		s->symTail->next = sym;
		s->symTail = sym;
	}
	/*
	 * This is temporary.  "d" gets reset to real delta once we
	 * build the graph.
	 */
	sym->d = sym->metad = d;
	d->flags |= D_SYMBOLS;

	/*
	 * If this succeeds, then ALL keys must be in long key format
	 */
	if (CSET(s) && streq(d->rev, "1.0") && streq(sym->symname, KEY_FORMAT2)) {
	    	s->xflags |= X_LONGKEY;
		/* get any previous xflags deltas */
		for (d = s->table; d; d = d->next) {
			if (d->flags & D_XFLAGS) d->xflags |= X_LONGKEY;
		}
		/* then meta() pushes this to all following deltas */
	}
}

private delta *
zoneArg(delta *d, char *arg)
{
	char	buf[20];

	unless ((arg[0] == '+') || (arg[0] == '-')) {
		sprintf(buf, "-%s", arg);
		arg = buf;
	}
	ARG(zone, D_NOZONE, D_DUPZONE);
}

/*
 * Take a string with newlines in it and split it into lines.
 * Note: null comments are accepted on purpose.
 */
private delta *
commentArg(delta *d, char *arg)
{
	char	*tmp;

	if (!d) d = (delta *)calloc(1, sizeof(*d));
	if (!arg) {
		/* don't call me unless you want one. */
		d->comments = addLine(d->comments, strdup(""));
		return (d);
	}
	while (arg && *arg) {
		tmp = arg;
		while (*arg && *arg++ != '\n');
		d->comments = addLine(d->comments, strnonldup(tmp));
	}
	return (d);
}

/*
 * Explode the rev.
 */
private delta *
revArg(delta *d, char *arg)
{
	if (!d) d = (delta *)calloc(1, sizeof(*d));
	d->rev = strdup(arg);
	explode_rev(d);
	return (d);
}
#undef	ARG

/*
 * Partially fill in a delta struct.  If the delta is null, allocate one.
 * Follow all the conventions used for delta creation such that this delta
 * can be added to the tree and freed later.
 */
delta *
sccs_parseArg(delta *d, char what, char *arg, int defaults)
{
	switch (what) {
	    case 'A':	/* hash */
		return (hashArg(d, arg));
	    case 'B':	/* csetFile */
		return (csetFileArg(d, arg));
	    case 'D':	/* any part of 1998/03/09 18:23:45.123-08:00 */
		return (dateArg(d, arg, defaults));
	    case 'U':	/* user or user@host */
		return (userArg(d, arg));
	    case 'H':	/* host */
		return (hostArg(d, arg));
	    case 'P':	/* pathname */
		return (pathArg(d, arg));
	    case 'O':	/* mode */
		return (modeArg(d, arg));
	    case 'C':	/* comments - one string, possibly multi line */
		return (commentArg(d, arg));
	    case 'R':	/* 1 or 1.2 or 1.2.3 or 1.2.3.4 */
		return (revArg(d, arg));
	    case 'Z':	/* zone */
		return (zoneArg(d, arg));
	    default:
		fprintf(stderr, "Unknown user arg %c ignored.\n", what);
		return (0);
	}
}

/*
 * Return true iff the most recent matching symbol is the same.
 */
private int
dupSym(symbol *symbols, char *s, char *rev)
{
	symbol	*sym;

	sym = findSym(symbols, s);
	/* If rev isn't set, then any name match is enough */
	if (sym && !rev) return (1);
	return (sym && streq(sym->rev, rev));
}

private int
addSym(char *me, sccs *sc, int flags, admin *s, int *ep)
{
	int	added = 0, i, error = 0;
	char	*rev;
	char	*sym;
	delta	*d, *n = 0;

	unless (s && s[0].flags) return (0);

	unless (CSET(sc)) {
		fprintf(stderr,
		    "Tagging files is not supported, use bk tag instead\n");
		return (0);
	}

	/*
	 * "sym" means TOT
	 * "sym|" means TOT
	 * "sym|1.2" means that rev.
	 */
	for (i = 0; s && s[i].flags; ++i) {
		sym = strdup(s[i].thing);
		if (rev = strrchr(sym, '|')) *rev++ = 0;
		/* Note: rev is set or null from above test */
		unless (d = sccs_findrev(sc, rev)) {
			verbose((stderr,
			    "%s: can't find %s in %s\n",
			    me, rev, sc->sfile));
sym_err:		error = 1; sc->state |= S_WARNED;
			free(sym);
			continue;
		}
		if (!rev || !*rev) rev = d->rev;
		if (sccs_badTag(me, sym, flags)) goto sym_err;
		if (dupSym(sc->symbols, sym, rev)) {
			verbose((stderr,
			    "%s: symbol %s exists on %s\n", me, sym, rev));
			goto sym_err;
		}

		// XXX - if anyone calls admin directly with two tags this can
		// be wrong.  bk tag doesn't.
		// We should just get rid of the multiple tag thing, it was
		// over engineered.
		unless (d == sccs_top(sc)) safe_putenv("BK_TAG_REV=%s", d->rev);
		safe_putenv("BK_TAG=%s", sym);
		if (trigger("tag", "pre")) goto sym_err;

		n = calloc(1, sizeof(delta));
		n->next = sc->table;
		sc->table = n;
		n = sccs_dInit(n, 'R', sc, 0);
		/*
		 * n->sum = (unsigned short) almostUnique(1);
		 *
		 * We don't write out the sum because we use chksum of zero to
		 * mean that this is a tag not a changeset.
		 * XXX - this is completely broken.  Changesets can have zero
		 * checksums.  The only place we use this is in the synckeys
		 * processing and we need to hand that info across some other
		 * way, like in the environment or an optional trailer block.
		 */
		n->rev = strdup(d->rev);
		explode_rev(n);
		n->pserial = d->serial;
		n->serial = sc->nextserial++;
		n->flags |= D_SYMBOLS;
		d->flags |= D_SYMBOLS;
		sc->numdeltas++;
		dinsert(sc, n, 1);
		if (addsym(sc, d, n, 1, rev, sym)) {
			verbose((stderr,
			    "%s: won't add identical symbol %s to %s\n",
			    me, sym, sc->sfile));
			/* No error here, it's not necessary */
			free(sym);
			continue;
		}
		added++;
		verbose((stderr,
		    "%s: add symbol %s->%s in %s\n",
		    me, sym, rev, sc->sfile));
		free(sym);
	}
	if (ep) *ep = error;
	return (added);
}


delta *
sccs_newDelta(sccs *sc, delta *p, int isNullDelta)
{
	delta	*n;
	char	*rev;

	/*
 	 * Until we fix the ChangeSet processing code
	 * we cannot allow null delta in ChangeSet file
	 */
	if (CSET(sc) && isNullDelta) {
		fprintf(stderr,
			"Cannot create null delta in ChangeSet file\n");
		return (0);
	}

	n = calloc(1, sizeof(delta));
	n->next = sc->table;
	sc->table = n;
	n = sccs_dInit(n, 'D', sc, 0);
	unless (p) p = findrev(sc, 0);
	rev = p->rev;
	sccs_getedit(sc, &rev);
	n->rev = strdup(rev);
	explode_rev(n);
	n->pserial = p->serial;
	n->serial = sc->nextserial++;
	sc->numdeltas++;
	if (isNullDelta) {
		n->added = n->deleted = 0;
		n->same = p->same + p->added;
		n->sum = almostUnique();
		n->flags |= D_CKSUM;
	}
	dinsert(sc, n, 1);
	return (n);
}

private void
addMode(char *me, sccs *sc, delta *n, mode_t m)
{
	char	buf[50];
	char	*newmode;

	assert(n);
	newmode = mode2a(m);
	sprintf(buf, "Change mode to %s", newmode);
	n->comments = addLine(n->comments, strdup(buf));
	(void)modeArg(n, newmode);
}

private int
changeXFlag(sccs *sc, delta *n, int flags, int add, char *flag)
{
	char	buf[50];
	u32	xflags, changing, eoln;

	assert(flag);

	changing = a2xflag(flag);
	unless (xflags = sccs_xflags(n)) xflags = sc->xflags;

	if (add) {
		if (xflags & changing) {
			verbose((stderr,
			    "admin: warning: %s %s flag is already on\n",
			    sc->sfile, flag));
			return (0);
		} 
		xflags |= changing;
	} else {
		unless ((changing == X_EOLN_UNIX) || (xflags & changing)) {
			verbose((stderr,
			    "admin: warning: %s %s flag is already off\n",
			    sc->sfile, flag));
			return (0);
		}
		xflags &= ~changing;
	}
	sc->xflags = xflags;
	assert(n);
	n->flags |= D_XFLAGS;
	n->xflags = xflags;
	/* pseudo flag, we speak only native & windows */
	unless (changing == X_EOLN_UNIX) {
		sprintf(buf, "Turn %s %s flag", add ? "on": "off", flag);
		n->comments = addLine(n->comments, strdup(buf));
	}
	/*
	 * We have two real EOLN xflags: X_EOLN_NATIVE and X_WINDOWS
	 * and one fake one: X_EOLN_UNIX = !X_EOLN_NATIVE
	 * and they are all mutually exclusive.
	 * Setting any one clears the others.
	 * Setting X_EOLN_UNIX clears all of them.
	 */
	eoln = X_EOLN_NATIVE|X_EOLN_WINDOWS|X_EOLN_UNIX;
	if (add && (eoln & changing)) {
		if (changing == X_EOLN_NATIVE) {
			if (n->xflags & X_EOLN_WINDOWS) {
				sprintf(buf, "Turn off EOLN_WINDOWS flag");
				n->comments = addLine(n->comments, strdup(buf));
				n->xflags &= ~X_EOLN_WINDOWS;
			}
		}
		if (changing == X_EOLN_WINDOWS) {
			if (n->xflags & X_EOLN_NATIVE) {
				sprintf(buf, "Turn off EOLN_NATIVE flag");
				n->comments = addLine(n->comments, strdup(buf));
				n->xflags &= ~X_EOLN_NATIVE;
			}
		}
		if (changing == X_EOLN_UNIX) {
			if (n->xflags & X_EOLN_WINDOWS) {
				sprintf(buf, "Turn off EOLN_WINDOWS flag");
				n->comments = addLine(n->comments, strdup(buf));
				n->xflags &= ~X_EOLN_WINDOWS;
			}
			if (n->xflags & X_EOLN_NATIVE) {
				sprintf(buf, "Turn off EOLN_NATIVE flag");
				n->comments = addLine(n->comments, strdup(buf));
				n->xflags &= ~X_EOLN_NATIVE;
			}
		}
		n->xflags &= ~X_EOLN_UNIX;	/* fake, in mem only */
	} else if (!add) {
		if (changing & (X_EOLN_WINDOWS|X_EOLN_UNIX)) {
			sprintf(buf, "Turn on EOLN_NATIVE flag");
			n->comments = addLine(n->comments, strdup(buf));
			n->xflags |= X_EOLN_NATIVE;
		}
	}
	assert(!(n->xflags & X_EOLN_UNIX));
	return (1);
}

int
sccs_xflags(delta *d)
{
	unless (d) return (0);
	if (d->flags & D_XFLAGS) return (d->xflags);
	if (d->parent) return (sccs_xflags(d->parent));
	return (0); /* old sfile, xflags values unknown */
}                    

/*
 * Translate an encoding string (e.g. "ascii") and a compression string
 * (e.g. "gzip") to a suitable value for sccs->encoding.
 */
int
sccs_encoding(sccs *sc, char *encp, char *compp)
{
	int enc, comp;

	if (encp) {
		if (streq(encp, "text")) enc = E_ASCII;
		else if (streq(encp, "ascii")) enc = E_ASCII;
		else if (streq(encp, "binary")) enc = sccs_binaryenc(sc);
		else if (streq(encp, "uuencode")) enc = E_UUENCODE;
		else if (streq(encp, "binpool")) enc = E_BINPOOL;
		else {
			fprintf(stderr,	"admin: unknown encoding format %s\n",
				encp);
			return (-1);
		}
	} else if (sc) {
		enc = (sc->encoding & E_DATAENC);
	} else {
		enc = 0;
	}

	/* never compress ChangeSet file */
	if (sc && CSET(sc)) compp = "none";

	if (compp) {
		if (streq(compp, "gzip")) {
			comp = E_GZIP;
		} else if (streq(compp, "none")) {
			comp = 0;
		} else {
			fprintf(stderr,
			    "admin: unknown compression format %s\n", compp);
			return (-1);
		}
		/* No gzip for binpool currently */
		if (enc == E_BINPOOL) comp = 0;
	} else if (sc) {
		comp = (sc->encoding & ~E_DATAENC);
	} else {
	        comp = 0;
	}

	return (enc | comp);
}

private void
adjust_serials(delta *d, int amount)
{
	int	i;

	d->serial += amount;
	d->pserial += amount;
	if (d->ptag) d->ptag += amount;
	if (d->mtag) d->mtag += amount;
	if (d->merge) d->merge += amount;
	EACH(d->include) d->include[i] += amount;
	EACH(d->exclude) d->exclude[i] += amount;
}

/*
 * Cons up a 1.0 delta, initializing as much as possible from the 1.1 delta.
 * If this is a BitKeeper file with changeset marks, then we have to 
 * replicate the key on the 1.1 delta.
 */
void
insert_1_0(sccs *s)
{
	delta	*d;
	delta	*t;
	int	csets = 0;

	/*
	 * First bump all the serial numbers.
	 */
	for (t = d = s->table; d; d = d->next) {
		if (d->flags & D_CSET) csets++;
		adjust_serials(d, 1);
		t = d;
	}

	d = calloc(1, sizeof(*d));
	t->next = d;		/* table is now linked */
	t = s->tree;
	d->kid = t;
	s->tree = d;		/* tree is now linked */
	d->rev = strdup("1.0");
	explode_rev(d);
	d->user = strdup(t->user);
	if (d->hostname = t->hostname) t->flags |= D_DUPHOST;
	if (d->pathname = t->pathname) t->flags |= D_DUPPATH;
	if (d->zone = t->zone) t->flags |= D_DUPZONE;
	d->serial = 1;
	if (csets) {
		d->date = t->date;	/* somebody is using this key already */
		d->sum = t->sum;
	} else {
		unless (d->random) {
			char	buf[20];

			buf[0] = 0;
			randomBits(buf);
			if (buf[0]) d->random = strdup(buf);
		}
		d->date = t->date - 1;
		d->sum = almostUnique();
	}
	date(d, d->date);
	d = sccs_dInit(d, 'D', s, 0);
}

private int
remove_1_0(sccs *s)
{
	if (streq(s->tree->rev, "1.0")) {
		delta	*d;

		MK_GONE(s, s->tree);
		for (d = s->table; d; d = d->next) adjust_serials(d, -1);
		return (1);
	}
	return (0);
}

int
sccs_newchksum(sccs *s)
{
	return (sccs_admin(s, 0, NEWCKSUM, 0, 0, 0, 0, 0, 0, 0, 0));
}

/*
 * Reverse sort it, we want the ^A's, if any, at the end.
 */
private int
c_compar(const void *a, const void *b)
{
	return (*(char*)b - *(char*)a);
}

private	char *
obscure(int rmlicense, int uu, char *buf)
{
	int	len;
	char	*new;
	char	*p, *t;

	/* XXX just write a strnldup */
	for (len = 0; buf[len] && (buf[len] != '\n'); len++);
	if (buf[len]) len++;
	assert(len);
	new = malloc(len+1);
	strncpy(new, buf, len);
	new[len] = 0;
	unless (len > 1) goto done; 	/* need to have something to obscure */

	if (*new == '\001') goto done;
	/*
	 * Try to match '.*\Wlicense\w*:.* or .*\Wlicsign\d\w*:
	 * and obsure them.
	 */
	if (rmlicense) {
		if (uu) goto done;
		/* only pick on active license and licsign lines */
		p = new;
		while (isspace(*p)) ++p;
		if (*p == '#') goto done;
		if ((*p == '[') && (p = strchr(p, ']'))) p++;
		unless (p && *p) goto done;
		while (isspace(*p)) ++p;
		unless ((strneq(p, "license", 7)) ||
		    (strneq(p, "licsign", 7))) {
			goto done;
		}
		t = p + 7;
		if ((t[-1] == 'n') && isdigit(*t)) ++t;	/* licsign\d */
		while (isspace(*t)) ++t;
		unless (*t == ':') goto done;
		*t = *new;		/* save first character in : */
		p[2] += ':' - '#';	/* turn a c into z */
		new[0] = '#';		/* replace : with # */
		uu = 1;			/* leave the first char in place */
	}
	if (uu) {
		qsort(new+1, len-2, 1, c_compar); /* leave first and last */
	} else {
		qsort(new, len-1, 1, c_compar);	/* leave last char */
	}
	assert(*new != '\001');
done:	return (new);
}

private	void
obscure_comments(sccs *s)
{
	delta	*d;
	char	*buf;
	int	i;

	for (d = s->table; d; d = d->next) {
		EACH(d->comments) {
			buf = obscure(0, 0, d->comments[i]);
			free(d->comments[i]);
			d->comments[i] = buf;
		}
	}
}

/*
 * admin the specified file.
 *
 * Note: flag values are optional.
 *
 * XXX - this could do the insert hack for paths/users/whatever.
 * For large files, this is a win.
 */
int
sccs_admin(sccs *sc, delta *p, u32 flags, char *new_encp, char *new_compp,
	admin *f, admin *z, admin *u, admin *s, char *mode, char *text)
{
	FILE	*sfile = 0;
	int	new_enc, error = 0, locked = 0, i, old_enc = 0;
	int	flagsChanged = 0;
	char	*t;
	char	*buf;
	delta	*d = 0;
	int	obscure_it, rmlicense;

	assert(!z); /* XXX used to be LOD item */

	new_enc = sccs_encoding(sc, new_encp, new_compp);
	if (new_enc == -1) return (-1);
	debug((stderr, "new_enc is %d\n", new_enc));
	GOODSCCS(sc);
	unless (flags & (ADMIN_BK|ADMIN_FORMAT|ADMIN_GONE)) {
		char	z = (flags & ADMIN_FORCE) ? 'Z' : 'z';

		unless (locked = sccs_lock(sc, z)) {
			verbose((stderr,
			    "admin: can't get lock on %s\n", sc->sfile));
			error = -1; sc->state |= S_WARNED;
out:
			if (sfile) fclose(sfile);
			if (locked) sccs_unlock(sc, 'z');
			debug((stderr, "admin returns %d\n", error));
			return (error);
		}
	}
#define	OUT	{ error = -1; sc->state |= S_WARNED; goto out; }
#define	ALLOC_D()	\
	unless (d) { \
		unless (d = sccs_newDelta(sc, p, 1)) OUT; \
		if (BITKEEPER(sc)) updatePending(sc); \
	}

	unless (HAS_SFILE(sc)) {
		verbose((stderr, "admin: no SCCS file: %s\n", sc->sfile));
		OUT;
	}

	if ((flags & ADMIN_BK) && checkInvariants(sc, flags)) OUT;
	if ((flags & ADMIN_GONE) && checkGone(sc, D_GONE, "admin")) OUT;
	if (flags & ADMIN_FORMAT) {
		if (checkrevs(sc, flags) ||
		    checkdups(sc) || checkMisc(sc, flags)) {
			OUT;
		}
#if 0
		/*
		 * Until such time as we decide to rewrite all the serial
		 * numbers when running stripdel, we can't do this.
		 */
		if (sc->nextserial != (sc->numdeltas + 1)) {
			verbose((stderr,
			    "admin: gaps in serials in %s (somewhat unusual)\n",
			    sc->sfile));
		}
#endif
	}
	if (flags & (ADMIN_BK|ADMIN_FORMAT)) goto out;
	if ((flags & (ADMIN_OBSCURE|ADMIN_RMLICENSE))
	    && sccs_clean(sc, (flags & SILENT))) {
		goto out;
	}

	if (addSym("admin", sc, flags, s, &error)) flags |= NEWCKSUM;
	if (mode) {
		delta	*n = sccs_top(sc);
		mode_t	m;

		assert(n);
		if ((n->flags & D_MODE) && n->symlink) {
			fprintf(stderr,
				"admin: %s: chmod on symlink is illegal\n",
				sc->gfile);
			OUT;
		} 
		unless (m = newMode(n, mode)) {
			fprintf(stderr, "admin: %s: Illegal file mode: %s\n",
			    sc->gfile, mode);
			OUT;
		}
		/* No null deltas for nothing */
		if (m == n->mode) goto skipmode;
		if (S_ISLNK(m) || S_ISDIR(m)) {
			fprintf(stderr, "admin: %s: Cannot change mode to/of "
			    "%s\n", sc->gfile,
			    S_ISLNK(m) ? "symlink" : "directory");
			OUT;
		}
		ALLOC_D();
		addMode("admin", sc, d, m);
		if (HAS_GFILE(sc) && HAS_PFILE(sc)) {
			chmod(sc->gfile, m);
		} else if (HAS_GFILE(sc)) {
			chmod(sc->gfile, m & ~0222);
		}
		flags |= NEWCKSUM;
	}
skipmode:

	if (text) {
		FILE	*desc;
		char	*c;
		char	dbuf[200];

		unless (text[0]) {
			if (sc->text) {
				freeLines(sc->text, free);
				sc->text = 0;
				flags |= NEWCKSUM;
			}
			ALLOC_D();
			c = "Remove Descriptive Text";
			d->comments = addLine(d->comments, strdup(c));
			assert(d->text == 0);
			d->flags |= D_TEXT;
			goto user;
		}
		desc = fopen(text, "rt"); /* must be text mode */
		unless (desc) {
			fprintf(stderr, "admin: can't open %s\n", text);
			error = 1; sc->state |= S_WARNED;
			goto user;
		}
		if (sc->text) {
			freeLines(sc->text, free);
			sc->text = 0;
		}
		ALLOC_D();
		c = "Change Descriptive Text";
		d->comments = addLine(d->comments, strdup(c));
		d->flags |= D_TEXT;
		while (fgets(dbuf, sizeof(dbuf), desc)) {
			sc->text = addLine(sc->text, strnonldup(dbuf));
			d->text = addLine(d->text, strnonldup(dbuf));
		}
		fclose(desc);
		flags |= NEWCKSUM;
	}

user:	for (i = 0; u && u[i].flags; ++i) {
		if (BITKEEPER(sc)) {
			fprintf(stderr,
			    "admin: changing user/group is not supported\n");
			OUT;
		}
		flags |= NEWCKSUM;
		if (u[i].flags & A_ADD) {
			sc->usersgroups =
			    addLine(sc->usersgroups, strdup(u[i].thing));
		} else {
			unless (removeLine(sc->usersgroups, u[i].thing, free)) {
				verbose((stderr,
				    "admin: user/group %s not found in %s\n",
				    u[i].thing, sc->sfile));
				error = 1; sc->state |= S_WARNED;
			}
		}
	}

	/*
	 * flags, unknown single letter passed through.
	 */
	for (i = 0; f && f[i].flags; ++i) {
		int	add = f[i].flags & A_ADD;
		char	*v = &f[i].thing[1];

		if (isupper(f[i].thing[0])) {
			char *fl = f[i].thing;

			v = strchr(v, '=');
			if (v) *v++ = '\0';
			if (v && *v == '\0') v = 0;

			if ((a2xflag(fl) & X_MONOTONIC) &&
			    sccs_top(sc)->dangling) {
			    	fprintf(stderr, "admin: "
				    "must remove danglers first (monotonic)\n");
				error = 1;
				sc->state |= S_WARNED;
				continue;
			}
			if (a2xflag(fl) & X_MAYCHANGE) {
				if (v) goto noval;
				ALLOC_D();
				flagsChanged +=
				    changeXFlag(sc, d, flags, add, fl);
			} else if (streq(fl, "DEFAULT")) {
				if (sc->defbranch) free(sc->defbranch);
				sc->defbranch = v ? strdup(v) : 0;
				flagsChanged++;
			} else {
				if (v) {
					fprintf(stderr,
					  "admin: unknown flag %s=%s\n", fl, v);
				} else {
					fprintf(stderr,
					     "admin: unknown flag %s\n", fl);
				}
				error = 1;
				sc->state |= S_WARNED;
			}
			continue;

		noval:	fprintf(stderr,
			    "admin: flag %s can't have a value\n", fl);
			error = 1;
			sc->state |= S_WARNED;
		} else {
			char	*buf;

			switch (f[i].thing[0]) {
			    case 'd':
				if (sc->defbranch) free(sc->defbranch);
				sc->defbranch = *v ? strdup(v) : 0;
				flagsChanged++;
				break;
			    case 'e':
				if (BITKEEPER(sc)) {
					fprintf(stderr, "Unsupported.\n");
				} else {
					if (*v) new_enc = atoi(v);
					verbose((stderr,
					    "New encoding %d\n", new_enc));
					flagsChanged++;
				}
		   		break;
			    default:
				buf = aprintf("%c %s", v[-1], v);
				flagsChanged++;
				if (add) {
					sc->flags =
						addLine(sc->flags, strdup(buf));
				} else {
					unless (removeLine(sc->flags,
					    buf, free)) {
						verbose((stderr,
					"admin: flag %s not found in %s\n",
							 buf, sc->sfile));
						error = 1;
						sc->state |= S_WARNED;
					}
				}
				free(buf);
				break;
			}
		}
	}
	if (flagsChanged || (sc->encoding != new_enc)) flags |= NEWCKSUM;

	if (flags & ADMIN_ADD1_0) {
		insert_1_0(sc);
		flags |= NEWCKSUM;
	} else if (flags & ADMIN_RM1_0) {
		if (remove_1_0(sc)) {
			flags |= NEWCKSUM;
		} else {
			flags &= ~ADMIN_RM1_0;
		}
	}

	if (flags & ADMIN_NEWPATH) {
		ALLOC_D(); /* We pick up the new path when we init the delta */
		assert(d->pathname);
		d->comments = addLine(d->comments,
				aprintf("Rename: %s -> %s",
					d->parent->pathname, d->pathname));
		flags |= NEWCKSUM;
	}

	if (flags & ADMIN_DELETE) {
		ALLOC_D(); /* We pick up the new path when we init the delta */
		assert(d->pathname);
		d->comments = addLine(d->comments,
				aprintf("Delete: %s", d->parent->pathname));
		flags |= NEWCKSUM;
	}

	unless (flags & NEWCKSUM) goto out;

	if (flags & ADMIN_OBSCURE) obscure_comments(sc);

	/*
	 * Do the delta table & misc.
	 */
	unless (locked || (locked = sccs_lock(sc, 'z'))) {
		verbose((stderr, "admin: can't get lock on %s\n", sc->sfile));
		OUT;
	}
	unless (sfile = fopen(sccsXfile(sc, 'x'), "w")) {
		fprintf(stderr, "admin: can't create %s: ", sccsXfile(sc, 'x'));
		perror("");
		OUT;
	}
	old_enc = sc->encoding;
	sc->encoding = new_enc;
	if (delta_table(sc, sfile, 0)) {
		sccs_unlock(sc, 'x');
		if (sc->io_warned) OUT;
		goto out;	/* we don't know why so let sccs_why do it */
	}
	assert(sc->state & S_SOPEN);
	seekto(sc, sc->data);
	debug((stderr, "seek to %d\n", (int)sc->data));
	obscure_it = (flags & ADMIN_OBSCURE);
	rmlicense = (flags & ADMIN_RMLICENSE);
	/* ChangeSet can't be obscured in any sense */
	if (CSET(sc)) {
	    	rmlicense = obscure_it = 0;
	}
	/* the BitKeeper/etc files can't be obscured in normal sense */
	if (sc->tree->pathname
	    && strneq(sc->tree->pathname,"BitKeeper/etc/",13)) {
	    	obscure_it = 0;
	}
	if ((old_enc & E_GZIP) && (obscure_it|rmlicense)) {
		fprintf(stderr, "admin: cannot obscure gzipped data.\n");
		OUT;
	}
	if (rmlicense) obscure_it = 1;
	if (old_enc & E_GZIP) {
		sc->zin = zgets_init(sc->where, sc->size - sc->data);
	}
	if (new_enc & E_GZIP) sccs_zputs_init(sc, sfile);
	/* if old_enc == new_enc, this is slower but handles both cases */
	sc->encoding = old_enc;
	while (buf = nextdata(sc)) {
		if (obscure_it) {
			buf = obscure(rmlicense, old_enc & E_UUENCODE, buf);
		}
		sc->encoding = new_enc;
		if (flags & ADMIN_ADD1_0) {
			fputbumpserial(sc, buf, 1, sfile);
		} else if (flags & ADMIN_RM1_0) {
			if (strneq(buf, "\001I 1\n", 5)) {
				sc->encoding = old_enc;
				buf = nextdata(sc);
				assert(strneq(buf, "\001E 1\n", 5));
				assert(!nextdata(sc));
				break;
			}
			fputbumpserial(sc, buf, -1, sfile);
		} else {
			fputdata(sc, buf, sfile);
		}
		sc->encoding = old_enc;
		if (obscure_it) free(buf);
	}
	if (flags & ADMIN_ADD1_0) {
		sc->encoding = new_enc;
		fputdata(sc, "\001I 1\n", sfile);
		fputdata(sc, "\001E 1\n", sfile);
	}

	/* not really needed, we already wrote it */
	sc->encoding = new_enc;
	if (fflushdata(sc, sfile)) {
		sccs_unlock(sc, 'x');
		sccs_close(sc), fclose(sfile), sfile = NULL;
		if (sc->io_warned) OUT;
		goto out;
	}
	fseek(sfile, 0L, SEEK_SET);
	fprintf(sfile, "\001%c%05u\n", BITKEEPER(sc) ? 'H' : 'h', sc->cksum);
#ifdef	DEBUG
	badcksum(sc, flags);
#endif
	sccs_close(sc), fclose(sfile), sfile = NULL;
	if (old_enc & E_GZIP) {
		if (zgets_done(sc->zin)) OUT;
		sc->zin = 0;
	}
	t = sccsXfile(sc, 'x');
	if (rename(t, sc->sfile)) {
		fprintf(stderr,
		    "admin: can't rename(%s, %s) left in %s\n",
		    t, sc->sfile, t);
		OUT;
	}

	sccs_setStime(sc, 0);
	chmod(sc->sfile, 0444);
	goto out;
#undef	OUT
}

private	char *scompress_file;

private int
scompress(delta *d)
{
	int	s;

	unless (d) return (1);
	s = scompress(d->next);
	if (s != d->serial) {
		if (scompress_file) {
			fprintf(stderr,
			    "Remap %s:%d ->%d\n", scompress_file, d->serial, s);
		}
		d->serial = s;
	}
	return (d->serial + 1);
}

/*
 * Remve any gaps in the serial numbers.
 * Should be called after a stripdel.
 */
int
sccs_scompress(sccs *s, int flags)
{
	FILE	*sfile = 0;
	int	ser, error = 0, locked = 0, i;
	char	*t;
	char	*buf;
	delta	*d;
	ser_t	*orig, *remap;

	unless (locked = sccs_lock(s, 'z')) {
		fprintf(stderr, "scompress: can't get lock on %s\n", s->sfile);
		error = -1; s->state |= S_WARNED;
out:
		if (sfile) fclose(sfile);
		if (locked) sccs_unlock(s, 'z');
		debug((stderr, "scompress returns %d\n", error));
		return (error);
	}
#define	OUT	{ error = -1; s->state |= S_WARNED; goto out; }

	orig = calloc(sizeof(ser_t), s->nextserial);
	remap = calloc(sizeof(ser_t), s->nextserial);
	for (i = 0, d = s->table; d; d = d->next) orig[i++] = d->serial;

	if (flags & SILENT) {
		scompress_file = 0;
	} else {
		scompress_file = s->gfile;
	}
	scompress(s->table);
	for (i = 0, d = s->table; d; d = d->next, i++) {
		if (d->next) assert(d->serial == (d->next->serial + 1));
		remap[orig[i]] = d->serial;
	}

	for (d = s->table; d; d = d->next) {
		d->pserial = remap[d->pserial];
		if (d->ptag) d->ptag = remap[d->ptag];
		if (d->mtag) d->mtag = remap[d->mtag];
		if (d->merge) d->merge = remap[d->merge];
		EACH(d->include) d->include[i] = remap[d->include[i]];
		EACH(d->exclude) d->exclude[i] = remap[d->exclude[i]];
	}

	unless (sfile = fopen(sccsXfile(s, 'x'), "w")) {
		fprintf(stderr,
		    "scompress: can't create %s: ", sccsXfile(s, 'x'));
		perror("");
		OUT;
	}
	if (delta_table(s, sfile, 0)) {
		sccs_unlock(s, 'x');
		if (s->io_warned) OUT;
		goto out;	/* we don't know why so let sccs_why do it */
	}
	assert(s->state & S_SOPEN);
	seekto(s, s->data);
	debug((stderr, "seek to %d\n", (int)s->data));
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
		sccs_zputs_init(s, sfile);
	}
	while (buf = nextdata(s)) {
		unless (buf[0] == '\001') {
			fputdata(s, buf, sfile);
			continue;
		}
		ser = atoi(&buf[3]);
		fputbumpserial(s, buf, remap[ser] - ser, sfile);
	}
	if (fflushdata(s, sfile)) {
		sccs_unlock(s, 'x');
		sccs_close(s), fclose(sfile), sfile = NULL;
		if (s->io_warned) OUT;
		goto out;
	}
	fseek(sfile, 0L, SEEK_SET);
	fprintf(sfile, "\001%c%05u\n", BITKEEPER(s) ? 'H' : 'h', s->cksum);
	sccs_close(s), fclose(sfile), sfile = NULL;
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) OUT;
		s->zin = 0;
	}
	t = sccsXfile(s, 'x');
	if (rename(t, s->sfile)) {
		fprintf(stderr,
		    "admin: can't rename(%s, %s) left in %s\n",
		    t, s->sfile, t);
		OUT;
	}
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	goto out;
#undef	OUT
}

private void
doctrl(sccs *s, char *pre, int val, char *post, FILE *out)
{
	char	tmp[10];

	sertoa(tmp, (ser_t) val);
	fputdata(s, pre, out);
	fputdata(s, tmp, out);
	fputdata(s, post, out);
	fputdata(s, "\n", out);
}

private void
finish(sccs *s, int *ip, int *pp, int *last, FILE *out, register serlist *state,
	ser_t *slist)
{
	int	print = *pp, incr = *ip;
	sum_t	sum;
	register char	*buf;
	ser_t	serial;
	int	lf_pend = *last;

	debug((stderr, "finish(incr=%d, sum=%d, print=%d) ",
		incr, s->dsum, print));
	if (lf_pend) s->dsum -= '\n';
	while (!eof(s)) {
		unless (buf = nextdata(s)) break;
		debug2((stderr, "G> %.*s", linelen(buf), buf));
		sum = fputdata(s, buf, out);
		if (isData(buf)) {
			/* CNTLA_ESCAPE is not part of the check sum */
			if (buf[0] == CNTLA_ESCAPE) sum -= CNTLA_ESCAPE;

			if (!print) {
				/* if we are skipping data from pending block */
				if (lf_pend &&
				    lf_pend == whatstate((const serlist*)state))
				{
					s->dsum += '\n';
					lf_pend = 0;
				}
				continue;
			}
			*last = 0;
			unless (lf_pend) sum -= '\n';
			lf_pend = print;
			s->dsum += sum;
			incr++;
			continue;
		}
		serial = atoi(&buf[3]);
		if (buf[1] == 'E' && lf_pend == serial &&
		    whatstate((const serlist*)state) == serial)
		{
			char	*n = &buf[3];
			while (isdigit(*n)) n++;
			if (*n != 'N') {
				lf_pend = 0;
				s->dsum += '\n';
			}
		}
		changestate(state, buf[1], serial);
		print = printstate((const serlist*)state, (const ser_t*)slist);
	}
	unless (lf_pend) *last = 0;
	*ip = incr;
	*pp = print;
	debug((stderr, "incr=%d, sum=%d\n", incr, s->dsum));
}

#define	nextline(inc)	\
    nxtline(s, &inc, 0, &lines, &print, &last, out, state, slist, &savenext)
#define	beforeline(inc) \
    nxtline(s, &inc, 1, &lines, &print, &last, out, state, slist, &savenext)

private void
nxtline(sccs *s, int *ip, int before, int *lp, int *pp, int *last, FILE *out,
	register serlist *state, ser_t *slist, char ***savenext)
{
	int	print = *pp, incr = *ip, lines = *lp;
	int	serial;
	char	*n;
	sum_t	sum;
	register char	*buf;

	debug((stderr, "nxtline(@%d, before=%d print=%d, sum=%d) ",
	    lines, before, print, s->dsum));
	while (!eof(s)) {
		if (before && print) { /* if move upto next printable line */
			if (isData(peek(s))) break;
		}
		unless (buf = nextdata(s)) break;
		debug2((stderr, "[%d] ", lines));
		debug2((stderr, "G> %.*s", linelen(buf), buf));
		sum = fputdata(s, buf, out);
		if (isData(buf)) {
			if (print) {
				if (*savenext) {
					char	*p = strchr(buf, '\n');

					assert(p);
					**savenext = strndup(buf, p - buf + 1);
					assert(**savenext);
					*savenext = 0;
				}
				/* CNTLA_ESCAPE is not part of the check sum */
				if (buf[0] == CNTLA_ESCAPE) sum -= CNTLA_ESCAPE;
				s->dsum += sum;
				incr++; lines++;
				break;
			}
			continue;
		}
		n = &buf[3];
		serial = atoi_p(&n);
		if ((buf[1] == 'E') && (*last == serial) &&
		    (whatstate((const serlist*)state) == serial) &&
		    (*n != 'N')) {
			*last = 0;
		}
		changestate(state, buf[1], serial);
		print = printstate((const serlist*)state, (const ser_t*)slist);
	}
	*ip = incr;
	*lp = lines;
	*pp = print;
	debug((stderr, "sum=%d\n", s->dsum));
}

/*
 * Get the hash checksum.
 * A side effect of get_reg() is to set dsum.
 * We're getting what looks like the new delta but it is really
 * the basis for the new delta -
 * we are still operating on the old file, without the diffs applied.
 */
private int
getHashSum(sccs *sc, delta *n, MMAP *diffs)
{
	char	*buf;
	char	key[MAXPATH], val[MAXPATH];
	char	*v, *t;
	u8	*e;
	unsigned int sum = 0;
	int	lines = 0;
	int	flags = SILENT|GET_HASHONLY|GET_SUM|GET_SHUTUP;
	delta	*d;
	int	offset;

	assert(HASH(sc));
	assert(!BINPOOL(sc));
	assert(diffs);
	/*
	 * If we have a hash already and it is a simple delta, then just
	 * use that.  Otherwise, regen from scratch.
	 */
	if (sc->mdbm
	    && !n->include && !n->exclude && (d = getCksumDelta(sc, n))) {
	    	sum = d->sum;
	} else {
		if (sc->mdbm) mdbm_close(sc->mdbm), sc->mdbm = 0;
		sccs_restart(sc);
		if (get_reg(sc, 0, flags, n, &lines, 0, 0)) {
			sccs_whynot("delta", sc);
			return (-1);
		}
		sum = sc->dsum;
	}
	mseekto(diffs, 0);
	unless (buf = mnext(diffs)) {
		/* there are no diffs, we have the checksum, it's OK */
		return (0);
	}
	offset = 0;
	if (strneq(buf, "0a0\n", 4)) {
		offset = 2;
	}
	else unless (strneq(buf, "I0 ", 3)) {
		fprintf(stderr, "Missing '0a0' or 'I0 #lines', ");
bad:		fprintf(stderr, "bad diffs: '%.*s'\n", linelen(buf), buf);
		return (-1);
	}
	while (buf = mnext(diffs)) {
		unless (offset == 0 || buf[0] == '>') goto bad;
		t = key, v = &buf[offset];
		if (CSET(sc)) {
			int	pipes = 0;

			/*
			 * Keys are like u@h|path|date|.... whatever
			 * We want to skip over any spaces in the path part.
			 */
			while (v < diffs->end) {
				if (*v == '|') pipes++;
				if ((*v == ' ') && (pipes >= 2)) break;
				*t++ = *v++;
			}
		} else {
			while ((v < diffs->end) && (*v != ' ')) *t++ = *v++;
		}
		unless (*v == ' ') goto bad;
		*t = 0;
		for (t = val, v++; (v < diffs->end) && (*v != '\n'); ) {
			*t++ = *v++;
		}
		unless (*v == '\n') goto bad;
		*t = 0;
		if (v = mdbm_fetch_str(sc->mdbm, key)) {
			if (streq(v, val)) {
				fprintf(stderr,
				    "Redundant: %s %s\n", key, val);
				return (-1);
			} else {
				/*
				 * Subtract off the old value and add in new
				 */
				for (e = v; *e; sum -= *e++);
				for (e = val; *e; sum += *e++);
				mdbm_store_str(sc->mdbm, key, val,MDBM_REPLACE);
			}
		} else {
			/* completely new, add in the whole line */
			for (e = key; *e; sum += *e++); sum += ' ';
			for (e = val; *e; sum += *e++); sum += '\n';
			mdbm_store_str(sc->mdbm, key, val, MDBM_INSERT);
		}
	}
	sc->dsum = sum;
	return (0);
}

private int
delta_body(sccs *s, delta *n, MMAP *diffs, FILE *out, int *ap, int *dp, int *up)
{
	serlist *state;
	ser_t	*slist;
	int	print;
	int	lines;
	int	last;
	int	lastdel;
	int	fixdel = 0;
	char	*addthis;
	char	**savenext;
	long	offset;
	int	added, deleted, unchanged;
	sum_t	cksumsave;
	sum_t	sum;
	char	*b;
	int	no_lf;

	if (binaryCheck(diffs)) {
		assert(!BINARY(s));
		fprintf(stderr,
		    "%s: file format is ascii, delta is binary.", s->sfile);
		fprintf(stderr, "  Unsupported operation.\n");
		return (-1);
	}
	assert(!READ_ONLY(s));
	assert(s->state & S_ZFILE);
	offset = ftell(out);
	cksumsave = s->cksum;
again:
	*ap = *dp = *up = 0;
	state = 0;
	slist = 0;
	print = 0;
	lines = 0;
	last = 0;
	lastdel = 0;
	addthis = 0;
	savenext = 0;
	added = 0;
	deleted = 0;
	unchanged = 0;
	no_lf = 0;
	/*
	 * Do the actual delta.
	 */
	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
		sccs_zputs_init(s, out);
	}
	slist = serialmap(s, n, 0, 0, 0);	/* XXX - -gLIST */
	s->dsum = 0;
	assert(s->state & S_SOPEN);
	state = allocstate(0, s->nextserial);
	while (b = mnext(diffs)) {
		int	where;
		char	what;
		int	howmany;

newcmd:
		if (scandiff(b, &where, &what, &howmany) != 0) {
			int	len = linelen(b);

			fprintf(stderr,
			    "delta: can't figure out '%.*s'\n", len, b);
			if (state) free(state);
			if (slist) free(slist);
			return (-1);
		}
		debug2((stderr, "where=%d what=%c\n", where, what));

#define	ctrl(pre, val, post)	doctrl(s, pre, val, post, out)

		if (what == 'c' || what == 'd' || what == 'D' || what == 'x')
		{
			where--;
			lastdel = where;
			if (where && (fixdel == where)) {
				where--;
				howmany++;
				while (lines < where) {
					nextline(unchanged);
				}
				savenext = &addthis;
			}
		}
		while (lines < where) {
			/*
			 * XXX - this loops when I don't use the fudge as part
			 * of the ID in make/takepatch of SCCSFILE.
			 */
			nextline(unchanged);
		}
		last = print;
		switch (what) {
		    case 'c':
		    case 'd':
			beforeline(unchanged);
			ctrl("\001D ", n->serial, "");
			sum = s->dsum;
			/* howmany != 0 only for nonewline corner fixer */
			while (howmany--) nextline(deleted);
			while (b = mnext(diffs)) {
				if (strneq(b, "---\n", 4)) break;
				if (strneq(b, "\\ No", 4)) continue;
				if (isdigit(b[0])) {
					ctrl("\001E ", n->serial, "");
					s->dsum = sum;
					goto newcmd;
				}
				nextline(deleted);
				if (last == print) last = 0;
			}
			s->dsum = sum;
			if (what != 'c') break;
			ctrl("\001E ", n->serial, "");
			/* fall through to */
		    case 'a':
			last = 0;
			ctrl("\001I ", n->serial, "");
			while (b = mnext(diffs)) {
				if (strneq(b, "\\ No", 4)) {
					s->dsum -= '\n';
					no_lf = 1;
					break;
				}
				if (isdigit(b[0])) {
					ctrl("\001E ", n->serial, "");
					goto newcmd;
				}
				fix_cntl_a(s, &b[2], out);
				s->dsum += fputdata(s, &b[2], out);
				debug2((stderr,
				    "INS %.*s", linelen(&b[2]), &b[2]));
				added++;
			}
			break;
		    case 'N':
		    case 'I':
		    case 'i':
			last = 0;
			ctrl("\001I ", n->serial, "");
			while (howmany--) {
				/* XXX: not break but error */
				unless (b = mnext(diffs)) break;
				if (what != 'i' && b[0] == '\\') {
					fix_cntl_a(s, &b[1], out);
					s->dsum += fputdata(s, &b[1], out);
				} else {
					fix_cntl_a(s, b, out);
					s->dsum += fputdata(s, b, out);
				}
				debug2((stderr, "INS %.*s", linelen(b), b));
				added++;
			}
			if (what == 'N') {
				s->dsum -= '\n';
				no_lf = 1;
			}
			break;
		    case 'D':
		    case 'x':
			beforeline(unchanged);
			ctrl("\001D ", n->serial, "");
			sum = s->dsum;
			while (howmany--) {
				nextline(deleted);
				if (last == print) last = 0;
			}
			s->dsum = sum;
			break;
		}
		ctrl("\001E ", n->serial, no_lf ? "N" : "");
	}
	if (addthis) {
		last = 0;
		ctrl("\001I ", n->serial, "");
		s->dsum += fputdata(s, addthis, out);
		if (addthis[0] == CNTLA_ESCAPE) s->dsum -= CNTLA_ESCAPE;
		ctrl("\001E ", n->serial, "");
		free(addthis);
	}
	finish(s, &unchanged, &print, &last, out, state, slist);
	*ap = added;
	*dp = deleted;
	*up = unchanged;
	if (state) free(state);
	if (slist) free(slist);
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) return (-1);
		s->zin = 0;
	}
	if (last) {
		off_t	end = offset;
		int	fd = fileno(out);

		assert(!fixdel);	/* no infinite loops */
		fixdel = lastdel;
		if (fflushdata(s, out) || fseek(out, offset, SEEK_SET)) {
			perror("delta");
			return (-1);
		}
		ftruncate(fd, end);	/* In case gzip file gets smaller */
		mseekto(diffs, 0);
		s->cksum = cksumsave;
		goto again;
	}
	if (HASH(s) && (getHashSum(s, n, diffs) != 0)) {
		return (-1);
	}

	/*
	 * For ChangeSet file, force d->same to one
	 * because we do not maintain this field in the cweave code
	 */
	if (CSET(s)) *up = 1;

	return (0);
}

/*
 * Patch format is a little different, it looks like
 * 0a0
 * > key
 * > key
 * 
 * We need to strip out all the non-key stuff: 
 * a) "0a0"
 * b) "> "
 *
 * We want the output to look like:
 * ^AI serial
 * key
 * key
 * ^AE
 */
private void
patchweave(sccs *s, u8 *p, u32 len, u8 *buf, FILE *f)
{
	u8	*stop;

	fputdata(s, "\001I ", f);
	fputdata(s, buf, f);
	fputdata(s, "\n", f);
	if (p) {
		stop = p + len;
		assert(strneq(p, "0a0\n> ", 6));
		p += 6; /* skip "0a0\n" */
		while (1) {
			fputdata(s, p, f);
			while (*p++ != '\n') { /* null body */ }
			if (p == stop) break;
			assert(strneq("> ", p, 2));
			p += 2; /* skip "> " */
		}
	}
	fputdata(s, "\001E ", f);
	fputdata(s, buf, f);
	fputdata(s, "\n", f);
}

/*
 * sccs_csetPatchWeave()
 * This is similar to delta body, and makes use of many of the I/O
 * functions, which is why it is here.  The purpose is to weave in
 * patch items to the weave as it is copied to the output file.
 * This is useful for fast-takepatch operation for cset file.
 */

int
sccs_csetPatchWeave(sccs *s, FILE *f)
{
	u8	buf[20];
	u8	*line;
	u32	i;
	u32	ser;
	loc	*lp;

	assert(s);
	assert(s->state & S_CSET);
	assert(s->locs);
	lp = s->locs;
	i = s->iloc - 1; /* set index to final element in array */
	assert(i > 0); /* base 1 data structure */

	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
		sccs_zputs_init(s, f);
	}
	while (line = nextdata(s)) {
		assert(strneq(line, "\001I ", 3));
		ser = atoi(&line[3]);

		for ( ; i ; i--) {
			if (ser + i > lp[i].serial) break;
			unless (lp[i].p || lp[i].serial == 1) continue;
			sertoa(buf, lp[i].serial);
			patchweave(s, lp[i].p, lp[i].len, buf, f);
		}
		unless (i) break;

		/* bump the serial number up by # of items we have left */
		ser += i;
		sertoa(buf, ser);
		fputdata(s, "\001I ", f);
		fputdata(s, buf, f);
		fputdata(s, "\n", f);
		while (line = nextdata(s)) {
			if (*line == '\001') break;
			fputdata(s, line, f);
		}
		assert(strneq(line, "\001E ", 3));
		fputdata(s, "\001E ", f);
		fputdata(s, buf, f);
		fputdata(s, "\n", f);
	}
	assert(!(i && line));
	/* Print out remaining, forcing serial 1 block at the end */
	for ( ; i ; i--) {
		unless (lp[i].p || lp[i].serial == 1) continue;
		sertoa(buf, lp[i].serial);
		patchweave(s, lp[i].p, lp[i].len, buf, f);
	}
	/* No translation of serial numbers needed for remainder of file */
	for ( ; line; line = nextdata(s)) {
		fputdata(s, line, f);
	}
	if (s->encoding & E_GZIP) {
		zgets_done(s->zin);
		s->zin = 0;
	}
	if (fflushdata(s, f)) return (-1);
	return (0);
}

int
sccs_hashcount(sccs *s)
{
	int	n;
	kvpair	kv;

	unless (HASH(s)) return (0);
	if (sccs_get(s, "+", 0, 0, 0, SILENT|GET_HASHONLY, 0)) {
		sccs_whynot("get", s);
		return (0);
	}
	/* count the number of long keys in the *values* since those
	 * are far more likely to be current.
	 */
	for (n = 0, kv = mdbm_first(s->mdbm);
	    kv.key.dsize; kv = mdbm_next(s->mdbm)) {
		unless (CSET(s)) {
			n++;
			continue;
		}
		if (sccs_iskeylong(kv.val.dptr)) n++;
	}
	return (n);
}

/*
 * Initialize as much as possible from the file.
 * Don't override any information which is already set.
 * XXX - this needs to track sccs_prsdelta/do_patch closely.
 *
 * R/D/M - delta type
 * B - cset file key
 * C - cset boundry marker
 * D - dangle marker
 * c - comments
 * E - ignored for now
 * F - date fudge
 * i - include keys
 * K - delta checksum
 * M - merge delta key
 * O - modes (file permissions)
 * P - pathname
 * R - random bits
 * S - symbols
 * s - symbol graph/leaf
 * T - text
 * V - file format version
 * x - exclude keys
 * X - X_flags
 */
delta *
sccs_getInit(sccs *sc, delta *d, MMAP *f, int patch, int *errorp, int *linesp,
	     char ***symsp)
{
	char	*s, *t;
	char	*buf;
	int	nocomments = d && d->comments;
	int	error = 0;
	int	lines = 0;
	char	type = '?';
	char	**syms = 0;

	unless (f) {
		if (errorp) *errorp = 0;
		return (d);
	}

#define	WANT(c) ((buf[0] == c) && (buf[1] == ' '))
	unless (buf = mkline(mnext(f))) {
		fprintf(stderr, "Warning: no delta line in init file.\n");
		error++;
		goto out;
	}
	lines++;
	unless (WANT('R') || WANT('D') || WANT('M')) {
		fprintf(stderr, "Warning: no D/R/M line in init file.\n");
		error++;
		goto out;
	}
	type = buf[0];

	/* D 1.2 93/03/11 00:50:40[-8:00] butthead 2 1	9/2/44 */
	assert((buf[1] == ' ') && isdigit(buf[2]));
	for (s = &buf[2]; *s++ != ' '; );
	if (!d || !d->rev) {
		s[-1] = 0;
		d = sccs_parseArg(d, 'R', &buf[2], 0);
	}
	assert(d);
	t = s;
	while (*s++ != ' ');	/* eat date */
	while (*s++ != ' ');	/* eat time */
	unless (d->sdate) {
		s[-1] = 0;
		d = sccs_parseArg(d, 'D', t, 0);
	}
	t = s;
	while (*s && (*s++ != ' '));	/* eat user */
	unless (d->user) {
		if (s[-1] == ' ') s[-1] = 0;
		d = sccs_parseArg(d, 'U', t, 0);
		if (!d->hostname) d->flags |= D_NOHOST;
	}
	if (patch) {
		if ((*s == '+') && !d->added) {
			d->added = atoi(s+1);
			while (*s && (*s++ != ' '));
			if (*s == '-') d->deleted = atoi(s+1);
		}
		goto skip;	/* skip the rest of this line */
	}
	t = s;
	while (*s && (*s++ != ' '));	/* serial */
	unless (d->serial) {
		if (s[-1] == ' ') s[-1] = 0;
		d->serial = atoi(t);
	}
	t = s;
	while (*s && (*s++ != ' '));	/* pserial */
	unless (d->pserial) {
		if (s[-1] == ' ') s[-1] = 0;
		d->pserial = atoi(t);
	}
	while (*s == ' ') s++;
	t = s;
	while (*s && (*s++ != '/'));	/* added */
	unless (d->added) {		// XXX - test a patch with > 99999 lines in a delta
		if (s[-1] == '/') s[-1] = 0;
		d->added = atoi(t);
	}
	t = s;
	while (*s && (*s++ != '/'));	/* deleted */
	unless (d->deleted) {
		if (s[-1] == '/') s[-1] = 0;
		d->deleted = atoi(t);
	}
	unless (d->same) {
		d->same = atoi(s);
	}

skip:
	buf = mkline(mnext(f)); lines++;

	/* hash hash */
	if (WANT('A')) {
		d = sccs_parseArg(d, 'A', &buf[2], 0);
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Cset file ID */
	if (WANT('B')) {
		unless (d->csetFile) d = sccs_parseArg(d, 'B', &buf[2], 0);
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Cset marker */
	if ((buf[0] == 'C') && !buf[1]) {
		d->flags |= D_CSET;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Dangle marker */
	if ((buf[0] == 'D') && !buf[1]) {
		d->dangling = 1;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/*
	 * Comments are optional and look like:
	 * c added 4.x etc targets
	 */
	while (buf[0] == 'c') {
		char *p;
		if (buf[1] == ' ') p = &buf[2];
		else if (buf[1] == '\0') p = &buf[1];
		else break;
		unless (nocomments) {
			d->comments = addLine(d->comments, strdup(p));
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	if (WANT('E')) {
		/* ignored */
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* date fudges are optional */
	if (WANT('F')) {
		d->dateFudge = atoi(&buf[2]);
		d->date += d->dateFudge;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Includes are optional and are specified as keys.
	 * If there is no sccs* ignore them.
	 */
	while (WANT('i')) {
		if (sc) {
			delta	*e = sccs_findKey(sc, &buf[2]);

			unless (e) {
				fprintf(stderr, "Can't find inc %s in %s\n",
				    &buf[2], sc->sfile);
				sc->state |= S_WARNED;
				error++;
				goto out;
			} else {
				d->include = addSerial(d->include, e->serial);
			}
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* cksums are optional but shouldn't be */
	if (WANT('K')) {
		d = sumArg(d, &buf[2]);
		d->flags |= D_ICKSUM;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* merge deltas are optional */
	if (WANT('M')) {
		if (sc) {
			delta	*e = sccs_findKey(sc, &buf[2]);

			unless (e) {
				fprintf(stderr, "Can't find merge %s in %s\n",
				    &buf[2], sc->sfile);
				sc->state |= S_WARNED;
				error++;
				goto out;
			} else {
				d->merge = e->serial;
			}
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* modes are optional */
	if (WANT('O')) {
		unless (d->mode) d = modeArg(d, &buf[2]);
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* pathnames are optional */
	if (WANT('P')) {
		unless (d->pathname) d = pathArg(d, &buf[2]);
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Random bits are used only for 1.0 deltas in conversion scripts */
	if (WANT('R')) {
		d->random = strnonldup(&buf[2]);
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* symbols are optional */
	while (WANT('S')) {
		syms = addLine(syms, strdup(&buf[2]));
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/*
	 * symbols as keys are optional.  Order of keys is ptag, mtag.
	 * s g - set d->symGraph
	 * s l - set d->symLeaf
	 * s <key> - set d->ptag
	 * s <key> - set d->mtag
	 */
	while (WANT('s')) {
		if (streq(&buf[2], "g")) {
			if (d) d->symGraph = 1;
		} else if (streq(&buf[2], "l")) {
			if (d) d->symLeaf = 1;
		} else if (d && sc) {
			delta	*e = sccs_findKey(sc, &buf[2]);

			assert(e);
			assert(e->symGraph);
			if (d->ptag) {
				d->mtag = e->serial;
			} else {
				d->ptag = e->serial;
			}
			e->symLeaf = 0;
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* text are optional */
	/* Cannot be WANT('T'), buf[1] could be null */
	while (buf[0] == 'T') {
		if (buf[1] == ' ') {
			d->text = addLine(d->text, strdup(&buf[2]));
		}
		d->flags |= D_TEXT;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	if (WANT('V')) {
		unless (streq("1.0", d->rev)) {
			fprintf(stderr, "sccs_getInit: version only on 1.0\n");
		} else if (sc) {
			int	vers = atoi(&buf[3]);

			if (sc->version < vers) sc->version = vers;
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* Excludes are optional and are specified as keys.
	 * If there is no sccs* ignore them.
	 */
	while (WANT('x')) {
		if (sc) {
			delta	*e = sccs_findKey(sc, &buf[2]);

			unless (e) {
				fprintf(stderr, "Can't find ex %s in %s\n",
				    &buf[2], sc->sfile);
				sc->state |= S_WARNED;
				error++;
			} else {
				d->exclude = addSerial(d->exclude, e->serial);
			}
		}
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	if (WANT('X')) {
		d->xflags = strtol(&buf[2], 0, 0); /* hex or dec */
		d->xflags &= ~X_SINGLE;
		d->flags |= D_XFLAGS;
		unless (buf = mkline(mnext(f))) goto out; lines++;
	}

	/* closing line is not optional. */
	if (strcmp("------------------------------------------------", buf)) {
		fprintf(stderr,
		    "Warning: Bad line in init file follows.\n'%s'\n", buf);
		error++;
	}

out:	if (d) {
		unless (hasComments(d)) d->flags |= D_NOCOMMENTS;
		unless (d->pathname) d->flags |= D_NOPATH;
		if (type == 'M') {
			d->flags |= D_META;
			type = 'R';
		}
		d->type = type;
#ifdef	GRAFT_BREAKS_LOD
		if (patch) {
			free(d->rev);
			d->rev = 0;
		}
#endif
	}
	assert(errorp || !error);
	if (errorp) *errorp = error;
	if (linesp) *linesp += lines;
	if (symsp) *symsp = syms;
	return (d);
}

/*
 * Read the p.file and extract and return the old/new/user/{inc, excl} lists.
 *
 * Returns 0 if OK, -1 on error.  Warns on all errors.
 */
int
sccs_read_pfile(char *who, sccs *s, pfile *pf)
{
	int	fsize = size(s->pfile);
	char	*iLst, *xLst;
	char	*mRev = malloc(MAXREV+1);
	char	c1 = 0, c2 = 0, c3 = 0;
	int	e;
	FILE	*tmp;
	char	date[10], time[10], user[40];

	bzero(pf, sizeof(*pf));
	if (!fsize) {
		fprintf(stderr, "Empty p.file %s - aborted.\n", s->pfile);
		return (-1);
	}
	unless (tmp = fopen(s->pfile, "r")) {
		fprintf(stderr, "pfile: can't open %s\n", s->pfile);
		free(mRev);
		return (-1);
	}
	iLst = malloc(fsize);
	xLst = malloc(fsize);
	iLst[0] = xLst[0] = 0;
	e = fscanf(tmp, "%s %s %s %s %s -%c%s -%c%s -%c%s",
	    pf->oldrev, pf->newrev, user, date, time, &c1, iLst, &c2, xLst,
	    &c3, mRev);
	pf->user = strdup(user);
	strcpy(pf->date, date);
	strcat(pf->date, " ");
	strcat(pf->date, time);
	fclose(tmp);
	pf->sccsrev[0] = 0;

	/*
	 * mRev always means there is at least an include -
	 * we already expolded it in get and wrote it out.
	 * There may or may not be an xLst w/ mRev.
	 */
	switch (e) {
	    case 5:		/* No extras, cool. */
		free(iLst); iLst = 0;
		free(xLst); xLst = 0;
		free(mRev); mRev = 0;
		break;
	    case 7:		/* figure out if it was -i or -x or -m */
		free(xLst); xLst = 0;
		free(mRev); mRev = 0;
		if (c1 == 'i') {
			/* do nothing */
		} else if (c1 == 'x') {
			xLst = iLst;
			iLst = 0;
		} else {
			assert(c1 == 'm');
			mRev = iLst;
			iLst = 0;
		}
		break;
	    case 9:		/* Could be -i -x or -i -m  or -x -m */
		assert(c1 != 'm');
		assert(c1 == 'i' || c1 == 'x');
		free(mRev);
		mRev = 0;
		if (c1 == 'x') {
			assert(c2 == 'm');
			mRev = xLst;
			xLst = iLst;
			iLst = 0;
		}
		else {
			assert(c1 == 'i');
			if (c2 == 'm') {
				mRev = xLst;
				xLst = 0;
			} else {
				assert(c2 == 'x');
			}
		}
		break;
	    case 11:		/* has to be -i -x -m */
		assert(c1 == 'i');
		assert(c2 == 'x');
		assert(c3 == 'm');
		break;
	    default:
		free(iLst);
		free(xLst);
		fprintf(stderr,
		    "%s: can't get revision info from %s\n", who, s->pfile);
		return (-1);
	}
	pf->iLst = iLst;
	pf->xLst = xLst;
	pf->mRev = mRev;
	debug((stderr, "pfile(%s, %s, %s, %s, %s, %s, %s)\n",
    	    pf->oldrev, pf->newrev, user, pf->date,
	    notnull(pf->iLst), notnull(pf->xLst), notnull(pf->mRev)));
	return (0);
}

int
isValidHost(char *h)
{
	if (!h || !(*h)) return 0;
	if (streq(h, "localhost") || streq(h, "localhost.localdomain")) {
		return 0;
	}
	/*
	 * XXX TODO: should we do a gethostbyname to verify host name ??
	 * 	     disallow non-alpha numberic character ?
	 */
	return 1;
}

int
isValidUser(char *u)
{
	if (!u || !(*u)) return 0;
	if (sameuser(u, ROOT_USER) || sameuser(u, UNKNOWN_USER)) return 0;
	/*
	 * XXX TODO:
	 * 	a) should we disallow "Guest/guest" as user name ??
	 * 	b) should we check /etc/passwd to verify user name ??
	 * 	c) disallow non-allha numberic character ?
	 */
	return 1;
}

/*
 * Add a metadata delta to the tree.
 * There are no contents, just various fields which are defined in the
 * init file passed in.
 *
 * XXX - there is a lot of duplicated code here, in checkin, in sccs_delta,
 * and probably other places.  I need to make a generic sccs_addDelta()
 * which can handle all of those cases.
 */
int
sccs_meta(char *me, sccs *s, delta *parent, MMAP *iF, int fixDate)
{
	delta	*m;
	int	i, e = 0;
	FILE	*sfile = 0;
	char	*t;
	char	*buf;
	char	**syms;

	unless (sccs_lock(s, 'z')) {
		fprintf(stderr,
		    "meta: can't get lock on %s\n", s->sfile);
		s->state |= S_WARNED;
		debug((stderr, "meta returns -1\n"));
		return (-1);
	}
	unless (iF) {
		sccs_unlock(s, 'z');
		return (-1);
	}
	m = sccs_getInit(s, 0, iF, 1, &e, 0, &syms);
	mclose(iF);
	if (m->rev) free(m->rev);
	m->rev = strdup(parent->rev);
	memcpy(m->r, parent->r, sizeof(m->r));
	m->serial = s->nextserial++;
	m->pserial = parent->serial;
	m->next = s->table;
	s->table = m;
	s->numdeltas++;
	dinsert(s, m, fixDate);
	EACH (syms) {
		addsym(s, m, m, 0, m->rev, syms[i]);
	}
	freeLines(syms, free);
	/*
	 * Do the delta table & misc.
	 */
	unless (sfile = fopen(sccsXfile(s, 'x'), "w")) {
		fprintf(stderr, "admin: can't create %s: ", sccsXfile(s, 'x'));
		perror("");
		sccs_unlock(s, 'z');
		exit(1);
	}
	if (delta_table(s, sfile, 0)) {
abort:		fclose(sfile);
		sccs_unlock(s, 'x');
		return (-1);
	}
	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
		sccs_zputs_init(s, sfile);
	}
	assert(s->state & S_SOPEN);
	while (buf = nextdata(s)) {
		fputdata(s, buf, sfile);
	}
	if (fflushdata(s, sfile)) goto abort;
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) goto abort;
		s->zin = 0;
	}
	fseek(sfile, 0L, SEEK_SET);
	fprintf(sfile, "\001%c%05u\n", BITKEEPER(s) ? 'H' : 'h', s->cksum);
	sccs_close(s); fclose(sfile); sfile = NULL;
	t = sccsXfile(s, 'x');
	if (rename(t, s->sfile)) {
		fprintf(stderr,
		    "%s: can't rename(%s, %s) left in %s\n",
		    me, t, s->sfile, t);
		sccs_unlock(s, 'z');
		exit(1);
	}
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	sccs_unlock(s, 'z');
	return (0);
}

/*
 * Make this delta start off as a .0.
 */
private void
dot0(sccs *s, delta *d)
{
	int	i = 1;
	char	buf[MAXREV];

	do {
		sprintf(buf, "1.0.%d.0", i++);
	} while (rfind(s, buf));
	free(d->rev);
	d->rev = strdup(buf);
	explode_rev(d);
}

/*
 * delta the specified file.
 *
 * Init file implies the old NODEFAULT flag, i.e., if there was an init
 * file, that is the truth.  The truth can be over ridden with a prefilled
 * delta but even that is questionable.
 * Return codes:
 *	-1 = error
 *	-2 = DELTA_AUTO (delta -a) and no diff in gfile
 *	-3 = not DELTA_AUTO or DELTA_FORCE and no diff in gfile (gfile unlinked)
 */
int
sccs_delta(sccs *s,
    	u32 flags, delta *prefilled, MMAP *init, MMAP *diffs, char **syms)
{
	FILE	*sfile = 0;	/* the new s.file */
	int	i, free_syms = 0, error = 0;
	char	*t;
	delta	*d = 0, *p, *n = 0;
	char	*rev, *tmpfile = 0;
	int	added = 0, deleted = 0, unchanged = 0;
	int	locked;
	pfile	pf;

	assert(s);
	debug((stderr, "delta %s %x\n", s->gfile, flags));
	if (flags & NEWFILE) mksccsdir(s->sfile);
	bzero(&pf, sizeof(pf));
	unless (locked = sccs_lock(s, 'z')) {
		fprintf(stderr,
		    "delta: can't get write lock on %s\n", s->sfile);
		repository_lockers(s->proj);
		error = -1; s->state |= S_WARNED;
out:
		if (prefilled) sccs_freetree(prefilled);
		if (sfile) fclose(sfile);
		if (diffs) mclose(diffs);
		free_pfile(&pf);
		if (free_syms) freeLines(syms, free); 
		if (tmpfile  && !streq(tmpfile, DEVNULL_WR)) unlink(tmpfile);
		if (locked) sccs_unlock(s, 'z');
		debug((stderr, "delta returns %d\n", error));
		return (error);
	}
#define	OUT	{ error = -1; s->state |= S_WARNED; goto out; }
#define	WARN	{ error = -1; goto out; }

	if (init) {
		int	e;

		if (syms) {
			fprintf(stderr, "delta: init or symbols, not both\n");
			init = 0;  /* prevent double free */
			OUT;
		}
		prefilled = sccs_getInit(s,
		    prefilled, init, flags&DELTA_PATCH, &e, 0, &syms);
		/*
		 * Normally, the syms list is passed in by the caller
		 * and we let the caller free it.
		 * Here we are getting the syms list from the init file,
		 * i.e this syms list is unknown to the caller.
		 * thus we must free the syms list before we return.
		 */
		if (syms) free_syms = 1;
		unless (prefilled && !e) {
			fprintf(stderr, "delta: bad init file\n");
			OUT;
		}
		debug((stderr, "delta got prefilled %s\n", prefilled->rev));
		if (flags & DELTA_PATCH) {
			if (prefilled->pathname &&
			    streq(prefilled->pathname, "ChangeSet")) {
				s->state |= S_CSET;
		    	}
			if (prefilled->flags & D_TEXT) {
				if (s->text) {
					freeLines(s->text, free);
					s->text = 0;
				}
				EACH(prefilled->text) {
					s->text = addLine(s->text,
						strnonldup(prefilled->text[i]));
				}
			}
			unless (flags & NEWFILE) {
				/* except the very first delta   */
				/* all rev are subject to rename */
				free(prefilled->rev);
				prefilled->rev = 0;

				/*
				 * If we have random bits, we are the root of
				 * some other file, so make our rev start at
				 * .0
				 */
				if (prefilled->random) dot0(s, prefilled);
			}
		}
	}

	if ((flags & NEWFILE) || (!HAS_SFILE(s) && HAS_GFILE(s))) {
		int rc;

		rc = checkin(s, flags, prefilled, init != 0, diffs, syms);
		if (free_syms) freeLines(syms, free); 
		return rc;
	}

	unless (HAS_SFILE(s) && HASGRAPH(s)) {
		fprintf(stderr, "delta: %s is not an SCCS file\n", s->sfile);
		s->state |= S_WARNED;
		OUT;
	}

	unless (HAS_PFILE(s)) {
		if (WRITABLE(s)) {
			fprintf(stderr,
			    "delta: %s writable but not checked out?\n",
			    s->gfile);
			s->state |= S_WARNED;
			OUT;
		} else {
			verbose((stderr,
			    "delta: %s is not locked.\n", s->sfile);
			goto out;
		}
	}

	unless (WRITABLE(s) || diffs) {
		fprintf(stderr,
		    "delta: %s is locked but not writable.\n", s->gfile));
		s->state |= S_WARNED;
		OUT;
	}

	if (HAS_GFILE(s) && diffs) {
		fprintf(stderr,
		    "delta: diffs or gfile for %s, but not both.\n",
		    s->gfile);
		s->state |= S_WARNED;
		OUT;
	}

	/* Refuse to make deltas to 100% dangling files */
	if (s->tree->dangling && !(flags & DELTA_PATCH)) {
		fprintf(stderr,
		    "delta: entire file %s is dangling, abort.\n", s->gfile);
		OUT;
	}

	/*
	 * OK, checking done, start the delta.
	 */
	if (sccs_read_pfile("delta", s, &pf)) OUT;
	unless (d = findrev(s, pf.oldrev)) {
		fprintf(stderr,
		    "delta: can't find %s in %s\n", pf.oldrev, s->gfile);
		OUT;
	}

	/*
	 * Catch p.files with bogus revs.
	 */
	rev = d->rev;
	p = sccs_getedit(s, &rev);
	assert(p);	/* we just found it above */
	unless (streq(rev, pf.newrev)) {
		fprintf(stderr,
		    "delta: invalid nextrev %s in p.file, using %s instead.\n",
		    pf.newrev, rev);
		strcpy(pf.newrev, rev);
	}

	if (d->dangling) {
		if (diffs && !(flags & DELTA_PATCH)) {
			fprintf(stderr,
			    "delta: dangling deltas may not be "
			    "combined with diffs\n");
			OUT;
		}
		while (d->next && (d->dangling || TAG(d))) d = d->next;
		assert(d->next);
		strcpy(pf.oldrev, d->rev);
	}

	if (pf.mRev || pf.xLst || pf.iLst) flags |= DELTA_FORCE;
	debug((stderr, "delta found rev\n"));
	if (diffs) {
		debug((stderr, "delta using diffs passed in\n"));
	} else {
		switch (diff_g(s, &pf, &tmpfile)) {
		    case 1:		/* no diffs */
			if (flags & DELTA_FORCE) {
				break;     /* forced 0 sized delta */
			}
			unless (flags & SILENT) {
				fprintf(stderr,
				    "Clean %s (no diffs)\n", s->gfile);
			}
			if (flags & DELTA_AUTO) {
				error = -2;
				goto out;
			}
			unlink(s->pfile);
			unless (flags & DELTA_SAVEGFILE) unlinkGfile(s);
			error = -3;
			goto out;
		    case 0:		/* diffs */
			break;
		    default: OUT;
		}
		/* We prefer binary mode, but win32 GNU diff used text mode */ 
		unless (tmpfile && (diffs = mopen(tmpfile, "t"))) {
			fprintf(stderr,
			    "delta: can't open diff file %s\n", tmpfile);
			OUT;
		}
	}
	if (flags & PRINT) {
		fprintf(stdout, "==== Changes to %s ====\n", s->gfile);
		if (BINPOOL(s)) {
			fprintf(stdout, "Binary files differ.\n");
		} else {
			fwrite(diffs->mmap, diffs->size, 1, stdout);
		}
		fputs("====\n\n", stdout);
	}
	/*
	 * Add a new delta table entry.	 We'll come back and fix up
	 * the add/del/unch lines later.
	 * In conflicts between prefilled and the pfile, prefilled wins.
	 * prefilled can have date, user, host, path, comments, and/or rev.
	 */
	if (prefilled) {
		n = prefilled;
		prefilled = 0;
	} else {
		n = calloc(sizeof(*n), 1);
		assert(n);
	}
	if (pf.iLst) {
		assert(!n->include);
		n->include = getserlist(s, 0, pf.iLst, &error);
	}
	if (pf.xLst) {
		assert(!n->exclude);
		n->exclude = getserlist(s, 0, pf.xLst, &error);
	}
	if (pf.mRev) {
		delta	*e = findrev(s, pf.mRev);

		unless (e) {
			fprintf(stderr,
			    "delta: no such rev %s in %s\n", pf.mRev, s->sfile);
		    	OUT;
		}
		if (n->merge && (e->serial != n->merge)) {
			fprintf(stderr,
			    "delta: conflicting merge revs: %s %s\n",
			    n->rev, e->rev);
			OUT;
		}
		n->merge = e->serial;
	}
	if (error) OUT;
	n = sccs_dInit(n, 'D', s, init != 0);
	updMode(s, n, d);

	if (!n->rev) {
		if (pf.sccsrev[0]) {
			n->rev = pf.sccsrev;
			explode_rev(n);
			n->rev = strdup(pf.newrev);
		} else {
			n->rev = strdup(pf.newrev);
			explode_rev(n);
		}
	} else {
		explode_rev(n);
	}
	n->serial = s->nextserial++;
	n->next = s->table;
	s->table = n;
	assert(d);
	n->pserial = d->serial;
	if (!hasComments(n) && !init &&
	    !(flags & DELTA_DONTASK) && !(n->flags & D_NOCOMMENTS)) {
		/*
		 * If they told us there should be a c.file, use it.
		 * Else look for one and if found, prompt and use it.
		 * Else ask for comments.
		 */
		if (flags & DELTA_CFILE) {
			if (comments_readcfile(s, 0, n)) OUT;
		} else switch (comments_readcfile(s, 1, n)) {
		    case -1: /* no c.file found */
			if (sccs_getComments(s->gfile, pf.newrev, n)) {
				error = -4;
				goto out;
			}
			break;
		    case -2: /* aborted in prompt */
			error = -4;
			goto out;
		}
	}
	dinsert(s, n, !(flags & DELTA_PATCH));
	s->numdeltas++;

	/* Uses n->parent, has to be after dinsert() */
	if ((flags & DELTA_MONOTONIC) && !(sccs_xflags(n) & X_MONOTONIC)) {
		n->xflags |= sccs_xflags(n->parent);
		n->xflags |= X_MONOTONIC;
		n->flags |= D_XFLAGS;
	}

	EACH (syms) {
		addsym(s, n, n, !(flags&DELTA_PATCH), n->rev, syms[i]);
	}

	if (BINPOOL(s)) {
		if (!(flags & DELTA_PATCH) && bp_delta(s, n)) {
			fprintf(stderr, "binpool: delta of %s failed\n",
			    s->gfile);
			return (-1);
		}
		added = s->added = n->added;
	}

	/*
	 * Do the delta table & misc.
	 */
	unless (sfile = fopen(sccsXfile(s, 'x'), "w")) {
		fprintf(stderr, "delta: can't create %s: ", sccsXfile(s, 'x'));
		perror("");
		OUT;
	}

	/*
	 * If the new delta is a top-of-trunk, update the xflags
	 * This is needed to maintain the xflags invariant:
	 * s->state should always match sccs_xflags(tot);
	 * where "tot" is the top-of-trunk delta.
 	 */
	if (init && (flags&DELTA_PATCH) && (n->flags & D_XFLAGS)) {
		if (n == sccs_top(s)) s->xflags = n->xflags;
	}

	if (delta_table(s, sfile, 1)) {
		fclose(sfile); sfile = NULL;
		sccs_unlock(s, 'x');
		goto out;	/* not OUT - we want the warning */
	}

	assert(d);
	unless (BINPOOL(s)) {
		if (delta_body(s, n, diffs,
			sfile, &added, &deleted, &unchanged)) {
			OUT;
		}
	} else if (S_ISLNK(n->mode)) {
		u8 *t;
		/*
		 * if symlink, check sum the symlink path
		 */
		for (t = n->symlink; *t; t++) s->dsum += *t;
	}
	if (end(s, n, sfile, flags, added, deleted, unchanged)) {
		fclose(sfile); sfile = NULL;
		sccs_unlock(s, 'x');
		WARN;
	}

	sccs_close(s), fclose(sfile), sfile = NULL;
	unless (flags & DELTA_SAVEGFILE)  {
		if (unlinkGfile(s)) {				/* Careful. */
			fprintf(stderr, "delta: cannot unlink %s\n", s->gfile);
			OUT;
		}
	}
	t = sccsXfile(s, 'x');
	if (rename(t, s->sfile)) {
		fprintf(stderr,
		    "delta: can't rename(%s, %s) left in %s\n",
		    t, s->sfile, t);
		OUT;
	}
	unlink(s->pfile);
	comments_cleancfile(s->gfile);
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	if (BITKEEPER(s) && !(flags & DELTA_NOPENDING)) {
		 updatePending(s);
	}
	goto out;
#undef	OUT
}

/*
 * works for 32 bit unsigned only
 */
private void
fit(char *buf, unsigned int i)
{
	int	j;
	u32	f;
	static	char *s[] = { "K", "M", "G", 0 };
	if (i < 100000) {
		sprintf(buf, "%05d\n", i);
		return;
	}
	for (j = 0, f = 1000; s[j]; j++, f *= 1000) {
		sprintf(buf, "%04u%s", (i+(f-1))/f, s[j]);
		if (strlen(buf) == 5) return;
	}
	sprintf(buf, "E2BIG");
	return;
}

void
sccs_fitCounters(char *buf, int a, int d, int s)
{
	/* ^As 12345/12345/12345\n
	 *  012345678901234567890
	 */
	strcpy(buf, "\001s ");
	fit(&buf[3], a);
	buf[8] = '/';
	fit(&buf[9], d);
	buf[14] = '/';
	fit(&buf[15], s);
	buf[20] = '\n';
	buf[21] = 0;
}

int	do_fsync = -1;

/*
 * Print the summary and go and fix up the top.
 */
private int
end(sccs *s, delta *n, FILE *out, int flags, int add, int del, int same)
{
	char	buf[100];

	/*
	 * Flush and make sure we have disk space.
	 */
	if (fflushdata(s, out)) return (-1);
	unless (flags & SILENT) {
		int	lines = count_lines(n->parent) - del + add;

		fprintf(stderr, "%s revision %s: ", s->gfile, n->rev);
		fprintf(stderr, "+%d -%d = %d\n", add, del, lines);
	}
	n->added = add;
	n->deleted = del;
	n->same = same;

	/*
	 * Now fix up the checksum and summary.
	 */
	fseek(out, 8L, SEEK_SET);
	sprintf(buf, "\001s %05d/%05d/%05d\n", add, del, same);
	if (strlen(buf) > 21) {
		unless (BITKEEPER(s)) {
			fprintf(stderr, "%s: file too large\n", s->gfile);
			exit(1);
		}
		sccs_fitCounters(buf, add, del, same);
	}
	fputmeta(s, buf, out);
	if (BITKEEPER(s)) {
		if ((add || del || same) && (n->flags & D_ICKSUM)) {
			delta	*z = getCksumDelta(s, n);

			/* we allow bad symlink chksums if they are zero;
			 * it's a bug in old binaries.
			 */
			if ((S_ISLNK(n->mode) && n->sum) &&
			    (!z || (s->dsum != z->sum))) {
				fprintf(stderr,
				    "%s: bad delta checksum: %u:%d for %s\n",
				    s->sfile, s->dsum,
				    z ? z->sum : -1, n->rev);
				s->bad_dsum = 1;
			}
		}
		unless (n->flags & D_ICKSUM) {
			/*
			 * XXX: would like "if cksum is same as parent"
			 * but we can't do that because we use the inc/ex
			 * in getCksumDelta().
			 */
			if (add || del || 
			    n->include || n->exclude ||
			    S_ISLNK(n->mode)) {
				n->sum = s->dsum;
			} else {
				n->sum = almostUnique();
			}
#if 0
Breaks up citool

			/*
			 * XXX - should this be fatal for cset?
			 * It probably should but only if the
			 * full keys are the same.
			 */
			if (n->parent && (n->parent->sum == n->sum)) {
				fprintf(stderr,
				    "%s: warning %s & %s have same sum\n",
				    s->sfile, n->parent->rev, n->rev);
			}
#endif
		}
		fseek(out, s->sumOff, SEEK_SET);
		sprintf(buf, "%05u", n->sum);
		fputmeta(s, buf, out);
	}
	fseek(out, 0L, SEEK_SET);
	fprintf(out, "\001%c%05u\n", BITKEEPER(s) ? 'H' : 'h', s->cksum);
	if (flushFILE(out)) {
		perror(s->sfile);
		s->io_warned = 1;
		return (-1);
	}
	if (do_fsync == -1) do_fsync = bk_fsync();
	if (do_fsync) fsync(fileno(out));
	return (0);
}

private void
mkTag(char *rev, char *revM, pfile *pf, char *path, char tag[])
{
	/*
	 * 1.0 => create (or reverse create in a reverse pacth )
	 * /dev/null => delete (i.e. sccsrm)
	 */
	if (streq(rev, "1.0") || streq(path, DEVNULL_RD)) {
		sprintf(tag, "%s", "/dev/null");
	} else {
		strcpy(tag, rev);
		if (revM) { 
			strcat(tag, "+");
			strcat(tag, revM);
		}
		if (pf) {
			if (pf->iLst) {
				strcat(tag, "+");
				strcat(tag, pf->iLst);
			}
			if (pf->xLst) {
				strcat(tag, "-");
				strcat(tag, pf->xLst);
			}
		}
		strcat(tag, "/");
		strcat(tag, path);
	}
}


/*
 * helper function for sccs_diffs
 */
private void
mkDiffHdr(u32 kind, char tag[], char *buf, FILE *out)
{
	char	*marker, *date;

	unless (kind & (DF_UNIFIED|DF_CONTEXT|DF_GNUp)) {
		fputs(buf, out);
		return; 
	}

	/*
	 * Fix the file names.
	 *
	 * For bk diff, the file name should be 
	 * +++ bk.sh 1.34  Thu Jun 10 21:22:08 1999
	 *
	 */
	if (strneq(buf, "+++ ", 4) || strneq(buf, "--- ", 4)) {
		date = strchr(buf, '\t'); assert(date);
		buf[3] = 0; marker = buf;
		fprintf(out, "%s %s%s", marker, tag, date);
	} else	fputs(buf, out);
}

/*
 * set_comments(), diffComments():
 * Print out the comments for the diff range passed in.
 */
private	char	**h;

private void
set_comments(sccs *s, delta *d)
{
	char	*buf;
	char	*p;

	buf = sccs_prsbuf(s, d, 0, 
	    ":D: :T: :P:$if(:HT:){@:HT:} :I: +:LI: -:LD:\n"
	    "$each(:C:){   (:C:)\n}");
	if (buf &&
	    (p = strchr(buf, '\n')) && !streq(p, "\n   Auto merged\n")) {
		h = addLine(h, strdup(buf));
	}
}

private void
diffComments(u32 kind, FILE *out, sccs *s, char *lrev, char *rrev)
{
	int	i;

	unless (rrev) rrev = "+";
	if (streq(rrev, "edited")) rrev = "+";	/* XXX - what about edit??? */
	set_diff(s, set_get(s, rrev), set_get(s, lrev), set_comments);
	if ((kind & DF_IFDEF) && h) fputs("#ifdef !!COMMENTS!!\n", out);
	EACH(h) {
		fputs(h[i], out);
	}
	if (h) {
		if (kind & DF_IFDEF) {
			fputs("#endif !!COMMENTS!!\n", out);
		} else {
			fputs("\n", out);
		}
		freeLines(h, free);
		h = 0;
	}
}

private int
doDiff(sccs *s, u32 flags, u32 kind, char *leftf, char *rightf,
	FILE *out, char *lrev, char *rrev, char *ltag, char *rtag)
{
	FILE	*diffs = 0;
	char	diffFile[MAXPATH];
	char	buf[MAXLINE];
	char	spaces[80];
	int	first = 1;
	char	*error = "";

	if (kind & DF_SDIFF) {
		int	i, c;
		char	*columns = 0;

		unless (columns = getenv("COLUMNS")) columns = "80";
		c = atoi(columns);
		for (i = 0; i < c/2 - 18; ) spaces[i++] = '=';
		spaces[i] = 0;
		sprintf(buf, "bk sdiff -w%s '%s' '%s'", columns, leftf, rightf);
		diffs = popen(buf, "r");
		if (!diffs) return (-1);
		diffFile[0] = 0;
	} else {
		strcpy(spaces, "=====");
		unless (bktmp(diffFile, "diffs")) return (-1);
		diff(leftf, rightf, kind, diffFile);
		diffs = fopen(diffFile, "rt");
	}
	if (WRITABLE(s) && !EDITED(s)) {
		error = " (writable without lock!) ";
	}
	while (fnext(buf, diffs)) {
		if (first) {
			if ((flags & DIFF_HEADER) && (kind & DF_IFDEF)) {
				fprintf(out, "#ifdef !!HEADER!!\n");
				fprintf(out, "< %s %s\n", s->gfile, lrev);
				fprintf(out, "> %s %s\n", s->gfile, rrev);
				fprintf(out, "#endif !!HEADER!!\n");
			} else if (flags & DIFF_HEADER) {
				fprintf(out, "%s %s %s vs %s%s %s\n",
				   spaces, s->gfile, lrev, rrev, error, spaces);
			}
			if (flags & DIFF_COMMENTS) {
				diffComments(kind, out, s, lrev, rrev);
			}
			unless (flags & DIFF_HEADER) fprintf(out, "\n");
			first = 0;
			mkDiffHdr(kind, ltag, buf, out);
			unless (fnext(buf, diffs)) break;
			mkDiffHdr(kind, rtag, buf, out);
		} else {
			fputs(buf, out);
		}
	}
	/* XXX - gross but useful hack to get spacers */
	if ((flags & DIFF_COMMENTS) && !(kind & DF_IFDEF) && !first) {
		fprintf(out, "\n");
	}
	if (kind & DF_SDIFF) {
		pclose(diffs);
	} else {
		fclose(diffs);
		unlink(diffFile);
	}
	return (0);
}


/*
 * Given r1, r2, compute rev1, rev2
 * r1 & r2 are user specified rev; can be incomplete 
 */
private int
mapRev(sccs *s, u32 flags, char *r1, char *r2,
			char **rev1, char **rev1M, char **rev2, pfile *pf)
{
	char *lrev, *lrevM = 0, *rrev;

	if (r1 && r2) {
		if (r1 == r2) { /* r1 == r2 means diffs against parent(s) */
			if (sccs_parent_revs(s, r2, &lrev, &lrevM)) {
				return (-1);
			}
		} else {
			lrev = r1;
		}
		rrev = r2;
	} else if (HAS_PFILE(s)) {
		if (r1) {
			lrev = r1;
		} else {
			if (sccs_read_pfile("diffs", s, pf)) return (-1);
			lrev = pf->oldrev;
			lrevM = pf->mRev;
		}
		rrev = "edited";
	} else if (r1) {
		lrev = r1;
		rrev = 0;
	} else {
		unless (HAS_GFILE(s)) {
			verbose((stderr,
			    "diffs: %s not checked out.\n", s->gfile));
			s->state |= S_WARNED;
			return (-1);
		}
		lrev = 0;
		rrev = "?";
	}
	unless (sccs_findrev(s, lrev)) {
		return (-2);
	}
	if (r2 && !sccs_findrev(s, r2)) {
		return (-3);
	}
	if (!rrev) rrev = findrev(s, 0)->rev;
	if (!lrev) lrev = findrev(s, 0)->rev;
	if (streq(lrev, rrev)) return (-3);
	*rev1 = lrev; *rev1M = lrevM, *rev2 = rrev; 
	return 0;
}

private char *
getHistoricPath(sccs *s, char *rev)
{
	delta *d;

	d = sccs_findrev(s, rev);
	if (d && d->pathname) {
		return (strdup(d->pathname));
	} else {
		return (proj_relpath(s->proj, s->gfile));
	}
}

private int
mkDiffTarget(sccs *s,
	char *rev, char *revM, u32 flags, char *target, pfile *pf)
{
	char	*pat;

	if (streq(rev, "1.0")) {
		strcpy(target, DEVNULL_RD);
		return (0);
	}
	pat = aprintf("%s-%s", basenm(s->gfile), rev);
	bktmp(target, pat);
	free(pat);

	if ((streq(rev, "edited") || streq(rev, "?")) && !findrev(s, rev)) {
		assert(HAS_GFILE(s));
		if (S_ISLNK(s->mode)) {
			char	buf[MAXPATH];
			FILE	*f;
			int	len;
			
			len = readlink(s->gfile, buf, sizeof(buf));
			if (len <= 0) return (-1);
			buf[len] = 0; /* stupid readlink... */
			f = fopen(target, "w");
			unless (f) return (-1);
			fprintf(f, "SYMLINK -> %s\n", buf);
			fclose(f);
		} else {
			unlink(target);
			strcpy(target, s->gfile);
		}
	} else if (sccs_get(s, rev, revM, pf ? pf->iLst : 0,
		    pf ? pf->xLst : 0, flags|SILENT|PRINT|GET_DTIME, target)) {
		return (-1);
	}
	return (0);
}

private int
normal_diff(sccs *s, char *lrev, char *lrevM,
	char *rrev, u32 flags, u32 kind, FILE *out, pfile *pf)
{
	char	*lpath = 0, *rpath = 0;
	int	rc = -1;
	char	lfile[MAXPATH], rfile[MAXPATH];
	char	ltag[MAXPATH],	rtag[MAXPATH];

	/*
	 * Create the lfile & rfile for diff
	 */
	if (mkDiffTarget(s, lrev, lrevM, flags, lfile, pf)) {
		goto done;
	}
	if (mkDiffTarget(s, rrev, NULL,  flags, rfile, 0 )) {
		goto done;
	}

	lpath = getHistoricPath(s, lrev); assert(lpath);
	rpath = getHistoricPath(s, rrev); assert(rpath);

	/*
	 * make the tag string to label the diff output, e.g.
	 *
	 * +++ bk.sh 1.34  Thu Jun 10 21:22:08 1999
	 */
	mkTag(lrev, lrevM, pf, lpath, ltag);
	mkTag(rrev, NULL, NULL, rpath, rtag);

	/*
	 * Now diff the lfile & rfile
	 */
	rc = doDiff(s, flags, kind, lfile, rfile, out, lrev, rrev, ltag, rtag);
done:	unless (streq(lfile, DEVNULL_RD)) unlink(lfile);
	unless (streq(rfile, s->gfile) || streq(rfile, DEVNULL_RD)) unlink(rfile);
	if (lpath) free(lpath);
	if (rpath) free(rpath);
	return (rc);
}

/*
 * diffs - diff the gfile or the specified (or implied) rev
 */
int
sccs_diffs(sccs *s, char *r1, char *r2, u32 flags, u32 kind, FILE *out)
{
	char	*lrev, *lrevM, *rrev;
	pfile	pf;
	int	rc = 0;
	
	bzero(&pf, sizeof(pf));
	GOODSCCS(s);

	/*
	 * Figure out which revision the user want.
	 * Translate r1 => lrev, r2 => rrev.
	 */
	if (rc = mapRev(s, flags, r1, r2, &lrev, &lrevM, &rrev, &pf)) {
		goto done;
	}

	rc = normal_diff(s, lrev, lrevM, rrev, flags, kind, out, &pf);

done:	free_pfile(&pf);
	return (rc);
}

/*
 * Include the kw2val() hash lookup function.  This is dynamically
 * generated during the build by kwextract.pl and gperf (see the
 * Makefile).
 */
#include "kw2val_lookup.c"

#define	notKeyword -1
#define	nullVal    0
#define	strVal	   1

/*
 * Given a PRS DSPEC keyword, get the associated string value
 * If out is non-null print to out
 * If vbuf is non-null, append the value to vbuf (vbuf size must be >= 1K)
 * If kw is not a keyword, return notKeyword
 * If kw has null value, return nullVal
 * Otherwise return strVal
 * Keyword definition is compatible with open group SCCS
 * This function may call itself recursively
 * kw2val() and fprintDelta() are mutually recursive
 *
 * XXX WARNING: If you add new keyord to this function, do _not_ print
 * to out directly, you _must_ use the fc()/fd()/fx()/f5d()/fs()
 * macros.
 *
 * ALSO: you must follow the format of the main switch statement below.
 * Each leg MUST look like:
 *     <tab>case KW_enumname:
 * followed by a comment containing the keyword name.  If a block follows,
 * the comment MUST appear before the {.
 */
int
kw2val(FILE *out, char ***vbuf, char *kw, int len, sccs *s, delta *d)
{
	struct kwval *kwval;
	char	*p, *q;
#define	KW(x)	kw2val(out, vbuf, x, strlen(x), s, d)
#define	fc(c)	show_d(s, out, vbuf, "%c", c)
#define	fd(n)	show_d(s, out, vbuf, "%d", n)
#define	fx(n)	show_d(s, out, vbuf, "0x%x", n)
#define	f5d(n)	show_d(s, out, vbuf, "%05d", n)
#define	fs(str)	show_s(s, out, vbuf, str, -1)
#define	fsd(d)	show_s(s, out, vbuf, d.dptr, d.dsize)

	/*
	 * Allow keywords of the form "word|rev"
	 * to mean "word" for revision "rev".
	 */
	for (p = kw, q = p+len; (*p != '|') && (p < q); ++p) ;
	if (p < q) {
		delta	*e;
		char	*rev;

		p++;
		rev = strndup(p, len - (p-kw));
		e = sccs_findrev(s, rev);
		free(rev);
		unless (e) return (nullVal);
		len = p - 1 - kw;
		d = e;
	}
	kwval = kw2val_lookup(kw, len);
	unless (kwval) return notKeyword;

	unless (out || vbuf) return (nullVal);
	switch (kwval->kwnum) {
	case KW_each: /* each */ {
		dspec_printeach(s, out, vbuf);
		return (strVal);
	}
	case KW_eachline: /* line */ {
		dspec_printline(s, out, vbuf);
		return (strVal);
	}
	case KW_Dt: /* Dt */ {
		/* :Dt: = :DT::I::D::T::P::DS::DP: */
		KW("DT"); fc(' '); KW("I"); fc(' ');
		KW("D"); fc(' '); KW("T"); fc(' ');
		KW("P"); fc(' '); KW("DS"); fc(' ');
		KW("DP");
		return (strVal);
	}
	case KW_DL: /* DL */ {
		/* :DL: = :LI:/:LD:/:LU: */
		KW("LI"); fc('/'); KW("LD"); fc('/'); KW("LU");
		return (strVal);
	}

	case KW_I: /* I */ {
		fs(d->rev);
		return (strVal);
	}

	case KW_D: /* D */ {
		/* date */
		KW("Dy"); fc('/'); KW("Dm"); fc('/'); KW("Dd");
		return (strVal);
	}

	case KW_D_: /* D_ */ {
		/* date */
		KW("Dy"); fc('-'); KW("Dm"); fc('-'); KW("Dd");
		return (strVal);
	}

	case KW_T: /* T */ {
		/* Time */
		/* XXX TODO: need to figure out when to print time zone info */
		KW("Th"); fc(':'); KW("Tm"); fc(':'); KW("Ts");
		return (strVal);
	}

	case KW_DI: /* DI */ {
		/* serial number of included and excluded deltas.
		 * :DI: = :Dn:/:Dx:/:Dg: in ATT, we do :Dn:/:Dx:
		 */
		unless (d->include || d->exclude) return (nullVal);
		KW("Dn");
		if (d->exclude) {
			if (d->include) fc('/');
			KW("Dx");
		}
		return (strVal);
	}

	case KW_RI: /* RI */ {
		/* rev number of included and excluded deltas.
		 * :DR: = :Rn:/:Rx:
		 */
		unless (d->include || d->exclude) return (nullVal);
		KW("Rn");
		if (d->exclude) {
			if (d->include) fc('/');
			KW("Rx");
		}
		return (strVal);
	}

	case KW_Dn: /* Dn */ {
		/* serial number of included deltas */
		int i;

		unless (d->include) return (nullVal);
		fc('+');
		EACH(d->include) {
			if (i > 1) fc(',');
			fd(d->include[i]);
		}
		return (strVal);
	}

	case KW_Dx: /* Dx */ {
		/* serial number of excluded deltas */
		int i;

		unless (d->exclude) return (nullVal);
		fc('-');
		EACH(d->exclude) {
			if (i > 1) fc(',');
			fd(d->exclude[i]);
		}
		return (strVal);
	}

	case KW_Dg: /* Dg */ {
		/* ignored delta - definition unknow, not implemented	*/
		/* always return null					*/
		return (nullVal);
	}

	/* rev number of included deltas */
	case KW_Rn: /* Rn */ {
		int	i;
		delta	*r;

		unless (d->include) return (nullVal);
		fc('+');
		EACH(d->include) {
			if (i > 1) fc(',');
			r = sfind(s, d->include[i]);
			fs(r->rev);
		}
		return (strVal);
	}

	/* rev number of excluded deltas */
	case KW_Rx: /* Rx */ {
		int	i;
		delta	*r;

		unless (d->exclude) return (nullVal);
		fc('-');
		EACH(d->exclude) {
			if (i > 1) fc(',');
			r = sfind(s, d->exclude[i]);
			fs(r->rev);
		}
		return (strVal);
	}

	/* Lose this in 2008 */
	case KW_W: /* W */ {
		/* a form of "what" string */
		/* :W: = :Z::M:\t:I: */
		KW("Z"); KW("M"); fc('\t'); KW("I");
		return (strVal);
	}

	case KW_A: /* A */ {
		/* a form of "what" string */
		/* :A: = :Z::Y: :M:I:Z: */
		KW("Z"); KW("Y"); fc(' ');
		KW("M"); KW("I"); KW("Z");
		return (strVal);
	}

	case KW_LI: /* LI */ {
		/* lines inserted */
		fd(d->added);
		return (strVal);
	}

	case KW_LD: /* LD */ {
		/* lines deleted */
		fd(d->deleted);
		return (strVal);
	}

	case KW_LU: /* LU */ {
		/* lines unchanged */
		fd(d->same);
		return (strVal);
	}

	case KW_Li: /* Li */ {
		/* lines inserted */
		f5d(d->added);
		return (strVal);
	}

	case KW_Ld: /* Ld */ {
		/* lines deleted */
		f5d(d->deleted);
		return (strVal);
	}

	case KW_Lu: /* Lu */ {
		/* lines unchanged */
		f5d(d->same);
		return (strVal);
	}

	case KW_DT: /* DT */ {
		/* delta type */
		if ((d->type == 'R') && d->symGraph) {
			fc('T');
		} else {
			fc(d->type);
		}
		return (strVal);
	}

	case KW_R: /* R */ {
		/* release */
		for (p = d->rev; *p && *p != '.'; )
			fc(*p++);
		return (strVal);
	}

	case KW_L: /* L */ {
		/* level */
		for (p = d->rev; *p && *p != '.'; p++); /* skip release field */
		for (p++; *p && *p != '.'; )
			fc(*p++);
		return (strVal);
	}

	case KW_B: /* B */ {
		/* branch */
		for (p = d->rev; *p && *p != '.'; p++); /* skip release field */
		for (p++; *p && *p != '.'; p++);	/* skip branch field */
		for (p++; *p && *p != '.'; )
			fc(*p++);
		return (strVal);
	}

	case KW_S: /* S */ {
		/* sequence */
		for (p = d->rev; *p && *p != '.'; p++); /* skip release field */
		for (p++; *p && *p != '.'; p++);	/* skip branch field */
		for (p++; *p && *p != '.'; p++);	/* skip level field */
		for (p++; *p; )
			fc(*p++);
		return (strVal);
	}

	case KW_Dy: /* Dy */ {
		/* year */
		if (d->sdate) {
			char	val[512];

			if (YEAR4(s)) {
				q = &val[2];
			} else {
				q = val;
			}
			for (p = d->sdate; *p && *p != '/'; )
				*q++ = *p++;
			*q = '\0';
			if (YEAR4(s)) {
				if (atoi(&val[2]) <= 68) {
					val[0] = '2';
					val[1] = '0';
				} else {
					val[0] = '1';
					val[1] = '9';
				}
			}
			fs(val);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_Dm: /* Dm */ {
		/* month */
		if (d->sdate) {
			for (p = d->sdate; *p && *p != '/'; p++);
			for (p++; *p && *p != '/'; )
				fc(*p++);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DM: /* DM */ {
		/* month in Jan, Feb format */
		if (d->sdate) {
			for (p = d->sdate; *p && *p != '/'; p++);
			switch (atoi(++p)) {
			    case 1: fs("Jan"); break;
			    case 2: fs("Feb"); break;
			    case 3: fs("Mar"); break;
			    case 4: fs("Apr"); break;
			    case 5: fs("May"); break;
			    case 6: fs("Jun"); break;
			    case 7: fs("Jul"); break;
			    case 8: fs("Aug"); break;
			    case 9: fs("Sep"); break;
			    case 10: fs("Oct"); break;
			    case 11: fs("Nov"); break;
			    case 12: fs("Dec"); break;
			    default: fs("???"); break;
			}
			return (strVal);
		}
		return (nullVal);
	}

	case KW_Dd: /* Dd */ {
		/* day */
		if (d->sdate) {
			for (p = d->sdate; *p && *p != '/'; p++);
			for (p++; *p && *p != '/'; p++);
			for (p++; *p && *p != ' '; )
				fc(*p++);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_Th: /* Th */ {
		/* hour */
		if (d->sdate)
		{
			for (p = d->sdate; *p && *p != ' '; p++);
			for (p++; *p && *p != ':'; )
				fc(*p++);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_Tm: /* Tm */ {
		/* minute */
		if (d->sdate) {
			for (p = d->sdate; *p && *p != ' '; p++);
			for (p++; *p && *p != ':'; p++);
			for (p++; *p && *p != ':'; )
				fc(*p++);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_Ts: /* Ts */ {
		/* second */
		if (d->sdate) {
			for (p = d->sdate; *p && *p != ' '; p++);
			for (p++; *p && *p != ':'; p++);
			for (p++; *p && *p != ':'; p++);
			for (p++; *p; )
				fc(*p++);
			return (strVal);
		}
		return (nullVal);
	}


	case KW_P: /* P */
	case KW_USER: /* USER */ {
		/* programmer */
		if (d->user) {
			if (p = strchr(d->user, '/')) *p = 0;
			fs(d->user);
			if (p) *p = '/';
			return (strVal);
		}
		return (nullVal);
	}
	case KW_REALUSER: /* REALUSER */ {
		if (d->user) {
			if (p = strchr(d->user, '/')) {
				++p;
			} else {
				p = d->user;
			}
			fs(p);
			return (strVal);
		}
		return (nullVal);
	}
	case KW_FULLUSER: /* FULLUSER */ {
		if (d->user) {
			fs(d->user);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DS: /* DS */ {
		/* serial number */
		fd(d->serial);
		return (strVal);
	}

	case KW_DP: /* DP */ {
		/* parent serial number */
		fd(d->pserial);
		return (strVal);
	}


	case KW_MR: /* MR */ {
		/* MR numbers for delta				*/
		/* not implemeted yet, return NULL for now	*/
		return (strVal);
	}

	case KW_C: /* C */ {
		int i, j = 0;
		/* comments */
		/* XXX TODO: we may need to the walk the comment graph	*/
		/* to get the latest comment				*/
		EACH(d->comments) {
			j++;
			fs(d->comments[i]);
		}
		if (j) return (strVal);
		return (nullVal);
	}

	case KW_HTML_C: /* HTML_C */ {
		int	i;
		char	html_ch[20];
		unsigned char *p;

		unless (d->comments && (int)(long)(d->comments[0])) {
			fs("&nbsp;");
		} else {
			EACH(d->comments) {
				if (i > 1) fs("<br>");
				if (d->comments[i][0] == '\t') {
					fs("&nbsp;&nbsp;&nbsp;&nbsp;");
					fs("&nbsp;&nbsp;&nbsp;&nbsp;");
					p = &d->comments[i][1];
				} else {
					p = d->comments[i];
				}
				for ( ; *p ; ++p) {
					if (isalnum(*p)) {
						fc(*p);
					} else if (*p == ' ') {
						fs("&nbsp;");
					} else {
						sprintf(html_ch, "&#%d;", *p);
						fs(html_ch);
					}
				}
			}
		}
		return (strVal);
	}

	case KW_UN: /* UN */ {
		/* users name(s) */
		/* XXX this is a multi-line text field, definition unknown */
		fs("??");
		return (strVal);
	}

	case KW_FL: /* FL */ {
		/* flag list */
		/* XX TODO: ouput flags in symbolic names ? */
		fx(d->flags);
		return (strVal);
	}

	case KW_Y: /* Y */ {
		/* moudle type, not implemented */
		fs("");
		return (strVal);
	}

	case KW_MF: /* MF */ {
		/* MR validation flag, not implemented	*/
		fs("");
		return (strVal);
	}

	case KW_MP: /* MP */ {
		/* MR validation pgm name, not implemented */
		fs("");
		return (strVal);
	}

	case KW_KF: /* KF */ {
		/* keyword error warining flag	*/
		/* not implemented		*/
		fs("no");
		return (strVal);
	}

	case KW_KV: /* KV */ {
		/* keyword validation string	*/
		/* not impleemnted		*/
		return (nullVal);
	}

	case KW_BF: /* BF */ {
		/* branch flag */
		/* BitKeeper does not have a branch flag */
		/* but we can derive the value		 */
		if (d->rev) {
			int i;
			/* count the number of dot */
			for (i = 0, p = d->rev; *p && i <= 2; p++) {
				if (*p == '.') i++;
			}
			if (i == 2)  { /* if we have 2 dot, it is a branch */
				fs("yes");
			} else	{
				fs("no");
			}
		}
		return (strVal);
	}

	case KW_J: /* J */ {
		/* Join edit flag  */
		/* not implemented */
		fs("no");
		return (strVal);
	}

	case KW_LK: /* LK */ {
		/* locked releases */
		/* not implemented */
		return (nullVal);
	}

	case KW_Q: /* Q */ {
		/* User defined keyword */
		/* not implemented	*/
		return (nullVal);
	}

	case KW_M: /* M */ {
		/* XXX TODO: get the value from the	*/
		/* 'm' flag if/when implemented		*/
		fs(basenm(s->gfile));
		return (strVal);
	}

	case KW_FB: /* FB */ {
		/* floor boundary */
		/* not implemented */
		return (nullVal);
	}

	case KW_CB: /* CB */ {
		/* ceiling boundary */
		return (nullVal);
	}

	case KW_Ds: /* Ds */ {
		/* default branch or "none", see also DFB */
		if (s->defbranch) {
			fs(s->defbranch);
		} else {
			fs("none");
		}
		return (strVal);
	}

	case KW_ND: /* ND */ {
		/* Null delta flag */
		/* not implemented */
		fs("no");
		return (strVal);
	}

	case KW_FD: /* FD */ {
		/* file description text */
		int i = 0, j = 0;
		EACH(s->text) {
			if (s->text[i][0] == '\001') continue;
			j++;
			fs(s->text[i]);
		}
		if (j) return (strVal);
		return (nullVal);
	}

	case KW_BD: /* BD */ {
		/* Body text */
		/* XX TODO: figure out where to extract this info */
		fs("??");
		return (strVal);
	}

	case KW_GB: /* GB */ {
		/* Gotten body */
		sccs_restart(s);
		sccs_get(s, d->rev, 0, 0, 0, GET_EXPAND|SILENT|PRINT, "-");
		return (strVal);
	}

	/* Lose this in 2008 */
	case KW_Z: /* Z */ {
		fs("@(#)");
		return (strVal);
	}

	case KW_F: /* F */ {
		/* s file basename */
		if (s->sfile) {
			/* scan backward for '/' */
			for (p = s->sfile, q = &p[strlen(p) -1];
				(q > p) && (*q != '/'); q--);
			if (*q == '/') q++;
			fs(q);
		}
		return (strVal);
	}

	case KW_PN: /* PN */
	case KW_SFILE: /* SFILE */ {
		/* s file path */
		if (s->sfile) {
			fs(s->sfile);
			return (strVal);
		}
		return nullVal;
	}

	case KW_N: /* N */ {
		fd(s->numdeltas);
		return (strVal);
	}

	case KW_ODD: /* ODD */ {
		return (s->prs_odd ? strVal : nullVal);
	}

	case KW_EVEN: /* EVEN */ {
		return (s->prs_odd ? nullVal : strVal);
	}

	case KW_JOIN: /* JOIN */ {
		unless (s->prs_join) {
			s->prs_join = 1;
			return (nullVal);
		}
		fs(",");
		return (strVal);
	}

	case KW_G: /* G */ {
		/* g file basename */
		if (s->gfile) {
			/* scan backward for '/' */
			for (p = s->gfile, q = &p[strlen(p) -1];
				(q > p) && (*q != '/'); q--);
			if (*q == '/') q++;
			fs(q);
		}
		return (strVal);
	}

	case KW_DSUMMARY: /* DSUMMARY */ {
	/* :DT: :I: :D: :T::TZ: :P:$if(:HT:){@:HT:} :DS: :DP: :Li:/:Ld:/:Lu: */
	 	KW("DT"); fc(' '); KW("I"); fc(' '); KW("D"); fc(' ');
		KW("T"); KW("TZ"); fc(' '); KW("P");
		if (d->hostname) { fc('@'); fs(d->hostname); } fc(' ');
	 	KW("DS"); fc(' '); KW("DP"); fc(' '); KW("DL");
		return (strVal);
	}

	case KW_PATH: /* PATH */ {	/* $if(:DPN:){P :DPN:\n} */
		if (d->pathname) {
			fs("P ");
			fs(d->pathname);
			fc('\n');
			return (strVal);
		}
		return (nullVal);
	}

	/* $each(:TAG:){S (:TAG:)\n} */
	case KW_SYMBOLS: /* SYMBOLS */
	case KW_TAGS: /* TAGS */ {
		symbol	*sym;
		int	j = 0;

		unless (d && (d->flags & D_SYMBOLS)) return (nullVal);
		for (sym = s->symbols; sym; sym = sym->next) {
			unless (sym->d == d) continue;
			j++;
			fs("S ");
			fs(sym->symname);
			fc('\n');
		}
		if (j) return (strVal);
		return (nullVal);
		
	}

	case KW_COMMENTS: /* COMMENTS */ {	/* $if(:C:){$each(:C:){C (:C:)}\n} */
		int i, j = 0;
		/* comments */
		/* XXX TODO: we may need to the walk the comment graph	*/
		/* to get the latest comment				*/
		EACH(d->comments) {
			fs("C ");
			fs(d->comments[i]);
			fc('\n');
		}
		if (j) return (strVal);
		return (nullVal);
		
	}

	case KW_DEFAULT: /* DEFAULT */
	case KW_PRS: /* PRS */ {
		KW("DSUMMARY");
		fc('\n');
		KW("PATH");
		KW("SYMBOLS");
		KW("COMMENTS");
		fs("------------------------------------------------\n");
		return (strVal);
	}

	case KW_LOG: /* LOG */ {
		int	i;
		symbol	*sym;

		fs(d->pathname);
		fc(' ');
		KW("REV");
		fs("\n  ");
		KW("D_"); fc(' '); KW("T"); KW("TZ");
		fc(' '); KW("P");
		if (d->hostname) { fc('@'); fs(d->hostname); } fc(' ');
		fc('+'); KW("LI"); fs(" -"); KW("LD"); fc('\n');
		EACH(d->comments) {
			fs("  ");
			fs(d->comments[i]);
			fc('\n');
		}
		fc('\n');
		unless (d && (d->flags & D_SYMBOLS)) return (strVal);
		for (i = 0, sym = s->symbols; sym; sym = sym->next) {
			unless (sym->d == d) continue;
			i++;
			fs("  TAG: ");
			fs(sym->symname);
			fc('\n');
		}
		fc('\n');
		return (strVal);
	}

	case KW_CHANGESET: /* CHANGESET */ {
		if (CSET(s)) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_CSETFILE: /* CSETFILE */ {
		if (s->tree->csetFile) {
			fs(s->tree->csetFile);
			return (strVal);
		}
		return nullVal;
	}

	case KW_RANDOM: /* RANDOM */ {
		if (s->tree->random) {
			fs(s->tree->random);
			return (strVal);
		}
		return nullVal;
	}

	case KW_ENC: /* ENC */ {
		switch (s->encoding & E_DATAENC) {
		    case E_ASCII:
			fs("ascii"); return (strVal);
		    case E_UUENCODE:
			fs("uuencode"); return (strVal);
		    case E_BINPOOL:
			fs("binpool"); return (strVal);
		}
		return nullVal;
	}

	case KW_COMPRESSION: /* COMPRESSION */ {
		switch (s->encoding & E_COMP) {
		    case 0: 
			fs("none"); return (strVal);
		    case E_GZIP:
			fs("gzip"); return (strVal);
		}
		return nullVal;
	}

	case KW_VERSION: /* VERSION */ {
		fd(s->version);
		return (strVal);
	}

	case KW_X_FLAGS: /* X_FLAGS */ {
		char	buf[20];

		sprintf(buf, "0x%x", sccs_xflags(d));
		fs(buf);
		return (strVal);
	}

	case KW_X_XFLAGS: /* X_XFLAGS */ {
		char	buf[20];

		sprintf(buf, "0x%x", s->xflags);
		fs(buf);
		return (strVal);
	}

	case KW_FLAGS: /* FLAGS */
	case KW_XFLAGS: /* XFLAGS */ {
		int	flags =
		    (kwval->kwnum == KW_FLAGS) ? sccs_xflags(d) : s->xflags;

		fs(xflags2a(flags));
		unless (flags & (X_EOLN_NATIVE|X_EOLN_WINDOWS)) {
			fs(",EOLN_UNIX");
		}
		return (strVal);
	}

	case KW_REV: /* REV */ {
		fs(d->rev);
		return (strVal);
	}

	/* print the first rev at/below this which is in a cset */
	case KW_CSETREV: /* CSETREV */ {
		while (d && !(d->flags & D_CSET)) d = d->kid;
		unless (d) return (nullVal);
		fs(d->rev);
		return (strVal);
	}

	case KW_CSETKEY: /* CSETKEY */ {
		char key[MAXKEY];
		unless (d->flags & D_CSET) return (nullVal);
		sccs_sdelta(s, d, key);
		fs(key);
		return (strVal);
	}

	case KW_HASHCOUNT: /* HASHCOUNT */ {
		int	n = sccs_hashcount(s);

		unless (HASH(s)) return (nullVal);
		fd(n);
		return (strVal);
	}

	case KW_MD5KEY: /* MD5KEY */ {
		char	b64[MD5LEN];

		sccs_md5delta(s, d, b64);
		fs(b64);
		return (strVal);
	}

	case KW_KEY: /* KEY */ {
		char	key[MAXKEY];

		sccs_sdelta(s, d, key);
		fs(key);
		return (strVal);
	}

	case KW_ROOTKEY: /* ROOTKEY */ {
		char key[MAXKEY];

		sccs_sdelta(s, sccs_ino(s), key);
		fs(key);
		return (strVal);
	}

	case KW_SHORTKEY: /* SHORTKEY */ {
		char	buf[MAXPATH+200];
		char	*t;

		sccs_sdelta(s, d, buf);
		if (t = sccs_iskeylong(buf)) *t = 0;
		fs(buf);
		return (strVal);
	}

	case KW_SYMBOL: /* SYMBOL */
	case KW_TAG: /* TAG */ {
		symbol	*sym;
		int	j = 0;

		unless (d && (d->flags & D_SYMBOLS)) return (nullVal);
		while (d->type == 'R') d = d->parent;
		for (sym = s->symbols; sym; sym = sym->next) {
			unless (sym->d == d) continue;
			j++;
			fs(sym->symname);
		}
		if (j) return (strVal);
		return (nullVal);
	}

	case KW_TAG_PSERIAL: /* TAG_PSERIAL */ {
		unless (d->ptag) return (nullVal);
		fd(d->ptag);
		return (strVal);
	}

	case KW_TAG_MSERIAL: /* TAG_MSERIAL */ {
		unless (d->mtag) return (nullVal);
		fd(d->mtag);
		return (strVal);
	}

	case KW_TAG_PREV: /* TAG_PREV */ {
		delta	*p;

		unless (d->ptag) return (nullVal);
		p = sfind(s, d->ptag);
		assert(p);
		fs(p->rev);
		return (strVal);
	}

	case KW_TAG_MREV: /* TAG_MREV */ {
		delta	*p;

		unless (d->mtag) return (nullVal);
		p = sfind(s, d->mtag);
		assert(p);
		fs(p->rev);
		return (strVal);
	}

	case KW_GFILE: /* GFILE */ {
		if (s->gfile) {
			fs(s->gfile);
		}
		return (strVal);
	}

	case KW_HT: /* HT */
	case KW_HOST: /* HOST */ {
		/* host without any importer name */
		if (d->hostname) {
			if (p = strchr(d->hostname, '/')) {
				*p = 0;
				fs(d->hostname);
				*p = '/';
			} else if (p = strchr(d->hostname, '[')) {
				*p = 0;
				fs(d->hostname);
				*p = '[';
			} else {
				fs(d->hostname);
			}
			return (strVal);
		}
		return (nullVal);
	}
	case KW_REALHOST: /* REALHOST */ {
		if (d->hostname) {
			if (p = strchr(d->hostname, '/')) {
				fs(p+1);
			} else if (p = strchr(d->hostname, '[')) {
				*p = 0;
				fs(d->hostname);
				*p = '[';
			} else {
				fs(d->hostname);
			}
			return (strVal);
		}
		return (nullVal);
	}
	case KW_FULLHOST: /* FULLHOST */ {
		if (d->hostname) {
			if (p = strchr(d->hostname, '[')) {
				*p = 0;
				fs(d->hostname);
				*p = '[';
			} else {
				fs(d->hostname);
			}
			return (strVal);
		}
		return (nullVal);
	}

	case KW_IMPORTER: /* IMPORTER */ {
		/* importer name */
		if (d->hostname) {
			for (p = d->hostname; *p && (*p != '['); p++);
			if (*p) {
				while (*(++p) != ']') fc(*p);
				return (strVal);
			}
		}
		return (nullVal);
	}

	case KW_DOMAIN: /* DOMAIN */ {
		/* domain: the truncated domain name.
		 * Counting from the right, we keep zero or more
		 * two-letter components, then zero or one three
		 * letter component, then one more component
		 * irrespective of length; anything after that is
		 * nonsignificant.  If we run out of hostname before
		 * we run out of short components, the entire name is
		 * significant.  This heuristic is not always right
		 * (consider .to) but works most of the time.
		 */
		char *p, *q;
		int i;
		
		if (!d->hostname) return (nullVal);
		q = d->hostname;
		p = &q[strlen(q)];
		do {
			i = 0;
			while (*--p != '.' && p != q) i++;
		} while (i == 2);
		if (i == 3) while (*--p != '.' && p != q);
		if (*p == '.') p++;
		fs(p);
		return (strVal);
	}

	case KW_TZ: /* TZ */ {
		/* time zone */
		if (d->zone) {
			fs(d->zone);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_TIME_T: /* TIME_T */ {
		char	buf[20];

		sprintf(buf, "%d", (int)d->date);
		fs(buf);
		return (strVal);
	}

	case KW_UTC: /* UTC */ {
		char	*utcTime;
		if (utcTime = sccs_utctime(d)) {
			fs(utcTime);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_UTC_FUDGE: /* UTC-FUDGE */ {
		char	*utcTime;

		DATE(d);
		d->date -= d->dateFudge;
		if (utcTime = sccs_utctime(d)) {
			fs(utcTime);
			d->date += d->dateFudge;
			return (strVal);
		}
		d->date += d->dateFudge;
		return (nullVal);
	}

	case KW_FUDGE: /* FUDGE */ {
		char	buf[20];

		sprintf(buf, "%d", (int)d->dateFudge);
		fs(buf);
		return (strVal);
	}

	case KW_AGE: /* AGE */ {	/* how recently modified */
		time_t	when = time(0) - (d->date - d->dateFudge);

		fs(age(when, " "));
		return (strVal);
	}

	case KW_HTML_AGE: /* HTML_AGE */ {	/* how recently modified */
		time_t	when = time(0) - (d->date - d->dateFudge);

		fs(age(when, "&nbsp;"));
		return (strVal);
	}

	case KW_DSUM: /* DSUM */ {
		if (d->flags & D_CKSUM) {
			fd((int)d->sum);
			return (strVal);
		}
		if (d->type == 'R') {
			assert(d->sum == 0);
			fs("0");
			return (strVal);
		}
		return (nullVal);
	}

	case KW_FSUM: /* FSUM */ {
		unless (s->cksumdone) badcksum(s, SILENT);
		if (s->cksumok) {
			char	buf[20];

			sprintf(buf, "%d", (int)s->cksum);
			fs(buf);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_SYMLINK: /* SYMLINK */ {
		if (d->symlink) {
			fs(d->symlink);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_MODE: /* MODE */ {
		char	buf[20];

		sprintf(buf, "%o", (int)d->mode);
		fs(buf);
		return (strVal);
	}

	case KW_RWXMODE: /* RWXMODE */ {
		char	buf[20];

		sprintf(buf, "%s", mode2a(d->mode));
		fs(buf);
		return (strVal);
	}

	case KW_TYPE: /* TYPE */ {
		if (BITKEEPER(s)) {
			fs("BitKeeper");
			if (CSET(s)) fs("|ChangeSet");
		} else {
			fs("SCCS");
		}
		return (strVal);
	}

	case KW_RENAME: /* RENAME */ {
		/* per delta path name if the pathname is a rename */
		if (d->pathname && !(d->flags & D_DUPPATH)) {
			fs(d->pathname);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DPN: /* DPN */ {
		/* per delta path name */
		if (d->pathname) {
			fs(d->pathname);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_SPN: /* SPN */ {
		/* per delta SCCS path name */
		if (d->pathname) {
			char	*p = name2sccs(d->pathname);

			fs(p);
			free(p);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_MGP: /* MGP */ {
		/* merge parent's serial number */
		fd(d->merge);
		return (strVal);
	}

	case KW_PARENT: /* PARENT */ {
		if (d->parent) {
			fs(d->parent->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_MPARENT: /* MPARENT */ {	/* print the merge parent if present */
		if (d->merge && (d = sfind(s, d->merge))) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_MERGE: /* MERGE */ {	/* print this rev if a merge node */
		if (d->merge) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GCA: /* GCA */ {		/* print gca rev if a merge node */
		if (d->merge && (d = gca(sfind(s, d->merge), d->parent))) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GCA2: /* GCA2 */ {	/* print gca rev if a merge node */
		if (d->merge && (d = gca2(s, sfind(s, d->merge), d->parent))) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_SETGCA: /* SETGCA */ {	/* print gca rev if a merge node */
		char	*inc, *exc;

		if (d->merge &&
		    (d = gca3(s, sfind(s, d->merge), d->parent, &inc, &exc))) {
			fs(d->rev);
			if (inc) {
				fc('+');
				fs(inc);
				free(inc);
			}
			if (exc) {
				fc('-');
				fs(exc);
				free(exc);
			}
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GET_SETGCA: /* GET_SETGCA */ {	/* print gca args for get*/
		char	*inc, *exc;

		if (d->merge &&
		    (d = gca3(s, sfind(s, d->merge), d->parent, &inc, &exc))) {
			fs("-r");
			fs(d->rev);
			if (inc) {
				fs(" -i");
				fs(inc);
				free(inc);
			}
			if (exc) {
				fs(" -x");
				fs(exc);
				free(exc);
			}
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GET_SETGCA301: /* GET_SETGCA301 */ {	/* print gca args for get*/
		char	*inc, *exc;

		if (d->merge &&
		    (d = gca3(s, sfind(s, d->merge), d->parent, &inc, &exc))) {
			fs("-r");
			fs(d->rev);
			if (inc) free(inc);
			if (exc) free(exc);
			return (strVal);
		}
		return (nullVal);
	}
	case KW_GET_SETGCA302: /* GET_SETGCA302 */ {	/* print gca args for get*/
		char	*inc, *exc;

		if (d->merge &&
		    (d = gca3(s, sfind(s, d->merge), d->parent, &inc, &exc))) {
			fs("-r");
			fs(d->rev);
			if (inc) {
				fs(" -M");
				fs(inc);
				free(inc);
			}
			if (exc) {
				fs(" -i");
				fs(exc);
				free(exc);
			}
			return (strVal);
		}
		return (nullVal);
	}

	case KW_PREV: /* PREV */ {
		if (d->next) {
			fs(d->next->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_NEXT: /* NEXT */ {
		if (d = sccs_next(s, d)) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_KID: /* KID */ {
		if (d = d->kid) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_KIDS: /* KIDS */ {
		int	space = 0;

		if (d->flags & D_MERGED) {
			delta	*m;

			for (m = s->table; m; m = m->next) {
				if (m->merge == d->serial) {
					if (space) fs(" ");
					fs(m->rev);
					space = 1;
				}
			}
		}
		unless (d = d->kid) return (space ? strVal : nullVal);
		if (space) fs(" ");
		fs(d->rev);
		while (d = d->siblings) {
			fs(" ");
			fs(d->rev);
		}
		return (strVal);
	}

	case KW_TIP: /* TIP */ {
		if (sccs_isleaf(s, d)) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_SIBLINGS: /* SIBLINGS */ {
		if (d = d->siblings) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DFB: /* DFB */ {
		/* default branch */
		if (s->defbranch) {
			fs(s->defbranch);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DANGLING: /* DANGLING */ {
		/* don't clause on MONOTONIC, we had a bug there, see chgset */
		if (d->dangling) {
			fs(d->rev);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_RREV: /* RREV */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->remote) {
			fs(n->remote);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_LREV: /* LREV */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->local) {
			fs(n->local);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GREV: /* GREV */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->gca) {
			fs(n->gca);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_RPN: /* RPN */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->remote && (d = findrev(s, n->remote))) {
			fs(d->pathname);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_LPN: /* LPN */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->local && (d = findrev(s, n->local))) {
			fs(d->pathname);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_GPN: /* GPN */ {
		names	*n;

		unless (s->rrevs) {
			s->rrevs = res_getnames(sccsXfile(s, 'r'), 'r');
		}
		n = (names *)s->rrevs;
		if (n && n->gca && (d = findrev(s, n->gca))) {
			fs(d->pathname);
			return (strVal);
		}
		return (nullVal);
	}

	case KW_DIFFS: /* DIFFS */
	case KW_DIFFS_U: /* DIFFS_U */
	case KW_DIFFS_UP: /* DIFFS_UP */ {
		int	kind;

		switch (kwval->kwnum) {
		    default:
		    case KW_DIFFS:	kind = DF_DIFF; break;
		    case KW_DIFFS_U:	kind = DF_UNIFIED; break;
		    case KW_DIFFS_UP:	kind = DF_UNIFIED|DF_GNUp; break;
		}
		if (d == s->tree) return (nullVal);
		if (out) {
			sccs_diffs(s, d->rev, d->rev, SILENT, kind, out);
		} else {
			char	*cmd;
			FILE	*f;
			char	buf[BUFSIZ];

			cmd = aprintf("bk diffs -%s%shR%s '%s'",
			    kind == DF_DIFF ? "" : "u",
			    kind & DF_GNUp ? "p" : "",
			    d->rev, s->gfile);
			unless (f = popen(cmd, "r")) {
				free(cmd);
				return (nullVal);
			}
			while (fnext(buf, f)) fs(buf);
			pclose(f);
			free(cmd);
		}
		return (strVal);
	}

	/* don't document, TRUE/FALSE are for t.prs */
	case KW_FALSE: /* FALSE */
		return (nullVal);

	case KW_TRUE: /* TRUE */
		fs("1");
		return (strVal);

	case KW_REPO_ID: /* REPO_ID */ {
		fs(proj_repoID(0));
		return (strVal);
	}
	case KW_BPHASH: /* BPHASH */ {
		if (d->hash) {
			fs(d->hash);
			return (strVal);
		}
		return (nullVal);
	}
	default:
		return (notKeyword);
	}
}

int
sccs_prsdelta(sccs *s, delta *d, int flags, char *dspec, FILE *out)
{
	if (d->type != 'D' && !(flags & PRS_ALL)) return (0);
	if (SET(s) && !(d->flags & D_SET)) return (0);
	s->prs_output = 0;
	dspec_eval(out, 0, s, d, dspec);
	if (s->prs_output) {
		s->prs_odd = !s->prs_odd;
		if (flags & PRS_LF) fputc('\n', out);
	}
	return (s->prs_output);
}

char *
sccs_prsbuf(sccs *s, delta *d, int flags, char *dspec)
{
	char	**buf = 0;

	if (d->type != 'D' && !(flags & PRS_ALL)) return (0);
	if (SET(s) && !(d->flags & D_SET)) return (0);
	dspec_eval(0, &buf, s, d, dspec);
	if (data_length(buf)) {
		s->prs_odd = !s->prs_odd;
		if (flags & PRS_LF) buf = str_append(buf, "\n", 0);
	}
	return (str_pullup(0, buf));
}


/*
 * Order is
 *	f d default
 *	f e encoding
 *	f x bitkeeper bits
 *	R random
 *	T descriptive text
 *	T descriptive text
 *	...
 */
void
sccs_perfile(sccs *s, FILE *out)
{
	int	i;

	if (s->defbranch) fprintf(out, "f d %s\n", s->defbranch);
	if (s->encoding) fprintf(out, "f e %d\n", s->encoding);
	EACH(s->text) fprintf(out, "T %s\n", s->text[i]);
	if (s->version) fprintf(out, "V %u\n", s->version);
	fprintf(out, "\n");
}

#define	FLAG(c)	((buf[0] == 'f') && (buf[1] == ' ') &&\
		(buf[2] == c) && (buf[3] == ' '))

sccs	*
sccs_getperfile(MMAP *in, int *lp)
{
	sccs	*s = calloc(1, sizeof(sccs));
	int	unused = 1;
	char	*buf;

	unless (buf = mkline(mnext(in))) goto err;
	unless (buf[0]) {
		free(s);
		return (0);
	}
	(*lp)++;
	if (FLAG('d')) {
		unused = 0;
		s->defbranch = strdup(&buf[4]);
		unless (buf = mkline(mnext(in))) {
err:			fprintf(stderr,
			    "takepatch: file format error near line %d\n", *lp);
			free(s);
			return (0);
		}
		(*lp)++;
	}
	if (FLAG('e')) {
		unused = 0;
		s->encoding = atoi(&buf[4]);
		unless (buf = mkline(mnext(in))) goto err; (*lp)++;
	}
	if (FLAG('x')) {
		/* Ignored */
		unless (buf = mkline(mnext(in))) goto err; (*lp)++;
	}
	while (strneq(buf, "T ", 2)) {
		unused = 0;
		s->text = addLine(s->text, strnonldup(&buf[2]));
		unless (buf = mkline(mnext(in))) goto err; (*lp)++;
	}
	if (strneq(buf, "V ", 2)) {
		unused = 0;
		s->version = atoi(&buf[2]);
		unless (buf = mkline(mnext(in))) goto err; (*lp)++;
	}
	unless (s->version) s->version = SCCS_VERSION;
	if (buf[0]) goto err;		/* should be empty */

	if (unused) {
		free(s);
		return (0);
	}
	return (s);
}

private int
do_patch(sccs *s, delta *d, int flags, FILE *out)
{
	int	i;	/* used by EACH */
	symbol	*sym;
	char	type;

	if (!d) return (0);
	type = d->type;
	if ((d->type == 'R') && d->parent && streq(d->rev, d->parent->rev)) {
	    	type = 'M';
	}

	fprintf(out, "%c %s %s%s %s%s%s +%u -%u\n",
	    type, d->rev, d->sdate,
	    d->zone ? d->zone : "",
	    d->user,
	    d->hostname ? "@" : "",
	    d->hostname ? d->hostname : "",
	    d->added, d->deleted);

	/*
	 * Order from here down is alphabetical.
	 */
	if (d->hash) fprintf(out, "A %s\n", d->hash);
	if (d->csetFile) fprintf(out, "B %s\n", d->csetFile);
	if (d->flags & D_CSET) fprintf(out, "C\n");
	if (d->dangling) fprintf(out, "D\n");
	EACH(d->comments) fprintf(out, "c %s\n", d->comments[i]);
	if (d->dateFudge) fprintf(out, "F %d\n", (int)d->dateFudge);
	EACH(d->include) {
		delta	*e = sfind(s, d->include[i]);
		assert(e);
		fprintf(out, "i ");
		sccs_pdelta(s, e, out);
		fprintf(out, "\n");
	}
	if (d->flags & D_CKSUM) {
		fprintf(out, "K %u\n", d->sum);
	}
	if (d->merge) {
		delta	*e = sfind(s, d->merge);
		assert(e);
		fprintf(out, "M ");
		sccs_pdelta(s, e, out);
		fprintf(out, "\n");
	}
	if (d->flags & D_MODE) {
		fprintf(out, "O %s", mode2a(d->mode));
		if (S_ISLNK(d->mode)) {
			assert(d->symlink);
			fprintf(out, " %s\n", d->symlink);
		} else {
			fprintf(out, "\n");
		}
	}
	if (s->tree->pathname) assert(d->pathname);
	if (d->pathname) fprintf(out, "P %s\n", d->pathname);
	if (d->random) fprintf(out, "R %s\n", d->random);
	if ((d->flags & D_SYMBOLS) || d->symGraph) {
		for (sym = s->symbols; sym; sym = sym->next) {
			unless (sym->metad == d) continue;
			fprintf(out, "S %s\n", sym->symname);
		}
		if (d->symGraph) fprintf(out, "s g\n");
		if (d->symLeaf) fprintf(out, "s l\n");
		if (d->ptag) {
			delta	*e = sfind(s, d->ptag);
			char	buf[MAXKEY];

			assert(e);
			sccs_sdelta(s, e, buf);
			fprintf(out, "s %s\n", buf);
		}
		if (d->mtag) {
			delta	*e = sfind(s, d->mtag);
			char	buf[MAXKEY];

			assert(e);
			sccs_sdelta(s, e, buf);
			fprintf(out, "s %s\n", buf);
		}
	}
	if (d->flags & D_TEXT) {
		if (d->text) {
			EACH(d->text) {
				fprintf(out, "T %s\n", d->text[i]);
			}
		} else {
			fprintf(out, "T\n");
		}
	}
	if ((flags & PRS_GRAFT) && s->version) {
		fprintf(out, "V %u\n", s->version);
	}
	EACH(d->exclude) {
		delta	*e = sfind(s, d->exclude[i]);
		assert(e);
		fprintf(out, "x ");
		sccs_pdelta(s, e, out);
		fprintf(out, "\n");
	}
	if (d->flags & D_XFLAGS) {
		assert((d->xflags & X_EOLN_UNIX) == 0);
		fprintf(out, "X 0x%x\n", d->xflags);
	}
	if (s->tree->zone) assert(d->zone);
	fprintf(out, "------------------------------------------------\n");
	return (0);
}

private void
prs_reverse(sccs *s, int flags, char *dspec, FILE *out)
{
	delta	*d;
	int	ser;
	
	for (ser = 1; ser < s->nextserial; ser++) {
		unless ((d = sfind(s, ser)) && (d->flags & D_SET)) continue;
		if (sccs_prsdelta(s, d, flags, dspec, out) && s->prs_one) {
			return;
		}
	}
}

private void
prs_forward(sccs *s, int flags, char *dspec, FILE *out)
{
	delta	*d;

	for (d = s->table; d; d = d->next) {
		unless (d->flags & D_SET) continue;
		if (sccs_prsdelta(s, d, flags, dspec, out) && s->prs_one) {
			return;
		}
	}
}

int
sccs_prs(sccs *s, u32 flags, int reverse, char *dspec, FILE *out)
{
	delta	*d;

	if (!dspec) dspec = ":DEFAULT:";
	s->prs_odd = 0;
	s->prs_join = 0;
	GOODSCCS(s);
	if (flags & PRS_PATCH) {
		assert(s->rstart == s->rstop);
		return (do_patch(s, s->rstart, flags, out));
	}
	/* print metadata if they asked */
	if (flags & PRS_META) {
		symbol	*sym;
		for (sym = s->symbols; sym; sym = sym->next) {
			fprintf(out, "S %s %s\n", sym->symname, sym->rev);
		}
	}
	unless (SET(s)) {
		for (d = s->rstop; d; d = d->next) {
			d->flags |= D_SET;
			if (d == s->rstart) break;
		}
	}
	if (reverse) {
		 prs_reverse(s, flags, dspec, out);
	} else {
		 prs_forward(s, flags, dspec, out);
	}

	if (KV(s) && s->mdbm) {
		mdbm_close(s->mdbm);
		s->mdbm = 0;
	}
	return (0);
}

/*
 * return the number of nodes, including this one, in this subgraph.
 */
private int
numNodes(delta *d)
{
	if (!d || (d->type != 'D')) return (0);

	return (1 + numNodes(d->kid) + numNodes(d->siblings));
}

private void
addNodes(sccs *s, delta **list, int j, delta *d)
{
	if (!d || (d->type != 'D')) return;
	list[j++] = d;
	addNodes(s, list, j, d->kid);
	while (list[j]) j++;
	addNodes(s, list, j, d->siblings);
}

private inline int
samekeystr(char *a, char *b)
{
	char	*parts_a[6], *parts_b[6];

	debug((stderr, "samekeystr(%s, %s)\n", a, b));
	/* tight compare */
	if (streq(a, b)) return (1);

	/* loose compare */
	explodeKey(a, parts_a);
	explodeKey(b, parts_b);
	debug((stderr, "samekeystr: parts_a:%s, %s, %s, %s, %s, %s\n",
		parts_a[0],
		parts_a[1] ? parts_a[1] : "NULL",
		parts_a[2],
		parts_a[3],
		parts_a[4] ? parts_a[4] : "NULL",
		parts_a[5] ? parts_a[5] : "NULL"));
	debug((stderr, "samekeystr: parts_b:%s, %s, %s, %s, %s, %s\n",
		parts_b[0],
		parts_b[1] ? parts_b[1] : "NULL",
		parts_b[2],
		parts_b[3],
		parts_b[4] ? parts_b[4] : "NULL",
		parts_b[5] ? parts_b[5] : "NULL"));

	/* user */
	assert(parts_a[0] && parts_b[0]);
	unless (streq(parts_a[0], parts_b[0])) return(0);
	/* host (may not be there; if not, then not in both) */
	unless ((!parts_a[1] && !parts_b[1]) ||
		(parts_a[1] && parts_b[1]))    return(0);
	unless (!parts_a[1] || streq(parts_a[1], parts_b[1]))
		return (0);
	/* pathname (might be zero length, but will be there */
	assert(parts_a[2] && parts_b[2]);
	unless (streq(parts_a[2], parts_b[2])) return(0);
	/* utc + fudge timestamp */
	assert(parts_a[3] && parts_b[3]);
	unless (streq(parts_a[3], parts_b[3])) return(0);
	/* chksum (or almostUnique if null delta) */
	if (parts_a[4] && parts_b[4])
		unless (streq(parts_a[4], parts_b[4])) return (0);
	/* if 1.0 node, then random number */
	if (parts_a[5] && parts_b[5])
		unless (streq(parts_a[5], parts_b[5])) return (0);
	return (1);
}

private int
samekey(delta *d, char *user, char *host, char *path, time_t date,
	sum_t *cksump)
{
	DATE(d);
	if (d->date != date) {
		debug((stderr, "samekey: No date match %s: %d (%s%s) vs %d\n",
		    d->rev, (u32)d->date, d->sdate,
		    d->zone ?  d->zone : "", (u32)date));
		return (0);
	}
	debug((stderr, "samekey: DATE matches\n"));
	debug((stderr, "samekey: USER %s %s\n", d->user, user));
	unless (streq(d->user, user)) return (0);
	debug((stderr, "samekey: HOST %s %s\n", d->hostname, host));
	if (d->hostname) {
		unless (host && streq(d->hostname, host)) return (0);
	} else if (host) {
		return (0);
	}
	debug((stderr, "samekey: PATH %s %s\n", d->pathname, path));
	if (d->pathname) {
		unless (path && streq(d->pathname, path)) return (0);
	} else if (path) {
		return (0);
	}
	/* XXX: all d->cksum are valid: we'll assume always there */
	if (cksump) {
		unless (d->sum == *cksump) return (0);
	}
	debug((stderr, "samekey: MATCH\n"));
	return (1);
}

/*
 * This gets a GCA which tends to be on the trunk.
 * Because it doesn't look up the mparent path, it tends not to get
 * the closest gca.
 */
private delta *
gca(delta *left, delta *right)
{
	delta	*d;

	unless (left && right) return (0);
	/*
	 * Clear the visited flag up to the root via one path,
	 * set it via the other path, then go look for it.
	 */
	for (d = left; d; d = d->parent) d->flags &= ~D_RED;
	for (d = right; d; d = d->parent) d->flags |= D_RED;
	for (d = left; d; d = d->parent) {
		if (d->flags & D_RED) return (d);
	}
	return (0);
}

private delta *
gca2(sccs *s, delta *left, delta *right)
{
	delta	*d;
	char	*slist;
	int	value;

	unless (s && s->nextserial && left && right) return (0);

	slist = calloc(s->nextserial, sizeof(char));
	slist[left->serial] |= 1;
	slist[right->serial] |= 2;
	d = (left->serial > right->serial) ? left : right;
	for ( ; d ; d = d->next) {
		unless (d->type == 'D') continue;
		unless (value = slist[d->serial]) continue;
		if (value == 3) break;
		if (d->parent)  slist[d->parent->serial] |= value;
		if (d->merge)   slist[d->merge] |= value;
	}
	free(slist);
	return (d);
}

/* XXX: could be one pass through table to do everything.
 * instead it is more or less 5 pass:
 *   gca (from bigger of left and right until gca)
 *   lmap (all of table)
 *   rmap (all of table)
 *   gmap (all of table)
 *   (not walking table, but do set equation for all elements in set)
 *   compress (all of table)
 *
 * could do as one really intense and messy loop, or could do with
 * streams somehow
 */

private delta *
gca3(sccs *s, delta *left, delta *right, char **inc, char **exc)
{
	delta	*ret = 0;
	delta	*gca;
	ser_t	*lmap, *rmap, *gmap;
	ser_t	serial;
	int	errp;

	*inc = *exc = 0;
	unless (s && s->nextserial && left && right) return (0);

	/* get three sets, fiddle with them, then compress */
	gca = gca2(s, left, right);

	errp = 0;
	lmap = serialmap(s, left, 0, 0, &errp);
	rmap = serialmap(s, right, 0, 0, &errp);
	gmap = serialmap(s, gca, 0, 0, &errp);

	if (errp || !lmap || !rmap || !gmap) goto bad;

	/* Compute simple set gca: (left & right) | ((left | right) & gca) */
	serial = (left->serial > right->serial)
		? left->serial : right->serial;
	for ( ; serial > 0; serial--) {
		gmap[serial] = ((lmap[serial] && rmap[serial])
			|| ((lmap[serial] || rmap[serial]) && gmap[serial]));
	}

	/* gmap was gca2 expanded.  It is now the set gca.
	 * compress it to be -i and -x relative to gca2 result
	 */

	if (compressmap(s, gca, gmap, 0, (void **)inc, (void **)exc))  goto bad;
	ret = gca;

bad:	if (lmap) free (lmap);
	if (rmap) free (rmap);
	if (gmap) free (gmap);
	return (ret);
}

delta	*
sccs_gca(sccs *s, delta *left, delta *right, char **inc, char **exc)
{
	return (gca3(s, left, right, inc, exc));
}

/*
 * Find the two tips of the graph so they can be closed by a merge.
 */
int
sccs_findtips(sccs *s, delta **a, delta **b)
{
	delta	*d;

	*a = *b = 0;

	/*
	 * b is that branch which needs to be merged.
	 * At any given point there should be exactly one of these.
	 */
	for (d = s->table; d; d = d->next) {
		if (d->type != 'D') continue;
		unless (isleaf(s, d)) continue;
		if (!*a) {
			*a = d;
		} else {
			assert(!*b);
			*b = d;
			/* Could break but I like the error checking */
		}
	}
	return (*b != 0);
}

/*
 * Create resolve file.
 * The order of the deltas in the file is important - the "branch"
 * should be last.
 * This currently only works for the trunk (i.e., there is one LOD).
 * XXX - this is also where we would handle pathnames, symbols, etc.
 */
int
sccs_resolveFiles(sccs *s)
{
	FILE	*f = 0;
	delta	*p, *g = 0, *a = 0, *b = 0;
	char	*n[3];
	int	retcode = -1;

	if (s->defbranch) {
		fprintf(stderr, "resolveFiles: defbranch set.  "
			"LODs are no longer supported.\n"
			"Please run 'bk support' to request "
			"assistance.\n");
err:
		return (retcode);
	}

	/*
	 * If we have no conflicts, then make sure the paths are the same.
	 * What we want to compare is whatever the tip path is with the
	 * whatever the path is in the most recent delta.
	 */
	unless (sccs_findtips(s, &a, &b)) {
		for (p = s->table; p; p = p->next) {
			if ((p->type == 'D') && !(p->flags & D_REMOTE)) {
				break;
			}
		}
		if (!p || streq(p->pathname, a->pathname)) {
			return (0);
		}
		b = a;
		a = g = p;
		retcode = 0;
		goto rename;
	} else {
		if (a->r[0] != b->r[0]) {
			fprintf(stderr, "resolveFiles: Found tips on "
			    "different LODs.\n"
			    "LODs are no longer supported.\n"
			    "Please run 'bk support' to "
			    "request assistance.\n");
			goto err;
		}
		retcode = 1;
	}

	g = gca2(s, a, b);
	assert(g);

	unless (f = fopen(sccsXfile(s, 'r'), "w")) {
		perror("r.file");
		goto err;
	}
	/*
	 * Always put the local stuff on the left, if there
	 * is any.
	 */
	if (a->flags & D_LOCAL) {
		fprintf(f, "merge deltas %s %s %s %s %s\n",
			a->rev, g->rev, b->rev, sccs_getuser(), time2date(time(0)));
	} else {
		fprintf(f, "merge deltas %s %s %s %s %s\n",
			b->rev, g->rev, a->rev, sccs_getuser(), time2date(time(0)));
	}
	fclose(f);
	unless (streq(g->pathname, a->pathname) &&
	    streq(g->pathname, b->pathname)) {
rename:		n[1] = name2sccs(g->pathname);
		if (b->flags & D_LOCAL) {
			n[2] = name2sccs(a->pathname);
			n[0] = name2sccs(b->pathname);
		} else {
			n[0] = name2sccs(a->pathname);
			n[2] = name2sccs(b->pathname);
		}
		unless (f = fopen(sccsXfile(s, 'm'), "w")) {
			perror("m.file");
			goto err;
		}
		fprintf(f, "rename %s|%s|%s\n", n[0], n[1], n[2]);
		fclose(f);
		free(n[0]);
		free(n[1]);
		free(n[2]);
	}
	/* retcode set above */
	return (retcode);
}

/*
 * Take a key like sccs_sdelta makes and find it in the tree.
 */
int
sccs_istagkey(char *key)
{
	char	*parts[6];	/* user, host, path, date as integer */
	char	buf[MAXKEY];

	strcpy(buf, key);
	explodeKey(buf, parts);
	unless (parts[4] && atoi(parts[4])) return (1);
	return (0);
}

/*
 * Make a mdbm of "char delta_key[]" => delta*
 * It is used by sccs_findKey() to quickly return a query.
 * This should be called when many calls to sccs_findKey() are made
 * so that the overall performance of the code will go up.
 *
 * If the 'selectflag' is zero, then all deltas are cached.
 * If it is set, then it is a bit map that will be anded with
 * the d->flags and if non zero, will cache those deltas.
 * This makes it easy to only cache deltas that are tagged
 * cset, D_SET, D_GONE, or whatever is desired.
 *
 * WARNING: calling this function with flags set means
 * that all future calls to sccs_findKey will be limited
 * to only keys in the list will be found.
 * If you use this "feature", and you don't call sccs_free(s)
 * (which erases this cache), then you should:
 *   if (s->findkeydb) {
 *      mdbm_close(s->findkeydb);
 *      s->findkeydb = 0;
 *   }
 */
MDBM	*
sccs_findKeyDB(sccs *s, u32 flags)
{
	delta	*d;
	datum	k, v;
	MDBM	*findkey;
	char	key[MAXKEY];

	if (s->findkeydb) {	/* do not know if different selection crit */
		mdbm_close(s->findkeydb);
		s->findkeydb = 0;
	}

	findkey = mdbm_mem();
	for (d = s->table; d; d = d->next) {
		if (flags && !(d->flags & flags)) continue;
		sccs_sdelta(s, d, key);
		k.dptr = key;
		k.dsize = strlen(key) + 1;
		v.dptr = (void*)&d;
		v.dsize = sizeof(d);
		if (mdbm_store(findkey, k, v, MDBM_INSERT)) {
			fprintf(stderr,
			    "findkey cache: insert error for %s\n", key);
			perror("insert");
			mdbm_close(findkey);
			return (0);
		}
	}
	s->findkeydb = findkey;
	return (findkey);
}

/*
 * Find an MD5 based key.  This is slow, it has to walk the whole table.
 * We walk in newest..oldest order hoping for something recent.
 */
delta	*
sccs_findMD5(sccs *s, char *md5)
{
	char	buf[MD5LEN];
	u32	date;
	delta	*d;

	sscanf(md5, "%08x", &date);
	if (s->tree->date > date) return (0);
	if (date > s->table->date) return (0);
	sccs_md5delta(s, s->tree, buf);
	if (streq(buf, md5)) return (s->tree);
	for (d = s->table; d; d = d->next) {
		unless (d->date == date) continue;
		sccs_md5delta(s, d, buf);
		if (streq(buf, md5)) return (d);
	}
	return (0);
}

int
isKey(char *key)
{
	int	i;

	if (strchr(key, '|')) return (1);
	if (isxdigit(key[0]) && (strlen(key) == 30)) {
		for (i = 1; i < 8; i++) unless (isxdigit(key[i])) return (0);
		for (; i < 30; i++) {
			unless (isalnum(key[i]) || (key[i] == '-') || (key[i] == '_')) {
				return (0);
			}
		}
		return (1);
	}
	return (0);
}

/*
 * Take a key like sccs_sdelta makes and find it in the tree.
 *
 * XXX - the findkeydb should be indexed by date and yield the
 * first delta table entry which matches that date.  Then we
 * could use it here and in sccs_findMD5().
 */
delta *
sccs_findKey(sccs *s, char *key)
{
	char	*parts[6];	/* user, host, path, date as integer */
	char	*user, *host, *path, *random;
	sum_t	cksum;
	sum_t	*cksump = 0;
	time_t	date;
	delta	*e;
	char	buf[MAXKEY];

	unless (s && HASGRAPH(s)) return (0);
	debug((stderr, "findkey(%s)\n", key));
	unless (strchr(key, '|')) return (sccs_findMD5(s, key));
	if (s->findkeydb) {	/* if cached by calling sccs_findKeyDB() */
		datum	k, v;
		k.dptr = key;
		k.dsize = strlen(key) + 1;
		v = mdbm_fetch(s->findkeydb, k);
		if (v.dsize) {
			memcpy(&e, v.dptr, sizeof(e));
			return (e);
		}
	}
	strcpy(buf, key);
	explodeKey(buf, parts);
	user = parts[0];
	host = parts[1];
	path = parts[2];
	/*
	 * We allow date2time to return 0 because of old Teamware deltas we
	 * did not fudge in the 3pardata import.
	 */
	date = date2time(parts[3], 0, EXACT);
	if (!date && !streq(&parts[3][2], "700101000000")) return (0);
	if (parts[4]) {
		/* If we went into the findkeyDB with a long key and failed
		 * then we aren't supposed to find this key.
		 */
		if (s->findkeydb) return (0);
		cksum = atoi(parts[4]);
		cksump = &cksum;
	}
	random = parts[5];
	if (samekey(s->tree, user, host, path, date, cksump))
		return (s->tree);
	for (e = s->table;
	    e && !samekey(e, user, host, path, date, cksump);
	    e = e->next);

	unless (e) return (0);

	/* Any delta may have random bits (grafted files) */
	if (random) unless (e->random && streq(e->random, random)) return (0);

	return (e);
}

void
sccs_print(delta *d)
{
	fprintf(stderr, "%c %s %s%s %s%s%s %d %d %u/%u/%u %s 0x%x\n",
	    d->type, d->rev,
	    d->sdate, d->zone ? d->zone : "",
	    d->user,
	    d->hostname ? "@" : "", d->hostname ? d->hostname : "",
	    d->serial, d->pserial,
	    d->added, d->deleted, d->same,
	    d->pathname ? d->pathname : "", d->flags);
}

/* return the time of the delta in UTC.
 * Do not change times without time zones to localtime.
 */
char *
sccs_utctime(delta *d)
{
	struct	tm *tp;
	static	char sdate[30];

	tp = utc2tm(getDate(d));
	sprintf(sdate, "%d%02d%02d%02d%02d%02d",
	    tp->tm_year + 1900,
	    tp->tm_mon + 1,
	    tp->tm_mday,
	    tp->tm_hour,
	    tp->tm_min,
	    tp->tm_sec);
	return (sdate);
}

/*
 * XXX why does this get an 'sccs *' ??
 */
void
sccs_pdelta(sccs *s, delta *d, FILE *out)
{
	assert(d);
	fprintf(out, "%s%s%s|%s|%s|%05u",
	    d->user,
	    d->hostname ? "@" : "",
	    d->hostname ? d->hostname : "",
	    d->pathname ? d->pathname : "",
	    sccs_utctime(d),
	    d->sum);
	if (d->random) fprintf(out, "|%s", d->random);
}

/* Get the checksum of the 5 digit checksum */
/*
 * XXX why does this get an 'sccs *' ??
 */
int
sccs_sdelta(sccs *s, delta *d, char *buf)
{
	char	*tail;
	int	len;

	assert(d);
	len = sprintf(buf, "%s%s%s|%s|%s|%05u",
	    d->user,
	    d->hostname ? "@" : "",
	    d->hostname ? d->hostname : "",
	    d->pathname ? d->pathname : "",
	    sccs_utctime(d),
	    d->sum);
	assert(len);
	unless (d->random) return (len);
	for (tail = buf; *tail; tail++);
	len += sprintf(tail, "|%s", d->random);
	return (len);
}

/*
 * This is really not an md5, it is <date><md5> so we can find the key fast.
 */
void
sccs_md5delta(sccs *s, delta *d, char *b64)
{
	char	*hash;
	char	key[MAXKEY+16];

	sccs_sdelta(s, d, key);
	if (s->tree->random) strcat(key, s->tree->random);
	hash = hashstr(key, strlen(key));
	sprintf(b64, "%08x%s", (u32)d->date, hash);
	free(hash);
}

/*
 * Given a delta, return the delta which is the cset marked one for the cset
 * which contains this delta.  Note Rick's cool code that handles going through
 * merges.
 * If the delta is not in a cset (i.e., it's pending) then return null.
 */
delta	*
sccs_csetBoundary(sccs *s, delta *d)
{
	delta	*e;

again:	
	/* find newest delta on this tree with cset mark */
	while (!(d->flags & D_CSET) && d->kid && (d->kid->type == 'D')) {
		d = d->kid;
	}
	/* might be merge side of a cset created by resolve (see test) */
	if (!(d->flags & D_CSET) && (d->flags & D_MERGED)) {
		for (e = s->table; e && e != d; e = e->next) {
			if (e->merge == d->serial) {
				d = e;
				goto again;
			}
		}
		fprintf(stderr,
		    "ERROR: delta %s marked merge, but no merge found\n",
		    d->rev);
		exit(1);
	}
	return ((d->flags & D_CSET) ? d : 0);
}

/*
 * Identify SCCS files that the user creates.  Seperate from
 * BK files.  Basicly the ChangeSet file and anything created
 * in BitKeeper/ is a system file.  The "created" is because of
 * BitKeeper/deleted...
 */
int
sccs_userfile(sccs *s)
{
	char	*pathname;

	if (CSET(s))		/* ChangeSet */
		return 0;

	pathname = sccs_ino(s)->pathname;
	if (strneq(pathname, "BitKeeper/", 10))
		return 0;
	return 1;
}

void
sccs_shortKey(sccs *s, delta *d, char *buf)
{
	assert(d);
	sprintf(buf, "%s%s%s|%s|%s",
	    d->user,
	    d->hostname ? "@" : "",
	    d->hostname ? d->hostname : "",
	    d->pathname ? d->pathname : "",
	    sccs_utctime(d));
}

/*
 * Take in a string like what pdelta spits out and break it into the
 * parts.
 */
void
explodeKey(char *key, char *parts[6])
{
	char	*s;

	/* user[@host]|sccs/slib.c|19970518232929[|23330|[233de234]] */
	/* user[@host]|path|date[|cksum|[random]] */

	/* parts[0] = user[@host] */
	for (s = key; *key && (*key != '|'); key++);
	parts[0] = s;
	assert(key);
	*key++ = 0;

	/* parts[2] = path or NULL if no path listed, but || */
	for (s = key; *key && (*key != '|'); key++);
	parts[2] = s == key ? 0 : s;
	assert(key);
	*key++ = 0;

	/* parts[3] = utc fudged time */
	for (s = key; *key && (*key != '|'); key++);
	parts[3] = s;

	/* if more data .... it's a cksum or maybe null field? */
	if (*key) {
		*key++ = 0;
		for (s = key; *key && (*key != '|'); key++);
		parts[4] = s == key ? 0 : s;
	}
	else {
		parts[4] = 0;
	}
	/* if more data .... random string */
	if (*key) {
		*key++ = 0;
		for (s = key; *key && (*key != '|'); key++);
		parts[5] = s == key ? 0 : s;
	}
	else {
		parts[5] = 0;
	}

	assert(!*key);

	/* go back and split user@host to user and host */
	for (key = parts[0]; *key && (*key != '@'); key++);
	if (key = strchr(parts[0], '@')) {
		*key++ = 0;
		parts[1] = key;
	} else {
		parts[1] = 0;
	}
}

/*
 * Sun has this sort of entry on files which have no common heritage.
 * Seems reasonable to use the first delta which is real to get something
 * unique.
 * XXX - this is a good reason to listen to Rick about putting the id elsewhere.
 */
delta *
sccs_ino(sccs *s)
{
	delta	*d = s->tree;

	if (streq(d->sdate, "70/01/01 00:00:00") && streq(d->user, "Fake")) {
		d = d->kid;
	}
	return (d);
}

int
sccs_reCache(int quiet)
{
	char	*av[4];
	int	i;

	av[0] = "bk";  av[1] = "idcache"; 
	if (quiet) {
		av[2] = "-q";
	} else {
		av[2] = 0;
	}
	av[3] = 0;
	i = spawnvp(_P_WAIT, av[0], av);
	unless (WIFEXITED(i)) return (1);
	return (WEXITSTATUS(i));
}

/*
 * Figure out if the file is gone from the DB.
 * XXX - currently only works with keys, not filenames.
 */
int
gone(char *key, MDBM *db)
{
	unless (db) return (0);
	unless (strchr(key, '|')) return (0);
	return (mdbm_fetch_str(db, key) != 0);
}

MDBM	*
loadDB(char *file, int (*want)(char *), int style)
{
	MDBM	*DB = 0;
	FILE	*f = 0;
	char	*v;
	int	idcache = 0, first = 1, quiet = 1;
	int	flags;
	char	buf[MAXLINE];
	char	*av[5];
	u32	sum = 0;


	// XXX awc->lm: we should check the z lock here
	// someone could be updating the file...
	idcache = strstr(file, IDCACHE) ? 1 : 0;
again:	unless (f = fopen(file, "rt")) {
		if (first && idcache) {
recache:		first = 0;
			sum = 0;
			if (f) fclose(f);
			if (DB) mdbm_close(DB), DB = 0;
			if (sccs_reCache(quiet)) goto out;
			goto again;
		}
		if (first && streq(file, GONE) && exists(SGONE)) {
			first = 0;
			/* get -s */
			av[0] = "bk"; av[1] = "get"; av[2] = "-q";
			av[3] = GONE; av[4] = 0;
			spawnvp(_P_WAIT, av[0], av);
			goto again;
		}
out:		if (f) fclose(f);
		if (DB) mdbm_close(DB), DB = 0;
		return (0);
	}
	DB = mdbm_mem();
	assert(DB);
	switch (style & (DB_NODUPS|DB_USEFIRST|DB_USELAST)) {
	    case DB_NODUPS:	flags = MDBM_INSERT; break;
	    case DB_USEFIRST:	flags = MDBM_INSERT; break;
	    case DB_USELAST:	flags = MDBM_REPLACE; break;
	    default:
		fprintf(stderr, "Bad option to loadDB: %x\n", style);
		exit(1);
	}
	while (fnext(buf, f)) {
		if (buf[0] == '#') {
			if (strneq(buf, "#$sum$ ", 7)) {
				if (atoi(&buf[7]) == sum) {
					idcache = 2;	/* OK */
					break;		/* done */
				}
				if (first) {
					fprintf(stderr,
					    "Bad idcache chksum %u:%u, ",
					    atoi(&buf[7]), sum);
					quiet = 0;
					goto recache;
				}
			}
			continue;
		}
		if (want && !want(buf)) continue;
		if (idcache) {
			u8	*u;

			for (u = buf; *u; sum += *u++);
		}
		if (chop(buf) != '\n') {
			if (first && idcache) {
				fprintf(stderr, "Detected bad idcache, ");
				quiet = 0;
				goto recache;
			}
			fprintf(stderr, "bad path: <%s> in %s\n", buf, file);
			mdbm_close(DB), DB = 0;
			return (0);
		}
		if (style & DB_KEYSONLY) {
			v = "";
		} else {
			if (style & DB_KEYFORMAT) {
				v = separator(buf);
			} else {
				v = strchr(buf, ' ');
			}
			if (!v && (style & DB_NOBLANKS)) continue;
			unless (v) {
				if (first && idcache) {
					fprintf(stderr,
					    "Detected bad idcache, ");
					quiet = 0;
					goto recache;
				}
				fprintf(stderr, "Corrupted DB %s\n", file);
				mdbm_close(DB), DB = 0;
				return (0);
			}
			*v++ = 0;
		}
		switch (mdbm_store_str(DB, buf, v, flags)) {
		    case 0: break;
		    case 1:
		    	if ((style == DB_NODUPS)) {
				fprintf(stderr,
				    "Duplicate key '%s' in %s.\n", buf, file);
				fprintf(stderr,
				    "\tvalue: %s\n\tvalue: %s\n",
				    mdbm_fetch_str(DB, buf), v);
				goto out;
			}
			break;
		    default:
			fprintf(stderr, "loadDB(%s) failed\n", file);
			goto out;
		}
	}
	if (idcache && (idcache != 2)) {
		fprintf(stderr, "No checksum trailer in idcache, ");
		quiet = 0;
		goto recache;
	}
	fclose(f);
	return (DB);
}

/*
 * Get all the ids associated with a changeset.
 * The db is db{root rev Id} = cset rev Id.
 *
 * Note: does not call sccs_restart, the caller of this sets up "s".
 */
int
csetIds_merge(sccs *s, char *rev, char *merge)
{
	kvpair	kv;
	char	*t;

	assert(HASH(s));
	if (sccs_get(s, rev, merge, 0, 0, SILENT|GET_HASHONLY, 0)) {
		sccs_whynot("get", s);
		return (-1);
	}
	unless (s->mdbm) {
		fprintf(stderr, "get: no mdbm found\n");
		return (-1);
	}

	/* If we are the new key format, then we shouldn't have mixed keys */
	if (LONGKEY(s)) return (0);

	/*
	 * If there are both long and short keys, then use the long form
	 * and delete the short form (the long form is later).
	 */
	for (kv = mdbm_first(s->mdbm); kv.key.dsize; kv = mdbm_next(s->mdbm)) {
		unless (t = sccs_iskeylong(kv.key.dptr)) continue;
		*t = 0;
		if (mdbm_fetch_str(s->mdbm, kv.key.dptr)) {
			mdbm_delete_str(s->mdbm, kv.key.dptr);
		}
		*t = '|';
	}
	return (0);
}

int
csetIds(sccs *s, char *rev)
{
	return (csetIds_merge(s, rev, 0));
}

/*
 * Scan key and return:
 * Location of '|' which starts long key part
 * or the NULL pointer (0) if it was already short.
 * Does not modify the string (though the caller can do a *t=0 upeon return
 * to make a short key).
 */
char *
sccs_iskeylong(char *t)
{
	assert(t);
	for ( ; *t && *t != '|'; t++);
	assert(t);
	for (t++; *t && *t != '|'; t++);
	assert(t);
	for (t++; *t && *t != '|'; t++);
	unless (*t) return (0);
	assert(*t == '|');
	return (t);
}

/*
 * Translate a key into an sccs struct.
 * If it is in the idDB, use that, otherwise use the name in the key.
 * Return NULL if we can't find (i.e., if there is no s.file).
 * Return NULL if the file is there but does not have the same root inode.
 */
sccs	*
sccs_keyinit(char *key, u32 flags, MDBM *idDB)
{
	datum	k, v;
	char	*p;
	sccs	*s;
	char	*localkey = 0;
	delta	*d;
	char	buf[MAXKEY];

	/*
	 * Id cache contains both long and short keys
	 * so we don't need to look things up as long then short.
	 */
	k.dptr = key;
	k.dsize = strlen(key) + 1;
	v  = mdbm_fetch(idDB, k);
	if (v.dsize) {
		p = name2sccs(v.dptr);
	} else {
		char	*t, *r;

		for (t = k.dptr; *t++ != '|'; );
		for (r = t; *r != '|'; r++);
		assert(*r == '|');
		*r = 0;
		p = name2sccs(t);
		*r = '|';
	}
	s = sccs_init(p, flags);
	free(p);
	unless (s && HAS_SFILE(s))  goto out;

	/*
	 * Go look for this key in the file.
	 * If we are a grafted together file, any root key is a match.
	 */
	d = sccs_ino(s);
	do {
		sccs_sdelta(s, d, buf);

		/* modifies buf and key, so copy key to local key */
		localkey = strdup(key);
		assert(localkey);
		if (samekeystr(buf, localkey)) {
			free(localkey);
			return (s);
		}
		free(localkey);
		localkey = 0;
		unless (s->grafted) goto out;
		while (d = d->next) {
			if (d->random) break;
		}
	} while (d);

out:	if (s) {
		sccs_free(s);
	}
	if (localkey) free(localkey);
	return (0);
}

void
sccs_color(sccs *s, delta *d)
{
        unless (d && !(d->flags & D_RED)) return;
        assert(d->type == 'D');
        sccs_color(s, d->parent);
        if (d->merge) sccs_color(s, sfind(s, d->merge));
        d->flags |= D_RED;
}

/*
 * Given an SCCS structure with a list of marked deltas, strip them from
 * the delta table and place the striped body in out
 */
int
stripDeltas(sccs *s, FILE *out)
{
	serlist *state = 0;
	ser_t	*slist = 0;
	int	print = 0;
	char	*buf;
	int	ser;

	slist = setmap(s, D_SET, 1);

	state = allocstate(0, s->nextserial);
	seekto(s, s->data);
	if (s->encoding & E_GZIP) {
		s->zin = zgets_init(s->where, s->size - s->data);
		sccs_zputs_init(s, out);
	}
	while (buf = nextdata(s)) {
		if (isData(buf)) {
			unless (print) fputdata(s, buf, out);
			continue;
		}
		debug2((stderr, "%.*s", linelen(buf), buf));
		ser = atoi(&buf[3]);
		if (slist[ser] == 0) fputdata(s, buf, out);
		changestate(state, buf[1], ser);
		print =
		    visitedstate((const serlist*)state, (const ser_t*)slist);
	}
	free(state);
	free(slist);
	if (fflushdata(s, out)) return (1);
	if (s->encoding & E_GZIP) {
		if (zgets_done(s->zin)) return (1);
		s->zin = 0;
	}
	fseek(out, 0L, SEEK_SET);
	fprintf(out, "\001%c%05u\n", BITKEEPER(s) ? 'H' : 'h', s->cksum);
	sccs_close(s);
	fclose(out);
	buf = sccsXfile(s, 'x');
	if (rename(buf, s->sfile)) {
		fprintf(stderr,
		    "stripdel: can't rename(%s, %s) left in %s\n",
		    buf, s->sfile, buf);
		return (1);
	}
	sccs_setStime(s, 0);
	chmod(s->sfile, 0444);
	return (0);
}

/*
 * Strip out the deltas marked with D_SET and D_GONE.
 * E.g. to remove top rev: 
 * delta *d = sccs_top(s); MK_GONE(d); d->flags |= D_SET;
 * sccs_stripdel(s, "die");
 */
int
sccs_stripdel(sccs *s, char *who)
{
	FILE	*sfile = 0;
	int	error = 0;
	int	locked;
	delta	*e;

#define	OUT	\
	do { error = -1; s->state |= S_WARNED; goto out; } while (0)

	assert(s && HASGRAPH(s) && !HAS_PFILE(s));
	debug((stderr, "stripdel %s %s\n", s->gfile, who));
	unless (locked = sccs_lock(s, 'z')) {
		fprintf(stderr, "%s: can't get lock on %s\n", who, s->sfile);
		OUT;
	}


	if (stripChecks(s, 0, who)) OUT;

	unless (sfile = fopen(sccsXfile(s, 'x'), "w")) {
		fprintf(stderr,
		    "%s: can't create %s: ", who, sccsXfile(s, 'x'));
		perror("");
		OUT;
	}

	/*
	 * find the new top-of-trunk
	 * XXX Is this good enough ??
	 */
	e = sccs_top(s);
	assert(e);
	s->xflags = sccs_xflags(e);

	/* write out upper half */
	if (delta_table(s, sfile, 0)) {  /* 0 means as-is, so chksum works */
		fprintf(stderr,
		    "%s: can't write delta table for %s\n", who, s->sfile);
		sccs_unlock(s, 'x');
		fclose(sfile);
		OUT;
	}

	/* write out the lower half */
	if (stripDeltas(s, sfile)) {
		fprintf(stderr,
		    "%s: can't write delta body for %s\n", who, s->sfile);
		sccs_unlock(s, 'x');
		OUT;
	}
#undef	OUT

out:
	/* sfile closed by stripDeltas() */
	if (locked) sccs_unlock(s, 'z');
	debug((stderr, "stripdel returns %d\n", error));
	return (error);
}

/*
 * Remove the delta, leaving a delta of type 'R' behind.
 */
int
sccs_rmdel(sccs *s, delta *d, u32 flags)
{
	d->flags |= D_SET;
	if (stripChecks(s, d, "rmdel")) return (1);

	d->type = 'R';	/* mark delta as Removed */
	d->flags &= ~D_CKSUM;

	return (sccs_stripdel(s, "rmdel"));
}

/*
 * Make sure it is OK to remove a delta.
 */
private int
stripChecks(sccs *s, delta *d, char *who)
{
	if (checkGone(s, D_SET, who)) return (1);

	unless (d) return (0);
	/*
	 * Do not let them remove the root.
	 */
	unless (d->parent) {	/* don't remove if this is 1.1 (no parent) */
		fprintf(stderr,
			"%s: can't remove root change %s in %s.\n",
			who, d->rev, s->sfile);
		s->state |= S_WARNED;
		return (1);
	}
	return (0);
}
/* TIMESTAMP HANDLING */
/*
 * timestampDBChanged is set when entries in the database have changed but
 * not deleted. This means the disk file may contain stale data if no
 * existing entries are modified or new ones added. This does not matter
 * as the checks that caused the entry to be deleted will fail next time
 */
#define	TIMESTAMPS	"BitKeeper/log/timestamps"

private int timestampDBChanged = 0;

/*
 * we need to parse the timestamp file. Its format is:
 * relative/file/path|gfile_mtime|gfile_size|permissions|sfile_mtime|sfile_size
 */
private int
parseTimestamps(char *buf, tsrec *ts)
{
	char	*p;

	/* parse gfile_mtime */
	ts->gfile_mtime = strtoul(buf, &p, 16);
	unless (p && (*p == BK_FS)) return (0);		/* bogus entry */

	/* parse gfile size */
	ts->gfile_size = strtoul(p + 1, &p, 10);
	unless (p && (*p == BK_FS)) return (0);		/* bogus entry */

	/* parse permissions */
	ts->permissions = strtoul(p + 1, &p, 8);
	unless (p && (*p == BK_FS)) return (0);		/* bogus entry */

	/* parse sfile_mtime */
	ts->sfile_mtime = strtoul(p + 1, &p, 16);
	unless (p && (*p == BK_FS)) return (0);		/* bogus entry */

	/* parse sfile_size */
	ts->sfile_size = strtoul(p + 1, &p, 10);
	unless (p && (*p == 0)) return (0);		/* bogus entry */

	return (1);
}

/*
 * generate a timestamp database - cannot use loadDB as it can't cope with
 * this format - or I couldn't figure out how! (andyc).
 */
hash *
generateTimestampDB(project *p)
{
	/* want to use the timestamp database */
	FILE	*f = 0;
	hash	*db;
	char	*tsname;
	char	buf[MAXLINE];

	assert(p);
	if (streq(proj_configval(p, "clock_skew"), "off")) return (0);

	tsname = aprintf("%s/%s", proj_root(p), TIMESTAMPS);
	db = hash_new(HASH_MEMHASH);
	assert(db);
	if (f = fopen(tsname, "r")) {
		while (fnext(buf, f)) {
			tsrec	*ts;
			char	*p;

			chomp(buf);
			unless (p = strchr(buf, BK_FS)) {
bad:				/* the database has invalid information in it
				 * so get it regenerated
				 */
				timestampDBChanged = 1;
				continue;
			}
			*p++ = 0;
			unless (ts = hash_fetchStr(db, buf)) {
				ts = hash_store(db,
				    buf, strlen(buf)+1, 0, sizeof(tsrec));
			}
			unless (parseTimestamps(p, ts)) goto bad;
		}
		fclose(f);
	}
	free(tsname);
	return (db);
}

void
dumpTimestampDB(project *p, hash *db)
{
	FILE	*f = 0;
	char	*tsname;

	assert(p);
	unless (timestampDBChanged) return;

	tsname = aprintf("%s/%s", proj_root(p), TIMESTAMPS);
	unless (f = fopen(tsname, "w")) {
		free(tsname);
		return;			/* leave stale timestamp file there */
	}
	EACH_HASH(db) {
		tsrec   *ts = (tsrec *)db->vptr;

		assert(db->vlen == sizeof(*ts));
		fprintf(f, "%s%c%lx%c%ld%c0%o%c%lx%c%ld\n",
		    db->kptr, BK_FS,
		    ts->gfile_mtime, BK_FS, ts->gfile_size, BK_FS,
		    ts->permissions, BK_FS,
		    ts->sfile_mtime, BK_FS, ts->sfile_size);
		if (ferror(f)) {
			/* some error writing the timestamp db so delete it */
			unlink(tsname);
			break;
		}
	}
	free(tsname);
	fclose(f);
}

/*
 * timeMatch checks our file of timestamps against the current timestamps
 * of the given file
 */
int
timeMatch(project *proj, char *gfile, char *sfile, hash *timestamps)
{
	tsrec	*ts;
	char	*relpath;
	int	ret = 0;
	struct	stat	sb;

	assert(proj);
	relpath = proj_relpath(proj, gfile);
	ts = (tsrec *)hash_fetchStr(timestamps, relpath);
	free(relpath);
	unless (ts) goto out;			/* no entry for file */

	if (lstat(gfile, &sb) != 0) goto out;	/* might not exist */

	if ((sb.st_mtime != ts->gfile_mtime) ||
	    (sb.st_size != ts->gfile_size) ||
	    (sb.st_mode != ts->permissions)) {
		goto out;			/* gfile doesn't match */
	}
	if (lstat(sfile, &sb) != 0) {
		/* We should never get here */
		perror(sfile);
		goto out;
	}
	if ((sb.st_mtime != ts->sfile_mtime) ||
	    (sb.st_size != ts->sfile_size)) {
		goto out;			/* sfile doesn't match */
	}

	/* as far as we're concerned, the file hasn't changed */
	ret = 1;
 out:	return (ret);
}

void
updateTimestampDB(sccs *s, hash *timestamps, int different)
{
	tsrec	ts;
	struct	stat	sb;
	char	*relpath;
	static time_t clock_skew = 0;
	time_t now;

	assert(s->proj);
	unless (clock_skew) {
		char	*p = proj_configval(s->proj, "check_skew");

		if (streq(p, "off")) {
			clock_skew = 2147483647;  /* 2^31 */
		} else {
			unless (clock_skew = strtoul(p, 0,0)) clock_skew = WEEK;
		}
	}

	relpath = proj_relpath(s->proj, s->gfile);
	if (different) {
		hash_deleteStr(timestamps, relpath);
		goto out;
	}

	if (lstat(s->gfile, &sb) != 0) goto out;	/* might not exist */
	ts.gfile_mtime = sb.st_mtime;
	ts.gfile_size = sb.st_size;
	ts.permissions = sb.st_mode;

	if (lstat(s->sfile, &sb) != 0) {	/* We should never get here */
		perror(s->sfile);
		goto out;
	}
	ts.sfile_mtime = sb.st_mtime;
	ts.sfile_size = sb.st_size;

	now = time(0);
	if ((now - ts.gfile_mtime) < clock_skew) {
		hash_deleteStr(timestamps, relpath);
	} else {
		hash_store(timestamps,
		    relpath, strlen(relpath) + 1, &ts, sizeof(ts));
		timestampDBChanged = 1;
	}
out:	free(relpath);
}

