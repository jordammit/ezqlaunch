/*
    qlaunch - qwquery.c

    Thread-safe QuakeWorld UDP server status query.
    Each call is self-contained: creates a socket, sends the status request,
    waits up to timeout_ms for the reply, parses it, and closes the socket.
    No shared global state is used so any number of threads may call this
    concurrently.
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
#define snprintf  _snprintf
#define vsnprintf _vsnprintf

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>

#include "qwquery.h"

/* QW OOB status request: FF FF FF FF status 23 \n */
static const char QW_STATUS_REQ[] = {
    '\xFF', '\xFF', '\xFF', '\xFF',
    's','t','a','t','u','s',' ','2','3','\n'
};
#define QW_STATUS_REQ_LEN  14

/* Expected response header: FF FF FF FF n */
static const char QW_STATUS_HDR[] = { '\xFF', '\xFF', '\xFF', '\xFF', 'n' };
#define QW_STATUS_HDR_LEN  5

#define RECV_BUF  8192

/* -----------------------------------------------------------------------
   Key/value parser
   ----------------------------------------------------------------------- */

/* Find value for a key in a \key\value\key\value... string.
   Returns pointer into buf (NUL-terminated in-place slice), or NULL. */
static const char *KVGet(char *kvbuf, const char *key) {
    char *p = kvbuf;
    while (*p == '\\') {
        char *kstart = p + 1;
        char *vsep   = strchr(kstart, '\\');
        char *vend;
        if (!vsep) break;
        *vsep  = '\0';
        vend   = strchr(vsep + 1, '\\');
        if (!vend) vend = vsep + 1 + strlen(vsep + 1);
        else       *vend = '\0';

        if (_stricmp(kstart, key) == 0) {
            /* Restore separators (caller doesn't need them again) */
            return vsep + 1;
        }
        /* Restore and advance */
        *vsep = '\\';
        if (*vend == '\0' && vend != kvbuf + strlen(kvbuf)) break;
        p = vend;
        if (*p == '\0') break;
    }
    return NULL;
}

/*
 * Simpler approach: scan for \key\value pairs without modifying in place.
 * Returns a pointer to the start of the value, and sets *vlen to its length.
 */
static int KVFind(const char *buf, const char *key,
                  const char **val_out, int *vlen_out) {
    const char *p = buf;
    int         klen = (int)strlen(key);

    while (*p == '\\') {
        const char *kstart = p + 1;
        const char *ksep   = strchr(kstart, '\\');
        const char *vstart, *vend;

        if (!ksep) break;
        vstart = ksep + 1;
        vend   = strchr(vstart, '\\');
        if (!vend) vend = vstart + strlen(vstart);

        if ((int)(ksep - kstart) == klen &&
            _strnicmp(kstart, key, klen) == 0) {
            *val_out  = vstart;
            *vlen_out = (int)(vend - vstart);
            return 1;
        }
        p = vend;
    }
    return 0;
}

static void KVCopy(const char *buf, const char *key,
                   char *out, int outlen) {
    const char *v;
    int         vlen;
    if (KVFind(buf, key, &v, &vlen)) {
        if (vlen >= outlen) vlen = outlen - 1;
        memcpy(out, v, vlen);
        out[vlen] = '\0';
    }
}

static int KVInt(const char *buf, const char *key) {
    const char *v;
    int         vlen;
    char        tmp[32];
    if (!KVFind(buf, key, &v, &vlen)) return 0;
    if (vlen >= (int)sizeof(tmp)) vlen = (int)sizeof(tmp) - 1;
    memcpy(tmp, v, vlen);
    tmp[vlen] = '\0';
    return atoi(tmp);
}

/* -----------------------------------------------------------------------
   Player line parser
   ----------------------------------------------------------------------- */

/* Skip whitespace */
static const char *SkipWS(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read integer, advance pointer */
static const char *ReadInt(const char *p, int *out) {
    p = SkipWS(p);
    *out = atoi(p);
    if (*p == '-') p++;
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

/* Read quoted string, advance pointer, write to buf */
static const char *ReadQStr(const char *p, char *buf, int buflen) {
    p = SkipWS(p);
    buf[0] = '\0';
    if (*p == '"') {
        const char *end = strchr(p + 1, '"');
        if (end) {
            int len = (int)(end - p - 1);
            if (len >= buflen) len = buflen - 1;
            memcpy(buf, p + 1, len);
            buf[len] = '\0';
            return end + 1;
        }
    }
    return p;
}

static void ParsePlayers(const char *pinfo, QW_SERVERINFO *out) {
    out->numplayers = 0;
    while (pinfo && *pinfo) {
        const char *nl = strchr(pinfo, '\n');
        int id, frags, time_m, ping, top, bot;
        char name[64], skin[64], team[64];

        if (!nl) break;
        if (out->numplayers >= QW_MAX_PLAYERS) break;

        /* Format: <id> <frags> <time> <ping> "<name>" "<skin>" <top> <bot> ["<team>"] */
        pinfo = ReadInt(pinfo, &id);
        pinfo = ReadInt(pinfo, &frags);
        pinfo = ReadInt(pinfo, &time_m);
        pinfo = ReadInt(pinfo, &ping);
        pinfo = ReadQStr(pinfo, name, sizeof(name));
        pinfo = ReadQStr(pinfo, skin, sizeof(skin));
        pinfo = ReadInt(pinfo, &top);
        pinfo = ReadInt(pinfo, &bot);
        /* optional team field */
        pinfo = SkipWS(pinfo);
        if (*pinfo == '"')
            pinfo = ReadQStr(pinfo, team, sizeof(team));

        /* Skip spectators (negative ping) for player count */
        if (ping > 0) {
            QW_PLAYER *pl = &out->players[out->numplayers++];
            strncpy(pl->name, name, sizeof(pl->name) - 1);
            strncpy(pl->skin, skin, sizeof(pl->skin) - 1);
            strncpy(pl->team, team, sizeof(pl->team) - 1);
            pl->frags       = frags;
            pl->ping        = ping;
            pl->time        = time_m;
            pl->topcolor    = top;
            pl->bottomcolor = bot;
        }

        /* Advance past newline */
        pinfo = strchr(pinfo, '\n');
        if (pinfo) pinfo++;
    }
}

/* -----------------------------------------------------------------------
   Public API
   ----------------------------------------------------------------------- */

int QW_QueryServer(const char *host, unsigned short port,
                   int timeout_ms, QW_SERVERINFO *out) {
    SOCKET             sock = INVALID_SOCKET;
    struct sockaddr_in sa;
    struct hostent    *he;
    char               recvbuf[RECV_BUF];
    int                recvlen = 0;
    DWORD              t0, t1;
    fd_set             rfds;
    struct timeval     tv;
    char               kvbuf[RECV_BUF];  /* mutable copy of key/value section */

    memset(out, 0, sizeof(*out));

    /* Resolve host */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname(host);
        if (!he) return 0;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return 0;

    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        closesocket(sock);
        return 0;
    }

    t0 = GetTickCount();
    send(sock, QW_STATUS_REQ, QW_STATUS_REQ_LEN, 0);

    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(0, &rfds, NULL, NULL, &tv) <= 0) {
        closesocket(sock);
        return 0;
    }

    recvlen = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
    t1 = GetTickCount();
    closesocket(sock);

    if (recvlen <= QW_STATUS_HDR_LEN) return 0;
    recvbuf[recvlen] = '\0';

    /* Validate header: FF FF FF FF n */
    if (memcmp(recvbuf, QW_STATUS_HDR, QW_STATUS_HDR_LEN) != 0) return 0;

    out->ping = (int)(t1 - t0);
    if (out->ping > 998) out->ping = 998;

    /* The key/value block starts at byte 5 and ends at the first \n */
    {
        const char *kvstart = recvbuf + QW_STATUS_HDR_LEN;
        const char *kvend   = strchr(kvstart, '\n');
        int         kvlen;
        const char *pinfo;

        if (!kvend) kvend = recvbuf + recvlen;
        kvlen = (int)(kvend - kvstart);
        if (kvlen >= RECV_BUF) kvlen = RECV_BUF - 1;
        memcpy(kvbuf, kvstart, kvlen);
        kvbuf[kvlen] = '\0';

        /* Extract well-known fields */
        KVCopy(kvbuf, "hostname",   out->hostname,  sizeof(out->hostname));
        KVCopy(kvbuf, "map",        out->map,        sizeof(out->map));
        KVCopy(kvbuf, "*gamedir",   out->gamedir,    sizeof(out->gamedir));
        if (!out->gamedir[0])
            KVCopy(kvbuf, "gamedir", out->gamedir,   sizeof(out->gamedir));
        out->maxplayers = KVInt(kvbuf, "maxclients");
        out->fraglimit  = KVInt(kvbuf, "fraglimit");
        out->timelimit  = KVInt(kvbuf, "timelimit");

        /* Store all raw key/value pairs for the server-info panel */
        {
            const char *p = kvbuf;
            out->numkvpairs = 0;
            while (*p == '\\' && out->numkvpairs < QW_MAX_KEYS) {
                const char *ks = p + 1;
                const char *ke = strchr(ks, '\\');
                const char *vs, *ve;
                int klen, vlen;
                if (!ke) break;
                vs   = ke + 1;
                ve   = strchr(vs, '\\');
                if (!ve) ve = vs + strlen(vs);
                klen = (int)(ke - ks);
                vlen = (int)(ve - vs);
                if (klen > 0 && klen < QW_MAX_KEYLEN && vlen < QW_MAX_VALLEN) {
                    memcpy(out->kvpairs[out->numkvpairs].key, ks, klen);
                    out->kvpairs[out->numkvpairs].key[klen] = '\0';
                    memcpy(out->kvpairs[out->numkvpairs].val, vs, vlen);
                    out->kvpairs[out->numkvpairs].val[vlen] = '\0';
                    out->numkvpairs++;
                }
                p = (*ve == '\\') ? ve : ve + strlen(ve);
            }
        }

        /* Player lines follow the first \n */
        pinfo = (*kvend == '\n') ? kvend + 1 : NULL;
        if (pinfo && *pinfo)
            ParsePlayers(pinfo, out);
    }

    return 1;
}
