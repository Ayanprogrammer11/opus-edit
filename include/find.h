/*
 * find.h – Incremental text search.
 */

#ifndef OPUSEDIT_FIND_H
#define OPUSEDIT_FIND_H

/*
 * Enter incremental-search mode: prompt the user for a query,
 * highlight matches live, and jump to the next match on each
 * keypress. Arrow keys cycle through results.
 */
void find(void);

/*
 * Find-and-replace: prompt for a query and replacement string,
 * then replace all occurrences in the buffer.
 */
void find_replace(void);

#endif /* OPUSEDIT_FIND_H */
