/* Copyright (c) 2000 Andrew W. Chang */
#include "bkd.h"
#include "resolve.h"

private int send_check_msg(remote *r);
private int remoteCheck(remote *r);

/*
 * Check the remote tree
 */
int
rcheck_main(int ac, char **av)
{

#if 	0
	while ((c = getopt(ac, av, "")) != -1) {
		switch (c) {
		    default:
usage:			//system("bk help -s rcheck");
			return (1);
		}
	}
#endif
	optind = 1;
	if (av[optind]) {
		remote *r;

		r = remote_parse(av[optind], REMOTE_BKDURL);
		unless (r) {
			fprintf(stderr, "Cannot parse \"%s\"\n", av[optind]);
			return (1);
		}
		if (r->host) {
			int	ret = remoteCheck(r);
			remote_free(r);
			return (ret);
		}
		remote_free(r);
	}
	fprintf(stderr, "rcheck: bad argument\n");
	return (1);
}

private int
send_check_msg(remote *r)
{
	char	buf[MAXPATH];
	FILE	*f;
	int	rc;

	bktmp(buf, "check");
	f = fopen(buf, "w");
	assert(f);
	sendEnv(f, 0, r, SENDENV_NOREPO);
	if (r->path) add_cd_command(f, r);
	fprintf(f, "check\n");
	fclose(f);

	rc = send_file(r, buf, 0);
	unlink(buf);
	return (rc);
}

private int
remoteCheck(remote *r)
{
	char	buf[MAXPATH];
	int	rc = 0;

	if (bkd_connect(r, 0)) return (1);
	if (send_check_msg(r)) return (1);
	if (r->type == ADDR_HTTP) skip_http_hdr(r);

	getline2(r, buf, sizeof (buf));
	unless (streq("@CHECK INFO@", buf)) return (1); /* protocol error */

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
	disconnect(r, 1);
	wait_eof(r, 0);
	disconnect(r, 2);
	return (rc);
}
