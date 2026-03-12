/*
    qlaunch - master.h
    QuakeWorld master server protocol.

    The QW master protocol is simple UDP:
      Client sends:  FF FF FF FF 63 0A          ("c\n" with 4-byte OOB header)
      Server replies: FF FF FF FF 64 0A          ("d\n") followed by N * 6 bytes
                      each 6-byte chunk = 4 bytes IP (network order) + 2 bytes port (big-endian)
                      Terminated by 0x00 00 00 00 00 00 (6 zero bytes).
*/

#ifndef MASTER_H
#define MASTER_H

#include <windows.h>

#define DEFAULT_MASTER_HOST     "master.quakeworld.nu"
#define DEFAULT_MASTER_PORT     "27000"
#define MASTER_RECV_TIMEOUT_MS  1500
#define MASTER_MAX_RETRIES      3

/*
 * Query a QW master server and populate g_servers with the returned addresses.
 * Existing list content is REPLACED (SL_Clear is called internally).
 *
 * host    - hostname or dotted-decimal IP of the master
 * port    - UDP port as a string (e.g. "27000")
 * hwndDlg - dialog HWND to receive WM_APP_STATUS progress messages
 *
 * Returns number of servers received, or -1 on error.
 * This function is designed to be called from a worker thread.
 */
int Master_Query(const char *host, const char *port, HWND hwndDlg);

#endif /* MASTER_H */
