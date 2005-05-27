/* Copyright (c) 1997 L.W.McVoy */
#include "system.h"
#include "sccs.h"

private	int	sccs_gone(int quiet, FILE *f);

/*
 * Emulate rm(1)
 *
 * usage: rm a b ....
 */
int
rm_main(int ac, char **av)
{
	char	*name;
	int	c, errors = 0;
	int 	useCommonDir = 0;
	int	force = 0;

	if (streq(basenm(av[0]), "rm")) useCommonDir = 1;
        while ((c = getopt(ac, av, "df")) != -1) {
                switch (c) {
                    case 'd': useCommonDir++; break;		/* undoc? 2.0 */
		    case 'f': force = 1; break;
                    default:
usage:			system("bk help -s rm");
                        return (1);
                }
        }
	if (ac < 2) goto usage;

	for (name = sfileFirst("sccsrm",&av[optind], 0);
	    name; name = sfileNext()) {
		errors |= sccs_rm(name, NULL, useCommonDir, force);
	}
	if (sfileDone()) errors |= 2;
	return (errors);
}

char *
sccs_rmName(sccs *s, int useCommonDir)
{
	char	path[MAXPATH];
	char	*r, *t, *b;
	int	try = 0;
	delta	*d;
	char	*root;

	b = basenm(s->sfile);
	if (useCommonDir) {
		unless (root = proj_root(s->proj)) {
			fprintf(stderr, "sccsrm: cannot find root?\n");
			return (NULL);
		}
		sprintf(path, "%s/BitKeeper/deleted/SCCS", root);
		t = &path[strlen(path)];
		*t++ = '/';
	} else {
		strcpy(path, s->sfile);
		t = strrchr(path, '/');
		assert(t);
		t++;
	}
	d = sccs_ino(s);
	if (d->random) {
		r = d->random;
	} else {
		char	buf[50];

		sprintf(buf, "%05u", d->sum);
		r = buf;
	}
	for (try = 0; ; try++) {
		if (try) {
			sprintf(t, "s..del-%s~%s~%d", &b[2], r, try);
		} else {
			sprintf(t, "s..del-%s~%s", &b[2], r);
		}
		unless (exists(path)) break;
	}
	return(strdup(path));
}

int
sccs_rm(char *name, char *del_name, int useCommonDir, int force)
{
	char	*rmName;
	char	*sfile;
	int	error = 0;
	sccs	*s;

	sfile = name2sccs(name);
	s = sccs_init(sfile, 0);
	unless (s && HASGRAPH(s) && BITKEEPER(s)) {
		fprintf(stderr,
		    "Warning: %s is not a BitKeeper file, ignored\n", name);
		sccs_free(s);
		return (1);
	}
	if (CSET(s) ||
	    (strneq("BitKeeper/", s->tree->pathname, 10) && !force)) {
		fprintf(stderr, "Will not remove BitKeeper file %s\n", name);
		sccs_free(s);
		return (1);
	}
	sccs_close(s);
	rmName = sccs_rmName(s, useCommonDir);
	if (del_name) strcpy(del_name, rmName);
	error |= sccs_mv(sfile, rmName, 0, 1, 0, force);
	sccs_free(s);
	free(rmName);
	free(sfile);
	return (error);
}

int
gone_main(int ac, char **av)
{
	int	c;
	int	quiet = 0;
	char	tmpfile[MAXPATH];
	FILE	*f;

	while ((c =  getopt(ac, av, "q")) != -1) { 
		switch (c) {
		    case 'q': quiet++; break;	/* doc 2.0 */
		    default: 
usage:			      system("bk help -s gone");
			      return (1);
		}
	}
	unless (av[optind]) goto usage;

	if (streq("-", av[optind])) exit(sccs_gone(quiet, stdin));
	unless (bktmp(tmpfile, "sccsrm")) exit(1);
	f = fopen(tmpfile, "w");
	while (av[optind]) fprintf(f, "%s\n", av[optind++]);
	fclose(f);
	f = fopen(tmpfile, "r");
	if (sccs_gone(quiet, f)) {
		fclose(f);
		unlink(tmpfile);
		exit(1);
	}
	fclose(f);
	unlink(tmpfile);
	exit(0);
}

private int
sccs_gone(int quiet, FILE *f)
{
	int	i;
	sccs	*s;
	FILE	*g;
	char	*root;
	kvpair	kv;
	int	dflags = SILENT|DELTA_DONTASK;
	MDBM	*db = mdbm_mem();
	char	**lines = 0;
	char	s_gone[MAXPATH], g_gone[MAXPATH], key[MAXKEY];


	/* eat the keys first because check will complain if we edit the file */
	while (fnext(key, f)) mdbm_store_str(db, key, "", MDBM_INSERT);

	unless (root = proj_root(0)) {
		fprintf(stderr, "Can't find package root\n");
		exit(1);
	}
	sprintf(s_gone, "%s/BitKeeper/etc/SCCS/s.gone", root);
	sprintf(g_gone, "%s/BitKeeper/etc/gone", root);

	s = sccs_init(s_gone, SILENT);
	assert(s);
	if (exists(s_gone)) {
		unless (IS_EDITED(s)) {
			sccs_get(s, 0, 0, 0, 0, SILENT|GET_EDIT, "-"); 
		}
		g = fopen(g_gone, "r");
		while (fnext(key, g)) mdbm_store_str(db, key, "", MDBM_INSERT);
		fclose(g);
		s = sccs_restart(s);
	} else {
		dflags |= NEWFILE;
	}
	for (kv = mdbm_first(db); kv.key.dsize != 0; kv = mdbm_next(db)) {
		lines = addLine(lines, kv.key.dptr);
	}
	/* do not close the mdbm yet, we are using that memory */
	sortLines(lines, 0);
	unless (g = fopen(g_gone, "w")) {
		perror(g_gone);
		exit(1);
	}
	EACH(lines) fputs(lines[i], g);
	fclose(g);
	mdbm_close(db);
	comments_save("Be gone, sir, you annoy me.");
	sccs_restart(s);
	if (sccs_delta(s, dflags, 0, 0, 0, 0)) {
		perror("delta on gone file");
		exit(1);
	}
	sccs_free(s);
	unless (quiet) {
		fprintf(stderr,
		    "Do not forget to commit the gone file "
		    "before attempting a pull.\n");
	}
	return (0);
}
