#ifndef	_GRAPH_H
#define	_GRAPH_H

#define	SD_MERGE	0x01	// merge left-as-a-list; right is empty

typedef int	(*walkfcn)(sccs *s, ser_t d, void *token);

int	graph_v1(sccs *s);	/* when done, graph is in v1 form */
int	graph_v2(sccs *s);	/* when done, graph is in v2 form */

int	graph_symdiff(sccs *s, ser_t left, ser_t right, ser_t *list,
	    u8 *slist, ser_t **sd, int count, int flags);
ser_t	**graph_sccs2symdiff(sccs *s);
int	graph_kidwalk(sccs *s, walkfcn toTip, walkfcn toRoot, void *token);
void	graph_sortLines(sccs *s, ser_t *list);

void	symdiff_setParent(sccs *s, ser_t d, ser_t new, ser_t **sd);
ser_t	*symdiff_noDup(ser_t *list);
ser_t	*symdiff_addBVC(ser_t **sd, ser_t *list, sccs *s, ser_t d);

int	graph_checkdups(sccs *s);

#endif
