#include "../system.h"
#include "../sccs.h"
#include "../bkd.h"

/*
 * Copyright (c) 2001 Larry McVoy & Andrew Chang       All rights reserved.
 */

void
bkd_reap(pid_t resync, int r_pipe, int w_pipe)
{
	close(w_pipe);
	close(r_pipe);
#ifndef WIN32
	if (resync > 0) {
		/*
		 * win32 does not support the WNOHANG options
		 * there s also no need to reap child process on win32
		 */
		int	i;

		/* give it a bit for the protocol to close */
		for (i = 0; i < 20; ++i) {
			if (waitpid(resync, 0, WNOHANG) == resync) return;
			usleep(10000);
		}
		kill(resync, SIGTERM);
		for (i = 0; i < 20; ++i) {
			if (waitpid(resync, 0, WNOHANG) == resync) return;
			usleep(10000);
		}
		kill(resync, SIGKILL);
		waitpid(resync, 0, 0);
	}
#endif
}

#ifndef WIN32
pid_t
bkd_tcp_connect(remote *r)
{
	int	i;
	char	*cgi = WEB_BKD_CGI;

	if (r->type == ADDR_HTTP) {
		if (r->path && strneq(r->path, "cgi-bin/", 8)) {
			cgi = r->path + 8;
		}
		http_connect(r, cgi);
	} else {
		i = tcp_connect(r->host, r->port);
		if (i < 0) {
			r->rfd = r->wfd = -1;
			if (i == -2) r->badhost = 1;
		} else {
			r->rfd = r->wfd = i;
		}
	}
	r->isSocket = 1;
	return ((pid_t)0);
}
#else

private void
get_http_proxy_cred(remote *r)
{
	char	buf[MAXLINE];

	if (getline2(r, buf, sizeof(buf)) <= 0) {
err:		fprintf(stderr, "cannot get proxy info block\n");
		return;
	}
	unless (streq(buf, "@PROXY INFO@")) goto err;
	if (getline2(r, buf, sizeof(buf)) <= 0) goto err;
	if (streq(buf, "@END@")) return; /* no proxy cred */
	if (r->cred) free(r->cred);
	r->cred = strdup(buf);
	if (getline2(r, buf, sizeof(buf)) <= 0) goto err;
	unless (streq(buf, "@END@")) goto err;
}

pid_t
tcp_pipe(remote *r)
{
	char	port[50], pipe_size[50];
	char	*av[9] = {"bk", "_socket2pipe"};
	int	i = 2;
	pid_t 	pid;

	sprintf(port, "%d", r->port);
	sprintf(pipe_size, "%d", BIG_PIPE);
	if (r->trace) av[i++] = "-d";
	if (r->type == ADDR_HTTP) av[i++] = "-h";
	av[i++] = "-p";
	av[i++] = pipe_size;
	av[i++] = r->host;
	av[i++] = port;
	av[i] = 0;
	pid = spawnvp_rwPipe(av, &(r->rfd), &(r->wfd), BIG_PIPE);
	if (pid == -1) return (pid);
	if (r->type == ADDR_HTTP) get_http_proxy_cred(r);
	return (pid);
}

pid_t
bkd_tcp_connect(remote *r)
{
	pid_t	p;

	p = tcp_pipe(r);
	if (p == ((pid_t) -1)) {
		fprintf(stderr, "cannot create socket_helper\n");
		return (-1);
	}
	r->isSocket = 0;
	return (p);
}
#endif






#ifndef WIN32
int
check_rsh(char *remsh)
{
	/*
	 * rsh is bundled with most Unix system
	 * so we skip the check
	 */
	return (0);
}
#else
int
check_rsh(char *remsh)
{
	char *t;

	if (!(t = prog2path(remsh)) || strstr(t, "system32/rsh")) {
		getMsg("missing_rsh", remsh, 0, '=', stderr);
		return (-1);
	}
	return (0);
}
#endif
