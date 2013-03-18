#include "bkd.h"
#include "logging.h"
#include "nested.h"

private int	compressed(int level, int lclone);

/*
 * Send the sfio file to stdout
 */
int
cmd_clone(int ac, char **av)
{
	int	c, rc = 1;
	int	attach = 0, detach = 0, gzip, delay = -1, lclone = 0;
	int	nlid = 0;
	char	*p, *rev = 0;
	int	quiet = 0;
	char	buf[MAXLINE];

	unless (isdir("BitKeeper/etc")) {
		out("ERROR-Not at package root\n");
		out("@END@\n");
		goto out;
	}
	gzip = bk_gzipLevel();
	while ((c = getopt(ac, av, "ADlNqr;w;z|", 0)) != -1) {
		switch (c) {
		    case 'A':
			attach = 1;
			break;
		    case 'D':
			detach = 1;
			break;
		    case 'l':
			lclone = 1;
			break;
		    case 'N':
			nlid = 1;
			break;
		    case 'w':
			delay = atoi(optarg);
			break;
		    case 'z':
			gzip = optarg ? atoi(optarg) : Z_BEST_SPEED;
			if (gzip < 0 || gzip > 9) gzip = Z_BEST_SPEED;
			break;
		    case 'q':
			quiet = 1;
			break;
		    case 'r':
			rev = optarg;
			break;
		    default: bk_badArg(c, av);
		}
	}
	trigger_setQuiet(quiet);
	/*
	 * This is where we would put in an exception for bk port.
	 */
	if (!nlid && proj_isComponent(0) && !detach) {
		out("ERROR-clone of a component is not allowed, use -s\n");
		goto out;
	}
	if (attach && proj_isComponent(0)) {
		out("ERROR-cannot attach a component\n");
		goto out;
	}
	if (detach && !proj_isComponent(0)) {
		out("ERROR-can detach only a component\n");
		goto out;
	}
	if (proj_isProduct(0)) {
		if (attach) {
			out("ERROR-cannot attach a product\n");
			goto out;
		}
	}
	if (bp_hasBAM() && !bk_hasFeature(FEAT_BAMv2)) {
		out("ERROR-please upgrade your BK to a BAMv2 aware version "
		    "(4.1.1 or later)\n");
		goto out;
	}
	if (hasLocalWork(GONE)) {
		out("ERROR-must commit local changes to ");
		out(GONE);
		out("\n");
		goto out;
	}
	if (hasLocalWork(ALIASES)) {
		out("ERROR-must commit local changes to ");
		out(ALIASES);
		out("\n");
		goto out;
	}

	/* moved down here because we're caching the sccs* */
	if (rev) {
		sccs	*s = sccs_csetInit(SILENT);
		ser_t	d;

		assert(s && HASGRAPH(s));
		if (rev) {
			d = sccs_findrev(s, rev);
			unless (d) {
				out("ERROR-rev ");
				out(rev);
				out(" doesn't exist\n");
				goto out;
			}
		}
		sccs_free(s);
	}

	safe_putenv("BK_CSETS=..%s", rev ? rev : "+");
	/* has to be here, we use the OK below as a marker. */
	if (rc = bp_updateServer(getenv("BK_CSETS"), 0, SILENT)) {
		printf("ERROR-unable to update BAM server %s (%s)\n",
		    bp_serverURL(buf),
		    (rc == 2) ? "can't get lock" : "unknown reason");
		goto out;
	}
	rc = 1;
	p = getenv("BK_REMOTE_PROTOCOL");
	if (p && streq(p, BKD_VERSION)) {
		out("@OK@\n");
	} else {
		out("ERROR-protocol version mismatch, want: ");
		out(BKD_VERSION);
		out(", got ");
		out(p ? p : "");
		out("\n");
		goto out;
	}
	if (trigger(av[0], "pre")) goto out;
	if (proj_isProduct(0)) printf("@PRODUCT@\n");
	printf("@SFIO@\n");
	rc = compressed(gzip, lclone);
	putenv(rc ? "BK_STATUS=FAILED" : "BK_STATUS=OK");
	rc = 1;
	if (trigger(av[0], "post")) goto out;

	rc = 0;
	/*
	 * XXX Hack alert: workaround for a ssh bug
	 * Give ssh sometime to drain the data
	 * We should not need this if ssh is working correctly 
	 */
out:	if (delay > 0) sleep(delay);

	putenv("BK_CSETS=");
	return (rc);
}

private int
compressed(int level, int lclone)
{
	int	status, fd;
	FILE	*fh;
	char	*sfiocmd;
	char	*larg = (lclone ? "-L" : "");
	char	*marg = (bk_hasFeature(FEAT_mSFIO) ? "-m2" : "");

	sfiocmd = aprintf("bk _sfiles_clone %s %s | bk sfio -oq %s %s",
	    larg, marg, larg, marg);
	fh = popen(sfiocmd, "r");
	free(sfiocmd);
	fd = fileno(fh);
	gzipAll2fh(fd, stdout, level, 0, 0, 0);
	fflush(stdout);
	status = pclose(fh);
	unless (WIFEXITED(status) && WEXITSTATUS(status) == 0) return (1);
	return (0);
}
