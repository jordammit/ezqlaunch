//{{NO_DEPENDENCIES}}
// qlaunch resource IDs
//

// Dialogs
#define IDD_MAIN                    101
#define IDD_ADDMASTER               110
#define IDD_FILTERS                 111
#define IDD_ENGINE                  112
#define IDD_ADDSERVER               113

// String table
#define IDS_PLAYER                  102
#define IDS_FRAGS                   103
#define IDS_TIME                    104
#define IDS_PING                    105
#define IDS_SERVER                  106
#define IDS_HOST                    107
#define IDS_MAP                     108
#define IDS_PLAYERS                 109
#define IDS_STATUS_IDLE             120
#define IDS_STATUS_QUERYING         121
#define IDS_STATUS_PINGING          122
#define IDS_STATUS_DONE             123
#define IDS_STATUS_LOADING_CACHE    124

// Main dialog controls
#define IDC_SERVERS                 1006
#define IDC_PLAYERS                 1003
#define IDC_CONNECT                 1004
#define IDC_QUIT                    1005

// Toolbar buttons
#define IDC_BTN_UPDATE_MASTER       1010
#define IDC_BTN_REFRESH_CACHE       1011
#define IDC_BTN_FILTERS             1012
#define IDC_BTN_ADD_MASTER          1013
#define IDC_BTN_ENGINE              1017
#define IDC_STATUS_BAR              1014
#define IDC_FILTER_SUMMARY          1015
#define IDC_PROGRESS                1016

// Tab control and per-tab ListViews
#define IDC_TAB                     1050
#define IDC_SERVERS_FAV             1051
#define IDC_SERVERS_HIST            1052
#define IDC_SRVINFO                 1053
#define IDC_SPLITTER                1054
#define IDC_SERVERS_NVR             1055

// Standard context menu
#define IDM_CTX_CONNECT             1060
#define IDM_CTX_FAVORITE            1061
#define IDM_CTX_NEVERSCAN           1062
#define IDM_CTX_REFRESH_SEL         1063
#define IDM_CTX_NVR_REMOVE          1064
#define IDM_CTX_NVR_REMOVEALL       1065

// Menu bar
#define IDR_MAINMENU                200

// Application menu
#define IDM_APP_SETCLIENT           2001
#define IDM_APP_SETMASTER           2002
#define IDM_APP_EXIT                2003

// Server menu
#define IDM_SRV_CONNECT             2010
#define IDM_SRV_FAVORITE            2011
#define IDM_SRV_REFRESH_SEL         2012
#define IDM_SRV_NEVERSCAN           2013
#define IDM_SRV_FULLUPDATE          2014
#define IDM_SRV_REFRESHALL          2015
#define IDM_SRV_ADDMANUAL           2016

// View - sort servers
#define IDM_VIEW_SORT_SRV_NAME      2020
#define IDM_VIEW_SORT_SRV_ADDR      2021
#define IDM_VIEW_SORT_SRV_MAP       2022
#define IDM_VIEW_SORT_SRV_PLAYERS   2023
#define IDM_VIEW_SORT_SRV_PING      2024
#define IDM_VIEW_SORT_SRV_LAST      2025

// View - sort players
#define IDM_VIEW_SORT_PLR_TEAM      2030
#define IDM_VIEW_SORT_PLR_NAME      2031
#define IDM_VIEW_SORT_PLR_SCORE     2032
#define IDM_VIEW_SORT_PLR_TIME      2033
#define IDM_VIEW_SORT_PLR_PING      2034
#define IDM_VIEW_SORT_PLR_SKIN      2035

// View - misc
#define IDM_VIEW_HIDE_BOTTOM        2040

// Filters menu
#define IDM_FILTER_TF_ONLY          2050
#define IDM_FILTER_HIDE_EMPTY       2051
#define IDM_FILTER_HIDE_FULL        2052
#define IDM_FILTER_HIDE_NOTEMPTY    2053
#define IDM_FILTER_HIDE_HIGHPING    2054

// About
#define IDM_ABOUT                   2060

// Add Master dialog
#define IDC_MASTER_HOST             1020
#define IDC_MASTER_PORT             1021
#define IDC_MASTER_OK               1022
#define IDC_MASTER_CANCEL           1023

// Engine dialog
#define IDC_ENGINE_PATH             1040
#define IDC_ENGINE_BROWSE           1041
#define IDC_ENGINE_ARGS             1042
#define IDC_ENGINE_OK               1043
#define IDC_ENGINE_CANCEL           1044

// Filter dialog
#define IDC_FILTER_HIDE_EMPTY       1030
#define IDC_FILTER_HIDE_FULL        1031
#define IDC_FILTER_HIDE_HIGHPING    1032
#define IDC_FILTER_PING_LIMIT       1033
#define IDC_FILTER_MAP              1034
#define IDC_FILTER_OK               1035
#define IDC_FILTER_CANCEL           1036
#define IDC_FILTER_HIDE_NOTEMPTY    1037
#define IDC_FILTER_TF_ONLY          1038

// Add Server dialog
#define IDC_ADDSERVER_ADDR          1070
#define IDC_ADDSERVER_OK            1071
#define IDC_ADDSERVER_CANCEL        1072

// WM_APP messages
#define WM_APP_SERVERINFO           (WM_APP + 1)
#define WM_APP_SCAN_DONE            (WM_APP + 2)
#define WM_APP_STATUS               (WM_APP + 3)
#define WM_APP_REFRESH_DONE         (WM_APP + 4)

#define IDT_SCAN_POLL               1001
#define IDT_AUTO                    1002   /* auto-scan interval timer */

/* Auto-scan dialog */
#define IDD_AUTO                    310
#define IDC_BTN_AUTO                1080   /* clock button on main toolbar */
#define IDC_BTN_FAV_SEL             1087   /* Add selection to Favorites   */
#define IDC_BTN_REFRESH_SEL_TB      1088   /* Refresh selected server      */
#define IDC_AUTO_UPDATE             1081   /* "Auto Update" checkbox       */
#define IDC_AUTO_REFRESH            1082   /* "Auto Refresh" checkbox      */
#define IDC_AUTO_UPDATE_MINS        1083   /* minutes edit for update      */
#define IDC_AUTO_REFRESH_MINS       1084   /* minutes edit for refresh     */
#define IDC_AUTO_OK                 1085
#define IDC_AUTO_CANCEL             1086

#define PIPE_ARG                    "--pipe"

#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE    114
#define _APS_NEXT_COMMAND_VALUE     40001
#define _APS_NEXT_CONTROL_VALUE     1073
#define _APS_NEXT_SYMED_VALUE       101
#endif
#endif
