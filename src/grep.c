/* Copyright (c) 2000 L.W.McVoy */
#include "system.h"
#include "sccs.h"
WHATSTR("@(#)%K%");

int
grep_main(int ac, char **av)
{
	char	sccscat_opts[20], grep_opts[20];
	char	*s = sccscat_opts, *g = grep_opts;
	char	**sav = malloc(sizeof(char*) * (ac + 10));
	char	*gav[10];
	char	*grep = "grep";
	int	c, i, fd, none = 0, status;
	char	rev[200];
	char	range[200];
	pid_t	pid;
	extern	int get_main(int ac, char **av);
	extern	int sccscat_main(int ac, char **av);

	if (ac == 2 && streq("--help", av[1])) {	
usage:		system("bk help -s grep");
		return (1);
	}

	*s++ = '-';
	*g++ = '-';
	rev[0] = range[0] = 0;
	while ((c = getopt(ac, av, "ac;defimnNur;R|")) != -1) {
		switch (c) {
		    case 'a':					/* doc 2.0 */
			none = 1;
			break;
		    case 'd':					/* doc 2.0 */
		    case 'f':					/* undoc? 2.0 */
		    case 'm':					/* doc 2.0 */
		    case 'N':					/* doc 2.0 */
		    case 'u':					/* doc 2.0 */
			*s++ = c;
			break;
		    case 'n':					/* doc 2.0 */
			*s++ = 'f';	/* -f undoc in get, may change */
			break;
		    case 'e':					/* doc 2.0 */
		    	grep = "egrep";
			break;
		    case 'r':					/* doc 2.0 */
			sprintf(rev, "-r%s", optarg);
			break;
		    case 'c':					/* doc 2.0 */
			sprintf(range, "-c%s", optarg);
			break;
		    case 'R':					/* doc 2.0 */
			if (optarg && optarg[0]) {
				sprintf(range, "-r%s", optarg);
			} else {
				strcpy(range, "-r1.0..");
			}
			break;
		    case 'i':					/* doc 2.0 */
			*g++ = c;
			break;
		    default:
			system("bk help -s grep");
			exit(0);
		}
	}
	if (s[-1] == '-') s = 0; else *s = 0;
	if (g[-1] == '-') g = 0; else *g = 0;

	/*
	 * Set up the grep part of the command line and spawn it.
	 */
	gav[i=0] = grep;
	if (g) gav[++i] = grep_opts;
	unless (gav[++i] = av[optind++]) goto usage;
	gav[++i] = 0;
	if ((pid = spawnvp_wPipe(gav, &fd, BIG_PIPE)) == -1) {
		fprintf(stderr, "cannot spawn grep\n");
		exit(1);
	}

	if (rev[0] && range[0]) {
		fprintf(stderr, "bk grep: can not mix -r with -R\n");
		exit(1);
	}

	unless (range[0] || rev[0]) strcpy(rev, "-r+");
	if (rev[0]) {
		sav[i=0] = "get";
		sav[++i] = rev;
		sav[++i] = "-kqp";
	} else {
		sav[i=0] = "sccscat";
		sav[++i] = "-q";
		if (range[0]) sav[++i] = range;
	}
	if (s) {
		sav[++i] = sccscat_opts;
	} else {
		unless (none) sav[++i] = "-nnm";
	}
	while (sav[++i] = av[optind++]);

	/*
	 * arrange to have our stdout go to the grep process' stdin.
	 */
	close(1); dup(fd); close(fd);
	getoptReset();
	if (rev[0]) {
		i = get_main(i, sav);
	} else {
		i = sccscat_main(i, sav);
	}
	fflush(stdout);
	close(1);
	if (i) {
		kill(pid, SIGTERM);
		waitpid(pid, 0, 0);
		exit(100);
	}
	waitpid(pid, &status, 0);
	if (WIFEXITED(status)) exit(WEXITSTATUS(status));
	exit(101);
}
