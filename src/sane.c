/*
 * sane.c - check for various things which should be true and whine if they
 * are not.  Exit status reflects the whining level.
 *
 * Copyright (c) 2000 Larry McVoy, Andrew Chang.
 * %K%
 */

#include "system.h"
#include "sccs.h"
#include "tomcrypt/mycrypt.h"

private	int	chk_permissions(void);
private	int	chk_idcache(void);
private void	chk_id(void);
private	void	chk_code(void);

int
sane_main(int ac, char **av)
{
	int	c, readonly = 0, errors = 0;
	
	if (ac == 2 && streq("--help", av[1])) {
		system("bk help sane");
		return (0);
	}

	while ((c = getopt(ac, av, "r")) != -1) {
		switch (c) {
		    case 'r':
			readonly = 1; /* Do not check write access */
			break;
		    default: 
			system("bk help -s sane");
			return (1);
		}
	}

	if (chk_host()) errors++;
	if (chk_user()) errors++;
	if (sccs_cd2root(0, 0) == 0) {
		if (!readonly && chk_permissions()) errors++;
		else if (chk_idcache()) errors++;
	} else {
		fprintf(stderr, "sane: not in a BitKeeper repository\n");
		errors++;
	}
	//chk_tcl();
	//chk_ssh();
	//chk_http();
	chk_id();
	chk_code();	/* various code sanity checks */
	return (errors);
}

/* insist on a valid host */
int
chk_host(void)
{
	char	*host = sccs_gethost();
	char	*p;

	if (host && (p = strrchr(host, '.')) && streq(&p[1], "localdomain")) {
		fprintf(stderr,
"================================================================\n"
"sane: Warning: unable to obtain fully qualified hostname for this machine.\n"
"\"%s\" does not look like a valid domain name.\n"
"================================================================\n",
		host);
	}

	unless (host && 
	    strchr(host, '.') && 
	    !strneq(host, "localhost", 9)) {
		fprintf(stderr,
"================================================================\n"
"sane: bad host name: \"%s\".\n"
"BitKeeper requires a fully qualified hostname.\n"
"Names such as \"localhost.*\" are also illegal.\n"
"================================================================\n",
		    host ? host : "<empty>");
		return (1);
	}
	/*
	 * Check for legal hostnames.  _, (, and ) are not legal 
	 * hostnames, but it should bother BK and we don't want to
	 * deal with the support calls.
	 *  ( RedHat sometimes installs machines with a hostname
	 *    of 'machine.(none)' )
	 * Later we might want to just warn about these if we know
	 * the user is interactive.
	 */
	for (p = host; *p; p++) {
		unless (isalnum(*p) || 
		    *p == '.' ||
		    *p == '-' ||
		    *p == '_' ||
		    *p == '(' ||
		    *p == ')') { 
			  fprintf(stderr,
"================================================================\n"
"sane: bad host name: \"%s\".\n"
"BitKeeper requires a valid hostname.\n"
"These consist of [a-z0-9-]+ separated by '.'.\n"
"See http://www.ietf.org/rfc/rfc952.txt\n"
"================================================================\n",
		    host ? host : "<empty>");
			return (1);
		}
	}
	return (0);
}

int
chk_user(void)
{
	char	*user = sccs_getuser();

	if (streq(user, "root")) {
		fprintf(stderr,
		    "Warning: running BitKeeper as root is not a good idea!\n");
		fprintf(stderr, 
		    "Set the BK_USER environment variable to your real user name.\n");
		return (0);
	}
	if (strchr(user, '@')) {
		fprintf(stderr, 
"User name (\"%s\") may not contain an @ sign.\n", user);
		fprintf(stderr, 
		    "Set the BK_USER environment variable to your real user name.\n");
		return (1);
	}
	unless (user) {
		fprintf(stderr, "Cannot determine user name.\n");
		return (1);
	}
	return (0);
}

int
write_chkdir(char *path, int must)
{
again:	unless (exists(path) && isdir(path)) {
		if (must) {
			if (mkdirp(path)) {
				perror("sane mkdir:");
				return (1);
			} else {
				goto again;
			}
			fprintf(stderr, "sane: %s is missing\n", path);
			return (1);
		}
		return (0);
	}
	if (access(path, W_OK) != 0) {
		fprintf(stderr, "sane: no write permission on %s\n", path);
		return (1);
	}
	return (0);
}

int
write_chkfile(char *path, int must)
{
again:	unless (exists(path)) {
		if (must) {
			if (mkdirf(path)) {
				perror("sane mkdir:");
				return (1);
			}
			if (close(creat(path, 0666))) {
				perror("sane create:");
				return (1);
			} else {
				goto again;
			}
			fprintf(stderr, "sane: %s is missing\n", path);
			return (1);
		}
		return (0);
	}
	if (access(path, W_OK) != 0) {
		fprintf(stderr, "sane: no write permission on %s\n", path);
		return (1);
	}
	return (0);
}

private int
chk_permissions(void)
{
	return (write_chkdir("BitKeeper", 1) |
	    write_chkdir("BitKeeper/etc", 1) |
	    write_chkdir("BitKeeper/etc/SCCS", 1) |
	    write_chkdir("BitKeeper/tmp", 1) |
	    write_chkdir("BitKeeper/log", 1) |
	    write_chkfile("BitKeeper/log/cmd_log", 0) |
	    write_chkfile("BitKeeper/log/repo_log", 0) |
	    write_chkdir("BitKeeper/triggers", 0) |
	    write_chkdir("SCCS", 1));
}

private int
chk_idcache(void)
{
	if (sccs_lockfile(IDCACHE_LOCK, 6, 0)) {
		fprintf(stderr, "sane: can't lock id cache\n");
		return (1);
	}
	sccs_unlockfile(IDCACHE_LOCK);
	return (0);
}

/*
 * If the repository does not have an identifier, create one if we can.
 * Note that the full name of the repository is the root key plus this.
 * The repo name is host|/path/to/repo|user|date|randbits.
 */
private void
chk_id(void)
{
	char	path[MAXPATH];
	char	buf[100];
	char	rand[6];
	unsigned long	outlen;
	time_t	t = time(0);
	FILE	*f;

	if (exists(REPO_ID)) return;	/* validate existing ID?? */
	unless (f = fopen(REPO_ID, "w")) return;	/* no write perms? */

	/*date*/
	strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", localtimez(&t, 0));
	fprintf(f, "%s%s|", buf, sccs_zone(time(0)));

	/*host*/
	fprintf(f, "%s|", sccs_realhost());

	/*path*/
	path[0] = 0;
	getcwd(path, MAXPATH);
	fprintf(f, "%s|", path);

	/*user*/
	fprintf(f, "%s|", sccs_realuser());

	/*randbits*/
	if (rng_get_bytes(buf, 3, 0) != 3) {
		fprintf(stderr, "chk_id: failed to get random date\n");
		exit(1);
	}
	outlen = sizeof(rand);
	if (base64_encode(buf, 3, rand, &outlen)) {
		fprintf(stderr, "chk_id: %s\n", crypt_error);
		exit(1);
	}
	assert(outlen < sizeof(rand));
	fprintf(f, "%s\n", rand);

	fclose(f);
}

private void
chk_code(void)
{
	struct	stat	sb1, sb2;

	if (lstat(CHANGESET, &sb1)) return; /* it happens in t.gone */
	if (fast_lstat(CHANGESET, &sb2)) {
		fprintf(stderr, "chk_code: failed to faststat ChangeSet\n");
		exit(1);
	}
	unless (getenv("TZ")) {
		/* The win32 _stat() function doesn't work if TZ is set! */
		if (sb1.st_mode != sb2.st_mode) {
			fprintf(stderr, "chk_code: st_mode %o %o\n",
			    sb1.st_mode, sb2.st_mode);
			exit(1);
		}
		if (sb1.st_mtime != sb2.st_mtime) {
			fprintf(stderr, "chk_code: st_mtime %u %u\n",
			    (u32)sb1.st_mtime, (u32)sb2.st_mtime);
			exit(1);
		}
		if (sb1.st_ctime != sb2.st_ctime) {
			fprintf(stderr, "chk_code: st_ctime %u %u\n",
			    (u32)sb1.st_ctime, (u32)sb2.st_ctime);
			exit(1);
		}
		/* don't test st_atime, the stat changes it */
	}
	if (sb1.st_size != sb2.st_size) {
		fprintf(stderr, "chk_code: st_size %u %u\n",
		    (u32)sb1.st_size, (u32)sb2.st_size);
		exit(1);
	}
}
