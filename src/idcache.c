#include "system.h"
#include "sccs.h"

WHATSTR("@(#)%K%");

private	u32	id_sum;
private	MDBM	*idDB;		/* used to detect duplicate keys */
private	int	dups;		/* duplicate key count */
private	int	mixed;		/* running in mixed long/short mode */
private	void	rebuild(void);
private	int	caches(char *filename, struct stat *sb, void *data);

private inline sccs *
init(char *name, int flags)
{
	sccs	*s = sccs_init(name, flags);

	return (s);
}

int
idcache_main(int ac, char **av)
{
	unless ((ac == 2) && streq(av[1], "-q")) {
		fprintf(stderr, "Rebuilding idcache\n");
	}

	if (proj_cd2root()) return (1);

	rebuild();
	return (dups ? 1 : 0);
}

/*
 * rebuild the caches.
 */
private	void
rebuild(void)
{
	sccs	*cset;
	FILE	*id_cache;
	char	s_cset[] = CHANGESET;
	char	*id_tmp;

	unless (cset = init(s_cset, 0)) {
		perror("sfiles: can't init ChangeSet");
		exit(1);
	}
	unless (HAS_SFILE(cset) && cset->cksumok) {
		perror("sfiles: can't init ChangeSet");
		sccs_free(cset);
		exit(1);
	}
	mixed = !LONGKEY(cset);
	sccs_free(cset);
	unless (id_tmp = bktmp_local(0, "id_tmp")) {
		perror("bktmp_local");
		exit(1);
	}
	unless (id_cache = fopen(id_tmp, "w")) {
		perror(id_tmp);
		exit(1);
	}
	fprintf(id_cache, "\
# This is a BitKeeper cache file.\n\
# If you suspect that the file is corrupted, simply remove it and \n\
# and it will be rebuilt as needed.  \n\
# The format of the file is <ID> <PATHNAME>\n\
# The file is used for performance during makepatch/takepatch commands.\n");
	id_sum = 0;
	idDB = mdbm_open(NULL, 0, 0, GOOD_PSIZE);
	assert(idDB);
	walksfiles(".", caches, id_cache);
	fprintf(id_cache, "#$sum$ %u\n", id_sum);
	fclose(id_cache);
	if (dups) {
		fprintf(stderr, "Not updating idcache due to dups.\n");
		unlink(id_tmp);
		goto out;
	}
	if (sccs_lockfile(IDCACHE_LOCK, 16, 0)) {
		fprintf(stderr, "Not updating cache due to locking.\n");
		unlink(id_tmp);
	} else {
		unlink(IDCACHE);
		if (rename(id_tmp, IDCACHE)) {
			perror("rename of idcache");
			unlink(IDCACHE);
		}
		if (sccs_unlockfile(IDCACHE_LOCK)) perror(IDCACHE_LOCK);
		chmod(IDCACHE, GROUP_MODE);
	}
out:	
	free(id_tmp);
	mdbm_close(idDB);
}

private	void
save(sccs *sc, MDBM *idDB, char *buf)
{
	if (mdbm_store_str(idDB, buf, sc->gfile, MDBM_INSERT)) {
		if (errno == EEXIST) {
			char	*sfile = name2sccs(mdbm_fetch_str(idDB, buf));

			fprintf(stderr,
			    "ROOTKEY %s\n\tused by %s\n\tand by  %s\n",
			    buf, sc->sfile, sfile);
			free(sfile);
			dups++;
		} else {
			perror("mdbm_store");
			exit(1);
		}
	}
}

private void
idsum(u8 *s)
{
	while (*s) id_sum += *s++;
}

private	int
caches(char *file, struct stat *sb, void *data)
{
	FILE	*id_cache = (FILE *)data;
	sccs	*sc;
	delta	*ino;
	char	*t;
	char	buf[MAXPATH*2];

	file += 2;
	unless (sc = init(file, INIT_NOCKSUM)) return (0);
	unless (HAS_SFILE(sc) && sc->cksumok) {
		sccs_free(sc);
		return (0);
	}

	ino = sccs_ino(sc);

	/*
	 * Update the id cache if root path != current path.
	 * Add entries for any grafted in roots as well.
	 */
	do {
		assert(ino->pathname);
		sccs_sdelta(sc, ino, buf);
		save(sc, idDB, buf);
		if (sc->grafted || !streq(ino->pathname, sc->gfile)) {
			fprintf(id_cache, "%s %s\n", buf, sc->gfile);
			idsum(buf);
			idsum(sc->gfile);
			idsum(" \n");
		}
		if (mixed && (t = sccs_iskeylong(buf))) {
			*t = 0;
			unless (streq(ino->pathname, sc->gfile)) {
				fprintf(id_cache,
				    "%s %s\n", buf, sc->gfile);
				idsum(buf);
				idsum(sc->gfile);
				idsum(" \n");
			}
			save(sc, idDB, buf);
			*t = '|';
		}
		unless (sc->grafted) break;
		while (ino = ino->next) {
			if (ino->random) break;
		}
	} while (ino);
	sccs_free(sc);
	return (0);
}
