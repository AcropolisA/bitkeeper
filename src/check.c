/*
 * ChangeSet fsck - make sure that the key pointers work in both directions.
 * It's slowly grown to include checks for many of the problems our users
 * have encountered.
 */
/* Copyright (c) 1999-2000 Larry McVoy */
#include "system.h"
#include "sccs.h"
#include "range.h"

extern	int	sane_main(int ac, char **av);
private	HASH	*buildKeys(sccs *cset, MDBM *idDB);
private	char	*csetFind(char *key);
private	int	check(sccs *s, HASH *db);
private	char	*getRev(char *root, char *key, MDBM *idDB);
private	char	*getFile(char *root, MDBM *idDB);
private	int	checkAll(HASH *db);
private	void	listFound(HASH *db);
private	void	listCsetRevs(char *key);
private void	init_idcache(void);
private int	checkKeys(sccs *s, char *root);
private	int	chk_csetpointer(sccs *s);
private void	warnPoly(void);
private int	chk_gfile(sccs *s, MDBM *pathDB);
private int	chk_dfile(sccs *s);
private int	writable_gfile(sccs *s);
private int	readonly_gfile(sccs *s);
private int	no_gfile(sccs *s);
private int	chk_eoln(sccs *s, int eoln_unix);
private int	chk_merges(sccs *s);
private sccs*	fix_merges(sccs *s);
private	int	update_idcache(MDBM *idDB, HASH *keys);

private	int	nfiles;		/* for progress bar */
private	int	actual;		/* for progress bar cache */
private	int	verbose;
private	int	details;	/* if set, show more information */
private	int	all;		/* if set, check every entry in the ChangeSet */
private	int	resync;		/* called in resync dir */
private	int	fix;		/* if set, fix up anything we can */
private	int	goneKey;	/* 1: list files, 2: list deltas, 3: both */
private	int	badWritable;	/* if set, list bad writable file only */
private	int	names;		/* if set, we need to fix names */
private	int	gotDupKey;	/* if set, we found dup keys */
private	int	csetpointer;	/* if set, we need to fix cset pointers */
private	int	lod;		/* if set, we need to fix lod data */
private	int	mixed;		/* mixed short/long keys */
private	int	check_eoln;
private	sccs	*cset;		/* the initialized cset file */
private int	flags = SILENT|INIT_NOGCHK|INIT_NOCKSUM;
private	FILE	*idcache;
private	u32	id_sum;
private char	id_tmp[MAXPATH]; /* BitKeeper/tmp/bkXXXXXX */
private	int	poly;
private	int	polyList;
private	MDBM	*goneDB;
private	char	ctmp[MAXPATH]; 	/* BitKeeper/tmp/bkXXXXXX */
int		xflags_failed;	/* notification */

#define	POLY	"BitKeeper/etc/SCCS/x.poly"

/*
 * This data structure is so we don't have to search through all tuples in
 * the ChangeSet file for every file.  Instead, we sort the ChangeSet hash
 * and read in the sorted list of tuples into "malloc".  Then we walk the
 * data, putting the delta keys (the second part) into deltas[].  The first
 * key from each file is recorded in r2i{rootkey} = index into deltas[].
 * The list in deltas is separated by (char*1) between each file.
 *
 * Going to this structure was about a 20x speedup for the Linux kernel.
 */
private	struct {
	char	*malloc;	/* the whole cset file read in */
	char	**deltas;	/* sorted on root key */
	int	n;		/* deltas[n] == 0 */
	HASH	*r2i;		/* {rootkey} = start in roots[] list */
} csetKeys;

int
check_main(int ac, char **av)
{
	int	c, n;
	HASH	*db;
	MDBM	*idDB;
	MDBM	*pathDB = mdbm_mem();
	HASH	*keys = hash_new();
	sccs	*s;
	int	errors = 0, eoln_native = 1, want_dfile;
	int	e;
	char	*name;
	char	buf[MAXKEY];
	char 	s_cset[] = CHANGESET;
	char	*t;

	while ((c = getopt(ac, av, "acdefgpRvw")) != -1) {
		switch (c) {
		    case 'a': all++; break;			/* doc 2.0 */
		    case 'c': flags &= ~INIT_NOCKSUM; break;	/* doc 2.0 */
		    case 'd': details++; break;
		    case 'e': check_eoln++; break;
		    case 'f': fix++; break;			/* doc 2.0 */
		    case 'g': goneKey++; break;			/* doc 2.0 */
		    case 'p': polyList++; break;		/* doc 2.0 */
		    case 'R': resync++; break;			/* doc 2.0 */
		    case 'v': verbose++; break;			/* doc 2.0 */
		    case 'w': badWritable++; break;		/* doc 2.0 */
		    default:
			system("bk help -s check");
			return (1);
		}
	}
	if (getenv("BK_NOTTY") && (verbose == 1)) verbose = 0;

	if (goneKey && badWritable) {
		fprintf(stderr, "check: cannot have both -g and -w\n");
		return (1);
	}

	if (all && (!av[optind] || !streq("-", av[optind]))) {
		fprintf(stderr, "check: -a syntax is ``bk -r check -a''\n");
		return (1);
	}
	if (proj_cd2root()) {
		fprintf(stderr, "check: cannot find package root.\n");
		return (1);
	}
	if (sane(0, resync)) return (1);
	unless (exists(s_cset)) {
		fprintf(stderr, "ERROR: %s is missing, aborting.\n", s_cset);
		exit(1);
	}
retry:	unless ((cset = sccs_init(s_cset, flags)) && HASGRAPH(cset)) {
		fprintf(stderr, "Can't init ChangeSet\n");
		exit(1);
	}
	if (cset->encoding & E_GZIP) {
		sccs_free(cset);
		if (verbose == 1) {
			fprintf(stderr, "Uncompressing ChangeSet file...\n");
		}
		/* This will fail if we are locked */
		if (sys("bk", "admin", "-Znone", "ChangeSet", SYS)) exit(1);
		if (verbose == 1) {
			fprintf(stderr,
			    "Restarting check from the beginning...\n");
		}
		goto retry;
	}
	unless (cset = cset_fixLinuxKernelChecksum(cset)) return (1);
	mixed = LONGKEY(cset) == 0;
	if (verbose == 1) {
		nfiles = repo_nfiles(cset);
		fprintf(stderr, "Preparing to check %u files...\r", nfiles);
	}
	unless (idDB = loadDB(IDCACHE, 0, DB_KEYFORMAT|DB_NODUPS)) {
		perror("idcache");
		exit(1);
	}
	db = buildKeys(cset, idDB);
	if (all) {
		mdbm_close(idDB);
		idDB = 0;
		init_idcache();
	}

	/* This can legitimately return NULL */
	/* XXX - I don't know for sure I always need this */
	goneDB = loadDB(GONE, 0, DB_KEYSONLY|DB_NODUPS);

	if (check_eoln) {
		eoln_native = !streq(user_preference("eoln"), "unix"); 
	}
	unless (fix) {
		if (t = user_preference("autofix")) {
			/* Note: this sets it to 1, not 2. */
			fix = streq(t, "yes") || streq(t, "on");
		}
	}

	want_dfile = exists(DFILE);
	for (n = 0, name = sfileFirst("check", &av[optind], 0);
	    name; n++, name = sfileNext()) {
		unless (s = sccs_init(name, flags)) {
			if (all) fprintf(stderr, "%s init failed.\n", name);
			errors |= 1;
			continue;
		}
		if (all) {
			actual++;
			if (verbose == 1) progressbar(n, nfiles, 0);
		}
		unless (s->cksumok == 1) {
			fprintf(stderr,
			    "%s: bad file checksum, corrupted file?\n",
			    s->gfile);
			sccs_free(s);
			errors |= 1;
			continue;
		}
		unless (HASGRAPH(s)) {
			if (!(s->state & S_SFILE)) {
				fprintf(stderr, "check: %s doesn't exist.\n",
				    s->sfile);
			} else {
				perror(s->gfile);
			}
			sccs_free(s);
			errors |= 1;
			continue;
		}
		/*
		 * exit code 2 means try again, all other errors should be
		 * distinct.
		 */
		unless (flags & INIT_NOCKSUM) {
			if (sccs_resum(s, 0, 0, 0)) errors |= 0x04;
			if (s->has_nonl && chk_nlbug(s)) errors |= 0x04;
		}
		if (chk_gfile(s, pathDB)) errors |= 0x08;
		if (no_gfile(s)) errors |= 0x08;
		if (readonly_gfile(s)) errors |= 0x08;
		if (writable_gfile(s)) errors |= 0x08;
		if (chk_csetpointer(s)) errors |= 0x10;
		if (want_dfile && chk_dfile(s)) errors |= 0x10;
		if (check_eoln && chk_eoln(s, eoln_native)) errors |= 0x10;
		if (chk_merges(s)) {
			if (fix) s = fix_merges(s);
			errors |= 0x20;
		}
		sccs_close(s); /* for HP */

		/*
		 * Store the full length key and only if we are in mixed mode,
		 * also store the short key.  We want all of them to be unique.
		 */
		sccs_sdelta(s, sccs_ino(s), buf);
		if (hash_storeStr(keys, buf, s->gfile)) {
			char *gfile, *sfile;

			gfile = hash_fetchStr(keys, buf);
			sfile = name2sccs(gfile);
			if (sameFiles(sfile, s->sfile)) {
				fprintf(stderr,
				    "%s and %s are identical. "
				    "Is one of these files copied?\n",
				    s->sfile, sfile);
			} else {
				fprintf(stderr,
				    "Same key %s used by\n\t%s\n\t%s\n",
				    buf, s->gfile, gfile);
			}
			free(sfile);
			gotDupKey = 1;
			errors |= 1;
		}
		if (mixed) {
			t = sccs_iskeylong(buf);
			assert(t);
			*t = 0;
			if (hash_storeStr(keys, buf, s->gfile)) {
				char *gfile, *sfile;

				gfile = hash_fetchStr(keys, buf);
				sfile = name2sccs(gfile);
				if (sameFiles(sfile, s->sfile)) {
					fprintf(stderr,
					    "%s and %s are identical. "
					    "Is one of these files copied?\n",
					    s->sfile, sfile);
				} else {
					fprintf(stderr,
					    "Same key %s used by\n\t%s\n\t%s\n",
					    buf, s->gfile, gfile);
				}
				free(sfile);
				gotDupKey = 1;
				errors |= 1;
			}
		}

		if (e = check(s, db)) {
			errors |= 0x40;
		} else {
			if (verbose>1) fprintf(stderr, "%s is OK\n", s->gfile);
		}
		sccs_free(s);
	}
	if (e = sfileDone()) return (e);
	if (all || update_idcache(idDB, keys)) {
		fprintf(idcache, "#$sum$ %u\n", id_sum);
		fclose(idcache);
		if (sccs_lockfile(IDCACHE_LOCK, 16, 0)) {
			fprintf(stderr, "Not updating cache due to locking.\n");
			unlink(id_tmp);
		} else {
			unlink(IDCACHE);
			if (rename(id_tmp, IDCACHE)) {
				perror("rename of idcache");
				unlink(IDCACHE);
			}
			sccs_unlockfile(IDCACHE_LOCK);
			chmod(IDCACHE, GROUP_MODE);
		}
	}
	/*
	 * Note: we may update NFILES more than needed when not in verbose mode.
	 */
	if (all &&
	    ((actual != nfiles) || !exists("BitKeeper/log/NFILES"))) {
		FILE	*f;

		unlink("BitKeeper/log/NFILES");
		if (f = fopen("BitKeeper/log/NFILES", "w")) {
			fprintf(f, "%u\n", actual);
			fclose(f);
		}
	}
	if ((all || resync) && checkAll(keys)) errors |= 0x40;
	unlink(ctmp);
	mdbm_close(pathDB);
	hash_free(db);
	hash_free(keys);
	if (goneDB) mdbm_close(goneDB);
	if (errors && fix) {
		if (names && !gotDupKey) {
			fprintf(stderr, "check: trying to fix names...\n");
			system("bk -r names");
			system("bk idcache");
		}
		if (xflags_failed) {
			fprintf(stderr, "check: trying to fix xflags...\n");
			system("bk -r xflags");
		}
		if (csetpointer) {
			char	buf[MAXKEY + 20];
			char	*csetkey = proj_rootkey(0);

			fprintf(stderr,
			    "check: "
			    "fixing %d incorrect cset file pointers...\n",
			    csetpointer);
			sprintf(buf, "bk -r admin -C'%s'", csetkey);
			system(buf);
		}
		if (lod && (fix > 1)) {
			fprintf(stderr, "check: trying to remove lods...\n");
			system("bk _fix_lod1");
		}
		if (names || xflags_failed || csetpointer || lod) {
			errors = 2;
			goto out;
		}
	}
	if (csetKeys.malloc) {
		int	i;

		free(csetKeys.malloc);
		for (i = csetKeys.n;
		    (--i > 0) && (csetKeys.deltas[i] != (char*)1); );
		if ((i >= 0) && (csetKeys.deltas[i] == (char*)1)) {
			for (i++; csetKeys.deltas[i] != (char*)1; i++) {
				free(csetKeys.deltas[i]);
			}
		}
	}
	if (csetKeys.deltas) free(csetKeys.deltas);
	if (csetKeys.r2i) hash_free(csetKeys.r2i);
	if (poly) warnPoly();
	if (resync) {
		chdir(RESYNC2ROOT);
		if (sys("bk", "sane", SYS)) errors |= 0x80;
		chdir(ROOT2RESYNC);
	} else {
		if (sys("bk", "sane", SYS)) errors |= 0x80;
	}
out:
	if (verbose == 1) progressbar(nfiles, nfiles, errors ? "FAILED":"OK");
	if (all && !errors && !(flags & INIT_NOCKSUM)) {
		unlink(CHECKED);
		touch(CHECKED, 0666);
	}
	return (errors);
}

private sccs *
fix_merges(sccs *s)
{
	sccs	*tmp;

	sccs_renumber(s, 0, 0);
	sccs_newchksum(s);
	tmp = sccs_init(s->sfile, 0);
	assert(tmp);
	sccs_free(s);
	return (tmp);
}

private int
chk_dfile(sccs *s)
{
	delta	*d;
	char	*p;

	d = sccs_top(s);
	unless (d) return (0);

       /*
	* XXX There used to be code here to handle the old style
	* lod "not in view" file (s->defbranch == 1.0).  Pulled
	* that and am leaving this marker as a reminder to see
	* if new single tip LOD design needs to handle 'not in view'
	* as a special case.
        */

	p = basenm(s->sfile);
	*p = 'd';
	if  (!(d->flags & D_CSET) && !exists(s->sfile)) { 
		*p = 's';
		getMsg("missing_dfile", s->gfile, '=', stdout);
		return (1);
	}
	*p = 's';
	return (0);

}

private int
chk_gfile(sccs *s, MDBM *pathDB)
{
	char	*type;
	char	*sfile;
	char	buf[MAXPATH];

	/* check for conflicts in the gfile pathname */
	strcpy(buf, s->gfile);
	while (1) {
		if (streq(dirname(buf), ".")) break;
		if (mdbm_store_str(pathDB, buf, "", MDBM_INSERT)) break;
		sfile = name2sccs(buf);
		if (exists(sfile)) {
			free(sfile);
			type = "directory";
			goto err;
		}
		free(sfile);
	}
	unless (HAS_GFILE(s)) return (0);
	/*
	 * XXX when running in checkout:get mode, these checks are still
	 * too expensive. Need to do ONE stat.
	 */
	if (isreg(s->gfile) || isSymlnk(s->gfile)) return (0);
	strcpy(buf, s->gfile);
	if (isdir(s->gfile)) {

		type = "directory";
err:		getMsg2("file_dir_conflict", buf, type, '=', stderr);
		return (1);
	} else {
		type = "unknown-file-type";
		goto err;
	}
	/* NOTREACHED */
}

/*
 * Doesn't need to be fast, should be a rare event
 */
private int
keywords(sccs *s)
{
	char	*a = bktmp(0, "check_wk");
	char	*b = bktmp(0, "check_wok");
	int	same;

	assert(a && b);
	sysio(0, a, 0, "bk", "get", "-qp", s->gfile, SYS);
	sysio(0, b, 0, "bk", "get", "-qkp", s->gfile, SYS);
	same = sameFiles(a, b);
	unlink(a);
	unlink(b);
	free(a);
	free(b);
	return (!same);
}

private int
writable_gfile(sccs *s)
{
	if (!HAS_PFILE(s) && S_ISREG(s->mode) && IS_WRITABLE(s)) {
		pfile pf = { "+", "?", "?", NULL, "?", NULL, NULL, NULL };

		if (badWritable) {
			printf("%s\n", s->gfile);
			return (0);
		}
		unless (fix) {
			fprintf(stderr,
			    "===================================="
			    "=====================================\n"
			    "check: ``%s'' writable but not checked out.\n"
			    "This means that a file has been modified "
			    "without first doing a \"bk edit\".\n"
			    "\tbk -R edit -g %s\n"
			    "will fix the problem by changing the  "
			    "file to checked out status.\n"
			    "Running \"bk -r check -acf\" will "
			    "fix most problems automatically.\n"
			    "===================================="
			    "=====================================\n",
			    s->gfile, s->gfile);
			return (32);
		}

		/*
		 * See if diffing against the gfile with or without keywords
		 * expanded results in no diffs, if so, just re-edit the file.
		 * If that doesn't work, get the file with and without keywords
		 * expanded, compare them, and if there are no differences then
		 * there are no keywords and we can edit the file.  Otherwise
		 * we have to warn them that they need save the diffs and 
		 * patch them in minus the keyword expansion.
		 */
		if (s->xflags & (X_RCS|X_SCCS)) {
			if ((diff_gfile(s, &pf, 1, DEV_NULL) == 1) ||
			    (diff_gfile(s, &pf, 0, DEV_NULL) == 1)) {
				if (unlink(s->gfile)) return (1);
				sys("bk", "edit", "-q", s->gfile, SYS);
				return (0);
			}
			if (keywords(s)) {
				fprintf(stderr,
				    "%s: unlocked, modified, with keywords.\n",
				    s->gfile);
				return (1);
			}
		}
		sys("bk", "edit", "-q", "-g", s->gfile, SYS);
	}
	return (0);
}

private int
no_gfile(sccs *s)
{
	if (HAS_PFILE(s) && !HAS_GFILE(s)) {
		if (unlink(sccs_Xfile(s, 'p'))) return (1);
		s->state &= ~S_PFILE;
	}
	return (0);
}

private int
gfile_unchanged(sccs *s)
{
	pfile	pf;
	int	rc;

	if (sccs_read_pfile("check", s, &pf)) {
		fprintf(stderr, "%s: cannot read pfile\n", s->gfile);
		return (1);
	}
	rc = diff_gfile(s, &pf, 0, DEV_NULL);
	if (rc == 1) return (1); /* no changed */
	if (rc != 0) return (rc); /* error */

	/*
	 * If RCS/SCCS keyword enabled, try diff it with keyword expanded
	 */
	if (SCCS(s) || RCS(s)) return (diff_gfile(s, &pf, 1, DEV_NULL));
	return (0); /* changed */
}

private int
readonly_gfile(sccs *s)
{
	/* XXX slow in checkout:edit mode */
	if ((HAS_PFILE(s) && HAS_GFILE(s) && !writable(s->gfile))) {
		if (gfile_unchanged(s) == 1) {
			unlink(s->pfile);
			s->state &= ~S_PFILE;
			if (resync) return (0);
			do_checkout(s);
			return (0);
		} else {
			fprintf(stderr,
"===========================================================================\n\
check: %s is locked but not writable.\n\
You need to go look at the file to see why it is read-only;\n\
just changing it back to read-write may not be the\n\
right answer as it may contain expanded keywords.\n\
It may also contain changes to the file which you may want.\n\
If the file is unneeded, a \"bk unedit %s\" will fix the problem.\n\
===========================================================================\n",
		    		s->gfile, s->gfile);
			return (128);
		}
	}
	return (0);
}

private int
vrfy_eoln(sccs *s, int want_cr)
{
	MMAP 	*m;
	char 	*p;

	unless (HAS_GFILE(s)) return (0);
	unless (m = mopen(s->gfile, "b")) return (256);

	if (m->size == 0) {
		mclose(m);
		return (0);
	}

	/*
	 * check the first line
	 */
	p = m->where;
	while (p <= m->end) {
		if (*p == '\n') break;
		p++;
	}

	if (*p != '\n') return (0); /* incomplete line */
	if (want_cr) {
		if ((p > m->where) && (p[-1] == '\r')) {
			mclose(m);
			return (0);
		}
		fprintf(stderr,
		    "Warning: %s: missing CR in eoln\n", s->gfile);
	} else {
		if ((p == m->where) || (p[-1] != '\r')) {
			mclose(m);
			return (0);
		}
		fprintf(stderr,
		    "Warning: %s: extra CR in eoln?\n", s->gfile);
	}
	mclose(m);
	return (256);
}

private int
chk_eoln(sccs *s, int eoln_native)
{
	/*
	 * Skip non-user file and binary file
	 */
	if (CSET(s) ||
	    ((strlen(s->gfile) > 10) && strneq(s->gfile, "BitKeeper/", 10)) ||
	    ((s->encoding != E_ASCII) && (s->encoding != E_GZIP))) {
		return (0);
	}

#ifdef WIN32 /* eoln */
	vrfy_eoln(s, EOLN_NATIVE(s));
#else
	vrfy_eoln(s, 0); /* on unix, we do not want CRLF */
#endif

	if (eoln_native) {
		unless (EOLN_NATIVE(s)) {
			fprintf(stderr,
			    "Warning: %s : EOLN_NATIVE is off\n", s->gfile);
			return (256);
		}
	} else {
		if (EOLN_NATIVE(s)) {
			fprintf(stderr,
			    "Warning: %s : EOLN_NATIVE is on\n", s->gfile);
			return (256);
		}
	}
	return (0);
}

private void
warnPoly(void)
{
	getMsg("warn_poly", 0, 0, stdout);
	touch(POLY, 0664);
}

/*
 * Look at the list handed in and make sure that we checked everything that
 * is in the ChangeSet file.  This will always fail if you are doing a partial
 * check.
 *
 * Updated for RESYNC checks.  Only check keys if they are not in repository
 * already.
 */
private int
checkAll(HASH *db)
{
	FILE	*keys;
	char	*t;
	HASH	*warned = hash_new();
	int	found = 0;
	char	buf[MAXPATH*3];

	/*
	 * If we are doing the resync tree, we just want the keys which
	 * are not in the local repo.
	 */
	if (resync) {
		HASH	*local = hash_new();
		FILE	*f;
		
		sprintf(buf, "%s/%s", RESYNC2ROOT, CHANGESET);
		unless (exists(buf)) {
			hash_free(local);
			goto full;
		}
		sprintf(buf,
		    "bk sccscat -h %s/ChangeSet | bk _sort", RESYNC2ROOT);
		f = popen(buf, "r");
		while (fgets(buf, sizeof(buf), f)) {
			unless (hash_alloc(local, buf, 0, 0)) {
				fprintf(stderr,
				    "ERROR: duplicate line in ChangeSet\n");
			}
		}
		pclose(f);
		f = fopen(ctmp, "r");
		sprintf(buf, "%s.p", ctmp);
		keys = fopen(buf, "w");
		while (fgets(buf, sizeof(buf), f)) {
			unless (hash_fetchStr(local, buf)) fputs(buf, keys);
		}
		fclose(f);
		fclose(keys);
		sprintf(buf, "%s.p", ctmp);
		keys = fopen(buf, "r");
		hash_free(local);
	} else {
full:		keys = fopen(ctmp, "rt");
	}
	unless (keys) {
		perror("checkAll");
		exit(1);
	}
	while (fnext(buf, keys)) {
		t = separator(buf);
		assert(t);
		*t = 0;
		if (hash_fetchStr(db, buf)) continue;
		if (mdbm_fetch_str(goneDB, buf)) continue;
		hash_alloc(warned, buf, 0, 0);
		found++;
	}
	fclose(keys);
	if (found) listFound(warned);
	hash_free(warned);
	sprintf(buf, "%s.p", ctmp);
	unlink(buf);
	return (found != 0);
}

private void
listFound(HASH *db)
{
	kvpair	kv;

	if (goneKey & 1) { /* -g option => key only, no header */
		EACH_HASH(db) printf("%s\n", kv.key.dptr);
		return;
	}

	/* -gg, don't want file keys, just delta keys */
	if (goneKey) return;

	EACH_HASH(db) {
		fprintf(stderr, "Missing file (chk3) %s\n", kv.key.dptr);
	}
}

private void
init_idcache()
{
	unless (bktmp_local(id_tmp, "check")) {
		perror("bktmp_local");
		exit(1);
	}
	unless (idcache = fopen(id_tmp, "w")) {
		perror(id_tmp);
		exit(1);
	}
	id_sum = 0;
}

/*
 * Open up the ChangeSet file and get every key ever added.
 * Build an mdbm which is indexed by key (value is root key).
 * We'll use this later for making sure that all the keys in a file
 * are there.
 *
 * XXX - this code currently insists that the set of all marked deltas
 * in all files have a unique set of keys.  That may not be so and should
 * not have to be so.  The combination of <root key>,<delta key> does need
 * to be unique, however.
 * Should this become a problem, the right way to fix it is this: when the
 * second (or Nth where N>1) store fails, do the store with the key 
 * being <rootkey>\n<deltakey> -> <rootkey>.  Also fix the original but
 * do not remove the first <deltakey> -> <rootkey> pair so that any other
 * duplicates also do the long form.  Instead, replace the <rootkey> with
 * a marker, like "", and handle that in the lookup code.
 * When looking up the key in check(), if the rootkey is the marker, then
 * redo the lookup with the <rootkey>\n<deltakey> pair, you have "s" so you
 * can get the root key.
 * What this means is that we are saying the full name of a key is root\nkey
 * not just key.  We use "key" as a shorthand.
 * We should *NOT* do this for all keys regardless.  The memory footprint of
 * this program is huge already.
 */
private HASH	*
buildKeys(sccs *cset, MDBM *idDB)
{
	HASH	*db = hash_new();
	HASH	*r2i = hash_new();
	char	*s, *t, *r;
	int	fd, sz, n = 0, e = 0;
	delta	*d;
	FILE	*f;
	char	buf[MAXPATH*3];
	char	key[MAXPATH*2];

	unless (db && r2i) {
		perror("buildkeys");
		exit(1);
	}
	unless (cset && HASGRAPH(cset)) {
		fprintf(stderr, "check: ChangeSet file not inited\n");
		exit(1);
	}
	unless (bktmp(ctmp, "check")) {
		fprintf(stderr, "bktmp failed to get temp file\n");
		exit(1);
	}
	sprintf(buf, "bk _sort > %s", ctmp);
	f = popen(buf, "w");
	s = t = cset->mmap + cset->data;
	r = cset->mmap + cset->size;
	while (s < r) {
		if (*s == '\001') {
			while (*s++ != '\n');
			continue;
		}
		t = s;
		while (*s != '\001') s++;
		fwrite(t, s - t, 1, f);
	}
	pclose(f);
	sz = size(ctmp);

	/*
 	 * Note: malloc would return 0 if sz == 0
	 */
	csetKeys.malloc = malloc(sz);
	fd = open(ctmp, 0, 0);
	unless (read(fd, csetKeys.malloc, sz) == sz) {
		perror(ctmp);
		exit(1);
	}
	close(fd);
	for (fd = 0, d = cset->table; d; d = d->next) {
		/* XXX - d->added is always 1 so this logic is fucked */
		fd += d->added;
	}
	/*
	 * Allocate enough space for 2x the number of deltas (because of
	 * separaters), plus the deltas in the cset, plus 2.
	 */
	fd *= 2;
	fd += cset->nextserial + 2;	
	csetKeys.deltas = malloc(fd * sizeof(char*));
	csetKeys.r2i = r2i;

	buf[0] = 0;
	csetKeys.n = 0;
	for (s = csetKeys.malloc; s < csetKeys.malloc + sz; ) {
		t = separator(s);
		assert(t);
		*t++ = 0;
		r = strchr(t, '\n');
		*r++ = 0;
		assert(t);
		if (hash_storeStr(db, t, s)) {
			char	*a, *b;
			char	*root = hash_fetchStr(db, t);

			fprintf(stderr,
			    "Duplicate delta found in ChangeSet\n");
			a = getRev(s, t, idDB);
			fprintf(stderr, "\tRev: %s  Key: %s\n", a, t);
			free(a);
			if (streq(root, s)) {
				a = getFile(root, idDB);
				fprintf(stderr,
				    "\tBoth keys in file %s\n", a);
				free(a);
			} else {
				a = getFile(root, idDB);
				b = getFile(s, idDB);
				fprintf(stderr,
				    "\tIn different files %s and %s\n",
				    a, b);
				free(a);
				free(b);
			}
			listCsetRevs(t);
			if (polyList) e++;
		}
		unless (streq(buf, s)) {
			/* mark the file boundries */
			if (buf[0]) csetKeys.deltas[csetKeys.n++] = (char*)1;
			*(int *)hash_alloc(r2i, s, 0, sizeof(int)) = csetKeys.n;
			strcpy(buf, s);
		}
		csetKeys.deltas[csetKeys.n++] = t;
		assert(csetKeys.n < fd);
		n++;
		s = r;
	}

	/* Add in ChangeSet keys */
	sccs_sdelta(cset, sccs_ino(cset), key);
	if (csetKeys.n > 0) csetKeys.deltas[csetKeys.n++] = (char*)1;
	*(int *)hash_alloc(r2i, key, 0, sizeof(int)) = csetKeys.n;
	for (d = cset->table; d; d = d->next) {
		unless ((d->type == 'D') && (d->flags & D_CSET)) continue;
		sccs_sdelta(cset, d, buf);
		if (hash_storeStr(db, buf, key)) {
			char	*root = hash_fetch(db, t, 0, 0);
			unless (streq(root, key)) {
				fprintf(stderr,
			    "check: key %s replicated in ChangeSet: %s %s\n",
				    buf, key, root);
			}
		}
		csetKeys.deltas[csetKeys.n++] = strdup(buf);
		assert(csetKeys.n < fd);
	}
	sccs_free(cset);
	csetKeys.deltas[csetKeys.n] = (char*)1;
	if (verbose > 2) {
		fprintf(stderr, "check: found %d keys in ChangeSet\n", n);
	}
	if (e) exit(1);
	return (db);
}

/*
 * List all revisions which have the specified key.
 */
private void
listCsetRevs(char *key)
{
	FILE	*keys = popen("bk sccscat -ar -h ChangeSet", "r");
	char	*t;
	int	first = 1;
	char	buf[MAXPATH*3];

	unless (keys) {
		perror("listCsetRevs");
		exit(1);
	}

	fprintf(stderr, "\tSame key found in ChangeSet:");
	while (fnext(buf, keys)) {
		if (chop(buf) != '\n') {
			fprintf(stderr, "bad data: <%s>\n", buf);
			goto out;
		}
		t = separator(buf); assert(t); *t++ = 0;
		unless (streq(t, key)) continue;
		for (t = buf; *t && !isspace(*t); t++);
		assert(isspace(*t));
		*t = 0;
		if (first) {
			first = 0;
		} else {
			fprintf(stderr, ",");
		}
		fprintf(stderr, "%s", buf);
	}
	fprintf(stderr, "\n");
out:	pclose(keys);
}

private char	*
getFile(char *root, MDBM *idDB)
{
	sccs	*s = sccs_keyinit(root, flags, idDB);
	char	*t;

	unless (s) return (strdup("[can not init]"));
	t = strdup(s->sfile);
	sccs_free(s);
	return (t);
}

private char	*
getRev(char *root, char *key, MDBM *idDB)
{
	sccs	*s = sccs_keyinit(root, flags, idDB);
	delta	*d;

	unless (s) return (strdup("[can not init]"));
	unless (d = sccs_findKey(s, key)) {
		sccs_free(s);
		return (strdup("[can not find key]"));
	}
	return (strdup(d->rev));
}

/*
 * Tag with D_SET all deltas in a cset.
 * Flag an error if delta already in cset.
 */
private void
markCset(sccs *s, delta *d)
{
	static time_t	now;

	unless (now) now = time(0);

	do {
		if (d->flags & D_SET) {
			/* warn if in the last 1.5 months */
			if ((now - d->date) < (45 * 24 * 60 * 60)) poly = 1;
			if (polyList) {
				fprintf(stderr,
				    "check: %s@%s "
				    "(%s@%s %.8s) in multiple csets\n",
				    s->gfile, d->rev,
				    d->user, d->hostname, d->sdate);
			}
		}
		d->flags |= D_SET;
		if (d->merge) {
			delta	*e = sfind(s, d->merge);

			assert(e);
			unless (e->flags & D_CSET) markCset(s, e);
		}
		d = d->parent;
	} while (d && !(d->flags & D_CSET));
}

private void
idsum(u8 *s)
{
	while (*s) id_sum += *s++;
}

/*
	1) for each key in the changeset file, we need to make sure the
	   key is in the source file and is marked.

	2) for each delta marked as recorded in the ChangeSet file, we
	   need to make sure it actually is in the ChangeSet file.

	3) for each tip, all but one need to be marked as in the ChangeSet
	   file and that one - if it exists - must be top of trunk.
	
	4) check that the file is in the recorded location

	5) rebuild the idcache if in -a mode.
*/
private int
check(sccs *s, HASH *db)
{
	static	int	haspoly = -1;
	delta	*d, *ino;
	int	errors = 0;
	int	i;
	char	*t;
	char	buf[MAXKEY];

	/*
	 * Make sure that all marked deltas are found in the ChangeSet
	 */
	for (d = s->table; d; d = d->next) {
		if (verbose > 3) {
			fprintf(stderr, "Check %s@%s\n", s->gfile, d->rev);
		}
		/* check for V1 LOD */
		if (d->r[0] != 1) {
			lod = 1;	/* global tag */
			errors++;
			unless ((fix & 2) || goneKey) {
				fprintf(stderr,
				    "Obsolete LOD data (chk4): %s|%s\n",
		    		    s->gfile, d->rev);
			}
		}

		unless (d->flags & D_CSET) continue;
		sccs_sdelta(s, d, buf);
		unless (t = hash_fetchStr(db, buf)) {
			char	*term;

			if (mixed && (term = sccs_iskeylong(buf))) {
				*term = 0;
				t = hash_fetch(db, buf, 0, 0);
			}
		}
		unless (t) {
			if (MONOTONIC(s) && d->dangling) continue;
			fprintf(stderr,
		    "%s: marked delta %s should be in ChangeSet but is not.\n",
			    s->gfile, d->rev);
			sccs_sdelta(s, d, buf);
			fprintf(stderr, "\t%s -> %s\n", d->rev, buf);
			errors++;
		} else if (verbose > 2) {
			fprintf(stderr, "%s: found %s in ChangeSet\n",
			    s->gfile, buf);
		}
	}

	/*
	 * The location recorded and the location found should match.
	 */
	unless (d = sccs_top(s)) {
		fprintf(stderr, "check: can't get TOT in %s\n", s->gfile);
		errors++;
	} else unless (sccs_setpathname(s)) {
		fprintf(stderr, "check: can't get spathname in %s\n", s->gfile);
		errors++;
	} else unless (resync ||
	    streq(s->sfile, s->spathname) || LOGS_ONLY(s)) {
		fprintf(stderr,
		    "check: %s should be %s\n", s->sfile, s->spathname);
		errors++;
		names = 1;
	}

	sccs_sdelta(s, ino = sccs_ino(s), buf);

	/*
	 * Rebuild the id cache if we are running in -a mode.
	 */
	if (all) {
		do {
			sccs_sdelta(s, ino, buf);
			if (s->grafted || !streq(ino->pathname, s->gfile)) {
				fprintf(idcache, "%s %s\n", buf, s->gfile);
				idsum(buf);
				idsum(s->gfile);
				idsum(" \n");
				if (mixed && (t = sccs_iskeylong(buf))) {
					*t = 0;
					 fprintf(idcache,
					    "%s %s\n", buf, s->gfile);
					idsum(buf);
					idsum(s->gfile);
					idsum(" \n");
					*t = '|';
				} 
			}
			unless (s->grafted) break;
			while (ino = ino->next) {
				if (ino->random) break;
			}
		} while (ino);
	}

	/*
	 * Check BitKeeper invariants, such as:
	 *  - no open branches (unless we are in a logging repository)
	 *  - xflags implied by s->state matches top-of-trunk delta.
	 */
#define	FL	ADMIN_BK|ADMIN_FORMAT|ADMIN_TIME
#define	RFL	ADMIN_FORMAT|ADMIN_TIME
	if (resync && sccs_admin(s, 0, SILENT|RFL, 0, 0, 0, 0, 0, 0, 0, 0)) {
		sccs_admin(s, 0, RFL, 0, 0, 0, 0, 0, 0, 0, 0);
		errors++;
	}
	if (!resync && sccs_admin(s, 0, SILENT|FL, 0, 0, 0, 0, 0, 0, 0, 0)) {
		sccs_admin(s, 0, FL, 0, 0, 0, 0, 0, 0, 0, 0);
	    	errors++;
	}

	if (csetKeys.n == 0) return (errors);

	/*
	 * Go through all the deltas that were found in the ChangeSet
	 * hash and belong to this file.
	 * Make sure we can find the deltas in this file.
	 *
	 * If we are in mixed key mode, try all three key styles:
	 * email|path|date
	 * email|path|date|chksum
	 * email|path|date|chksum|randombits
	 */
	errors += checkKeys(s, buf);

	unless (mixed) return (errors);

	for (i = 0, t = buf; t = strchr(t+1, '|'); i++);
	if (i == 4) {
		t = strrchr(buf, '|');
		*t = 0;
		errors += checkKeys(s, buf);
		*t = '|';
		while (*--t != '|');
		*t = 0;
		errors += checkKeys(s, buf);
		*t = '|';
	}
	if (i == 3) {
		t = strrchr(buf, '|');
		*t = 0;
		errors += checkKeys(s, buf);
		*t = '|';
	}

	/* If we are not already marked as a repository having poly
	 * cseted deltas, then check to see if it is the case
	 */
	if (haspoly == -1) haspoly = (exists(POLY) != 0);
	if (!haspoly && CSETMARKED(s)) {
		for (d = s->table; d; d = d->next) {
			d->flags &= ~D_SET;
		}
		for (d = s->table; d; d = d->next) {
			if (d->flags & D_CSET) markCset(s, d);
		}
	}
	return (errors);
}

private int
chk_merges(sccs *s)
{
	delta	*p, *m, *d;

	for (d = s->table; d; d = d->next) {
		unless (d->merge) continue;
		p = d->parent;
		assert(p);
		m = sfind(s, d->merge);
		assert(m);
		if (sccs_needSwap(p, m)) {
			if (fix) return (1);
			fprintf(stderr,
			    "%s|%s: %s/%s need to be swapped, run with -f.\n",
			    s->gfile, d->rev, p->rev, m->rev);
			return (1);
		}
	}
	return (0);
}

private int
isGone(sccs *s, char *key)
{
	char	buf[MAXKEY];

	if (key) return (mdbm_fetch_str(goneDB, key) != 0);
	sccs_sdelta(s, sccs_ino(s), buf);
	return (mdbm_fetch_str(goneDB, buf) != 0);
}

private int
checkKeys(sccs *s, char *root)
{
	int	errors = 0;
	int	i;
	char	*a;
	delta	*d;
	char	*p;
	HASH	*findkey;
	char	key[MAXKEY];

	unless (p = hash_fetch(csetKeys.r2i, root, 0, 0)) return (0);
	i = *(int *)p;
	assert(i < csetKeys.n);
	findkey = hash_new();
	for (d = s->table; d; d = d->next) {
		unless (d->flags & D_CSET) continue;
		sccs_sdelta(s, d, key);
		*(delta**)hash_fetchAlloc(findkey, key, 0, sizeof(delta *)) = d;
		unless (mixed) continue;
		*strrchr(key, '|') = 0;
		*(delta**)hash_fetchAlloc(findkey, key, 0, sizeof(delta *)) = d;
	}
	do {
		assert(csetKeys.deltas[i]);
		assert(csetKeys.deltas[i] != (char*)1);

		/*
		 * We should find the delta key in the s.file if it is
		 * in the ChangeSet file.
		 */
		unless ((p = hash_fetch(findkey, csetKeys.deltas[i], 0, 0)) &&
		    (d = *(delta **)p)) {
			/* skip if the delta is marked as being OK to be gone */
			if (isGone(s, csetKeys.deltas[i])) continue;

			/* Spit out key to be gone-ed */
			if (goneKey & 2) {
				printf("%s\n", csetKeys.deltas[i]);
				continue;
			}

			/* don't want noisy messages in this mode */
			if (goneKey) continue;

			/* let them know if they need to delete the file */
			if (isGone(s, 0)) {
				fprintf(stderr,
				    "Marked gone (chk1): %s\n", s->gfile);
				continue;
			} else {
		    		errors++;
			}


			/*
			 * If we get here we have the key in the ChangeSet
			 * file, we have the s.file, it's not marked gone,
			 * so complain about it.
			 */
			fprintf(stderr,
			    "Missing delta (chk2) in %s\n", s->gfile);
			unless (details) continue;
			a = csetFind(csetKeys.deltas[i]);
			fprintf(stderr,
			    "\tkey: %s in ChangeSet|%s\n",
			    csetKeys.deltas[i], a);
			free(a);
		} else unless (d->flags & D_CSET) {
			fprintf(stderr,
			    "%s@%s is in ChangeSet but not marked\n",
			   s->gfile, d->rev);
		    	errors++;
		} else if (verbose > 2) {
			fprintf(stderr, "%s: found %s from ChangeSet\n",
			    s->gfile, d->rev);
		}
	} while (csetKeys.deltas[++i] != (char*)1);

	hash_free(findkey);
	return (errors);
}

#if 0
dumpkeys(sccs *s)
{
	char	k[MAXKEY];
	delta	*d;

	for (d = s->table; d; d = d->next) {
		sccs_sdelta(s, d, k);
		fprintf(stderr, "%s %s\n", d->rev, k);
	}
}

dump(int key)
{
	int	i;

	for (i = 0; i < csetKeys.n; ++i) {
		if (csetKeys.deltas[i] == (char*)1) {
			fprintf(stderr, "-\n");
		} else {
			fprintf(stderr, "[%d]=%s\n", i, csetKeys.deltas[i]);
		}
	}
}

listMarks(HASH *db)
{
	kvpair	kv;
	int	n = 0;

	EACH_HASH(db) {
		n++;
		fprintf(stderr,
		    "check: %s is missing cset marks,\n", kv.key.dptr);
	}
	if (n) fprintf(stderr, "   run ``bk cset -fvM1.0..'' to correct.\n");
}
#endif

private char	*
csetFind(char *key)
{
	char	buf[MAXPATH*2];
	FILE	*p;
	char	*s;

	char *k, *r =0;

	sprintf(buf, "bk sccscat -ar -h ChangeSet");
	unless (p = popen(buf, "r")) return (strdup("[popen failed]"));
	while (fnext(buf, p)) {
		if (r) continue;
		chop(buf);				/* remove '\n' */
		for (s = buf; *s && !isspace(*s); s++); /* skip rev */
		for (k = s; *k && isspace(*k); k++);	/* skip space */
		for (; *k && !isspace(*k); k++);	/* skip root key */
		for (; *k && isspace(*k); k++);		/* skip space */
		unless (*k) return (strdup("[bad data]"));
		if (streq(key, k)) {
			*s = 0;
			r = strdup(buf);
		}
	}
	pclose(p);
	unless (r) return (strdup("[not found]"));
	return (r);
}

private int
chk_csetpointer(sccs *s)
{
	char	*csetkey = proj_rootkey(s->proj);

	if (s->tree->csetFile == NULL ||
	    !(streq(csetkey, s->tree->csetFile))) {
		fprintf(stderr, 
"Extra file: %s\n\
     belongs to: %s\n\
     should be:  %s\n",
			s->gfile,
			s->tree->csetFile == NULL ? "NULL" : s->tree->csetFile,
			csetkey);
		csetpointer++;
		return (1);
	}
	return (0);
}

/*
 * This function is called when we are doing a partial check after all
 * the files have been read.  The idcache is in 'idDB', and the
 * current pathnames to the checked files are in the 'keys' DB.  Look
 * for any inconsistancies and then update the idcache if something is
 * out of date.
 *
 * $idDB{rootkey} = pathname to where cache thinks the inode is
 * $keys{rootkey} = pathname to where the inode actually is
 * Both have long/short rootkeys.
 *
 * XXX Code to manipulate the id_cache should be moved to idcache.c in 4.0
 */
private int
update_idcache(MDBM *idDB, HASH *keys)
{
	kvpair	kv;
	int	updated = 0;
	char	*p, *e;
	char	*cached;	/* idcache idea of where the file is */
	char	*found;		/* where we found the gfile */
	int	inkeyloc;	/* is gfile in inode location? */

	EACH_HASH(keys) {
		p = strchr(kv.key.dptr, '|');
		assert(p);
		p++;
		e = strchr(p, '|');
		assert(e);
		*e = 0;
		found = kv.val.dptr;
		inkeyloc = streq(p, found);
		*e = '|';
		cached = mdbm_fetch_str(idDB, kv.key.dptr);
		/* FIXUP idDB if it is wrong */
		if (inkeyloc) {
			if (cached) {
				updated = !streq(cached, found);
				mdbm_delete(idDB, kv.key);
			}
		} else {
			if (!cached || !streq(cached, found)) {
				updated = 1;
				mdbm_store(idDB, kv.key, kv.val, MDBM_REPLACE);
			}
		}
	}
	if (updated) {
		init_idcache();
		EACH_KV (idDB) {
			fprintf(idcache, "%s %s\n", kv.key.dptr, kv.val.dptr);
			idsum(kv.key.dptr);
			idsum(kv.val.dptr);
			idsum(" \n");
		}
	}
	mdbm_close(idDB);
	return (updated);
}
