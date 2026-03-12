/*
    ezqlaunch - serverlist.c
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "serverlist.h"

SERVER_ENTRY     g_servers[MAX_SERVERS];
int              g_serverCount = 0;
CRITICAL_SECTION g_listLock;

void SL_Init(void) {
    InitializeCriticalSection(&g_listLock);
    memset(g_servers, 0, sizeof(g_servers));
    g_serverCount = 0;
}

void SL_Destroy(void) {
    DeleteCriticalSection(&g_listLock);
}

void SL_Clear(void) {
    EnterCriticalSection(&g_listLock);
    memset(g_servers, 0, sizeof(g_servers));
    g_serverCount = 0;
    LeaveCriticalSection(&g_listLock);
}

int SL_AddAddr(const char *addr) {
    int idx = -1;
    EnterCriticalSection(&g_listLock);
    if (g_serverCount < MAX_SERVERS) {
        idx = g_serverCount++;
        memset(&g_servers[idx], 0, sizeof(SERVER_ENTRY));
        strncpy(g_servers[idx].addr, addr, sizeof(g_servers[idx].addr) - 1);
        g_servers[idx].state = SRV_UNKNOWN;
        g_servers[idx].ping  = 999;
    }
    LeaveCriticalSection(&g_listLock);
    return idx;
}

BOOL SL_LoadCache(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[512];
    int   ver;
    long  ts;

    if (!f) return FALSE;

    /* Header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return FALSE; }
    if (sscanf(line, "QLAUNCH_CACHE %d %ld", &ver, &ts) != 2 || ver != CACHE_VERSION) {
        fclose(f);
        return FALSE;
    }

    EnterCriticalSection(&g_listLock);
    g_serverCount = 0;

    while (fgets(line, sizeof(line), f) && g_serverCount < MAX_SERVERS) {
        SERVER_ENTRY *e = &g_servers[g_serverCount];
        char addr[22], hostname[MAX_HOSTNAME], map[MAX_MAPNAME], gamedir[MAX_GAMEDIR];
        int  players, maxplayers, ping, fraglimit, timelimit;

        /* Strip trailing newline */
        char *nl = strrchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strrchr(line, '\r');
        if (nl) *nl = '\0';

        if (sscanf(line, "%21[^\t]\t%63[^\t]\t%31[^\t]\t%15[^\t]\t%d\t%d\t%d\t%d\t%d",
                   addr, hostname, map, gamedir,
                   &players, &maxplayers, &ping, &fraglimit, &timelimit) < 1) continue;

        memset(e, 0, sizeof(*e));
        strncpy(e->addr,     addr,     sizeof(e->addr)     - 1);
        strncpy(e->hostname, hostname, sizeof(e->hostname) - 1);
        strncpy(e->map,      map,      sizeof(e->map)      - 1);
        strncpy(e->gamedir,  gamedir,  sizeof(e->gamedir)  - 1);
        e->players    = players;
        e->maxplayers = maxplayers;
        e->ping       = ping;
        e->fraglimit  = fraglimit;
        e->timelimit  = timelimit;
        /* Entries from cache are SRV_ALIVE so they show immediately */
        e->state      = (ping < 999) ? SRV_ALIVE : SRV_DEAD;

        g_serverCount++;
    }

    LeaveCriticalSection(&g_listLock);
    fclose(f);
    return TRUE;
}

BOOL SL_SaveCache(const char *path) {
    FILE *f = fopen(path, "w");
    int   i;
    time_t ts = time(NULL);

    if (!f) return FALSE;

    fprintf(f, "QLAUNCH_CACHE %d %ld\n", CACHE_VERSION, (long)ts);

    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++) {
        const SERVER_ENTRY *e = &g_servers[i];
        if (e->state == SRV_UNKNOWN) continue; /* don't cache unqueried entries */
        fprintf(f, "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n",
                e->addr, e->hostname, e->map, e->gamedir,
                e->players, e->maxplayers, e->ping,
                e->fraglimit, e->timelimit);
    }
    LeaveCriticalSection(&g_listLock);

    fclose(f);
    return TRUE;
}

FILETIME SL_CacheModTime(const char *path) {
    FILETIME ft = {0, 0};
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
        ft = fa.ftLastWriteTime;
    return ft;
}

/* -----------------------------------------------------------------------
   Simple one-addr-per-line file helpers
   ----------------------------------------------------------------------- */

/* Read a file of "ip:port" lines into addrs[maxn][22].
   Returns count read. */
static int ReadAddrFile(const char *path, char addrs[][22], int maxn) {
    FILE *f = fopen(path, "r");
    char  line[32];
    int   n = 0;
    if (!f) return 0;
    while (n < maxn && fgets(line, sizeof(line), f)) {
        int len;
        /* strip newline */
        char *nl = strrchr(line, '\n'); if (nl) *nl = '\0';
        nl = strrchr(line, '\r'); if (nl) *nl = '\0';
        len = (int)strlen(line);
        if (len < 9 || len > 21) continue;  /* sanity: "1.1.1.1:1" min */
        strncpy(addrs[n], line, 21); addrs[n][21] = '\0';
        n++;
    }
    fclose(f);
    return n;
}

/* Write addrs[n][22] to file, one per line. */
static void WriteAddrFile(const char *path, char addrs[][22], int n) {
    FILE *f = fopen(path, "w");
    int   i;
    if (!f) return;
    for (i = 0; i < n; i++)
        fprintf(f, "%s\n", addrs[i]);
    fclose(f);
}

/* -----------------------------------------------------------------------
   Favorites
   ----------------------------------------------------------------------- */

/* In-memory favorites list */
static char g_favAddrs[MAX_SERVERS][22];
static int  g_favCount = 0;

void SL_LoadFavorites(void) {
    int i, j;
    g_favCount = ReadAddrFile(FAVORITES_FILE, g_favAddrs, MAX_SERVERS);
    /* Mark matching g_servers entries */
    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++) {
        g_servers[i].isFavorite = FALSE;
        for (j = 0; j < g_favCount; j++) {
            if (_stricmp(g_servers[i].addr, g_favAddrs[j]) == 0) {
                g_servers[i].isFavorite = TRUE;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_listLock);
}

void SL_SaveFavorites(void) {
    WriteAddrFile(FAVORITES_FILE, g_favAddrs, g_favCount);
}

BOOL SL_ToggleFavorite(const char *addr) {
    int i;
    /* Check if already a favorite */
    for (i = 0; i < g_favCount; i++) {
        if (_stricmp(g_favAddrs[i], addr) == 0) {
            /* Remove: shift array down */
            int j;
            for (j = i; j < g_favCount - 1; j++)
                strncpy(g_favAddrs[j], g_favAddrs[j+1], 21);
            g_favCount--;
            SL_SaveFavorites();
            /* Update g_servers flag */
            EnterCriticalSection(&g_listLock);
            for (j = 0; j < g_serverCount; j++)
                if (_stricmp(g_servers[j].addr, addr) == 0)
                    g_servers[j].isFavorite = FALSE;
            LeaveCriticalSection(&g_listLock);
            return FALSE;
        }
    }
    /* Add */
    if (g_favCount < MAX_SERVERS) {
        strncpy(g_favAddrs[g_favCount], addr, 21);
        g_favAddrs[g_favCount][21] = '\0';
        g_favCount++;
        SL_SaveFavorites();
        EnterCriticalSection(&g_listLock);
        for (i = 0; i < g_serverCount; i++)
            if (_stricmp(g_servers[i].addr, addr) == 0)
                g_servers[i].isFavorite = TRUE;
        LeaveCriticalSection(&g_listLock);
        return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   History  (format: "ip:port\tYYYY-MM-DD HH:MM" per line)
   ----------------------------------------------------------------------- */

/* Get current local time as "YYYY-MM-DD HH:MM" */
static void GetTimestamp(char *buf, int buflen) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(buf, buflen - 1, "%04d-%02d-%02d %02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    buf[buflen - 1] = '\0';
}

void SL_AddHistory(const char *addr) {
    /* Read existing entries */
    char  histAddr[MAX_HISTORY][22];
    char  histTime[MAX_HISTORY][20];
    int   n = 0, i;
    char  now[20];
    FILE *f;

    /* Load existing */
    n = SL_LoadHistoryEx(histAddr, histTime, MAX_HISTORY);

    /* Remove duplicate if present */
    for (i = 0; i < n; i++) {
        if (_stricmp(histAddr[i], addr) == 0) {
            int j;
            for (j = i; j < n - 1; j++) {
                strncpy(histAddr[j], histAddr[j+1], 21);
                strncpy(histTime[j], histTime[j+1], 19);
            }
            n--;
            break;
        }
    }

    /* Prepend to front (most recent first) */
    if (n >= MAX_HISTORY) n = MAX_HISTORY - 1;
    memmove(histAddr[1], histAddr[0], sizeof(char[22]) * (size_t)n);
    memmove(histTime[1], histTime[0], sizeof(char[20]) * (size_t)n);
    strncpy(histAddr[0], addr, 21); histAddr[0][21] = '\0';
    GetTimestamp(now, sizeof(now));
    strncpy(histTime[0], now, 19); histTime[0][19] = '\0';
    n++;

    /* Write back */
    f = fopen(HISTORY_FILE, "w");
    if (f) {
        for (i = 0; i < n; i++)
            fprintf(f, "%s\t%s\n", histAddr[i], histTime[i]);
        fclose(f);
    }
}

int SL_LoadHistory(char addrs[][22], int maxn) {
    char dummy[MAX_HISTORY][20];
    /* re-use extended loader, discard timestamps */
    return SL_LoadHistoryEx(addrs, dummy, maxn);
}

int SL_LoadHistoryEx(char addrs[][22], char timestamps[][20], int maxn) {
    FILE *f = fopen(HISTORY_FILE, "r");
    char  line[64];
    int   n = 0;
    if (!f) return 0;
    while (n < maxn && fgets(line, sizeof(line), f)) {
        char *tab, *nl;
        /* strip newline */
        nl = strrchr(line, '\n'); if (nl) *nl = '\0';
        nl = strrchr(line, '\r'); if (nl) *nl = '\0';
        tab = strchr(line, '\t');
        if (tab) {
            int alen = (int)(tab - line);
            if (alen < 9 || alen > 21) continue;
            strncpy(addrs[n], line, 21);  addrs[n][alen < 21 ? alen : 21] = '\0';
            strncpy(timestamps[n], tab + 1, 19); timestamps[n][19] = '\0';
        } else {
            /* Legacy: addr-only line */
            int len = (int)strlen(line);
            if (len < 9 || len > 21) continue;
            strncpy(addrs[n], line, 21); addrs[n][21] = '\0';
            timestamps[n][0] = '\0';
        }
        n++;
    }
    fclose(f);
    return n;
}

/* -----------------------------------------------------------------------
   Never-scan list
   ----------------------------------------------------------------------- */

static char g_neverAddrs[MAX_SERVERS][22];
static int  g_neverCount = 0;

void SL_LoadNeverScan(void) {
    int i, j;
    g_neverCount = ReadAddrFile(NEVERSCAN_FILE, g_neverAddrs, MAX_SERVERS);
    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++) {
        g_servers[i].neverPing = FALSE;
        for (j = 0; j < g_neverCount; j++) {
            if (_stricmp(g_servers[i].addr, g_neverAddrs[j]) == 0) {
                g_servers[i].neverPing = TRUE;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_listLock);
}

BOOL SL_ToggleNeverScan(const char *addr) {
    int i;
    for (i = 0; i < g_neverCount; i++) {
        if (_stricmp(g_neverAddrs[i], addr) == 0) {
            int j;
            for (j = i; j < g_neverCount - 1; j++)
                strncpy(g_neverAddrs[j], g_neverAddrs[j+1], 21);
            g_neverCount--;
            WriteAddrFile(NEVERSCAN_FILE, g_neverAddrs, g_neverCount);
            EnterCriticalSection(&g_listLock);
            for (j = 0; j < g_serverCount; j++)
                if (_stricmp(g_servers[j].addr, addr) == 0)
                    g_servers[j].neverPing = FALSE;
            LeaveCriticalSection(&g_listLock);
            return FALSE;
        }
    }
    if (g_neverCount < MAX_SERVERS) {
        strncpy(g_neverAddrs[g_neverCount], addr, 21);
        g_neverAddrs[g_neverCount][21] = '\0';
        g_neverCount++;
        WriteAddrFile(NEVERSCAN_FILE, g_neverAddrs, g_neverCount);
        EnterCriticalSection(&g_listLock);
        for (i = 0; i < g_serverCount; i++)
            if (_stricmp(g_servers[i].addr, addr) == 0)
                g_servers[i].neverPing = TRUE;
        LeaveCriticalSection(&g_listLock);
        return TRUE;
    }
    return FALSE;
}

void SL_RemoveNeverScan(const char *addr) {
    int i, j;
    for (i = 0; i < g_neverCount; i++) {
        if (_stricmp(g_neverAddrs[i], addr) == 0) {
            for (j = i; j < g_neverCount - 1; j++)
                strncpy(g_neverAddrs[j], g_neverAddrs[j+1], 21);
            g_neverCount--;
            WriteAddrFile(NEVERSCAN_FILE, g_neverAddrs, g_neverCount);
            EnterCriticalSection(&g_listLock);
            for (j = 0; j < g_serverCount; j++)
                if (_stricmp(g_servers[j].addr, addr) == 0)
                    g_servers[j].neverPing = FALSE;
            LeaveCriticalSection(&g_listLock);
            return;
        }
    }
}

void SL_ClearNeverScan(void) {
    int i;
    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++)
        g_servers[i].neverPing = FALSE;
    LeaveCriticalSection(&g_listLock);
    g_neverCount = 0;
    WriteAddrFile(NEVERSCAN_FILE, g_neverAddrs, 0);
}

int SL_GetNeverCount(void) {
    return g_neverCount;
}

const char *SL_GetNeverAddr(int i) {
    if (i < 0 || i >= g_neverCount) return NULL;
    return g_neverAddrs[i];
}

/* Add addr to favorites only if not already present. Returns TRUE if added. */
BOOL SL_AddFavoriteAddr(const char *addr) {
    int i;
    for (i = 0; i < g_favCount; i++)
        if (_stricmp(g_favAddrs[i], addr) == 0) return FALSE; /* already present */
    if (g_favCount < MAX_SERVERS) {
        strncpy(g_favAddrs[g_favCount], addr, 21);
        g_favAddrs[g_favCount][21] = '\0';
        g_favCount++;
        SL_SaveFavorites();
        EnterCriticalSection(&g_listLock);
        for (i = 0; i < g_serverCount; i++)
            if (_stricmp(g_servers[i].addr, addr) == 0)
                g_servers[i].isFavorite = TRUE;
        LeaveCriticalSection(&g_listLock);
        return TRUE;
    }
    return FALSE;
}

/* Re-stamp isFavorite on g_servers[] from the in-memory g_favAddrs[] list.
   Call this any time g_servers[] is rebuilt (e.g. after a full scan clears it)
   so that favorites flags survive SL_Clear() + re-population. */
void SL_ReapplyFavorites(void) {
    int i, j;
    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++) {
        g_servers[i].isFavorite = FALSE;
        for (j = 0; j < g_favCount; j++) {
            if (_stricmp(g_servers[i].addr, g_favAddrs[j]) == 0) {
                g_servers[i].isFavorite = TRUE;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_listLock);
}

int SL_GetFavCount(void) {
    return g_favCount;
}

const char *SL_GetFavAddr(int i) {
    if (i < 0 || i >= g_favCount) return NULL;
    return g_favAddrs[i];
}
