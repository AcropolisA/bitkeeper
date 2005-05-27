#include "system.h"
#include "sccs.h"

int
gethost_main(int ac, char **av)
{
	char 	*host;

	if (ac == 2 && streq("-r", av[1])) {
		host = sccs_realhost();
	} else {
		host = sccs_gethost();
	}
	unless (host && *host) return (1);
	printf("%s\n", host);
	/* make sure we have a good domain name */
	unless (strchr(host, '.')) return (1);
	return (0);
}
