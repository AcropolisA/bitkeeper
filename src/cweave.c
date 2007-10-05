/*
 * ChangeSet weave - a one pass weave for the changeset file.
 * Copyright January 13th 2001, Larry McVoy, Andrew Chang
 *
 * The key observation is that there is no nesting in the ChangeSet file;
 * each delta either adds nothing (content free merge) or it adds a block
 * of contiguous lines.  The only requirement is that the file grows "up",
 * i.e., the most recent block is at the top of the weave body so that when
 * we go through the file looking for a version, we hit our stuff first.
 *
 * The fact that there is no real "weave", it's all additions, no subtractions,
 * and each delta is exactly one or zero additional blocks, makes putting the
 * file back together much easier.
 *
 * Our goal is to add the incoming data to the existing file in one pass,
 * but preserve the attributes that we have in the current explode/implode
 * approach, i.e., that the file converges to the same bits with the same
 * deltas.  Here's how we do that.
 *
 * We need a array, indexed by serial number, and containing a pointer and a
 * length.  When we map in the file, we point the pointer at the offset in the
 * file which contains our block of data (which could be handy in general).
 * We scan ahead until the end of the block and record the length.  The
 * pointer points at the first *data* byte, not the ^AI <ser>, and the length
 * is the length of the data only, not the ^AE <ser>.  We're going to be
 * rewriting the serial numbers anyway, they get wacked as a result of
 * weaving in the incoming deltas.
 *
 * In the patch, for the data, we do pretty much the same thing.  
 * For the deltas, we weave them into the delta table as we hit them, which
 * forces us to rewrite all the serial numbers which come after the one we
 * are inserting.  This is no big deal, you scan until you find the place
 * in the linear order, add the delta using the existing serial, then scan
 * again, incrementing every serial number you find (self, parent, in/ex
 * lists), up to but not including the delta you just added.  We also save
 * pointers to the data in the patch.  Note that these pointers have an
 * extra two bytes of "> " at the beginning of each line which must be
 * stripped out.
 *
 * At this point, we have a rebuilt delta table in memory, so we can write
 * that out.  Note that it has correct serial numbers.  Now we take another
 * pass through the inmemory delta table, and we write out the data.  To
 * do this, we just write out
 *	^AI <d->serial>
 *	write the data; in the patch case we need to strip off the leading
 *	"> "
 *	^AE <d->serial>
 */
#include "system.h"
#include "sccs.h"
#include "range.h"

/*
 * Initialize the structure used by cset_insert according to how
 * many csets are going to be added.
 */
int
cweave_init(sccs *s, int extras)
{
	assert(s);
	assert(s->state & S_CSET);
	assert(!s->locs);
	s->iloc = 1;
	s->nloc = extras + 1;
	s->locs = calloc(s->nloc, sizeof(loc));
	return (0);
}

/*
 * Return true if 'a' is earlier than 'b'
 */
private int
earlier(sccs *s, delta *a, delta *b)
{
        int ret;
	char	keya[MAXKEY], keyb[MAXKEY];
 
        if (a->date < b->date) return 1;
        if (a->date > b->date) return 0;
	
	sccs_sdelta(s, a, keya);
	sccs_sdelta(s, b, keyb);
        ret = strcmp(keya, keyb);
        if (ret < 0)   return 1;
        if (ret > 0)   return 0;
        assert("Can't figure out the order of deltas\n" == 0);
        return (-1); /* shut off VC++ comipler warning */
}     

/*
 * Insert the delta into the table.
 * We do not bother with the parent/sibling pointers, just the table
 * pointer. If we need the others, we need to call dinsert.
 *
 * XXX Do we need any special processing for meta delta?
 */
delta *
cset_insert(sccs *s, MMAP *iF, MMAP *dF, char *parentKey)
{
	int	i, error, added = 0;
	delta	*d, *e, *p;
	ser_t	serial = 0; /* serial number for 'd' */ 
	char	*t, *r;
	char	**syms = 0;

	assert(iF);

	/*
	 * Get the "new" delta from the mmaped patch file (iF)
	 */
	d = sccs_getInit(s, 0, iF, DELTA_PATCH, &error, 0, &syms);

	/*
	 * Insert new delta 'd' into s->table in time sorted order
	 */
	if (!s->table) {
		/*
	 	 * Easy case, this is a empty cset file
		 * We are propably getting the 1.0 delta
		 * We only get here if we do "takepatch -i"
		 */
		s->table = d;
		s->tree = d; /* sccs_findKey() wants this */
		d->next = NULL;
		serial = 1;
	} else if (earlier(s, s->table, d)) {
		/*
	 	 * Easy case, this is the newest delta
		 * Just insert it at the top
		 */
		serial = s->nextserial;
		d->next = s->table;
		s->table = d;
	} else {
		/*
		 * OK, We need to insert d somewhere in the middle of the list
		 * find the insertion point...
		 */
		p = s->table;
		while (p) {
			e = p->next;
			if (!e || earlier(s, e, d)) { /* got insertion point */
				serial = p->serial;
				d->next = e;
				p->next = d;
				break;
			}
			p = p->next;
		}
		/*
		 * Update all reference to the moved serial numbers 
		 */
		for (e = s->table; e; e = e->next) {
			int	i;
			if (e->serial < serial) break; /* optimization */

			if (e->serial >= serial) e->serial++;
			if (e->pserial >= serial) e->pserial++;
			if (e->merge >= serial) e->merge++;
			if (e->ptag >= serial) e->ptag++;
			if (e->mtag >= serial) e->mtag++;
			EACH(e->include) {
				if (e->include[i] >= serial) e->include[i]++;
			}
			EACH(e->exclude) {
				if (e->exclude[i] >= serial) e->exclude[i]++;
			}
		}
	}
	s->nextserial++;

	/*
	 * Fix up the parent pointer & serial.
	 * Note: this is just linked enough that we can finish takepatch,
	 * it is NOT a fully linked tree.
	 */
	if (parentKey) {
		p = sccs_findKey(s, parentKey);
		assert(p);
		d->pserial = p->serial;
		d->parent = p;
	}
	sccs_inherit(s, d);
	if ((d->type == 'D') && (s->tree != d)) d->same = 1;

	/*
	 * Fix up d->serial
	 */
	d->serial = serial;
	assert((d->serial == 0) || (d->serial > d->pserial));

	/*
	 * Save dF info, used by cset_write() later
	 * Note: meta delta have NULL dF
	 */
	assert(s->iloc < s->nloc);
	s->locs[s->iloc].p = NULL;
	s->locs[s->iloc].len = 0;
	s->locs[s->iloc].serial = serial;
	if (dF && dF->where) {
		s->locs[s->iloc].p = dF->where;
		s->locs[s->iloc].len = dF->size;

		/*
		 * Fix up d->added
		 */
		t = dF->where;
		r = t + dF->size - 1;
		while ( t <= r) if (*t++ == '\n') added++;
		d->added = added - 1;
	}
	s->iloc++;

	/*
	 * Fix up tag/symbols
	 * We have not run "bk renumber" yet, remote delta 'd' may have
	 * the same rev as a local delta, i.e. We may have two rev 1.3.
	 * We pass in a NULL rev to addsym(), this force the symbol to be
	 * always added. Otherwise, addsym() will skip the symbol if it finds
	 * a local rev 1.3 tagged with the same symbol.
	 * Passing a NULL rev means s->symbols will be in a incomplete state. 
	 * This is ok, because delta_table() uses the s->symbols->metad pointer
	 * to dump the sysmbol information. It does not use rev in that process.
	 */
	EACH (syms) addsym(s, d, d, 0, NULL, syms[i]);
	if (syms) freeLines(syms, free);

	if (s->findkeydb) {
		char	key[MAXKEY];
		datum	k, v;

		sccs_sdelta(s, d, key);
		k.dptr = key;
		k.dsize = strlen(key) + 1;
		v.dptr = (void*)&d;
		v.dsize = sizeof(d);
		if (mdbm_store(s->findkeydb, k, v, MDBM_INSERT)) {
			fprintf(stderr, "fk cache: insert error for %s\n", key);
			perror("insert");
			mdbm_close(s->findkeydb);
			exit(1);
		}
		if (LONGKEY(s)) return (d);

		*strrchr(key, '|') = 0;
		k.dsize = strlen(key) + 1;
		if (mdbm_store(s->findkeydb, k, v, MDBM_INSERT)) {
			fprintf(stderr, "fk cache: insert error for %s\n", key);
			perror("insert");
			mdbm_close(s->findkeydb);
			exit(1);
		}
	}

	return (d);
}

/*
 * Write out the new ChangeSet file.
 */
int
cset_write(sccs *s, int spinners)
{
	FILE	*f;
	int	i;
	delta	*d;

	assert(s);
	assert(s->state & S_CSET);
	assert(s->locs);

	/*
	 * Call sccs_renumber() before writing out the new cset file.
	 * Since the cweave code doesn't build the ->kid pointers
	 * correctly we need to create them here. First we invalidate
	 * the sfind() cache and then call the code used by dinsert()
	 * to update d->kid and d->siblings.
	 */
	s->ser2dsize = 0;
	for (i = 1; i < s->nextserial; i++) {
		unless (d = sfind(s, i)) continue;
		d->kid = d->siblings = 0;
		sccs_kidlink(d);
	}
	sccs_renumber(s, SILENT, spinners);

	unless (f = fopen(sccs_Xfile(s, 'x'), "w")) {
		perror(sccs_Xfile(s, 'x'));
		return (-1);
	}
	s->state |= S_ZFILE|S_PFILE;
	if (delta_table(s, f, 0)) {
		perror("table");
		fclose(f);
		return (-1);
	}
	if (sccs_csetPatchWeave(s, f)) return (-1);
	fseek(f, 0L, SEEK_SET);
	fprintf(f, "\001H%05u\n", s->cksum);
	if (fclose(f)) perror(sccs_Xfile(s, 'x'));

	sccs_close(s);
	if (rename(sccs_Xfile(s, 'x'), s->sfile)) {
		perror(s->sfile);
		return (-1);
	}
	return (0);
}
