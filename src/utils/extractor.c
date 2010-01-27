/*
 * %K%
 * Copyright (c) 1999 Larry McVoy
 */
#include "system.h"

#ifndef MAXPATH
#define	MAXPATH		1024
#endif
#ifdef	WIN32
#define	PFKEY		"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
#define	WIN_UNSUPPORTED	"Windows 2000 or later required to install BitKeeper"
#endif
#define	TMP		"bksetup"

#ifdef	WIN32
static char MSYS_ERROR[] =
"You seem to be running the installer under MINGW-MSYS. The installer\n"
"is not supported under this configuration. Please start a cmd.exe console\n"
"and run the installer from there.\n"
"Thanks!\n";
#endif

typedef struct opts {
	u32	shellx:1;	/* -l: install shellx extension */
	u32	scc:1;		/* -s: install visual studio dll */
	u32	upgrade:1;	/* -u: batch upgrade, no prompts */
} opts;

#ifdef WIN32
char	*options = "hslu";
#else
char	*options = "hu";
#endif
#define	EXP_OPT	"explorer-plugin"
#define	VS_OPT	"visual-studio-plugin"
#define	UP_OPT	"batch-upgrade"
longopt	lopts[] = {
#ifdef WIN32
	{ EXP_OPT, 'l' },
	{ VS_OPT, 's' },
#endif
	{ UP_OPT, 'u' },
};

char	*prog;
char	*bindir;

extern unsigned int sfio_size;
extern unsigned char sfio_data[];
extern unsigned int data_size;
extern unsigned char data_data[];
extern unsigned int keys_size;
extern unsigned char keys_data[];

void	cd(char *dir);
void	extract(char *, char *, u32, char *);
char*	findtmp(void);
char*	getbkpath(void);
void	symlinks(void);
int	hasDisplay(void);
char	*defaultBin(void);
void	usage(void);

int
main(int ac, char **av)
{
	int	i, c;
	int	rc = 0, dolinks = 0, embeddedkey = 0;
	pid_t	pid = getpid();
	FILE	*f;
	char	*dest = 0, *bkpath = 0, *tmp = findtmp();
	char	tmpdir[MAXPATH], buf[MAXPATH], pwd[MAXPATH];
	char	*p;
	opts	opts;
#ifdef	WIN32
	HCURSOR h;

	/* Refuse to install on unsupported versions of Windows */

	/* The following code has been commented out because right now the
	 * bk installer is a Console application (i.e. if it doesn't have
	 * a console because the user double-clicked on the icon, a console
	 * is created. There's a cset in the RTI queue (2005-08-18-001) that
	 * GUIfies the installer. When that cset gets pulled, this code should
	 * be used instead of the next block.
	 *
	 *unless (win_supported()) {
	 *	if (hasConsole()) {
	 *		fprintf(stderr, "%s\n", WIN_UNSUPPORTED);
	 *		exit(1);
	 *	} else {
	 *		MessageBox(0, WIN_UNSUPPORTED, 0, 
	 *		    MB_OK | MB_ICONERROR);
	 *		exit(1);
	 *	}
	 *}
	 */
	 unless (win_supported()) {
		MessageBox(0, WIN_UNSUPPORTED, 0, MB_OK | MB_ICONERROR);
		exit(1);
	 }
	 if ((p = getenv("OSTYPE")) && streq(p, "msys")
	      && !getenv("BK_REGRESSION")) {
		 fprintf(stderr, MSYS_ERROR);
		 exit(1);
	 }
	_fmode = _O_BINARY;
#endif

	bzero(&opts, sizeof(opts));
	prog = av[0];
	bindir = defaultBin();

	/* rxvt bugs */
	setbuf(stderr, 0);
	setbuf(stdout, 0);

	getcwd(pwd, sizeof(pwd));

	/*
	 * If they want to upgrade, go find that dir before we fix the path.
	 */
	bkpath = getbkpath();
	while ((c = getopt(ac, av, options, lopts)) != -1) {
		switch (c) {
		    case 'l': opts.shellx = 1; break;
		    case 's': opts.scc = 1; break;
		    case 'u': opts.upgrade = 1; break;
		    default: usage();
		}
	}

	if (opts.upgrade) {
		unless (dest = bkpath) dest = bindir;
	} else if (av[optind]) {
		dest = fullname(av[optind], 0);
#ifndef	WIN32
		unless (getenv("BK_NOLINKS")) dolinks = 1;
#endif
	} else if (!hasDisplay()) {
		usage();
	}

	sprintf(tmpdir, "%s/%s%u", tmp, TMP, pid);
#ifdef	WIN32
	h = SetCursor(LoadCursor(0, IDC_WAIT));
#endif
	fprintf(stderr, "Please wait while we unpack in %s ...\n", tmpdir);
	if (mkdir(tmpdir, 0700)) {
		perror(tmpdir);
		exit(1);
	}
	cd(tmpdir);

	/*
	 * Add this directory and BK directory to the path.
	 * Save the old path first, subprocesses need it.
	 */
#ifdef	WIN32
	safe_putenv("BK_USEMSYS=1");
#endif
	safe_putenv("_BK_ITOOL_OPATH=%s", getenv("PATH"));
	safe_putenv("BK_OLDPATH=");
	safe_putenv("PATH=%s%c%s/bitkeeper%c%s",
	    tmpdir, PATH_DELIM, tmpdir, PATH_DELIM, getenv("PATH"));

	/* The name "sfio.exe" should work on all platforms */
	extract("sfio.exe", sfio_data, sfio_size, tmpdir);
	extract("sfioball", data_data, data_size, tmpdir);

	/* Unpack the sfio file, this creates ./bitkeeper/ */
	if (system("sfio.exe -imq < sfioball")) {
		if (errno == EPERM) {
			fprintf(stderr,
"bk install failed because it was unabled to execute sfio in %s.\n"
"On some systems this might be /tmp does not have execute permissions.\n"
"In that case try rerunning with TMPDIR set to a new directory.\n",
			    tmpdir);
			exit(1);
		}
		perror("sfio");
		exit(1);
	}
	symlinks();

	/*
	 * extract the embedded config file
	 */
	if (bkpath) concat_path(buf, bkpath, "config");
	/* We can't compare with the inskeys_marker template directly
	 * because that would put two copies of the template in the binary.
	 * That 12 is the lenght of 'license: BKL'
	 */
	for (p = keys_data + 12; *p != '\n'; p++) {
		if (*p != 'X') embeddedkey = 1;
	}
	if (embeddedkey) {
		if (bkpath && exists(buf)) {
			/* merge embedded file into existing config */
			sprintf(buf,
			    "bk config -m '%s'/config - > bitkeeper/config",
			    bkpath);
			f = popen(buf, "w");
			fputs(keys_data, f);
			pclose(f);
		} else {
			/* Just write embedded config */
			f = fopen("bitkeeper/config", "w");
			fputs(keys_data, f);
			fclose(f);
		}
	} else if (bkpath && exists(buf)) {
		/* just copy existing config */
		fileCopy(buf, "bitkeeper/config");
		chmod("bitkeeper/config", 0666);
	}

	mkdir("bitkeeper/gnu/tmp", 0777);

	/* cd back to the original pwd so we get any config's */
	cd(pwd);
#ifdef	WIN32
	/*
	 * XXX Careful... understand before removing..
	 * Without this, a gui install can fail with
	 * .. bkscript: bk not found
	 * Debugging shows that PATH=c/tmp/bksetupXXX/bitkeeper:c/tmp/...
	 * That is, the leading '/' is missing from all components in
	 * PATH, HOME, TMP, TEMP
	 * which is stuff that msys munges with the mount stuff.
	 * however, this is the first time msys.dll might have been
	 * called and there might not be a fstab setup up yet?
	 * Anyway, calling it once early on seems to fix later races
	 * from happening.
	 */
	system("bk sh -c exit");
#endif

	if (dest) {
		putenv("BK_NO_GUI_PROMPT=1");
		buf[0] = 0;
		/*
		 * This is silent unless we have an error.  And if there is
		 * an error we want that error to print out.
		 */
		if (system("bk lease renew")) {
			    rc = 1;
			    goto out;
		}
		fprintf(stderr, "Installing BitKeeper in %s\n", dest);
#ifdef WIN32
		sprintf(buf, "bk _install %s %s %s \"%s\"",
		    opts.shellx ? "-l" : "",
		    opts.scc ? "-s" : "",
		    opts.upgrade ? "-u" : "",
		    dest);
#else
		sprintf(buf, "bk _install %s %s \"%s\"",
		    dolinks ? "-S" : "",
		    opts.upgrade ? "-u" : "",
		    dest);
#endif
		unless (rc = system(buf)) {
			fprintf(stderr, "\nInstalled version information:\n\n");
			sprintf(buf, "'%s/bk' version", dest);
			system(buf);
			fprintf(stderr, "\nInstallation directory: ");
			sprintf(buf, "'%s/bk' bin", dest);
			system(buf);
			fprintf(stderr, "\n");
		}
	} else {
		sprintf(buf, "bk installtool");
		for (i = 1; av[i]; i++) {
			strcat(buf, " ");
			strcat(buf, av[i]);
		}
		av[i] = 0;
#ifdef	WIN32
		fprintf(stderr, "Running installer...\n");
		SetCursor(h);
#endif
		rc = system(buf);
	}

	/* Clean up your room, kids. */
out:	cd(tmpdir);
	p = 0;
	if ((rc == 0) && (f = fopen("bitkeeper/install_dir", "r"))) {
		/*
		 * install_dir is written at the end of _install in bk.sh
		 * We need to run the new bk so register can run in the
		 * background while the old bk is deleted.  Fixes a
		 * windows race condition.
		 */
		if (fnext(buf, f)) {
			chomp(buf);
			p = aprintf("\"%s/bk\" _register", buf);
		}
		fclose(f);
	}
	cd("..");
	if (p) {
		system(p);
		free(p);
	}
	unless (getenv("BK_SAVE_INSTALL")) {
		fprintf(stderr,
		    "Cleaning up temp files in %s%u ...\n", TMP, pid);
		sprintf(buf, "%s%u", TMP, pid);
		/*
		 * Sometimes windows has processes sitting in here and we
		 * have to wait for them to go away.
		 * XXX - The "if (install_dir) system(bk _register)"
		 * above intends to fix the waiting problem, so the retry
		 * loop is not needed anymore -- yet we're not wanting to
		 * pull it out right before a release and have customers
		 * hit a problem that this would solve.
		 * Please remove the retry loop and test on windows.
		 */
		for (i = 0; i < 10; ) {
			/* careful */
			rmtree(buf);
			unless (isdir(buf)) break;
			sleep(++i);
		}
	}

	/*
	 * Bitchin'
	 */
	exit(rc);
}

void
usage(void)
{
#ifdef WIN32
	fprintf(stderr,
	    "usage: %s [--%s][--%s][--%s || <directory>]\n", 
	    prog, EXP_OPT, VS_OPT, UP_OPT);
#else
	fprintf(stderr, "usage: %s [--%s || <directory>]\n", prog, UP_OPT);
#endif
	fprintf(stderr,
"\nInstalls BitKeeper on the system.\n"
"\n"
"With no arguments this installer will unpack itself in a temp\n"
"directory and then start a graphical installer to walk through the\n"
"installation.\n"
"\n"
"If a directory is provided on the command line then a default\n"
"installation is written to that directory.\n"
"\n"
"The --"
UP_OPT
" option is for batch upgrades.\n"
"The existing BitKeeper is found on your PATH and then this version\n"
"is installed over the top of it.  If no existing version of BitKeeper\n"
"is found, then a new installation is written to:\n"
"%s\n\n"
#ifdef WIN32
"The --"
VS_OPT
" option enables the bkscc dll for Visual Studio integration.\n"
"\n"
"The --"
EXP_OPT
" option enables the shell extension for Windows Explorer.\n"
"\n"
"Administrator privileges are required for a full installation.  If\n"
"installing from a non-privileged account, then the installer will only\n"
"be able to do a partial install.\n"
#else
"Normally symlinks are created in /usr/bin for 'bk' and common SCCS\n"
"tools.  If the user doesn't have permissions to write in /usr/bin\n"
"or BK_NOLINKS is set then this step will be skipped.\n"
#endif
#if !defined(WIN32) && !defined (__APPLE__)
"\n"
"If DISPLAY is not set in the environment, then the destination must\n"
"be set on the command line.\n"
#endif
	    , bindir);
	exit(1);
}

char *
defaultBin(void)
{
#ifdef WIN32
	char	*bindir;
	char	*p, *buf;

	if (buf = reg_get(PFKEY, "ProgramFilesDir", 0)) {
		for (p = buf; *p; p++) {
			if (*p == '\\') *p = '/';
		}
		bindir = aprintf("%s/BitKeeper", buf);
		free(buf);
	} else {
		bindir = "C:/Program Files/BitKeeper";
	}
	return (bindir);
#else
	return ("/usr/libexec/bitkeeper");
#endif
}

int
hasDisplay(void)
{
#if defined(WIN32) || defined (__APPLE__)
	return (1);
#else
	return (getenv("DISPLAY") != 0);
#endif
}

/*
 * If we have symlinks file then emulate:
 * while read a b; do ln -s $a $b; done < symlinks
 */
void
symlinks(void)
{
	FILE	*f;
	char	*p;
	char	buf[MAXPATH*2];

	unless (size("bitkeeper/symlinks") > 0) return;
	cd("bitkeeper");
	unless (f = fopen("symlinks", "r")) goto out;
	while (fgets(buf, sizeof(buf), f)) {
		chomp(buf);
		unless (p = strchr(buf, '|')) goto out;
		*p++ = 0;
		symlink(buf, p);
	}
out:	fclose(f);
	cd("..");
}

void
cd(char *dir)
{
	if (chdir(dir)) {
		perror(dir);
		exit(1);
	}
}

void
extract(char *name, char *data, u32 size, char *dir)
{
	int	fd, n;
	GZIP	*gz;
	char	buf[BUFSIZ];

	sprintf(buf, "%s/%s.zz", dir, name);
	fd = open(buf, O_WRONLY | O_TRUNC | O_CREAT | O_EXCL, 0755);
	if (fd == -1) {
		perror(buf);
		exit(1);
	}
	setmode(fd, _O_BINARY);
	if (write(fd, data, size) != size) {
		perror(buf);
		exit(1);
	}
	close(fd);
	unless (gz = gzopen(buf, "rb")) {
		perror(buf);
		exit(1);
	}
	sprintf(buf, "%s/%s", dir, name);
	fd = open(buf, O_WRONLY | O_TRUNC | O_CREAT | O_EXCL, 0755);
	setmode(fd, _O_BINARY);
	while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
		write(fd, buf, n);
	}
	close(fd);
	gzclose(gz);
	sprintf(buf, "%s/%s.zz", dir, name);
	unlink(buf);
}

char *
getbkpath(void)
{
	FILE	*f;
	char	*p;
	char	buf[MAXPATH], buf2[MAXPATH];

	unless (p = which("bk")) return (0);
	free(p);
	unless (f = popen("bk bin", "r")) return (0);

	buf[0] = 0;
	fnext(buf, f);
	pclose(f);
	unless (buf[0])	return (0);
	chomp(buf);
	sprintf(buf2, "bk pwd '%s'", buf);
	f = popen(buf2, "r");
	buf[0] = 0;
	fnext(buf, f);
	pclose(f);
	unless (buf[0]) return (0);
	chomp(buf);
	return (strdup(buf));
}

#ifdef WIN32
private int
istmp(char *path)
{
	char	*p = &path[strlen(path)];
	int	fd;

	sprintf(p, "/findtmp%d", getpid());
	fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
	if (fd == -1) {
err:		*p = 0;
		return (0);
	}
	if (write(fd, "Hi\n", 3) != 3) {
		close(fd);
		goto err;
	}
	close(fd);
	unlink(path);
	*p = 0;
	return (1);
}
#endif

/*
 * I'm not at all convinced we need this.
 */
char*
findtmp(void)
{
	char	*p;

	if (p = getenv("TMPDIR")) return (p);
#ifdef	WIN32
	char	*places[] = {
			"Temp",
			"Tmp",
			"WINDOWS/Temp",
			"WINDOWS/Tmp",
			"WINNT/Temp",
			"WINNT/Tmp",
			"cygwin/tmp",
			0
		};
	int	i;
	char	drive;
	char	path[MAXPATH];

	sprintf(path, "%s", TMP_PATH);
	if (istmp(path)) return (strdup(path));
	for (drive = 'C'; drive <= 'Z'; drive++) {
		for (i = 0; places[i]; ++i) {
			sprintf(path, "%c:/%s", drive, places[i]);
			if (istmp(path)) return (strdup(path));
		}
	}
	fprintf(stderr, "Can't find a temp directory\n");
	exit(1);
#else
	return ("/tmp");
#endif
}
