/*
 * resolve.c - multi pass resolver for renames, contents, etc.
 *
 * a) pull everything out of RESYNC into RENAMES which has pending name changes
 * b) build a DBM of rootkeys true/false of all the files in the RESYNC dir
 * c) process the files in RENAMES, resolving any conflicts, and put the file
 *    in RESYNC - if there isn't a file there already.  If there is, ask if 
 *    they want to move it and if so, move it to RENAMES.
 * d) before putting the file in RESYNC, if the same pathname is taken in the
 *    repository and the rootkey of that file is not in our list, then ask
 *    the user if they want to move the other file out of the way or move
 *    this file somewhere else.  If somewhere else is chosen, repeat process
 *    using that name.  If move is chosen, then copy the file to RENAMES and
 *    put this file in RESYNC.
 * e) repeat RENAMES processing until RENAMES dir is empty.  Potentially all
 *    files could pass through RENAMES.
 * f) move each file from RESYNC to repository, making sure that if the file
 *    exists in the repository, we have either overwritten it or deleted it.
 *
 * Invariants:
 * opts->rootDB{key} = <path>.  If the key is there then the inode is in the
 * 	RESYNC directory or the RENAMES directory under <path>.
 */
#include "resolve.h"
#include "logging.h"

private	void	commit(opts *opts);
private	void	conflict(opts *opts, char *rfile);
private	int	create(resolve *rs);
private	void	edit_tip(resolve *rs, char *sf, delta *d, char *rf, int which);
private	void	freeStuff(opts *opts);
private	int	nameOK(opts *opts, sccs *s);
private	void	pass1_renames(opts *opts, sccs *s);
private	int	pass2_renames(opts *opts);
private	int	pass3_resolve(opts *opts);
private	int	pass4_apply(opts *opts);
private	int	passes(opts *opts);
private	int	pending(int checkComments);
private	int	pendingEdits(void);
private	int	pendingRenames(void);
private void	checkins(opts *opts, char *comment);
private	void	rename_delta(resolve *rs, char *sf, delta *d, char *rf, int w);
private	int	rename_file(resolve *rs);
private void	resolve_post(opts *o, int c);
private void	unapply(FILE *f);
private int	copyAndGet(opts *opts, char *from, char *to);
private int	writeCheck(sccs *s, MDBM *db);
private	void	listPendingRenames(void);
private	int	noDiffs(void);
private char	**find_files(opts *opts, int pfiles);

private MDBM	*localDB;	/* real name cache for local tree */
private MDBM	*resyncDB;	/* real name cache for resyn tree */

int
resolve_main(int ac, char **av)
{
	int	c;
	static	opts opts;	/* so it is zero */

	opts.pass1 = opts.pass2 = opts.pass3 = opts.pass4 = 1;
	setmode(0, _O_TEXT);
	while ((c = getopt(ac, av, "l|y|m;aAcdFi;qrstTvx;1234")) != -1) {
		switch (c) {
		    case 'a': opts.automerge = 1; break;	/* doc 2.0 */
		    case 'A': opts.advance = 1; break;		/* doc 2.0 */
		    case 'c': opts.noconflicts = 1; break;	/* doc 2.0 */
		    case 'd': 					/* doc 2.0 */
			opts.debug = 1; putenv("BK_DEBUG_CMD=YES"); break;
		    case 'F': opts.force = 1; break;		/* undoc? 2.0 */
		    case 'l':					/* doc 2.0 */
			if (optarg) {
				opts.log = fopen(optarg, "a");
			} else {
				opts.log = stderr;
			}
			break;
		    case 'm': opts.mergeprog = optarg; break;	/* doc 2.0 */
		    case 'q': opts.quiet = 1; break;		/* doc 2.0 */
		    case 'r': opts.remerge = 1; break;		/* doc 2.0 */
		    case 's':					/* doc */
			opts.automerge = opts.autoOnly = 1;
			break;
		    case 'T': /* -T is preferred, remove -t in 5.0 */
		    case 't': opts.textOnly = 1; break;		/* doc 2.0 */
		    case 'i':
			opts.partial = 1;
			opts.includes = addLine(opts.includes, strdup(optarg));
			break;
		    case 'x':
			opts.partial = 1;
			opts.excludes = addLine(opts.excludes, strdup(optarg));
			break;
		    case 'y': 					/* doc 2.0 */
			unless (opts.comment = optarg) opts.comment = "";
			break;
		    case '1': opts.pass1 = 0; break;		/* doc 2.0 */
		    case '2': opts.pass2 = 0; break;		/* doc 2.0 */
		    case '3': opts.pass3 = 0; break;		/* doc 2.0 */
		    case '4': opts.pass4 = 0; break;		/* doc 2.0 */
		    default:
		    	fprintf(stderr, "resolve: bad opt %c\n", optopt);
			system("bk help -s resolve");
			exit(1);
		}
    	}
	if (opts.quiet) putenv("BK_QUIET_TRIGGERS=YES");
	/*
	 * It is the responsibility of the calling code to set this env
	 * var to indicate that we were not run standalone, we are called
	 * from a higher level and they will run the triggers.
	 */
	if (getenv("FROM_PULLPUSH") && streq(getenv("FROM_PULLPUSH"), "YES")) {
		opts.from_pullpush = 1;
	}
	unless (opts.mergeprog) opts.mergeprog = getenv("BK_RESOLVE_MERGEPROG");
	if ((av[optind] != 0) && isdir(av[optind])) chdir(av[optind++]);
	while (av[optind]) {
		opts.includes = addLine(opts.includes, strdup(av[optind++]));
		opts.partial = 1;
	}
	if (opts.partial) opts.pass4 = 0;

	unless (opts.textOnly) putenv("BK_GUI=WANT_CITOOL");
	if (opts.pass3 && !opts.textOnly && !gui_useDisplay()) {
		opts.textOnly = 1; 
	}
	if (opts.pass3 &&
	    !opts.textOnly && !opts.quiet && !win32() && gui_useDisplay()) {
		fprintf(stderr,
		    "Using %s as graphical display\n", gui_displayName());
	}

	/*
	 * Load the config file before we may delete it in the apply pass
	 * and then go looking for it (and not find it).
	 * XXX - If Wayne ever starts looking for the backing file before
	 * returning stuff from the DB, this hack will fail.
	 */
	proj_checkout(0);

	/* Globbing is a user interface, not one we want in RESYNC. */
	putenv("BK_NO_FILE_GLOB=TRUE");

	/*
	 * Make sure we are where we think we are.
	 */
	unless (exists("BitKeeper/etc")) proj_cd2root();
	unless (exists("BitKeeper/etc")) {
		fprintf(stderr, "resolve: can't find package root.\n");
		resolve_cleanup(&opts, 0);
	}
	unless (exists(ROOT2RESYNC)) {
		fprintf(stderr,
		    "resolve: can't find RESYNC dir, nothing to resolve?\n");
		freeStuff(&opts);
		exit(0);
	}
	if (proj_isCaseFoldingFS(0)) {
		localDB = mdbm_mem();
		resyncDB = mdbm_mem();
	}
	c = passes(&opts);
	if (localDB) mdbm_close(localDB);
	if (resyncDB) mdbm_close(resyncDB);
	resolve_post(&opts, c);
	return (c);
}

private void
resolve_post(opts *opts, int c)
{
	char	*cmd = "resolve";

	if (opts->from_pullpush) return;

	/* XXX - there can be other reasons */
	if (c) {
		putenv("BK_STATUS=CONFLICTS");
	} else {
		putenv("BK_STATUS=OK");
	}
	if (getenv("BK_REMOTE") && streq(getenv("BK_REMOTE"), "YES")) {
		cmd = "remote resolve";
	}
	trigger(cmd, "post");
}

private void
listPendingRenames(void)
{
	int fd1;

	fprintf(stderr, "List of name conflicts:\n");
	fflush(stderr);
	fd1 = dup(1); dup2(2, 1); /* redirect stdout to stderr */
	system("bk _find . -name 'm.*' | xargs cat");
	dup2(fd1, 1); close(fd1);
}

/*
 * Do the setup and then work through the passes.
 */
private	int
passes(opts *opts)
{
	sccs	*s = 0;
	FILE	*p = 0;
	char	*t;
	char	buf[MAXPATH];
	char	path[MAXPATH];
	char	flist[MAXPATH];

	/* Make sure only one user is in here at a time. */
	if (sccs_lockfile(RESOLVE_LOCK, 0, 0)) {
		pid_t	pid;
		char	*host = "UNKNOWN";
		time_t	time;
		sccs_readlockf(RESOLVE_LOCK, &pid, &host, &time);
		fprintf(stderr, "Could not get lock on RESYNC directory.\n");
		fprintf(stderr, "lockfile: %s\n", RESOLVE_LOCK);
		fprintf(stderr, "Lock held by pid %d on host %s since %s\n",
		    pid, host, ctime(&time));
		return (1);
	}

	if (exists("SCCS/s.ChangeSet")) {
		unless (opts->idDB = loadDB(IDCACHE, 0, DB_IDCACHE)) {
			fprintf(stderr, "resolve: can't open %s\n", IDCACHE);
			resolve_cleanup(opts, 0);
		}
	} else {
		/* fake one for new repositories */
		opts->idDB = mdbm_mem();
	}
	chdir(ROOT2RESYNC);
	unless (opts->from_pullpush || opts->quiet) {
		fprintf(stderr,
		    "Verifying consistency of the RESYNC tree...\n");
	}
	unless (opts->from_pullpush || 
	    (sys("bk", "-r", "check", "-cR", SYS) == 0)) {
		syserr("failed.  Resolve not even started.\n");
		/* XXX - better help message */
		resolve_cleanup(opts, 0);
	}

	/*
	 * Pass 1 - move files to RENAMES and/or build up rootDB
	 */
	opts->pass = 1;
	bktmp(flist, "respass1");
	if (sysio(0, flist, 0, "bk", "sfiles", SYS)) {
		perror("sfiles list");
err:		if (p) fclose(p);
		if (flist[0]) unlink(flist);
		return (1);
	}
	unless (p = fopen(flist, "r")) {
		perror(flist);
		goto err;
	}
	unless (opts->rootDB = mdbm_open(NULL, 0, 0, GOOD_PSIZE)) {
		perror("mdbm_open");
		goto err;
	}
	if (opts->log) {
		fprintf(opts->log,
		    "==== Pass 1%s ====\n",
		    opts->pass1 ? "" : ": build DB for later passes");
	}
	while (fnext(buf, p)) {
		chop(buf);
		unless (s = sccs_init(buf, INIT_NOCKSUM)) continue;

		/* save the key for ALL sfiles, RESYNC and RENAMES alike */
		sccs_sdelta(s, sccs_ino(s), path);
		saveKey(opts, path, s->sfile);

		/* If we aren't doing pass 1, just remember the key */
		unless (opts->pass1) {
			sccs_free(s);
			continue;
		}

		/* skip stuff we've already moved */
		if (strneq("BitKeeper/RENAMES/SCCS/s.", buf, 25)) {
			sccs_free(s);
			continue;
		}

		sccs_close(s);
		pass1_renames(opts, s);
	}
	fclose(p);
	p = 0;
	unlink(flist);
	flist[0] = 0;
	if (opts->pass1 && !opts->quiet && opts->renames) {
		fprintf(stdlog,
		    "resolve: found %d renames in pass 1\n", opts->renames);
	}

	/*
	 * If there are edited files, bail.
	 * This shouldn't happen, takepatch looks for them and 
	 * the repository is locked, but just in case...
	 */
	if (opts->edited) {
		SHOUT();
		fprintf(stderr,
"There are modified files in the repository which are also in the patch.\n\
BitKeeper is aborting this patch until you check in those files.\n\
You need to check in the edited files and run takepatch on the file\n\
in the PENDING directory.  Alternatively, you can rerun pull or resync\n\
that will work too, it just gets another patch.\n");
		resolve_cleanup(opts, CLEAN_RESYNC);
	}

	/*
	 * Pass 2 - move files back into RESYNC
	 */
	if (opts->pass2) {
		int	save, old, n = -1;

		if (opts->log) fprintf(opts->log, "==== Pass 2 ====\n");
		opts->pass = 2;

		/*
		 * Keep going as long as we are reducing the list.
		 * We can hit cases where there is a conflict that would
		 * go away if we processed the whole list.
		 */
		do {
			old = n;
			n = pass2_renames(opts);
		} while (n && ((old == -1) || (n < old)));

		unless (n) {
			unless (opts->quiet || !opts->renames2) {
				fprintf(stdlog,
				    "resolve: resolved %d renames in pass 2\n",
				    opts->renames2);
			}
			goto pass3;
		}

		if (opts->noconflicts) {
			SHOUT();
			fprintf(stderr,
			    "Did not resolve %d renames, "
			    "no conflicts causing abort.\n", n);
			/*
			 * Call it again to get the error messages.
			 * This means that all the code that is called
			 * below this has to respect noconflicts.
			 */
			opts->resolveNames = 1;
			pass2_renames(opts);
			resolve_cleanup(opts, CLEAN_RESYNC|CLEAN_PENDING);
		}

		if (opts->automerge) {
			t = aprintf("| %d unresolved name conflict[s] |", n);
			opts->notmerged = addLine(opts->notmerged, t);
			opts->hadConflicts = n;
			goto pass3;
		}

		/*
		 * Now do the same thing, calling the resolver.
		 */
		opts->resolveNames = 1;
		save = opts->renames2;
		n = -1;
		do {
			old = n;
			n = pass2_renames(opts);
		} while (n && ((old == -1) || (n < old)));
		if (n && (opts->pass4 || opts->partial)) {
			fprintf(stderr,
			    "Did not resolve %d renames, abort\n", n);
			listPendingRenames();
			resolve_cleanup(opts, 0);
		}
		unless (opts->quiet) {
			fprintf(stdlog,
			    "resolve: resolved %d renames in pass 2\n",
			    opts->renames2);
		}
		opts->renamed = opts->renames2 - save;
	}

	/*
	 * Pass 3 - resolve content/permissions/flags conflicts.
	 */
pass3:	if (opts->pass3) pass3_resolve(opts);

	/*
	 * Pass 4 - apply files to repository.
	 */
	if (opts->pass4) pass4_apply(opts);  /* never returns */
	
	freeStuff(opts);

	/* Unlock the RESYNC dir so someone else can get in */
	chdir(RESYNC2ROOT);
	sccs_unlockfile(RESOLVE_LOCK);

	/*
	 * Whoohooo...
	 */
	return (0);
}

/* ---------------------------- Pass1 stuff ---------------------------- */

/*
 * Save the key away so we know what we have.
 * This is called for each file in (or added to) the RESYNC dir.
 */
void
saveKey(opts *opts, char *key, char *file)
{
	if (opts->debug) fprintf(stderr, "saveKey(%s)->%s\n", key, file);
	if (mdbm_store_str(opts->rootDB, key, file, MDBM_INSERT)) {
		fprintf(stderr, "Duplicate key: %s\n", key);
		fprintf(stderr, "\twanted by %s\n", file);
		fprintf(stderr,
		    "\tused by %s\n", mdbm_fetch_str(opts->rootDB, key));
		resolve_cleanup(opts, 0);
	}
}

/*
 * If there is a file in the repository in the same location,
 * and it is the same file (root keys match),
 * then return true. 
 *
 * If there is no matching file in local (both path slot and key slot), then
 * return 0.
 */
private	int
nameOK(opts *opts, sccs *s)
{
	char	path[MAXPATH], realname[MAXPATH];
	char	buf[MAXPATH];
	sccs	*local = 0;
	int	ret;

	if (CSET(s)) return (1); /* ChangeSet won't have conflicts */

	/*
	 * Same path slot and key?
	 */
	sprintf(path, "%s/%s", RESYNC2ROOT, s->sfile);
	if (resyncDB) {
		getRealName(path, resyncDB, realname);
		assert(strcasecmp(path, realname) == 0);
	} else {
		strcpy(realname, path);
	}
	if (streq(path, realname) &&
	    (local = sccs_init(path, INIT_NOCKSUM|INIT_MUSTEXIST)) &&
	    HAS_SFILE(local)) {
		if (EDITED(local) && sccs_hasDiffs(local, SILENT, 1)) {
			fprintf(stderr,
			    "Warning: %s is modified, will not overwrite it.\n",
			    local->gfile);
			opts->edited = 1;
		}
		sccs_sdelta(local, sccs_ino(local), path);
		sccs_sdelta(s, sccs_ino(s), buf);
		if (opts->debug) {
		    fprintf(stderr,
			"nameOK(%s) => paths match, keys %smatch\n",
			s->gfile, streq(path, buf) ? "" : "don't ");
		}
		sccs_free(local);
		return (streq(path, buf));
	}
	if (local) sccs_free(local);

	chdir(RESYNC2ROOT);
	sccs_sdelta(s, sccs_ino(s), buf);
	local = sccs_keyinit(buf, INIT_NOCKSUM, opts->idDB);
	if (local) {
		if (EDITED(local) && sccs_hasDiffs(local, SILENT, 1)) {
			fprintf(stderr,
			    "Warning: %s is modified, will not overwrite it.\n",
			    local->gfile);
			opts->edited = 1;
		}
		sccs_free(local);
		if (opts->debug) {
			fprintf(stderr,
			    "nameOK(%s) => keys match, paths don't match\n",
			    s->gfile);
		}
		ret = 0;
	} else if (exists(s->gfile)) {
		if (opts->debug) {
			fprintf(stderr,
			    "nameOK(%s) => no sfile, but has local gfile\n",
			    s->gfile);
		}
		ret = 0;
	} else {
		if (opts->debug) fprintf(stderr, "nameOK(%s) = 1\n", s->gfile);
		ret = 1;
	}
	chdir(ROOT2RESYNC);
	return (ret);
}

/*
 * Pass 1 - move to RENAMES
 */
private	void
pass1_renames(opts *opts, sccs *s)
{
	char	path[MAXPATH];
	static	int filenum;
	char	*mfile;
	char	*rfile;

	if (nameOK(opts, s)) {
		unlink(sccs_Xfile(s, 'm'));
		sccs_free(s);
		return;
	}

	unless (filenum) {
		mkdir("BitKeeper/RENAMES", 0777);
		mkdir("BitKeeper/RENAMES/SCCS", 0777);
	}
	do {
		sprintf(path, "BitKeeper/RENAMES/SCCS/s.%d", ++filenum);
	} while (exists(path));
	mfile = strdup(sccs_Xfile(s, 'm'));
	rfile = strdup(sccs_Xfile(s, 'r'));
	sccs_close(s);
	if (opts->debug) {
		fprintf(stderr, "%s -> %s\n", s->sfile, path);
	}
	if (rename(s->sfile, path)) {
		fprintf(stderr, "Unable to rename(%s, %s)\n", s->sfile, path);
		resolve_cleanup(opts, 0);
	} else if (opts->log) {
		fprintf(opts->log, "rename(%s, %s)\n", s->sfile, path);
	}
	if (exists(mfile)) {
		sprintf(path, "BitKeeper/RENAMES/SCCS/m.%d", filenum);
		if (rename(mfile, path)) {
			fprintf(stderr,
			    "Unable to rename(%s, %s)\n", mfile, path);
			resolve_cleanup(opts, 0);
		} else if (opts->log) {
			fprintf(opts->log, "rename(%s, %s)\n", mfile, path);
		}
	}
	if (exists(rfile)) {
		sprintf(path, "BitKeeper/RENAMES/SCCS/r.%d", filenum);
		if (rename(rfile, path)) {
			fprintf(stderr,
			    "Unable to rename(%s, %s)\n", rfile, path);
			resolve_cleanup(opts, 0);
		} else if (opts->log) {
			fprintf(opts->log, "rename(%s, %s)\n", rfile, path);
		}
	}
	sccs_free(s);
	free(mfile);
	free(rfile);
	opts->renames++;
}

/* ---------------------------- Pass2 stuff ---------------------------- */

/*
 * pass2_renames - for each file in RENAMES, try to move it in place.
 * Return the number of files found in RENAMES.
 * This is called in two passes - the first pass just moves nonconflicting
 * files into place.  The second pass, indicated by opts->resolveNames,
 * handles all conflicts.
 * This routine may be called multiple times in each pass, until it gets to
 * a pass where it does no work.
 */
private	int
pass2_renames(opts *opts)
{
	sccs	*s = 0;
	int	n = 0;
	int	i;
	resolve	*rs;
	char	*t;
	char	**names;
	char	path[MAXPATH];

	if (opts->debug) fprintf(stderr, "pass2_renames\n");

	unless (exists("BitKeeper/RENAMES/SCCS")) return (0);

	/*
	 * This needs to be sorted or the regressions don't pass
	 * because some filesystems do not do FIFO ordering on directory
	 * files (think hashed directories.
	 */
	names = getdir("BitKeeper/RENAMES/SCCS");
	EACH(names) {
		sprintf(path, "BitKeeper/RENAMES/SCCS/%s", names[i]);

		/* may have just been deleted it but be in readdir cache */
		unless (exists(path)) continue;

		t = strrchr(path, '/');
		unless (t[1] == 's') continue;

		unless ((s = sccs_init(path, INIT_NOCKSUM)) && HASGRAPH(s)) {
			if (s) sccs_free(s);
			fprintf(stderr, "Ignoring %s\n", path);
			continue;
		}
		if (opts->debug) {
			fprintf(stderr,
			    "pass2.%u %s %d done\n",
				opts->resolveNames, path, opts->renames2);
		}

		n++;
		rs = resolve_init(opts, s);

		/*
		 * This code is ripped off from the create path.
		 * We are looking for the case that a directory component is
		 * in use for a file name that we want to put there.
		 * If that's true, then we want to call the resolve_create()
		 * code and fall through if they said EAGAIN or just skip
		 * this one.
		 */
		if (rs->d && (slotTaken(opts, rs->dname) == DIR_CONFLICT)) {
			/* If we are just looking for space, skip this one */
			unless (opts->resolveNames) goto out;
			if (opts->noconflicts) {
				fprintf(stderr,
				    "resolve: dir/file conflict for ``%s'',\n",
				    rs->d->pathname);
				goto out;
			}
			if (resolve_create(rs, DIR_CONFLICT) != EAGAIN) {
				goto out;
			}
		}

		/*
		 * If we have a "names", then we have a conflict or a 
		 * rename, so do that.  Otherwise, this must be a create.
		 */
		if (rs->gnames) {
			rename_file(rs);
		} else {
			assert(rs->d); /* no rs->gnames implies single tip */
			create(rs);
		}
out:		resolve_free(rs);
	}
	freeLines(names, free);
	return (n);
}

/*
 * Extract the names from either a
 * m.file: rename local_name gca_name remote_name
 * or an
 * r.file: merge deltas 1.491 1.489 1.489.1.21 lm 00/01/08 10:39:11
 */
names	*
res_getnames(char *path, int type)
{
	FILE	*f = fopen(path, "r");
	names	*names = 0;
	char	*s, *t;
	char	buf[MAXPATH*3];

	unless (f) return (0);
	unless (fnext(buf, f)) {
out:		fclose(f);
		freenames(names, 1);
		return (0);
	}
	unless (names = calloc(1, sizeof(*names))) goto out;
	if (type == 'm') {
		unless (strneq("rename ", buf, 7)) {
			fprintf(stderr, "BAD m.file: %s", buf);
			goto out;
		}
		s = &buf[7];
	} else {
		assert(type == 'r');
		unless (strneq("merge deltas ", buf, 13)) {
			fprintf(stderr, "BAD r.file: %s", buf);
			goto out;
		}
		s = &buf[13];
	}

	t = s;
	if (type == 'm') { /* local_name gca_name remote_name */
		s = strchr(t, '|');
		*s++ = 0;
		unless (names->local = strdup(t)) goto out;
		t = s;
		s = strchr(t, '|');
		*s++ = 0;
		unless (names->gca = strdup(t)) goto out;
		chop(s);
		unless (names->remote = strdup(s)) goto out;
	} else { /* 1.491 1.489 1.489.1.21[ lm 00/01/08 10:39:11] */
		unless (s = strchr(s, ' ')) goto out;
		*s++ = 0;
		unless (names->local = strdup(t)) goto out;
		t = s;
		unless (s = strchr(s, ' ')) goto out;
		*s++ = 0;
		unless (names->gca = strdup(t)) goto out;
		t = s;
		if (s = strchr(t, ' ')) {
			*s = 0;
		} else {
			chop(t);
		}
		unless (names->remote = strdup(t)) goto out;
	}
	fclose(f);
	return (names);
}

/*
 * We think it is a create, but it might be a rename because an earlier
 * invocation of resolve already renamed it and threw away the m.file.
 * In that case, treat it like a non-conflicting rename with local & gca
 * the same and the remote side whatever it is in the RESYNC dir.
 *
 * It's a create if we can't find the file using the key and there is no
 * other file in that path slot.
 *
 * The lower level routines may make space for us and ask us to move it
 * with EAGAIN.
 */
private	int
create(resolve *rs)
{
	char	buf[MAXKEY];
	opts	*opts = rs->opts;
	sccs	*local;
	int	ret = 0;
	int	how;
	static	char *cnames[] = {
			"Huh?",
			"an SCCS file",
			"a regular file without an SCCS file",
			"another SCCS file in the patch",
			"another file already in RESYNC",
			"an SCCS file that is marked gone"
		};

	if (opts->debug) {
		fprintf(stderr,
		    ">> create(%s) in pass %d.%d\n",
		    rs->d->pathname, opts->pass, opts->resolveNames);
	}

	/* 
	 * See if this name is taken in the repository.
	 */
again:	if (how = slotTaken(opts, rs->dname)) {
		if (!opts->noconflicts &&
		    (how == GFILE_CONFLICT) && (ret = gc_sameFiles(rs))) {
			if (ret == EAGAIN) goto again;
			return (ret);
		}

		/* If we are just looking for space, skip this one */
		unless (opts->resolveNames) return (-1);

		if (opts->noconflicts) {
	    		fprintf(stderr,
			    "resolve: name conflict for ``%s'',\n",
			    rs->d->pathname);
	    		fprintf(stderr,
			    "\tpathname is used by %s.\n", cnames[how]);
			opts->errors = 1;
			return (-1);
		}
      		ret = resolve_create(rs, how);
		if (opts->debug) {
			fprintf(stderr, "resolve_create = %d %s\n",
			    ret, ret == EAGAIN ? "EAGAIN" : "");
		}
		if (ret == EAGAIN) goto again;
		return (ret);
	}
	
	/*
	 * Mebbe we resolved this rename already.
	 */
	sccs_sdelta(rs->s, sccs_ino(rs->s), buf);
	chdir(RESYNC2ROOT);
	local = sccs_keyinit(buf, INIT_NOCKSUM, opts->idDB);
	chdir(ROOT2RESYNC);
	if (local) {
		if (opts->debug) {
			fprintf(stderr, "%s already renamed to %s\n",
			    local->gfile, rs->d->pathname);
		}
		/* dummy up names which make it a remote move */
		if (rs->gnames) freenames(rs->gnames, 1);
		if (rs->snames) freenames(rs->snames, 1);
		rs->gnames	   = calloc(1, sizeof(names));
		rs->gnames->local  = strdup(local->gfile);
		rs->gnames->gca    = strdup(local->gfile);
		rs->gnames->remote = strdup(rs->d->pathname);
		rs->snames	   = calloc(1, sizeof(names));
		rs->snames->local  = name2sccs(rs->gnames->local);
		rs->snames->gca    = name2sccs(rs->gnames->gca);
		rs->snames->remote = name2sccs(rs->gnames->remote);
		sccs_free(local);
		assert(!exists(sccs_Xfile(rs->s, 'm')));
		return (rename_file(rs));
	}

	/*
	 * OK, looking like a real create.
	 */
	sccs_close(rs->s);
	if (ret = rename(rs->s->sfile, rs->dname)) {
		mkdirf(rs->dname);
		ret = rename(rs->s->sfile, rs->dname);
	}
	if (rs->opts->log) {
		fprintf(rs->opts->log, "rename(%s, %s) = %d\n", 
		    rs->s->gfile, rs->d->pathname, ret);
	}
	if (opts->debug) {
		fprintf(stderr,
		    "%s -> %s = %d\n", rs->s->gfile, rs->d->pathname, ret);
	}
	opts->renames2++;
	return (ret);
}

void
freenames(names *names, int free_struct)
{
	unless (names) return;
	if (names->local) free(names->local);
	if (names->gca) free(names->gca);
	if (names->remote) free(names->remote);
	if (free_struct) free(names);
}

/*
 * Handle renames.
 * May be called twice on the same file, when called with resolveNames
 * set, then we have to resolve the file somehow.
 */
private	int
rename_file(resolve *rs)
{
	opts	*opts = rs->opts;
	char	*to = 0;
	int	how = 0;

again:
	if (opts->debug) {
		fprintf(stderr, ">> rename_file(%s - %s)\n",
			rs->s->gfile, rs->d ? rs->d->pathname : "<conf>");
	}

	/*
	 * We can handle local or remote moves in no conflict mode,
	 * but not both.
	 * Only error on this once, so hence the check on resolveNames.
	 */
	if (opts->noconflicts &&
	    (!streq(rs->gnames->local, rs->gnames->gca) &&
	    !streq(rs->gnames->gca, rs->gnames->remote))) {
		unless (opts->resolveNames) return (-1);	/* shhh */
	    	fprintf(stderr, "resolve: can't process rename conflict\n");
		fprintf(stderr,
		    "%s ->\n\tLOCAL:  %s\n\tGCA:    %s\n\tREMOTE: %s\n",
		    rs->s->gfile,
		    rs->gnames->local, rs->gnames->gca, rs->gnames->remote);
		opts->errors = 1;
		return (-1);
	}

	if (opts->debug) {
		fprintf(stderr,
		    "%s ->\n\tLOCAL:  %s\n\tGCA:    %s\n\tREMOTE: %s\n",
		    rs->s->gfile,
		    rs->gnames->local, rs->gnames->gca, rs->gnames->remote);
	}

	/*
	 * See if we can just slide the file into place.
	 * If remote moved, and local didn't, ok if !slotTaken.
	 * If local is moved and remote isn't, ok if RESYNC slot is open.
	 */
	if (rs->d) {
		to = rs->dname;
		if (how = slotTaken(opts, to)) to = 0;
	}
	if (to) {
		char	*mfile;

		sccs_close(rs->s); /* for win32 */
		if (rename(rs->s->sfile, to)) {
			mkdirf(to);
			if (rename(rs->s->sfile, to)) return (-1);
		}
		mfile = strdup(sccs_Xfile(rs->s, 'm'));
		if (rs->revs) {
			delta	*d;
			char	rfile[MAXPATH];
			char	*t = strrchr(to, '/');

			t[1] = 'r';
			strcpy(rfile, to);
			t[1] = 's';
			if (rename(sccs_Xfile(rs->s, 'r'), rfile)) {
				free(mfile);
				return (-1);
			}
			if (rs->opts->log) {
				fprintf(rs->opts->log,
				    "rename(%s, %s)\n",
				    sccs_Xfile(rs->s, 'r'), rfile);
			}
			sccs_free(rs->s);
			rs->s = sccs_init(to, INIT_NOCKSUM);
			d = sccs_findrev(rs->s, rs->revs->local);
			assert(d);
			t = name2sccs(d->pathname);
			unless (streq(t, to)) {
				rename_delta(rs, to, d, rfile, LOCAL);
			}
			free(t);
			d = sccs_findrev(rs->s, rs->revs->remote);
			assert(d);
			t = name2sccs(d->pathname);
			unless (streq(t, to)) {
				rename_delta(rs, to, d, rfile, REMOTE);
			}
			free(t);
		}
		if (rs->opts->log) {
			fprintf(rs->opts->log,
			    "rename(%s, %s)\n", rs->s->sfile, to);
		}
		opts->renames2++;
		(void)unlink(mfile);	/* may not exist */
		free(mfile);
	    	return (0);
	}

	/*
	 * If we have a name conflict, we know we can't do anything at first.
	 */
	unless (opts->resolveNames) return (-1);

	/*
	 * Figure out why we have a conflict, if it is like a create
	 * conflict, try that.
	 */
	if (how) {
		int	ret;

		if ((ret = resolve_create(rs, how)) == EAGAIN) {
			how = 0;
			goto again;
		}
		return (ret);
	} else {
		return (resolve_renames(rs));
	}
}

/*
 * Move a file into the RESYNC dir in a new location and delta it such that
 * it has recorded the new location in the s.file.
 * If there is an associated r.file, update that to have the new tip[s].
 */
int
move_remote(resolve *rs, char *sfile)
{
	int	ret;
	delta	*d;
	char	rfile[MAXPATH];

	if (rs->opts->debug) {
		fprintf(stderr, "move_remote(%s, %s)\n", rs->s->sfile, sfile);
	}

	sccs_close(rs->s);
	if (ret = rename(rs->s->sfile, sfile)) {
		mkdirf(sfile);
		if (ret = rename(rs->s->sfile, sfile)) return (ret);
	}
	unlink(sccs_Xfile(rs->s, 'm'));
	if (rs->opts->resolveNames) rs->opts->renames2++;
	if (rs->opts->log) {
		fprintf(rs->opts->log, "rename(%s, %s)\n", rs->s->sfile, sfile);
	}

	/*
	 * If we have revs, then there is an r.file, so move it too.
	 * And delta the tips if they need it.
	 */
	if (rs->revs) {
		char	*t = strrchr(sfile, '/');

		t[1] = 'r';
		strcpy(rfile, sfile);
		t[1] = 's';
		if (ret = rename(sccs_Xfile(rs->s, 'r'), rfile)) return (ret);
		if (rs->opts->log) {
			fprintf(rs->opts->log,
			    "rename(%s, %s)\n", sccs_Xfile(rs->s, 'r'), rfile);
		}
		d = sccs_findrev(rs->s, rs->revs->local);
		assert(d);
		t = name2sccs(d->pathname);
		unless (streq(t, sfile)) {
			rename_delta(rs, sfile, d, rfile, LOCAL);
		}
		free(t);
		d = sccs_findrev(rs->s, rs->revs->remote);
		assert(d);
		t = name2sccs(d->pathname);
		unless (streq(t, sfile)) {
			rename_delta(rs, sfile, d, rfile, REMOTE);
		}
		free(t);
	} else unless (streq(rs->dname, sfile)) {
		rename_delta(rs, sfile, rs->d, 0, 0);
	}
	/* Nota bene: *s is may be out of date */
	return (0);
}

/*
 * Add a null rename delta to the specified tip.
 */
private	void
rename_delta(resolve *rs, char *sfile, delta *d, char *rfile, int which)
{
	char	*t;
	char	buf[MAXPATH+100];

	if (rs->opts->debug) {
		fprintf(stderr, "rename_delta(%s, %s, %s, %s)\n", sfile,
		    d->rev, d->pathname, which == LOCAL ? "local" : "remote");
	}
	edit_tip(rs, sfile, d, rfile, which);
	t = sccs2name(sfile);
	sprintf(buf, "-PyMerge rename: %s -> %s", d->pathname, t);
	free(t);

	/*
	 * We are holding this open in the calling process, have to close it
	 * for winblows.
	 * XXX - why not do an internal delta?
	 */
	win32_close(rs->s);
	if (rs->opts->log) {
		sys("bk", "delta", "-f", buf, sfile, SYS);
	} else {
		sys("bk", "delta", "-qf", buf, sfile, SYS);
	}
}

/*
 * Resolve a file type by taking the file the winning delta and checking it
 * over the losing delta.
 * winner == REMOTE means the local is delta-ed with the remote's data.
 */
void
type_delta(resolve *rs,
	char *sfile, delta *l, delta *r, char *rfile, int winner)
{
	char	buf[MAXPATH+100];
	char	*g = sccs2name(sfile);
	delta	*o, *n;		/* old -> new */
	sccs	*s;
	int	loser;

	if (rs->opts->debug) {
		fprintf(stderr, "type(%s, %s, %s, %s)\n", sfile,
		    l->rev, r->rev, winner == LOCAL ? "local" : "remote");
	}

	/*
	 * XXX - there is some question here as to should I do a get -i.
	 * This is not clear.
	 */
	if (winner == LOCAL) {
		loser = -REMOTE;
		o = r;
		n = l;
	} else {
		loser = -LOCAL;
		o = l;
		n = r;
	}
	edit_tip(rs, sfile, o, rfile, loser);
	if (S_ISREG(n->mode)) {
		/* bk _get -kpqr{n->rev} sfile > g */
		sprintf(buf, "-kpqr%s", n->rev);
		if (sysio(0, g, 0, "bk", "_get", buf, sfile, SYS)) {
			fprintf(stderr, "%s failed\n", buf);
			resolve_cleanup(rs->opts, 0);
		}
		chmod(g, n->mode);
	} else if (S_ISLNK(n->mode)) {
		assert(n->symlink);
		if (symlink(n->symlink, g)) {
			perror(g);
			resolve_cleanup(rs->opts, 0);
		}
	} else {
		fprintf(stderr,
		    "type_delta called on unknown file type %o\n", n->mode);
		resolve_cleanup(rs->opts, 0);
	}
	free(g);
	/* bk delta -qPy'Merge file types: {o->mode} {n->mode} sfile */
	sprintf(buf, "-qPyMerge file types: %s -> %s", 
	    mode2FileType(o->mode), mode2FileType(n->mode));
	if (sys("bk", "delta", "-f", buf, sfile, SYS)) {
		syserr("failed\n");
		resolve_cleanup(rs->opts, 0);
	}
	strcpy(buf, sfile);	/* it's from the sccs we are about to free */
	sfile = buf;
	sccs_free(rs->s);
	s = sccs_init(sfile, INIT_NOCKSUM);
	assert(s);
	if (rs->opts->debug) {
		fprintf(stderr, "type reopens %s (%s)\n", sfile, s->gfile);
	}
	rs->s = s;
	rs->d = sccs_top(s);
	assert(rs->d);
	free(rs->dname);
	rs->dname = name2sccs(rs->d->pathname);
}

/*
 * Add a null permissions delta to the specified tip.
 */
void
mode_delta(resolve *rs, char *sfile, delta *d, mode_t m, char *rfile, int which)
{
	char	*a = mode2a(m);
	char	buf[MAXPATH], opt[100];
	sccs	*s;

	if (rs->opts->debug) {
		fprintf(stderr, "mode(%s, %s, %s, %s)\n",
		    sfile, d->rev, a, which == LOCAL ? "local" : "remote");
	}
	edit_tip(rs, sfile, d, rfile, which);
	/* bk delta -[q]Py'Change mode to {a}' -M{a} sfile */
	sprintf(buf, "-%sPyChange mode to %s", rs->opts->log ? "" : "q", a);
	sprintf(opt, "-M%s", a);
	if (sys("bk", "delta", "-f", buf, opt, sfile, SYS)) {
		syserr("failed\n");
		resolve_cleanup(rs->opts, 0);
	}
	strcpy(buf, sfile);	/* it's from the sccs we are about to free */
	sfile = buf;
	sccs_free(rs->s);
	s = sccs_init(sfile, INIT_NOCKSUM);
	assert(s);
	if (rs->opts->debug) {
		fprintf(stderr, "mode reopens %s (%s)\n", sfile, s->gfile);
	}
	rs->s = s;
	rs->d = sccs_top(s);
	assert(rs->d);
	free(rs->dname);
	rs->dname = name2sccs(rs->d->pathname);
}

/*
 * Add a null flags delta to the specified tip.
 */
void
flags_delta(resolve *rs,
	char *sfile, delta *d, int flags, char *rfile, int which)
{
	char	*av[40];
	char	buf[MAXPATH];
	char	fbuf[10][25];	/* Must match list below */
	int	n, i, f;
	sccs	*s;
	int	bits = flags & X_MAYCHANGE;

	if (rs->opts->debug) {
		fprintf(stderr, "flags(%s, %s, 0x%x, %s)\n",
		    sfile, d->rev, bits, which == LOCAL ? "local" : "remote");
	}
	edit_tip(rs, sfile, d, rfile, which);
	sys("bk", "clean", sfile, SYS);
	av[n=0] = "bk";
	av[++n] = "admin";
	sprintf(buf, "-qr%s", d->rev);
	av[++n] = buf;

#define	add(s)		{ sprintf(fbuf[f], "-f%s", s); av[++n] = fbuf[f]; }
#define	del(s)		{ sprintf(fbuf[f], "-F%s", s); av[++n] = fbuf[f]; }
#define	doit(bit,s)	if (bits&bit) add(s) else del(s); f++ 
	f = 0;
	doit(X_RCS, "RCS");
	doit(X_YEAR4, "YEAR4");
#ifdef X_SHELL
	doit(X_SHELL, "SHELL");
#endif
	doit(X_EXPAND1, "EXPAND1");
	doit(X_SCCS, "SCCS");
	doit(X_EOLN_NATIVE, "EOLN_NATIVE");
	doit(X_EOLN_WINDOWS, "EOLN_WINDOWS");
	doit(X_NOMERGE, "NOMERGE");
	doit(X_MONOTONIC, "MONOTONIC");
	for (i = 0; i < f; ++i) av[++n] = fbuf[i];
	av[++n] = sfile;
	assert(n < 38);	/* matches 40 in declaration */
	av[++n] = 0;

	n = spawnvp(_P_WAIT, av[0], av);
	if (!WIFEXITED(n) || WEXITSTATUS(n)) {
		for (i = 0; av[i]; ++i) fprintf(stderr, "%s ", av[i]);
		fprintf(stderr, "failed\n");
		resolve_cleanup(rs->opts, 0);
	}
	strcpy(buf, sfile);	/* it's from the sccs we are about to free */
	sfile = buf;
	sccs_free(rs->s);
	s = sccs_init(sfile, INIT_NOCKSUM);
	assert(s);
	if (rs->opts->debug) {
		fprintf(stderr, "flags reopens %s (%s)\n", sfile, s->gfile);
	}
	rs->s = s;
	rs->d = sccs_top(s);
	assert(rs->d);
	free(rs->dname);
	rs->dname = name2sccs(rs->d->pathname);
}

/*
 * Set up to add a delta to the file, such as a rename or mode delta.
 * If which is set, update the r.file with the new data.
 *	which == LOCAL means do local rev, REMOTE means do remote rev.
 * If the which value is negative, it means don't get the data.
 */
private	void
edit_tip(resolve *rs, char *sfile, delta *d, char *rfile, int which)
{
	char	buf[MAXPATH+100];
	char	opt[100];
	FILE	*f;
	char	*t;
	char	*newrev;

	if (rs->opts->debug) {
		fprintf(stderr, "edit_tip(%s %s %s)\n",
		    sfile, d->rev, abs(which) == LOCAL ? "local" : "remote");
	}
	/* bk _get -e[g][q] -r{d->rev} sfile */
	sprintf(buf, "-e%s%s", which < 0 ? "g" : "", rs->opts->log ? "" : "q");
	sprintf(opt, "-r%s", d->rev);
	if (sys("bk", "_get", buf, opt, sfile, SYS)) {
		syserr("failed\n");
		resolve_cleanup(rs->opts, 0);
	}
	if (which) {
		t = strrchr(sfile, '/');
		assert(t && (t[1] == 's'));
		t[1] = 'p';
		f = fopen(sfile, "r");
		t[1] = 's';
		fnext(buf, f);
		fclose(f);
		newrev = strchr(buf, ' ');
		newrev++;
		t = strchr(newrev, ' ');
		*t = 0;
		f = fopen(rfile, "w");
		/* 0123456789012
		 * merge deltas 1.9 1.8 1.8.1.3 lm 00/01/15 00:25:18
		 */
		if (abs(which) == LOCAL) {
			free(rs->revs->local);
			rs->revs->local = strdup(newrev);
		} else {
			free(rs->revs->remote);
			rs->revs->remote = strdup(newrev);
		}
		fprintf(f, "merge deltas %s %s %s\n",
		    rs->revs->local, rs->revs->gca, rs->revs->remote);
		fclose(f);
	}
}

/*
 * Walk a directory tree and determine if it is a directory conflict or
 * it can be removed because the only contents are sfiles that will be
 * renamed as part of the incoming patch.
 */
private int
findDirConflict(char *file, struct stat *sb, void *token)
{
	opts	*opts = token;
	char	*t;
	char	*sfile;
	char	buf[MAXKEY];

	if (S_ISDIR(sb->st_mode)) {
		opts->dirlist = addLine(opts->dirlist, strdup(file));
		return (0);
	}

	if (t = strstr(file, "/SCCS/")) {
		if (strneq(t+6, "s.", 2)) {
			sccs	*local = sccs_init(file, INIT_NOCKSUM);

			sccs_sdelta(local, sccs_ino(local), buf);
			sccs_free(local);
			unless (mdbm_fetch_str(opts->rootDB, buf)) {
				if (opts->debug) {
					fprintf(stderr,
					    "%s new sfile in local repo\n",
					    file);
				}
				return (DIR_CONFLICT);
			}
		}
		/* ignore other files in SCCS */
		return (0);
	}
	/*
	 * Ignore gfiles with sfiles, because we'll pick them up above
	 */
	sfile = name2sccs(file);
	if (exists(sfile)) {
		free(sfile);
		return (0);
	}
	free(sfile);
	return (DIR_CONFLICT);
}

private int
pathConflict(opts *opts, char *gfile)
{
	char	*t, *s;
	
	for (t = strrchr(gfile, '/'); t; ) {
		*t = 0;
		if (exists(gfile) && !isdir(gfile)) {
			char	*sfile = name2sccs(gfile);

			/*
			 * If the gfile has a matching sfile and that file
			 * doesn't exist in the RESYNC dir, then it is not
			 * a conflict, the file has been successfully moved.
			 */
			unless (exists(sfile) && 
			    !mdbm_fetch_str(opts->rootDB, sfile)) {
				if (opts->debug) {
					fprintf(stderr,
					    "%s exists in local repository\n", 
					    gfile);
				}
				*t = '/';
				free(sfile);
				return (1);
			}
			free(sfile);
		}
		s = t;
		t = strrchr(t, '/');
		*s = '/';
	}
	return (0);
}

private	void
deleteDirs(char **list)
{
	int	i;

	reverseLines(list);
	EACH(list) {
		if (rmdir(list[i]) == 0) {
			fprintf(stderr,
			    "resolve: removing empty directory: %s\n", list[i]);
		}
	}
}


/*
 * Return 1 if the pathname in question is in use in the repository by
 * an SCCS file that is not already in the RESYNC/RENAMES dirs.
 * Return 2 if the pathname in question is in use in the repository by
 * a file without an SCCS file.
 * Return 3 if the pathname in question is in use in the RESYNC dir.
 */
int
slotTaken(opts *opts, char *slot)
{
	assert(slot);

	if (opts->debug) fprintf(stderr, "slotTaken(%s) = ", slot);

	if (exists(slot)) {
		if (opts->debug) fprintf(stderr, "%s exists in RESYNC\n", slot);
		return (RESYNC_CONFLICT);
	}

	/* load gone from RESYNC */
	unless (opts->goneDB) {
		opts->goneDB = loadDB(GONE, 0, DB_KEYSONLY|DB_NODUPS);
	}

	chdir(RESYNC2ROOT);
	if (exists(slot)) {
		char	buf2[MAXKEY];
		sccs	*local = sccs_init(slot, INIT_NOCKSUM);

		/*
		 * If we can find this key in the RESYNC dir then it is
		 * not a conflict, the file has been successfully moved.
		 */
		sccs_sdelta(local, sccs_ino(local), buf2);
		sccs_free(local);
		if (mdbm_fetch_str(opts->rootDB, buf2)) {
			/* in resync */
		} else if (mdbm_fetch_str(opts->goneDB, buf2)) {
			/* gone file */
			if (opts->debug) {
				fprintf(stderr,
				    "%s exists and is gone\n", slot);
			}
			chdir(ROOT2RESYNC);
			return (GONE_SFILE_CONFLICT);
		} else {
			if (opts->debug) {
				fprintf(stderr,
				    "%s exists in local repository\n", slot);
			}
			chdir(ROOT2RESYNC);
			return (SFILE_CONFLICT);
		}
	} else {
		char	*gfile = sccs2name(slot);

		if (exists(gfile)) {
			int	conf;

			if (isdir(gfile)) {
				opts->dirlist = 0;
				conf = walkdir(gfile, findDirConflict, opts);
				deleteDirs(opts->dirlist);
				freeLines(opts->dirlist, free);
			} else {
				conf = GFILE_CONFLICT;
			}
			if (conf && opts->debug) {
				fprintf(stderr,
				    "%s %s exists in local repository\n",
				    conf == DIR_CONFLICT ? "dir" : "file",
				    gfile);
			}
			free(gfile);
			chdir(ROOT2RESYNC);
			return (conf);
		} else if (pathConflict(opts, gfile)) {
			if (opts->debug) {
			    	fprintf(stderr,
				    "directory %s exists in local repository\n",
				    gfile);
			}
			free(gfile);
			chdir(ROOT2RESYNC);
			return (DIR_CONFLICT);
		}
		free(gfile);
	}
	chdir(ROOT2RESYNC);
	if (opts->debug) fprintf(stderr, "0\n");
	return (0);
}

/* ---------------------------- Pass3 stuff ---------------------------- */

private int
noDiffs(void)
{
	FILE	*p = popen("bk -r diffs", "r");
	int	none;

	unless (p) return (0);
	none = (fgetc(p) == EOF);
	pclose(p);
	return (none);
}

/*
 * Sanity check: the ChangeSet file has to have 2 tips if we 
 * got this far. If it doesn't, it means someone (possibly the
 * 3.2.x citool) has already committed part of the work.
 */
private int
fix_singletip(opts *opts)
{
	sccs	*s = 0, *sf = 0;
	delta	*a, *b;
	FILE	*f = 0;
	char	*rev;
	char	buf[MAXPATH];

	unless (s = sccs_csetInit(INIT_NOCKSUM)) {
		fprintf(stderr, 
		    "Can't init ChangeSet file in RESYNC directory\n");
err:		if (s) sccs_free(s);
		if (sf) sccs_free(sf);
		if (f) pclose(f);
		return (1);
	}
		
	if (sccs_findtips(s, &a, &b)) {
		/* everything's fine, graph has two tips */
		sccs_free(s);
		return (0);
	}
	unless (a->merge) {
		fprintf(stderr, "Failed assertion: "
		    "ChangeSet is single tip but not a merge.\n");
		goto err;
	}
	unless (opts->quiet) fprintf(stderr, "Autofixing partial merge\n");
	unless (f = popen("bk changes "
	    "-r+ -vnd'$unless(:CHANGESET:){:SFILE:|:REV:}'", "r")) {
		fprintf(stderr, "Could not popen changes\n");
		goto err;
	}
	while(fgets(buf, sizeof(buf), f)) {
		chomp(buf);
		rev = strchr(buf, '|');
		*rev++ = 0;
		unless (sf = sccs_init(buf, INIT_NOCKSUM)) {
			fprintf(stderr, "Could not init %s\n", buf);
			goto err;
		}
		unless (b = sccs_findrev(sf, rev)) {
			fprintf(stderr,
			    "Could not find revision %s in %s\n", rev, buf);
			goto err;
		}
		b->flags &= ~D_CSET;
		sccs_newchksum(sf);
		sccs_free(sf);
		sf = 0;
	}
	pclose(f);
	f = 0;
	/* preserve ChangeSet comments */
	comments_load(s, a);
	lines2File(a->cmnts, CCHANGESET);
	/* now stripdel the top revision of the ChangeSet file */
	a->flags |= D_SET;
	MK_GONE(s, a);
	if (sccs_stripdel(s, "resolve_autofix")) {
		fprintf(stderr, "Unable to stripdel ChangeSet "
		    "file in the RESYNC directory\n");
		goto err;
	}
	sccs_free(s);
	s = 0;
	if (system("bk get -q -eM -g " CHANGESET)) {
	        fprintf(stderr, "Get of ChangeSet failed\n");
	        goto err;
	}
	opts->willMerge = 1;
	return (0);
}

/*
 * Handle all the non-rename conflicts.
 * Handle committing anything which needs a commit.
 *
 * Idempotency is handled as follows:
 * a) if there is an r.file, we know that we need to do a merge unless there
 *    is also a p.file, in which case we've been here already.
 * b) if there is not an r.file, we should validate that there are no
 *    open branches in this LOD - if there are, reconstruct the r.file.
 *    XXX - not currently implemented.
 *
 * Text vs graphics:  if resolve is run in text only mode, then each commit
 * of a merge will cause resolve to run "ci" and prompt the user for comments.
 * If we are not run in text mode, then a commit of a merge just leaves the
 * file there and we run citool at the end.
 * In either case, the last step is to find out if we need a commit by looking
 * for the r.ChangeSet, or for any modified files, or for any files with
 * pending deltas.
 */
private	int
pass3_resolve(opts *opts)
{
	char	buf[MAXPATH], **conflicts, s_cset[] = CHANGESET;
	int	n = 0, i;
	int	mustCommit = 0, pc, pe;

	if (opts->log) fprintf(opts->log, "==== Pass 3 ====\n");
	opts->pass = 3;

	unless (opts->automerge) {
		FILE	*p;

		unless (p = popen("bk _find . -name 'm.*'", "r")) {
			perror("popen of find");
			resolve_cleanup(opts, 0);
		}
		while (fnext(buf, p)) {
			fprintf(stderr, "Needs rename: %s", buf);
			n++;
		}
		pclose(p);
	}
	if (n) {
		fprintf(stderr,
"There are %d pending renames which need to be resolved before the conflicts\n\
can be resolved.  Please rerun resolve and fix these first.\n", n);
		resolve_cleanup(opts, 0);
	}

	/*
	 * Process any conflicts.
	 * Note: this counts on the rename resolution having bumped these
	 * revs if need be.
	 * XXX Note: if we are going to be really paranoid, we should just
	 * do an sfiles, open up each file, check for conflicts, and
	 * reconstruct the r.file if necessary.  XXX - not done.
	 */
	conflicts = find_files(opts, 0);
	EACH(conflicts) {
		if (opts->debug) fprintf(stderr, "pass3: %s\n", conflicts[i]);
		if (streq(conflicts[i], "SCCS/s.ChangeSet")) continue;

		if (opts->debug) fprintf(stderr, "pass3: %s", conflicts[i]);

		conflict(opts, conflicts[i]);
		if (opts->errors) {
			goto err;
		}
	}
	resolve_tags(opts);
	unless (opts->quiet || !opts->resolved) {
		fprintf(stdlog,
		    "resolve: resolved %d conflicts in pass 3\n",
		    opts->resolved);
	}

	if (opts->errors) {
err:		fprintf(stderr, "resolve: had errors, nothing is applied.\n");
		resolve_cleanup(opts, 0);
	}

	/* hadConflicts only gets touched by automerge */
	if (opts->hadConflicts) {
		char	*nav[20];
		int	i;

		if (opts->autoOnly) {
			unless (opts->quiet) {
				fprintf(stderr,
				    "resolve: %d conflicts "
				    "will need manual resolve.\n",
				    opts->hadConflicts);
			}
			resolve_cleanup(opts, 0);
			/* NOT REACHED */
		}

		if (getenv("_BK_PREVENT_RESOLVE_RERUN")) {
			fprintf(stderr,
			    "resolve: %d unresolved conflicts, "
			    "nothing is applied.\n",
			    opts->hadConflicts);
			resolve_cleanup(opts, 0);
			exit(1);	/* Shouldn't get here */
	    	}

		nav[i=0] = "bk";
		nav[++i] = "resolve";
		if (opts->mergeprog) {
			nav[++i] = aprintf("-m%s", opts->mergeprog);
		}
		if (opts->quiet) nav[++i] = "-q";
		if (opts->textOnly) nav[++i] = "-t";
		if (opts->comment) nav[++i] = aprintf("-y%s", opts->comment);
		nav[++i] = 0;
		fprintf(stderr,
		    "resolve: %d unresolved conflicts, "
		    "starting manual resolve process for:\n",
		    opts->hadConflicts);
		EACH(opts->notmerged) {
			fprintf(stderr, "\t%s\n", opts->notmerged[i]);
		}
		freeLines(opts->notmerged, free);
		opts->notmerged = 0;
		chdir(RESYNC2ROOT);
		sccs_unlockfile(RESOLVE_LOCK);
		i = spawnvp(P_WAIT, "bk", nav);
		proj_restoreAllCO(0, opts->idDB);
		exit(WIFEXITED(i) ? WEXITSTATUS(i) : 1);
	}

	/*
	 * Get -M the ChangeSet file if we need to, before calling citool
	 * or commit.
	 */
	if (opts->partial) goto nocommit;
	if (mustCommit = exists("SCCS/r.ChangeSet")) {
		sccs	*s;
		resolve	*rs;

		s = sccs_init(s_cset, INIT_NOCKSUM);
		rs = resolve_init(opts, s);
		edit(rs);
		/* We may restore it if we bail early */
		rename(sccs_Xfile(s, 'r'), "BitKeeper/tmp/r.ChangeSet");
		resolve_free(rs);
		opts->willMerge = 1;
	} else if (exists("SCCS/p.ChangeSet")) {
		/*
		 * The only way I can think of this happening is if we are
		 * coming back in after passing the above clause on an
		 * earlier run.  I suppose this could happen if we asked
		 * about open logging and they aborted, got a key, and reran.
		 */
		mustCommit = 1;
	}

	/*
	 * Since we are about to commit, clean up the r.files
	 */
	freeLines(conflicts, free);
	opts->remerge = 1;  /* Make sure we catch all r.files */
	conflicts = find_files(opts, 0);
	EACH(conflicts) {
		char	*t = strrchr(conflicts[i], '/');

		t[1] = 'r';
		if (exists(conflicts[i])) unlink(conflicts[i]);
		t[1] = 's';
	}
nocommit:


	/*
	 * If there is nothing to do, bail out.
	 */
	pc = pending(0);
	pe = pendingEdits();
	unless (mustCommit || pc || pe) return (0);

	if (pe && noDiffs()) {
		if (opts->log) {
			fprintf(opts->log, "==== Pass 3 autocheckins ====\n");
		}
		checkins(opts, SCCS_MERGE);
		pe = 0;		/* saves a call to pendingEdits() below */
	}

	/*
	 * Always do autocommit if there are no pending changes.
	 * We supply a default comment because there is little point
	 * in bothering a user for a merge.
	 */
	unless (opts->partial || (pe && pendingEdits())) {
		if (opts->log) {
			fprintf(opts->log, "==== Pass 3 autocommits ====\n");
		}
		commit(opts);
		return (0);
	}

	if (fix_singletip(opts)) goto err;

	/*
	 * Unless we are in textonly mode, let citool do all the work.
	 */
	unless (opts->textOnly) {
		if (opts->partial) {
			char	*p, **av, **pfiles = 0;
			int	j = 0;

			/* Save off the files that need deltas so we
			 * can remove the r.file after the delta is done
			 */
			pfiles = find_files(opts, 1);
			/* Run citool across any files in conflict that
			 * have pending deltas.
			 */
			unless (nLines(pfiles)) {
				unless (opts->resolved) {
					fprintf(stderr,
					    "No files listed need deltas\n");
				}
				return (0);
			}
			av = (char **)malloc((nLines(pfiles) + 5) *
			    sizeof(*av));
			av[j++] = "bk";
			av[j++] = "citool";
			av[j++] = "-R";
			av[j++] = "-P";
			EACH(pfiles) av[j++] = pfiles[i];
			av[j] = 0;
			spawnvp(_P_WAIT, av[0], av);
			free(av);
			/* Now that citool is done, remove r.files for
			 * files that no longer need a delta.
			 */
			EACH(pfiles) {
				p = strrchr(pfiles[i], '/');
				p[1] = 'p';
				unless (exists(pfiles[i])) {
					p[1] = 'r';
					unlink(pfiles[i]);
				}
				p[1] = 's';
			}
			freeLines(pfiles, free);
		} else {
			if (sys("bk", "citool", "-R", SYS)) {
				syserr("failed, aborting.\n");
				resolve_cleanup(opts, 0);
			}
			if (pending(0) || pendingEdits()) {
				fprintf(stderr, "Failed to check in/commit "
				    "all files, aborting.\n");
				resolve_cleanup(opts, 0);
			}
			opts->didMerge = opts->willMerge;
		}
		return (0);
	}

	/*
	 * We always go look for pending and/or locked files even when
	 * in textonly mode, because an earlier partial run could have
	 * been in a different mode.  So be safe.
	 */
	checkins(opts, 0);
	if (opts->errors) {
		fprintf(stderr, "resolve: had errors, nothing is applied.\n");
		resolve_cleanup(opts, 0);
	}

	if (!opts->partial && pending(0)) {
		assert(!opts->noconflicts);
		commit(opts);
	}

	return (0);
}

void
do_delta(opts *opts, sccs *s, char *comment)
{
	int     flags = DELTA_FORCE;
	delta	*d = 0;

	comments_done();
	if (comment) {
		comments_save(comment);
		d = comments_get(0);
		flags |= DELTA_DONTASK;
	}
	if (opts->quiet) flags |= SILENT;
	sccs_restart(s);
	if (sccs_delta(s, flags, d, 0, 0, 0)) {
		fprintf(stderr, "Delta of %s failed\n", s->gfile);
		resolve_cleanup(opts, 0);
	} else {
		unlink(sccs_Xfile(s, 'r'));
	}
}

private void
checkins(opts *opts, char *comment)
{
	sccs	*s;
	FILE	*p;
	char	buf[MAXPATH];

	unless (p = popen("bk sfiles -c", "r")) {
		perror("popen of find");
		resolve_cleanup(opts, 0);
	}
	while (fnext(buf, p)) {
		chop(buf);
		s = sccs_init(buf, INIT_NOCKSUM);
		assert(s);
		do_delta(opts, s, comment);
		sccs_free(s);
	}
	pclose(p);
}
	
/*
 * Merge a conflict, manually or automatically.
 * We handle permission conflicts here as well.
 *
 * XXX - we need to handle descriptive text and symbols.
 * The symbol case is weird: a conflict is when the same symbol name
 * is in both branches without one below the trunk.
 */
private	void
conflict(opts *opts, char *sfile)
{
	sccs	*s;
	resolve	*rs;
	deltas	d;
	
	s = sccs_init(sfile, INIT_NOCKSUM);

	rs = resolve_init(opts, s);
	assert(rs->dname && streq(rs->dname, s->sfile));

	d.local = sccs_findrev(s, rs->revs->local);
	d.gca = sccs_findrev(s, rs->revs->gca);
	d.remote = sccs_findrev(s, rs->revs->remote);
	/*
	 * The smart programmer will notice that the case where both sides
	 * changed the mode to same thing is just silently merged below.
	 *
	 * The annoyed programmer will note that the resolver may
	 * replace the sccs pointer.
	 */
	unless (fileType(d.local->mode) == fileType(d.remote->mode)) {
		rs->opaque = (void*)&d;
		if (resolve_filetypes(rs) == EAGAIN) {
			resolve_free(rs);
			return;
		}
		s = rs->s;
		if (opts->errors) return;
		/* Need to re-init these, the resolve stomped on the s-> */
		d.local = sccs_findrev(s, rs->revs->local);
		d.gca = sccs_findrev(s, rs->revs->gca);
		d.remote = sccs_findrev(s, rs->revs->remote);
	}

	unless (d.local->mode == d.remote->mode) {
		rs->opaque = (void*)&d;
		if (resolve_modes(rs) == EAGAIN) {
			resolve_free(rs);
			return;
		}
		s = rs->s;
		if (opts->errors) return;
		/* Need to re-init these, the resolve stomped on the s-> */
		d.local = sccs_findrev(s, rs->revs->local);
		d.gca = sccs_findrev(s, rs->revs->gca);
		d.remote = sccs_findrev(s, rs->revs->remote);
	}

	/*
	 * Merge changes to the flags (RCS/SCCS/EXPAND1/etc).
	 */
	unless (sccs_xflags(d.local) == sccs_xflags(d.remote)) {
		rs->opaque = (void*)&d;
		resolve_flags(rs);
		s = rs->s;
		if (opts->errors) return;
		/* Need to re-init these, the resolve stomped on the s-> */
		d.local = sccs_findrev(s, rs->revs->local);
		d.gca = sccs_findrev(s, rs->revs->gca);
		d.remote = sccs_findrev(s, rs->revs->remote);
	}

	if (opts->debug) {
		fprintf(stderr, "Conflict: %s: %s %s %s\n",
		    s->gfile,
		    rs->revs->local, rs->revs->gca, rs->revs->remote);
	}
	if (opts->noconflicts) {
	    	fprintf(stderr,
		    "resolve: can't process conflict in %s\n", s->gfile);
err:		resolve_free(rs);
		opts->errors = 1;
		return;
	}

	/*
	 * See if we merged some symlink conflicts, if so
	 * there can't be any conflict left.
	 */
	if (S_ISLNK(fileType(d.local->mode))) {
		int	flags;
		delta	*e;

		assert(d.local->mode == d.remote->mode);
		if (sccs_get(rs->s, 0, 0, 0, 0, SILENT, "-")) {
			sccs_whynot("delta", rs->s);
			goto err;
		}
		if (!LOCKED(rs->s) && edit(rs)) goto err;
		comments_save("Auto merged");
		e = comments_get(0);
		sccs_restart(rs->s);
		flags = DELTA_DONTASK|DELTA_FORCE|(rs->opts->quiet? SILENT : 0);
		if (sccs_delta(rs->s, flags, e, 0, 0, 0)) {
			sccs_whynot("delta", rs->s);
			goto err;
		}
	    	rs->opts->resolved++;
		unlink(sccs_Xfile(rs->s, 'r'));
		resolve_free(rs);
		return;
	}

	if (opts->automerge) {
		automerge(rs, 0, 0);
		resolve_free(rs);
		return;
	}
	resolve_contents(rs);
	resolve_free(rs);
}

int
get_revs(resolve *rs, names *n)
{
	int	flags = PRINT | (rs->opts->debug ? 0 : SILENT);

	if (sccs_get(rs->s, rs->revs->local, 0, 0, 0, flags, n->local)) {
		fprintf(stderr, "Unable to get %s\n", n->local);
		return (-1);
	}

	if (sccs_get(rs->s, rs->revs->gca, 0, 0, 0, flags, n->gca)) {
		fprintf(stderr, "Unable to get %s\n", n->gca);
		return (-1);
	}
	if (sccs_get(rs->s, rs->revs->remote, 0, 0, 0, flags, n->remote)) {
		fprintf(stderr, "Unable to get %s\n", n->remote);
		return (-1);
	}
	return (0);
}

/*
 * Try to automerge.
 */
void
automerge(resolve *rs, names *n, int identical)
{
	char	cmd[MAXPATH*4];
	int	ret;
	char	*name = basenm(rs->d->pathname);
	names	tmp;
	int	do_free = 0;
	int	flags;
	delta	*a, *b;

	unless (sccs_findtips(rs->s, &a, &b)) {
		unless (rs->opts->quiet) {
			fprintf(stderr, "'%s' already merged\n",
			    rs->s->gfile);
		}
		return;
	}
	if (rs->opts->debug) fprintf(stderr, "automerge %s\n", name);

	unless (n) {
		sprintf(cmd, "BitKeeper/tmp/%s@%s", name, rs->revs->local);
		tmp.local = strdup(cmd);
		sprintf(cmd, "BitKeeper/tmp/%s@%s", name, rs->revs->gca);
		tmp.gca = strdup(cmd);
		sprintf(cmd, "BitKeeper/tmp/%s@%s", name, rs->revs->remote);
		tmp.remote = strdup(cmd);
		if (get_revs(rs, &tmp)) {
			rs->opts->errors = 1;
			freenames(&tmp, 0);
			return;
		}
		n = &tmp;
		do_free = 1;
	}

	unless (unlink(rs->s->gfile)) rs->s = sccs_restart(rs->s);
	if (identical || sameFiles(n->local, n->remote)) {
		assert(n);
		sys("cp", n->local, rs->s->gfile, SYS);
		goto same;
	}

	if (BINARY(rs->s)) {
		unless (rs->opts->quiet) {
			fprintf(stderr,
			    "Not automerging binary '%s'\n", rs->s->gfile);
		}
nomerge:	rs->opts->hadConflicts++;
		return;
	}
	if (NOMERGE(rs->s)) goto nomerge;

	/*
	 * The interface to the merge program is
	 * "smerge -lleft_ver -rright_ver"
	 * and the program must return as follows:
	 * 0 for no overlaps, 1 for some overlaps, 2 for errors.
	 */
	if (rs->opts->mergeprog) {
		ret = sys("bk", rs->opts->mergeprog,
		    n->local, n->gca, n->remote, rs->s->gfile, SYS);
	} else {
		char	*l = aprintf("-l%s", rs->revs->local);
		char	*r = aprintf("-r%s", rs->revs->remote);

		ret = sysio(0,
		    rs->s->gfile, 0, "bk", "smerge", l, r, rs->s->gfile, SYS);
		free(l);
		free(r);
	}

	if (do_free) {
		unlink(tmp.local);
		unlink(tmp.gca);
		unlink(tmp.remote);
		freenames(&tmp, 0);
	}
	if (ret == 0) {
		delta	*d;

		unless (rs->opts->quiet) {
			fprintf(stderr,
			    "Content merge of %s OK\n", rs->s->gfile);
		}
same:		if (!LOCKED(rs->s) && edit(rs)) return;
		comments_save("Auto merged");
		d = comments_get(0);
		rs->s = sccs_restart(rs->s);
		flags = DELTA_DONTASK|DELTA_FORCE|SILENT;
		if (sccs_delta(rs->s, flags, d, 0, 0, 0)) {
			sccs_whynot("delta", rs->s);
			rs->opts->errors = 1;
			return;
		}
	    	rs->opts->resolved++;
		unlink(sccs_Xfile(rs->s, 'r'));
		return;
	}
	rs->opts->notmerged =
	    addLine(rs->opts->notmerged,strdup(rs->s->gfile));
	unless (WIFEXITED(ret)) {
		fprintf(stderr, "Unknown merge status: 0x%x\n", ret);
		rs->opts->errors = 1;
		unlink(rs->s->gfile);
		return;
	}
	if (WEXITSTATUS(ret) == 1) {
		fprintf(stderr,
		    "Conflicts during automerge of %s\n", rs->s->gfile);
		rs->opts->hadConflicts++;
		unlink(rs->s->gfile);
		return;
	}
	fprintf(stderr,
	    "Automerge of %s failed for unknown reasons\n", rs->s->gfile);
	rs->opts->errors = 1;
	unlink(rs->s->gfile);
	return;
}

/*
 * Figure out which delta is the branch one and merge it in.
 */
int
edit(resolve *rs)
{
	int	flags = GET_EDIT|GET_SKIPGET|SILENT;
	char	*branch;

	branch = strchr(rs->revs->local, '.');
	assert(branch);
	if (strchr(++branch, '.')) {
		branch = rs->revs->local;
	} else {
		branch = rs->revs->remote;
	}
	if (sccs_get(rs->s, 0, branch, 0, 0, flags, "-")) {
		fprintf(stderr,
		    "resolve: cannot edit/merge %s\n", rs->s->sfile);
		rs->opts->errors = 1;
		return (1);
	}
	sccs_restart(rs->s);
	return (0);
}

/* ---------------------------- Pass4 stuff ---------------------------- */

private	int
pendingRenames(void)
{
	char	**d;
	int	i, n = 0;

	unless (d = getdir("BitKeeper/RENAMES/SCCS")) return (0);
	EACH (d) {
		fprintf(stderr, "Pending: BitKeeper/RENAMES/SCCS/%s\n", d[i]);
		n++;
	}
	freeLines(d, free);
	return (n);
}

/*
 * Return true if there are pending deltas (not in a changeset yet).
 * If checkComments is set, do not return true if all of the comments
 * are either the automerge or the sccs no diffs merge.
 */
private	int
pending(int checkComments)
{
	FILE	*f;
	char	buf[MAXPATH];
	char	*t;
	int	ret;

	unless (checkComments) {
		f = popen("bk sfiles -pC", "r");
		ret = fgetc(f) != EOF;
		pclose(f);
		return (ret);
	}
	f = popen("bk sfiles -pCA | bk sccslog -CA -", "r");
	while (fnext(buf, f)) {
		unless (t = strchr(buf, '\t')) {
			pclose(f);
			return (1);	/* error means we assume pending */
		}
		t++;
		chop(t);
		unless (streq(t, AUTO_MERGE) || streq(t, SCCS_MERGE)) {
			pclose(f);
			return (1);
		}
	}
	pclose(f);
	return (0);
}

/*
 * Return true if there are modified files not yet checked in (except ChangeSet)
 */
private	int
pendingEdits(void)
{
	char	buf[MAXPATH];
	FILE	*f;

	f = popen("bk sfiles -c", "r");

	while (fnext(buf, f)) {
		chop(buf);
		/* Huh? */
		if (streq("SCCS/s.ChangeSet", buf)) continue;
		pclose(f);
		return (1);
	}
	pclose(f);
	return (0);
}

/*
 * Commit the changeset.
 */
private	void
commit(opts *opts)
{
	int	i;
	FILE	*cmtfile;
	char	*cmds[12], *cmt = 0;

	cmds[i = 0] = "bk";
	cmds[++i] = "commit";
	cmds[++i] = "-R";
	/* force a commit if we are a null merge */
	unless (opts->resolved || opts->renamed) cmds[++i] = "-F";
	if (opts->quiet) cmds[++i] = "-s";
	if (opts->comment) {
	    	/* Only do comments if they really gave us one */
		if (opts->comment[0]) {
			cmt = cmds[++i] = aprintf("-y%s", opts->comment);
		}
	} else if (size("SCCS/c.ChangeSet") > 0) {
		cmds[++i] = "-c";
	} else if (cmtfile = fopen("SCCS/c.ChangeSet", "w")) {
		fputs("Merge\n", cmtfile);
		fclose(cmtfile);
	}
	cmds[++i] = 0;
	i = spawnvp(_P_WAIT, "bk", cmds);
	if (cmt) free(cmt);
	if (WIFEXITED(i) && !WEXITSTATUS(i)) {
		opts->didMerge = opts->willMerge;
		return;
	}
	fprintf(stderr, "Commit aborted, no changes applied.\n");
	resolve_cleanup(opts, 0);
}

private void
unfinished(opts *opts)
{
	FILE	*p;
	int	n = 0;
	char	buf[MAXPATH];

	if (pendingRenames()) resolve_cleanup(opts, 0);
	unless (p = popen("bk _find . -name '[mr].*'", "r")) {
		perror("popen of find");
		resolve_cleanup(opts, 0);
	}
	while (fnext(buf, p)) {
		if (strneq(buf, "BitKeeper/tmp/", 14)) continue;
		fprintf(stderr, "Pending: %s", buf);
		n++;
	}
	pclose(p);
	if (n) resolve_cleanup(opts, 0);
}

/*
 * Remove a sfile, if its parent is empty, remove them too.
 * We leave stuff in place if we are being called from apply;
 * if we are called from unapply, we try and clean up everything.
 */
private int
rm_sfile(char *sfile, int leavedirs)
{
	char	*p;

	assert(!IsFullPath(sfile));
	sys("bk", "clean", sfile, SYS);
	if (unlink(sfile)) {
		perror(sfile);
		return (-1);
	}
	p = strrchr(sfile, '/');
	unless (p) {
		/* This should never happen, we at least have SCCS/ */
		fprintf(stderr, "No slash in %s??\n", sfile);
		return (-1);
	}
	while (p > sfile) {
		*p-- = 0;
		if (isdir(sfile)) {
			char	rdir[MAXPATH];

			if (leavedirs) {
				sprintf(rdir, "%s/%s", ROOT2RESYNC, sfile);
				if (isdir(rdir)) break;
			}
			/* careful */
			if (emptyDir(sfile) && rmdir(sfile)) {
				perror(sfile);
				return (-1);
			}
		}
		while ((p > sfile) && (*p != '/')) p--;
	}
	return (0);
}

/*
 * Make sure the file name did not change case orientation after we copied it
 */
private int
chkCaseChg(FILE *f)
{
	char	buf[MAXPATH], realname[MAXPATH];

	fflush(f);
	rewind(f);
	assert(localDB);
	while (fnext(buf, f)) {
		chomp(buf);
		getRealName(buf, localDB, realname);
		unless (streq(buf, realname)) {
			if (strcasecmp(buf, realname) == 0) {
				getMsg2("case_folding",
				    buf, realname, '=', stderr);
			} else {
				assert("Unknown error" == 0);
			}
			return (-1);
		}
	}
	return (0);
}

/*
 * Make sure there are no edited files, no RENAMES, and {r,m}.files and
 * apply all files.
 */
private	int
pass4_apply(opts *opts)
{
	sccs	*r, *l;
	int	offset = strlen(ROOT2RESYNC) + 1;	/* RESYNC/ */
	int	eperm = 0, flags, ret;
	FILE	*f = 0;
	FILE	*save = 0;
	char	buf[MAXPATH];
	char	key[MAXKEY];
	MDBM	*permDB = mdbm_mem();
	char	*cmd = "apply";
	char	*p;

	if (opts->log) fprintf(opts->log, "==== Pass 4 ====\n");
	opts->pass = 4;

	/*
	 * If the user called us directly we don't have a repo lock and need
	 * to get one.
	 */
	chdir(RESYNC2ROOT);
	putenv("_BK_IGNORE_RESYNC_LOCK=YES");
	for ( ; !opts->from_pullpush; ) {
		unless (ret = repository_wrlock()) break;
		switch (ret) {
		    case LOCKERR_LOST_RACE:
			fprintf(stderr, "Waiting for write lock...\n");
			if (getenv("_BK_NO_LOCK_RETRY")) {
				resolve_cleanup(opts, 0);
			}
			sleep(1);
			break;
		    case LOCKERR_PERM:
			assert(0);	/* sane should have caught this */
		    default:
			fprintf(stderr, "Unknown locking error.\n");
			resolve_cleanup(opts, 0);
		}
	}
	chdir(ROOT2RESYNC);

	unfinished(opts);

	/*
	 * Call the pre-apply trigger if there is one so that we have
	 * one last chance to bail out.
	 */
	putenv("BK_CSETLIST=BitKeeper/etc/csets-in");
	if (getenv("BK_REMOTE") && streq(getenv("BK_REMOTE"), "YES")) {
		cmd = "remote apply";
	}
	if (ret = trigger(cmd,  "pre")) {
		switch (ret) {
		    case 3: flags = CLEAN_MVRESYNC; break;
		    case 2: flags = CLEAN_RESYNC; break;
		    default: flags = CLEAN_RESYNC|CLEAN_PENDING; break;
		}
		mdbm_close(permDB);
		resolve_cleanup(opts, CLEAN_NOSHOUT|flags);
	}

	/*
	 * Pass 4a - check for edited files and build up a list of files to
	 * backup and remove.
	 */
	chdir(RESYNC2ROOT);
	save = fopen(BACKUP_LIST, "w+");
	assert(save);
	unlink(PASS4_TODO);
	sprintf(key, "bk sfiles '%s' > " PASS4_TODO, ROOT2RESYNC);
	if (system(key) || !(f = fopen(PASS4_TODO, "r+")) || !save) {
		fprintf(stderr, "Unable to create|open " PASS4_TODO);
		fclose(save);
		mdbm_close(permDB);
		resolve_cleanup(opts, 0);
	}
	chmod(PASS4_TODO, 0666);
	while (fnext(buf, f)) {
		chop(buf);
		/*
		 * We want to check the checksum here and here only
		 * before we throw it over the wall.
		 */
		unless (r = sccs_init(buf, 0)) {
			fprintf(stderr,
			    "resolve: can't init %s - abort.\n", buf);
			fclose(save);
			fprintf(stderr, "resolve: no files were applied.\n");
			resolve_cleanup(opts, 0);
		}
#define	F ADMIN_FORMAT|ADMIN_TIME|ADMIN_BK
		if (sccs_admin(r, 0, SILENT|F, 0, 0, 0, 0, 0, 0, 0)) {
			sccs_admin(r, 0, F, 0, 0, 0, 0, 0, 0, 0);
			fprintf(stderr, "resolve: bad file %s;\n", r->sfile);
			fprintf(stderr, "resolve: no files were applied.\n");
			fclose(save);
			mdbm_close(permDB);
			resolve_cleanup(opts, 0);
		}
		/* Can I write where the file wants to go? */
		if (writeCheck(r, permDB)) eperm = 1;

		sccs_sdelta(r, sccs_ino(r), key);
		sccs_free(r);
		if (l = sccs_keyinit(key, INIT_NOCKSUM, opts->idDB)) {
			proj_saveCO(l);
			/*
			 * This should not happen, the repository is locked.
			 */
			if (sccs_clean(l, SILENT)) {
				fprintf(stderr,
				    "\nWill not overwrite edited %s\n",
				    l->gfile);
				fclose(save); 
				resolve_cleanup(opts, 0);
			}
			/* Can I write where it used to live? */
			if (writeCheck(l, permDB)) eperm = 1;

			fprintf(save, "%s\n", l->sfile);
			sccs_free(l);
			/* buf+7 == skip RESYNC/ */
			mdbm_store_str(opts->idDB, key, buf + 7, MDBM_REPLACE);
		} else {
			/*
			 * handle new files.
			 * This little chunk of magic is to detect BAM files
			 * and respect BAM_checkout.
			 */
			if ((p = strrchr(key, '|')) && strneq(p, "|B:", 3)) {
				proj_saveCOkey(0, key, proj_checkout(0) >> 4);
			} else {
				proj_saveCOkey(0, key, proj_checkout(0) & 0xf);
			}
		}
	}
	fclose(save);
	mdbm_close(permDB);

	if (eperm) {
		getMsg("write_perms", 0, '=', stderr);
		resolve_cleanup(opts, 0);
	}

	/*
	 * Pass 4b.
	 * Save the list of files and then remove them.
	 */
	if (size(BACKUP_LIST) > 0) {
		unlink(BACKUP_SFIO);
		if (sysio(BACKUP_LIST,
		    BACKUP_SFIO, 0, "bk", "sfio", "-omq", SYS)) {
			fprintf(stderr,
			    "Unable to create backup %s from %s\n",
			    BACKUP_SFIO, BACKUP_LIST);
			resolve_cleanup(opts, 0);
		}
		save = fopen(BACKUP_LIST, "rt");
		assert(save);
		while (fnext(buf, save)) {
			chop(buf);
			if (opts->log) fprintf(stdlog, "unlink(%s)\n", buf);
			if (rm_sfile(buf, 1)) {
				fclose(save);
				restore_backup(BACKUP_SFIO, 0);
				resolve_cleanup(opts, 0);
			}
		}
		fclose(save);
	}

	/*
	 * Pass 4c - apply the files.
	 */
	unless (save = fopen(APPLIED, "w+")) {
		restore_backup(BACKUP_SFIO, 0);
		resolve_cleanup(opts, 0);
	}
	fflush(f);
	rewind(f);

	while (fnext(buf, f)) {
		chop(buf);
		/*
		 * We want to get the part of the path without the RESYNC.
		 */
		if (exists(&buf[offset])) {
			fprintf(stderr,
			    "resolve: failed to remove conflict %s\n",
			    &buf[offset]);
			unapply(save);
			restore_backup(BACKUP_SFIO, 0);
			resolve_cleanup(opts, 0);
		}
		if (opts->log) {
			fprintf(stdlog, "copy(%s, %s)\n", buf, &buf[offset]);
		}
		if (copyAndGet(opts, buf, &buf[offset])) {
			perror("copy");
			fprintf(stderr,
			    "copy(%s, %s) failed\n", buf, &buf[offset]);
err:			unapply(save);
			restore_backup(BACKUP_SFIO, 0);
			resolve_cleanup(opts, 0);
		} else {
			opts->applied++;
		}
		fprintf(save, "%s\n", &buf[offset]);
	}
	fclose(f);

	/* Remove cache of cset revisions */
	delete_cset_cache(".", 0);

 	/*
	 * If case folding file system , make sure file name
	 * did not change case after we copied it
	 */
	if (localDB && chkCaseChg(save)) goto err;

	unless (opts->quiet) {
		fprintf(stderr,
		    "resolve: applied %d files in pass 4\n", opts->applied);
	}
	unless (opts->quiet) {
		fprintf(stderr,
		    "resolve: running consistency check, please wait...\n");
	}
	proj_restoreAllCO(0, opts->idDB);
	if (proj_sync(0)) {
		/*
		 * It's worth pointing out that we still call this when we
		 * are resolving the creation of a new project.  It doesn't
		 * happen in the real world, it's a makepatch -r.. |
		 * takepatch -i type of thing but I ran into it when doing
		 * some perf tests.
		 */
		sync();
	}
	if (proj_configbool(0, "partial_check")) {
		fflush(save); /*  important */
		ret = run_check(APPLIED, opts->quiet ? 0 : "-v");
	} else {
		ret = run_check(0, opts->quiet ? 0 : "-v");
	}
	if (ret) {
		fprintf(stderr, "Check failed.  Resolve not completed.\n");
		/*
		 * Clean up any gfiles we may have pulled out to run check.
		 */
		system("bk clean BitKeeper/etc");
		goto err;
	}
	fclose(save);
	unlink(BACKUP_LIST);
	unlink(BACKUP_SFIO);
	unlink(APPLIED);
	unlink(PASS4_TODO);
	flags = CLEAN_OK|CLEAN_RESYNC|CLEAN_PENDING;
	resolve_cleanup(opts, flags);
	/* NOTREACHED */
	return (0);
}

private int
writeCheck(sccs *s, MDBM *db)
{
	char	*t;
	char	path[MAXPATH];
	struct	stat	sb;

	t = s->sfile;
	if (strneq(t, "RESYNC/", 7)) t += 7;
	strcpy(path, t);	/* RESYNC/SCCS want SCCS */
	if (t = strrchr(path, '/')) *t = 0;
	while (1) {
		t = strrchr(path, '/');
		if (mdbm_store_str(db, path, "", MDBM_INSERT)) return (0);
		unless (lstat(path, &sb)) {
			if (S_ISDIR(sb.st_mode)) {
				if (access(path, W_OK) != 0) {
					fprintf(stderr,
					    "No write permission: %s\n", path);
					return (1);
				}
				/* Must check parent of SCCS dir */
				unless (t && streq(t, "/SCCS") ||
					streq(path, "SCCS")) {
					return (0);
				}
			} else {
				/*
				 * File where a directory needs to be,
				 * The rest of resolve will deal with
				 * this, just make sure we can write
				 * this directory so it can.
				 */
			}
		}
		if (t) {
			*t = 0;
		} else {
			if (streq(path, ".")) break;
			strcpy(path, ".");
		}
	}
	return (0);
}

/*
 * Do NOT assert in this function, return -1 instead so we clean up.
 */
private int
copyAndGet(opts *opts, char *from, char *to)
{
	if (link(from, to)) {
		if (mkdirf(to)) {
			fprintf(stderr,
			    "mkdir: %s: %s\n",
			    to, strerror(errno));
			return (-1);
		}
		/*
		 * We need to fall back to fileCopy() if:
		 * a) The enclosing tree and the RESYNC tree are on
		 *    different file system. 
		 * b) it is Samba, which does not support hard link.
		 * Note: It is alos reported that in AFS file system,
		 *    the link() call return EXDEV when we try to
		 *    create hard link across different
		 *    directories, because AFS's ACL is per directory.
		 */
		if (link(from, to) && fileCopy(from, to)) return (-1);
	}
	return (0);
}

/*
 * Unlink all the things we successfully applied.
 */
private void
unapply(FILE *f)
{
	char	buf[MAXPATH];

	fflush(f);
	rewind(f);
	while (fnext(buf, f)) {
		chop(buf);
		rm_sfile(buf, 0);
	}
	fclose(f);
}

private	void
freeStuff(opts *opts)
{
	/*
	 * Clean up allocations/files/dbms
	 */
	if (opts->log && (opts->log != stderr)) fclose(opts->log);
	if (opts->rootDB) mdbm_close(opts->rootDB);
	if (opts->idDB) mdbm_close(opts->idDB);
	if (opts->goneDB) mdbm_close(opts->goneDB);
}

private void
csets_in(opts *opts)
{
	sccs	*s;
	delta	*d;
	FILE	*in, *out;
	char	s_cset[] = CHANGESET;
	char	buf[MAXPATH];

	if (opts->didMerge) {
		chdir(ROOT2RESYNC);
		s = sccs_init(s_cset, 0);
		assert(s && HASGRAPH(s));
		d = sccs_top(s);
		assert(d && d->merge);
		in = fopen(CSETS_IN, "r");	/* RESYNC one */
		assert(in);
		sprintf(buf, "%s/%s", RESYNC2ROOT, CSETS_IN);
		unlink(buf);
		if (out = fopen(buf, "w")) {	/* real one */
			while (fnext(buf, in)) {
				unless (streq(buf, "\n")) fputs(buf, out);
			}
			sccs_sdelta(s, d, buf);
			fprintf(out, "%s\n", buf);
			fclose(out);
		}
		fclose(in);
		sccs_free(s);
		unlink(CSETS_IN);
		chdir(RESYNC2ROOT);
	} else {
		sprintf(buf, "%s/%s", ROOT2RESYNC, CSETS_IN);
		/* May not exist if we pulled in tags only */
		if (exists(buf)) rename(buf, CSETS_IN);
	}
}

void
resolve_cleanup(opts *opts, int what)
{
	char	buf[MAXPATH];
	char	pendingFile[MAXPATH];
	FILE	*f;
	int	rc = 1;

	unless (exists(ROOT2RESYNC)) chdir(RESYNC2ROOT);
	unless (exists(ROOT2RESYNC)) {
		fprintf(stderr, "cleanup: can't find RESYNC dir\n");
		fprintf(stderr, "cleanup: nothing removed.\n");
		goto exit;
	}

	unless (opts->from_pullpush) repository_wrunlock(0);

	/*
	 * Get the patch file name from RESYNC before deleting RESYNC.
	 */
	sprintf(buf, "%s/%s", ROOT2RESYNC, "BitKeeper/tmp/patch");
	unless (f = fopen(buf, "r")) {
		fprintf(stderr, "Warning: no BitKeeper/tmp/patch\n");
		pendingFile[0] = 0;
	} else {
		fnext(pendingFile, f);
		chop(pendingFile);
		fclose(f);
	}

	/*
	 * If we are done, save the csets file.
	 */
	if ((what & CLEAN_OK) && exists(buf)) csets_in(opts);
	if (what & CLEAN_RESYNC) {
		assert(exists("RESYNC"));
		if (rmtree("RESYNC")) {
			fprintf(stderr, "resolve: rmtree failed\n");
		}
	} else if (what & CLEAN_MVRESYNC) {
		char	*dir = savefile(".", "RESYNC-", 0);

		assert(exists("RESYNC"));
		assert(dir);
		unlink(dir);
		sys("mv", "RESYNC", dir, SYS);
	} else {
		if (exists(ROOT2RESYNC "/SCCS/p.ChangeSet")) {
			assert(!exists("RESYNC/ChangeSet"));
			unlink(ROOT2RESYNC "/SCCS/p.ChangeSet");
			rename(ROOT2RESYNC "/BitKeeper/tmp/r.ChangeSet",
			    ROOT2RESYNC "/SCCS/r.ChangeSet");
		}
		fprintf(stderr,
		    "resolve: RESYNC directory left intact.\n");
	}

	if (what & CLEAN_PENDING) {
		if (pendingFile[0] && !getenv("TAKEPATCH_SAVEDIRS")) {
			unlink(pendingFile);
		}
		rmdir(ROOT2PENDING);
	}

	/* XXX - shouldn't this be below the CLEAN_OK?
	 * And maybe claused on opts->pass4?
	 */
	proj_restoreAllCO(0, opts->idDB);

	freeStuff(opts);
	unless (what & CLEAN_OK) {
		unless (what & CLEAN_NOSHOUT) SHOUT2();
		goto exit;
	}

	resolve_post(opts, 0);

	/*
	 * Force a logging process even if we did not create a merge node
	 * This is needed because we do not carry the log marker in
	 * external patch, csets from remote tree would look like they
	 * have pending logs. If we don't force a loggging process now,
	 * we will get a bogus warning message on the next commit.
	 *
	 * if (opts->didMerge && !opts->logging) ...
	 */
	logChangeSet();

	/* Only get here from pass4_apply() */
	unless (opts->quiet) {
		fprintf(stderr,
		    "Consistency check passed, resolve complete.\n");
	}
	rc = 0;
exit:
	sccs_unlockfile(RESOLVE_LOCK);
	exit(rc);
}

typedef	struct {
	char	**files;
	opts	*opts;
	u32	pfiles_only:1;
} cinfo;

/*
 * LMXXX - This does way too many things and is almost certainly going to
 * bite us someday, I'd like it split up to do one thing well.
 */
private int
resolvewalk(char *file, struct stat *sb, void *data)
{
	char	*p, *q;
	cinfo	*ci = (cinfo *)data;
	opts	*opts = ci->opts;
	int	e;

	unless (p = strrchr(file, '/')) return (0);
	unless (strneq(file, "./", 2)) return (0);
	file += 2;
	assert(p[1] = 's');
	if (opts->partial && streq(file, "SCCS/s.ChangeSet")) return (0);
	p[1] = 'm';
	if (opts->automerge && exists(file)) goto out;
	p[1] = 'r';
	unless (ci->pfiles_only || exists(file)) goto out;
	p[1] = 'p';
	e = exists(file);
	if (ci->pfiles_only && !e) goto out;
	if (!ci->pfiles_only && !opts->remerge && e) goto out;
	p[1] = 's';
	q = sccs2name(file);

	/* just like in export -tpatch */
	if (opts->excludes || opts->includes) {
		int	skip = 0;

		if (opts->excludes && match_globs(q, opts->excludes, 0)) {
			if (opts->debug) fprintf(stderr, "SKIPx %s\n", q);
			skip = 1;
		}
		if (opts->includes && !match_globs(q, opts->includes, 0)) {
			if (opts->debug) fprintf(stderr, "SKIPi %s\n", q);
			skip = 1;
		}
		unless (skip) {
			if (opts->debug) fprintf(stderr, "ADD(%s)\n", q);
			ci->files = addLine(ci->files, strdup(file));
		}
	} else {
		if (opts->debug) fprintf(stderr, "ADD(%s)\n", q);
		ci->files = addLine(ci->files, strdup(file));
	}
	free(q);
out:	p[1] = 's';
	return (0);
}

private char **
find_files(opts *opts, int pfiles)
{
	cinfo   cinfo;

	cinfo.files = 0;
	cinfo.opts = opts;
	cinfo.pfiles_only = pfiles;
	walksfiles(".", resolvewalk, &cinfo);
	return (cinfo.files);
}
