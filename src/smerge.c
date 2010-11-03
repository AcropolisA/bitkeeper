#include "system.h"
#include "sccs.h"
#include "logging.h"

typedef struct conflct	conflct;
typedef struct ld	ld_t;
typedef struct diffln	diffln;
typedef struct file	file_t;

private char	*find_gca(char *file, char *left, char *right);
private int	do_weave_merge(u32 start, u32 end);
private conflct	*find_conflicts(void);
private void	merge_conflicts(conflct *head);
private int	resolve_conflict(conflct *curr);
private diffln	*unidiff(conflct *curr, int left, int right);
private int	sameline(ld_t *left, ld_t *right);
private	int	file_init(char *file, char *rev, char *anno, file_t *f);
private	void	file_free(file_t *f);
private	int	do_diff_merge(void);

private void	highlight_diff(diffln *diff);

private	void	user_conflict(conflct *curr);
private	void	user_conflict_fdiff(conflct *curr);

/* automerge functions */
private	void	enable_mergefcns(char *list, int enable);
private	void	mergefcns_help(void);

private int	merge_same_changes(conflct *r);
private int	merge_only_one(conflct *r);
private int	merge_content(conflct *r);
private int	merge_common_header(conflct *r);
private int	merge_common_footer(conflct *r);
private	int	merge_common_deletes(conflct *r);
private int	merge_added_oneside(conflct *r);

enum {
	MODE_GCA,
	MODE_2WAY,
	MODE_3WAY,
	MODE_NEWONLY
};

enum {
	LEFT,
	GCA,
	RIGHT
};

/* ld = line data  (Information to save about each line in the file) */
struct ld {
	u32	seq;		/* seq number for line */
	char	*line;		/* Pointer to data in line */
	int	len;		/* length of line */
	char	*anno;		/* Pointer to annotations */
};

struct diffln {
	ld_t	*ld;
	int	*highlight;
	char	c;		/* character at start of line */
};

struct file {
	char	*tmpfile;
	MMAP	*m;
	ld_t	*lines;
	int	n;
};


private	char	*revs[3];
private	file_t	body[3];
private	char	*file;
private	int	mode;
private	int	fdiff;
private	char	*anno = 0;
#ifdef	SHOW_SEQ
private	int	parse_range(char *range, u32 *start, u32 *end);
private	int	show_seq;
#endif

int
smerge_main(int ac, char **av)
{
	int	c;
	int	i;
	u32	start = 0, end = ~0;
	int	ret = 2;
	int	do_diff3 = 0;
	int	identical = 0;
	char	*p;

	/* A "just in case" hook to disable one or more of the merge
	 * heuristics.  We can add a BK_MERGE_ENABLE in the future if
	 * needed.
	 */
	if (p = getenv("BK_MERGE_DISABLE")) {
		enable_mergefcns(p, 0);
	}
	if (getenv("BK_MERGE_DIFF3")) do_diff3 = 1;

	mode = MODE_3WAY;
	while ((c = getopt(ac, av, "234A;a;defghI;l;npr;R;s", 0)) != -1) {
		switch (c) {
		    case '2': /* 2 way format (like diff3) */
			mode = MODE_2WAY;
			break;
		    case '3': /* 3 way format (default) */
			mode = MODE_3WAY;
			break;
		    case 'g': /* gca format */
			mode = MODE_GCA;
			break;
		    case 'n': /* newonly (like -2 except marks added lines) */
			mode = MODE_NEWONLY;
			break;
		    case 'I': /* Add annotations (Info) */
		    	anno = optarg;
			break;
		    case 'A':
		    case 'a':
			enable_mergefcns(optarg, c == 'a');
			break;
		    case 'd':	do_diff3 = 1; break;
		    case 'f': /* fdiff output mode */
			fdiff = 1;
			break;
		    case 'e': /* show examples */
			getMsg("smerge_examples", 0, 0, stdout);
			return(2);
		    case 'l':
			revs[LEFT] = strdup(optarg);
			break;
		    case 'r':
			revs[RIGHT] = strdup(optarg);
			break;
#ifdef	SHOW_SEQ
/*
 * This stuff is removed for 3.0.
 * If it gets added back, there is also a regression for this
 * in the t.smerge history that should be recovered.
 */
		    case 'R': /* show output in the range <r> */
			if (parse_range(optarg, &start, &end)) {
				usage();
				return (2);
			}
			break;
		    case 's': /* show sequence numbers */
			show_seq = 1;
			break;
#endif
		    case 'h': /* help */
		    default: bk_badArg(c, av);
		}
	}
	unless (av[optind] && !av[optind+1] && revs[LEFT] && revs[RIGHT]) {
		system("bk help -s smerge");
		mergefcns_help();
		return (2);
	}
	if (fdiff && mode == MODE_3WAY) mode = MODE_GCA;
	file = av[optind];
	revs[GCA] = find_gca(file, revs[LEFT], revs[RIGHT]);

	/*
	 * Disable the merge_content heuristic if the merge GCA is a
	 * set node and not a single rev.  We have had cases of
	 * merging two merges that resolve a conflict different just
	 * deleting everything as a result of this code.  See testcase
	 * in same cset.
	 */
	if (strchr(revs[GCA], '+') || strchr(revs[GCA], '-')) {
		enable_mergefcns("3", 0);
	}

	for (i = 0; i < 3; i++) {
		if (file_init(file, revs[i], anno, &body[i])) {
			goto err;
		}
	}
	if (body[LEFT].n == body[RIGHT].n) {
		for (i = 0; i < body[LEFT].n; i++) {
			if (!sameline(&body[LEFT].lines[i],
				&body[RIGHT].lines[i])) break;
		}
		if (i == body[LEFT].n) {
			/* The left and right files are identical! */
			free(revs[RIGHT]);
			revs[RIGHT] = strdup(revs[LEFT]);
			file_free(&body[RIGHT]);
			body[RIGHT] = body[LEFT];
			identical = 1;
		}
	}

	if (do_diff3) {
		ret = do_diff_merge();
	} else {
		ret = do_weave_merge(start, end);
	}
 err:
	for (i = 0; i < 3; i++) {
		unless (i == RIGHT && identical) {
			file_free(&body[i]);
		}
		free(revs[i]);
	}
	return (ret);
}

static	char	**seqlist = 0;

void
smerge_saveseq(u32 seq)
{
	seqlist = addLine(seqlist, int2p(seq));
}

/*
 * Open a file and populate the file_t structure.
 */
private	int
file_init(char *file, char *rev, char *anno, file_t *f)
{
	char	*p;
	char	*end;
	int	l;
	sccs	*s;
	int	flags = GET_SEQ|SILENT|PRINT;
	int	i;
	char	*sfile = name2sccs(file);
	char	*inc, *exc;
	char	tmp[MAXPATH];

	if (anno) flags = annotate_args(flags|GET_ALIGN, anno);

	bktmp(tmp, "smerge");
	f->tmpfile = strdup(tmp);
	s = sccs_init(sfile, 0);
	unless (s && s->tree) return (-1);
	free(sfile);
	rev = strdup(rev);
	if (inc = strchr(rev, '+')) *inc++ = 0;
	if (exc = strchr(inc ? inc : rev, '-')) *exc++ = 0;

	if (sccs_get(s, rev, 0, inc, exc, flags, f->tmpfile)) {
		fprintf(stderr, "Fetch of revision %s failed!\n", rev);
		return (-1);
	}
	free(rev);
	rev = 0;
	sccs_free(s);

	f->m = mopen(f->tmpfile, "r");
	unless (f->m) {
		fprintf(stderr, "Open of %s failed!\n", f->tmpfile);
		exit(2);
	}

	end = f->m->end;

	f->n = nLines(seqlist);
	f->lines = calloc(f->n+1, sizeof(ld_t));
	EACH (seqlist) {
		f->lines[i-1].seq = p2int(seqlist[i]);
		seqlist[i] = 0;
	}
	freeLines(seqlist, 0);
	seqlist = 0;

	l = 0;
	p = f->m->where;
	while (p) {
		char	*start;

		assert(l < f->n);
		if (anno) {
			f->lines[l].anno = p;
			while (p < end && *p++ != '|');
			++p;	/* skip space after | */
		} else {
			f->lines[l].anno = 0;
		}
		f->lines[l].line = p;
		start = p;
		while (p < end && *p++ != '\n');
		assert(p > start); /* no empty lines */
		f->lines[l].len = p - start;
		if (p == end) p = 0;
		++l;
	}
	f->lines[l].seq = ~0;
	f->lines[l].line = end;
	f->lines[l].anno = 0;
	f->lines[l].len = 0;
	assert(f->n == l);

	return (0);
}

private	void
file_free(file_t *f)
{
	if (f->lines) {
		free(f->lines);
		f->lines = 0;
	}
	if (f->m) {
		mclose(f->m);
		f->m = 0;
	}
	if (f->tmpfile) {
		unlink(f->tmpfile);
		free(f->tmpfile);
		f->tmpfile = 0;
	}
}

private char *
find_gca(char *file, char *left, char *right)
{
	sccs	*s;
	char	*sfile = name2sccs(file);
	delta	*dl, *dr, *dg;
	char	*inc = 0, *exc = 0;
	char	**revlist = 0;

	s = sccs_init(sfile, INIT_NOCKSUM);
	free(sfile);
	unless (s) {
		perror(file);
		exit(2);
	}
	dl = sccs_findrev(s, left);
	unless (dl) {
		fprintf(stderr, "ERROR: couldn't find %s in %s\n", left, file);
		sccs_free(s);
		exit(2);
	}
	dr = sccs_findrev(s, right);
	unless (dr) {
		fprintf(stderr, "ERROR: couldn't find %s in %s\n", right, file);
		sccs_free(s);
		exit(2);
	}
	dg = sccs_gca(s, dl, dr, &inc, &exc);
	revlist = str_append(0, dg->rev, 0);
	if (inc) {
		revlist = str_append(revlist, "+", 0);
		revlist = str_append(revlist, inc, 0);
		free(inc);
	}
	if (exc) {
		revlist = str_append(revlist, "-", 0);
		revlist = str_append(revlist, exc, 0);
		free(exc);
	}
	sccs_free(s);
	return (str_pullup(0, revlist));
}

/*
 * print a line from a ld_t structure and optional put the
 * first_char argument at the beginngin.
 * Handles the -s (show_seq) knob.
 */
private void
printline(ld_t *ld, char first_char, int forcenl)
{
	char	*a = ld->anno;
	char	*p = ld->line;
	char	*end = ld->line + ld->len;

	if (fdiff && !first_char) first_char = ' ';
	if (first_char) putchar(first_char);
#ifdef	SHOW_SEQ
	if (show_seq) printf("%6d\t", ld->seq);
#endif

	/* Print annotation before line, if present */
	if (a) while (a < p) putchar(*a++);
	/* Print line */
	while (p < end) putchar(*p++);
	if (forcenl && (end[-1] != '\n')) putchar('\n');
}

private void
printhighlight(int *highlight)
{
	int	s, e;

	putchar('h');
	while (1) {
		s = highlight[0];
		e = highlight[1];
		if (s == 0 && e == 0) break;
		printf(" %d-%d", s, e);
		highlight += 2;
	}
	putchar('\n');
}

struct conflct {
	int	start[3];	/* lines at start of conflict */
	int	end[3];		/* lines after end of conflict */
	int	start_seq;	/* seq of lines before 'start' */
	int	end_seq;	/* seq of lines at 'end' */
	ld_t	*merged;	/* result of an automerge */
	conflct	*prev, *next;
	/* XXX need list of automerge algros that touched this block */
};

/*
 * Print a series of lines on one side of a conflict
 */
private void
printlines(conflct *curr, int side, int forcenl)
{
	int	len = curr->end[side] - curr->start[side];
	int	i;
	int	idx = curr->start[side];

	for (i = 0; i < len; i++) {
		printline(&body[side].lines[idx++], 0, forcenl);
	}
}

private int
do_weave_merge(u32 start, u32 end)
{
	conflct	*clist;
	conflct	*curr;
	int	mk[3];
	int	i;
	int	len;
	int	ret = 0;

	/* create a linked list of all conflict regions */
	clist = find_conflicts();

	/* Merge any that *might* be incorrectly marked */
	merge_conflicts(clist);

	mk[0] = mk[1] = mk[2] = 0;
	curr = clist;
	while (curr) {
		conflct	*tmp;

		/* print lines up the the next conflict */
		len = curr->start[GCA] - mk[GCA];

		/* should be the same number of lines */
		for (i = 0; i < 3; i++) assert(len == curr->start[i] - mk[i]);

		for (i = 0; i < len; i++) {
			ld_t	*gcaline = &body[GCA].lines[mk[GCA] + i];

			/* All common lines should match exactly */
			assert(gcaline->seq ==
			    body[LEFT].lines[mk[LEFT]+i].seq &&
			    gcaline->seq ==
			    body[RIGHT].lines[mk[RIGHT]+i].seq);

			if (gcaline->seq <= start) continue;
			if (gcaline->seq >= end) {
				while (curr) {
					tmp = curr;
					curr = curr->next;
					free(tmp);
				}
				return (ret);
			}
			printline(gcaline, 0, 0);
		}
		if (curr->start_seq >= start) {
			/* handle conflict */
			if (resolve_conflict(curr)) ret = 1;
		}
		for (i = 0; i < 3; i++) mk[i] = curr->end[i];

		tmp = curr;
		curr = curr->next;
		free(tmp);
	}
	/* Handle any lines after the last conflict */

	len = body[GCA].n - mk[GCA];
	for (i = 0; i < 3; i++) assert(len == body[i].n - mk[i]);

	for (i = 0; i < len; i++) {
		ld_t	*gcaline = &body[GCA].lines[mk[GCA] + i];

		assert(gcaline->seq ==
		    body[LEFT].lines[mk[LEFT]+i].seq &&
		    gcaline->seq ==
		    body[RIGHT].lines[mk[RIGHT]+i].seq);
		if (gcaline->seq <= start) continue;
		if (gcaline->seq >= end) return (ret);
		printline(gcaline, 0, 0);
	}
	return (ret);
}

/*
 * Find all conflict sections in the file based only of sequence numbers.
 * Return a linked list of conflict regions.
 */
private conflct *
find_conflicts(void)
{
	int	i, j;
	int	mk[3];
	ld_t	*lines[3];
	conflct	*list = 0;
	conflct	*end = 0;
	conflct	*p;

	for (i = 0; i < 3; i++) mk[i] = 0;

	while (1) {
		int	minlen;
		int	at_end;
		u32	seq[3];

		/* Assume everything matches here */
		minlen = INT_MAX;
		for (i = 0; i < 3; i++) {
			lines[i] = &body[i].lines[mk[i]];
			if (minlen > body[i].n - mk[i]) {
				minlen = body[i].n - mk[i];
			}
		}
		j = 0;
		while (j < minlen &&
		    lines[0][j].seq == lines[1][j].seq &&
		    lines[0][j].seq == lines[2][j].seq) {
			j++;
		}
		/* at start of conflict or end of file */
		at_end = 1;
		for (i = 0; i < 3; i++) {
			mk[i] += j;
			if (mk[i] < body[i].n) at_end = 0;
		}
		if (at_end) break;

		p = new(conflct);

		/* link into end of chain */
		if (list) {
			end->next = p;
			p->prev = end;
		} else {
			list = p;
		}
		end = p;

		if (mk[0] > 0) {
			p->start_seq = body[0].lines[mk[0]-1].seq;
		} else {
			p->start_seq = 0;
		}
		for (i = 0; i < 3; i++) {
			p->start[i] = mk[i];
			lines[i] = body[i].lines;
		}

		/* find end */
		for (i = 0; i < 3; i++) {
			seq[i] = lines[i][mk[i]].seq;
		}
		do {
			u32	maxseq = 0;
			at_end = 1;
			for (i = 0; i < 3; i++) {
				if (seq[i] > maxseq) maxseq = seq[i];
			}
			for (i = 0; i < 3; i++) {
				while (seq[i] < maxseq) {
					++mk[i];
					seq[i] = lines[i][mk[i]].seq;
					at_end = 0;
				}
				if (seq[i] > maxseq) maxseq = seq[i];
			}
		} while (!at_end);

		assert(seq[0] == seq[1] && seq[0] == seq[2]);
		for (i = 0; i < 3; i++) {
			p->end[i] = mk[i];
		}
		assert(seq[0]);
		p->end_seq = seq[0];
	}
	return (list);
}

/*
 * Return true if a line appears anywhere inside a conflict.
 */
private int
contains_line(conflct *c, ld_t *line)
{
	int	i, j;

	for (i = 0; i < 3; i++) {
		for (j = c->start[i]; j < c->end[i]; j++) {
			if (sameline(&body[i].lines[j], line)) return (1);
		}
	}
	return (0);
}

/*
 * Walk list of conflicts and merge any that are only
 * seperated by one line that exists in one of the conflict
 * regions.
 * XXX should handle N
 */
private void
merge_conflicts(conflct *head)
{
	while (head) {
		if (head->next && head->next->start[0] - head->end[0] == 1) {
			ld_t	*line = &body[0].lines[head->end[0]];

			if (contains_line(head, line) ||
			    contains_line(head->next, line)) {
				conflct	*tmp;
				int	i;

				/* merge the two regions */
				tmp = head->next;
				for (i = 0; i < 3; i++) {
					head->end[i] = tmp->end[i];
				}
				assert(tmp->end_seq);
				head->end_seq = tmp->end_seq;
				head->next = tmp->next;
				if (tmp->next) tmp->next->prev = head;
				free(tmp);
				/*
				 * don't update head so we try this
				 * conflict again.
				 */
			} else {
				head = head->next;
			}
		} else {
			head = head->next;
		}
	}
}

/*
 * The structure for a class that creates and then
 * walks the diffs of two files and returns the
 * lines for the right file as requested.  All struct members
 * should be considered private and all operations are done
 * with the member functions below.
 */
typedef struct difwalk difwalk;
struct difwalk {
	FILE	*diff;
	int	offset;		/* difference between gca lineno and right */
	int	start, end;
	int	lines;
	struct {
		int	gcastart, gcaend;
		int	lines;
	} cmd;
};

/*
 * Read a line from the diff and return it.  The arguments are like fgets().
 * We need to just skip lines about missing newlines, we don't actually
 * read the lines so we don't need to know about the missing newline.
 * We are just looking for line counts.
 */
private char *
readdiff(char *buf, int size, FILE *f)
{
	char	*ret;
	int	c;

	do {
		if ((ret = fgets(buf, size, f)) && !strchr(buf, '\n')) {
			/* skip to next newline */
			while (((c = getc(f)) != EOF) && (c != '\n'));
		}
	} while (ret && strneq(buf, "\\ No newline", 12));
	return (ret);
}

/*
 * Internal function.
 * Read next command from diff stream
 */
private void
diffwalk_readcmd(difwalk *dw)
{
	char	*p;
	int	start, end;
	int	cnt;
	char	cmd;
	char	buf[64];

	unless (readdiff(buf, sizeof(buf), dw->diff)) {
		dw->cmd.gcastart = dw->cmd.gcaend = INT_MAX;
		return;
	}

	dw->cmd.gcastart = strtol(buf, &p, 10);
	if (*p == ',') {
		dw->cmd.gcaend = strtol(p+1, &p, 10) + 1;
	} else {
		dw->cmd.gcaend = dw->cmd.gcastart;
	}
	cmd = *p++;
	start = strtol(p, &p, 10);
	if (*p == ',') {
		end = strtol(p+1, &p, 10);
	} else {
		end = start;
	}
	dw->cmd.lines = (cmd == 'd') ? 0 : (end - start + 1);

	if (cmd != 'a') {
		char	buf[MAXLINE];
		int	cnt;

		if (dw->cmd.gcaend == dw->cmd.gcastart) {
			++dw->cmd.gcaend;
		}
		cnt = dw->cmd.gcaend - dw->cmd.gcastart;
		while (cnt--) {
			readdiff(buf, sizeof(buf), dw->diff);
			assert(buf[0] == '<');
			assert(buf[1] == ' ');
		}
		if (cmd == 'c') {
			readdiff(buf, sizeof(buf), dw->diff);
			assert(buf[0] == '-');
		}
	} else {
		assert(dw->cmd.gcaend == dw->cmd.gcastart);
		dw->cmd.gcaend = ++dw->cmd.gcastart;
	}
	cnt = dw->cmd.lines;
	while (cnt--) {
		readdiff(buf, sizeof(buf), dw->diff);
		assert(buf[0] == '>');
		assert(buf[1] == ' ');
	}
}

/*
 * create a new diffwalk struct from two filenames
 */
private difwalk *
diffwalk_new(file_t *left, file_t *right)
{
	difwalk	*dw;
	char	*cmd;

	dw = new(difwalk);
	cmd = aprintf("bk diff %s '%s' '%s'",
	    (anno ? "--ignore-to-str='\\| '" : ""),
	    left->tmpfile, right->tmpfile);
	dw->diff = popen(cmd, "r");
	free(cmd);

	diffwalk_readcmd(dw);
	return (dw);
}

/*
 * return the gca linenumber of the start of the next diff in the
 * file.
 */
private int
diffwalk_nextdiff(difwalk *dw)
{
	return (dw->cmd.gcastart);
}

/*
 * Extend the current diff region to at least contain the given
 * GCA line number.
 */
private void
diffwalk_extend(difwalk *dw, int gcalineno)
{
	if (gcalineno < dw->end) return;

	while (gcalineno > dw->end && dw->end < dw->cmd.gcastart) {
		++dw->end;
	}
	if (gcalineno >= dw->cmd.gcastart) {
		int	extra;

		extra = dw->cmd.lines - (dw->cmd.gcaend - dw->cmd.gcastart);
		dw->lines += extra;
		dw->offset += extra;
		dw->end = dw->cmd.gcaend;
		diffwalk_readcmd(dw);
		diffwalk_extend(dw, gcalineno);
	}
}

/*
 * Start a new diff at the given line number in the GCA file.
 */
private void
diffwalk_start(difwalk *dw, int gcalineno)
{
	assert(gcalineno <= dw->cmd.gcastart);
	dw->start = dw->end = gcalineno;
	dw->lines = 0;
	if (gcalineno == dw->cmd.gcastart) diffwalk_extend(dw, dw->cmd.gcaend);
}

/*
 * Return the ending GCA line number for the current diff region
 */
private int
diffwalk_end(difwalk *dw)
{
	return (dw->end);
}

private void
diffwalk_range(difwalk *dw, int side, conflct *conf)
{
	conf->start[side] = dw->start + dw->offset - 1 - dw->lines;
	conf->end[side] = dw->end + dw->offset - 1;
}

/*
 * free a diffwalk struct
 */
private void
diffwalk_free(difwalk *dw)
{
	pclose(dw->diff);
	free(dw);
}

private int
do_diff_merge(void)
{
	difwalk	*ldiff;
	difwalk	*rdiff;
	int	gcaline;
	int	ret = 0;
	conflct	*c;
	conflct	conf;

	ldiff = diffwalk_new(&body[GCA], &body[LEFT]);
	rdiff = diffwalk_new(&body[GCA], &body[RIGHT]);

	gcaline = 1;
	while (1) {
		int	start, end;
		int	tmp;

		start = diffwalk_nextdiff(ldiff);
		tmp = diffwalk_nextdiff(rdiff);
		if (tmp < start) start = tmp;

		while (gcaline < start) {
			if (gcaline > body[GCA].n) break;
			printline(&body[GCA].lines[gcaline-1], 0, 0);
			++gcaline;
		}
		if (gcaline < start) break;
		diffwalk_start(ldiff, start);
		diffwalk_start(rdiff, start);
		while (1) {
			int	lend, rend;

			lend = diffwalk_end(ldiff);
			rend = diffwalk_end(rdiff);

			if (lend < rend) {
				diffwalk_extend(ldiff, rend);
			} else if (rend < lend) {
				diffwalk_extend(rdiff, lend);
			} else {
				/* They agree */
				end = lend;
				break;
			}
		}
		memset(&conf, 0, sizeof(conf));
		conf.start[GCA] = start - 1;
		conf.end[GCA] = end - 1;
#if SHOW_SEQ
		if (start - 1 > 0) {
			conf.start_seq = body[GCA].lines[start - 2].seq;
		}
		conf.end_seq = body[GCA].lines[end - 1].seq;
#endif

		diffwalk_range(ldiff, LEFT, &conf);
		diffwalk_range(rdiff, RIGHT, &conf);

		c = &conf;
		while (c) {
			if (resolve_conflict(c)) ret = 1;
			c = c->next;
		}
		gcaline = end;
	}

	diffwalk_free(ldiff);
	diffwalk_free(rdiff);
	return (ret);
}

/*
 * Define of set of autoresolve function for processing a
 * conflict. Each function takes conflict regression and looks at it to
 * determine if a merge is possible.  If a merge is possible then the
 * ->merged array is initialized with the lines in the merged output
 * and 1 is returned.  The conflict can be split into to adjacent
 * conflicts that exactly cover the original conflict region.  (use
 * split_conflict() to make this split) If a split occurs then a 1 is
 * returns.  If the function changes nothing then 0 is returned.
 *
 * The ->merged array is a malloced array of ld_t's that have line data. 
 * Most merge functions will just fill this with copies of ld_t's from
 * one of the two sides.  When a function is written that starts to
 * generate merged lines that didn't exist on either side (like
 * character merging) then the lines themselves will need to be
 * included in same memory block.  Also don't forget that annotations
 * need to be faked.  Perhaps this will be ugly enough to warrant
 * rewriting this interface...
 *
 * The numbers used below, once shipped, must always mean the same thing.
 * If you evolve this code, use new numbers.
 *
 * Currently all functions are enabled by default, we use -1 to mean that
 * it is enabled, but hasn't been override by the command line.  It gets
 * set to 1 if a function is explicitly enabled.
 */
struct mergefcns {
	char	*name;		/* "name" of function for commandline */
	int	enable;		/* is this function enabled */
	int	(*fcn)(conflct *r);
	char	*help;
} mergefcns[] = {
	{"1",	-1, merge_same_changes,
	"Merge identical changes made by both sides"},
	{"2",	-1, merge_only_one,
	"Merge when only one side changes"},
	{"3",	-1, merge_content,
	"Merge adjacent non-overlapping modifications on both sides"},
	{"4",	-1, merge_common_header,
	"Merge identical changes at the start of a conflict"},
	{"7",	-1, merge_added_oneside,
	"Split one side add from conflict"},
	{"5",	-1, merge_common_footer,
	"Merge identical changes at the end of a conflict"},
	{"6",	-1, merge_common_deletes,
	"Merge identical deletions made by both sides"},
};
#define	N_MERGEFCNS (sizeof(mergefcns)/sizeof(struct mergefcns))

/*
 * Called by the command line parses to enable/disable different functions.
 * This function takes a comma or space seperated list of function names
 * and enables/disables those functions.  "all" is an alias for all functions.
 */
private void
enable_mergefcns(char *list, int enable)
{
	int	i;
	int	len;

	while (*list) {
		list += strspn(list, ", ");
		len = strcspn(list, ", ");
		unless (len) break;

		if (len == 3 && strneq(list, "all", 3)) {
			for (i = 0; i < N_MERGEFCNS; i++) {
				mergefcns[i].enable = enable;
			}
		} else {
			for (i = 0; i < N_MERGEFCNS; i++) {
				if (strlen(mergefcns[i].name) == len &&
				    strneq(list, mergefcns[i].name, len)) {
					mergefcns[i].enable = enable;
					break;
				}
			}
			if (i == N_MERGEFCNS) {
				fprintf(stderr,
					"ERROR: unknown merge function '%*s'\n",
					len, list);
				exit(2);
			}
		}
		list += len;
	}
}

private void
mergefcns_help(void)
{
	int	i;

	fprintf(stderr,
"The following is a list of merge algorithms which may be enabled or disabled\n\
to change the way smerge will automerge.  Starred entries are on by default.\n");
	for (i = 0; i < N_MERGEFCNS; i++) {
		fprintf(stderr, "%2s%c %s\n",
		    mergefcns[i].name,
		    mergefcns[i].enable ? '*' : ' ',
		    mergefcns[i].help);
	}
}

/*
 * Resolve a conflict region.
 * Try automerging and then print the merge, or display a conflict.
 */
private int
resolve_conflict(conflct *curr)
{
	int	i;
	int	ret = 0;
	int	changed;

	/*
	 * Try every enabled automerge function on the current conflict
	 * until an automerge occurs or no more changes are possible.
	 */
	do {
		changed = 0;
		for (i = 0; i < N_MERGEFCNS; i++) {
			unless (mergefcns[i].enable) continue;
			changed |= mergefcns[i].fcn(curr);
			if (curr->merged) break;
		}
	} while (changed && !curr->merged);
	if (curr->merged) {
		ld_t	*p;
		/* This region was automerged */
		if (fdiff) {
			putchar('M');
#ifdef SHOW_SEQ
			printf(" %d", curr->start_seq);
#endif
			putchar('\n');
		}
		for (p = curr->merged; p->line; p++) {
			printline(p, 0, (fdiff != 0));
		}
		free(curr->merged);
		if (fdiff) user_conflict_fdiff(curr);
	} else {
		/* found a conflict that needs to be resolved
		 * by the user
		 */
		ret = 1;
		if (fdiff) {
			user_conflict_fdiff(curr);
		} else {
			user_conflict(curr);
		}
	}
	if (fdiff) {
		putchar('E');
#ifdef SHOW_SEQ
		assert(curr->end_seq);
		printf(" %d", curr->end_seq);
#endif
		putchar('\n');
	}
	return (ret);
}

/*
 * Compare two lines and return true if they are the same.
 * The function does not compare the sequence numbers.
 */
private int
sameline(ld_t *left, ld_t *right)
{
	if (left->len != right->len) return (0);
	return (memcmp(left->line, right->line, left->len) == 0);
}

/*
 * Return true if the lines on side 'l' of a conflict are the same as the
 * lines on side 'r'
 * Sequence numbers are ignored in the comparison.
 */
private int
samedata(conflct *c, int l, int r)
{
	int	i;
	int	len;
	int	sl, sr;

	len = c->end[l] - c->start[l];
	if (len != c->end[r] - c->start[r]) return (0);

	sl = c->start[l];
	sr = c->start[r];
	for (i = 0; i < len; i++) {
		unless (sameline(&body[l].lines[sl + i],
			    &body[r].lines[sr + i])) {
			return (0);
		}
	}
	return (1);
}

/*
 * Print a conflict region that must be resolved by the user.
 */
private void
user_conflict(conflct *curr)
{
	int	i;
	diffln	*diffs;
	int	sameleft;
	int	sameright;

	switch (mode) {
	    case MODE_GCA:
		sameleft = samedata(curr, GCA, LEFT);
		sameright = samedata(curr, GCA, RIGHT);
		unless (sameleft && !sameright) {
			printf("<<<<<<< local %s %s vs %s\n",
			    file, revs[GCA], revs[LEFT]);
			diffs = unidiff(curr, GCA, LEFT);
			for (i = 0; diffs[i].ld; i++) {
				printline(diffs[i].ld, diffs[i].c, 1);
			}
			free(diffs);
		}
		unless (sameright && !sameleft) {
			printf("<<<<<<< remote %s %s vs %s\n",
			    file, revs[GCA], revs[RIGHT]);
			diffs = unidiff(curr, GCA, RIGHT);
			for (i = 0; diffs[i].ld; i++) {
				printline(diffs[i].ld, diffs[i].c, 1);
			}
			free(diffs);
		}
		printf(">>>>>>>\n");
		break;
	    case MODE_2WAY:
/* #define MATCH_DIFF3 */
#ifdef MATCH_DIFF3
		printf("<<<<<<< L\n");
		printlines(curr, LEFT, 1);
		printf("=======\n");
		printlines(curr, RIGHT, 1);
		printf(">>>>>>> R\n");
#else
		printf("<<<<<<< local %s %s\n", file, revs[LEFT]);
		printlines(curr, LEFT, 1);
		printf("<<<<<<< remote %s %s\n", file, revs[RIGHT]);
		printlines(curr, RIGHT, 1);
		printf(">>>>>>>\n");
#endif
		break;
	    case MODE_3WAY:
		printf("<<<<<<< gca %s %s\n", file, revs[GCA]);
		printlines(curr, GCA, 1);
		printf("<<<<<<< local %s %s\n", file, revs[LEFT]);
		printlines(curr, LEFT, 1);
		printf("<<<<<<< remote %s %s\n", file, revs[RIGHT]);
		printlines(curr, RIGHT, 1);
		printf(">>>>>>>\n");
		break;
	    case MODE_NEWONLY:
		sameleft = samedata(curr, GCA, LEFT);
		sameright = samedata(curr, GCA, RIGHT);
		unless (sameleft && !sameright) {
			printf("<<<<<<< local %s %s vs %s\n",
			    file, revs[GCA], revs[LEFT]);
			diffs = unidiff(curr, GCA, LEFT);
			for (i = 0; diffs[i].ld; i++) {
				if (diffs[i].c != '-') {
					printline(diffs[i].ld, diffs[i].c, 1);
				}
			}
			free(diffs);
		}
		unless (sameright && !sameleft) {
			printf("<<<<<<< remote %s %s vs %s\n",
			    file, revs[GCA], revs[RIGHT]);
			diffs = unidiff(curr, GCA, RIGHT);
			for (i = 0; diffs[i].ld; i++) {
				if (diffs[i].c != '-') {
					printline(diffs[i].ld, diffs[i].c, 1);
				}
			}
			free(diffs);
		}
		printf(">>>>>>>\n");
		break;
	}
}

/*
 * Print a conflict in fdiff format
 */
private void
user_conflict_fdiff(conflct *c)
{
	int	i, j;
	diffln	*left = 0, *right = 0;
	diffln	*diffs;
	diffln	*rightbuf;
	diffln	*lp, *rp;
	ld_t	*p, *end;
	ld_t	blankline;

	switch (mode) {
	    case MODE_GCA:
		left = unidiff(c, GCA, LEFT);
		right = unidiff(c, GCA, RIGHT);
		break;
	    case MODE_2WAY:
		/* fake a diff output */
		left = calloc(c->end[LEFT] - c->start[LEFT] + 1,
		    sizeof(diffln));
		p = &body[LEFT].lines[c->start[LEFT]];
		end = &body[LEFT].lines[c->end[LEFT]];
		i = 0;
		while (p < end) {
			left[i].ld = p++;
			left[i].c = ' ';
			i++;
		}
		left[i].ld = 0;
		right = calloc(c->end[RIGHT] - c->start[RIGHT] + 1,
		    sizeof(diffln));
		p = &body[RIGHT].lines[c->start[RIGHT]];
		end = &body[RIGHT].lines[c->end[RIGHT]];
		i = 0;
		while (p < end) {
			right[i].ld = p++;
			right[i].c = ' ';
			i++;
		}
		right[i].ld = 0;
		break;
	    case MODE_NEWONLY:
		diffs = unidiff(c, GCA, LEFT);
		for (i = 0; diffs[i].ld; i++);
		left = calloc(i+1, sizeof(diffln));
		j = 0;
		for (i = 0; diffs[i].ld; i++) {
			if (diffs[i].c != '-') {
				left[j++] = diffs[i];
			}
		}
		left[j].ld = 0;
		free(diffs);
		diffs = unidiff(c, GCA, RIGHT);
		for (i = 0; diffs[i].ld; i++);
		right = calloc(i+1, sizeof(diffln));
		j = 0;
		for (i = 0; diffs[i].ld; i++) {
			if (diffs[i].c != '-') {
				right[j++] = diffs[i];
			}
		}
		right[j].ld = 0;
		free(diffs);
		break;
	    case MODE_3WAY:
		fprintf(stderr, "Can't use 3-way diff with fdiff output!\n");
		exit(2);
		break;
	}
	
	/*
	 * generate highlighting, don't forget to free below
	 */
	highlight_diff(left);
	highlight_diff(right);

	/*
	 * Need to allocate rightbuf to hold data that will be printed
	 * on the right.
	 * The length can never be longer that right + left + 1
	 */
	i = 0;
	for (diffs = left; diffs->ld; diffs++) i++;
	for (diffs = right; diffs->ld; diffs++) i++;
	rightbuf = calloc(i+1, sizeof(diffln));

	blankline.line = "\n";
	blankline.len = 1;
	blankline.seq = 0;
	blankline.anno = 0;

	putchar('L');
#ifdef SHOW_SEQ
	unless (c->merged) printf(" %d", c->start_seq);
#endif
	putchar('\n');
	lp = left;
	rp = right;
	i = 0;
	while (lp->ld || rp->ld) {
		if (!rp->ld || lp->ld && lp->ld->seq < rp->ld->seq) {
			/* line on left */
			printline(lp->ld, lp->c, 1);
			if (lp->highlight) printhighlight(lp->highlight);
			rightbuf[i].ld = &blankline;
			rightbuf[i].c = 's';
			rightbuf[i].highlight = 0;
			++lp;
		} else if (!lp->ld || rp->ld->seq < lp->ld->seq) {
			/* line on right */
			printline(&blankline, 's', 1);
			rightbuf[i].ld = rp->ld;
			rightbuf[i].c = rp->c;
			rightbuf[i].highlight = rp->highlight;
			++rp;
		} else {
			/* matching line */
			printline(lp->ld, lp->c, 1);
			if (lp->highlight) printhighlight(lp->highlight);
			rightbuf[i].ld = rp->ld;
			rightbuf[i].c = rp->c;
			rightbuf[i].highlight = rp->highlight;
			++lp;
			++rp;
		}
		++i;
	}

	fputs("R\n", stdout);
	for (j = 0; j < i; j++) {
		printline(rightbuf[j].ld, rightbuf[j].c, 1);
		if (rightbuf[j].highlight) {
			printhighlight(rightbuf[j].highlight);
		}
	}

	/* 
	 * free highlight,  C++ makes this much nicer
	 */
	for (i = 0; left[i].ld; i++) {
		if (left[i].highlight) free(left[i].highlight);
	}
	for (i = 0; right[i].ld; i++) {
		if (right[i].highlight) free(right[i].highlight);
	}

	free(left);
	free(right);
	free(rightbuf);
}

/*
 * do a diff of two sides of a conflict region and return
 * an array of diffln structures.  The diff is purely based on
 * sequence numbers and is very quick.
 * The returned array is allocated with malloc and needs to be freed by
 * the user and the end of the array is marked by a ld pointer set to null.
 * Typical usage:
 *
 *    diffs = unidiff(curr, GCA, LEFT);
 *    for (i = 0; diffs[i].ld; i++) {
 *	     printline(diffs[i].ld, diffs[i].c, 0);
 *    }
 *    free(diffs);
 */
private diffln *
unidiff(conflct *curr, int left, int right)
{
	ld_t	*llines, *rlines;
	ld_t	*el, *er;
	diffln	*out;
	diffln	*p;
	diffln	*del, *delp;
	diffln	*ins, *insp;

	llines = &body[left].lines[curr->start[left]];
	rlines = &body[right].lines[curr->start[right]];
	el = &body[left].lines[curr->end[left]];
	er = &body[right].lines[curr->end[right]];

	/* allocate the maximum space that might be needed. */
	p = out = calloc(el - llines + er - rlines + 1, sizeof(diffln));
	delp = del = calloc(el - llines, sizeof(diffln));
	insp = ins = calloc(er - rlines, sizeof(diffln));

	while ((llines < el) || (rlines < er)) {
		if ((rlines == er) ||
		    ((llines < el) && (llines->seq < rlines->seq))) {
			delp->ld = llines++;
			delp->c = '-';
			++delp;
		} else if ((llines == el) || (rlines->seq < llines->seq)) {
			insp->ld = rlines++;
			insp->c = '+';
			++insp;
		} else {
			assert(llines->seq == rlines->seq);
			if (delp > del) {
				int	cnt = delp - del;
				memcpy(p, del, cnt * sizeof(diffln));
				p += cnt;
				delp = del;
			}
			if (insp > ins) {
				int	cnt = insp - ins;
				memcpy(p, ins, cnt * sizeof(diffln));
				p += cnt;
				insp = ins;
			}
			p->ld = llines++;
			p->c = ' ';
			++p;
			++rlines;
		}

	}
	if (delp > del) {
		int	cnt = delp - del;
		memcpy(p, del, cnt * sizeof(diffln));
		p += cnt;
		delp = del;
	}
	free(del);
	if (insp > ins) {
		int	cnt = insp - ins;
		memcpy(p, ins, cnt * sizeof(diffln));
		p += cnt;
		insp = ins;
	}
	free(ins);
	p->ld = 0;
	p->c = 0;

	return (out);
}

/*
 * Look at a pair of matching lines and attempt to highlight them.
 */
private void
highlight_line(diffln *del, diffln *add)
{
	int	s, e;
	char	*dline = del->ld->line;
	int	dlen = del->ld->len;
	char	*aline = add->ld->line;
	int	alen = add->ld->len;
	int	len;

	s = 0;
	while (s < dlen && s < alen && dline[s] == aline[s]) s++;
	
	e = 0;
	while (e < dlen - s && e < alen - s && 
	    dline[dlen - e - 1] == aline[alen - e - 1]) e++;
	
	len = max(alen, dlen);

	if (s + e < len / 3) return; /* not enough matched */
	
	if (s + e < dlen) {
		del->highlight = calloc(4, sizeof(int));
		del->highlight[0] = s;
		del->highlight[1] = dlen - e;
	}
	if (s + e < alen) {
		add->highlight = calloc(4, sizeof(int));
		add->highlight[0] = s;
		add->highlight[1] = alen - e;
	}
}

/*
 * Walk replacements in a diff and find lines that are mostly similar.
 * For those lines generate chararacter highlighting information to
 * mark the characters that have changed.
 *
 * For any line that should be highlighted, the highlight field of
 * the diffln struct is set of an array of character pairs.  The array
 * ends at the pair 0,0.  (The array is allocated with malloc and it
 * is up to the user to free.
 */
private void
highlight_diff(diffln *diff)
{
	int	a, b;

	a = 0;
	while (1) {
		/* find start of deleted region */
		while (diff[a].ld && diff[a].c != '-') a++;
		unless (diff[a].ld) return;
		
		/* find replacements */
		b = a + 1;
		while (diff[b].ld && diff[b].c == '-') b++;

		while (diff[b].ld && diff[a].c == '-' && diff[b].c == '+') {
			/* found a replacement do highlighting */
			highlight_line(&diff[a], &diff[b]);
			a++;
			b++;
		}
		a = b;
	}
}

#ifdef SHOW_SEQ
/*
 * Parse a range string from the command line.
 * Valid formats:
 *	"44..67"	after 44 to before 67
 *	"44.."		after 44 to end of file
 *	"..67"		start of file to before 67
 */
private	int
parse_range(char *range, u32 *start, u32 *end)
{
	if (isdigit(range[0])) {
		*start = strtoul(range, &range, 10);
	}
	if (range[0] == 0 || range[0] != '.' || range[1] != '.') {
		return (-1);
	}
	range += 2;
	if (isdigit(range[0])) {
		*end = strtoul(range, 0, 10);
	}
	return(0);
}
#endif

/*--------------------------------------------------------------------
 * Automerge functions
 *--------------------------------------------------------------------
 */

/*
 * All the lines on both sides are identical.
 */
private int
merge_same_changes(conflct *c)
{
	int	i;

	if (samedata(c, LEFT, RIGHT)) {
		int	len = c->end[LEFT] - c->start[LEFT];
		c->merged = calloc(len + 1, sizeof(ld_t));
		for (i = 0; i < len; i++) {
			c->merged[i] = body[LEFT].lines[c->start[LEFT] + i];
		}
		c->merged[i].line = 0;
		return (1);
	}
	return (0);
}

/*
 * Only one side make changes
 */
private int
merge_only_one(conflct *c)
{
	int	i;

	if (samedata(c, GCA, LEFT)) {
		int	len = c->end[RIGHT] - c->start[RIGHT];
		c->merged = calloc(len + 1, sizeof(ld_t));
		for (i = 0; i < len; i++) {
			c->merged[i] = body[RIGHT].lines[c->start[RIGHT] + i];
		}
		c->merged[i].line = 0;
		return (1);
	}
	if (samedata(c, GCA, RIGHT)) {
		int	len = c->end[LEFT] - c->start[LEFT];
		c->merged = calloc(len + 1, sizeof(ld_t));
		for (i = 0; i < len; i++) {
			c->merged[i] = body[LEFT].lines[c->start[LEFT] + i];
		}
		c->merged[i].line = 0;
		return (1);
	}
	return (0);
}

/*
 * Look at a diff and return the list of sequence numbers that were
 * modified in that diff.
 * Return NULL if the diff doesn't match the 'modification' pattern.
 */
private u32 *
lines_modified(diffln *diff)
{
	u32	*out = 0;
	u32	*op;
	int	i;
	int	saw_deletes;
	int	saw_adds;
	diffln	*p;

	/* count number of deleted lines */
	i = 0;
	for (p = diff; p->ld; ++p) if (p->c == '-') ++i;
	out = calloc(i+1, sizeof(u32));

	/* same seq of deleted lines and require right pattern */
	op = out;
	saw_adds = saw_deletes = 0;
	for (p = diff; p->ld; ++p) {
		switch (p->c) {
		    case ' ':
			saw_adds = saw_deletes = 0;
			break;
		    case '-':
			if (saw_adds) goto bad;
			saw_deletes = 1;
			*op++ = p->ld->seq;
			break;
		    case '+':
			unless (saw_deletes) goto bad;
			saw_adds = 1;
			break;
		}
	}
	*op = 0;
	return (out);
 bad:
	free(out);
	return (0);
}

/*
 * Take a diff and a list of sequence numbers and determine
 * if those numbers are unmodified in the diff.
 */
private int
are_unmodified(diffln *diff, u32 *lines)
{
	int	lcnt;
	u32	s;

	lcnt = 0;
	s = lines[lcnt];
	while (diff->ld) {
		if (*lines < diff->ld->seq) return (0);
		if (*lines == diff->ld->seq) {
			if (diff->c != ' ') return (0);
			++lines;
			unless (*lines) return (1);
		}
		++diff;
	}
	return (0);
}

/*
 * Determine if both sides made modifications to non-overlaping sections.
 * A modification is a delete of 1 or more lines follow by an addition
 * of 0 or more lines.
 */
private int
merge_content(conflct *c)
{
	diffln	*left, *right;
	diffln	*lp, *rp;
	u32	*modified;
	int	i;
	int	ret = 0;
	int	ok;

	left = unidiff(c, GCA, LEFT);
	right = unidiff(c, GCA, RIGHT);

	modified = lines_modified(left);
	unless (modified) goto bad;
	ok = are_unmodified(right, modified);
	free(modified);
	unless (ok) goto bad;

	modified = lines_modified(right);
	unless (modified) goto bad;
	ok = are_unmodified(left, modified);
	free(modified);
	unless (ok) goto bad;

	/*
	 * Allocate room for merged lines.  Worst case we need room for all
	 * the left and right lines in the output, plus a null
	 */
	c->merged = calloc(
	    (c->end[LEFT] - c->start[LEFT] +
	     c->end[RIGHT] - c->start[RIGHT] + 1), sizeof(ld_t));

	/* we are good to go, do merge */
	i = 0;
	lp = left;
	rp = right;
	while (lp->ld || rp->ld) {
		if (!rp->ld || lp->ld && lp->ld->seq < rp->ld->seq) {
			/* line on left */
			if (lp->c != '+') {
				/* error? */
				fprintf(stderr,
"ERROR: merge_content expected add on left\n");
				exit(2);
			}
			c->merged[i++] = *lp->ld;
			++lp;
		} else if (!lp->ld || rp->ld->seq < lp->ld->seq) {
			/* line on right */
			if (rp->c != '+') {
				/* error? */
				fprintf(stderr,
"ERROR: merge_content expected add on right\n");
				exit(2);
			}
			c->merged[i++] = *rp->ld;
			++rp;
		} else {
			/* matching line */
			if (lp->c == ' ' && rp->c == ' ') {
				/* common line add to merge */
				c->merged[i++] = *lp->ld;
			} else if ((lp->c == '-' && rp->c == ' ') ||
			    (lp->c == ' ' && rp->c == '-')) {
				/* deleted line ignore */
			} else {
				fprintf(stderr,
"ERROR: merge_content can't see adds on matching lines!\n");
				exit(2);
			}
			++lp;
			++rp;
		}
	}
	c->merged[i].line = 0;
	ret = 1;
 bad:
	free(left);
	free(right);
	return (ret);
}

/*
 * Take a conflict region and a set of 3 indexes for the 3 sides
 * and split the conflict into two such that the second conflict starts
 * and those indexes.
 */
private void
split_conflict(conflct *c, int splitidx[3])
{
	conflct	*newc;
	int	i;
	u32	seq;

	seq = body[GCA].lines[c->start[GCA] + splitidx[GCA]].seq;
	assert(seq);

	/* create a new conflict region */
	newc = new(conflct);

	for (i = 0; i < 3; i++) {
		newc->end[i] = c->end[i];
		newc->start[i] = c->end[i] = c->start[i] + splitidx[i];
	}
	newc->end_seq = c->end_seq;

	c->end_seq = seq;
	newc->start_seq = seq;

	/* link it up */
	if (c->next) c->next->prev = newc;
	newc->next = c->next;
	c->next = newc;
	newc->prev = c;
}

/*
 * Split a conflict if the first N lines of the RIGHT and the LEFT have
 * identical data.
 */
private int
merge_common_header(conflct *c)
{
	ld_t	*start[3];
	int	len[3];
	int	minlen;
	int	splitidx[3];
	int	i, j;
	int	chkgca;

	for (i = 0; i < 3; i++) {
		start[i] = &body[i].lines[c->start[i]];
		len[i] = c->end[i] - c->start[i];
	}
	minlen = min(len[LEFT], len[RIGHT]);

	chkgca = 1;
	for (i = j = 0; i < minlen; i++) {
		unless (sameline(&start[LEFT][i], &start[RIGHT][i])) break;
		if (chkgca && (i < len[GCA]) &&
		    sameline(&start[LEFT][i], &start[GCA][i])) {
			j++;
		} else {
			chkgca = 0;
		}
	}
	/* i == number of matching lines at start */
	if (i == 0 || i == len[LEFT] && i == len[RIGHT]) return (0);

	splitidx[LEFT] = i;
	splitidx[RIGHT] = i;
	splitidx[GCA] = j;

	split_conflict(c, splitidx);

	return (1);
}

/*
 * Split a conflict if the last N lines of the RIGHT and the LEFT have
 * identical data.
 */
private int
merge_common_footer(conflct *c)
{
	ld_t	*start[3];
	int	len[3];
	int	minlen;
	int	splitidx[3];
	int	i, j;
	int	chkgca;

	for (i = 0; i < 3; i++) {
		start[i] = &body[i].lines[c->start[i]];
		len[i] = c->end[i] - c->start[i];
	}
	minlen = min(len[LEFT], len[RIGHT]);

	chkgca = 1;
	for (i = j = 0; i < minlen; i++) {
		unless (sameline(&start[LEFT][len[LEFT] - i - 1],
			    &start[RIGHT][len[RIGHT] - i - 1])) break;
		if (chkgca && (i < len[GCA]) &&
		    sameline(&start[LEFT][len[LEFT] - i - 1],
			&start[GCA][len[GCA] - i - 1])) {
			j++;
		} else {
			chkgca = 0;
		}
	}
	/* i == number of matching lines at end */
	/* j == number of GCA matching lines at end */
	if (i == 0 || i == len[LEFT] && i == len[RIGHT]) return (0);

	splitidx[LEFT] = len[LEFT] - i;
	splitidx[RIGHT] = len[RIGHT] - i;
	splitidx[GCA] = len[GCA] - j;

	split_conflict(c, splitidx);

	return (1);
}

/*
 * Split a conflict if the same lines are deleted at the end of the
 * conflict.
 */
private int
merge_common_deletes(conflct *c)
{
	diffln	*left, *right;
	diffln	*p;
	int	splitidx[3];
	int	i, j;
	int	ret = 0;
	int	cnt;

	left = unidiff(c, GCA, LEFT);
	right = unidiff(c, GCA, RIGHT);
	/*
	 * remove matching deletes at beginning
	 *
	 * XXX not implimented.  Why?
	 */

	/*
	 * move matching deletes at end to new region
	 */
	p = left;
	while (p->ld) p++;
	--p;
	cnt = 0;
	while (p >= left && p->c == '-') {
		++cnt;
		--p;
	}
	p = right;
	while (p->ld) p++;
	--p;
	j = 0;
	while (p >= left && p->c == '-') {
		++j;
		--p;
	}
	if (j < cnt) cnt = j;

	if (cnt > 0) {
		for (i = 0; i < 3; i++) {
			splitidx[i] = c->end[i] - c->start[i];
		}
		splitidx[GCA] -= cnt;
		split_conflict(c, splitidx);
		ret = 1;
	}

	free(left);
	free(right);
	return (ret);
}

/*
 * Split a conflict if one side adds new code between identical
 * seq of other side and GCA.
 * Only need seq numbers for this.
 */
private int
merge_added_oneside(conflct *c)
{
	ld_t	*start[3];
	int	maxlen;
	int	splitidx[3];
	int	i, side;
	u32	seq;

	/* Each need to have some lines for this case to make sense */
	for (i = 0; i < 3; i++) {
		unless (c->end[i] != c->start[i]) return (0);
	}

	for (i = 0; i < 3; i++) {
		start[i] = &body[i].lines[c->start[i]];
		splitidx[i] = 0;
	}

	seq = start[GCA][0].seq;
	if ((start[LEFT][0].seq == seq) && (start[RIGHT][0].seq < seq)) {
		side = RIGHT;
	} else if ((start[RIGHT][0].seq == seq) && (start[LEFT][0].seq < seq)) {
		side = LEFT;
	} else {
		return (0);
	}
	maxlen = c->end[side] - c->start[side];

	for (i = 0; i < maxlen; i++) {
		unless (start[side][i].seq < seq) break;
	}

	splitidx[side] = i;

	split_conflict(c, splitidx);

	return (1);
}
