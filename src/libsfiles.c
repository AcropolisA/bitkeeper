#include "system.h"
#include "sccs.h"
WHATSTR("@(#)%K%");

/*
 * sfiles.c - s.file file name processing.
 *
 * File name expansion.
 *	<dir> means <dir>/SCCS/s.* if <dir>/SCCS exists
 *		XXX - shouldn't it really mean `bk sfiles dir`???
 *	<dir> means <dir>/s.* if <dir>/SCCS doesn't exist
 *	NULL means SCCS/s.* if SCCS exists
 *	NULL means ./s.* if SCCS doesn't exist
 *	<files> means <files>
 *	- means read files from stdin until EOF.
 *
 * XXX - if you have a file s.foo, "foo" doesn't work.  OK?
 * XXX - if SCCS/SCCS exists, that bites.
 *
 * char	*sfileFirst(av, flags);
 * char	*sfileNext();
 * void sfileDone();
 *
 * It is a fatal error to call sfileFirst() without calling sfileDone() or
 * before sfileNext() returned NULL.
 */
private	int flags;		/* saved flags */
private	char **av;		/* saved copy of argv */
private	int ac;			/* where we are - starts at 0 */
private	FILE *flist;		/* set if getting files from stdin */
private	DIR *d;			/* directory we are reading */
private	char prefix[MAXPATH];	/* path/to/dir/SCCS/ */
private	char buf[MAXPATH];	/* pathname we actually pass back */
private	int unget;		/* if set, return path again */
private	char *prog;		/* av[0], sort of */
private	char *glob;		/* if set, filter through this */
private	char rev[MAXREV+1];	/* 1.1.1.1 - see HASREVS */

private	int oksccs(char *s, int flags, int complain);

private int
isDelete(char *s)
{
	char	*t = strrchr(s, '/');

	t = t ? t+1 : s;
	return (strneq("s..del-", t, 5));
}

/*
 * Get the next file and munge it into an s.file name.
 */
char *
sfileNext()
{
	char	*name;
	struct	dirent *e;

	if (unget) {
		unget = 0;
		return (buf);
	}
again:
	if (!flist && !d && !av) {
		return (0);
	}
	if (flist) {
		unless (fgets(buf, sizeof(buf), flist)) {
			if (flist != stdin) fclose(flist);
			flist = 0;
			return (0);
		}
		chomp(buf);
		debug((stderr, "sfiles::FILE got %s\n", buf));
	} else if (d) {
		while ((e = readdir(d))) {
			/*
			 * readdir returns the base name only, must re-construct
			 * the relative path. Otherwise oksccs will be checking
			 * the wrong file.
			 * See test case "A/" in "basic test" of regression test
			 */
			concat_path(buf, prefix, e->d_name);
			/* I thought I didn't need this but I was wrong. */
			unless (oksccs(buf, flags, 0)) continue;
			if ((flags & SF_NOCSET) && isCsetFile(buf)) {
				continue;
			}
			debug((stderr, "sfiles::DIR got %s\n", buf));
			goto norev;
		}
		closedir(d);
		d = 0;
		return (0);
	} else if (av) {
		unless (name = av[ac++]) {
			av = 0;
			return (0);
		}
		strcpy(buf, name);
		localName2bkName(buf, buf);
		cleanPath(buf, buf); /* for win98 */
		debug((stderr, "sfiles::AV got %s\n", buf));
	}

	/*
	 * XXX TODO someday we'll handle escaped BK_FS in the filename
	 * But why would anyone want ^A in their file name ?
	 */
	if (flags & SF_HASREVS)  {
		char	*r = strrchr(buf, BK_FS);

		rev[0] = 0;	/* paranoia is your friend */
		if (!r) goto norev;
		*r++ = 0;
		strcpy(rev, r);
		debug((stderr, "sfiles::REV got %s\n", rev));
		/*
		 * XXX - this works for diffs but may or may not be the right
		 * long term answer.
		 */
		flags &= ~SF_GFILE;
	}
norev:
	unless (sccs_filetype(buf) == 's') {
#ifdef	ATT_SCCS
		fprintf(stderr, "Not an SCCS file: %s\n", buf);
		goto again;
#endif
		name = name2sccs(buf);
		unless (name) goto again;
		strcpy(buf, name);
		free(name);
	}
	/*
	 * Don't expand deleted file names unless they asked.
	 */
	if (isDelete(buf) && !(flags & SF_DELETES)) {
		goto again;
	}
	if (glob) {
		char	*f = strrchr(buf, '/');

		if (f) {
			f += 3;
		} else {
			f = &buf[2];
		}
		unless (match_one(f, glob, 0)) goto again;
	}
	if (oksccs(buf, flags, !(flags & SF_SILENT))) {
		return (buf);
	}
	goto again;
}

char *
sfileRev()
{
	return (rev[0] ? rev : 0);
}

int
sfiles_glob(char *glob)
{
	if (getenv("BK_NO_FILE_GLOB")) return (0);
	return (!strchr(glob, '/') && is_glob(glob));
}

/*
 * Initialization - figure out where we are getting the names from and
 * set it up.  This routine doesn't actually get any names, it calls
 * sfileNext() to do that.
 */
char *
sfileFirst(char *cmd, char **Av, int Flags)
{
	sfileDone();
	rev[0] = 0;
	prog = cmd;
	flags = Flags|SF_HASREVS; 
	if (Av[0]) {
		int	i;

		for (i = 0; Av[i+1]; ++i);
		if (streq("-", Av[i])) {
			if ((i > 0) && sfiles_glob(Av[i-1])) {
				glob = strdup(Av[i-1]);
				Av[i-1] = "-";
				Av[i] = 0;
			}
		} else if (sfiles_glob(Av[i])) {
			glob = strdup(Av[i]);
			Av[i] = 0;
		}
	}
	if (Av[0]) {
		if (streq("-", Av[0])) {
			if (Av[1]) {
				fprintf(stderr,
				    "%s: - option must be alone.\n", prog);
				sfileDone();
				return (0);
			}
			flist = stdin;
			flags |= SF_SILENT;
			/*
			 * If they specify a file on stdin then they must
			 * mean it.  (Doing otherwise breaks pending, commit,
			 * resolve on deleted files.)
			 */
			flags |= SF_DELETES;
			return (sfileNext());
		}
		localName2bkName(Av[0], Av[0]);
		if (isdir(Av[0])) {
			if (flags & SF_NODIREXPAND) return (0);
			if (Av[1]) {
				fprintf(stderr,
				    "%s: directory must be alone.\n", prog);
				sfileDone();
				return (0);
			}
			concat_path(prefix, Av[0], "SCCS");
			unless (d = opendir(prefix)) {
				/*
				 * trim off the "SCCS" part
				 * and try again
				 */
				prefix[strlen(prefix) - 4] = 0;
				unless (d = opendir(prefix)) {
					perror(prefix);
				}
			}
			return (sfileNext());
		}
		/*
		 * If they specify a file in argv then they must mean it.
		 */
		flags |= SF_DELETES;
		av = Av;
		ac = 0;
		return (sfileNext());
	}
	if (flags & SF_NODIREXPAND) return (0);
	if (!d) {
		strcpy(prefix, "SCCS");
		d = opendir("SCCS");
	}
	if (!d) {
		/*
		 * trim off the "SCCS" part
		 * and try again
		 */
		prefix[0] = 0;
		d = opendir(".");
	}
	return (sfileNext());
}

void
sfileDone()
{
	if (av) {
		av = 0;
		ac = 0;
	} else if (d) {
		closedir(d);
		d = 0;
	} else if (flist) {
		if (flist != stdin) fclose(flist);
		flist = 0;
	}
	if (glob) free(glob);
	glob = 0;
	prog = "";
}

private int
oksccs(char *sfile, int flags, int complain)
{
	char	*g;
	int	ok;
	struct	stat sbuf;

	unless (sccs_filetype(buf) == 's') {
		if (complain)
			fprintf(stderr, "%s: not an s.file: %s\n", prog, sfile);
		return (0);
	}
	g = sccs2name(sfile);
	ok = fast_lstat(g, &sbuf) == 0;
	if ((flags&SF_GFILE) && !ok) {
		if (complain) {
			unless (exists(sfile)) {
				fprintf(stderr,
				    "%s: neither '%s' nor '%s' exists.\n",
				    prog, g, sfile);
			} else {
				fprintf(stderr,
				    "%s: no such file: %s\n", prog, g);
			}
		}
		free(g);
		return (0);
	}
	if ((flags&SF_WRITE_OK) && (!ok || !(sbuf.st_mode & 0200))) {
		if (complain)
			fprintf(stderr,
			    "%s: %s: no write permission\n", prog, g);
		free(g);
		return (0);
	}
	free(g);
	return (1);
}

/*
 * concatenate two paths "first" and "second", and put the result in "buf"
 * TODO: This function should be grouped with cleanPath() and put in
 *	 the same file.
 */
void
concat_path(char *buf, char *first, char *second)
{
	int	len;
	if (buf != first) strcpy(buf, first);
	len = strlen(buf);
	if (len >= 2 &&
	    (buf[len -2] == '/') && (buf[len -1] == '.') && second[0]) {
		buf[len - 1] = 0; len--;
	}
	/*
	 * if "first" and "second" already have a seperator between them,
	 * don't add another one.
	 * Another special case is also checked here:
	 * 	first or "second" is a null string.
	 */
	if ((buf[0] != '\0') && (second[0] != '\0') &&
	    (buf[len -1] != '/') && (second[0] != '/'))
		strcat(buf, "/");
	strcat(buf, second);
}

#ifdef        MAIN
main(int ac, char **av)
{
	char	*name;
	sccs	*s;

	for (name = sfileFirst(&av[1], 0); name; name = sfileNext()) {
		printf("NAME %s\n", name);
		if (s = sccs_init(name, 0)) {
			printf("\t%s\n\t%s\n",
			    s->sfile, s->gfile);
			sccs_free(s);
		}
	}
}
#endif

