/*
    qlaunch - main.c  (rewritten)

    New features over original:
      - Master server query (Full Update button)
      - Re-ping cached list (Refresh button)
      - Progressive list population: servers appear as each ping completes
      - Server list cache (servers.cache): auto-loaded on start, saved after scan
      - Filter dialog: hide empty/full/high-ping/map
      - Add Master dialog: change master host:port at runtime
      - Pipe IPC: when --pipe <name> is given on the command line, writing the
        chosen server address to the pipe replaces launching the engine directly
*/

/* Force narrow (MBCS) character set before any system header.
   In this build _T() was resolving to wide strings; this corrects it. */
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif
#ifndef _MBCS
#define _MBCS
#endif

/* MSVC 2003 CRT: underscore-prefixed names for C99 functions */
#define snprintf  _snprintf
#define vsnprintf _vsnprintf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
/* HDF_SORTUP/HDF_SORTDOWN were added in the Vista SDK.
   Define them manually for MSVC 2003 compatibility. */
#ifndef HDF_SORTUP
#define HDF_SORTUP   0x0400
#endif
#ifndef HDF_SORTDOWN
#define HDF_SORTDOWN 0x0200
#endif
#include "resource.h"
#include "conf.h"
#include "serverlist.h"
#include "filter.h"
#include "ping.h"
#include "qwquery.h"

/* -----------------------------------------------------------------------
   Defaults and constants
   ----------------------------------------------------------------------- */
#define DEFAULT_ENGINE          "ezquake.exe"
#define DEFAULT_ENGINEARGS      ""
#define DEFAULT_MASTER_HOST     "master.quakeworld.nu"
#define DEFAULT_MASTER_PORT     "27000"
#define CONFIG_FILE             "ezqlaunch.conf"

/* ListView column indices for the server list */
#define COL_SRV_NAME    0
#define COL_SRV_ADDR    1
#define COL_SRV_MAP     2
#define COL_SRV_PLAYERS 3
#define COL_SRV_PING    4
#define COL_SRV_LAST    5   /* "Last Played" date/time */

/* ListView column indices for the player list.
   Col 0 (Player) is owner-drawn: color swatches + name in one cell,
   matching GameSpy 3D's combined "Player Name" column.
   COL_PLR_TEAM is a zero-width hidden column that holds the team string
   for sort purposes on fortress servers; it is never displayed. */
#define COL_PLR_NAME    0   /* owner-drawn: swatches left, name text right */
#define COL_PLR_FRAGS   1
#define COL_PLR_TIME    2
#define COL_PLR_PING    3
#define COL_PLR_SKIN    4
#define COL_PLR_TEAM    5   /* hidden; team name for fortress sort */

/* -----------------------------------------------------------------------
   Globals
   ----------------------------------------------------------------------- */
static CONF         *g_conf         = NULL;
static HINSTANCE     g_hInst        = NULL;
static PING_CONTEXT *g_pingCtx      = NULL;   /* NULL when no scan running  */
static HANDLE        g_pingThread   = NULL;
static BOOL          g_scanning     = FALSE;
static char         *g_pipeName     = NULL;   /* non-NULL when in pipe mode */

/* Mapping: ListView item index -> g_servers[] index
   (needed because filtered items are skipped)         */
static int g_itemToServer[MAX_SERVERS];
static int g_itemCount = 0;

/* Sort state for the server ListView */
static int  g_sortCol = -1;   /* -1 = unsorted */
static BOOL g_sortAsc = TRUE;
/* Set TRUE during ListView_SortItemsEx to suppress LVN_ITEMCHANGED */
static BOOL g_sorting = FALSE;

/* Incremented each time a new scan starts; WM_APP_SERVERINFO messages
   carry the serial at post time and are discarded if stale */
static LONG g_scanSerial = 0;

/* Absolute path to the config file — set in WinMain before anything opens files */
static char g_configPath[MAX_PATH] = CONFIG_FILE;

/* Initial client size captured at WM_INITDIALOG for resize calculations */
static int g_initW = 0;
static int g_initH = 0;

/* Splitter drag state (horizontal divider between players and srvinfo) */
static int  g_splitterX  = 0;    /* current splitter x in client pixels */
static BOOL g_splDragging = FALSE;
static int  g_splDragOff  = 0;   /* cursor offset within splitter at drag start */
#define SPLITTER_W  4             /* splitter width in pixels */
#define SPLITTER_MIN_LEFT  80     /* minimum width of player panel */
#define SPLITTER_MIN_RIGHT 80     /* minimum width of srvinfo panel */

/* Tab indices — TAB_NEVERSCAN is dynamic (-1 when hidden) */
#define TAB_ALL       0
#define TAB_FAVORITES 1
#define TAB_HISTORY   2
#define TAB_NEVERSCAN 3
static int g_activeTab = TAB_ALL;
static BOOL g_nvrTabVisible = FALSE;   /* TRUE when Never Ping tab exists */

/* View state */
static BOOL g_hidePanes  = FALSE;      /* Hide bottom panes toggle */

/* Player sort column (-1 = no sort, matches COL_PLR_* indices) */
static int  g_plrSortCol = COL_PLR_FRAGS;  /* default: score descending */
static BOOL g_plrSortAsc = TRUE;

/* Shared server-list column widths — synced across All/Favorites/History/NeverPing tabs.
   Initialised from defaults; updated whenever the user drags a column divider. */
#define NUM_SRV_COLS 6
static int g_srvColWidths[NUM_SRV_COLS] = {240, 140, 90, 65, 50, 140};

/* Address of the server selected before a scan — restored after rebuild */
static char g_savedSelAddr[22] = "";

/* Auto-scan state */
#define AUTO_MODE_OFF     0
#define AUTO_MODE_UPDATE  1   /* periodic Full Update (master + ping) */
#define AUTO_MODE_REFRESH 2   /* periodic Refresh (re-ping cache only) */
static int  g_autoMode     = AUTO_MODE_OFF;
static int  g_autoInterval = 5;   /* minutes between scans */

/* History entries (addr strings loaded from disk) */
#define MAX_HIST_DISPLAY 128
static char g_histAddrs[MAX_HIST_DISPLAY][22];
static int  g_histCount = 0;

/* -----------------------------------------------------------------------
   Utility: TCHAR <-> char helpers
   MBCS build: TCHAR == char, so these are plain strncpy wrappers.
   ----------------------------------------------------------------------- */
static void T2A_safe(const char *t, char *buf, int buflen) {
    strncpy(buf, t, buflen - 1);
    buf[buflen - 1] = '\0';
}

static void A2T_safe(const char *a, char *buf, int buflen) {
    strncpy(buf, a, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* -----------------------------------------------------------------------
   Status bar helpers
   ----------------------------------------------------------------------- */
static void SetStatus(HWND hwnd, const char *msg) {
    HWND hStatus = GetDlgItem(hwnd, IDC_STATUS_BAR);
    if (hStatus) SetWindowTextA(hStatus, msg);
}

static void UpdateFilterSummary(HWND hwnd) {
    char  buf[128];
    int   n = 0;
    HWND  hLbl = GetDlgItem(hwnd, IDC_FILTER_SUMMARY);
    if (!hLbl) return;

    if (g_filters.tfOnly)       n++;
    if (g_filters.hideEmpty)    n++;
    if (g_filters.hideFull)     n++;
    if (g_filters.hideNotEmpty) n++;
    if (g_filters.hideHighPing) n++;
    if (g_filters.filterMap && g_filters.mapFilter[0]) n++;

    if (n == 0)
        strncpy(buf, "Filters: none", sizeof(buf)-1);
    else
        _snprintf(buf, sizeof(buf)-1, "Filters: %d active", n);
    buf[sizeof(buf)-1] = '\0';

    SetWindowTextA(hLbl, buf);
}

/* -----------------------------------------------------------------------
   Server ListView helpers
   ----------------------------------------------------------------------- */

/* Sort forward declarations — used by Rebuild* functions below,
   defined later after QW color table */
static int CALLBACK SrvSortCmp(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);
static void SrvSort_Apply(HWND hList);

/* Strip QW high-bit color encoding from a string — forward decl,
   defined after QW color table */
static void QW_StripName(const char *src, char *dst, int dstlen);

/* Insert or update one server entry in the ListView.
   lvIdx  = ListView item index (use -1 to insert new, >= 0 to update)
   srvIdx = index into g_servers[]
   Returns the ListView item index. */
static int LV_SetServerItem(HWND hList, int lvIdx, int srvIdx) {
    const SERVER_ENTRY *e = &g_servers[srvIdx];
    LVITEM  lvi = {0};
    char    buf[64];   /* MBCS: char == TCHAR, use char directly */

    lvi.mask    = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem   = (lvIdx < 0) ? ListView_GetItemCount(hList) : lvIdx;
    lvi.iSubItem = 0;
    lvi.lParam  = (LPARAM)srvIdx;  /* used by sort comparator */

    /* Col 0: hostname (or addr if hostname empty), QW color codes stripped */
    if (e->hostname[0])
        QW_StripName(e->hostname, buf, 64);
    else
        strncpy(buf, e->addr, 63);
    buf[63] = '\0';
    lvi.pszText = (LPSTR)buf;

    if (lvIdx < 0)
        lvIdx = ListView_InsertItem(hList, &lvi);
    else
        ListView_SetItem(hList, &lvi);

    /* Subitems: lParam is not valid on subitems - clear it from mask */
    lvi.mask = LVIF_TEXT;

    /* Col 1: address */
    strncpy(buf, e->addr, 63); buf[63] = '\0';
    lvi.iSubItem = COL_SRV_ADDR; lvi.pszText = (LPSTR)buf;
    ListView_SetItem(hList, &lvi);

    /* Col 2: map */
    strncpy(buf, e->map[0] ? e->map : "-", 63); buf[63] = '\0';
    lvi.iSubItem = COL_SRV_MAP; lvi.pszText = (LPSTR)buf;
    ListView_SetItem(hList, &lvi);

    /* Col 3: players */
    if (e->maxplayers > 0)
        _snprintf(buf, 63, "%d/%d", e->players, e->maxplayers);
    else if (e->state == SRV_UNKNOWN)
        strncpy(buf, "...", 63);
    else
        _snprintf(buf, 63, "%d", e->players);
    buf[63] = '\0';
    lvi.iSubItem = COL_SRV_PLAYERS; lvi.pszText = (LPSTR)buf;
    ListView_SetItem(hList, &lvi);

    /* Col 4: ping */
    if (e->state == SRV_UNKNOWN)
        strncpy(buf, "...", 63);
    else if (e->state == SRV_DEAD)
        strncpy(buf, "N/A", 63);
    else
        _snprintf(buf, 63, "%d", e->ping);
    buf[63] = '\0';
    lvi.iSubItem = COL_SRV_PING; lvi.pszText = (LPSTR)buf;
    ListView_SetItem(hList, &lvi);

    /* Col 5: last played */
    strncpy(buf, e->lastPlayed[0] ? e->lastPlayed : "-", 63); buf[63] = '\0';
    lvi.iSubItem = COL_SRV_LAST; lvi.pszText = (LPSTR)buf;
    ListView_SetItem(hList, &lvi);

    return lvIdx;
}

/* Rebuild the entire server ListView from g_servers (applies current filters). */
static void RebuildServerList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SERVERS);
    int  i;

    ListView_DeleteAllItems(hList);
    g_itemCount = 0;

    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++) {
        if (g_servers[i].passesFilter) {
            g_itemToServer[g_itemCount] = i;
            LV_SetServerItem(hList, -1, i);
            g_itemCount++;
        }
    }
    LeaveCriticalSection(&g_listLock);

    /* Re-apply current sort order */
    if (g_sortCol >= 0) {
        g_sorting = TRUE;
        ListView_SortItemsEx(hList, SrvSortCmp, (LPARAM)hList);
        SrvSort_Apply(hList);
        g_sorting = FALSE;
    }

    {
        char buf[64];
        _snprintf(buf, 63, "%d servers", g_itemCount); buf[63] = '\0';
        SetStatus(hwnd, buf);
    }
}

/* -----------------------------------------------------------------------
   Player ListView helpers
   ----------------------------------------------------------------------- */
/* TF skin name -> class display name table.
   Unrecognised tf_* skins get the prefix stripped; others shown as-is. */
static const struct { const char *skin; const char *cls; } TF_MAP[] = {
    { "tf_scout",  "scout"    },
    { "airscout",  "scout"    },
    { "tf_snipe",  "sniper"   },
    { "tf_sold",   "soldier"  },
    { "tf_demo",   "demoman"  },
    { "tf_medic",  "medic"    },
    { "tf_pyro",   "pyro"     },
    { "tf_hwguy",  "heavy"    },
    { "tf_spy",    "spy"      },
    { "tf_eng",    "engineer" },
    { NULL, NULL }
};

/* Strip QuakeWorld colored-name encoding from a player name.
   QW sets bit 7 of characters to produce "colored" text in-game.
   Characters 16-27 are colored digit glyphs (map to '0'-'9', '-', ' ').
   Strip the high bit from all bytes to recover plain ASCII. */
static void QW_StripName(const char *src, char *dst, int dstlen) {
    int i = 0;
    while (*src && i < dstlen - 1) {
        unsigned char c = (unsigned char)*src++;
        c &= 0x7F;                      /* strip high bit */
        if (c < 16) c = ' ';            /* non-printable control chars -> space */
        if (c == 127) c = ' ';
        dst[i++] = (char)c;
    }
    /* Trim trailing spaces */
    while (i > 0 && dst[i-1] == ' ') i--;
    dst[i] = '\0';
}


static void RefreshPlayersBySrvIdx(HWND hwnd, int srvIdx) {
    HWND         hPlayers = GetDlgItem(hwnd, IDC_PLAYERS);
    SERVER_ENTRY e;
    int          i;

    ListView_DeleteAllItems(hPlayers);

    if (srvIdx < 0 || srvIdx >= g_serverCount) return;

    /* Take a local snapshot under the lock */
    EnterCriticalSection(&g_listLock);
    e = g_servers[srvIdx];
    LeaveCriticalSection(&g_listLock);

    if (e.state != SRV_ALIVE) return;

    for (i = 0; i < e.numplayers_detail && i < QW_MAX_PLAYERS; i++) {
        const QW_PLAYER *pl = &e.players_detail[i];
        char   buf[64];
        LVITEM lvi = {0};

        /* Col 0: "Player" — owner-drawn (swatches left, name text right).
           Store stripped name as text so the default draw path shows something
           in case customdraw is unavailable; actual painting done in NM_CUSTOMDRAW. */
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iSubItem = 0;
        QW_StripName(pl->name, buf, 64);
        lvi.pszText  = (LPSTR)buf;
        lvi.lParam   = (LPARAM)i;   /* row index for NM_CUSTOMDRAW + sort */
        lvi.iItem    = ListView_InsertItem(hPlayers, &lvi);

        lvi.mask = LVIF_TEXT;

        lvi.iSubItem = COL_PLR_FRAGS;
        _snprintf(buf, 63, "%d", pl->frags); buf[63] = '\0';
        lvi.pszText = (LPSTR)buf;
        ListView_SetItem(hPlayers, &lvi);

        lvi.iSubItem = COL_PLR_TIME;
        _snprintf(buf, 63, "%d", pl->time); buf[63] = '\0';
        lvi.pszText = (LPSTR)buf;
        ListView_SetItem(hPlayers, &lvi);

        lvi.iSubItem = COL_PLR_PING;
        _snprintf(buf, 63, "%d", pl->ping); buf[63] = '\0';
        lvi.pszText = (LPSTR)buf;
        ListView_SetItem(hPlayers, &lvi);

        /* Skin / Class column */
        lvi.iSubItem = COL_PLR_SKIN;
        if (g_filters.tfOnly) {
            const char *skin = pl->skin;
            const char *display = NULL;
            int k;
            for (k = 0; TF_MAP[k].skin; k++) {
                if (_stricmp(skin, TF_MAP[k].skin) == 0) {
                    display = TF_MAP[k].cls;
                    break;
                }
            }
            if (display) {
                strncpy(buf, display, 63);
            } else if (_strnicmp(skin, "tf_", 3) == 0) {
                strncpy(buf, skin + 3, 63);
            } else {
                strncpy(buf, skin, 63);
            }
            buf[63] = '\0';
        } else {
            strncpy(buf, pl->skin, 63); buf[63] = '\0';
        }
        lvi.pszText = (LPSTR)buf;
        ListView_SetItem(hPlayers, &lvi);

        /* Hidden team column (COL_PLR_TEAM) — zero-width, used only for
           fortress-server sorting; never visible to the user. */
        lvi.iSubItem = COL_PLR_TEAM;
        QW_StripName(pl->team, buf, 64);
        lvi.pszText = (LPSTR)buf;
        ListView_SetItem(hPlayers, &lvi);

    }
}

/* Legacy wrapper: lvIdx into g_itemToServer[] (All tab only) */
static void RefreshPlayers(HWND hwnd, int lvIdx) {
    if (lvIdx < 0 || lvIdx >= g_itemCount) {
        ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_PLAYERS));
        return;
    }
    RefreshPlayersBySrvIdx(hwnd, g_itemToServer[lvIdx]);
}

/* Forward declarations (defined later in tab management section) */
static HWND GetActiveServerList(HWND hwnd);
static int  GetContextSrvIdx(HWND hwnd, int lvIdx);
static void RefreshSrvInfo(HWND hwnd, int srvIdx);
static void SortPlayers(HWND hwnd);
static void UpdateMenuCheckmarks(HWND hwnd);
static void UpdateSelectionButtons(HWND hwnd, BOOL hasSelection);

/* Refresh players from whichever server is selected on the active tab */
static void RefreshPlayersFromActiveTab(HWND hwnd) {
    HWND hList  = GetActiveServerList(hwnd);
    int  lvIdx  = ListView_GetSelectionMark(hList);
    int  srvIdx = GetContextSrvIdx(hwnd, lvIdx);
    RefreshPlayersBySrvIdx(hwnd, srvIdx);
    SortPlayers(hwnd);
    RefreshSrvInfo(hwnd, srvIdx);
    /* Enable/disable selection-sensitive buttons */
    UpdateSelectionButtons(hwnd, srvIdx >= 0);
}

/* Enable or disable all buttons that require a server to be selected */
static void UpdateSelectionButtons(HWND hwnd, BOOL hasSelection) {
    EnableWindow(GetDlgItem(hwnd, IDC_CONNECT),            hasSelection);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_FAV_SEL),        hasSelection);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH_SEL_TB), hasSelection);
}

/* After a full rebuild, re-select the server whose address was saved in
   g_savedSelAddr (set in StartScan). No-op if the address is not found. */
static void RestoreSavedSelection(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SERVERS);
    int  i, n;
    if (g_savedSelAddr[0] == '\0') return;
    n = ListView_GetItemCount(hList);
    for (i = 0; i < n; i++) {
        LVITEM lvi = {0};
        lvi.mask  = LVIF_PARAM;
        lvi.iItem = i;
        ListView_GetItem(hList, &lvi);
        if ((int)lvi.lParam >= 0 && (int)lvi.lParam < g_serverCount) {
            if (_stricmp(g_servers[(int)lvi.lParam].addr, g_savedSelAddr) == 0) {
                ListView_SetItemState(hList, i,
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, i, FALSE);
                /* Do NOT clear g_savedSelAddr here — the poll timer calls
                   RebuildServerList every 250ms during a scan, which wipes
                   the selection each time. Keep the saved addr so each
                   rebuild re-applies it. It is cleared by the caller once
                   the scan is fully complete. */
                return;
            }
        }
    }
    /* Server not currently visible (filtered/not-yet-pinged) — leave
       g_savedSelAddr intact so it can be matched once the server appears. */
}


/* Get selected server address from whichever tab is active.
   Returns FALSE if nothing is selected or addr cannot be determined. */
static BOOL GetSelectedAddr(HWND hwnd, char *addr_out, int addrlen) {
    int lvIdx;
    addr_out[0] = '\0';

    if (g_activeTab == TAB_ALL) {
        HWND hList = GetDlgItem(hwnd, IDC_SERVERS);
        lvIdx = ListView_GetSelectionMark(hList);
        if (lvIdx < 0 || lvIdx >= g_itemCount) return FALSE;
        EnterCriticalSection(&g_listLock);
        strncpy(addr_out, g_servers[g_itemToServer[lvIdx]].addr, addrlen - 1);
        LeaveCriticalSection(&g_listLock);
    } else if (g_activeTab == TAB_FAVORITES) {
        HWND   hList = GetDlgItem(hwnd, IDC_SERVERS_FAV);
        LVITEM lvi   = {0};
        char   buf[22];
        lvIdx = ListView_GetSelectionMark(hList);
        if (lvIdx < 0) return FALSE;
        /* Address is in col 1 */
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = lvIdx;
        lvi.iSubItem = COL_SRV_ADDR;
        lvi.pszText  = buf;
        lvi.cchTextMax = sizeof(buf);
        if (!ListView_GetItem(hList, &lvi)) return FALSE;
        strncpy(addr_out, buf, addrlen - 1);
    } else if (g_activeTab == TAB_HISTORY) {
        HWND   hList = GetDlgItem(hwnd, IDC_SERVERS_HIST);
        LVITEM lvi   = {0};
        char   buf[22];
        lvIdx = ListView_GetSelectionMark(hList);
        if (lvIdx < 0) return FALSE;
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = lvIdx;
        lvi.iSubItem = COL_SRV_ADDR;
        lvi.pszText  = buf;
        lvi.cchTextMax = sizeof(buf);
        if (!ListView_GetItem(hList, &lvi)) return FALSE;
        strncpy(addr_out, buf, addrlen - 1);
    }

    addr_out[addrlen - 1] = '\0';
    return (addr_out[0] != '\0');
}

static void DoConnect(HWND hwnd) {
    char addr_a[22];

    if (!GetSelectedAddr(hwnd, addr_a, sizeof(addr_a))) return;

    /* Record in history */
    SL_AddHistory(addr_a);

    /* ---- Pipe mode: write address and close ---- */
    if (g_pipeName) {
        HANDLE hPipe = CreateFileA(g_pipeName, GENERIC_WRITE, 0, NULL,
                                   OPEN_EXISTING, 0, NULL);
        if (hPipe != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hPipe, addr_a, (DWORD)(strlen(addr_a) + 1),
                      &written, NULL);
            CloseHandle(hPipe);
        }
        EndDialog(hwnd, 0);
        return;
    }

    /* ---- Standalone mode: launch engine ---- */
    {
        char        cmdline[512];
        char        engineDir[MAX_PATH];
        char       *dirSep;
        STARTUPINFOA si  = {0};
        PROCESS_INFORMATION pi = {0};
        const char *eng  = ConfGetOpt(g_conf, "engine");
        const char *args = ConfGetOpt(g_conf, "engineargs");
        const char *eng_a  = eng  ? eng  : DEFAULT_ENGINE;
        const char *args_a = args ? args : DEFAULT_ENGINEARGS;

        /* Derive the engine's directory so we can pass it as
           lpCurrentDirectory.  Quake engines locate data files (gfx/,
           id1/, fortress/, etc.) relative to their working directory, not
           the executable path.  When lpCurrentDirectory is NULL the child
           inherits our CWD (the launcher's folder), which is wrong.
           Setting it to the folder that contains the engine exe replicates
           the behaviour of a desktop shortcut with "Start in" set. */
        strncpy(engineDir, eng_a, MAX_PATH - 1);
        engineDir[MAX_PATH - 1] = '\0';
        dirSep = strrchr(engineDir, '\\');
        if (!dirSep) dirSep = strrchr(engineDir, '/');
        if (dirSep)
            *dirSep = '\0';   /* strip exe name, keep directory */
        else
            engineDir[0] = '\0';  /* relative name — use CWD (fallback) */

        /* Build command line.  When lpApplicationName is non-NULL,
           argv[0] is still parsed from lpCommandLine by the CRT, so
           quote the exe path to handle spaces correctly. */
        _snprintf(cmdline, sizeof(cmdline) - 1,
                  "\"%s\" %s +connect %s", eng_a, args_a, addr_a);
        cmdline[sizeof(cmdline)-1] = '\0';

        si.cb = sizeof(si);
        if (!CreateProcessA(eng_a, cmdline, NULL, NULL, FALSE, 0, NULL,
                            engineDir[0] ? engineDir : NULL, &si, &pi)) {
            /* Fallback: try with lpApplicationName=NULL (lets Windows search PATH) */
            CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL,
                           engineDir[0] ? engineDir : NULL, &si, &pi);
        }
        if (pi.hProcess) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
}

/* -----------------------------------------------------------------------
   Scan management
   ----------------------------------------------------------------------- */
static void StopScan(HWND hwnd) {
    KillTimer(hwnd, IDT_SCAN_POLL);
    if (g_pingCtx) {
        Ping_Abort(g_pingCtx);
        g_pingCtx = NULL;
    }
    if (g_pingThread) {
        WaitForSingleObject(g_pingThread, 5000);
        CloseHandle(g_pingThread);
        g_pingThread = NULL;
    }
    g_scanning = FALSE;
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE_MASTER), TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH_CACHE), TRUE);
}

static void StartScan(HWND hwnd, SCAN_MODE mode) {
    char masterHost[128], masterPort[16];
    const char *mh, *mp;

    if (g_scanning) StopScan(hwnd);

    mh = ConfGetOpt(g_conf, "master_host");
    mp = ConfGetOpt(g_conf, "master_port");
    T2A_safe(mh ? mh : DEFAULT_MASTER_HOST, masterHost, sizeof(masterHost));
    T2A_safe(mp ? mp : DEFAULT_MASTER_PORT, masterPort, sizeof(masterPort));

    /* Increment serial first so the new context and the handler share the same value */
    InterlockedIncrement(&g_scanSerial);

    g_pingCtx = Ping_CreateContext(hwnd, mode, masterHost, masterPort, g_scanSerial);
    if (!g_pingCtx) return;

    g_pingThread = Ping_StartScan(g_pingCtx);
    if (!g_pingThread) {
        free(g_pingCtx);
        g_pingCtx  = NULL;
        return;
    }

    g_scanning = TRUE;

    /* Save currently selected server address BEFORE clearing g_itemCount,
       because GetSelectedAddr uses g_itemCount for bounds checking. */
    g_savedSelAddr[0] = '\0';
    {
        char tmp[22];
        if (GetSelectedAddr(hwnd, tmp, sizeof(tmp)))
            strncpy(g_savedSelAddr, tmp, sizeof(g_savedSelAddr) - 1);
    }

    g_itemCount = 0;

    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_SERVERS));
    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_PLAYERS));
    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_SRVINFO));

    EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE_MASTER), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH_CACHE), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CONNECT), FALSE);
    UpdateSelectionButtons(hwnd, FALSE);

    {
        HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
        SendMessage(hProg, PBM_SETRANGE32, 0, 100);
        SendMessage(hProg, PBM_SETPOS, 0, 0);
        ShowWindow(hProg, SW_SHOW);
    }

    /* Poll g_servers[] every 250ms to update the list progressively */
    SetTimer(hwnd, IDT_SCAN_POLL, 250, NULL);
}

/* Forward declaration - defined after EngineDlgProc */
static void UpdatePlayerSkinHeader(HWND hwnd);

/* -----------------------------------------------------------------------
   Filter dialog
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK FilterDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            CheckDlgButton(hwnd, IDC_FILTER_TF_ONLY,      g_filters.tfOnly       ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_FILTER_HIDE_EMPTY,   g_filters.hideEmpty    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_FILTER_HIDE_FULL,    g_filters.hideFull     ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_FILTER_HIDE_NOTEMPTY,g_filters.hideNotEmpty ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_FILTER_HIDE_HIGHPING,g_filters.hideHighPing ? BST_CHECKED : BST_UNCHECKED);
            {
                char tmp[16];
                _snprintf(tmp, sizeof(tmp)-1, "%d", g_filters.pingLimit);
                tmp[sizeof(tmp)-1] = '\0';
                SetDlgItemTextA(hwnd, IDC_FILTER_PING_LIMIT, tmp);
            }
            SetDlgItemTextA(hwnd, IDC_FILTER_MAP, g_filters.mapFilter);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_FILTER_OK: {
                    char tmp[64];
                    HWND hwndMain = GetParent(hwnd);
                    BOOL tfWas    = g_filters.tfOnly;

                    g_filters.tfOnly       = (IsDlgButtonChecked(hwnd, IDC_FILTER_TF_ONLY)       == BST_CHECKED);
                    g_filters.hideEmpty    = (IsDlgButtonChecked(hwnd, IDC_FILTER_HIDE_EMPTY)    == BST_CHECKED);
                    g_filters.hideFull     = (IsDlgButtonChecked(hwnd, IDC_FILTER_HIDE_FULL)     == BST_CHECKED);
                    g_filters.hideNotEmpty = (IsDlgButtonChecked(hwnd, IDC_FILTER_HIDE_NOTEMPTY) == BST_CHECKED);
                    g_filters.hideHighPing = (IsDlgButtonChecked(hwnd, IDC_FILTER_HIDE_HIGHPING) == BST_CHECKED);

                    GetDlgItemTextA(hwnd, IDC_FILTER_PING_LIMIT, tmp, 16);
                    g_filters.pingLimit = atoi(tmp);
                    if (g_filters.pingLimit <= 0) g_filters.pingLimit = 150;

                    GetDlgItemTextA(hwnd, IDC_FILTER_MAP, g_filters.mapFilter, MAX_MAPNAME);
                    g_filters.mapFilter[MAX_MAPNAME-1] = '\0';
                    g_filters.filterMap = (g_filters.mapFilter[0] != '\0');

                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);

                    /* If TF Only changed, rename the skin/class column header */
                    if (hwndMain && tfWas != g_filters.tfOnly)
                        UpdatePlayerSkinHeader(hwndMain);

                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDC_FILTER_CANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   Add Master dialog
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK AddMasterDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            const char *mh = ConfGetOpt(g_conf, "master_host");
            const char *mp = ConfGetOpt(g_conf, "master_port");
            SetDlgItemTextA(hwnd, IDC_MASTER_HOST, mh ? mh : DEFAULT_MASTER_HOST);
            SetDlgItemTextA(hwnd, IDC_MASTER_PORT, mp ? mp : DEFAULT_MASTER_PORT);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_MASTER_OK: {
                    char host[128], port[16];
                    GetDlgItemTextA(hwnd, IDC_MASTER_HOST, host, 128);
                    GetDlgItemTextA(hwnd, IDC_MASTER_PORT, port,  16);
                    if (strlen(host) == 0) {
                        MessageBoxA(hwnd, "Please enter a master server hostname.",
                                   "qlaunch", MB_ICONWARNING);
                        return TRUE;
                    }
                    ConfSetOpt(g_conf, "master_host", host);
                    ConfSetOpt(g_conf, "master_port", port[0] ? port : "27000");
                    ConfSave(g_conf, g_configPath);
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDC_MASTER_CANCEL:
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   Main dialog initialisation helpers
   ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
   Server list sorting
   ----------------------------------------------------------------------- */

/* ListView_SortItemsEx callback.  lParam1/lParam2 are ListView item indices. */
static int CALLBACK SrvSortCmp(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
    HWND              hList = (HWND)lParamSort;
    const SERVER_ENTRY *a, *b;
    int               srvA, srvB, r = 0;
    LVITEM            lvi = {0};

    /* Fetch the stored srvIdx from each item's lParam */
    lvi.mask  = LVIF_PARAM;
    lvi.iItem = (int)lParam1;
    ListView_GetItem(hList, &lvi);
    srvA = (int)lvi.lParam;

    lvi.iItem = (int)lParam2;
    ListView_GetItem(hList, &lvi);
    srvB = (int)lvi.lParam;

    if (srvA < 0 || srvA >= MAX_SERVERS) return 0;
    if (srvB < 0 || srvB >= MAX_SERVERS) return 0;

    a = &g_servers[srvA];
    b = &g_servers[srvB];

    switch (g_sortCol) {
        case COL_SRV_NAME:
            r = _stricmp(a->hostname[0] ? a->hostname : a->addr,
                         b->hostname[0] ? b->hostname : b->addr);
            break;
        case COL_SRV_ADDR:
            r = strcmp(a->addr, b->addr);
            break;
        case COL_SRV_MAP:
            r = _stricmp(a->map, b->map);
            break;
        case COL_SRV_PLAYERS:
            r = (a->players - b->players);
            if (r == 0) r = (a->maxplayers - b->maxplayers);
            break;
        case COL_SRV_PING:
            r = (a->ping - b->ping);
            break;
        default:
            r = 0;
            break;
    }

    return g_sortAsc ? r : -r;
}

/* After sorting, optionally rebuild g_itemToServer from the new order and
   update the header arrow.  hList must be the server ListView HWND.
   g_itemToServer[] is ONLY valid for IDC_SERVERS (the All tab). */
static void SrvSort_Apply(HWND hList) {
    HWND hHdr;
    int  i, nCols;

    /* Only rebuild g_itemToServer[] when sorting the All-tab list.
       Applying sort to FAV/HIST lists must NOT overwrite this mapping. */
    if (hList == GetDlgItem(GetParent(hList), IDC_SERVERS)) {
        for (i = 0; i < g_itemCount; i++) {
            LVITEM lvi = {0};
            lvi.mask   = LVIF_PARAM;
            lvi.iItem  = i;
            ListView_GetItem(hList, &lvi);
            if ((int)lvi.lParam >= 0 && (int)lvi.lParam < MAX_SERVERS)
                g_itemToServer[i] = (int)lvi.lParam;
        }
    }

    /* Update column header arrows */
    hHdr  = ListView_GetHeader(hList);
    nCols = Header_GetItemCount(hHdr);
    for (i = 0; i < nCols; i++) {
        HDITEM hdi = {0};
        hdi.mask = HDI_FORMAT;
        Header_GetItem(hHdr, i, &hdi);
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sortCol)
            hdi.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hHdr, i, &hdi);
    }
}

/* -----------------------------------------------------------------------
   QW player color palette and row coloring
   ----------------------------------------------------------------------- */

/*
 * QuakeWorld topcolor/bottomcolor display colors, indices 0-16.
 * Colors verified against actual in-game display:
 *   0=White, 1=Brown, 2=Blue, 3=Green, 4=Red,
 *   5=Gold/Orange-Yellow, 6=Orange-Red, 7=Gold/Yellow,
 *   8=Purple, 9=Dark Purple, 10=Teal/Cyan, 11=Bright Green,
 *   12=Yellow, 13=Blue(dark), 14=Orange, 15=Bright Red, 16=Black
 */
static const COLORREF QW_COLORS[17] = {
    RGB(220, 220, 220), /*  0 - White          */
    RGB( 96,  52,  12), /*  1 - Brown          */
    RGB( 52, 100, 188), /*  2 - Blue           */
    RGB( 60, 180,  60), /*  3 - Green          */
    RGB(200,  32,  24), /*  4 - Red            */
    RGB(212, 168,  36), /*  5 - Gold           */
    RGB(204,  96,  36), /*  6 - Orange-Red     */
    RGB(220, 184,  60), /*  7 - Yellow-Gold    */
    RGB(140,  48, 172), /*  8 - Purple         */
    RGB( 72,  16, 100), /*  9 - Dark Purple    */
    RGB( 48, 176, 168), /* 10 - Teal           */
    RGB( 80, 220,  60), /* 11 - Bright Green   */
    RGB(228, 212,  36), /* 12 - Yellow         */
    RGB( 24,  64, 196), /* 13 - Dark Blue      */
    RGB(220, 140,  24), /* 14 - Orange         */
    RGB(228,  64,  40), /* 15 - Bright Red     */
    RGB( 16,  16,  16), /* 16 - Black          */
};

/* Return a lightened background color for a player row.
   We lighten by blending 55% toward white so text remains readable. */
static COLORREF PlayerRowColor(int topcolor) {
    COLORREF c;
    int r, g, b;
    if (topcolor < 0)  topcolor = 0;
    if (topcolor > 16) topcolor = topcolor % 17;
    c = QW_COLORS[topcolor];
    /* Blend toward white: out = in*0.45 + 255*0.55 */
    r = (int)(GetRValue(c) * 45 / 100) + 140; if (r > 255) r = 255;
    g = (int)(GetGValue(c) * 45 / 100) + 140; if (g > 255) g = 255;
    b = (int)(GetBValue(c) * 45 / 100) + 140; if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* -----------------------------------------------------------------------
   Player column header rename (Skin <-> Class)
   ----------------------------------------------------------------------- */
static void UpdatePlayerSkinHeader(HWND hwnd) {
    HWND     hList = GetDlgItem(hwnd, IDC_PLAYERS);
    LVCOLUMN lvc   = {0};
    lvc.mask    = LVCF_TEXT;
    lvc.pszText = g_filters.tfOnly ? "Class" : "Skin";
    ListView_SetColumn(hList, COL_PLR_SKIN, &lvc);
}

/* -----------------------------------------------------------------------
   Engine settings dialog
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK EngineDlgProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            const char *eng  = ConfGetOpt(g_conf, "engine");
            const char *args = ConfGetOpt(g_conf, "engineargs");
            SetDlgItemTextA(hwnd, IDC_ENGINE_PATH, eng  ? eng  : DEFAULT_ENGINE);
            SetDlgItemTextA(hwnd, IDC_ENGINE_ARGS, args ? args : DEFAULT_ENGINEARGS);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_ENGINE_BROWSE: {
                    /* Simple open-file dialog for the .exe */
                    char path[MAX_PATH] = "";
                    OPENFILENAMEA ofn = {0};
                    ofn.lStructSize  = sizeof(ofn);
                    ofn.hwndOwner    = hwnd;
                    ofn.lpstrFilter  = "Executables (*.exe)\0*.exe\0All Files\0*.*\0";
                    ofn.lpstrFile    = path;
                    ofn.nMaxFile     = MAX_PATH;
                    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    ofn.lpstrTitle   = "Select Engine Executable";
                    if (GetOpenFileNameA(&ofn))
                        SetDlgItemTextA(hwnd, IDC_ENGINE_PATH, path);
                    return TRUE;
                }
                case IDOK:
                case IDC_ENGINE_OK: {
                    char path[MAX_PATH], args[512];
                    GetDlgItemTextA(hwnd, IDC_ENGINE_PATH, path, sizeof(path));
                    GetDlgItemTextA(hwnd, IDC_ENGINE_ARGS, args, sizeof(args));
                    ConfSetOpt(g_conf, "engine",     path);
                    ConfSetOpt(g_conf, "engineargs", args);
                    if (!ConfSave(g_conf, g_configPath)) {
                        char errmsg[MAX_PATH + 64];
                        _snprintf(errmsg, sizeof(errmsg)-1,
                            "Failed to save settings to:\n%s\n\nCheck file permissions.",
                            g_configPath);
                        errmsg[sizeof(errmsg)-1] = '\0';
                        MessageBoxA(hwnd, errmsg, "Save Error", MB_ICONWARNING);
                    }
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDC_ENGINE_CANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

static void SortByColumn(HWND hwnd, int col) {
    HWND hList = GetActiveServerList(hwnd);

    if (g_sortCol == col)
        g_sortAsc = !g_sortAsc;
    else {
        g_sortCol = col;
        g_sortAsc = TRUE;
    }

    g_sorting = TRUE;
    ListView_SortItemsEx(hList, SrvSortCmp, (LPARAM)hList);
    SrvSort_Apply(hList);

    /* Mirror sort arrow to the other two ListViews */
    {
        HWND hAll  = GetDlgItem(hwnd, IDC_SERVERS);
        HWND hFav  = GetDlgItem(hwnd, IDC_SERVERS_FAV);
        HWND hHist = GetDlgItem(hwnd, IDC_SERVERS_HIST);
        if (hList != hAll) {
            ListView_SortItemsEx(hAll, SrvSortCmp, (LPARAM)hAll);
            SrvSort_Apply(hAll);
        }
        if (hList != hFav) {
            ListView_SortItemsEx(hFav, SrvSortCmp, (LPARAM)hFav);
            SrvSort_Apply(hFav);
        }
        if (hList != hHist) {
            ListView_SortItemsEx(hHist, SrvSortCmp, (LPARAM)hHist);
            SrvSort_Apply(hHist);
        }
    }
    g_sorting = FALSE;
    UpdateMenuCheckmarks(hwnd);
}

/* -----------------------------------------------------------------------
   Tab management
   ----------------------------------------------------------------------- */

/* Get the currently active server ListView (All, Favorites, History, or Never Ping) */
static HWND GetActiveServerList(HWND hwnd) {
    switch (g_activeTab) {
        case TAB_FAVORITES: return GetDlgItem(hwnd, IDC_SERVERS_FAV);
        case TAB_HISTORY:   return GetDlgItem(hwnd, IDC_SERVERS_HIST);
        case TAB_NEVERSCAN: return GetDlgItem(hwnd, IDC_SERVERS_NVR);
        default:            return GetDlgItem(hwnd, IDC_SERVERS);
    }
}

/* Show the correct ListView for the selected tab, hide others */
static void ShowActiveTab(HWND hwnd) {
    HWND hAll  = GetDlgItem(hwnd, IDC_SERVERS);
    HWND hFav  = GetDlgItem(hwnd, IDC_SERVERS_FAV);
    HWND hHist = GetDlgItem(hwnd, IDC_SERVERS_HIST);
    HWND hNvr  = GetDlgItem(hwnd, IDC_SERVERS_NVR);
    ShowWindow(hAll,  g_activeTab == TAB_ALL       ? SW_SHOW : SW_HIDE);
    ShowWindow(hFav,  g_activeTab == TAB_FAVORITES ? SW_SHOW : SW_HIDE);
    ShowWindow(hHist, g_activeTab == TAB_HISTORY   ? SW_SHOW : SW_HIDE);
    ShowWindow(hNvr,  g_activeTab == TAB_NEVERSCAN ? SW_SHOW : SW_HIDE);
}

/* Populate the Favorites ListView from g_favAddrs / g_servers */
static void RebuildFavoritesList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SERVERS_FAV);
    int  i, j;
    int  favCount;
    const char *favAddr;

    ListView_DeleteAllItems(hList);

    /* Pass 1: add any g_servers[] entry that is flagged as a favorite */
    for (i = 0; i < g_serverCount; i++) {
        if (g_servers[i].isFavorite)
            LV_SetServerItem(hList, -1, i);
    }

    /* Pass 2: for favorites in g_favAddrs[] that have no matching g_servers[]
       entry yet (e.g. no cache, or server not returned by master this session),
       insert a stub row so the user can still see and connect to their favorites.
       The stub shows the address with "..." placeholders for unknown fields. */
    favCount = SL_GetFavCount();
    for (j = 0; j < favCount; j++) {
        BOOL found = FALSE;
        LVITEM lvi = {0};
        char   buf[64];

        favAddr = SL_GetFavAddr(j);
        if (!favAddr) continue;

        /* Check if already added in pass 1 */
        for (i = 0; i < g_serverCount; i++) {
            if (_stricmp(g_servers[i].addr, favAddr) == 0) {
                found = TRUE;
                break;
            }
        }
        if (found) continue;

        /* Insert stub row. lParam = -1 signals "not in g_servers[]". */
        lvi.mask     = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem    = ListView_GetItemCount(hList);
        lvi.iSubItem = 0;
        lvi.lParam   = (LPARAM)-1;
        strncpy(buf, favAddr, 63); buf[63] = '\0';  /* col 0: addr as name placeholder */
        lvi.pszText  = (LPSTR)buf;
        lvi.iItem    = ListView_InsertItem(hList, &lvi);

        lvi.mask = LVIF_TEXT;

        lvi.iSubItem = COL_SRV_ADDR; lvi.pszText = (LPSTR)favAddr;
        ListView_SetItem(hList, &lvi);

        lvi.iSubItem = COL_SRV_MAP;     lvi.pszText = (LPSTR)"-";
        ListView_SetItem(hList, &lvi);
        lvi.iSubItem = COL_SRV_PLAYERS; lvi.pszText = (LPSTR)"...";
        ListView_SetItem(hList, &lvi);
        lvi.iSubItem = COL_SRV_PING;    lvi.pszText = (LPSTR)"...";
        ListView_SetItem(hList, &lvi);
        lvi.iSubItem = COL_SRV_LAST;    lvi.pszText = (LPSTR)"-";
        ListView_SetItem(hList, &lvi);
    }

    if (g_sortCol >= 0) {
        g_sorting = TRUE;
        ListView_SortItemsEx(hList, SrvSortCmp, (LPARAM)hList);
        SrvSort_Apply(hList);
        g_sorting = FALSE;
    }
}

/* Populate the History ListView */
static void RebuildHistoryList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_SERVERS_HIST);
    char histAddrs[MAX_HIST_DISPLAY][22];
    char histTimes[MAX_HIST_DISPLAY][20];
    int  i, n;
    LVITEM lvi = {0};
    char   buf[64];

    ListView_DeleteAllItems(hList);
    n = SL_LoadHistoryEx(histAddrs, histTimes, MAX_HIST_DISPLAY);
    g_histCount = n;
    /* Also store addrs for GetSelectedAddr on History tab */
    for (i = 0; i < n && i < MAX_HIST_DISPLAY; i++)
        strncpy(g_histAddrs[i], histAddrs[i], 21);

    for (i = 0; i < n; i++) {
        int srvIdx = -1;
        int j;
        /* Try to find in g_servers for full detail */
        for (j = 0; j < g_serverCount; j++) {
            if (_stricmp(g_servers[j].addr, histAddrs[i]) == 0) {
                srvIdx = j;
                break;
            }
        }

        if (srvIdx >= 0) {
            /* Stamp the lastPlayed field so LV_SetServerItem can show it */
            strncpy(g_servers[srvIdx].lastPlayed, histTimes[i],
                    sizeof(g_servers[srvIdx].lastPlayed)-1);
            g_servers[srvIdx].lastPlayed[sizeof(g_servers[srvIdx].lastPlayed)-1] = '\0';
            LV_SetServerItem(hList, -1, srvIdx);
        } else {
            /* Server not in current list — show addr + timestamp only */
            lvi.mask     = LVIF_TEXT;
            lvi.iItem    = ListView_GetItemCount(hList);
            lvi.iSubItem = 0;
            strncpy(buf, histAddrs[i], 63); buf[63] = '\0';
            lvi.pszText  = (LPSTR)buf;
            ListView_InsertItem(hList, &lvi);
            /* addr in col 1 */
            lvi.iSubItem = COL_SRV_ADDR;
            lvi.pszText  = (LPSTR)buf;
            ListView_SetItem(hList, &lvi);
            /* last played in col 5 */
            lvi.iSubItem = COL_SRV_LAST;
            strncpy(buf, histTimes[i][0] ? histTimes[i] : "-", 63); buf[63] = '\0';
            lvi.pszText  = (LPSTR)buf;
            ListView_SetItem(hList, &lvi);
        }
    }

    if (g_sortCol >= 0) {
        g_sorting = TRUE;
        ListView_SortItemsEx(hList, SrvSortCmp, (LPARAM)hList);
        SrvSort_Apply(hList);
        g_sorting = FALSE;
    }
}

/* -----------------------------------------------------------------------
   Never Ping tab management
   ----------------------------------------------------------------------- */

static void InitNeverListView(HWND hwnd) {
    HWND     hList = GetDlgItem(hwnd, IDC_SERVERS_NVR);
    LVCOLUMN lvc   = {0};

    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.fmt  = LVCFMT_LEFT;

    lvc.iSubItem = 0; lvc.cx = 200; lvc.pszText = "Address";
    ListView_InsertColumn(hList, 0, &lvc);
    lvc.iSubItem = 1; lvc.cx = 300; lvc.pszText = "Hostname";
    ListView_InsertColumn(hList, 1, &lvc);
}

static void RebuildNeverList(HWND hwnd) {
    HWND  hList = GetDlgItem(hwnd, IDC_SERVERS_NVR);
    int   n = SL_GetNeverCount();
    int   i;
    LVITEM lvi = {0};

    ListView_DeleteAllItems(hList);
    lvi.mask = LVIF_TEXT;
    for (i = 0; i < n; i++) {
        const char *addr = SL_GetNeverAddr(i);
        char hostname[64];
        int  j;

        if (!addr) continue;

        /* Look up hostname from g_servers if available */
        hostname[0] = '\0';
        for (j = 0; j < g_serverCount; j++) {
            if (_stricmp(g_servers[j].addr, addr) == 0) {
                if (g_servers[j].hostname[0])
                    QW_StripName(g_servers[j].hostname, hostname, sizeof(hostname));
                break;
            }
        }

        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = (LPSTR)addr;
        ListView_InsertItem(hList, &lvi);
        lvi.iSubItem = 1;
        lvi.pszText  = hostname[0] ? (LPSTR)hostname : (LPSTR)"-";
        ListView_SetItem(hList, &lvi);
    }
}

/* Add or remove the Never Ping tab based on whether the list has entries.
   Called after any change to the never-scan list. */
static void SyncNeverTab(HWND hwnd) {
    HWND hTab   = GetDlgItem(hwnd, IDC_TAB);
    int  nCount = SL_GetNeverCount();
    BOOL shouldShow = (nCount > 0);

    if (shouldShow && !g_nvrTabVisible) {
        TCITEMA ti = {0};
        ti.mask    = TCIF_TEXT;
        ti.pszText = "Never Ping";
        TabCtrl_InsertItem(hTab, TAB_NEVERSCAN, &ti);
        g_nvrTabVisible = TRUE;
    } else if (!shouldShow && g_nvrTabVisible) {
        /* If user is on the never ping tab, switch back to All first */
        if (g_activeTab == TAB_NEVERSCAN) {
            g_activeTab = TAB_ALL;
            TabCtrl_SetCurSel(hTab, TAB_ALL);
            ShowActiveTab(hwnd);
        }
        TabCtrl_DeleteItem(hTab, TAB_NEVERSCAN);
        g_nvrTabVisible = FALSE;
    }

    if (shouldShow)
        RebuildNeverList(hwnd);
}



/* Get srvIdx for a given ListView item on the currently active server tab.
   For Favorites/History tabs, fetches the addr text from col 1 and searches
   g_servers[]. Returns -1 if not found or item index invalid. */
static int GetContextSrvIdx(HWND hwnd, int lvIdx) {
    if (g_activeTab == TAB_ALL) {
        if (lvIdx < 0 || lvIdx >= g_itemCount) return -1;
        return g_itemToServer[lvIdx];
    } else if (g_activeTab == TAB_NEVERSCAN) {
        /* Never Ping tab: col 0 is the address directly */
        HWND   hList = GetDlgItem(hwnd, IDC_SERVERS_NVR);
        LVITEM lvi   = {0};
        char   addr[22];
        int    j;
        if (lvIdx < 0) return -1;
        addr[0] = '\0';
        lvi.mask       = LVIF_TEXT;
        lvi.iItem      = lvIdx;
        lvi.iSubItem   = 0;
        lvi.pszText    = addr;
        lvi.cchTextMax = sizeof(addr);
        if (!ListView_GetItem(hList, &lvi) || addr[0] == '\0') return -1;
        for (j = 0; j < g_serverCount; j++)
            if (_stricmp(g_servers[j].addr, addr) == 0) return j;
        return -1;
    } else {
        /* Favorites or History: read address from col 1 of the active ListView */
        HWND   hList = GetActiveServerList(hwnd);
        LVITEM lvi   = {0};
        char   addr[22];
        int    j;
        if (lvIdx < 0) return -1;
        addr[0] = '\0';
        lvi.mask       = LVIF_TEXT;
        lvi.iItem      = lvIdx;
        lvi.iSubItem   = COL_SRV_ADDR;
        lvi.pszText    = addr;
        lvi.cchTextMax = sizeof(addr);
        if (!ListView_GetItem(hList, &lvi) || addr[0] == '\0') return -1;
        /* Search g_servers for this address */
        for (j = 0; j < g_serverCount; j++)
            if (_stricmp(g_servers[j].addr, addr) == 0) return j;
        return -1;
    }
}

static void ShowContextMenu(HWND hwnd, int lvIdx, POINT pt) {
    HMENU hMenu  = CreatePopupMenu();
    int   srvIdx;
    BOOL  isFav, isNvr;
    /* Address resolved from the ListView text (valid even for stub rows) */
    char  ctxAddr[22];

    if (!hMenu) return;

    /* Never Ping tab gets its own simplified context menu */
    if (g_activeTab == TAB_NEVERSCAN) {
        BOOL hasSelection = (lvIdx >= 0 &&
                             lvIdx < ListView_GetItemCount(GetDlgItem(hwnd, IDC_SERVERS_NVR)));
        AppendMenuA(hMenu, MF_STRING | (hasSelection ? 0 : MF_GRAYED),
                    IDM_CTX_NVR_REMOVE, "Remove Selected");
        AppendMenuA(hMenu, MF_STRING, IDM_CTX_NVR_REMOVEALL, "Remove All");
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(hMenu);
        return;
    }

    srvIdx = GetContextSrvIdx(hwnd, lvIdx);
    isFav  = (srvIdx >= 0) ? g_servers[srvIdx].isFavorite : FALSE;
    isNvr  = (srvIdx >= 0) ? g_servers[srvIdx].neverPing  : FALSE;

    /* For stub rows on the Favorites tab (srvIdx == -1), we can still read
       the address from the ListView text to determine true favorite status. */
    ctxAddr[0] = '\0';
    if (srvIdx < 0 && g_activeTab == TAB_FAVORITES && lvIdx >= 0) {
        HWND   hList = GetDlgItem(hwnd, IDC_SERVERS_FAV);
        LVITEM lvi   = {0};
        lvi.mask       = LVIF_TEXT;
        lvi.iItem      = lvIdx;
        lvi.iSubItem   = COL_SRV_ADDR;
        lvi.pszText    = ctxAddr;
        lvi.cchTextMax = sizeof(ctxAddr);
        ListView_GetItem(hList, &lvi);
        /* If we got an address, check the in-memory favorites list */
        if (ctxAddr[0] != '\0') {
            int k, fc = SL_GetFavCount();
            for (k = 0; k < fc; k++) {
                if (_stricmp(SL_GetFavAddr(k), ctxAddr) == 0) {
                    isFav = TRUE;
                    break;
                }
            }
        }
    }

    AppendMenuA(hMenu, MF_STRING, IDM_CTX_CONNECT, "Connect to Server");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_FAVORITE,
                isFav ? "Remove from Favorites" : "Add to Favorites");
    AppendMenuA(hMenu, MF_STRING | (isNvr ? MF_CHECKED : 0),
                IDM_CTX_NEVERSCAN, "Never Ping This Server");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_REFRESH_SEL, "Refresh Selected");

    if (srvIdx < 0) {
        /* No g_servers[] entry: Connect and Never Ping need live server data */
        EnableMenuItem(hMenu, IDM_CTX_CONNECT,   MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(hMenu, IDM_CTX_NEVERSCAN, MF_BYCOMMAND | MF_GRAYED);
        /* Allow Remove from Favorites and Refresh if we have an address */
        if (ctxAddr[0] == '\0') {
            EnableMenuItem(hMenu, IDM_CTX_FAVORITE,    MF_BYCOMMAND | MF_GRAYED);
            EnableMenuItem(hMenu, IDM_CTX_REFRESH_SEL, MF_BYCOMMAND | MF_GRAYED);
        }
        /* else: isFav/Refresh enabled — user can remove the stub or retry ping */
    }

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

/* -----------------------------------------------------------------------
   Single-server / selected-server refresh
   ----------------------------------------------------------------------- */

/* Thread param for refreshing individual selected servers */
typedef struct {
    HWND  hwndDlg;
    char  addrs[64][22];   /* up to 64 selected servers */
    int   count;
} REFRESH_SEL_PARAM;

static DWORD WINAPI RefreshSelThread(LPVOID param) {
    REFRESH_SEL_PARAM *p = (REFRESH_SEL_PARAM*)param;
    int i;
    for (i = 0; i < p->count; i++) {
        int j;
        SERVER_ENTRY local;
        SERVER_ENTRY *copy;
        char host[64];
        unsigned short port = 0;
        const char *colon;

        /* Find in g_servers */
        EnterCriticalSection(&g_listLock);
        for (j = 0; j < g_serverCount; j++) {
            if (_stricmp(g_servers[j].addr, p->addrs[i]) == 0) break;
        }
        if (j >= g_serverCount) {
            LeaveCriticalSection(&g_listLock);
            continue;
        }
        local = g_servers[j];  /* full copy preserves isFavorite, neverPing, lastPlayed */
        LeaveCriticalSection(&g_listLock);

        colon = strrchr(local.addr, ':');
        if (!colon) continue;
        {
            size_t n = (size_t)(colon - local.addr);
            if (n >= sizeof(host)) n = sizeof(host)-1;
            memcpy(host, local.addr, n); host[n] = '\0';
        }
        port = (unsigned short)atoi(colon + 1);

        {
            QW_SERVERINFO info;
            if (QW_QueryServer(host, port, 1500, &info)) {
                local.ping       = info.ping;
                local.maxplayers = info.maxplayers;
                local.players    = info.numplayers;
                local.fraglimit  = info.fraglimit;
                local.timelimit  = info.timelimit;
                local.state      = SRV_ALIVE;
                strncpy(local.hostname, info.hostname[0] ? info.hostname : local.addr,
                        sizeof(local.hostname)-1);
                local.hostname[sizeof(local.hostname)-1] = '\0';
                strncpy(local.map,     info.map,     sizeof(local.map)-1);
                local.map[sizeof(local.map)-1] = '\0';
                strncpy(local.gamedir, info.gamedir, sizeof(local.gamedir)-1);
                local.gamedir[sizeof(local.gamedir)-1] = '\0';
                local.numplayers_detail = info.numplayers;
                {
                    int k;
                    for (k = 0; k < info.numplayers && k < QW_MAX_PLAYERS; k++)
                        local.players_detail[k] = info.players[k];
                }
                local.numkvpairs = info.numkvpairs;
                {
                    int k;
                    for (k = 0; k < info.numkvpairs && k < QW_MAX_KEYS; k++)
                        local.kvpairs[k] = info.kvpairs[k];
                }
            } else {
                local.state = SRV_DEAD;
                local.ping  = 999;
            }
        }

        /* isFavorite, neverPing, lastPlayed already preserved in local copy above */
        local.passesFilter = Filter_Pass(&local, &g_filters);

        EnterCriticalSection(&g_listLock);
        g_servers[j] = local;
        LeaveCriticalSection(&g_listLock);

        copy = (SERVER_ENTRY*)malloc(sizeof(SERVER_ENTRY));
        if (copy) {
            *copy = local;
            PostMessage(p->hwndDlg, WM_APP_SERVERINFO,
                        (WPARAM)((((LONG)g_scanSerial & 0xFFFF) << 16) | (j & 0xFFFF)),
                        (LPARAM)copy);
        }
    }
    /* Use REFRESH_DONE, not SCAN_DONE — avoids corrupting a running full scan */
    PostMessage(p->hwndDlg, WM_APP_REFRESH_DONE, 0, 0);
    free(p);
    return 0;
}

static void RefreshSelected(HWND hwnd) {
    HWND               hList = GetActiveServerList(hwnd);
    REFRESH_SEL_PARAM *p;
    int                iNext;
    HANDLE             hThread;

    p = (REFRESH_SEL_PARAM*)calloc(1, sizeof(REFRESH_SEL_PARAM));
    if (!p) return;
    p->hwndDlg = hwnd;

    /* Save the current selection so WM_APP_SERVERINFO can restore it
       after RebuildServerList wipes the ListView. */
    {
        char tmp[22];
        g_savedSelAddr[0] = '\0';
        if (GetSelectedAddr(hwnd, tmp, sizeof(tmp)))
            strncpy(g_savedSelAddr, tmp, sizeof(g_savedSelAddr) - 1);
    }

    /* Collect all selected items. For normal rows use GetContextSrvIdx;
       for stub rows on the Favorites tab (srvIdx == -1) read the addr
       from the ListView text and add a new g_servers[] entry so the
       ping thread can update it like any other server. */
    iNext = -1;
    while (p->count < 64) {
        int srvIdx;
        iNext = ListView_GetNextItem(hList, iNext, LVNI_SELECTED);
        if (iNext < 0) break;
        srvIdx = GetContextSrvIdx(hwnd, iNext);
        if (srvIdx >= 0) {
            strncpy(p->addrs[p->count], g_servers[srvIdx].addr, 21);
            p->addrs[p->count][21] = '\0';
            p->count++;
        } else if (g_activeTab == TAB_FAVORITES) {
            /* Stub row: read addr from ListView text */
            char stubAddr[22];
            LVITEM lvi = {0};
            stubAddr[0] = '\0';
            lvi.mask       = LVIF_TEXT;
            lvi.iItem      = iNext;
            lvi.iSubItem   = COL_SRV_ADDR;
            lvi.pszText    = stubAddr;
            lvi.cchTextMax = sizeof(stubAddr);
            ListView_GetItem(hList, &lvi);
            if (stubAddr[0] != '\0') {
                /* Add to g_servers[] so the refresh thread can ping it */
                int newIdx = SL_AddAddr(stubAddr);
                if (newIdx >= 0) {
                    g_servers[newIdx].isFavorite = TRUE;
                    strncpy(p->addrs[p->count], stubAddr, 21);
                    p->addrs[p->count][21] = '\0';
                    p->count++;
                }
            }
        }
    }

    if (p->count == 0) { free(p); return; }

    hThread = CreateThread(NULL, 0, RefreshSelThread, p, 0, NULL);
    if (hThread) CloseHandle(hThread);
    else         free(p);
}

/* Shared column setup for any server ListView (All, Favorites, or History) */
/* Read column widths from hSrc and apply them to all server ListViews,
   then save into g_srvColWidths so new tabs pick them up too. */
static void SyncSrvColWidths(HWND hwnd, HWND hSrc) {
    int col;
    HWND lists[4];
    int  n, i;

    /* Snapshot widths from the source list */
    for (col = 0; col < NUM_SRV_COLS; col++)
        g_srvColWidths[col] = ListView_GetColumnWidth(hSrc, col);

    /* Apply to every other server ListView */
    lists[0] = GetDlgItem(hwnd, IDC_SERVERS);
    lists[1] = GetDlgItem(hwnd, IDC_SERVERS_FAV);
    lists[2] = GetDlgItem(hwnd, IDC_SERVERS_HIST);
    lists[3] = GetDlgItem(hwnd, IDC_SERVERS_NVR);
    n = 4;
    for (i = 0; i < n; i++) {
        if (lists[i] == hSrc) continue;
        for (col = 0; col < NUM_SRV_COLS; col++)
            ListView_SetColumnWidth(lists[i], col, g_srvColWidths[col]);
    }
}

static void InitServerListViewColumns(HWND hList) {
    LVCOLUMN lvc = {0};
    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.fmt  = LVCFMT_LEFT;
    lvc.iSubItem = COL_SRV_NAME;    lvc.cx = g_srvColWidths[COL_SRV_NAME];    lvc.pszText = "Server";      ListView_InsertColumn(hList, COL_SRV_NAME,    &lvc);
    lvc.iSubItem = COL_SRV_ADDR;    lvc.cx = g_srvColWidths[COL_SRV_ADDR];    lvc.pszText = "Address";     ListView_InsertColumn(hList, COL_SRV_ADDR,    &lvc);
    lvc.iSubItem = COL_SRV_MAP;     lvc.cx = g_srvColWidths[COL_SRV_MAP];     lvc.pszText = "Map";         ListView_InsertColumn(hList, COL_SRV_MAP,     &lvc);
    lvc.iSubItem = COL_SRV_PLAYERS; lvc.cx = g_srvColWidths[COL_SRV_PLAYERS]; lvc.pszText = "Players";     ListView_InsertColumn(hList, COL_SRV_PLAYERS, &lvc);
    lvc.iSubItem = COL_SRV_PING;    lvc.cx = g_srvColWidths[COL_SRV_PING];    lvc.pszText = "Ping";        ListView_InsertColumn(hList, COL_SRV_PING,    &lvc);
    lvc.iSubItem = COL_SRV_LAST;    lvc.cx = g_srvColWidths[COL_SRV_LAST];    lvc.pszText = "Last Played"; ListView_InsertColumn(hList, COL_SRV_LAST,    &lvc);
}

static void InitServerListView(HWND hwnd) {
    HWND hFav, hHist;
    HWND hList = GetDlgItem(hwnd, IDC_SERVERS);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    InitServerListViewColumns(hList);

    /* Favorites ListView */
    hFav = GetDlgItem(hwnd, IDC_SERVERS_FAV);
    ListView_SetExtendedListViewStyle(hFav,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    InitServerListViewColumns(hFav);
    ShowWindow(hFav, SW_HIDE);

    /* History ListView */
    hHist = GetDlgItem(hwnd, IDC_SERVERS_HIST);
    ListView_SetExtendedListViewStyle(hHist,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    InitServerListViewColumns(hHist);
    ShowWindow(hHist, SW_HIDE);

    /* Never Ping ListView — hidden until entries exist; columns set by InitNeverListView */
    ShowWindow(GetDlgItem(hwnd, IDC_SERVERS_NVR), SW_HIDE);
}

static void InitTabControl(HWND hwnd) {
    HWND     hTab = GetDlgItem(hwnd, IDC_TAB);
    TCITEMA  ti   = {0};
    ti.mask    = TCIF_TEXT;
    ti.pszText = "All";       TabCtrl_InsertItem(hTab, TAB_ALL,       &ti);
    ti.pszText = "Favorites"; TabCtrl_InsertItem(hTab, TAB_FAVORITES, &ti);
    ti.pszText = "History";   TabCtrl_InsertItem(hTab, TAB_HISTORY,   &ti);
}


static void InitPlayerListView(HWND hwnd) {
    HWND     hList = GetDlgItem(hwnd, IDC_PLAYERS);
    LVCOLUMN lvc  = {0};

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.fmt  = LVCFMT_LEFT;

    /* Col 0: "Player Name" — owner-drawn: color swatches + name in one cell,
       matching GameSpy 3D's combined "Player Name" column layout. */
    lvc.iSubItem = COL_PLR_NAME;  lvc.cx = 160; lvc.pszText = "Player Name";
    ListView_InsertColumn(hList, COL_PLR_NAME,  &lvc);
    lvc.iSubItem = COL_PLR_FRAGS; lvc.cx =  40; lvc.pszText = "Score";
    ListView_InsertColumn(hList, COL_PLR_FRAGS, &lvc);
    lvc.iSubItem = COL_PLR_TIME;  lvc.cx =  36; lvc.pszText = "Time";
    ListView_InsertColumn(hList, COL_PLR_TIME,  &lvc);
    lvc.iSubItem = COL_PLR_PING;  lvc.cx =  36; lvc.pszText = "Ping";
    ListView_InsertColumn(hList, COL_PLR_PING,  &lvc);
    lvc.iSubItem = COL_PLR_SKIN;  lvc.cx =  64;
    lvc.pszText = g_filters.tfOnly ? "Class" : "Skin";
    ListView_InsertColumn(hList, COL_PLR_SKIN, &lvc);

    /* Hidden team column — zero-width; stores team name for fortress-server sort */
    lvc.iSubItem = COL_PLR_TEAM; lvc.cx = 0; lvc.pszText = "";
    ListView_InsertColumn(hList, COL_PLR_TEAM, &lvc);
}

/* -----------------------------------------------------------------------
   Server info ListView  (two columns: Key | Value)
   ----------------------------------------------------------------------- */
static void InitSrvInfoView(HWND hwnd) {
    HWND     hList = GetDlgItem(hwnd, IDC_SRVINFO);
    LVCOLUMN lvc   = {0};

    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_GRIDLINES);

    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.fmt  = LVCFMT_LEFT;

    lvc.iSubItem = 0; lvc.cx = 100; lvc.pszText = "Key";
    ListView_InsertColumn(hList, 0, &lvc);
    lvc.iSubItem = 1; lvc.cx = 200; lvc.pszText = "Value";
    ListView_InsertColumn(hList, 1, &lvc);
}

static void RefreshSrvInfo(HWND hwnd, int srvIdx) {
    HWND hList = GetDlgItem(hwnd, IDC_SRVINFO);
    SERVER_ENTRY e;
    int i;
    LVITEM lvi = {0};
    char buf[QW_MAX_VALLEN];

    ListView_DeleteAllItems(hList);

    if (srvIdx < 0 || srvIdx >= g_serverCount) return;

    EnterCriticalSection(&g_listLock);
    e = g_servers[srvIdx];
    LeaveCriticalSection(&g_listLock);

    if (e.state != SRV_ALIVE) return;

    lvi.mask = LVIF_TEXT;
    for (i = 0; i < e.numkvpairs; i++) {
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = (LPSTR)e.kvpairs[i].key;
        ListView_InsertItem(hList, &lvi);

        /* Strip QW high-bit color encoding from the value */
        QW_StripName(e.kvpairs[i].val, buf, sizeof(buf));
        ListView_SetItemText(hList, i, 1, (LPSTR)buf);
    }
}

/* -----------------------------------------------------------------------
   Splitter positioning — reposition players and srvinfo panels
   ----------------------------------------------------------------------- */
static void MoveSplitter(HWND hwnd, int newX) {
    RECT rcClient;
    int  bottomTop, bottomH;
    int  rightEdge;
    HWND hPly = GetDlgItem(hwnd, IDC_PLAYERS);
    HWND hSpl = GetDlgItem(hwnd, IDC_SPLITTER);
    HWND hSrv = GetDlgItem(hwnd, IDC_SRVINFO);
    RECT rc;
    HDWP hdwp;

    GetClientRect(hwnd, &rcClient);
    rightEdge = rcClient.right - 5;

    /* Clamp */
    if (newX < 5 + SPLITTER_MIN_LEFT)              newX = 5 + SPLITTER_MIN_LEFT;
    if (newX > rightEdge - SPLITTER_MIN_RIGHT - SPLITTER_W)
        newX = rightEdge - SPLITTER_MIN_RIGHT - SPLITTER_W;

    g_splitterX = newX;

    /* Get current bottom panel top/height from the players control */
    GetWindowRect(hPly, &rc);
    MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
    bottomTop = rc.top;
    bottomH   = rc.bottom - rc.top;

    hdwp = BeginDeferWindowPos(3);
    if (!hdwp) return;
    hdwp = DeferWindowPos(hdwp, hPly, NULL,
        5, bottomTop, newX - 5, bottomH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, hSpl, NULL,
        newX, bottomTop, SPLITTER_W, bottomH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    hdwp = DeferWindowPos(hdwp, hSrv, NULL,
        newX + SPLITTER_W, bottomTop,
        rightEdge - (newX + SPLITTER_W), bottomH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(hdwp);
}


/* -----------------------------------------------------------------------
   Player sort comparator and column-header click
   ----------------------------------------------------------------------- */

/* PlrSortCmp - comparator for ListView_SortItemsEx on the player list.
   g_plrSortCol selects the primary key; for fortress servers the sort is
   always team-name first (ascending), then frags descending regardless of
   g_plrSortCol, so players are grouped by team. */
static int CALLBACK PlrSortCmp(LPARAM lp1, LPARAM lp2, LPARAM lParamSort) {
    /* ListView_SortItemsEx passes item INDICES (not lParam values) as lp1/lp2.
       Retrieve the stored row index (original insertion order = players_detail index)
       via LVM_GETITEM on the player ListView. */
    HWND hPlayers = (HWND)lParamSort;
    HWND hDlg;
    int  srvIdx = -1;
    int  isFortress = 0;
    int  r1, r2;
    LVITEM lvi;
    const QW_PLAYER *p1, *p2;

    hDlg = GetParent(hPlayers);

    /* Retrieve stored lParam (= original row index) for each item */
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask   = LVIF_PARAM;
    lvi.iItem  = (int)lp1;
    lvi.lParam = -1;
    ListView_GetItem(hPlayers, &lvi);
    r1 = (int)lvi.lParam;

    lvi.iItem  = (int)lp2;
    lvi.lParam = -1;
    ListView_GetItem(hPlayers, &lvi);
    r2 = (int)lvi.lParam;

    /* Resolve srvIdx and check gamedir */
    {
        HWND hSrv  = GetActiveServerList(hDlg);
        int  lvIdx = ListView_GetSelectionMark(hSrv);
        srvIdx = GetContextSrvIdx(hDlg, lvIdx);
        if (srvIdx >= 0)
            isFortress = (_stricmp(g_servers[srvIdx].gamedir, "fortress") == 0);
    }

    if (srvIdx < 0 ||
        r1 < 0 || r1 >= g_servers[srvIdx].numplayers_detail ||
        r2 < 0 || r2 >= g_servers[srvIdx].numplayers_detail)
        return 0;

    p1 = &g_servers[srvIdx].players_detail[r1];
    p2 = &g_servers[srvIdx].players_detail[r2];

    /* Fortress servers: always group by team name first, then frags descending.
       The user-selected sort column applies within each team group as a
       tiebreaker — except when they explicitly chose Name/Time/Ping/Skin,
       in which case team grouping still takes priority. */
    if (isFortress) {
        int tcmp = _stricmp(p1->team, p2->team);
        if (tcmp != 0) return tcmp;          /* team group first */
        /* Within a team: respect user sort col; default to frags desc */
        {
            int result;
            switch (g_plrSortCol) {
                case COL_PLR_NAME:  result = _stricmp(p1->name, p2->name); break;
                case COL_PLR_TIME:  result = p1->time - p2->time;          break;
                case COL_PLR_PING:  result = p1->ping - p2->ping;          break;
                case COL_PLR_SKIN:  result = _stricmp(p1->skin, p2->skin); break;
                default:            result = p2->frags - p1->frags;        break; /* frags desc */
            }
            return g_plrSortAsc ? result : -result;
        }
    }

    /* Non-fortress: straight single-column sort */
    {
        int result;
        switch (g_plrSortCol) {
            case COL_PLR_NAME:  result = _stricmp(p1->name, p2->name); break;
            case COL_PLR_FRAGS: result = p2->frags - p1->frags;        break; /* natural desc */
            case COL_PLR_TIME:  result = p1->time  - p2->time;         break;
            case COL_PLR_PING:  result = p1->ping  - p2->ping;         break;
            case COL_PLR_SKIN:  result = _stricmp(p1->skin, p2->skin); break;
            default:            result = p2->frags - p1->frags;        break;
        }
        return g_plrSortAsc ? result : -result;
    }
}

static void SortPlayers(HWND hwnd) {
    HWND hPlayers = GetDlgItem(hwnd, IDC_PLAYERS);
    ListView_SortItemsEx(hPlayers, PlrSortCmp, (LPARAM)hPlayers);
}

/* -----------------------------------------------------------------------
   Menu state helpers
   ----------------------------------------------------------------------- */
static void UpdateMenuCheckmarks(HWND hwnd) {
    HMENU hMenu = GetMenu(hwnd);
    HMENU hView, hFilters, hSortSrv, hSortPlr;
    static const UINT srvSortIDs[6] = {
        IDM_VIEW_SORT_SRV_NAME, IDM_VIEW_SORT_SRV_ADDR, IDM_VIEW_SORT_SRV_MAP,
        IDM_VIEW_SORT_SRV_PLAYERS, IDM_VIEW_SORT_SRV_PING, IDM_VIEW_SORT_SRV_LAST
    };
    static const UINT plrSortIDs[5] = {
        IDM_VIEW_SORT_PLR_NAME, IDM_VIEW_SORT_PLR_SCORE, IDM_VIEW_SORT_PLR_TIME,
        IDM_VIEW_SORT_PLR_PING, IDM_VIEW_SORT_PLR_SKIN
    };
    if (!hMenu) return;

    hFilters = GetSubMenu(hMenu, 3); /* Filters is 4th top-level item (0-based) */
    if (hFilters) {
        CheckMenuItem(hFilters, IDM_FILTER_TF_ONLY,
                      g_filters.tfOnly       ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hFilters, IDM_FILTER_HIDE_EMPTY,
                      g_filters.hideEmpty    ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hFilters, IDM_FILTER_HIDE_FULL,
                      g_filters.hideFull     ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hFilters, IDM_FILTER_HIDE_NOTEMPTY,
                      g_filters.hideNotEmpty ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hFilters, IDM_FILTER_HIDE_HIGHPING,
                      g_filters.hideHighPing ? MF_CHECKED : MF_UNCHECKED);
    }

    hView = GetSubMenu(hMenu, 2); /* View is 3rd top-level item */
    if (hView) {
        CheckMenuItem(hView, IDM_VIEW_HIDE_BOTTOM,
                      g_hidePanes ? MF_CHECKED : MF_UNCHECKED);

        /* Sort Servers submenu — bullet on the active column */
        hSortSrv = GetSubMenu(hView, 0);
        if (hSortSrv && g_sortCol >= 0 && g_sortCol < 6) {
            CheckMenuRadioItem(hSortSrv,
                IDM_VIEW_SORT_SRV_NAME, IDM_VIEW_SORT_SRV_LAST,
                srvSortIDs[g_sortCol], MF_BYCOMMAND);
        }

        /* Sort Players submenu — bullet on the active column.
           g_plrSortCol uses COL_PLR_* indices; map to menu IDs.
           COL_PLR_NAME=0, FRAGS=1, TIME=2, PING=3, SKIN=4. */
        hSortPlr = GetSubMenu(hView, 1);
        if (hSortPlr && g_plrSortCol >= 0 && g_plrSortCol < 5) {
            CheckMenuRadioItem(hSortPlr,
                IDM_VIEW_SORT_PLR_NAME, IDM_VIEW_SORT_PLR_SKIN,
                plrSortIDs[g_plrSortCol], MF_BYCOMMAND);
        }
    }
}

/* Show/hide the bottom panels (players, splitter, srvinfo) */
static void ApplyHideBottomPanes(HWND hwnd) {
    int   show = g_hidePanes ? SW_HIDE : SW_SHOW;
    ShowWindow(GetDlgItem(hwnd, IDC_PLAYERS),  show);
    ShowWindow(GetDlgItem(hwnd, IDC_SPLITTER), show);
    ShowWindow(GetDlgItem(hwnd, IDC_SRVINFO),  show);
}

/* -----------------------------------------------------------------------
   Add Server manually dialog proc
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK AddServerDlgProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            SetWindowLongA(hwnd, GWL_USERDATA, (LONG)lParam); /* addr buffer */
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_ADDSERVER_OK:
                case IDOK: {
                    char *buf = (char*)(LONG_PTR)GetWindowLongA(hwnd, GWL_USERDATA);
                    if (buf) {
                        GetDlgItemTextA(hwnd, IDC_ADDSERVER_ADDR, buf, 21);
                        buf[21] = '\0';
                        /* Basic validation: must contain ':' */
                        if (!strchr(buf, ':')) {
                            MessageBoxA(hwnd,
                                "Please enter an address in ip:port format\n"
                                "(e.g. 192.168.1.1:27500).",
                                "Add Server", MB_ICONWARNING);
                            return TRUE;
                        }
                    }
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDC_ADDSERVER_CANCEL:
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

static void LoadConfig(HWND hwnd) {
    const char *sv;
    g_conf = ConfCreate();
    if (!ConfLoad(g_conf, g_configPath)) {
        /* First run defaults */
        ConfSetOpt(g_conf, "engine",      DEFAULT_ENGINE);
        ConfSetOpt(g_conf, "engineargs",  DEFAULT_ENGINEARGS);
        ConfSetOpt(g_conf, "master_host", DEFAULT_MASTER_HOST);
        ConfSetOpt(g_conf, "master_port", DEFAULT_MASTER_PORT);
    } else {
        if (!ConfGetOpt(g_conf, "engine"))      ConfSetOpt(g_conf, "engine",      DEFAULT_ENGINE);
        if (!ConfGetOpt(g_conf, "engineargs"))  ConfSetOpt(g_conf, "engineargs",  DEFAULT_ENGINEARGS);
        if (!ConfGetOpt(g_conf, "master_host")) ConfSetOpt(g_conf, "master_host", DEFAULT_MASTER_HOST);
        if (!ConfGetOpt(g_conf, "master_port")) ConfSetOpt(g_conf, "master_port", DEFAULT_MASTER_PORT);
    }

    /* Load sort state */
    sv = ConfGetOpt(g_conf, "sort_col");
    if (sv) g_sortCol = atoi(sv);
    sv = ConfGetOpt(g_conf, "sort_asc");
    if (sv) g_sortAsc = atoi(sv) ? TRUE : FALSE;

    /* Load auto-scan state */
    sv = ConfGetOpt(g_conf, "auto_mode");
    if (sv) g_autoMode = atoi(sv);
    if (g_autoMode < AUTO_MODE_OFF || g_autoMode > AUTO_MODE_REFRESH)
        g_autoMode = AUTO_MODE_OFF;
    sv = ConfGetOpt(g_conf, "auto_interval");
    if (sv) g_autoInterval = atoi(sv);
    if (g_autoInterval < 1) g_autoInterval = 1;

    Filter_Load(&g_filters, g_conf);
    /* Do NOT call ConfSave here — only save when values actually change */
}

/* -----------------------------------------------------------------------
   Auto-scan helpers
   ----------------------------------------------------------------------- */

/* Reflect g_autoMode in the toolbar button states.
   The clock button (IDC_BTN_AUTO) stays checked whenever auto is active.
   The Full Update or Refresh button is also checked to show which mode
   is running. Both use BS_AUTOCHECKBOX | BS_PUSHLIKE so BM_SETCHECK
   produces the depressed visual without the button being disabled. */
static void UpdateAutoButtons(HWND hwnd) {
    HWND hClock   = GetDlgItem(hwnd, IDC_BTN_AUTO);
    HWND hUpdate  = GetDlgItem(hwnd, IDC_BTN_UPDATE_MASTER);
    HWND hRefresh = GetDlgItem(hwnd, IDC_BTN_REFRESH_CACHE);

    SendMessage(hClock,   BM_SETCHECK,
                g_autoMode != AUTO_MODE_OFF ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(hUpdate,  BM_SETCHECK,
                g_autoMode == AUTO_MODE_UPDATE  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(hRefresh, BM_SETCHECK,
                g_autoMode == AUTO_MODE_REFRESH ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* Arm or rearm the IDT_AUTO timer. Call after g_autoMode/g_autoInterval change. */
static void StartAutoTimer(HWND hwnd) {
    KillTimer(hwnd, IDT_AUTO);
    if (g_autoMode != AUTO_MODE_OFF)
        SetTimer(hwnd, IDT_AUTO, (UINT)(g_autoInterval * 60 * 1000), NULL);
}

static void StopAutoTimer(HWND hwnd) {
    KillTimer(hwnd, IDT_AUTO);
}

/* -----------------------------------------------------------------------
   Auto-scan dialog proc
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK AutoDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            char buf[16];

            /* Initialise checkboxes from current global state */
            CheckDlgButton(hwnd, IDC_AUTO_UPDATE,
                           g_autoMode == AUTO_MODE_UPDATE  ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_AUTO_REFRESH,
                           g_autoMode == AUTO_MODE_REFRESH ? BST_CHECKED : BST_UNCHECKED);

            /* Populate minute fields with current interval */
            _snprintf(buf, sizeof(buf)-1, "%d", g_autoInterval);
            buf[sizeof(buf)-1] = '\0';
            SetDlgItemTextA(hwnd, IDC_AUTO_UPDATE_MINS,  buf);
            SetDlgItemTextA(hwnd, IDC_AUTO_REFRESH_MINS, buf);

            /* Grey the minute fields unless their checkbox is checked */
            EnableWindow(GetDlgItem(hwnd, IDC_AUTO_UPDATE_MINS),
                         g_autoMode == AUTO_MODE_UPDATE);
            EnableWindow(GetDlgItem(hwnd, IDC_AUTO_REFRESH_MINS),
                         g_autoMode == AUTO_MODE_REFRESH);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {

                case IDC_AUTO_UPDATE:
                    /* Checking Update clears Refresh (mutual exclusion) */
                    if (IsDlgButtonChecked(hwnd, IDC_AUTO_UPDATE) == BST_CHECKED) {
                        CheckDlgButton(hwnd, IDC_AUTO_REFRESH, BST_UNCHECKED);
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_UPDATE_MINS),  TRUE);
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_REFRESH_MINS), FALSE);
                    } else {
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_UPDATE_MINS),  FALSE);
                    }
                    break;

                case IDC_AUTO_REFRESH:
                    /* Checking Refresh clears Update (mutual exclusion) */
                    if (IsDlgButtonChecked(hwnd, IDC_AUTO_REFRESH) == BST_CHECKED) {
                        CheckDlgButton(hwnd, IDC_AUTO_UPDATE, BST_UNCHECKED);
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_REFRESH_MINS), TRUE);
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_UPDATE_MINS),  FALSE);
                    } else {
                        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_REFRESH_MINS), FALSE);
                    }
                    break;

                case IDOK:
                case IDC_AUTO_OK: {
                    BOOL wantUpdate  = (IsDlgButtonChecked(hwnd, IDC_AUTO_UPDATE)  == BST_CHECKED);
                    BOOL wantRefresh = (IsDlgButtonChecked(hwnd, IDC_AUTO_REFRESH) == BST_CHECKED);
                    int  mins = 0;
                    char buf[16];

                    if (wantUpdate) {
                        GetDlgItemTextA(hwnd, IDC_AUTO_UPDATE_MINS, buf, sizeof(buf));
                        mins = atoi(buf);
                    } else if (wantRefresh) {
                        GetDlgItemTextA(hwnd, IDC_AUTO_REFRESH_MINS, buf, sizeof(buf));
                        mins = atoi(buf);
                    }

                    if ((wantUpdate || wantRefresh) && mins < 1) {
                        MessageBoxA(hwnd,
                            "Please enter an interval of at least 1 minute.",
                            "ezqlaunch", MB_ICONWARNING | MB_OK);
                        break;
                    }

                    if (wantUpdate)        g_autoMode = AUTO_MODE_UPDATE;
                    else if (wantRefresh)  g_autoMode = AUTO_MODE_REFRESH;
                    else                   g_autoMode = AUTO_MODE_OFF;

                    if (mins > 0) g_autoInterval = mins;

                    /* Persist to config */
                    {
                        char modebuf[4], intbuf[12];
                        _snprintf(modebuf, sizeof(modebuf)-1, "%d", g_autoMode);
                        _snprintf(intbuf,  sizeof(intbuf)-1,  "%d", g_autoInterval);
                        modebuf[sizeof(modebuf)-1] = '\0';
                        intbuf[sizeof(intbuf)-1]   = '\0';
                        ConfSetOpt(g_conf, "auto_mode",     modebuf);
                        ConfSetOpt(g_conf, "auto_interval", intbuf);
                        ConfSave(g_conf, g_configPath);
                    }

                    EndDialog(hwnd, IDOK);
                    break;
                }

                case IDCANCEL:
                case IDC_AUTO_CANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    break;
            }
            break;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   Main dialog proc
   ----------------------------------------------------------------------- */
static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

        /* ----------------------------------------------------------------
           WM_INITDIALOG
           ---------------------------------------------------------------- */
        case WM_INITDIALOG: {
            LoadConfig(hwnd);
            InitServerListView(hwnd);
            InitTabControl(hwnd);
            InitNeverListView(hwnd);
            InitPlayerListView(hwnd);
            InitSrvInfoView(hwnd);

            /* Connect starts disabled until a server is selected */
            EnableWindow(GetDlgItem(hwnd, IDC_CONNECT), FALSE);

            /* Set window title to show current master */
            {
                char title[256];
                const char *mh = ConfGetOpt(g_conf, "master_host");
                _snprintf(title, sizeof(title)-1, "ezQLaunch  [master: %s]",
                          mh ? mh : DEFAULT_MASTER_HOST);
                title[sizeof(title)-1] = '\0';
                SetWindowTextA(hwnd, title);
            }

            /* Try loading cache */
            if (SL_LoadCache(CACHE_FILE)) {
                Filter_ApplyAll(&g_filters);
                /* Load persistent per-server flags AFTER cache populates g_servers[] */
                SL_LoadFavorites();
                SL_LoadNeverScan();
                RebuildServerList(hwnd);
                RebuildFavoritesList(hwnd);
                SyncNeverTab(hwnd);
                SetStatus(hwnd, "Cache loaded. Press Refresh to re-ping, or Full Update to query master.");
            } else {
                /* No cache yet — still load flags (they'll apply when servers arrive) */
                SL_LoadFavorites();
                SL_LoadNeverScan();
                SyncNeverTab(hwnd);
                SetStatus(hwnd, "No cache found. Press Full Update to query the master server.");
            }

            /* Progress bar: hidden until scan starts */
            ShowWindow(GetDlgItem(hwnd, IDC_PROGRESS), SW_HIDE);

            /* Give Full Update and Refresh BS_PUSHLIKE so BM_SETCHECK can
               depress them visually when auto-scan is running. */
            {
                HWND hU = GetDlgItem(hwnd, IDC_BTN_UPDATE_MASTER);
                HWND hR = GetDlgItem(hwnd, IDC_BTN_REFRESH_CACHE);
                LONG sU = GetWindowLongA(hU, GWL_STYLE);
                LONG sR = GetWindowLongA(hR, GWL_STYLE);
                SetWindowLongA(hU, GWL_STYLE,
                    (sU & ~BS_TYPEMASK) | BS_AUTOCHECKBOX | BS_PUSHLIKE);
                SetWindowLongA(hR, GWL_STYLE,
                    (sR & ~BS_TYPEMASK) | BS_AUTOCHECKBOX | BS_PUSHLIKE);
                SetWindowPos(hU, NULL, 0,0,0,0,
                    SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
                SetWindowPos(hR, NULL, 0,0,0,0,
                    SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            }
            UpdateAutoButtons(hwnd);
            StartAutoTimer(hwnd);

            /* Load shell32 icons onto the small toolbar buttons.
               Shell32 icon indices (these are stable across XP/Vista/7):
                 #20  = clock/timer  (Auto button)
                 #44  = star/favourite
                 #238 = circular arrows / refresh
               LR_SHARED means Windows owns the handle — no cleanup needed. */
            {
                HMODULE hShell = LoadLibraryA("shell32.dll");
                if (hShell) {
                    struct { int id; UINT ico; } map[3];
                    int mi;
                    map[0].id  = IDC_BTN_AUTO;            map[0].ico = 20;
                    map[1].id  = IDC_BTN_FAV_SEL;         map[1].ico = 44;
                    map[2].id  = IDC_BTN_REFRESH_SEL_TB;  map[2].ico = 238;
                    for (mi = 0; mi < 3; mi++) {
                        HICON hIco = (HICON)LoadImage(hShell,
                            MAKEINTRESOURCE(map[mi].ico),
                            IMAGE_ICON, 14, 14, LR_SHARED);
                        if (hIco) {
                            HWND  hB = GetDlgItem(hwnd, map[mi].id);
                            LONG  st = GetWindowLongA(hB, GWL_STYLE);
                            SetWindowLongA(hB, GWL_STYLE, st | BS_ICON);
                            SendMessage(hB, BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIco);
                            SetWindowPos(hB, NULL, 0,0,0,0,
                                SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
                        }
                    }
                    FreeLibrary(hShell);
                }
            }

            /* Selection-sensitive buttons start disabled */
            UpdateSelectionButtons(hwnd, FALSE);

            /* Create a tooltip control and register all small icon buttons */
            {
                HWND hTT;
                TOOLINFOA ti;
                struct { int id; char *tip; } tips[5];
                int ti_i;

                tips[0].id  = IDC_BTN_AUTO;
                tips[0].tip = "Auto-Scan";
                tips[1].id  = IDC_BTN_FAV_SEL;
                tips[1].tip = "Add Server to Favorites";
                tips[2].id  = IDC_BTN_REFRESH_SEL_TB;
                tips[2].tip = "Refresh selected server";
                tips[3].id  = IDC_BTN_UPDATE_MASTER;
                tips[3].tip = "Full Update: query master server and re-ping all servers";
                tips[4].id  = IDC_BTN_REFRESH_CACHE;
                tips[4].tip = "Refresh All: re-ping all cached servers without querying master";

                hTT = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
                          WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                          hwnd, NULL, g_hInst, NULL);

                if (hTT) {
                    SendMessage(hTT, TTM_SETMAXTIPWIDTH, 0, 300);
                    memset(&ti, 0, sizeof(ti));
                    ti.cbSize   = sizeof(TOOLINFOA);
                    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
                    ti.hwnd     = hwnd;
                    ti.hinst    = g_hInst;
                    for (ti_i = 0; ti_i < 5; ti_i++) {
                        ti.uId      = (UINT_PTR)GetDlgItem(hwnd, tips[ti_i].id);
                        ti.lpszText = tips[ti_i].tip;
                        SendMessage(hTT, TTM_ADDTOOLA, 0, (LPARAM)&ti);
                    }
                }
            }

            UpdateMenuCheckmarks(hwnd);

            /* Capture initial client area for resize calculations */
            {
                RECT rc;
                RECT rcSpl;
                GetClientRect(hwnd, &rc);
                g_initW = rc.right;
                g_initH = rc.bottom;
                /* Splitter initial X from the actual splitter control position */
                GetWindowRect(GetDlgItem(hwnd, IDC_SPLITTER), &rcSpl);
                MapWindowPoints(NULL, hwnd, (POINT*)&rcSpl, 2);
                g_splitterX = rcSpl.left;
            }
            return TRUE;
        }

        /* ----------------------------------------------------------------
           WM_COMMAND - buttons
           ---------------------------------------------------------------- */
        case WM_COMMAND:
            switch (LOWORD(wParam)) {

                case IDC_BTN_REFRESH_CACHE:
                    /* Re-ping the cached list (no master query) */
                    if (g_serverCount == 0) {
                        MessageBoxA(hwnd,
                            "No cached servers to refresh.\nUse Full Update to query the master server first.",
                            "qlaunch", MB_ICONINFORMATION);
                        break;
                    }
                    StartScan(hwnd, SCAN_REFRESH_CACHE);
                    break;

                case IDC_BTN_FILTERS:
                    if (DialogBox(g_hInst, MAKEINTRESOURCE(IDD_FILTERS),
                                  hwnd, FilterDlgProc) == IDOK) {
                        Filter_ApplyAll(&g_filters);
                        RebuildServerList(hwnd);
                        UpdateMenuCheckmarks(hwnd);
                    }
                    break;

                case IDC_BTN_ADD_MASTER: {
                    int r = (int)DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ADDMASTER),
                                           hwnd, AddMasterDlgProc);
                    if (r == IDOK) {
                        char title[256];
                        const char *mh = ConfGetOpt(g_conf, "master_host");
                        _snprintf(title, sizeof(title)-1, "ezQLaunch  [master: %s]",
                                  mh ? mh : DEFAULT_MASTER_HOST);
                        title[sizeof(title)-1] = '\0';
                        SetWindowTextA(hwnd, title);
                    }
                    break;
                }

                case IDC_BTN_ENGINE:
                case IDM_APP_SETCLIENT:
                    DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ENGINE),
                              hwnd, EngineDlgProc);
                    break;

                case IDM_APP_SETMASTER: {
                    int r = (int)DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ADDMASTER),
                                           hwnd, AddMasterDlgProc);
                    if (r == IDOK) {
                        char title[256];
                        const char *mh = ConfGetOpt(g_conf, "master_host");
                        _snprintf(title, sizeof(title)-1, "ezQLaunch  [master: %s]",
                                  mh ? mh : DEFAULT_MASTER_HOST);
                        title[sizeof(title)-1] = '\0';
                        SetWindowTextA(hwnd, title);
                    }
                    break;
                }

                case IDM_APP_EXIT:
                    SendMessage(hwnd, WM_CLOSE, 0, 0);
                    break;

                case IDCANCEL: {
                    /* Escape key: deselect all items on the active server list
                       rather than closing the window. */
                    HWND hList = GetActiveServerList(hwnd);
                    int  i, n  = ListView_GetItemCount(hList);
                    for (i = 0; i < n; i++)
                        ListView_SetItemState(hList, i, 0,
                            LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetSelectionMark(hList, -1);
                    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_PLAYERS));
                    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_SRVINFO));
                    UpdateSelectionButtons(hwnd, FALSE);
                    break;
                }

                case IDC_CONNECT:
                case IDM_SRV_CONNECT:
                case IDM_CTX_CONNECT:
                    DoConnect(hwnd);
                    break;

                case IDM_CTX_FAVORITE:
                case IDM_SRV_FAVORITE: {
                    HWND hFavList = GetActiveServerList(hwnd);
                    int  lvIdx2   = ListView_GetSelectionMark(hFavList);
                    int  srvIdx2  = GetContextSrvIdx(hwnd, lvIdx2);
                    if (srvIdx2 >= 0) {
                        BOOL nowFav = SL_ToggleFavorite(g_servers[srvIdx2].addr);
                        RebuildFavoritesList(hwnd);
                        SetStatus(hwnd, nowFav ? "Added to Favorites." : "Removed from Favorites.");
                    } else if (g_activeTab == TAB_FAVORITES && lvIdx2 >= 0) {
                        /* Stub row: not in g_servers[] yet — read addr from ListView text */
                        char stubAddr[22];
                        LVITEM lvi = {0};
                        stubAddr[0] = '\0';
                        lvi.mask       = LVIF_TEXT;
                        lvi.iItem      = lvIdx2;
                        lvi.iSubItem   = COL_SRV_ADDR;
                        lvi.pszText    = stubAddr;
                        lvi.cchTextMax = sizeof(stubAddr);
                        ListView_GetItem(hFavList, &lvi);
                        if (stubAddr[0] != '\0') {
                            SL_ToggleFavorite(stubAddr);
                            RebuildFavoritesList(hwnd);
                            SetStatus(hwnd, "Removed from Favorites.");
                        }
                    }
                    break;
                }

                case IDM_CTX_NEVERSCAN:
                case IDM_SRV_NEVERSCAN: {
                    HWND hNvrList = GetActiveServerList(hwnd);
                    int  iNvr     = -1;
                    while ((iNvr = ListView_GetNextItem(hNvrList, iNvr, LVNI_SELECTED)) >= 0) {
                        int srvIdx3 = GetContextSrvIdx(hwnd, iNvr);
                        if (srvIdx3 >= 0)
                            SL_ToggleNeverScan(g_servers[srvIdx3].addr);
                    }
                    SyncNeverTab(hwnd);
                    break;
                }

                case IDM_CTX_REFRESH_SEL:
                case IDM_SRV_REFRESH_SEL:
                    RefreshSelected(hwnd);
                    break;

                /* ---- Never Ping tab context menu ---- */
                case IDM_CTX_NVR_REMOVE: {
                    HWND hList = GetDlgItem(hwnd, IDC_SERVERS_NVR);
                    int  iNext = -1;
                    /* Collect addresses first, then remove (avoid index shifting) */
                    {
                        char toRemove[64][22];
                        int  removeCount = 0;
                        int  k;
                        while (removeCount < 64) {
                            LVITEM lvi = {0};
                            char   addr[22];
                            iNext = ListView_GetNextItem(hList, iNext, LVNI_SELECTED);
                            if (iNext < 0) break;
                            addr[0] = '\0';
                            lvi.mask       = LVIF_TEXT;
                            lvi.iItem      = iNext;
                            lvi.iSubItem   = 0;
                            lvi.pszText    = addr;
                            lvi.cchTextMax = sizeof(addr);
                            if (ListView_GetItem(hList, &lvi) && addr[0])
                                strncpy(toRemove[removeCount++], addr, 21);
                        }
                        for (k = 0; k < removeCount; k++)
                            SL_RemoveNeverScan(toRemove[k]);
                    }
                    SyncNeverTab(hwnd);
                    break;
                }

                case IDM_CTX_NVR_REMOVEALL:
                    SL_ClearNeverScan();
                    SyncNeverTab(hwnd);
                    break;

                case IDC_BTN_FAV_SEL: {
                    char addr[22];
                    addr[0] = '\0';
                    if (GetSelectedAddr(hwnd, addr, sizeof(addr)) && addr[0] != '\0') {
                        BOOL nowFav = SL_ToggleFavorite(addr);
                        RebuildFavoritesList(hwnd);
                        SetStatus(hwnd, nowFav ? "Added to Favorites." : "Removed from Favorites.");
                    }
                    break;
                }

                case IDC_BTN_REFRESH_SEL_TB:
                    RefreshSelected(hwnd);
                    break;

                case IDC_BTN_AUTO:
                    if (DialogBoxParamA(g_hInst, MAKEINTRESOURCEA(IDD_AUTO),
                                        hwnd, AutoDlgProc, 0) == IDOK) {
                        UpdateAutoButtons(hwnd);
                        StartAutoTimer(hwnd);
                    } else {
                        /* Cancelled — revert clock button to reflect actual state */
                        SendMessage(GetDlgItem(hwnd, IDC_BTN_AUTO), BM_SETCHECK,
                            g_autoMode != AUTO_MODE_OFF ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                    break;

                case IDC_BTN_UPDATE_MASTER:
                case IDM_SRV_FULLUPDATE:
                    StartScan(hwnd, SCAN_FULL_UPDATE);
                    break;

                case IDM_SRV_REFRESHALL:
                    if (g_serverCount == 0) {
                        MessageBoxA(hwnd,
                            "No cached servers to refresh.\nUse Full Update to query the master server first.",
                            "qlaunch", MB_ICONINFORMATION);
                        break;
                    }
                    StartScan(hwnd, SCAN_REFRESH_CACHE);
                    break;

                case IDM_SRV_ADDMANUAL: {
                    char addr[22];
                    addr[0] = '\0';
                    if (DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_ADDSERVER),
                                       hwnd, AddServerDlgProc, (LPARAM)addr) == IDOK
                        && addr[0] != '\0') {
                        SL_AddFavoriteAddr(addr);
                        RebuildFavoritesList(hwnd);
                        SetStatus(hwnd, "Server added to Favorites.");
                    }
                    break;
                }

                /* ---- View menu — sort servers ---- */
                case IDM_VIEW_SORT_SRV_NAME:    SortByColumn(hwnd, COL_SRV_NAME);    break;
                case IDM_VIEW_SORT_SRV_ADDR:    SortByColumn(hwnd, COL_SRV_ADDR);    break;
                case IDM_VIEW_SORT_SRV_MAP:     SortByColumn(hwnd, COL_SRV_MAP);     break;
                case IDM_VIEW_SORT_SRV_PLAYERS: SortByColumn(hwnd, COL_SRV_PLAYERS); break;
                case IDM_VIEW_SORT_SRV_PING:    SortByColumn(hwnd, COL_SRV_PING);    break;
                case IDM_VIEW_SORT_SRV_LAST:    SortByColumn(hwnd, COL_SRV_LAST);    break;

                /* ---- View menu — sort players ---- */
                /* IDM_VIEW_SORT_PLR_TEAM removed — color sort dropped per GameSpy parity */
                case IDM_VIEW_SORT_PLR_NAME:    g_plrSortCol = COL_PLR_NAME;   SortPlayers(hwnd); UpdateMenuCheckmarks(hwnd); break;
                case IDM_VIEW_SORT_PLR_SCORE:   g_plrSortCol = COL_PLR_FRAGS;  SortPlayers(hwnd); UpdateMenuCheckmarks(hwnd); break;
                case IDM_VIEW_SORT_PLR_TIME:    g_plrSortCol = COL_PLR_TIME;   SortPlayers(hwnd); UpdateMenuCheckmarks(hwnd); break;
                case IDM_VIEW_SORT_PLR_PING:    g_plrSortCol = COL_PLR_PING;   SortPlayers(hwnd); UpdateMenuCheckmarks(hwnd); break;
                case IDM_VIEW_SORT_PLR_SKIN:    g_plrSortCol = COL_PLR_SKIN;   SortPlayers(hwnd); UpdateMenuCheckmarks(hwnd); break;

                /* ---- View menu — hide bottom panes ---- */
                case IDM_VIEW_HIDE_BOTTOM:
                    g_hidePanes = !g_hidePanes;
                    ApplyHideBottomPanes(hwnd);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                /* ---- Filters menu ---- */
                case IDM_FILTER_TF_ONLY:
                    g_filters.tfOnly = !g_filters.tfOnly;
                    Filter_ApplyAll(&g_filters);
                    RebuildServerList(hwnd);
                    UpdatePlayerSkinHeader(hwnd);
                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                case IDM_FILTER_HIDE_EMPTY:
                    g_filters.hideEmpty = !g_filters.hideEmpty;
                    Filter_ApplyAll(&g_filters);
                    RebuildServerList(hwnd);
                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                case IDM_FILTER_HIDE_FULL:
                    g_filters.hideFull = !g_filters.hideFull;
                    Filter_ApplyAll(&g_filters);
                    RebuildServerList(hwnd);
                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                case IDM_FILTER_HIDE_NOTEMPTY:
                    g_filters.hideNotEmpty = !g_filters.hideNotEmpty;
                    Filter_ApplyAll(&g_filters);
                    RebuildServerList(hwnd);
                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                case IDM_FILTER_HIDE_HIGHPING:
                    g_filters.hideHighPing = !g_filters.hideHighPing;
                    Filter_ApplyAll(&g_filters);
                    RebuildServerList(hwnd);
                    Filter_Save(&g_filters, g_conf);
                    ConfSave(g_conf, g_configPath);
                    UpdateMenuCheckmarks(hwnd);
                    break;

                /* ---- About ---- */
                /* ---- About ---- */
                case IDM_ABOUT:
                    MessageBoxA(hwnd,
                        "ezQLaunch v1.1\r\n"
                        "A QuakeWorld server browser.\r\n\r\n"
                        "Running in: Standalone Mode\r\n\r\n"
                        "Designed for use with ezQWTF Client as a\r\n"
                        "background service.\r\n\r\n"
                        "Forked from QLaunch v1.0 (MIT lic.) created by\r\n"
                        "Cory Nelson, int64.org\r\n\r\n"
                        "ezQLaunch fork is maintained by\r\n"
                        "Jordan Siegler @ iKM Media, for the\r\n"
                        "QuakeWorld Team Fortress Unification Project.\r\n",
                        "About ezQLaunch",
                        MB_ICONINFORMATION | MB_OK);
                    break;
            }
            break;

        /* ----------------------------------------------------------------
           WM_CONTEXTMENU - right-click on a server ListView
           ---------------------------------------------------------------- */
        case WM_CONTEXTMENU: {
            HWND hSrc = (HWND)wParam;
            HWND hAllList = GetDlgItem(hwnd, IDC_SERVERS);
            /* Only show context menu on the server ListViews */
            if (hSrc == hAllList ||
                hSrc == GetDlgItem(hwnd, IDC_SERVERS_FAV) ||
                hSrc == GetDlgItem(hwnd, IDC_SERVERS_HIST) ||
                hSrc == GetDlgItem(hwnd, IDC_SERVERS_NVR)) {
                POINT pt;
                int   lvIdx;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                /* If right-click came from keyboard (pt == -1,-1), use focus item */
                if (pt.x == -1 && pt.y == -1) {
                    lvIdx = ListView_GetSelectionMark(hSrc);
                    pt.x = 0; pt.y = 0;
                    ClientToScreen(hSrc, &pt);
                } else {
                    POINT cpt = pt;
                    ScreenToClient(hSrc, &cpt);
                    {
                        LVHITTESTINFO ht = {{0}};
                        ht.pt = cpt;
                        lvIdx = ListView_HitTest(hSrc, &ht);
                    }
                }
                /* Show context menu on any server ListView tab */
                ShowContextMenu(hwnd, lvIdx, pt);
            }
            break;
        }

        /* ----------------------------------------------------------------
           WM_NOTIFY - ListView and Tab events
           ---------------------------------------------------------------- */
        case WM_NOTIFY: {
            NMHDR *nmh = (NMHDR*)lParam;

            /* ---- Tab control: tab switch ---- */
            if (nmh->idFrom == IDC_TAB && nmh->code == TCN_SELCHANGE) {
                g_activeTab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB));
                ShowActiveTab(hwnd);
                if (g_activeTab == TAB_FAVORITES)
                    RebuildFavoritesList(hwnd);
                else if (g_activeTab == TAB_HISTORY)
                    RebuildHistoryList(hwnd);
                else if (g_activeTab == TAB_NEVERSCAN)
                    RebuildNeverList(hwnd);
                /* Clear player list when switching tabs */
                ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_PLAYERS));
                break;
            }

            if (nmh->idFrom == IDC_SERVERS ||
                nmh->idFrom == IDC_SERVERS_FAV ||
                nmh->idFrom == IDC_SERVERS_HIST ||
                nmh->idFrom == IDC_SERVERS_NVR) {
                switch (nmh->code) {
                    case NM_CLICK: {
                        NMITEMACTIVATE *nmia = (NMITEMACTIVATE*)nmh;
                        if (nmia->iItem == -1) {
                            /* Click on empty list area — deselect everything */
                            HWND hList = GetActiveServerList(hwnd);
                            int  i, n  = ListView_GetItemCount(hList);
                            for (i = 0; i < n; i++)
                                ListView_SetItemState(hList, i, 0,
                                    LVIS_SELECTED | LVIS_FOCUSED);
                            ListView_SetSelectionMark(hList, -1);
                            ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_PLAYERS));
                            ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_SRVINFO));
                            UpdateSelectionButtons(hwnd, FALSE);
                        } else {
                            RefreshPlayersFromActiveTab(hwnd);
                        }
                        break;
                    }
                    case NM_DBLCLK:
                        DoConnect(hwnd);
                        break;
                    case LVN_COLUMNCLICK:
                        SortByColumn(hwnd, ((NMLISTVIEW*)nmh)->iSubItem);
                        break;
                    case LVN_ITEMCHANGED: {
                        /* Keyboard up/down navigation: update players when
                           selection changes (only care about gaining selection).
                           Suppressed during sort — ListView_SortItemsEx fires
                           hundreds of these as it reorders items. */
                        NMLISTVIEW *nmlv = (NMLISTVIEW*)nmh;
                        if (!g_sorting &&
                            (nmlv->uNewState & LVIS_SELECTED) &&
                            !(nmlv->uOldState & LVIS_SELECTED)) {
                            RefreshPlayersFromActiveTab(hwnd);
                        }
                        break;
                    }
                }
            }
            /* Header column resize — fired when user finishes dragging a divider
               or double-clicks it for auto-fit. Sync widths to all server tabs. */
            else if (nmh->code == HDN_ENDTRACKA    || nmh->code == HDN_ENDTRACKW ||
                     nmh->code == HDN_DIVIDERDBLCLICKA || nmh->code == HDN_DIVIDERDBLCLICKW) {
                HWND hLists[4];
                int  li;
                hLists[0] = GetDlgItem(hwnd, IDC_SERVERS);
                hLists[1] = GetDlgItem(hwnd, IDC_SERVERS_FAV);
                hLists[2] = GetDlgItem(hwnd, IDC_SERVERS_HIST);
                hLists[3] = GetDlgItem(hwnd, IDC_SERVERS_NVR);
                for (li = 0; li < 4; li++) {
                    if (nmh->hwndFrom == ListView_GetHeader(hLists[li])) {
                        /* Post so the ListView has finished applying the new
                           width before we read it back */
                        PostMessage(hwnd, WM_APP + 10, (WPARAM)li, 0);
                        break;
                    }
                }
            }
            else if (nmh->idFrom == IDC_PLAYERS) {
                if (nmh->code == LVN_COLUMNCLICK) {
                    int col = ((NMLISTVIEW*)nmh)->iSubItem;
                    /* Ignore the hidden team column */
                    if (col == COL_PLR_TEAM) break;
                    if (col == g_plrSortCol)
                        g_plrSortAsc = !g_plrSortAsc;
                    else {
                        g_plrSortCol = col;
                        g_plrSortAsc = TRUE;
                    }
                    SortPlayers(hwnd);
                    break;
                }
                if (nmh->code == NM_CUSTOMDRAW) {
                    NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW*)nmh;
                    switch (cd->nmcd.dwDrawStage) {
                        case CDDS_PREPAINT:
                            SetWindowLong(hwnd, DWL_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                            return TRUE;

                        case CDDS_ITEMPREPAINT: {
                            int row    = (int)cd->nmcd.lItemlParam;
                            HWND hActiveSrv = GetActiveServerList(hwnd);
                            int  lvIdx  = ListView_GetSelectionMark(hActiveSrv);
                            int  srvIdx = GetContextSrvIdx(hwnd, lvIdx);
                            if (srvIdx >= 0 &&
                                row >= 0 && row < g_servers[srvIdx].numplayers_detail) {
                                int tc = g_servers[srvIdx].players_detail[row].topcolor;
                                cd->clrTextBk = PlayerRowColor(tc);
                                cd->clrText   = RGB(0, 0, 0);
                            }
                            /* Request subitem notification; do NOT set CDRF_NEWFONT
                               as it causes unnecessary redraws */
                            SetWindowLong(hwnd, DWL_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                            return TRUE;
                        }

                        case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                            /* Only custom-draw col 0 "Player Name" (swatches + name).
                               All other columns use default rendering. */
                            if (cd->iSubItem != COL_PLR_NAME) {
                                SetWindowLong(hwnd, DWL_MSGRESULT, CDRF_DODEFAULT);
                                return TRUE;
                            }
                            {
                                int  row    = (int)cd->nmcd.lItemlParam;
                                HWND hActiveSrv2 = GetActiveServerList(hwnd);
                                int  lvIdx  = ListView_GetSelectionMark(hActiveSrv2);
                                int  tc = 0, bc = 0;
                                HDC  hdc = cd->nmcd.hdc;
                                RECT rc;
                                int  sw, sh, x, y;
                                char label[4];
                                HFONT hOldFont, hSmall;
                                LOGFONTA lf;
                                COLORREF swColor;
                                HBRUSH hBr;
                                RECT sr;
                                char nameText[64];
                                RECT nameRc;
                                int  swatchW;

                                ListView_GetSubItemRect(nmh->hwndFrom,
                                    (int)cd->nmcd.dwItemSpec,
                                    COL_PLR_NAME, LVIR_BOUNDS, &rc);

                                if (lvIdx >= 0) {
                                    int srvIdx = GetContextSrvIdx(hwnd, lvIdx);
                                    if (srvIdx >= 0 &&
                                        row >= 0 && row < g_servers[srvIdx].numplayers_detail) {
                                        tc = g_servers[srvIdx].players_detail[row].topcolor;
                                        bc = g_servers[srvIdx].players_detail[row].bottomcolor;
                                    }
                                }

                                /* Fill entire cell with row background color */
                                hBr = CreateSolidBrush(PlayerRowColor(tc));
                                FillRect(hdc, &rc, hBr);
                                DeleteObject(hBr);

                                /* Two square swatches sized to row height */
                                sh = rc.bottom - rc.top - 4;
                                if (sh < 6)  sh = 6;
                                if (sh > 16) sh = 16;
                                sw = sh;
                                y  = rc.top + (rc.bottom - rc.top - sh) / 2;
                                x  = rc.left + 3;

                                /* Small bold font for color number labels in swatches */
                                memset(&lf, 0, sizeof(lf));
                                lf.lfHeight = -(sh - 3);
                                if (lf.lfHeight > -5) lf.lfHeight = -5;
                                lf.lfWeight = FW_BOLD;
                                strncpy(lf.lfFaceName, "Arial", LF_FACESIZE - 1);
                                hSmall   = CreateFontIndirectA(&lf);
                                hOldFont = (HFONT)SelectObject(hdc, hSmall);
                                SetBkMode(hdc, TRANSPARENT);

                                /* Topcolor swatch */
                                swColor = (tc >= 0 && tc <= 16) ? QW_COLORS[tc] : RGB(180,180,180);
                                hBr = CreateSolidBrush(swColor);
                                sr.left = x; sr.top = y; sr.right = x+sw; sr.bottom = y+sh;
                                FillRect(hdc, &sr, hBr);
                                DeleteObject(hBr);
                                FrameRect(hdc, &sr, (HBRUSH)GetStockObject(BLACK_BRUSH));
                                {
                                    int lum = ((int)GetRValue(swColor)*3 + (int)GetGValue(swColor)*6 + (int)GetBValue(swColor)) / 10;
                                    SetTextColor(hdc, lum > 128 ? RGB(0,0,0) : RGB(255,255,255));
                                }
                                _snprintf(label, sizeof(label)-1, "%d", tc);
                                label[sizeof(label)-1] = '\0';
                                DrawTextA(hdc, label, -1, &sr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

                                /* Bottomcolor swatch */
                                x += sw + 2;
                                swColor = (bc >= 0 && bc <= 16) ? QW_COLORS[bc] : RGB(180,180,180);
                                hBr = CreateSolidBrush(swColor);
                                sr.left = x; sr.top = y; sr.right = x+sw; sr.bottom = y+sh;
                                FillRect(hdc, &sr, hBr);
                                DeleteObject(hBr);
                                FrameRect(hdc, &sr, (HBRUSH)GetStockObject(BLACK_BRUSH));
                                {
                                    int lum = ((int)GetRValue(swColor)*3 + (int)GetGValue(swColor)*6 + (int)GetBValue(swColor)) / 10;
                                    SetTextColor(hdc, lum > 128 ? RGB(0,0,0) : RGB(255,255,255));
                                }
                                _snprintf(label, sizeof(label)-1, "%d", bc);
                                label[sizeof(label)-1] = '\0';
                                DrawTextA(hdc, label, -1, &sr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);

                                SelectObject(hdc, hOldFont);
                                DeleteObject(hSmall);

                                /* Draw player name text to the right of the swatches,
                                   using the default list font (already selected in hdc). */
                                swatchW = 3 + sw + 2 + sw + 4;
                                nameRc.left   = rc.left + swatchW;
                                nameRc.right  = rc.right - 2;
                                nameRc.top    = rc.top;
                                nameRc.bottom = rc.bottom;
                                {
                                    LVITEM lviN = {0};
                                    nameText[0] = '\0';
                                    lviN.mask       = LVIF_TEXT;
                                    lviN.iItem      = (int)cd->nmcd.dwItemSpec;
                                    lviN.iSubItem   = COL_PLR_NAME;
                                    lviN.pszText    = nameText;
                                    lviN.cchTextMax = sizeof(nameText);
                                    ListView_GetItem(nmh->hwndFrom, &lviN);
                                }
                                SetTextColor(hdc, RGB(0, 0, 0));
                                SetBkMode(hdc, TRANSPARENT);
                                DrawTextA(hdc, nameText, -1, &nameRc,
                                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                                SetWindowLong(hwnd, DWL_MSGRESULT, CDRF_SKIPDEFAULT);
                                return TRUE;
                            }
                        }
                    }
                }
            }
            break;
        }

        /* ----------------------------------------------------------------
           WM_TIMER - IDT_SCAN_POLL: poll g_servers during a full scan.
           Fires every 250ms. Rebuilds the server list from whatever has
           been written by worker threads so far, and updates the progress
           bar from ctx->nDone / ctx->nTotal.
           ---------------------------------------------------------------- */
        case WM_TIMER: {
            if (wParam == IDT_SCAN_POLL && g_scanning && g_pingCtx) {
                LONG nDone  = g_pingCtx->nDone;
                LONG nTotal = g_pingCtx->nTotal;

                /* Rebuild All list from current g_servers state */
                RebuildServerList(hwnd);
                RestoreSavedSelection(hwnd);
                SL_ReapplyFavorites();
                RebuildFavoritesList(hwnd);

                /* Update progress bar */
                if (nTotal > 0) {
                    int pct = (int)((nDone * 100L) / nTotal);
                    if (pct > 100) pct = 100;
                    SendMessage(GetDlgItem(hwnd, IDC_PROGRESS),
                                PBM_SETPOS, (WPARAM)pct, 0);
                }

                {
                    char buf[64];
                    _snprintf(buf, 63, "Pinging... %ld / %ld", nDone, nTotal);
                    buf[63] = '\0';
                    SetStatus(hwnd, buf);
                }
            }
            /* Auto-scan interval timer — fire a scan when not already scanning */
            if (wParam == IDT_AUTO && !g_scanning) {
                if (g_autoMode == AUTO_MODE_UPDATE)
                    StartScan(hwnd, SCAN_FULL_UPDATE);
                else if (g_autoMode == AUTO_MODE_REFRESH)
                    StartScan(hwnd, SCAN_REFRESH_CACHE);
            }
            break;
        }

        /* ----------------------------------------------------------------
        /* WM_APP+10: deferred column-width sync after HDN_ENDTRACK/DIVIDERDBLCLICK.
           wParam = index into hLists[] of the source ListView. */
        case WM_APP + 10: {
            HWND hLists[4];
            hLists[0] = GetDlgItem(hwnd, IDC_SERVERS);
            hLists[1] = GetDlgItem(hwnd, IDC_SERVERS_FAV);
            hLists[2] = GetDlgItem(hwnd, IDC_SERVERS_HIST);
            hLists[3] = GetDlgItem(hwnd, IDC_SERVERS_NVR);
            if ((int)wParam >= 0 && (int)wParam < 4)
                SyncSrvColWidths(hwnd, hLists[(int)wParam]);
            break;
        }

        /* ----------------------------------------------------------------
           WM_APP_SERVERINFO - single server refreshed (RefreshSel only).
           NOT used for full scans — those use the IDT_SCAN_POLL timer.
           wParam: high 16 = scan serial, low 16 = g_servers[] index
           ---------------------------------------------------------------- */
        case WM_APP_SERVERINFO: {
            int  msgSerial = (int)((wParam >> 16) & 0xFFFF);
            int  idx       = (int)(wParam & 0xFFFF);
            SERVER_ENTRY *e = (SERVER_ENTRY*)lParam;

            if (!e) break;

            /* Discard if from a superseded scan */
            if (msgSerial != (int)(g_scanSerial & 0xFFFF)) {
                free(e);
                break;
            }

            EnterCriticalSection(&g_listLock);
            if (idx < 0 || idx >= g_serverCount) {
                LeaveCriticalSection(&g_listLock);
                free(e);
                break;
            }
            g_servers[idx] = *e;
            LeaveCriticalSection(&g_listLock);
            free(e);

            /* Refresh the UI for this one server */
            RebuildServerList(hwnd);
            RestoreSavedSelection(hwnd);
            g_savedSelAddr[0] = '\0';
            RebuildFavoritesList(hwnd);
            RefreshPlayersFromActiveTab(hwnd);
            SetStatus(hwnd, "Refresh complete.");
            break;
        }

        /* ----------------------------------------------------------------
           WM_APP_SCAN_DONE - worker thread finished
           ---------------------------------------------------------------- */
        case WM_APP_SCAN_DONE: {
            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);

            KillTimer(hwnd, IDT_SCAN_POLL);
            SendMessage(hProg, PBM_SETPOS, 100, 0);
            ShowWindow(hProg, SW_HIDE);

            g_scanning   = FALSE;
            g_pingCtx    = NULL;
            if (g_pingThread) {
                CloseHandle(g_pingThread);
                g_pingThread = NULL;
            }

            EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE_MASTER), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH_CACHE), TRUE);
            UpdateAutoButtons(hwnd);  /* restore depressed state if auto-scan is active */

            /* Final rebuild — gets anything that arrived since last timer tick */
            RebuildServerList(hwnd);
            SL_ReapplyFavorites();   /* re-stamp isFavorite after scan rebuilt g_servers[] */
            RebuildFavoritesList(hwnd);
            RebuildHistoryList(hwnd);

            /* Re-apply sort to all three lists now that all data is in */
            if (g_sortCol >= 0) {
                HWND hAll  = GetDlgItem(hwnd, IDC_SERVERS);
                HWND hFav  = GetDlgItem(hwnd, IDC_SERVERS_FAV);
                HWND hHist = GetDlgItem(hwnd, IDC_SERVERS_HIST);
                g_sorting = TRUE;
                ListView_SortItemsEx(hAll,  SrvSortCmp, (LPARAM)hAll);
                SrvSort_Apply(hAll);
                ListView_SortItemsEx(hFav,  SrvSortCmp, (LPARAM)hFav);
                SrvSort_Apply(hFav);
                ListView_SortItemsEx(hHist, SrvSortCmp, (LPARAM)hHist);
                SrvSort_Apply(hHist);
                g_sorting = FALSE;
            }

            {
                char buf[64];
                _snprintf(buf, 63, "Done. %d servers shown.", g_itemCount);
                buf[63] = '\0';
                SetStatus(hwnd, buf);
            }

            /* Restore previously selected server if it's still in the list */
            RestoreSavedSelection(hwnd);
            g_savedSelAddr[0] = '\0';
            RefreshPlayersFromActiveTab(hwnd);
            break;
        }

        /* ----------------------------------------------------------------
           WM_APP_STATUS - status string update from worker thread
           ---------------------------------------------------------------- */
        case WM_APP_STATUS: {
            char *msg = (char*)lParam;
            if (msg) {
                SetStatus(hwnd, msg);
                free(msg);
            }
            break;
        }

        /* ----------------------------------------------------------------
           WM_GETMINMAXINFO - enforce a minimum window size
           ---------------------------------------------------------------- */
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO*)lParam;
            mm->ptMinTrackSize.x = 520;
            mm->ptMinTrackSize.y = 300;
            return TRUE;
        }

        /* ----------------------------------------------------------------
           WM_SIZE - resize controls for new layout:
             Top: tab + server ListViews   full width, stretch R+B
             Bottom-left: players          stretch bottom only; right follows splitter
             Splitter: moves with right edge of players
             Bottom-right: srvinfo         stretch R+B; left follows splitter
             Progress + status: full width at bottom
           ---------------------------------------------------------------- */
        case WM_SIZE: {
            /* Controls that simply stretch/move by delta (no splitter logic) */
            static const int anchorIds[]    = { IDC_TAB, IDC_SERVERS, IDC_SERVERS_FAV, IDC_SERVERS_HIST, IDC_SERVERS_NVR, IDC_PROGRESS, IDC_STATUS_BAR, 0 };
            static const int anchorRight[]  = { 1, 1, 1, 1, 1, 1, 1 };
            static const int anchorBottom[] = { 0, 0, 0, 0, 0, 0, 0 };
            static const int anchorMoveX[]  = { 0, 0, 0, 0, 0, 0, 0 };
            static const int anchorMoveY[]  = { 0, 0, 0, 0, 0, 1, 1 };

            int cw = LOWORD(lParam);
            int ch = HIWORD(lParam);
            int dw, dh, i;
            HDWP hdwp;

            if (g_initW == 0 || g_initH == 0) break;
            if (wParam == SIZE_MINIMIZED) break;

            dw = cw - g_initW;
            dh = ch - g_initH;

            hdwp = BeginDeferWindowPos(10);
            if (!hdwp) break;

            for (i = 0; anchorIds[i] != 0; i++) {
                HWND hCtrl = GetDlgItem(hwnd, anchorIds[i]);
                RECT rc;
                int  x, y, w, h;
                GetWindowRect(hCtrl, &rc);
                MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
                x = rc.left;
                y = rc.top;
                w = rc.right  - rc.left;
                h = rc.bottom - rc.top;
                if (anchorMoveX[i])   x += dw;
                if (anchorMoveY[i])   y += dh;
                if (anchorRight[i])   w += dw;
                if (anchorBottom[i])  h += dh;
                hdwp = DeferWindowPos(hdwp, hCtrl, NULL, x, y, w, h,
                                      SWP_NOZORDER | SWP_NOACTIVATE);
            }

            EndDeferWindowPos(hdwp);

            /* Bottom panels: grow taller; splitter stays at same relative X */
            {
                HWND hPly = GetDlgItem(hwnd, IDC_PLAYERS);
                RECT rcPly;
                GetWindowRect(hPly, &rcPly);
                MapWindowPoints(NULL, hwnd, (POINT*)&rcPly, 2);
                /* Grow height of all three bottom panels */
                {
                    HWND hSrv = GetDlgItem(hwnd, IDC_SRVINFO);
                    HWND hSpl = GetDlgItem(hwnd, IDC_SPLITTER);
                    RECT rcSrv, rcSpl;
                    int  newH;
                    GetWindowRect(hSrv, &rcSrv); MapWindowPoints(NULL, hwnd, (POINT*)&rcSrv, 2);
                    GetWindowRect(hSpl, &rcSpl); MapWindowPoints(NULL, hwnd, (POINT*)&rcSpl, 2);
                    newH = (rcPly.bottom - rcPly.top) + dh;
                    if (newH < 30) newH = 30;
                    hdwp = BeginDeferWindowPos(3);
                    if (hdwp) {
                        hdwp = DeferWindowPos(hdwp, hPly, NULL,
                            rcPly.left, rcPly.top,
                            rcPly.right - rcPly.left, newH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        hdwp = DeferWindowPos(hdwp, hSpl, NULL,
                            rcSpl.left, rcSpl.top,
                            SPLITTER_W, newH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        hdwp = DeferWindowPos(hdwp, hSrv, NULL,
                            rcSrv.left, rcSrv.top,
                            (cw - 5) - rcSrv.left, newH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        EndDeferWindowPos(hdwp);
                    }
                }
            }

            g_initW = cw;
            g_initH = ch;
            break;
        }

        /* ----------------------------------------------------------------
           Splitter drag — WM_SETCURSOR, WM_LBUTTONDOWN, WM_MOUSEMOVE,
           WM_LBUTTONUP
           ---------------------------------------------------------------- */
        case WM_SETCURSOR: {
            /* Show resize cursor when over the splitter */
            HWND hSpl = GetDlgItem(hwnd, IDC_SPLITTER);
            if ((HWND)wParam == hSpl) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                SetWindowLong(hwnd, DWL_MSGRESULT, TRUE);
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            HWND hSpl = GetDlgItem(hwnd, IDC_SPLITTER);
            POINT pt;
            RECT  rcSpl;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            GetWindowRect(hSpl, &rcSpl);
            MapWindowPoints(NULL, hwnd, (POINT*)&rcSpl, 2);
            if (pt.x >= rcSpl.left - 2 && pt.x <= rcSpl.right + 2 &&
                pt.y >= rcSpl.top  && pt.y <= rcSpl.bottom) {
                g_splDragging = TRUE;
                g_splDragOff  = pt.x - rcSpl.left;
                SetCapture(hwnd);
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            }
            break;
        }

        case WM_MOUSEMOVE: {
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            if (g_splDragging) {
                MoveSplitter(hwnd, pt.x - g_splDragOff);
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            } else {
                /* Show resize cursor on hover near splitter even before clicking */
                HWND hSpl = GetDlgItem(hwnd, IDC_SPLITTER);
                RECT rcSpl;
                GetWindowRect(hSpl, &rcSpl);
                MapWindowPoints(NULL, hwnd, (POINT*)&rcSpl, 2);
                if (pt.x >= rcSpl.left - 2 && pt.x <= rcSpl.right + 2 &&
                    pt.y >= rcSpl.top       && pt.y <= rcSpl.bottom) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                }
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (g_splDragging) {
                g_splDragging = FALSE;
                ReleaseCapture();
            }
            break;
        }

        /* ----------------------------------------------------------------
           WM_APP_REFRESH_DONE - selective refresh finished (not a full scan)
           ---------------------------------------------------------------- */
        case WM_APP_REFRESH_DONE:
            SetStatus(hwnd, "Refresh complete.");
            break;

        /* ----------------------------------------------------------------
           WM_CLOSE / WM_DESTROY
           ---------------------------------------------------------------- */
        case WM_CLOSE:
            if (g_scanning) {
                StopScan(hwnd);
            }
            StopAutoTimer(hwnd);
            Filter_Save(&g_filters, g_conf);
            {
                char buf[16];
                _snprintf(buf, sizeof(buf)-1, "%d", g_sortCol); buf[sizeof(buf)-1] = '\0';
                ConfSetOpt(g_conf, "sort_col", buf);
                _snprintf(buf, sizeof(buf)-1, "%d", g_sortAsc ? 1 : 0); buf[sizeof(buf)-1] = '\0';
                ConfSetOpt(g_conf, "sort_asc", buf);
            }
            ConfSave(g_conf, g_configPath);
            EndDialog(hwnd, 0);
            break;

        case WM_DESTROY:
            if (g_conf) {
                ConfDestroy(g_conf);
                g_conf = NULL;
            }
            break;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   WinMain
   ----------------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    g_hInst = hInstance;

    /* Set working directory to the folder containing the executable so that
       all relative paths (ezqlaunch.conf, servers.cache, etc.) resolve correctly
       regardless of how the app was launched (shortcut, shell, etc.). */
    {
        char exePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
            char *lastSep = strrchr(exePath, '\\');
            if (lastSep) {
                *lastSep = '\0';
                SetCurrentDirectoryA(exePath);
                /* Build absolute config path so ConfSave/Load always hits the
                   correct file even if a child dialog (e.g. GetOpenFileNameA
                   without OFN_NOCHANGEDIR) changes the working directory. */
                _snprintf(g_configPath, MAX_PATH - 1, "%s\\%s", exePath, CONFIG_FILE);
                g_configPath[MAX_PATH - 1] = '\0';
            }
        }
    }

    /* Parse --pipe <name> from the MBCS command line.
       lpCmdLine does not include argv[0] (the exe path). */
    {
        char *p = lpCmdLine;
        while (p && *p) {
            /* skip leading spaces */
            while (*p == ' ') p++;
            if (strncmp(p, "--pipe", 6) == 0 && (p[6] == ' ' || p[6] == '\0')) {
                p += 6;
                while (*p == ' ') p++;
                if (*p) {
                    /* argument runs to end of string or next space-delimited token.
                       Pipe names don't contain spaces, so take to end of string. */
                    g_pipeName = _strdup(p);
                    break;
                }
            }
            /* advance past this token */
            while (*p && *p != ' ') p++;
        }
    }

    InitCommonControls();
    { WSADATA wd; WSAStartup(MAKEWORD(1,1), &wd); }
    SL_Init();

    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN), NULL, DlgProc);

    SL_Destroy();
    WSACleanup();

    if (g_pipeName) free(g_pipeName);

    return 0;
}
