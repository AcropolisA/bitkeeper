/*
 * Copyright (c) 2001 Larry McVoy & Andrew Chang       All rights reserved.
 */
#include "../bkd.h"
#define Respond(s)	write(licenseServer[1], s, 4)

extern	time_t	requestEnd;

#ifndef WIN32
#include <grp.h>

void
ids(void)
{
	struct	passwd *pw;
	struct	group *gp;
	gid_t	g;
	uid_t	u;

	unless (Opts.gid && *Opts.gid) goto uid;

	if (isdigit(*Opts.gid)) {
		g = (gid_t)atoi(Opts.gid);
	} else {
		while (gp = getgrent()) {
			if (streq(Opts.gid, gp->gr_name)) break;
		}
		unless (gp) {
			fprintf(stderr,
			    "Unable to find group '%s', abort\n", Opts.gid);
			exit(1);
		}
		g = gp->gr_gid;
	}
	if (setgid(g)) perror("setgid");

uid:	unless (Opts.uid && *Opts.uid) return;

	if (isdigit(*Opts.uid)) {
		u = (uid_t)atoi(Opts.uid);
	} else {
		unless (pw = getpwnam(Opts.uid)) {
			fprintf(stderr,
			    "Unable to find user '%s', abort\n", Opts.uid);
			exit(1);
		}
		u = pw->pw_uid;
	}
	if (setuid(u)) perror("setuid");
	pw = getpwuid(getuid());
	safe_putenv("USER=%s", pw->pw_name);
}
#else
void
ids() {} /* no-op */
#endif

#ifndef WIN32
void
reap(int sig)
{
	while (waitpid((pid_t)-1, 0, WNOHANG) > 0);
	signal(SIGCHLD, reap);
}
#else
/*
 * There is no need to reap process on NT
 */
void reap(int sig) {} /* no-op */
#endif

#ifndef WIN32
void
requestWebLicense()
{

#define LICENSE_HOST	"licenses.bitkeeper.com"
#define	LICENSE_PORT	80
#define LICENSE_REQUEST	"/cgi-bin/bkweb-license.cgi"

	int f;
	char buf[MAXPATH];
	extern char *url(char*);

	time(&requestEnd);
	requestEnd += 60;

	if (fork() == 0) {
		if ((f = tcp_connect(LICENSE_HOST, LICENSE_PORT)) != -1) {
			sprintf(buf, "GET %s?license=%s:%u\n\n",
			    LICENSE_REQUEST,
			    sccs_gethost(), 
			    Opts.port ? Opts.port : BK_PORT);
			write(f, buf, strlen(buf));
			read(f, buf, sizeof buf);
			close(f);
		}

		exit(0);
	}
}
#endif

#ifndef WIN32
void
bkd_server(int ac, char **av)
{
	fd_set	fds;
	int	sock;
	int	maxfd;
	time_t	now;
	extern	int licenseServer[2];	/* bkweb license pipe */ 
	extern	time_t licenseEnd;	/* when a temp bk license expires */

	sock = tcp_server(Opts.port ? Opts.port : BK_PORT, Opts.quiet);
	ids();
	if (sock < 0) exit(-sock);
	unless (Opts.debug) if (fork()) exit(0);
	unless (Opts.debug) setsid();	/* lose the controlling tty */
	if (Opts.pidfile) {
		FILE	*f = fopen(Opts.pidfile, "w");

		if (f) {
			fprintf(f, "%u\n", getpid());
			fclose(f);
		}
	}
	if (Opts.logfile) Opts.log = fopen(Opts.logfile, "a");
	signal(SIGCHLD, reap);
	signal(SIGPIPE, SIG_IGN);
	if (Opts.alarm) {
		signal(SIGALRM, exit);
		alarm(Opts.alarm);
	}
	maxfd = (sock > licenseServer[1]) ? sock : licenseServer[1];

	for (;;) {
		int n;
		struct timeval delay;

		FD_ZERO(&fds);
		FD_SET(licenseServer[1], &fds);
		FD_SET(sock, &fds);
		delay.tv_sec = 60;
		delay.tv_usec = 0;

		unless (select(maxfd+1, &fds, 0, 0, &delay) > 0) continue;

		if (FD_ISSET(licenseServer[1], &fds)) {
			char req[5];

			if (read(licenseServer[1], req, 4) == 4) {
				if (strneq(req, "MMI?", 4)) {
					/* get current license (YES/NO) */
					time(&now);

					if (now < licenseEnd) {
						Respond("YES\0");
					} else {
						if (requestEnd < now)
							requestWebLicense();
						Respond("NO\0\0");
					}
				} else if (req[0] == 'S') {
					/* set license: usage is Sddd */
					req[4] = 0;
					licenseEnd = now + (60*atoi(1+req));
				}
			}
		}

		unless (FD_ISSET(sock, &fds)) continue;

		if ( (n = tcp_accept(sock)) == -1) continue;

		if (fork()) {
		    	close(n);
			/* reap 'em if you got 'em */
			reap(0);
			if ((Opts.count > 0) && (--(Opts.count) == 0)) break;
			continue;
		}

		if (Opts.log) {
			char	*name;

			strcpy(Opts.remote, (name = peeraddr(n)) ? name : "unknown");
		}
		/*
		 * Make sure all the I/O goes to/from the socket
		 */
		close(0); dup(n);
		close(1); dup(n);
		close(n);
		close(sock);
		signal(SIGCHLD, SIG_DFL);	/* restore signals */
		do_cmds();
		exit(0);
	}
}

#else

#define APPNAME            	"BitKeeper"
#define SERVICENAME        	"BitKeeperService"
#define SERVICEDISPLAYNAME 	"BitKeeper Service"
#define DEPENDENCIES       	""

static SERVICE_STATUS		srvStatus;
static SERVICE_STATUS_HANDLE	statusHandle;
static HANDLE			hServerStopEvent = NULL;
static char			err[256];
int				bkd_quit = 0; /* global */

static void WINAPI bkd_service_ctrl(DWORD dwCtrlCode);
static char *getError(char *buf, int len);
void reportStatus(SERVICE_STATUS_HANDLE, int, int, int);
void bkd_remove_service(int verbose);
void bkd_install_service(bkdopts *opts, int ac, char **av);
void bkd_start_service(void (*service_func)(int, char**));
void logMsg(char *msg);

private void
argv_save(int ac, char **av, char **nav, int j)
{
	int	c;

	/*
	 * Parse the av[] to decide which one we should pass down stream
	 * Note: the option string below must match the one in bkd_main().
	 */
	getoptReset();
	while ((c =
	    getopt(ac, av, "c:CdDeE:g:hi:l|L:p:P:qRs:St:u:V:x:z")) != -1) {
		switch (c) {
		    case 'C':	nav[j++] = strdup("-C"); break;
		    case 'D':	nav[j++] = strdup("-D"); break;
		    case 'i':
			nav[j++] = strdup("-i");
			nav[j++] = strdup(optarg);
			break;
		    case 'h':	nav[j++] = strdup("-h"); break;
		    case 'l':
			nav[j++] = aprintf( "-l%s", optarg ? optarg : "");
			break;
		    case 'V':
			nav[j++] = strdup("-V");
			nav[j++] = strdup(optarg);
			break;
		    case 'P':
			nav[j++] = strdup("-P");
			nav[j++] = strdup(optarg);
			break;
		    case 'x':
			nav[j++] = strdup("-x");
			nav[j++] = strdup(optarg);
			break;
		    case 'L':
			nav[j++] = strdup("-L");
			nav[j++] = strdup(optarg);
			break;
		    case 'q': nav[j++] = strdup("-q"); break;

		    /* no default, any extras should be caught in bkd.c */
	    	}
	}
	nav[j] = 0;
}

private int
argv_size(char **nav)
{
	int 	j = 0, len = 0;

 	/* allow for space and quoting */
	while (nav[j]) len += (strlen(nav[j++]) + 3);
	return (len);
}

private void
argv_free(char **nav, int j)
{
	unless (nav) return;
	while (nav[j]) free(nav[j++]);
}

void
bkd_service_loop(int ac, char **av)
{
	SOCKET	sock = 0;
	int	n;
	char	pipe_size[50], socket_handle[20];
	char	*nav[100] = {
		"bk", "_socket2pipe",
		"-s", socket_handle,	/* socket handle */
		"-p", pipe_size,	/* set pipe size */
		"bk", "bkd", "-z",	/* bkd command */
		0};
	extern	int bkd_quit; /* This is set by the helper thread */
	extern	int bkd_register_ctrl(void);
	extern	void reportStatus(SERVICE_STATUS_HANDLE, int, int, int);
	extern	void logMsg(char *);
	SERVICE_STATUS_HANDLE   sHandle;
	
	/*
	 * Register our control interface with the service manager
	 */
	if ((sHandle = bkd_register_ctrl()) == 0) goto done;

	/*
	 * Get a socket
	 */
	sock = (SOCKET) tcp_server(Opts.port ? Opts.port : BK_PORT, 1);
	if (sock < 0) goto done;

	if (Opts.startDir) {
		if (chdir(Opts.startDir) != 0) {
			char msg[MAXLINE];

			sprintf(msg, "bkd: cannot cd to \"%s\"",
								Opts.startDir);
			logMsg(msg);
			goto done;
		}
	}
	if (Opts.logfile) Opts.log = fopen(Opts.logfile, "a");

	/*
	 * Main loop
	 */
	sprintf(pipe_size, "%d", BIG_PIPE);
	argv_save(ac, av, nav, 9);
	reportStatus(sHandle, SERVICE_RUNNING, NO_ERROR, 0);
	for (;;) {
		n = accept(sock, 0 , 0);
		/*
		 * We could be interrupted if the service manager
		 * want to shut us down.
		 */
		if (bkd_quit == 1) break; 
		if (n == INVALID_SOCKET) {
			logMsg("bkd: got invalid socket, re-trying...");
			continue; /* re-try */
		}
		/*
		 * On win32, we cannot dup a socket,
		 * so just pass the socket handle as a argument
		 */
		sprintf(socket_handle, "%d", n);
		if (Opts.log) {
			char	*name = peeraddr(n);

			strcpy(Opts.remote, name ? name : "unknown");
		}
		/*
		 * Spawn a socket helper which will spawn a new bkd process
		 * to service this connection. The new bkd process is connected
		 * to the socket helper via pipes. Socket helper forward
		 * all data between the pipes and the socket.
		 */
		if (spawnvp_ex(_P_NOWAIT, nav[0], nav) == -1) {
			logMsg("bkd: cannot spawn socket_helper");
			break;
		}
		CloseHandle((HANDLE) n); /* important for EOF */
        	if ((Opts.count > 0) && (--(Opts.count) == 0)) break;
		if (bkd_quit == 1) break;
	}

done:	if (sock) CloseHandle((HANDLE)sock);
	if (sHandle) reportStatus(sHandle, SERVICE_STOPPED, NO_ERROR, 0);
	argv_free(nav, 9);
	_exit(0); /* We don't want to process atexit() in this */
		  /* env. otherwise XP will flag an error      */
}


/*
 * There are two major differences between the Unix/Win32
 * bkd_server implementation:
 * 1) Unix bkd is a regular daemon, win32 bkd is a NT service
 *    (NT services has a more complex interface, think 10 X)
 * 2) Win32 bkd uses a socket_helper process to convert a pipe interface
 *    to socket intertface, because the main code always uses read()/write()
 *    instead of send()/recv(). On win32, read()/write() does not
 *    work on socket.
 */
void
bkd_server(int ac, char **av)
{
	extern void bkd_service_loop(int, char **);

	if (Opts.start) { 
		bkd_start_service(bkd_service_loop);
		exit(0);
	} else if (Opts.remove) { 
		bkd_remove_service(1);
		exit(0);
	} else {
		/* install and start bkd service */
		bkd_install_service(&Opts, ac, av);
	}
}

static int
envSize(char *envVar)
{
	char *e;

	unless (e = getenv(envVar)) return (0);
	return (strlen(envVar) + 1 + strlen(e));
}

/* e.g. append "-E \"BK_BKDIR=path\"" */
static void
addEnvVar(char *cmd, char *envVar)
{
	char	*p, *v;

	unless (v = getenv(envVar)) return;
	p = &cmd[strlen(cmd)];
	sprintf(p, "-E \"%s=%s\"", envVar, v);
}

/*
 * Install and start bkd service
 */
void
bkd_install_service(bkdopts *opts, int ac, char **av)
{
	SC_HANDLE   schService = 0;
	SC_HANDLE   schSCManager = 0;
	SERVICE_STATUS serviceStatus;
	char	path[1024], here[1024];
	char	*start_dir, *cmd, *p;
	char	**nav;
	char	*eVars[3] = {"BK_REGRESION", "BK_BKDIR", 0};
	int	i, len, try = 0;

	if (GetModuleFileName(NULL, path, sizeof(path)) == 0) {
		fprintf(stderr, "Unable to install %s - %s\n",
		    SERVICEDISPLAYNAME, getError(err, 256));
		return;
	}

	if (opts->startDir) {
		start_dir = opts->startDir;
	}  else {
		getcwd(here, sizeof(here));
		start_dir = here;
	}
	
	p = aprintf("\"%s\"  bkd -S -p %d -c %d \"-s%s\" -E \"PATH=%s\"",
		path, opts->port, opts->count, start_dir, getenv("PATH"));
	len = strlen(p) + 1;
	for (i = 0; eVars[i]; i++) len += envSize(eVars[i]);
	nav = malloc((ac + 1) * sizeof(char *));
	argv_save(ac, av, nav, 0);
	len += argv_size(nav);
	cmd = malloc(len);
	strcpy(cmd, p);
	free(p);
	for (i = 0; eVars[i]; i++) addEnvVar(cmd, eVars[i]);
	for (i = 0; nav[i]; i++) {
		strcat(cmd, " \"");
		strcat(cmd, nav[i]);
		strcat(cmd, "\"");
	}
	assert(strlen(cmd) < len);

	unless (schSCManager = OpenSCManager(0, 0, SC_MANAGER_ALL_ACCESS)) {
        	fprintf(stderr,
		    "OpenSCManager failed - %s\n", getError(err, 256));
out:		if (cmd) free(cmd);
		if (schService) CloseServiceHandle(schService);
       		if (schSCManager) CloseServiceHandle(schSCManager);
		argv_free(nav, 0);
		return;
	}

	schService = OpenService(schSCManager, SERVICENAME, SERVICE_ALL_ACCESS);
	if (schService) {	/* if there is a old entry remove it */
		CloseServiceHandle(schService);
		bkd_remove_service(0);
	}

	/*
	 * XXX Starting bk on a network drive is unsupported.
	 */
	while (!(schService = CreateService(schSCManager, SERVICENAME,
			SERVICEDISPLAYNAME, SERVICE_ALL_ACCESS,
			SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
			SERVICE_ERROR_NORMAL, cmd, NULL, NULL,
			DEPENDENCIES, NULL, NULL))) {
		if (try++ > 3) {
			fprintf(stderr,
			    "CreateService failed - %s\n", getError(err, 256));
			goto out;
		}
		usleep(0);
	}
	unless (Opts.quiet) {
		fprintf(stderr, "%s installed.\n", SERVICEDISPLAYNAME);
	}

	/*
	 * Here is where we enter the bkd_service_loop()
	 */
	if (StartService(schService, --ac, (LPCTSTR *)++av) == 0) {
		fprintf(stderr, "%s cannot start service. %s\n",
		    SERVICEDISPLAYNAME, getError(err, 256));
		goto out;
	}

	/*
	 * Make sure the service is fully started before we return
	 */
	for (try = 0; QueryServiceStatus(schService, &serviceStatus) &&
		serviceStatus.dwCurrentState == SERVICE_START_PENDING; ) {
		if (try++ > 3) break;
		usleep(10000);
	}
	if (serviceStatus.dwCurrentState != SERVICE_RUNNING) {
		fprintf(stderr,
		    "Warning: %s did not start fully.\n", SERVICEDISPLAYNAME);
		goto out;
	}
	usleep(100000);

	unless (Opts.quiet) { 
		fprintf(stderr, "%s started.\n", SERVICEDISPLAYNAME);
	}
	
	goto out;
}

/*
 * start bkd service
 */
void
bkd_start_service(void (*service_func)(int, char **))
{
	SERVICE_TABLE_ENTRY dispatchTable[] = {
		{SERVICENAME, NULL},
		{NULL, NULL}
	};

	dispatchTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION) service_func;
	unless (StartServiceCtrlDispatcher(dispatchTable)) {
		logMsg("StartServiceCtrlDispatcher failed.");
	}
}

/*
 * stop & remove the bkd service
 */
void
bkd_remove_service(int verbose)
{
	SC_HANDLE   schService;
	SC_HANDLE   schSCManager;

	schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	unless (schSCManager) {
        	fprintf(stderr, "OpenSCManager failed:%s\n", getError(err,256));
		return;
	}
	schService = OpenService(schSCManager, SERVICENAME, SERVICE_ALL_ACCESS);

	unless (schService) {
		fprintf(stderr, "OpenService failed:%s\n", getError(err,256));
		CloseServiceHandle(schSCManager);
		return;
	}
	if (ControlService(schService, SERVICE_CONTROL_STOP, &srvStatus)) {
		if (verbose) {
			fprintf(stderr, "Stopping %s.", SERVICEDISPLAYNAME);
		}
		Sleep(1000);

		while(QueryServiceStatus(schService, &srvStatus)) {
			if (srvStatus.dwCurrentState == SERVICE_STOP_PENDING ) {
				if (verbose) fprintf(stderr, ".");
				Sleep(1000);
			} else {
				break;
			}
		}
		if (srvStatus.dwCurrentState == SERVICE_STOPPED) {
			if (verbose) {
				fprintf(stderr,
				    "\n%s stopped.\n", SERVICEDISPLAYNAME);
			}
		} else {
			fprintf(stderr,
			    "\n%s failed to stop.\n", SERVICEDISPLAYNAME);
		}
	}
	if(DeleteService(schService)) {
		if (verbose) {
			fprintf(stderr, "%s removed.\n", SERVICEDISPLAYNAME);
		}
	} else {
		fprintf(stderr,
		    "DeleteService failed - %s\n", getError(err,256));
	}
	CloseServiceHandle(schService);
	CloseServiceHandle(schSCManager);
}

/*
 * code for (mini) helper thread
 */
DWORD WINAPI
helper(LPVOID param)
{
	SOCKET	sock;

	hServerStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	for (;;) {
		WaitForSingleObject(hServerStopEvent, INFINITE);
		bkd_quit = 1;

		/*
		 * Send a fake connection to unblock the accept() call in
		 * bkd_service_loop(), We do this because SIGINT
		 * cause a error exit, and it is done in a new thread.
		 * This is not what we want, we want bkd_service_loop()
		 * to shut down gracefully.
		 * XXX Note: If we need to use SIGINT in the future; try
		 * calling reportStatus() and exit(0) in the signal handler,
		 * it may be enough to keep the service manager happy.
		 */
		sock = tcp_connect("localhost", Opts.port ?Opts.port : BK_PORT);
		CloseHandle((HANDLE) sock);
	}
}

int
bkd_register_ctrl(void)
{
	DWORD threadId;
	/*
	 * Create a mini helper thread to handle the stop request.
	 * We need the helper thread becuase we cannot raise SIGINT in the 
	 * context of the service manager.
	 * We cannot use event object directly becuase we cannot wait
	 * for a socket event and a regular event together
	 * with the WaitForMultipleObject() interface.
	 */
	CreateThread(NULL, 0, helper, 0, 0, &threadId);

	/*
	 * register our service control handler:
	 */
	statusHandle =
		RegisterServiceCtrlHandler(SERVICENAME, bkd_service_ctrl);
	if (statusHandle == 0) {
		char msg[2048];

            	sprintf(msg, "bkd_register_ctrl: cannot get statusHandle, %s",
		    getError(err, 256));
            	logMsg(msg);
	}
	return (statusHandle);
}

/*
 * This function is called by the service control manager
 */
void WINAPI
bkd_service_ctrl(DWORD dwCtrlCode)
{
	switch(dwCtrlCode)
	{
	    case SERVICE_CONTROL_STOP:
           	reportStatus(statusHandle, SERVICE_STOP_PENDING, NO_ERROR, 500);
    		if (hServerStopEvent) {
			SetEvent(hServerStopEvent);
		} else {
			/* we should never get here */
			logMsg("bkd_service_ctrl: missing stop event object");
		}
            	return;

	    case SERVICE_CONTROL_INTERROGATE:
		break;

	    default:
		break;
	}
	reportStatus(statusHandle, srvStatus.dwCurrentState, NO_ERROR, 0);
}

/*
 * Belows are utilities functions used by the bkd service
 */
char *
getError(char *buf, int len)
{
	int	rc;
	char	*buf1 = NULL;

	rc = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|
		FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_ARGUMENT_ARRAY,
		NULL, GetLastError(), LANG_NEUTRAL, (LPTSTR)&buf1, 0, NULL);

    	/* supplied buffer is not long enough */
    	if (!rc || (len < rc+14)) {
       		buf[0] = 0;
    	} else {
        	buf1[lstrlen(buf1)-2] = 0;
        	sprintf(buf, "%s (0x%lx)", buf1, GetLastError());
    	}
    	if (buf1) LocalFree((HLOCAL) buf1);
	return buf;
}

void
reportStatus(SERVICE_STATUS_HANDLE sHandle, 
			int dwCurrentState, int dwWin32ExitCode, int dwWaitHint)
{
	static int dwCheckPoint = 1;

        if (dwCurrentState == SERVICE_START_PENDING) {
		srvStatus.dwControlsAccepted = 0;
        } else {
		srvStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	}
	srvStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	srvStatus.dwServiceSpecificExitCode = 0;
        srvStatus.dwCurrentState = dwCurrentState;
        srvStatus.dwWin32ExitCode = dwWin32ExitCode;
        srvStatus.dwWaitHint = dwWaitHint ? dwWaitHint : 100;
        if ((dwCurrentState == SERVICE_RUNNING) ||
	    (dwCurrentState == SERVICE_STOPPED)) {
		srvStatus.dwCheckPoint = 0;
        } else {
		srvStatus.dwCheckPoint = dwCheckPoint++;
	}
        if (SetServiceStatus(sHandle, &srvStatus) == 0) {
		char msg[2048];
	
		sprintf(msg,
		    "bkd: cannot set service status; %s", getError(err, 256));
		logMsg(msg);
		exit(1);
        }
}

void
logMsg(char *msg)
{
	HANDLE	evtSrc = RegisterEventSource(NULL, SERVICENAME);

	unless (evtSrc) return;
	ReportEvent(evtSrc, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0,
		(LPCTSTR *)&msg, NULL);
	DeregisterEventSource(evtSrc);
}
#endif
