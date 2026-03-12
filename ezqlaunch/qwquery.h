/*
    qlaunch - qwquery.h

    Raw QuakeWorld UDP status query, implemented directly with Winsock.
    Each call creates and closes its own socket so it is fully thread-safe
    and can be called from any number of concurrent threads.

    Protocol:
      Send:    FF FF FF FF 's' 't' 'a' 't' 'u' 's' ' ' '2' '3' '\n'
      Receive: FF FF FF FF 'n' '\key\value\key\value...' '\n' <player lines>

    Key/value pairs of interest:
      hostname, map, maxclients, *gamedir, fraglimit, timelimit

    Player lines (one per player):
      <id> <frags> <time> <ping> "<name>" "<skin>" <topcolor> <bottomcolor>
      Spectators have negative ping.
*/

#ifndef QWQUERY_H
#define QWQUERY_H

#include <windows.h>

#define QW_MAX_PLAYERS  32
#define QW_MAX_KEYLEN   64
#define QW_MAX_VALLEN   128
#define QW_MAX_KEYS     32

typedef struct {
    char name[QW_MAX_KEYLEN];
    char skin[QW_MAX_KEYLEN];
    char team[QW_MAX_KEYLEN];   /* team name (optional, TF servers) */
    int  frags;
    int  ping;       /* negative = spectator */
    int  time;       /* minutes connected    */
    int  topcolor;
    int  bottomcolor;
} QW_PLAYER;

typedef struct {
    char key[QW_MAX_KEYLEN];
    char val[QW_MAX_VALLEN];
} QW_KV;

typedef struct {
    /* Fields extracted from key/value block */
    char hostname[128];
    char map[64];
    char gamedir[32];
    int  maxplayers;
    int  fraglimit;
    int  timelimit;

    /* All raw key/value pairs from the status response */
    QW_KV kvpairs[QW_MAX_KEYS];
    int   numkvpairs;

    /* Players */
    int       numplayers;
    QW_PLAYER players[QW_MAX_PLAYERS];

    /* Round-trip time in ms */
    int       ping;
} QW_SERVERINFO;

/*
 * Query a QuakeWorld server.
 * host    - hostname or dotted-decimal IP string
 * port    - UDP port number
 * timeout - receive timeout in milliseconds
 * out     - filled in on success
 *
 * Returns non-zero on success, 0 on failure/timeout.
 * Thread-safe: creates its own socket per call.
 */
int QW_QueryServer(const char *host, unsigned short port,
                   int timeout_ms, QW_SERVERINFO *out);

#endif /* QWQUERY_H */
