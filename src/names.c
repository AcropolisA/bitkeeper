/*
 * names.c - make sure all files are where they are supposed to be.
 *
 * Alg: for each file, if it is in the right place, skip it, if not,
 * move it to a temp name in BitKeeper/RENAMES and leave it for pass 2.
 * In pass 2, move each file out of BitKeeper/RENAMES to where it wants
 * to be, erroring if there is some other file in that place.
 */
#include "system.h"
#include "sccs.h"

private	 void	pass1(char *spath);
private	 void	pass2(u32 flags);
private	 int	try_rename(char *old, char *new, int dopass1, u32 flags);

private	int filenum;

int
names_main(int ac, char **av)
{
	sccs	*s;
	char	*n;
	int	c, todo = 0, error = 0;
	u32	flags = 0;

	if (ac == 2 && streq("--help", av[1])) {
		system("bk help names");
		return (1);
	}

	while ((c = getopt(ac, av, "q")) != -1) {
		switch (c) {
		    case 'q':	flags |= SILENT; break;		/* doc 2.0 */
		    default:	system("bk help -s names");
				return (1);
		}
	}
	
	names_init();
	for (n = sfileFirst("names", &av[optind], 0); n; n = sfileNext()) {
		unless (s = sccs_init(n, 0)) continue;
		unless (sccs_setpathname(s)) {
			fprintf(stderr,
			    "names: can't initialize pathname in %s\n",
			    	s->gfile);
			sccs_free(s);
			continue;
		}
		if (streq(s->spathname, s->sfile)) {
			sccs_free(s);
			continue;
		}
		if (sccs_clean(s, SILENT|CLEAN_SKIPPATH)) {
			fprintf(stderr,
			    "names: %s is edited and modified\n", s->gfile);
			fprintf(stderr, "Wimping out on this rename\n");
			sccs_free(s);
			error |= 2;
			continue;
		}
		sccs_close(s); /* for win32 */
		todo += names_rename(s->sfile, s->spathname, flags);
		sccs_free(s);
	}
	if (sfileDone()) error |= 4;
	names_cleanup(flags);
	return (error);
}

void
names_init(void)
{
	/* this should be redundant, we should always be at the project root */

	if (proj_cd2root()) {
		fprintf(stderr, "names: cannot find project root.\n");
		exit(1);
	}
	filenum = 0;
}

int
names_rename(char *pathold, char *pathnew, u32 flags)
{
	return(try_rename(pathold, pathnew, 1, flags));
}

void
names_cleanup(u32 flags)
{
	if (filenum) pass2(flags);
}

private	void
pass1(char *spath)
{
	char	path[MAXPATH];

	unless (filenum) {
		mkdir("BitKeeper/RENAMES", 0777);
		mkdir("BitKeeper/RENAMES/SCCS", 0777);
	}
	sprintf(path, "BitKeeper/RENAMES/SCCS/s.%d", ++filenum);
	if (rename(spath, path)) {
		fprintf(stderr, "Unable to rename(%s, %s)\n", spath, path);
	}
}

private	void
pass2(u32 flags)
{
	char	path[MAXPATH];
	sccs	*s;
	int	worked = 0, failed = 0;
	int	i;
	
	unless (filenum) return;
	for (i = 1; i <= filenum; ++i) {
		sprintf(path, "BitKeeper/RENAMES/SCCS/s.%d", i);
		unless (s = sccs_init(path, 0)) {
			fprintf(stderr, "Unable to init %s\n", path);
			failed++;
			continue;
		}
		unless (sccs_setpathname(s)) {
			fprintf(stderr,
			    "names: can't initialize pathname in %s\n",
			    	s->gfile);
			sccs_free(s);
			failed++;
			continue;
		}
		sccs_close(s); /* for Win32 NTFS */
		if (try_rename(path, s->spathname, 0, flags)) {
			fprintf(stderr, "Can't rename %s -> %s\n",
			    path, s->spathname);
			if (exists(s->spathname)) {
				fprintf(stderr,
				    "REASON: destination exists.\n");
			}
			fprintf(stderr, "ERROR: File left in %s\n", path);
			sccs_free(s);
			failed++;
			continue;
		}
		sccs_free(s);
		worked++;
	}
	unless (flags & SILENT) {
		fprintf(stderr,
		    "names: %d/%d worked, %d/%d failed\n",
		    worked, filenum, failed, filenum);
	}
}

/*
 * Just for fun, see if the place where this wants to go is taken.
 * If not, just move it there.  We should be clean so just do the s.file.
 */
private	int
try_rename(char *spathold, char *spathnew, int dopass1, u32 flags)
{
	sccs	*s;
	int	ret;

	assert(spathold);
	assert(spathnew);
	if (exists(spathnew)) {
		/* circular or deadlock */
		if (dopass1) pass1(spathold);
		return (1);
	}
	mkdirf(spathnew);
	if (rename(spathold, spathnew)) {
		if (dopass1) pass1(spathold);
		return (1);
	}
	unless (flags & SILENT) {
		fprintf(stderr, "names: %s -> %s\n", spathold, spathnew);
	}
	s = sccs_init(spathnew, flags|INIT_NOCKSUM);
	unless (s) return (1);
	ret = 0;
	if (do_checkout(s)) ret = 1;
	sccs_free(s);
	return (ret);
}
