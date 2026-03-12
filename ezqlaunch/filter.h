/*
    qlaunch - filter.h
    Filter configuration and per-server predicate.
*/

#ifndef FILTER_H
#define FILTER_H

#include <windows.h>
#include "serverlist.h"

typedef struct _FILTER_STATE {
    BOOL  hideEmpty;        /* hide servers with 0 players         */
    BOOL  hideFull;         /* hide servers at maxplayers           */
    BOOL  hideNotEmpty;     /* hide servers that have any players   */
    BOOL  hideHighPing;     /* hide servers above pingLimit         */
    int   pingLimit;        /* max ping to show (when hideHighPing) */
    BOOL  filterMap;        /* enable map-name filter               */
    char  mapFilter[MAX_MAPNAME]; /* substring to match in map name */
    BOOL  tfOnly;           /* only show servers with gamedir=fortress */
} FILTER_STATE;

/*
 * Default filter: hide empty and dead servers only.
 */
extern FILTER_STATE g_filters;

/* Returns TRUE if the server should be SHOWN given the current filter state. */
BOOL Filter_Pass(const SERVER_ENTRY *e, const FILTER_STATE *f);

/* Forward declaration - include conf.h before filter.h for the full type. */
struct __conf;

/* Load / save filter settings via the CONF system. */
void Filter_Load(FILTER_STATE *f, struct __conf *conf);
void Filter_Save(const FILTER_STATE *f, struct __conf *conf);

/* Re-evaluate passesFilter on every entry in g_servers. */
void Filter_ApplyAll(const FILTER_STATE *f);

#endif /* FILTER_H */
