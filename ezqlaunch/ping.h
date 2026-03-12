/*
    qlaunch - ping.h
    Background worker thread that queries every server in g_servers,
    filling in hostname/map/players/ping fields and posting results
    back to the dialog window as they arrive.
*/

#ifndef PING_H
#define PING_H

#include <windows.h>

/*
 * Scan mode passed to Ping_StartScan.
 */
typedef enum {
    SCAN_FULL_UPDATE,   /* query master first, then ping everything         */
    SCAN_REFRESH_CACHE  /* use existing g_servers list, just re-ping them   */
} SCAN_MODE;

/*
 * Context block passed to the worker thread.
 * Caller allocates with Ping_CreateContext and must not free it — the
 * worker thread frees it when done.
 */
typedef struct _PING_CONTEXT {
    HWND        hwndDlg;        /* dialog to receive WM_APP_* messages  */
    SCAN_MODE   mode;
    char        masterHost[128];
    char        masterPort[16];
    BOOL        abort;          /* set to TRUE to request early exit    */
    LONG        serial;         /* g_scanSerial value at scan start; stale messages are discarded */
    volatile LONG nTotal;       /* total servers to ping (set by scan thread before workers start) */
    volatile LONG nDone;        /* servers pinged so far (incremented by each worker) */
} PING_CONTEXT;

/*
 * Allocate and populate a PING_CONTEXT.  The caller should keep a pointer
 * only to set abort = TRUE if needed; do not free the block.
 */
PING_CONTEXT *Ping_CreateContext(HWND hwnd, SCAN_MODE mode,
                                 const char *masterHost, const char *masterPort,
                                 LONG serial);

/*
 * Spawn the worker thread.  Returns the thread HANDLE (or NULL on failure).
 * The PING_CONTEXT is owned and freed by the thread.
 */
HANDLE Ping_StartScan(PING_CONTEXT *ctx);

/* Call to signal the running thread to abort as soon as possible. */
void Ping_Abort(PING_CONTEXT *ctx);

#endif /* PING_H */
