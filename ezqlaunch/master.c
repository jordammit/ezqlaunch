/*
    ezqlaunch - master.c
    QuakeWorld master server UDP query implementation.

    Uses only Winsock 1.1 API (gethostbyname, socket, etc.) so that no
    additional .lib beyond what gamestat.lib already pulls in is required.
    ws2_32.lib is still needed and must be listed in the project linker
    dependencies (see updated qlaunch.vcproj).
*/
/* MSVC 2003 CRT uses underscore-prefixed names for these C99 functions.
   Must be defined before any #include that pulls in stdio.h prototypes. */
#define vsnprintf  _vsnprintf
#define snprintf   _snprintf

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "master.h"
#include "serverlist.h"
#include "resource.h"

/*
 * QW master protocol (from ezQuake EX_browser_sources.c):
 *   Query:  "c\n\0"  -- just 3 bytes, NO 0xFF OOB header on the outgoing query
 *   Reply:  FF FF FF FF 64 0A  followed by N*6 byte server entries
 * The OOB header only appears on the reply, not the query.
 */
static const char          MASTER_QUERY[]  = { 'c', '\n', '\0' };
static const unsigned char MASTER_REPLY[]  = { 0xFF, 0xFF, 0xFF, 0xFF, 'd', '\n' };

#define MASTER_QUERY_LEN   3   /* c \n \0 */
#define MASTER_REPLY_HDR   6
#define MASTER_ENTRY_SIZE  6    /* 4 bytes IP + 2 bytes port (big-endian) */
#define RECV_BUF_SIZE      (MASTER_ENTRY_SIZE * MAX_SERVERS + MASTER_REPLY_HDR + 16)

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

int Master_Query(const char *host, const char *port, HWND hwndDlg) {
    SOCKET          sock = INVALID_SOCKET;
    unsigned char   recvbuf[RECV_BUF_SIZE];
    int             received = 0;
    int             attempt;
    int             count = 0;

    PostStatus(hwndDlg, "Resolving master server: %s:%s ...", host, port);

    {
        struct hostent     *he;
        struct sockaddr_in  sa;
        unsigned short      portnum = (unsigned short)atoi(port);

        /* Try dotted-decimal first, then DNS */
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(portnum);
        sa.sin_addr.s_addr = inet_addr(host);

        if (sa.sin_addr.s_addr == INADDR_NONE) {
            he = gethostbyname(host);
            if (!he) {
                PostStatus(hwndDlg, "ERROR: Could not resolve master server '%s'", host);
                return -1;
            }
            memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            PostStatus(hwndDlg, "ERROR: Could not create UDP socket.");
            return -1;
        }

        if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
            closesocket(sock);
            PostStatus(hwndDlg, "ERROR: Could not connect to master server socket.");
            return -1;
        }
    }

    for (attempt = 1; attempt <= MASTER_MAX_RETRIES && received <= 0; attempt++) {
        fd_set  rfds;
        struct timeval tv;

        PostStatus(hwndDlg, "Querying master server (attempt %d/%d)...", attempt, MASTER_MAX_RETRIES);
        send(sock, MASTER_QUERY, MASTER_QUERY_LEN, 0);

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        tv.tv_sec  = MASTER_RECV_TIMEOUT_MS / 1000;
        tv.tv_usec = (MASTER_RECV_TIMEOUT_MS % 1000) * 1000;

        if (select(0, &rfds, NULL, NULL, &tv) > 0) {
            received = recv(sock, (char*)recvbuf, sizeof(recvbuf) - 1, 0);
        }
    }

    closesocket(sock);

    if (received <= 0) {
        PostStatus(hwndDlg, "ERROR: No response from master server after %d attempts.", MASTER_MAX_RETRIES);
        return -1;
    }

    /* Validate reply header */
    if (received < MASTER_REPLY_HDR ||
        memcmp(recvbuf, MASTER_REPLY, MASTER_REPLY_HDR) != 0)
    {
        PostStatus(hwndDlg, "ERROR: Unexpected response from master server.");
        return -1;
    }

    /* Parse 6-byte server entries, clear old list first */
    SL_Clear();

    {
        const unsigned char *p   = recvbuf + MASTER_REPLY_HDR;
        const unsigned char *end = recvbuf + received;

        while (p + MASTER_ENTRY_SIZE <= end) {
            /* Terminator: 6 zero bytes */
            if (p[0]==0 && p[1]==0 && p[2]==0 && p[3]==0 && p[4]==0 && p[5]==0) break;

            {
                char addr[22];
                unsigned short port_n = ((unsigned short)p[4] << 8) | p[5];
                snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u",
                         p[0], p[1], p[2], p[3], port_n);
                if (SL_AddAddr(addr) >= 0) count++;
            }
            p += MASTER_ENTRY_SIZE;
        }
    }

    PostStatus(hwndDlg, "Master returned %d servers.", count);
    return count;
}
