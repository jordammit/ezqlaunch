/*
    ezqlaunch - ping.c

    Worker thread flow:
      1. If SCAN_FULL_UPDATE: call Master_Query() to populate g_servers.
      2. Ping all servers in parallel using a thread pool (PING_THREAD_COUNT
         workers).  Each worker grabs the next unstarted server index from a
         shared atomic counter, queries it, writes the result back, and posts
         WM_APP_SERVERINFO to the dialog so the row appears immediately.
      3. After all workers finish, save cache and post WM_APP_SCAN_DONE.

    Parallelism dramatically reduces wall-clock time: with 30 threads and a
    1-second gamestat timeout, 600 servers finish in ~20 seconds instead of
    10+ minutes.
*/

#define vsnprintf  _vsnprintf
#define snprintf   _snprintf

#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif
#ifndef _MBCS
#define _MBCS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ping.h"
#include "master.h"
#include "serverlist.h"
#include "filter.h"
#include "qwquery.h"
#include "resource.h"

/* Number of concurrent ping worker threads.
   I/O-bound workload: each thread spends most of its time waiting on
   GSQueryServer's network timeout, so more threads = proportionally faster.
   64 is well within Windows thread limits and keeps scan time reasonable. */
#define PING_THREAD_COUNT  64

/* -----------------------------------------------------------------------
   Internal helpers
   ----------------------------------------------------------------------- */

static void PostStatus(HWND hwnd, const char *fmt, ...) {
    if (!hwnd) return;
    {
        char    buf[256];
        char   *msg;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        msg = _strdup(buf);
        if (msg) PostMessage(hwnd, WM_APP_STATUS, 0, (LPARAM)msg);
    }
}

/* Parse "ip:port" into separate host string + port number. */
static int ParseAddr(const char *addr, char *host, size_t hostlen,
                     unsigned short *port) {
    const char *colon = strrchr(addr, ':');
    if (!colon) return 0;
    {
        size_t n = (size_t)(colon - addr);
        if (n >= hostlen) n = hostlen - 1;
        memcpy(host, addr, n);
        host[n] = '\0';
    }
    *port = (unsigned short)atoi(colon + 1);
    return 1;
}

/* Query a single server and fill in a SERVER_ENTRY. Thread-safe. */
static void QueryOneServer(SERVER_ENTRY *e) {
    char           host[64];
    unsigned short port = 0;
    QW_SERVERINFO  info;
    int            i;

    if (!ParseAddr(e->addr, host, sizeof(host), &port)) {
        e->state = SRV_DEAD;
        e->ping  = 999;
        return;
    }

    if (!QW_QueryServer(host, port, 1000, &info)) {
        e->state = SRV_DEAD;
        e->ping  = 999;
        return;
    }

    e->ping       = info.ping;
    e->maxplayers = info.maxplayers;
    e->players    = info.numplayers;
    e->fraglimit  = info.fraglimit;
    e->timelimit  = info.timelimit;
    e->state      = SRV_ALIVE;

    strncpy(e->hostname, info.hostname[0] ? info.hostname : e->addr,
            sizeof(e->hostname) - 1);
    strncpy(e->map,     info.map,     sizeof(e->map)     - 1);
    strncpy(e->gamedir, info.gamedir, sizeof(e->gamedir) - 1);

    /* Copy player detail for the player panel */
    e->numplayers_detail = info.numplayers;
    for (i = 0; i < info.numplayers && i < QW_MAX_PLAYERS; i++)
        e->players_detail[i] = info.players[i];

    /* Copy raw key/value pairs for the server-info panel */
    e->numkvpairs = info.numkvpairs;
    {
        int k;
        for (k = 0; k < info.numkvpairs && k < QW_MAX_KEYS; k++)
            e->kvpairs[k] = info.kvpairs[k];
    }
}

/* -----------------------------------------------------------------------
   Thread pool
   ----------------------------------------------------------------------- */

/*
 * Shared state for the pool.  Workers read nextIndex (interlocked) to
 * claim the next server slot to query.
 */
typedef struct {
    PING_CONTEXT *ctx;
    int           total;
    volatile LONG nextIndex;   /* interlocked counter */
} POOL_STATE;

static DWORD WINAPI PingWorker(LPVOID param) {
    POOL_STATE   *pool = (POOL_STATE*)param;
    PING_CONTEXT *ctx  = pool->ctx;

    for (;;) {
        int          i;
        SERVER_ENTRY local;

        /* Atomically claim the next server index. */
        i = (int)InterlockedIncrement(&pool->nextIndex) - 1;
        if (i >= pool->total) break;
        if (ctx->abort)       break;

        /* Take a local copy of the address. */
        EnterCriticalSection(&g_listLock);
        local = g_servers[i];
        LeaveCriticalSection(&g_listLock);

        /* Skip servers the user has marked as never-ping */
        if (local.neverPing) {
            InterlockedIncrement(&ctx->nDone);
            continue;
        }

        QueryOneServer(&local);
        local.passesFilter = Filter_Pass(&local, &g_filters);

        /* Write result back under lock */
        EnterCriticalSection(&g_listLock);
        g_servers[i] = local;
        LeaveCriticalSection(&g_listLock);

        /* Signal one more server is done — UI timer reads this for progress */
        InterlockedIncrement(&ctx->nDone);
    }
    return 0;
}

/* -----------------------------------------------------------------------
   Main scan thread
   ----------------------------------------------------------------------- */

static DWORD WINAPI ScanThread(LPVOID param) {
    PING_CONTEXT *ctx = (PING_CONTEXT*)param;
    int           total;
    POOL_STATE    pool;
    HANDLE        workers[PING_THREAD_COUNT];
    int           nWorkers, i;

    /* ---- Step 1: populate address list ---- */
    if (ctx->mode == SCAN_FULL_UPDATE) {
        int n = Master_Query(ctx->masterHost, ctx->masterPort, ctx->hwndDlg);
        if (n < 0) {
            PostStatus(ctx->hwndDlg, "Master query failed. Scan aborted.");
            PostMessage(ctx->hwndDlg, WM_APP_SCAN_DONE, 0, 0);
            free(ctx);
            return 1;
        }
    }

    EnterCriticalSection(&g_listLock);
    total = g_serverCount;
    LeaveCriticalSection(&g_listLock);

    if (total == 0) {
        PostStatus(ctx->hwndDlg, "No servers to ping.");
        PostMessage(ctx->hwndDlg, WM_APP_SCAN_DONE, 0, 0);
        free(ctx);
        return 0;
    }

    PostStatus(ctx->hwndDlg, "Pinging %d servers (%d threads)...",
               total, PING_THREAD_COUNT);

    /* ---- Step 2: launch thread pool ---- */
    ctx->nTotal    = (LONG)total;
    ctx->nDone     = 0;
    pool.ctx       = ctx;
    pool.total     = total;
    pool.nextIndex = 0;

    nWorkers = (total < PING_THREAD_COUNT) ? total : PING_THREAD_COUNT;
    for (i = 0; i < nWorkers; i++) {
        workers[i] = CreateThread(NULL, 0, PingWorker, &pool, 0, NULL);
        if (!workers[i]) {
            /* If we can't create a thread, just mark remaining as done. */
            workers[i] = NULL;
        }
    }

    /* Wait for all workers to finish. */
    for (i = 0; i < nWorkers; i++) {
        if (workers[i]) {
            WaitForSingleObject(workers[i], INFINITE);
            CloseHandle(workers[i]);
        }
    }

    /* ---- Step 3: save cache ---- */
    if (!ctx->abort) {
        SL_SaveCache(CACHE_FILE);
        PostStatus(ctx->hwndDlg, "Scan complete. %d servers queried.", total);
    } else {
        PostStatus(ctx->hwndDlg, "Scan aborted.");
    }

    PostMessage(ctx->hwndDlg, WM_APP_SCAN_DONE, 0, 0);
    free(ctx);
    return 0;
}

/* -----------------------------------------------------------------------
   Public API
   ----------------------------------------------------------------------- */

PING_CONTEXT *Ping_CreateContext(HWND hwnd, SCAN_MODE mode,
                                 const char *masterHost,
                                 const char *masterPort,
                                 LONG serial) {
    PING_CONTEXT *ctx = (PING_CONTEXT*)calloc(1, sizeof(PING_CONTEXT));
    if (!ctx) return NULL;
    ctx->hwndDlg = hwnd;
    ctx->mode    = mode;
    ctx->abort   = FALSE;
    ctx->serial  = serial;
    strncpy(ctx->masterHost,
            masterHost ? masterHost : DEFAULT_MASTER_HOST,
            sizeof(ctx->masterHost) - 1);
    strncpy(ctx->masterPort,
            masterPort ? masterPort : DEFAULT_MASTER_PORT,
            sizeof(ctx->masterPort) - 1);
    return ctx;
}

HANDLE Ping_StartScan(PING_CONTEXT *ctx) {
    return CreateThread(NULL, 0, ScanThread, ctx, 0, NULL);
}

void Ping_Abort(PING_CONTEXT *ctx) {
    if (ctx) ctx->abort = TRUE;
}
