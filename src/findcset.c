#include "system.h"
#include "sccs.h"
#include "range.h"

#define	MIN_GAP	(2*HOUR)
#define	MAX_GAP	(30*DAY)

typedef struct dinfo dinfo;

private	int	compar(const void *a, const void *b);
private	void	sortDelta(int flags);
private	void	findcset(void);
private	void	mkList(sccs *s, int fileIdx);
private void	dumpCsetMark(void);
private dinfo	*findFirstDelta(sccs *s, dinfo *first);
private void	mkDeterministicKeys(void);
private	int	openTags(char *tagfile);
private	char *	readTag(time_t *tagDate);
private	void	closeTags(void);
private int	do_patch(sccs *s, delta *d, char *tag,
		    char *tagparent, FILE *out);
private	void	preloadView(sccs *s, MDBM *db, delta *d);
private	void	free_dinfo(void *x);
private	dinfo	*delta2dinfo(sccs *s, delta *d);

private	char	**list;
private	char	**flist, **keylist;
private	MDBM	*csetBoundary;
private	int	deltaCounter;

typedef	struct {
	u32	timeGap;		/* Time gap (in minutes) */
	u32	singleUserCset:1;	/* Force one user per cset */
	u32	noSkip:1;		/* do not skip recent deltas */
	u32	ensemble:1;		/* shorten comments */
	u32	BKtree:1;		/* findset in existing bk tree */
	u32	verbose;		/* 1: basic, 2:debug */
	u32	ignoreCsetMarker:1;	/*
					 * Strip/re-do existing
					 * Cset & Cset Marker
					 * for debugging only
					 */
	u32	blackOutTime;		/*
					 * Ignore delta Younger then
					 * "blackOutTime" (in month)
					 * for debugging only.
					 */
} fopts;

/* info remembered from each delta */
struct dinfo {
	time_t	date;
	int	dateFudge;
	sum_t	sum;
	char	*rev;
	char	*zone;
	char	*pathname;
	char	*user;
	char	*hostname;
	char	*comments;
	char	*dkey;
	int	f_index;
};

private	fopts	opts;
private	dinfo	*oldest;		/* The oldest user delta */


int
findcset_main(int ac, char **av)
{
	sccs	*s;
	char	*name;
	char	*tagFile = 0;
	char	key[MAXKEY];
	int	save, c, flags = SILENT|INIT_MUSTEXIST;
	int	fileIdx;

	while ((c = getopt(ac, av, "b:Bkt:T:ivu", 0)) != -1) {
		switch (c)  {
		    case 'b' : opts.blackOutTime = atoi(optarg); break;
		    case 'B' : opts.BKtree = 1; break;
		    case 'k'  : opts.noSkip++; break;
		    case 't' : opts.timeGap = atoi(optarg); break;
		    case 'T' : tagFile = optarg; break;
		    case 'i' : opts.ignoreCsetMarker = 1; break; /* for debug */
		    case 'u' : opts.singleUserCset = 1; break;
		    case 'v' : opts.verbose++; flags &= ~SILENT; break;
		    default: bk_badArg(c, av);
		}
	}

	has_proj("findcset");
	if (proj_isEnsemble(0)) opts.ensemble = 1;

	if (tagFile && openTags(tagFile)) return (1);

	/*
	 * If -i option is set, we need to:
	 * a) clear D_CSET mark for all delta after 1.1 in the ChangeSet file
	 * b) strip for all delta after 1.1 in the ChangeSet file
	 */
	if (opts.ignoreCsetMarker) {
		delta	*d;
		int	didone = 0;

		s = sccs_init(CHANGESET, INIT_NOCKSUM|flags);
		assert(s);
		for (d = s->table; d; d = NEXT(d)) {
			if (!streq(REV(s, d), "1.0") &&
			    !streq(REV(s, d), "1.1")) {
				d->flags &= ~D_CSET;
				d->flags |= D_SET|D_GONE;
				didone = 1;
			}
		}
		if (didone) {
			if (sccs_stripdel(s, "findcset")) {
				sccs_free(s);
				return (1);
			}
		}
		sccs_free(s);
		system("bk -r admin -D");
		system("bk cset -C");
	}

	fileIdx = 0;
	csetBoundary = mdbm_mem();
	for (name = sfileFirst("findcset", &av[optind], 0);
	    name; name = sfileNext()) {
		unless (s = sccs_init(name, INIT_NOCKSUM|flags)) {
			continue;
		}
		unless (sccs_userfile(s) ||
		    (opts.BKtree && !streq(name, CHANGESET))) {
			sccs_free(s);
			verbose((stderr, "Skipping non-user file %s.\n", name));
			continue;
		}
		save = deltaCounter;
		sccs_sdelta(s, sccs_ino(s), key);
		keylist = addLine(keylist, strdup(key));
		flist = addLine(flist, strdup(s->sfile));
		oldest = findFirstDelta(s, oldest);
		mkList(s, ++fileIdx);
		//verbose((stderr,
		//    "%s: %d deltas\n", s->sfile, deltaCounter - save));
		sccs_free(s);
	}
	sfileDone();
	verbose((stderr, "Total %d deltas\n", deltaCounter));
	if (deltaCounter) {
		sortDelta(flags);
		mkDeterministicKeys();
		findcset();
		dumpCsetMark();
	}
	closeTags();
	mdbm_close(csetBoundary);
	freeLines(list, free_dinfo);
	sysio(NULL, DEVNULL_WR, DEVNULL_WR, "bk", "-R", "sfiles", "-P", SYS);

	/* update rootkey embedded in files */
	unlink("BitKeeper/log/ROOTKEY");
	proj_reset(0);
	return (0);
}

private	int
compar(const void *a, const void *b)
{
	dinfo	*d1, *d2;

	d1 = *((dinfo**)a);
	d2 = *((dinfo**)b);
	return (d1->date - d2->date);
}

private	void
sortDelta(int flags)
{
	verbose((stderr, "Sorting...."));
	qsort(list+1, deltaCounter, sizeof(dinfo *), compar);
	verbose((stderr, "done.\n"));
}

/*
 * Save the rev that needs a cset marker
 * This assumes the rev associated with a node stay constant during the
 * the findcset processing. This is safe because we only rename a rev
 * when we are in bk resolve.
 */
private void
saveCsetMark(ser_t findex, char *rev)
{
	datum	k, v, tmp;
	char	**revlist, **tmplist = 0;

	k.dptr = (char *) &findex;
	k.dsize = sizeof (ser_t);
	tmp = mdbm_fetch(csetBoundary, k);
	if (tmp.dptr) memcpy(&tmplist, tmp.dptr, sizeof (char **));
	revlist = addLine(tmplist, strdup(rev));
	v.dptr = (char *) &revlist;
	v.dsize = sizeof (char **);
	if (revlist != tmplist) {
		int ret = mdbm_store(csetBoundary, k, v, MDBM_REPLACE);
		assert(ret == 0);
	}
}

/*
 * Walk thru all files and update cset mark
 */
private void
dumpCsetMark(void)
{
	kvpair	kv;
	char	**revlist;
	char	*gfile;
	sccs	*s;
	delta	*d;
	int	flags = INIT_NOCKSUM|SILENT;
	int	i;
	ser_t	findex;

	if (opts.verbose > 1) fprintf(stderr, "Updating cset mark...\n");
	EACH_KV(csetBoundary) {
		memcpy(&findex, kv.key.dptr, sizeof (ser_t));
		memcpy(&revlist, kv.val.dptr, sizeof (char **));

		gfile = sccs2name(flist[findex]);
		if (opts.verbose > 1) fprintf(stderr, "%s\n", gfile);
		free(gfile);

		s = sccs_init(flist[findex], flags);
		assert(s);

		/*
		 * Set new cset mark
		 */
		EACH(revlist) {
			d = sccs_findrev(s, revlist[i]);
			assert(d);
			d->flags |= D_CSET;
			if (opts.verbose > 1) {
				fprintf(stderr, "\t%s\n", REV(s, d));
			}
		}
		sccs_newchksum(s);
		sccs_free(s);
		freeLines(revlist, free);
	}
	mdbm_close(csetBoundary);
	csetBoundary = 0;
}

/*
 *  Compute the time gap that seperate two csets.
 */
private int
time_gap(time_t t)
{
	static time_t now = 0;
	time_t	i, gap;

	/*
	 * Easy case, user wants constant time gap
	 */
	if (opts.timeGap) {
		gap = (opts.timeGap * MINUTE);
		goto done;
	}

	/*
	 * Increase the time gap as we go further into the pass
	 */
	unless (now) now = time(0);
	t = now - t;

	i = 3 * MONTH;
	gap = MIN_GAP;
	while (i < t) {
		gap *= 2;
		i *= 2;
		if (i >= MAX_GAP) {
			i = MAX_GAP;	/* Cap out at MAX_GAP */
			break;
		}
	}

done:	return (gap);
}

/*
 * Return true if d1 and d2 are separated by a cset boundary
 * "hasTag" is a outgoing parameter; set to one if the new cset is tagged.
 *
 * Cset boundary is determined by one of the following
 * a) tag bounary
 * b) time gap boundary
 * c) user boundary
 */
private int
isCsetBoundary(sccs *s, dinfo *d1, dinfo *d2,
    time_t tagDate, time_t now, int *skip)
{

	assert(d1->date <= d2->date);

	/*
	 * For debugging:
	 * Ignore delta older than N months.
	 * N = opts.blackOutTime.
	 */

	if (opts.blackOutTime &&
	    (d2->date < (now - (opts.blackOutTime * MONTH)))) {
		(*skip)++;
		return (0);
	}

	/* (tagDate == 0) => no tag */
	if (tagDate && d1->date <= tagDate && d2->date > tagDate) return (1);

	/*
	 * Ignore delta with fake user
	 */
	if ((d1->date == 0) && streq(d1->user, "Fake")) {
		return (0);
	}
	if ((d2->date == 0) && streq(d2->user, "Fake")) {
		return (0);
	}

	/*
	 * Check time gap
	 */
	if ((d2->date - d1->date) >= time_gap(d2->date)) {
		return (1);
	}

	/*
	 * Change user boundary
	 */
	if (opts.singleUserCset && !streq(d1->user, d2->user)) {
		if (opts.verbose > 1) {
			fprintf(stderr,
			    "Splitting cset on user boundary: %s/%s\n",
			    d1->user, d2->user);
		}
		return (1);
	}

	return (0);
}

/*
 * Return true if blank line
 */
private int
isBlank(char *p)
{
	while (*p) {
		unless (isspace(*p++))  return (0);
	}
	return (1);
}

/*
 * Convert a string into line array
 */
private char **
str2line(char **lines, char *prefix, char *str)
{
	char	*p, *q;

	q = p = str;
	while (1) {
		if (!*q) break;
		if (*q == '\n') {
			*q = 0;
			lines = addLine(lines, aprintf("%s%s", prefix, p));
			*q++ = '\n';
			p = q;
		} else {
			q++;
		}
	}
	return (lines);
}

/*
 * sort function for sorting an array of strings shortest first
 */
private int
bylen(const void *a, const void *b)
{
	char	*achar = *(char **)a;
	char	*bchar = *(char **)b;
	int	alen = strlen(achar);
	int	blen = strlen(bchar);

	if (alen != blen) return (alen - blen);
	return (strcmp(achar, bchar));
}

/*
 * Convert cset comment store in a mdbm into lines format
 */
private char **
db2line(MDBM *db)
{
	kvpair	kv;
	char	**lines = 0, **glist, **comments = 0;
	char	*comment, *gfiles, *lastg, *p;
	char	*many = "Many files:";
	int	i, len = 0;
	MDBM	*gDB;

	/* sort comments from shortest to longest */
	EACH_KV(db) comments = addLine(comments, kv.key.dptr);
	sortLines(comments, bylen);
	lastg = "";
	EACH (comments) {
		comment = comments[i];

		kv.key.dptr = comment;
		kv.key.dsize = strlen(comment) + 1;
		kv.val = mdbm_fetch(db, kv.key);

		/*
		 * Compute length of file list
		 */
		memcpy(&gDB, kv.val.dptr, sizeof (MDBM *));
		len = 0;
		EACH_KV(gDB) {
			len += strlen(kv.key.dptr) + 2;
		}
		len += 2;

		/*
		 * Extract gfile list and set it as the section header
		 * Skip gfile list if too long
		 */
		if (len <= 80) {
			glist = 0;
			EACH_KV(gDB) {
				glist = addLine(glist, strdup(kv.key.dptr));
			}
			sortLines(glist, 0);
			p = joinLines(", ", glist);
			freeLines(glist, free);
			gfiles = malloc(strlen(p) + 2);
			sprintf(gfiles, "%s:", p);
			free(p);
		} else {
			gfiles = strdup(many);
		}

		/*
		 * If gfile list is same as previous, skip
		 */
		if (opts.ensemble && streq(gfiles, "ChangeSet:")) {
			// nothing
			;
		} else unless (streq(lastg, gfiles)) {
			lines = addLine(lines, gfiles);
		}
		lastg = gfiles;

		/*
		 * Now extract the comment block
		 */
		lines = str2line(lines, "  ", comment);
	}
	freeLines(comments, 0);
	return (lines);
}

/*
 * Return TURE if s1 is same as s2
 */
private int
sameStr(char *s1, char *s2)
{
	if (s1 == NULL) {
		if (s2 == NULL) return (1);
		return (0);
	}

	/* When we get here, s1 != NULL */
	if (s2 == NULL) return (0);

	/* When we get here, s1 != NULL && s2 != NULL */
	return (streq(s1, s2));
}

/*
 * Fix up date/timezone/user/hostname of delta 'd' to match 'template'
 */
private  void
fix_delta(sccs *s, dinfo *template, delta *d, int fudge)
{
	delta *parent;

	if ((opts.verbose > 1) && (fudge != -1)) {
		if ((template->date == d->date) &&
		    sameStr(template->zone, ZONE(s, d)) &&
		    sameStr(template->user, USER(s, d)) &&
		    sameStr(template->hostname, HOSTNAME(s, d))) {
			return;
		}
		fprintf(stderr,
		    "Fixing ChangeSet rev %s to match oldest delta: %s %s",
		    REV(s, d), template->rev, template->user);
		fprintf(stderr, " %s\n",
				template->hostname ? template->hostname : "");
	}

	/*
	 * Fix time zone and date
	 */
	parent = PARENT(s, d);
	if (template->zone) {
		d->zone = sccs_addUniqStr(s, template->zone);
	} else {
		d->zone = 0;
	}

	d->date = template->date;
	if (fudge != -1) { /* -1 => do not fudge the date */
		d->dateFudge = template->dateFudge + fudge;
		d->date += fudge;
	} else {
		assert(d->dateFudge == 0);
	}

	/*
	 * Copy user
	 */
	assert(template->user);
	if (parent && streq(USER(s, parent), template->user)) {
		d->user = parent->user;
	} else {
		d->user = sccs_addUniqStr(s, template->user);
	}

	/*
	 * Copy hostname
	 */
	if (template->hostname) {
		d->hostname = sccs_addUniqStr(s, template->hostname);
	} else {
		d->hostname = 0;
	}
}

/*
 * Make sure the 1.0 and 1.1 delta
 * in the ChangeSet file is looks like is created around the same time
 * as the first delta
 */
private void
mkDeterministicKeys(void)
{
	int	iflags = INIT_NOCKSUM|INIT_MUSTEXIST|SILENT;
	sccs	*cset;
	delta	*d2;

	cset = sccs_init(CHANGESET, iflags);
	assert(cset);

	/*
	 * Fix 1.0 delta of the ChangeSet file
	 * We need to do this first, so "bk new" of IGNORE
	 * and CONFIG picks up the correct back pointer to
	 * the ChangeSet file
	 */
	assert(oldest);
	d2 = cset->tree;
	fix_delta(cset, oldest, d2, 0);
	d2->sum = oldest->sum;
	d2 = sccs_kid(cset, d2);
	assert(d2);
	fix_delta(cset, oldest, d2, 1);
	sccs_newchksum(cset);
	sccs_free(cset);
	unlink("BitKeeper/log/ROOTKEY");
	proj_reset(0);
}

typedef struct {
	MDBM	*view;
	MDBM	*db;
	MDBM	*csetComment;
	FILE	*patch;
	dinfo	*tip;
	sum_t	sum;
	int	rev;
	char	parent[MAXKEY];
	char	tagparent[MAXKEY];
	char	patchFile[MAXPATH];
	sccs	*cset;
	int	date;
} mkcs_t;

/*
 * output a cset to cur->patch to similate a makepatch entry
 * Build up a delta *e with enough data to get sccs_prs() to do the
 * heavy lifting.  Manually generate all the other patch info.
 *
 * NOTE: e is never woven into the cset graph, no parent or kid set.
 * it works by setting cur->cset->rstart = cur->cset->rstop = e;
 * which sccs_prs() uses.
 */

private void
mkCset(mkcs_t *cur, dinfo *d)
{
	char	**comments;
	int	v;
	ser_t	k;
	kvpair	kv, vk;
	dinfo	*top;
	delta	*e = new(delta);
	char	dkey[MAXKEY];
	char	dline[2 * MAXKEY + 4];
	char	**lines;
	int	i;
	int	added;

	/*
	 * Set date/user/host to match delta "d", the last delta.
	 * The person who make the last delta is likely to be the person
	 * making the cset.
	 */
	fix_delta(cur->cset, d, e, -1);

	/* More into 'e' from sccs * and by hand. */
	e = sccs_dInit(e, 'D', cur->cset, 1);
	e->flags |= D_CSET; /* copied from cset.c, probably redundant */
	comments = db2line(cur->csetComment);
	comments_set(cur->cset, e, comments);
	freeLines(comments, free);

	/*
	 * Need to do compute patch diff entry (and save into 'lines')
	 * before outputting the patch header, because we need lines added
	 * in the header even though the importer just sets it to be
	 * one.  Also need to get the file checksum corresponding to the
	 * delta, computed by adding in new and subtracting out old. 
	 * Assumes straightline graph, so new entry always replaces previous.
	 */
	lines = 0;
	added = 0;
// fprintf(stderr, "cur=%u\n", cur->sum);
	EACH_KV(cur->db) {
		sum_t	linesum, sumch;
		u8	*ch;

		/*
		 * Extract filename and top delta for this cset
		 */
		memcpy(&k, kv.key.dptr, sizeof(ser_t));
		memcpy(&v, kv.val.dptr, sizeof(int));
		top = (dinfo *)list[v+1];

		saveCsetMark(k, top->rev);

		sprintf(dline, "%s %s\n", keylist[k], top->dkey);
		lines = addLine(lines, strdup(dline));
		linesum = 0;
		for (ch = dline; *ch; ch++) {
			sumch = *ch;
			linesum += sumch;
		}
		/*
		 * Compute file checksum by subtracting off old
		 * then adding in new and saving new in hash to use
		 * next time as the old entry.
		 * hash key is delta root key
		 * hash value is checksum of corresponding line in cset file
		 */
		if (ch = mdbm_fetch_str(cur->view, keylist[k])) {
			memcpy(&sumch, ch, sizeof(sum_t));
			cur->sum -= sumch;
// fprintf(stderr, "--- cur=%u %u\n", cur->sum, sumch);
		}
		cur->sum += linesum;
// fprintf(stderr, "+++ cur=%u %u\n", cur->sum, linesum);
		vk.key.dptr = keylist[k];
		vk.key.dsize = strlen(keylist[k])+1;
		vk.val.dptr = (void *)&linesum;
		vk.val.dsize = sizeof(linesum);
		if (mdbm_store(cur->view, vk.key, vk.val, MDBM_REPLACE)) {
			assert("insert failed" == 0);
		}
		added++;
	}
	e->added = added;
	e->sum = cur->sum;
//  fprintf(stderr, "SUM=%u\n", e->sum);
	/* Some hacks to get sccs_prs to do some work for us */
	cur->cset->rstart = cur->cset->rstop = e;
	if (cur->cset->tree->pathname && !e->pathname) {
		e->pathname = cur->cset->tree->pathname;
	}
	if (cur->cset->tree->zone && !e->zone) {
		e->zone = cur->cset->tree->zone;
	}
	sprintf(dkey, "1.%u", ++cur->rev);
	assert(!e->rev);
	e->rev = sccs_addStr(cur->cset, dkey);

	unless (cur->date < e->date) {
		int	fudge = (cur->date - e->date) + 1;

		e->date += fudge;
		e->dateFudge += fudge;
	}
	cur->date = e->date;

	/*
	 * All the data has been faked into place.  Now generate a
	 * patch header (parent key, prs entry), patch diff, blank line.
	 */
	fputs(cur->parent, cur->patch);
	fputs("\n", cur->patch);
	sccs_sdelta(cur->cset, e, cur->parent);
	if (do_patch(cur->cset, e, 0, 0, cur->patch)) exit(1);
	sortLines(lines, 0);
	fputs("\n0a0\n", cur->patch); /* Fake diff header */
	EACH(lines) {
		fputs("> ", cur->patch);
		fputs(lines[i], cur->patch);
	}
	fputs("\n", cur->patch);
	freeLines(lines, free);
	if (cur->tip) free_dinfo(cur->tip);
	cur->tip = delta2dinfo(cur->cset, e);
	sccs_freedelta(e);
}

private void
mkTag(mkcs_t *cur, char *tag)
{
	delta	*e = new(delta);
	char	dkey[MAXKEY];
	char	*tagparent = cur->tagparent[0] ? cur->tagparent : 0;

	/*
	 * Set date/user/host to match delta "d", the last delta.
	 * The person who make the last delta is likely to be the person
	 * making the cset.
	 */
	assert(cur->tip);	/* tagging something */
	fix_delta(cur->cset, cur->tip, e, -1);

	/* More into 'e' from sccs * and by hand. */
	e = sccs_dInit(e, 'R', cur->cset, 1);
	e->comments = 0;

	/* Some hacks to get sccs_prs to do some work for us */
	cur->cset->rstart = cur->cset->rstop = e;
	if (cur->cset->tree->pathname && !e->pathname) {
		e->pathname = cur->cset->tree->pathname;
	}
	if (cur->cset->tree->zone && !e->zone) {
		e->zone = cur->cset->tree->zone;
	}
	sprintf(dkey, "1.%u", cur->rev);
	assert(!e->rev);
	e->rev = sccs_addStr(cur->cset, dkey);

	unless (cur->date < e->date) {
		int	fudge = (cur->date - e->date) + 1;

		e->date += fudge;
		e->dateFudge += fudge;
	}
	cur->date = e->date;
	e->flags |= D_SYMGRAPH | D_SYMLEAF;

	/*
	 * All the data has been faked into place.  Now generate a
	 * patch header (parent key, prs entry), patch diff, blank line.
	 */
	fputs(cur->parent, cur->patch);
	fputs("\n", cur->patch);
	if (do_patch(cur->cset, e, tag, tagparent, cur->patch)) exit(1);
	fputs("\n\n", cur->patch);
	sccs_sdelta(cur->cset, e, cur->tagparent);
	sccs_freedelta(e);
}

private	void
preloadView(sccs *s, MDBM *db, delta *d)
{
	kvpair	kv;
	sum_t	linesum, sumch;
	u8	*ch;

	if (sccs_get(s, REV(s, d), 0, 0, 0, SILENT|GET_HASHONLY, 0)) {
		assert("cannot get hash" == 0);
	}
	EACH_KV(s->mdbm) {
		/* sum up rootkey, deltakey, space and newline */
		linesum = ' ' + '\n';	
		for (ch = kv.key.dptr; *ch; ch++) {
			sumch = *ch;
			linesum += sumch;
		}
		for (ch = kv.val.dptr; *ch; ch++) {
			sumch = *ch;
			linesum += sumch;
		}
		kv.val.dptr = (void *)&linesum;
		kv.val.dsize = sizeof(linesum);
		if (mdbm_store(db, kv.key, kv.val, MDBM_INSERT)) {
			assert("insert failed" == 0);
		}
	}
	mdbm_close(s->mdbm);
	s->mdbm = 0;
}

/*
 * We dump the comments into a mdbm to factor out repeated comments
 * that came from different files.
 */
private void
saveComment(MDBM *db, char *rev, char *comment_str, char *gfile)
{
	datum	k, v, tmp;
	MDBM	*gDB = 0;
	int	ret;
	char	*p;
	char	cset[MAXPATH*2];

#define	CVS_NULL_COMMENT "*** empty log message ***\n"
#define	SCCS_REV_1_1_DEFAULT_COMMENT	"date and time created "
#define	CODE_MGR_DEFAULT_COMMENT	"CodeManager Uniquification: "

	if (isBlank(comment_str)) return;
	if (streq(CVS_NULL_COMMENT, comment_str)) return;
	if ((streq("1.1", rev) || streq(rev, "1.2")) &&
	    strneq(SCCS_REV_1_1_DEFAULT_COMMENT, comment_str, 22)) {
		return;
	}
	/*
	 * For 3pardate's teamware repository
	 */
	if ((streq("1.1", rev) || streq(rev, "1.2")) &&
	    strneq(CODE_MGR_DEFAULT_COMMENT, comment_str, 28)) {
		return;
	}

	if (opts.ensemble &&
	    (p = strrchr(gfile, '/')) && streq(p, "/ChangeSet")) {
		*p = 0;
		sprintf(cset, "Commit in %s\n", gfile);
		*p = '/';
		k.dptr = cset;
	} else {
		k.dptr = comment_str;
	}
	k.dsize = strlen(k.dptr) + 1;
	tmp = mdbm_fetch(db, k);
	if (tmp.dptr) memcpy(&gDB, tmp.dptr, sizeof (MDBM *));
	unless (gDB) gDB = mdbm_mem();
	ret = mdbm_store_str(gDB, basenm(gfile), "", MDBM_INSERT);
	/* This should work, or it will be another file with the same
	 * basename.
	 */
	assert(ret == 0 || (ret == 1 && errno == EEXIST));
	unless (tmp.dptr) {
		v.dptr = (char *) &gDB;
		v.dsize = sizeof (MDBM *);
		ret = mdbm_store(db, k, v, MDBM_REPLACE);
	}
}

private void
freeComment(MDBM *db)
{
	kvpair	kv;
	MDBM	*gDB;

	EACH_KV(db) {
		memcpy(&gDB, kv.val.dptr, sizeof (MDBM *));
		mdbm_close(gDB);
	}
	mdbm_close(db);
}

private int
isBreakPoint(time_t now, dinfo *d)
{
	if (opts.noSkip) return (0);

	/*
	 * Do not make a cset of recent deltas, becuase we may be
	 * missing delta that will be added in the near future
	 */
	if (d->date >= (now - time_gap(d->date))) {
		if (opts.verbose > 1) {
			fprintf(stderr,
			    "Skipping delta younger than %d minutes\n",
			    opts.timeGap);
		}
		return (1);
	}
	return (0);
}

private	void
findcset(void)
{
	int	j;
	dinfo	*d = 0, *previous;
	datum	key, val;
	FILE	*f;
	delta	*d2;
	time_t	now, tagDate = 0;
	char	*nextTag;
	int	ret;
	int	skip = 0;
	mkcs_t  cur;
	int     flags = 0;
	char	buf[MAXLINE];

	cur.db = mdbm_mem();
	cur.view = mdbm_mem();
	cur.csetComment = mdbm_mem();
	cur.sum = 0;
	cur.parent[0] = 0;
	cur.tagparent[0] = 0;
	cur.rev = 1;
	cur.date = 0;
	cur.tip = 0;
	bktmp(cur.patchFile, "cpatch");
	sprintf(buf, "bk _adler32 > '%s'", cur.patchFile);
	unless (cur.patch = popen(buf, "w")) {
		perror("findcset");
		exit (1);
	}

	unless (opts.verbose) flags |= SILENT;
	cur.cset = sccs_init(CHANGESET, INIT_NOCKSUM|flags);
	assert(cur.cset && cur.cset->tree);

	/*
	 * Force 1.0 and 1.1 cset delta to match oldest
	 * delta in the repository. Warning; this changes the cset root key!
	 */
	assert(oldest);
	d2 = cur.cset->tree;
	fix_delta(cur.cset, oldest, d2, 0);
	d2 = sccs_kid(cur.cset, d2);
	assert(d2);
	fix_delta(cur.cset, oldest, d2, 1);
	sccs_newchksum(cur.cset);
	cur.cset = sccs_restart(cur.cset);

	fputs("\001 Patch start\n", cur.patch);
	fputs("# Patch vers:\t1.3\n# Patch type:\tREGULAR\n\n", cur.patch);
	fputs("== ChangeSet ==\n", cur.patch);
	sccs_pdelta(cur.cset, sccs_ino(cur.cset), cur.patch);
	fputs("\n", cur.patch);

	d2 = sccs_top(cur.cset);
	sccs_sdelta(cur.cset, d2, cur.parent);
	cur.sum = d2->sum;
	cur.date = d2->date;
	preloadView(cur.cset, cur.view, d2);

	nextTag = readTag(&tagDate);

	f = stderr;
	now = time(0);
	for (j = 0; j < deltaCounter; ++j) {
		d = (dinfo *)list[j+1];
		if (j > 0) {
			previous = (dinfo *)list[j];
			/* skip tags that are too early */
			while (nextTag && tagDate < previous->date) {
				free(nextTag);
				nextTag = readTag(&tagDate);
			}
			if (isCsetBoundary(cur.cset, previous, d,
			    tagDate, now, &skip)) {
				mkCset(&cur, previous);
				while (nextTag && tagDate >= previous->date &&
				    tagDate < d->date) {
					mkTag(&cur, nextTag);
					nextTag = readTag(&tagDate);
				}
				if (isBreakPoint(now, d)) goto done;
				freeComment(cur.csetComment);
				cur.csetComment = mdbm_mem();
				mdbm_close(cur.db);
				cur.db = mdbm_mem();
			}
		}

		/*
		 * Find the top delta of a file for this cset.
		 * This is done by stuffing the f_index/delta pair into a mdbm
		 */
		assert(d->f_index > 0);
		key.dptr = (char *) &(d->f_index);
		key.dsize = sizeof (ser_t);
		val.dptr = (char *) &j;
		val.dsize = sizeof (int);
		/* last entry wins */
		ret = mdbm_store(cur.db, key, val, MDBM_REPLACE);
		assert(ret == 0);

		/*
		 * Extract per file comment and copy them to cset comment
		 */
		saveComment(cur.csetComment, d->rev, d->comments, d->pathname);
	}
	if (skip && (opts.verbose > 1)) {
		fprintf(stderr,
		    "Skipping %d delta%s older than %d month.\n",
		    skip, ((skip == 1) ? "" : "s"), opts.blackOutTime);
	}
	assert(d == (dinfo *)list[deltaCounter]);
	if (isBreakPoint(now, d)) goto done;
	mkCset(&cur, d);
	while (nextTag && tagDate >= d->date) {
		mkTag(&cur, nextTag);
		nextTag = readTag(&tagDate);
	}
done:	freeComment(cur.csetComment);
	if (cur.tip) free_dinfo(cur.tip);
	fputs("\001 End\n", cur.patch);
	pclose(cur.patch);
	sccs_free(cur.cset);
	mdbm_close(cur.db);
	mdbm_close(cur.view);
	if (sys("bk", "takepatch", "-f", cur.patchFile, SYS)) {
		sys("cat", cur.patchFile, SYS);
		fileMove(cur.patchFile, "mypatch");
		exit(1);
	}
	rename("RESYNC/SCCS/s.ChangeSet", "SCCS/s.ChangeSet");
	system("rm -rf RESYNC");
	unlink(cur.patchFile);
}

private	void
free_dinfo(void *x)
{
	dinfo	*di = (dinfo *)x;

	if (di->rev) free(di->rev);
	if (di->zone) free(di->zone);
	if (di->user) free(di->user);
	if (di->hostname) free(di->hostname);
	if (di->pathname) free(di->pathname);
	if (di->comments) free(di->comments);
	if (di->dkey) free(di->dkey);
	free(di);
}

private	dinfo *
delta2dinfo(sccs *s, delta *d)
{
	dinfo	*di = new(dinfo);
	char	key[MAXKEY];

	assert(di);
	di->date = d->date;
	di->dateFudge = d->dateFudge;
	di->sum = d->sum;
	di->rev = strdup(REV(s, d));
	if (d->zone) di->zone = strdup(ZONE(s, d));
	di->user = strdup(USER(s, d));
	di->hostname = strdup(HOSTNAME(s, d));
	di->pathname = strdup(PATHNAME(s, d));
	di->comments = strdup(COMMENTS(s, d));
	sccs_sdelta(s, d, key);
	di->dkey = strdup(key);
	return (di);
}

/*
 * Find the oldest user delta
 */
private dinfo *
findFirstDelta(sccs *s, dinfo *first)
{
	delta	*d = sccs_findrev(s, "1.1");

	unless (d) return (first);

	/*
	 * Skip teamware dummy user
	 * XXX - there can be more than one.
	 */
	if ((d->date == 0) && streq(USER(s, d), "Fake")) {
		d = sccs_kid(s, d);
	}
	unless (d) return (first);

	/*
	 * XXX TODO If d->date == first->date
	 * we need to sort on sfile name
	 */
	if ((first == NULL) || (d->date < first->date)) {
		if (first) free_dinfo(first);
		first = delta2dinfo(s, d);
	}
	return (first);
}

/*
 * Collect the delta that we need into "list"
 */
private	void
mkList(sccs *s, int fileIdx)
{
	delta	*d;
	dinfo	*di;

	assert(fileIdx > 0);

	/*
	 * Mark the delta in pending state on the main trunk
	 */
	d = sccs_top(s);
	while (d) {
		assert(!d->r[2]);
		if (d->flags & D_CSET) break;

		/*
		 * Skip 1.0 delta, we do not want a 1.0 delta
		 * show up as the top delta in a cset. A top
		 * delta in a cset should be 1.1 or higher.
		 */
		if (d == s->tree) break;
		d->flags |= D_SET;
		d = PARENT(s, d);
	}

	for (d = s->table; d; d = NEXT(d)) {
		if (d->flags & D_SET) {
			/*
			 * Collect marked delta into "list"
			 */
			di = delta2dinfo(s, d);
			di->f_index = fileIdx; /* needed in findcset() */
			list = addLine(list, di);
			deltaCounter++;
		}
	}
}

FILE	*tf;

private int
openTags(char *tagfile)
{
	tf = fopen (tagfile, "rt");
	unless (tf) perror(tagfile);
	return (tf == 0);
}

private char *
readTag(time_t *tagDate)
{
	char	nextTag[2048];
	char	buf[MAXLINE];

	*tagDate = 0;	/* default */
	unless (tf) return (0);
	if (fnext(buf, tf)) {
		int	rc;
		rc = sscanf(buf, "%s %lu", nextTag, tagDate);
		assert(rc == 2);
		assert(strlen(nextTag) < sizeof(nextTag));
	} else {
		closeTags();
		return (0);
	}
	return (strdup(nextTag));
}

private void
closeTags(void)
{
	if (tf) fclose(tf);
	tf = 0;
}

/*
 * Lifted from slib.c and modified as we are synthesizing a patch
 * and don't have a real sfile.  Old method was to fake the parts
 * needed for sccs_prs and do_patch to work; new way is to just
 * output the patch here.
 */

private int
do_patch(sccs *s, delta *d, char *tag, char *tagparent, FILE *out)
{
	int	len, i;	/* used by EACH */
	char	*p, *t;
	char	type;

	if (!d) return (0);
	type = TAG(d) ? 'M' : 'D';

	fprintf(out, "%c %s %s%s %s%s%s +%u -%u\n",
	    type, REV(s, d), delta_sdate(s, d),
	    ZONE(s, d),
	    USER(s, d),
	    d->hostname ? "@" : "",
	    d->hostname ? HOSTNAME(s, d) : "",
	    d->added, d->deleted);

	/*
	 * Order from here down is alphabetical.
	 */
	if (d->csetFile) fprintf(out, "B %s\n", CSETFILE(s, d));
	if (d->flags & D_CSET) fprintf(out, "C\n");
	if (DANGLING(d)) fprintf(out, "D\n");
	t = COMMENTS(s, d);
	while (p = eachline(&t, &len)) fprintf(out, "c %.*s\n", len, p);
	if (d->dateFudge) fprintf(out, "F %d\n", (int)d->dateFudge);
	assert(!d->include);
	if (d->flags & D_CKSUM) {
		fprintf(out, "K %u\n", d->sum);
	}
	assert(!d->merge);
	if (d->pathname) fprintf(out, "P %s\n", PATHNAME(s, d));
	if (d->random) fprintf(out, "R %s\n", d->random);
	if (tag) {
		fprintf(out, "S %s\n", tag);
		if (SYMGRAPH(d)) fprintf(out, "s g\n");
		if (SYMLEAF(d)) fprintf(out, "s l\n");
		if (tagparent) {
			fprintf(out, "s %s\n", tagparent);
		}
		assert (!d->mtag);
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
	assert (!d->exclude);
	if (d->flags & D_XFLAGS) {
		assert((d->xflags & X_EOLN_UNIX) == 0);
		fprintf(out, "X 0x%x\n", d->xflags);
	}
	fprintf(out, "------------------------------------------------\n");
	return (0);
}
