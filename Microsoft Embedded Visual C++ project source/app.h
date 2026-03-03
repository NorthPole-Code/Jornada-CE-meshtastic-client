#pragma once

#include <windows.h>
#include <commctrl.h>

//--------------------------------------------------
// Tab indices
//--------------------------------------------------
#define TAB_CHAT      0
#define TAB_DIRECT    1
#define TAB_NODES     2
#define TAB_MAP       3
#define TAB_SETTINGS  4
#define TAB_SERIAL    5

//--------------------------------------------------
// Control IDs
//--------------------------------------------------
#define IDC_MAIN_TAB          100

#define IDC_CHAT_HISTORY      110
#define IDC_CHAT_INPUT        111
#define IDC_CHAT_SEND         112
// Compact indicator for current public-channel preset (read from device)
#define IDC_CHAT_PRESET       113

#define IDC_DM_NODELIST       120
#define IDC_DM_HISTORY        121
#define IDC_DM_INPUT          122
#define IDC_DM_SEND           123

#define IDC_NODES_SEARCH      140
#define IDC_NODES_LIST        141
#define IDC_NODES_CLEAR       142
#define IDC_NODES_COUNT       143

// Top bar (always visible)
#define IDC_TOP_CONNECT       145

// Map view control ID
#define IDC_MAP_VIEW          150

// Settings tab controls
#define IDC_SET_STORAGE_PATH  160
#define IDC_SET_BROWSE        161
#define IDC_SET_COM_PORT      162
#define IDC_SET_ROLE          163
#define IDC_SET_REGION        164
#define IDC_SET_MODEM_PRESET  165
#define IDC_SET_TX_ENABLED    166
#define IDC_SET_TX_POWER      167
#define IDC_SET_HOP_LIMIT     168
#define IDC_SET_FREQ_SLOT     169
#define IDC_SET_GPS_MODE      170
#define IDC_SET_APPLY         172
#define IDC_SET_SAVE          173
#define IDC_SET_CONNECT       174
#define IDC_SET_GET_SETTINGS  175

// App-only settings
#define IDC_SET_CHAT_TS_MODE  176
// Follow new messages (auto-scroll)
#define IDC_SET_CHAT_FOLLOW   177

// Button LED (Jornada notification LED)
#define IDC_SET_LED_NEWMSG    178
#define IDC_SET_LED_UNREAD     179

// Power management
#define IDC_SET_PREVENT_SLEEP 186

// Serial monitor app settings
#define IDC_SET_SERIAL_MONITOR_ENABLE 187
#define IDC_SET_SERIAL_MAX_LINES      188

// About (Settings page)
#define IDC_SET_ABOUT                 189

// Serial monitor tab controls
#define IDC_SERIAL_LOG        180
#define IDC_SERIAL_INPUT      181
#define IDC_SERIAL_SEND       182
#define IDC_SERIAL_CLEAR      183

//--------------------------------------------------
// Node data
//--------------------------------------------------
struct NodeInfo
{
    DWORD nodeNum;
    TCHAR shortName[16];
    TCHAR longName[64];
    TCHAR nodeId[12];
    // Power/telemetry (best-effort; may be unknown if not reported)
    // batteryPct: -1 = unknown, otherwise 0-100
    int   batteryPct;
    // batteryMv: 0 = unknown, otherwise millivolts
    int   batteryMv;
    bool  online;

    SYSTEMTIME lastHeard;

    bool  hasPosition;
    double latitude;
    double longitude;

    // Extra node/radio info (best-effort; may be unknown if not reported)
    int   hopsAway;        // -1 = unknown
    bool  encrypted;       // true if node/channel indicates encrypted traffic
    int   snr_x10;         // SNR * 10 (e.g. 25 = 2.5). INT_MIN = unknown
    TCHAR model[24];       // e.g. "UNSET", "HELTEC", "RAK", ...
};

//--------------------------------------------------
// Global app state
//--------------------------------------------------
struct APPSTATE
{
    HINSTANCE hInst;
    HWND hMain;
    HWND hTab;

    HWND hPageChat;
    HWND hPageDirect;
    HWND hPageNodes;
    HWND hPageMap;
    HWND hPageSettings;
    HWND hPageSerial;

    // Chat tab
    HWND hChatHistory;
    HWND hChatInput;
    HWND hChatSend;
    HWND hChatPreset;

    // Direct tab
    HWND hDMNodeList;
    HWND hDMHistory;
    HWND hDMInput;
    HWND hDMSend;

    // Nodes tab
    HWND hNodesSearch;
    HWND hNodesClear;
    HWND hNodesCount;
    HWND hNodesList;

    // Top bar
    HWND hTopConnect;
    HWND hTopRx;
    HWND hTopTx;
    // (fix) removed accidental duplicate hTopRx/hTopTx members

    // Map tab
    HWND hMapView;

    // Settings tab
    HWND hSetStoragePath;
    HWND hSetBrowse;
    HWND hSetComPort;
    HWND hSetChatTsMode;
    HWND hSetChatFollow;
    HWND hSetLedNewMsg;
    HWND hSetLedUnread;
    HWND hSetPreventSleep;
    HWND hSetSerialMonitorEnable;
    HWND hSetSerialMaxLines;
    HWND hSetRole;
    HWND hSetRegion;
    HWND hSetModemPreset;
    HWND hSetTxEnabled;
    HWND hSetTxPower;
    HWND hSetHopLimit;
    HWND hSetFreqSlot;
    HWND hSetGpsMode;

    // Settings page header button
    HWND hSetAbout;

    // Settings fetch (hourglass)
    bool  isFetchingSettings;
    HCURSOR hOldCursor;
    HWND hSetApply;
    HWND hSetSave;
    HWND hSetConnect;
    HWND hSetGetSettings;

    // Serial monitor tab
    HWND hSerialLog;
    HWND hSerialInput;
    HWND hSerialSend;
    HWND hSerialClear;
};

extern APPSTATE g_App;

//--------------------------------------------------
// UI helpers (ui_main.cpp)
//--------------------------------------------------
void CreateMainControls(HWND hwnd);
void LayoutControls(HWND hwnd);
void ShowTabPage(int index);

// Top bar connect button (ui_main.cpp)
void UI_UpdateTopConnectButton();
void AppendTextWithTimestamp(HWND hEdit, LPCTSTR pszPrefix, LPCTSTR pszText);
void AppendSerialLog(const char* pszLine, bool isRx);
void UI_RefreshSerialMonitorState();
void UI_RefreshSerialMonitorState();

// Chat tab preset indicator (ui_main.cpp)
void UI_UpdateChatPresetIndicator();

// UI helpers
void UI_GotoDirectConversation(DWORD nodeNum);
void UI_HistoryPushChat(LPCTSTR text);
void UI_HistoryPushDM(LPCTSTR text);
void UI_HistoryPushSerial(LPCTSTR text);

//--------------------------------------------------
// Jornada notification LED helpers (MESHTASTIC.cpp)
//--------------------------------------------------
void LED_Init();
void LED_Shutdown();
void LED_UpdateFromConfig();
void LED_SetUnreadActive(bool hasUnread);
void LED_OnNewMessage();

//--------------------------------------------------
// Unread / new-message UI helpers (MESHTASTIC.cpp)
//--------------------------------------------------
// Call these when a message is received.
void UI_Unread_OnIncomingChat();
void UI_Unread_OnIncomingDM(DWORD fromNode);

// Call this after tab changes / DM selection changes.
void UI_Unread_OnTabChanged(int newTabIndex);
void UI_Unread_OnDirectSelectionChanged(DWORD selectedNode);
