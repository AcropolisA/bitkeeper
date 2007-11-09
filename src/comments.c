/* Copyright (c) 1998-2000 L.W.McVoy */
#include "system.h"
#include "sccs.h"

#define	MAXCMT	1020

private	char	**saved;	/* saved copy of comments across files */
private	char	*comment;	/* a placed to parse the comment from */
private	int	gotComment;	/* seems redundant but it isn't, this is
				 * is how we know we have a null comment.
				 */

int
comments_save(char *s)
{
	char	*p, **split;
	int	i, len;

	unless (s) {
		comment = 0;
		goto out;
	}

	if (saved) freeLines(saved, free);
	saved = 0;
	split = splitLineToLines(s, 0);
	EACH(split) {
		if (comments_checkStr(split[i])) {
			freeLines(split, free);
			freeLines(saved, free);
			saved = 0;
			return (-1);
		}
		len = strlen(split[i]);
		if (len <= MAXCMT) {
			saved = addLine(saved, strdup(split[i]));
		} else {
			for (p = split[i]; len > MAXCMT; len -= MAXCMT) {
				saved = addLine(saved, strndup(p, MAXCMT));
				p += MAXCMT;
				fprintf(stderr,
				    "Splitting comment line \"%.50s\"...\n", p);
			}
			saved = addLine(saved, strdup(p));
		}
	}
	freeLines(split, free);
out:	gotComment = 1;
	return (0);
}

int
comments_savefile(char *s)
{
	FILE	*f;
	char	*last;
	int	split = 0;
	char	splitmsg[51];
	char	buf[MAXCMT];

	if (!s || !exists(s) || (size(s) <= 0)) {
		return (-1);
	}
	unless (f = fopen(s, "r")) {
		perror(s);
		return (-1);
	}
	gotComment = 1;
	comment = "";
	while (fnext(buf, f)) {
		last = buf;
		while (*last && (*last != '\n')) last++;
		while ((last > buf) && (last[-1] == '\r')) --last;
		if (split) {
			split = 0;
			if (last == buf) continue;
			fprintf(stderr,
			    "Splitting comment line \"%s\"...\n", splitmsg);
		}
		if ((last == &buf[MAXCMT - 1]) && !*last) {
			split = 1;
			strncpy(splitmsg, buf, 50);
			splitmsg[50] = 0;
		}
		*last = 0;	/* strip any trailing NL */
		if (comments_checkStr(buf)) {
			freeLines(saved, free);
			saved = 0;
			comment = "";
			fclose(f);
			return (-1);
		}
		saved = addLine(saved, strdup(buf));
	}
	if (saved) comment = 0;
	fclose(f);
	return (0);
}

int
comments_got(void)
{
	return (gotComment);
}

void
comments_done(void)
{
	int	i;

	if (!saved) return;
	EACH(saved) free(saved[i]);
	free(saved);
	saved = 0;
	gotComment = 0;
	comment = 0;
	/* XXX - NULL comment as well? */
}

delta *
comments_get(delta *d)
{
	int	i;

	unless (d) d = calloc(1, sizeof(*d));
	if (!comment && !saved && gotComment) return (d);
	if (!comment) {
		if (saved) {
			EACH(saved) comments_append(d, strdup(saved[i]));
			return (d);
		}
		if (sccs_getComments("Group comments", 0, d)) {
			return (0);
		}
		EACH_COMMENT(0, d) {
			saved = addLine(saved, strdup(d->cmnts[i]));
		}
	} else {
		d = sccs_parseArg(d, 'C', comment, 0);
	}
	if (d && (d->flags & D_ERROR)) {
		sccs_freetree(d);
		return (0);
	}
	return (d);
}

/*
 * Prompt the user with a set of comments, returning
 * 0 if they want to use them,
 * -1 for an error or an abort.
 */
int
comments_prompt(char *file)
{
	char	*cmd, buf[10];

	unless (editor || (editor = getenv("EDITOR"))) editor = EDITOR;
	while (1) {
		printf("\n-------------------------------------------------\n");
		fflush(stdout);
		if (cat(file)) return (-1);

		printf("-------------------------------------------------\n");
		printf("Use these comments: (e)dit, (a)bort, (u)se? ");
		fflush(stdout);
		unless (getline(0, buf, sizeof(buf)) > 0) return (-1);
		switch (buf[0]) {
		    case 'y': 
		    case 'u':
			return (0);
		    case 'e':
			cmd = aprintf("%s '%s'", editor, file);
			system(cmd);
			free(cmd);
			break;
		    case 'a':
		    case 'q':
			return (-1);
		}
	}
}

int
comments_readcfile(sccs *s, int prompt, delta *d)
{
	char	*cfile = sccs_Xfile(s, 'c');
	char	*p;
	MMAP	*m;

	unless (access(cfile, R_OK) == 0) return (-1);
	if (prompt && comments_prompt(cfile)) return (-2);
	unless (m = mopen(cfile, "r")) return (-1);
	while (p = mnext(m)) {
		comments_append(d, strnonldup(p));
	}
	mclose(m);
	return (0);
}

void
comments_cleancfile(char *file)
{
	char	*cfile = name2sccs(file);
	char	*p;

	p = strrchr(cfile, '/');
	if (p) {
		p[1] = 'c';
		unlink(cfile);
	}
	free(cfile);
}

void
comments_writefile(char *file)
{
	FILE	*f;
	int	i;

	if (f = fopen(file, "w")) {
		if (comment) fprintf(f, "%s\n", comment);
		EACH (saved) {
			fprintf(f, "%s\n", saved[i]);
		}
		fclose(f);
	}
}

int
comments_checkStr(u8 *s)
{
	assert(s);
	for (; *s; s++) {
		/* disallow control characters, but still allow UTF-8 */
		if ((*s < 0x20) && (*s != '\t')) {
			fprintf(stderr,
			    "Non-printable character (0x%x) is illegal in "
			    "comments string.\n", *s);
			return (1);
		}
	}
	return (0);
}

/*
 * Append 1 line to the comments for a delta.
 * A newline is added to the line.
 */
void
comments_append(delta *d, char *line)
{
	assert(d->localcomment || (d->cmnts == 0));

	d->localcomment = 1;
	d->cmnts = addLine(d->cmnts, line);
}

/* force comments to be allocated locally */
char **
comments_load(sccs *s, delta *d)
{
	char	**lines = 0;
	char	*p, *t;

	if (d->localcomment) goto out;
	d->localcomment = 1;
	unless (d->cmnts) goto out;
	assert(s && (s->state & S_SOPEN));
	p = s->mmap + p2int(d->cmnts);
	while (*p) {
		unless (strneq(p, "\001c ", 3)) break;
		p += 3;
		t = strchr(p, '\n');
		lines = addLine(lines, strndup(p, t-p));
		p = t+1;
	}
	d->cmnts = lines;
out:	return (d->cmnts);
}

void
comments_free(delta *d)
{
	if (d->localcomment) freeLines(d->cmnts, free);
	d->cmnts = 0;
	d->localcomment = 0;
}
