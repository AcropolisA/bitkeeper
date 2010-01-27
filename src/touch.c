#include "sccs.h"

typedef struct opts {
	u32	atime:1;		/* change access time */
	u32	create:1;		/* create file if it doesn't exist */
	u32	mtime:1;		/* change the modification time */
	char	*file;			/* touch -r (stat other file) */
	char	*tspec;			/* touch -t <timespec> */
} opts;

int
touch_main(int ac, char **av)
{
	int	c, i, fd, rval = 0;
	char	*fn, *err;
	opts	opts;
	struct	utimbuf	ut;
	struct	stat	sb;

	bzero(&opts, sizeof(opts));
	opts.create = 1;

	while ((c = getopt(ac, av, "acmr:t:", 0)) != -1) {
		switch (c) {
			case 'a': opts.atime  = 1;	break;
			case 'c': opts.create = 0;	break;
			case 'm': opts.mtime  = 1;	break;
			case 'r': opts.file   = optarg;	break;
			case 't': opts.tspec  = optarg;	break;
			default: bk_badArg(c, av);
		}
	}
	unless (av[optind]) usage();
	if (opts.tspec && opts.file) usage();
	unless (opts.atime || opts.mtime) opts.atime = opts.mtime = 1;
	if (opts.tspec) {
		ut.actime = ut.modtime = strtoul(opts.tspec, 0, 10);
	} else if (opts.file) {
		if (stat(opts.file, &sb)) {
			perror("bk touch:");
			return (1);
		}
		ut.actime = sb.st_atime;
		ut.modtime = sb.st_mtime;
	} else {
		ut.actime = ut.modtime = time(0);
	}
	for (i = optind; fn = av[i]; i++) {
		if (stat(fn, &sb)) {
			unless (opts.create) continue;
			fd = open(fn, O_WRONLY | O_CREAT, 0644);
			if ((fd == -1) || fstat(fd, &sb) || close(fd)) {
				rval = 1;
				err = aprintf("bk touch: "
				    "cannot touch '%s':", fn);
				perror(err);
				continue;
			}
			unless (opts.tspec || opts.file) continue;
		}
		unless (opts.atime) ut.actime = sb.st_atime;
		unless (opts.mtime) ut.modtime = sb.st_mtime;
		if (utime(fn, &ut)) {
			perror("bk touch:");
			rval =1;
		}
	}
	return (rval);
}
