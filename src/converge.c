#include "system.h"
#include "sccs.h"
#include "resolve.h"


/* global state for converge operations */
typedef struct {
	MDBM	*idDB;
	u32	iflags;		/* sccs_init flags */
	opts	opts;		/* resolve options */
} State;

private	void	converge(State *g, char *gfile, char *opts);
private	void	merge(State *g, char *gfile, char *pathname, char *opts);
private	sccs	*copy_to_resync(State *g, sccs *s);
private	void	free_slot(State *g, sccs *s);

/*
 * For certain files in BitKeeper/etc we automerge the contents in
 * takepatch.  Also since these files can be created on the first use
 * we need to make sure that files created in parallel are merged and
 * the oldest file is preserved.  This is so parallel merges will
 * converge on the same answer.
 */
void
converge_hash_files(void)
{
	FILE	*f;
	char	*t;
	int	i;
	char	*bn, *gfile;
	State	*g;
	struct	{
		char	*file;
		char	*opts;
	} files[] = {{ "BitKeeper/etc/aliases", "-hs"},
		     { "BitKeeper/etc/collapsed", "-s" },
		     { "BitKeeper/etc/gone", "-s" },
		     { "BitKeeper/etc/ignore", "-s" },
		     { "BitKeeper/etc/skipkeys", "-s"},
		     { 0, 0 }
	};

	/* everything in this file is run from the RESYNC dir */
	chdir(ROOT2RESYNC);
	g = new(State);
	g->iflags = INIT_NOCKSUM|INIT_MUSTEXIST|SILENT;

	/*
	 * Find all files in RESYNC that may contain a merge conflict for
	 * the files above and merge them.
	 */
	f = popen("bk sfiles -g BitKeeper/etc BitKeeper/deleted", "r");
	assert(f);
	while (gfile = fgetline(f))  {
		/* find basename of file with deleted stuff stripped */
		bn = basenm(gfile);
		t = 0;
		if (strneq(bn, ".del-", 5)) {
			bn += 5;
			if (t = strchr(bn, '~')) *t = 0;
		}
		for (i = 0; files[i].file; i++) {
			if (streq(bn, basenm(files[i].file))) {
				if (t) *t = '~'; /* restore gfile */
				merge(g, gfile, files[i].file, files[i].opts);
				break;
			}
		}
	}
	pclose(f);

	/*
	 * Now for each file, check to see if we need to converge multiple
	 * versions.
	 */
	for (i = 0; files[i].file; i++) {
		converge(g, files[i].file, files[i].opts);
	}

	if (g->idDB) {
		idcache_write(0, g->idDB);
		mdbm_close(g->idDB);
	}
	free(g);
	chdir(RESYNC2ROOT);
}

/*
 * Automerge any updates before converging the inodes.
 *
 * as long as 'gfile' was created at the path 'pathname', then
 * merge with 'bk merge -s gfile'
 */
private void
merge(State *g, char *gfile, char *pathname, char *opts)
{
	char	*sfile = name2sccs(gfile);
	char	*t;
	int	rc;
	sccs	*s;
	resolve	*rs;
	char	rootkey[MAXKEY];

	/* skip files with no conflicts */
	t = strrchr(sfile, '/'), t[1] = 'r';
	unless (exists(sfile)) {	 /* rfile */
		t[1] = 'm';
		unless (exists(sfile)) goto out;
	}
	t[1] = 's';

	/* only merge file if it is for the right pathname */
	s = sccs_init(sfile, g->iflags);
	sccs_close(s);
	unless (streq(sccs_ino(s)->pathname, pathname)) {
		sccs_free(s);
		goto out;
	}
	sccs_sdelta(s, sccs_ino(s), rootkey);
	rs = resolve_init(&g->opts, s);

	/* resolve rename conflicts */
	if (rs->gnames) {
		unless (g->idDB) g->idDB = loadDB(IDCACHE, 0, DB_IDCACHE);

		if (streq(rs->gnames->remote, pathname)) {
			/* must keep remote copy in place */
			t = name2sccs(pathname);
			if (!streq(gfile, pathname) &&
			    (s = sccs_init(t, g->iflags))) {
				/*
				 * Some other file is in the way.
				 * Get rid of it.
				 */
				free_slot(g, s);
				sccs_free(s);
			}
		} else  if (rs->dname) {
			/* try to resolve to "natural" name */
			t = name2sccs(rs->dname);
			if (!streq(gfile, rs->dname) && exists(t)) {
				/*
				 * conflicts with another sfile, oh well
				 * just delete it.
				 */
				free(t);
				t = sccs_rmName(s);
			}
		} else {
			/* resolve conflict by deleting */
			t = sccs_rmName(s);
		}
		move_remote(rs, t); /* fine if this doesn't move */

		/* 's' is stale, but update s->sfile and s->gfile */
		free(s->sfile);
		s->sfile = t;
		free(s->gfile);
		s->gfile = sccs2name(s->sfile);

		/* mark that it moved */
		mdbm_store_str(g->idDB, rootkey, s->gfile, MDBM_REPLACE);
	}

	/* handle contents conflicts */
	if (rs->revs) {
		/*
		 * Both remote and local have updated the file.
		 * We automerge here, saves trouble later.
		 */
		rc = sys("bk", "get", "-qeM", s->gfile, SYS);
		assert(!rc);
		rc = sysio(0, s->gfile, 0, "bk", "merge", opts, s->gfile, SYS);
		assert(!rc);
		rc = sys("bk", "ci", "-qdPyauto-union", s->gfile, SYS);
		assert(!rc);

		/* delete rfile */
		t = strrchr(s->sfile, '/'), t[1] = 'r';
		unlink(s->sfile);
		t[1] = 's';
	}
	resolve_free(rs);	/* free 's' too */
out:	free(sfile);
}

/*
 * The goal here is to converge on a single copy of a sfile when we
 * may have had multiple sfiles created in parallel.  So we look for
 * these create/create conflicts and resolve them by picking the
 * oldest sfile and merging in the contents of the other sfile.
 *
 * Note this function is distributed so it assumes both sides of a
 * merge have already been converged, so there is no need to look at
 * any files in the deleted directory.  We have already decided to
 * ignore them some other time.
 */
private void
converge(State *g, char *gfile, char *opts)
{
	sccs	*skeep, *srm, *s;
	int	rc;
	resolve	*rs;
	char	*sfile = name2sccs(gfile);
	char	key_keep[MAXKEY];
	char	key_rm[MAXKEY];
	char	buf[MAXPATH];
	char	tmp[MAXPATH];

	concat_path(buf, RESYNC2ROOT, sfile);	/* ../sfile */
	unless (exists(sfile) && exists(buf)) {
		/* no conflict, we are done */
		free(sfile);
		return;
	}

	skeep = sccs_init(buf, INIT_MUSTEXIST);
	assert(skeep);
	sccs_sdelta(skeep, sccs_ino(skeep), key_keep);
	srm = sccs_init(sfile, INIT_MUSTEXIST);
	assert(srm);
	sccs_sdelta(srm, sccs_ino(srm), key_rm);

	if (streq(key_keep, key_rm)) {
		/* same rootkey, no conflict */
		sccs_free(skeep);
		sccs_free(srm);
		free(sfile);
		return;
	}
	sccs_clean(skeep, SILENT);
	sccs_clean(srm, SILENT);

	/* pick the older sfile */
	if ((sccs_ino(srm)->date < sccs_ino(skeep)->date) ||
	    ((sccs_ino(srm)->date == sccs_ino(skeep)->date) &&
		(strcmp(key_rm, key_keep) < 0))) {
		s = skeep;
		skeep = srm;
		srm = s;
		sccs_sdelta(skeep, sccs_ino(skeep), key_keep);
		sccs_sdelta(srm, sccs_ino(srm), key_rm);
	}
	unless (g->idDB) g->idDB = loadDB(IDCACHE, 0, DB_IDCACHE);

	/* copy both sfiles to RESYNC */
	skeep = copy_to_resync(g, skeep);
	srm = copy_to_resync(g, srm);

	/* get contents of old version */
	bktmp(tmp, "converge");
	rc = sccs_get(srm, "+", 0, 0, 0, SILENT|PRINT, tmp);
	assert(!rc);
	sccs_free(srm);

	/* Check if something is in the way (other than skeep) */
	if (s = sccs_init(sfile, g->iflags)) {
		sccs_sdelta(s, sccs_ino(s), buf);
		unless (streq(buf, key_keep)) free_slot(g, s);
		sccs_free(s);
	}

	/* if skeep is not in the right place move it */
	unless (samepath(skeep->gfile, gfile)) {
		rs = resolve_init(&g->opts, skeep);
		assert(!rs->revs);
		assert(!rs->snames);
		move_remote(rs, sfile);
		rs->s = 0;
		mdbm_store_str(g->idDB, key_keep, gfile, MDBM_REPLACE);
		resolve_free(rs);
	}
	sccs_free(skeep);	/* done with skeep */

	/* Add other files contents as branch of 1.0 */
	rc = sys("bk", "_get", "-egqr1.0", gfile, SYS);
	assert(!rc);
	mv(tmp , gfile);		/* srm contents saved above */
	rc = sys("bk", "ci", "-qdPyconverge", gfile, SYS);
	assert(!rc);

	/*
	 * merge in new tip
	 *
	 * The get -M may fail if the ci above doesn't create a new
	 * delta, because the old file was empty.  In that case there is
	 * nothing to union.
	 */
	if (!sys("bk", "get", "-qeM", gfile, SYS)) {
		rc = sysio(0, gfile, 0, "bk", "merge", opts, gfile, SYS);
		assert(!rc);
		rc = sys("bk", "ci", "-qdPyauto-union", gfile, SYS);
		assert(!rc);
	}
	free(sfile);
}

/*
 * Assume we are in the repository root and move the sfile
 * for the given sccs* to the RESYNC directory.
 */
private sccs *
copy_to_resync(State *g, sccs *s)
{
	resolve	*rs;
	sccs	*snew;
	char	*rmName;
	char	rootkey[MAXKEY];

	/* already in RESYNC? */
	if (proj_isResync(s->proj)) return (s);

	/* Is it already there? */
	sccs_sdelta(s, sccs_ino(s), rootkey);

	if (snew = sccs_keyinit(0, rootkey, g->iflags, g->idDB)) {
		/* Found this rootkey in RESYNC already */
		sccs_free(s);
		return (snew);
	}

	/* so we need copy to RESYNC and then move to the deleted dir */
	fileCopy(s->sfile, "BitKeeper/etc/SCCS/s.converge-tmp");
	sccs_free(s);
	s = sccs_init("BitKeeper/etc/SCCS/s.converge-tmp", g->iflags);
	rs = resolve_init(&g->opts, s);
	assert(!rs->revs);
	assert(!rs->snames);
	rmName = sccs_rmName(s);
	move_remote(rs, rmName);
	resolve_free(rs);
	s = sccs_init(rmName, g->iflags);
	mdbm_store_str(g->idDB, rootkey, s->gfile, MDBM_REPLACE);
	free(rmName);
	return(s);
}

/* just delete the sfile to free up its current location */
private void
free_slot(State *g, sccs *s)
{
	resolve	*rs;
	char	*rmName, *t;
	char	key[MAXKEY];

	rs = resolve_init(&g->opts, s);
	sccs_sdelta(s, sccs_ino(s), key);
	rmName = sccs_rmName(s);
	move_remote(rs, rmName);
	t = sccs2name(rmName);
	free(rmName);
	mdbm_store_str(g->idDB, key, t, MDBM_REPLACE);
	free(t);
	rs->s = 0;
	resolve_free(rs);
}
