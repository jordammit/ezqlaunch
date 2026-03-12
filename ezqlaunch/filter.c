/*
    ezqlaunch - filter.c
*/
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif
#ifndef _MBCS
#define _MBCS
#endif
#define snprintf _snprintf
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "filter.h"
#include "conf.h"

FILTER_STATE g_filters = {
    TRUE,   /* hideEmpty     */
    FALSE,  /* hideFull      */
    FALSE,  /* hideNotEmpty  */
    FALSE,  /* hideHighPing  */
    150,    /* pingLimit     */
    FALSE,  /* filterMap     */
    "",     /* mapFilter     */
    TRUE    /* tfOnly        */
};

BOOL Filter_Pass(const SERVER_ENTRY *e, const FILTER_STATE *f) {
    /* Always hide dead/unreachable servers */
    if (e->state == SRV_DEAD) return FALSE;
    /* Still being queried: hide until known */
    if (e->state == SRV_UNKNOWN) return FALSE;

    if (f->hideEmpty && e->players == 0) return FALSE;
    if (f->hideFull  && e->maxplayers > 0 && e->players >= e->maxplayers) return FALSE;
    if (f->hideNotEmpty && e->players > 0) return FALSE;
    if (f->hideHighPing && e->ping > f->pingLimit) return FALSE;

    if (f->tfOnly) {
        if (_stricmp(e->gamedir, "fortress") != 0) return FALSE;
    }

    if (f->filterMap && f->mapFilter[0] != '\0') {
        /* Case-insensitive substring match */
        char lmap[MAX_MAPNAME], lfilt[MAX_MAPNAME];
        int i;
        strncpy(lmap,  e->map,       sizeof(lmap)  - 1); lmap[sizeof(lmap)-1]   = '\0';
        strncpy(lfilt, f->mapFilter, sizeof(lfilt) - 1); lfilt[sizeof(lfilt)-1] = '\0';
        for (i = 0; lmap[i];  i++) if (lmap[i]  >= 'A' && lmap[i]  <= 'Z') lmap[i]  += 32;
        for (i = 0; lfilt[i]; i++) if (lfilt[i] >= 'A' && lfilt[i] <= 'Z') lfilt[i] += 32;
        if (!strstr(lmap, lfilt)) return FALSE;
    }

    return TRUE;
}

/* MBCS build: TCHAR == char, LPCTSTR == const char*, _T() is a no-op,
   _tcstol == strtol, _stprintf == sprintf.  No wide-char conversions needed. */

void Filter_Load(FILTER_STATE *f, CONF *conf) {
    const char *v;

    v = ConfGetOpt(conf, "filter_hide_empty");
    if (v) f->hideEmpty    = (atoi(v) != 0);

    v = ConfGetOpt(conf, "filter_hide_full");
    if (v) f->hideFull     = (atoi(v) != 0);

    v = ConfGetOpt(conf, "filter_hide_notempty");
    if (v) f->hideNotEmpty = (atoi(v) != 0);

    v = ConfGetOpt(conf, "filter_hide_highping");
    if (v) f->hideHighPing = (atoi(v) != 0);

    v = ConfGetOpt(conf, "filter_ping_limit");
    if (v) f->pingLimit    = atoi(v);

    v = ConfGetOpt(conf, "filter_map_enable");
    if (v) f->filterMap    = (atoi(v) != 0);

    v = ConfGetOpt(conf, "filter_map");
    if (v) strncpy(f->mapFilter, (const char*)v, MAX_MAPNAME - 1);

    v = ConfGetOpt(conf, "filter_tf_only");
    if (v) f->tfOnly = (atoi(v) != 0);
}

void Filter_Save(const FILTER_STATE *f, CONF *conf) {
    char tmp[32];

    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->hideEmpty);    tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_hide_empty",    tmp);
    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->hideFull);     tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_hide_full",     tmp);
    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->hideNotEmpty); tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_hide_notempty", tmp);
    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->hideHighPing); tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_hide_highping", tmp);
    _snprintf(tmp, sizeof(tmp)-1, "%d", f->pingLimit);         tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_ping_limit",    tmp);
    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->filterMap);    tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_map_enable",    tmp);
    ConfSetOpt(conf, "filter_map", f->mapFilter);
    _snprintf(tmp, sizeof(tmp)-1, "%d", (int)f->tfOnly);       tmp[sizeof(tmp)-1]='\0'; ConfSetOpt(conf, "filter_tf_only",       tmp);
}

void Filter_ApplyAll(const FILTER_STATE *f) {
    int i;
    EnterCriticalSection(&g_listLock);
    for (i = 0; i < g_serverCount; i++)
        g_servers[i].passesFilter = Filter_Pass(&g_servers[i], f);
    LeaveCriticalSection(&g_listLock);
}
