/*
    qlaunch - serverlist.h
    Server entry data structure, thread-safe list management, and disk cache.
*/

#ifndef SERVERLIST_H
#define SERVERLIST_H

#include <windows.h>

#include "qwquery.h"

#define MAX_SERVERS         2048
#define MAX_HOSTNAME        64
#define MAX_MAPNAME         32
#define MAX_GAMEDIR         16
#define CACHE_FILE          "servers.cache"
#define FAVORITES_FILE      "favorites.txt"
#define HISTORY_FILE        "history.txt"
#define NEVERSCAN_FILE      "neverscan.txt"
#define CACHE_VERSION       1
#define MAX_HISTORY         128   /* max history entries kept on disk */

/*
 * Describes one game server. Fields filled in two passes:
 *   Pass 1 (master query): addr populated, everything else zeroed.
 *   Pass 2 (ping/query):   all fields populated, state = SRV_ALIVE or SRV_DEAD.
 */
typedef enum {
    SRV_UNKNOWN = 0,   /* came from master, not yet pinged */
    SRV_ALIVE,         /* queried successfully              */
    SRV_DEAD           /* timed out / unreachable           */
} SRV_STATE;

typedef struct _SERVER_ENTRY {
    char    addr[22];           /* "ip:port\0"                 */
    char    hostname[MAX_HOSTNAME];
    char    map[MAX_MAPNAME];
    char    gamedir[MAX_GAMEDIR];
    int     players;
    int     maxplayers;
    int     ping;               /* ms, 999 = unreachable       */
    int     fraglimit;
    int     timelimit;
    SRV_STATE state;
    BOOL    passesFilter;       /* cached filter result        */
    BOOL    neverPing;          /* skip during scans           */
    BOOL    isFavorite;         /* in favorites list           */
    char    lastPlayed[20];     /* "YYYY-MM-DD HH:MM" or ""    */
    /* Player list (populated by QW_QueryServer) */
    int       numplayers_detail;
    QW_PLAYER players_detail[QW_MAX_PLAYERS];

    /* Raw key/value pairs from the last status response */
    QW_KV     kvpairs[QW_MAX_KEYS];
    int       numkvpairs;
} SERVER_ENTRY;

/*
 * Global server list.  Written by the ping worker thread, read by the
 * dialog / UI thread.  Access must be guarded by g_listLock.
 */
extern SERVER_ENTRY  g_servers[MAX_SERVERS];
extern int           g_serverCount;   /* number of valid entries      */
extern CRITICAL_SECTION g_listLock;

/* Initialise / destroy the critical section. Call once at startup/shutdown. */
void SL_Init(void);
void SL_Destroy(void);

/* Clear all entries (lock must NOT be held by caller). */
void SL_Clear(void);

/*
 * Add a bare address entry (from master query).
 * Returns the index assigned, or -1 if the list is full.
 * Thread-safe.
 */
int SL_AddAddr(const char *addr);

/*
 * Load / save the disk cache.
 * Format: text file, first line is "QLAUNCH_CACHE <version> <unix_timestamp>"
 * followed by one tab-separated line per server:
 *   addr \t hostname \t map \t gamedir \t players \t maxplayers \t ping \t fraglimit \t timelimit
 */
BOOL SL_LoadCache(const char *path);
BOOL SL_SaveCache(const char *path);

/* Returns the cache file modification time as a FILETIME, zeroed if missing. */
FILETIME SL_CacheModTime(const char *path);

/* -----------------------------------------------------------------------
   Favorites, History, Never-Scan lists
   Each is stored as a plain text file with one "ip:port" per line.
   ----------------------------------------------------------------------- */

/* Load favorites from disk; sets isFavorite on matching g_servers entries. */
void SL_LoadFavorites(void);
/* Save current favorites to disk. */
void SL_SaveFavorites(void);
/* Toggle favorite status for addr. Returns new state (TRUE = is now favorite). */
BOOL SL_ToggleFavorite(const char *addr);
/* Add addr to favorites without toggling. Returns TRUE if newly added, FALSE if already present. */
BOOL SL_AddFavoriteAddr(const char *addr);
/* Re-stamp isFavorite on g_servers[] from in-memory list (call after SL_Clear + re-populate). */
void SL_ReapplyFavorites(void);
/* Number of favorites currently in memory. */
int  SL_GetFavCount(void);
/* Return the addr string for favorite entry i (or NULL if out of range). */
const char *SL_GetFavAddr(int i);

/* Append addr to history (no-dup, trims to MAX_HISTORY). */
void SL_AddHistory(const char *addr);
/* Load history file into a caller-supplied array of addr strings.
   Returns number of entries loaded (each entry is char[22]). */
int  SL_LoadHistory(char addrs[][22], int maxn);
/* Extended load: also fills timestamps[][20] with "YYYY-MM-DD HH:MM" per entry. */
int  SL_LoadHistoryEx(char addrs[][22], char timestamps[][20], int maxn);

/* Load never-scan list from disk; sets neverPing on matching g_servers entries. */
void SL_LoadNeverScan(void);
/* Toggle never-scan status for addr. Returns new state (TRUE = will be skipped). */
BOOL SL_ToggleNeverScan(const char *addr);
/* Remove a single addr from the never-scan list. */
void SL_RemoveNeverScan(const char *addr);
/* Clear the entire never-scan list from memory and disk. */
void SL_ClearNeverScan(void);
/* Return number of entries in the never-scan list. */
int  SL_GetNeverCount(void);
/* Return the addr string for never-scan entry i (or NULL if out of range). */
const char *SL_GetNeverAddr(int i);

#endif /* SERVERLIST_H */
