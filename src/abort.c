/* Copyright (c) 2000 L.W.McVoy */
#include "bkd.h"
#include "resolve.h"

private	void	abort_patch(int leavepatch);
private int	send_abort_msg(remote *r);
private int	remoteAbort(remote *r);

/*
 * Abort a pull/resync by deleting the RESYNC dir and the patch file in
 * the PENDING dir.
 */
int
abort_main(int ac, char **av)
{
	int	c, force = 0, leavepatch = 0;
	char	buf[MAXPATH];

	while ((c = getopt(ac, av, "fp")) != -1) {
		switch (c) {
		    case 'f': force = 1; break; 	/* doc 2.0 */
		    case 'p': leavepatch = 1; break; 	/* undoc? 2.0 */
		    default:
			system("bk help -s abort");
			return (1);
		}
	}
	if (av[optind]) {
		remote *r;

		r = remote_parse(av[optind], REMOTE_BKDURL);
		unless (r) {
			fprintf(stderr, "Cannot parse \"%s\"\n", av[optind]);
			return (1);
		}
		if (r->host) {
			int	ret = remoteAbort(r);
			remote_free(r);
			return (ret);
		}
		remote_free(r);
		chdir(av[optind]);
	}
	proj_cd2root();
	unless (exists(ROOT2RESYNC)) {
		fprintf(stderr, "No RESYNC dir, nothing to abort.\n");
		exit(0);
	}
	unless (force) {
		prompt("Abort update? (y/n)", buf);
		switch (buf[0]) {
		    case 'y':
		    case 'Y':
			break;
		    default:
			fprintf(stderr, "Not aborting.\n");
			exit(0);
		}
	}
	abort_patch(leavepatch);
	exit(0);
}

void
abort_patch(int leavepatch)
{
	char	buf[MAXPATH];
	char	pendingFile[MAXPATH];
	FILE	*f;

	unless (exists(ROOT2RESYNC)) chdir(RESYNC2ROOT);
	unless (exists(ROOT2RESYNC)) {
		fprintf(stderr, "abort: can't find RESYNC dir\n");
		fprintf(stderr, "abort: nothing removed.\n");
		exit(1);
	}

	/*
	 * Get the patch file name from RESYNC before deleting RESYNC.
	 */
	sprintf(buf, "%s/%s", ROOT2RESYNC, "BitKeeper/tmp/patch");

	/* One of our regressions makes an empty BitKeeper/tmp/patch */
	pendingFile[0] = 0;
	if (f = fopen(buf, "r")) {
		if (fnext(pendingFile, f)) chop(pendingFile);
		fclose(f);
	} else {
		fprintf(stderr, "Warning: no BitKeeper/tmp/patch\n");
	}

	assert(exists("RESYNC"));
	rmtree("RESYNC");
	if (!leavepatch && pendingFile[0]) unlink(pendingFile);
	rmdir(ROOT2PENDING);
	unlink(BACKUP_LIST);
	unlink(PASS4_TODO);
	unlink(APPLIED);
	repository_wrunlock(0, 1);
	repository_lockers(0);
	exit(0);
}

private int
send_abort_msg(remote *r)
{
	char	buf[MAXPATH];
	FILE	*f;
	int	rc;

	bktmp(buf, "abort");
	f = fopen(buf, "w");
	assert(f);
	sendEnv(f, 0, r, SENDENV_NOREPO);
	add_cd_command(f, r);
	fprintf(f, "abort\n");
	fclose(f);

	rc = send_file(r, buf, 0);
	unlink(buf);
	return (rc);
}

private int
remoteAbort(remote *r)
{
	char	buf[MAXPATH];
	int	rc = 0;

	if (bkd_connect(r)) return (1);
	if (send_abort_msg(r)) return (1);
	if (r->type == ADDR_HTTP) skip_http_hdr(r);

	
	getline2(r, buf, sizeof (buf));
	if (streq("@SERVER INFO@", buf)) {
		if (getServerInfo(r)) {
			rc = 1;
			goto out;
		}
		getline2(r, buf, sizeof (buf));
	}

	unless (streq("@ABORT INFO@", buf)) return (1); /* protocol error */

	while (getline2(r, buf, sizeof (buf)) > 0) {
		if (buf[0] == BKD_NUL) break;
		printf("%s\n", buf);
	}
	getline2(r, buf, sizeof (buf));
	if (buf[0] == BKD_RC) {
		rc = atoi(&buf[1]);
		getline2(r, buf, sizeof (buf));
	}
	unless (streq("@END@", buf)) return(1); /* protocol error */
out:	disconnect(r, 1);
	wait_eof(r, 0);
	disconnect(r, 2);
	return (rc);
}
