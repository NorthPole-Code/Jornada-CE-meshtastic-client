#include "stdafx.h"
#include <commctrl.h>
#include <commdlg.h>
#include <ctype.h>
#include "app.h"
#include "nodes.h"
#include "NodeDetails.h"

#include "msgstore.h"
#include "config.h"

// ------------------------------------------------------------
// Edit-control scroll helpers (WinCE friendly)
// ------------------------------------------------------------
static int Edit_GetVisibleLineCount(HWND hEdit)
{
    if (!hEdit) return 1;
    RECT rc; GetClientRect(hEdit, &rc);
    int h = (rc.bottom - rc.top);
    if (h <= 0) return 1;

    HDC hdc = GetDC(hEdit);
    int lineH = 12;
    if (hdc)
    {
        HFONT hFont = (HFONT)SendMessage(hEdit, WM_GETFONT, 0, 0);
        HFONT hOld = NULL;
        if (hFont) hOld = (HFONT)SelectObject(hdc, hFont);
        TEXTMETRIC tm;
        if (GetTextMetrics(hdc, &tm))
        {
            lineH = (int)tm.tmHeight + (int)tm.tmExternalLeading;
            if (lineH <= 0) lineH = 12;
        }
        if (hOld) SelectObject(hdc, hOld);
        ReleaseDC(hEdit, hdc);
    }

    int vis = h / lineH;
    if (vis < 1) vis = 1;
    return vis;
}

static bool Edit_IsAtBottom(HWND hEdit)
{
    if (!hEdit) return true;
    int first = (int)SendMessage(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    int total = (int)SendMessage(hEdit, EM_GETLINECOUNT, 0, 0);
    int vis = Edit_GetVisibleLineCount(hEdit);
    return (first + vis >= total - 1);
}
static NodeInfo g_Nodes[MAX_NODES];
static int      g_NodeCount = 0;

#define MAX_DM_CONVOS 32
#define MAX_DM_HISTORY_CHARS 8192

struct DMCONVO
{
    DWORD nodeNum;

    // Persisted/canonical history (always stores full "DD/MM/YY HH:MM" timestamps).
    // Rendering for the UI (time-only vs date+time) is applied on demand.
    TCHAR historyRaw[MAX_DM_HISTORY_CHARS];

    // Day-rollover separator support
    bool hasLastDay;
    WORD lastY;
    WORD lastM;
    WORD lastD;

    // Unread tracking (UI only)
    int  unreadCount;
};

static DMCONVO g_DmConvos[MAX_DM_CONVOS];
static int     g_DmConvoCount = 0;

// --------------------------------------------------
// Outgoing DM ACK tracking
// We append a fixed-width 4-char marker to the end of the outgoing line:
//   " - " (default), " + " (mesh ack), "++ " (direct ack by recipient)
// markerPos is a TCHAR index into historyRaw.
// --------------------------------------------------
struct DmAckTrack
{
    DWORD packetId;
    DWORD toNode;
    int   convoIdx;
    int   markerPos;
    bool  used;
};

static DmAckTrack g_DmAcks[64];

// ACK marker rendering (fixed width)
#define ACK_MARKER_LEN 4
static const TCHAR* const ACK_PENDING_DM = TEXT("  - ");
static const TCHAR* const ACK_MESH_DM    = TEXT("  + ");
static const TCHAR* const ACK_DIRECT_DM  = TEXT(" ++ ");
static const TCHAR* const ACK_BLANK_DM   = TEXT("    ");


// User-selectable storage path (nodes DB & later logs)
static TCHAR g_szStoragePath[MAX_PATH] = TEXT("\\Storage Card\\Meshtastic\\nodes.dat");

static void FormatNodeLine(const NodeInfo* pNode, LPTSTR out, int cchOut)
{
    if (!pNode) { out[0] = 0; return; }

    TCHAR disp[16];
    Nodes_GetDisplayShortName(pNode, disp, 16);

    _sntprintf(out, cchOut,
               TEXT("0x%04X  %-8s  %3d%%  %s"),
               pNode->nodeNum,
               disp[0] ? disp : TEXT("(node)"),
               pNode->batteryPct,
               pNode->online ? TEXT("Online") : TEXT("Offline"));
}

// --------------------------------------------------
// Name fallback helpers
// If a node doesn't report short/long name, use last 4 characters of the USER ID.
// USER ID is formatted like "!%08lX" (meshtastic android shows last 4).
// --------------------------------------------------
static void MakeFallbackName(const NodeInfo* n, LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!n) return;
    // Format as 8 hex digits, then take the last 4
    TCHAR tmp[16];
    _sntprintf(tmp, 16, TEXT("%08lX"), n->nodeNum);
    // tmp is always 8 chars. Copy last 4.
    _tcsncpy(out, tmp + 4, cchOut - 1);
    out[cchOut - 1] = 0;
}

void Nodes_GetDisplayShortName(const NodeInfo* n, LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!n) return;
    if (n->shortName[0]) { _tcsncpy(out, n->shortName, cchOut - 1); out[cchOut - 1] = 0; return; }
    MakeFallbackName(n, out, cchOut);
}

void Nodes_GetDisplayLongName(const NodeInfo* n, LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!n) return;
    if (n->longName[0]) { _tcsncpy(out, n->longName, cchOut - 1); out[cchOut - 1] = 0; return; }
    MakeFallbackName(n, out, cchOut);
}


static bool StrContainsI(const TCHAR* haystack, const TCHAR* needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;

    TCHAR h[256], n[64];
    _tcsncpy(h, haystack, 255); h[255] = 0;
    _tcsncpy(n, needle, 63);   n[63] = 0;

    TCHAR* p;
    for (p = h; *p; ++p) *p = (TCHAR)_totlower(*p);
    for (p = n; *p; ++p) *p = (TCHAR)_totlower(*p);

    return (_tcsstr(h, n) != NULL);
}

// --------------------------------------------------
// ListView sorting (Nodes tab)
// --------------------------------------------------
static int g_SortCol = 0;
static bool g_SortAsc = true;

static int CmpTextI(const TCHAR* a, const TCHAR* b)
{
    if (!a) a = TEXT("");
    if (!b) b = TEXT("");
    // Case-insensitive compare (WinCE _tcsicmp exists)
    return _tcsicmp(a, b);
}

static unsigned __int64 SysTimeToKey(const SYSTEMTIME& st)
{
    // yyyymmddhhmmss as integer key
    // eVC++ (WinCE) doesn't reliably support ULL literal suffixes; use (u)i64 instead.
    return (unsigned __int64)st.wYear * 10000000000ui64 +
           (unsigned __int64)st.wMonth * 100000000ui64 +
           (unsigned __int64)st.wDay * 1000000ui64 +
           (unsigned __int64)st.wHour * 10000ui64 +
           (unsigned __int64)st.wMinute * 100ui64 +
           (unsigned __int64)st.wSecond;
}

// --------------------------------------------------
// Chat timestamp helpers (used by Direct Messages)
// --------------------------------------------------
static void FormatChatTimestampStorage(const SYSTEMTIME& st, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;

    // Always store full date+time.
    _sntprintf(out, cchOut, TEXT("%02d/%02d/%02d %02d:%02d"),
        (int)st.wDay, (int)st.wMonth, (int)(st.wYear % 100),
        (int)st.wHour, (int)st.wMinute);
    out[cchOut - 1] = 0;
}

// If time-only mode is enabled, convert persisted timestamps:
//   "[DD/MM/YY HH:MM]" -> "[HH:MM]"
// Best-effort; leaves unknown/legacy formats intact.
static void ApplyTimestampDisplayModeInPlace(TCHAR* text)
{
    if (!text || !text[0]) return;

    AppConfig* cfg = Config_GetApp();
    int mode = (cfg ? cfg->chatTimestampMode : 0);
    if (mode != 0) return; // 1 = date+time

    TCHAR* p = text;
    while (*p)
    {
        if (*p == TEXT('['))
        {
            // Expect: [dd/mm/yy hh:mm]
            if (_istdigit(p[1]) && _istdigit(p[2]) &&
                p[3] == TEXT('/') &&
                _istdigit(p[4]) && _istdigit(p[5]) &&
                p[6] == TEXT('/') &&
                _istdigit(p[7]) && _istdigit(p[8]) &&
                p[9] == TEXT(' ') &&
                _istdigit(p[10]) && _istdigit(p[11]) &&
                p[12] == TEXT(':') &&
                _istdigit(p[13]) && _istdigit(p[14]) &&
                p[15] == TEXT(']'))
            {
                // Shift "hh:mm]" left over "dd/mm/yy " (9 chars)
                p[1] = p[10];
                p[2] = p[11];
                p[3] = p[12];
                p[4] = p[13];
                p[5] = p[14];
                p[6] = p[15];

                memmove(p + 7, p + 16, (_tcslen(p + 16) + 1) * sizeof(TCHAR));
                p += 7;
                continue;
            }
        }
        ++p;
    }
}

static void Direct_RenderHistoryForDisplay(const TCHAR* inRaw, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!inRaw) return;

    _tcsncpy(out, inRaw, cchOut - 1);
    out[cchOut - 1] = 0;
    ApplyTimestampDisplayModeInPlace(out);
}

// Forward declarations required by eVC++ 3.0 (no implicit function declarations)
static int Direct_FindIndex(DWORD nodeNum);

static void Direct_RefreshHistoryUI(DWORD nodeNum)
{
    if (!g_App.hDMHistory) return;

    // IRC-style refresh behavior:
    // - if user was at bottom, stay at bottom
    // - otherwise preserve scroll position
    AppConfig* cfg = Config_GetApp();
    int scrollMode = (cfg ? cfg->chatAutoScroll : 1);
    if (scrollMode < 0 || scrollMode > 2) scrollMode = 1;
    bool wasAtBottom = (scrollMode == 1) ? Edit_IsAtBottom(g_App.hDMHistory) : false;
    bool forceBottom = (scrollMode == 2);
    int savedFirst = (int)SendMessage(g_App.hDMHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
    DWORD selStart = 0, selEnd = 0;
    SendMessage(g_App.hDMHistory, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    if (nodeNum == 0)
    {
        SendMessage(g_App.hDMHistory, WM_SETREDRAW, FALSE, 0);
        SetWindowText(g_App.hDMHistory, TEXT("(no receiving node selected)"));
        if (forceBottom || (scrollMode == 1 && wasAtBottom))
        {
            int len = GetWindowTextLength(g_App.hDMHistory);
            SendMessage(g_App.hDMHistory, EM_SETSEL, len, len);
            SendMessage(g_App.hDMHistory, EM_SCROLLCARET, 0, 0);
        }
        else
        {
            SendMessage(g_App.hDMHistory, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
            int nowFirst = (int)SendMessage(g_App.hDMHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessage(g_App.hDMHistory, EM_LINESCROLL, 0, (LPARAM)(savedFirst - nowFirst));
        }
        SendMessage(g_App.hDMHistory, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_App.hDMHistory, NULL, TRUE);
        return;
    }
    int idx = Direct_FindIndex(nodeNum);
    if (idx < 0)
    {
        SendMessage(g_App.hDMHistory, WM_SETREDRAW, FALSE, 0);
        SetWindowText(g_App.hDMHistory, TEXT(""));
        if (forceBottom || (scrollMode == 1 && wasAtBottom))
        {
            int len = GetWindowTextLength(g_App.hDMHistory);
            SendMessage(g_App.hDMHistory, EM_SETSEL, len, len);
            SendMessage(g_App.hDMHistory, EM_SCROLLCARET, 0, 0);
        }
        else
        {
            SendMessage(g_App.hDMHistory, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
            int nowFirst = (int)SendMessage(g_App.hDMHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessage(g_App.hDMHistory, EM_LINESCROLL, 0, (LPARAM)(savedFirst - nowFirst));
        }
        SendMessage(g_App.hDMHistory, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_App.hDMHistory, NULL, TRUE);
        return;
    }
    TCHAR disp[MAX_DM_HISTORY_CHARS];
    Direct_RenderHistoryForDisplay(g_DmConvos[idx].historyRaw, disp, MAX_DM_HISTORY_CHARS);
    SendMessage(g_App.hDMHistory, WM_SETREDRAW, FALSE, 0);
    SetWindowText(g_App.hDMHistory, disp);
    if (forceBottom || (scrollMode == 1 && wasAtBottom))
    {
        int len = GetWindowTextLength(g_App.hDMHistory);
        SendMessage(g_App.hDMHistory, EM_SETSEL, len, len);
        SendMessage(g_App.hDMHistory, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        SendMessage(g_App.hDMHistory, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
        int nowFirst = (int)SendMessage(g_App.hDMHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessage(g_App.hDMHistory, EM_LINESCROLL, 0, (LPARAM)(savedFirst - nowFirst));
    }
    SendMessage(g_App.hDMHistory, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_App.hDMHistory, NULL, TRUE);
}

static void FormatDaySeparator(const SYSTEMTIME& st, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    _sntprintf(out, cchOut, TEXT("----- %02d/%02d/%02d -----\r\n"),
        (int)st.wDay, (int)st.wMonth, (int)(st.wYear % 100));
    out[cchOut - 1] = 0;
}

static bool ParseLastDayFromText(const TCHAR* text, WORD* y, WORD* m, WORD* d)
{
    if (!text || !text[0]) return false;
    int len = _tcslen(text);
    if (len <= 0) return false;

    for (int i = len - 1; i >= 0; --i)
    {
        if (text[i] < TEXT('0') || text[i] > TEXT('9')) continue;

        int start = i - 7;
        if (start < 0) start = 0;

        for (int s = i; s >= start; --s)
        {
            if (s + 7 < len &&
                _istdigit(text[s]) && _istdigit(text[s + 1]) &&
                text[s + 2] == TEXT('/') &&
                _istdigit(text[s + 3]) && _istdigit(text[s + 4]) &&
                text[s + 5] == TEXT('/') &&
                _istdigit(text[s + 6]) && _istdigit(text[s + 7]))
            {
                int dd = (text[s] - TEXT('0')) * 10 + (text[s + 1] - TEXT('0'));
                int mm = (text[s + 3] - TEXT('0')) * 10 + (text[s + 4] - TEXT('0'));
                int yy = (text[s + 6] - TEXT('0')) * 10 + (text[s + 7] - TEXT('0'));

                if (dd >= 1 && dd <= 31 && mm >= 1 && mm <= 12)
                {
                    if (d) *d = (WORD)dd;
                    if (m) *m = (WORD)mm;
                    if (y) *y = (WORD)(2000 + yy);
                    return true;
                }
            }
        }
    }
    return false;
}

static int CALLBACK Nodes_ListViewCompare(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
    (void)lParamSort;
    NodeInfo* A = Nodes_FindOrAdd((DWORD)lParam1);
    NodeInfo* B = Nodes_FindOrAdd((DWORD)lParam2);

    int c = 0;

    switch (g_SortCol)
    {
    case 0: // Short
    {
        TCHAR as[16], bs[16];
        Nodes_GetDisplayShortName(A, as, 16);
        Nodes_GetDisplayShortName(B, bs, 16);
        c = CmpTextI(as, bs);
    }
    break;
    case 1: // Long
    {
        TCHAR al[64], bl[64];
        Nodes_GetDisplayLongName(A, al, 64);
        Nodes_GetDisplayLongName(B, bl, 64);
        c = CmpTextI(al, bl);
    }
    break;
    case 2: // Node ID
        c = CmpTextI((A && A->nodeId[0]) ? A->nodeId : TEXT(""),
                     (B && B->nodeId[0]) ? B->nodeId : TEXT(""));
        break;
    case 3: // Status
        c = (int)((A && A->online) ? 1 : 0) - (int)((B && B->online) ? 1 : 0);
        break;
    case 4: // Batt
        c = (A ? A->batteryPct : 0) - (B ? B->batteryPct : 0);
        break;
    case 5: // Last online
    {
        unsigned __int64 ak = A ? SysTimeToKey(A->lastHeard) : 0;
        unsigned __int64 bk = B ? SysTimeToKey(B->lastHeard) : 0;
        if (ak < bk) c = -1;
        else if (ak > bk) c = 1;
        else c = 0;
        break;
    }
    case 6: // Conn (hops)
        c = (A ? A->hopsAway : -1) - (B ? B->hopsAway : -1);
        break;
    case 7: // Encryp
        c = (int)((A && A->encrypted) ? 1 : 0) - (int)((B && B->encrypted) ? 1 : 0);
        break;
    case 8: // SNR
        c = (A ? A->snr_x10 : (int)0x80000000) - (B ? B->snr_x10 : (int)0x80000000);
        break;
    case 9: // Model
        c = CmpTextI((A && A->model[0]) ? A->model : TEXT(""),
                     (B && B->model[0]) ? B->model : TEXT(""));
        break;
    default:
        c = 0;
        break;
    }

    if (c == 0)
        c = (int)((DWORD)lParam1 - (DWORD)lParam2);

    return g_SortAsc ? c : -c;
}

void Nodes_OnColumnClick(int column)
{
    if (column < 0) return;

    if (g_SortCol == column)
        g_SortAsc = !g_SortAsc;
    else
    {
        g_SortCol = column;
        g_SortAsc = true;
    }

    if (g_App.hNodesList)
        ListView_SortItems(g_App.hNodesList, Nodes_ListViewCompare, 0);
}

int Nodes_GetCount() { return g_NodeCount; }

NodeInfo* Nodes_GetAt(int index)
{
    if (index < 0 || index >= g_NodeCount) return NULL;
    return &g_Nodes[index];
}

NodeInfo* Nodes_FindOrAdd(DWORD nodeNum)
{
    for (int i = 0; i < g_NodeCount; ++i)
        if (g_Nodes[i].nodeNum == nodeNum) return &g_Nodes[i];

    if (g_NodeCount >= MAX_NODES) return NULL;

    NodeInfo* p = &g_Nodes[g_NodeCount++];
    ZeroMemory(p, sizeof(NodeInfo));
    p->nodeNum = nodeNum;
    p->batteryPct = -1; // unknown until telemetry is seen
    p->batteryMv = 0;
    p->online = false;
    p->hasPosition = false;
    p->hopsAway = -1;
    p->encrypted = false;
    p->snr_x10 = (int)0x80000000; // INT_MIN (avoid including limits.h)
    p->model[0] = 0;
    p->shortName[0] = 0;
    p->longName[0] = 0;
    p->nodeId[0] = 0;

    // lastHeard default: unknown until we actually hear from the node
    ZeroMemory(&p->lastHeard, sizeof(SYSTEMTIME));
    return p;
}

void Nodes_GetStorageFilePath(LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    _tcsncpy(out, g_szStoragePath, cchOut - 1);
    out[cchOut - 1] = 0;
}

void Nodes_GetStorageDir(LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return;

    _tcsncpy(out, g_szStoragePath, cchOut - 1);
    out[cchOut - 1] = 0;

    // trim to last '\'
    int len = _tcslen(out);
    while (len > 0 && out[len - 1] != TEXT('\\')) len--;
    out[len] = 0;
}

void Nodes_Init()
{
    g_NodeCount = 0;
    Direct_Init();

    Nodes_RebuildList();

    // Let map view refresh if it exists
    if (g_App.hMapView)
        InvalidateRect(g_App.hMapView, NULL, TRUE);
}

// eVC++ 3.0 needs this prototype before first use (see Direct_Init)
static void Direct_EnumLoadCb(DWORD nodeNum, void* ctx);

void Direct_Init()
{
    g_DmConvoCount = 0;
    for (int i = 0; i < MAX_DM_CONVOS; ++i)
    {
        g_DmConvos[i].nodeNum = 0;
        g_DmConvos[i].historyRaw[0] = 0;
        g_DmConvos[i].hasLastDay = false;
        g_DmConvos[i].lastY = g_DmConvos[i].lastM = g_DmConvos[i].lastD = 0;
    }

    // No default nodes are added here; demo nodes can be added by the user.
    if (g_App.hDMHistory)
        SetWindowText(g_App.hDMHistory, TEXT(""));

    // Load persisted DM conversations from disk (best-effort).
    // Forward-declare callback for older eVC++ compilers.
    MsgStore_EnumDMConversations(Direct_EnumLoadCb, NULL);
}

static int Direct_FindIndex(DWORD nodeNum)
{
    for (int i = 0; i < g_DmConvoCount; ++i)
        if (g_DmConvos[i].nodeNum == nodeNum) return i;
    return -1;
}
static void Direct_EnumLoadCb(DWORD nodeNum, void* ctx)
{
    // ctx unused
    (void)ctx;
    Direct_AddConversation(nodeNum);
}

void Direct_AddConversation(DWORD nodeNum)
{
    if (nodeNum == 0) return;
    if (Direct_FindIndex(nodeNum) >= 0) return;
    if (g_DmConvoCount >= MAX_DM_CONVOS) return;

    g_DmConvos[g_DmConvoCount].nodeNum = nodeNum;
    g_DmConvos[g_DmConvoCount].historyRaw[0] = 0;
    g_DmConvos[g_DmConvoCount].hasLastDay = false;
    g_DmConvos[g_DmConvoCount].lastY = g_DmConvos[g_DmConvoCount].lastM = g_DmConvos[g_DmConvoCount].lastD = 0;
    g_DmConvos[g_DmConvoCount].unreadCount = 0;

    // Load persisted history (if any)
    MsgStore_LoadDMHistory(nodeNum, g_DmConvos[g_DmConvoCount].historyRaw, MAX_DM_HISTORY_CHARS);

    // Prime last-day tracker from loaded history (best-effort)
    {
        WORD y = 0, m = 0, d = 0;
        if (ParseLastDayFromText(g_DmConvos[g_DmConvoCount].historyRaw, &y, &m, &d))
        {
            g_DmConvos[g_DmConvoCount].hasLastDay = true;
            g_DmConvos[g_DmConvoCount].lastY = y;
            g_DmConvos[g_DmConvoCount].lastM = m;
            g_DmConvos[g_DmConvoCount].lastD = d;
        }
    }
    g_DmConvoCount++;

    Nodes_RebuildList();
}

// ------------------------------------------------------------
// Direct unread helpers
// ------------------------------------------------------------
int Direct_GetUnreadCount(DWORD nodeNum)
{
    int idx = Direct_FindIndex(nodeNum);
    if (idx < 0) return 0;
    if (g_DmConvos[idx].unreadCount < 0) g_DmConvos[idx].unreadCount = 0;
    return g_DmConvos[idx].unreadCount;
}

int Direct_GetTotalUnread()
{
    int total = 0;
    for (int i = 0; i < g_DmConvoCount; ++i)
    {
        if (g_DmConvos[i].unreadCount > 0)
            total += g_DmConvos[i].unreadCount;
    }
    return total;
}

void Direct_IncUnread(DWORD nodeNum)
{
    int idx = Direct_FindIndex(nodeNum);
    if (idx < 0) return;
    if (g_DmConvos[idx].unreadCount < 0) g_DmConvos[idx].unreadCount = 0;
    if (g_DmConvos[idx].unreadCount < 999)
        g_DmConvos[idx].unreadCount++;
}

int Direct_ClearUnread(DWORD nodeNum)
{
    int idx = Direct_FindIndex(nodeNum);
    if (idx < 0) return 0;
    int cleared = g_DmConvos[idx].unreadCount;
    if (cleared < 0) cleared = 0;
    g_DmConvos[idx].unreadCount = 0;
    return cleared;
}

static void Direct_AppendLine(int idx, LPCTSTR prefix, LPCTSTR text, LPCTSTR marker3, int* outMarkerPos)
{
    if (idx < 0 || idx >= g_DmConvoCount) return;
    if (!text) return;

    if (outMarkerPos) *outMarkerPos = -1;

    SYSTEMTIME st;
    GetLocalTime(&st);

    // Day rollover separator (IRC style)
    bool needSep = false;
    if (g_DmConvos[idx].hasLastDay)
    {
        if (g_DmConvos[idx].lastY != st.wYear || g_DmConvos[idx].lastM != st.wMonth || g_DmConvos[idx].lastD != st.wDay)
            needSep = true;
    }

    TCHAR sep[64];
    if (needSep)
        FormatDaySeparator(st, sep, 64);

    TCHAR ts[64];
    FormatChatTimestampStorage(st, ts, 64);

    TCHAR line[512];
    if (marker3 && marker3[0])
        _sntprintf(line, 512, TEXT("[%s] %s %s%s\r\n"), ts,
                   prefix ? prefix : TEXT(""), text, marker3);
    else
        _sntprintf(line, 512, TEXT("[%s] %s %s\r\n"), ts,
                   prefix ? prefix : TEXT(""), text);

    int curLen = _tcslen(g_DmConvos[idx].historyRaw);
    int addLen = _tcslen(line) + (needSep ? _tcslen(sep) : 0);

    // If we are about to overflow, drop oldest half (simple)
    if (curLen + addLen + 1 >= MAX_DM_HISTORY_CHARS)
    {
        int keepFrom = curLen / 2;
        _tcsncpy(g_DmConvos[idx].historyRaw, g_DmConvos[idx].historyRaw + keepFrom, MAX_DM_HISTORY_CHARS - 1);
        g_DmConvos[idx].historyRaw[MAX_DM_HISTORY_CHARS - 1] = 0;
        curLen = _tcslen(g_DmConvos[idx].historyRaw);
    }

    if (needSep)
        _tcscat(g_DmConvos[idx].historyRaw, sep);
    int basePos = _tcslen(g_DmConvos[idx].historyRaw);
    _tcscat(g_DmConvos[idx].historyRaw, line);

    // marker is fixed width and is placed right before "\r\n".
    if (marker3 && marker3[0] && outMarkerPos)
    {
        int ll = _tcslen(line);
        if (ll >= 5) // at least "x - \r\n"
            *outMarkerPos = basePos + (ll - 2 - ACK_MARKER_LEN);
    }

    // Persist DM history to disk
    if (needSep)
        MsgStore_AppendDMLine(g_DmConvos[idx].nodeNum, sep);
    MsgStore_AppendDMLine(g_DmConvos[idx].nodeNum, line);

    // Update last-day marker
    g_DmConvos[idx].hasLastDay = true;
    g_DmConvos[idx].lastY = st.wYear;
    g_DmConvos[idx].lastM = st.wMonth;
    g_DmConvos[idx].lastD = st.wDay;

    // If this convo is currently selected, reflect it in the UI
    if (g_App.hDMNodeList && g_App.hDMHistory)
    {
        DWORD sel = Direct_GetSelectedNode();
        if (sel == g_DmConvos[idx].nodeNum)
            Direct_RefreshHistoryUI(sel);
    }
}

void Direct_OnIncomingText(DWORD fromNode, const char* utf8Text)
{
    if (!fromNode || !utf8Text || !utf8Text[0]) return;

    Direct_AddConversation(fromNode);
    int idx = Direct_FindIndex(fromNode);

    TCHAR wText[512];
#ifdef UNICODE
    MultiByteToWideChar(CP_UTF8, 0, utf8Text, -1, wText, 512);
    Direct_AppendLine(idx, TEXT("[Them]"), wText, NULL, NULL);
#else
    // best-effort for non-unicode builds
    _tcsncpy(wText, utf8Text, 511); wText[511] = 0;
    Direct_AppendLine(idx, "[Them]", wText, NULL, NULL);
#endif
}

void Direct_OnOutgoingText(DWORD toNode, LPCTSTR localText)
{
    if (!toNode || !localText || !localText[0]) return;

    Direct_AddConversation(toNode);
    int idx = Direct_FindIndex(toNode);
    Direct_AppendLine(idx, TEXT("[Me]"), localText, NULL, NULL);
}

void Direct_OnOutgoingTextWithAck(DWORD toNode, LPCTSTR localText, DWORD packetId)
{
    if (!toNode || !localText || !localText[0]) return;

    Direct_AddConversation(toNode);
    int idx = Direct_FindIndex(toNode);

    int markerPos = -1;
    Direct_AppendLine(idx, TEXT("[Me]"), localText, ACK_PENDING_DM, &markerPos);
    if (packetId && markerPos >= 0)
        Direct_RegisterOutgoingAck(packetId, toNode, markerPos);
}

void Direct_RegisterOutgoingAck(DWORD packetId, DWORD toNode, int markerPos)
{
    if (!packetId || markerPos < 0) return;
    int idx = Direct_FindIndex(toNode);
    if (idx < 0) return;

    // Replace any existing entry with same id
    int i;
    for (i = 0; i < 64; ++i)
    {
        if (g_DmAcks[i].used && g_DmAcks[i].packetId == packetId)
        {
            g_DmAcks[i].toNode = toNode;
            g_DmAcks[i].convoIdx = idx;
            g_DmAcks[i].markerPos = markerPos;
            return;
        }
    }
    for (i = 0; i < 64; ++i)
    {
        if (!g_DmAcks[i].used)
        {
            g_DmAcks[i].used = true;
            g_DmAcks[i].packetId = packetId;
            g_DmAcks[i].toNode = toNode;
            g_DmAcks[i].convoIdx = idx;
            g_DmAcks[i].markerPos = markerPos;
            return;
        }
    }
    // If full, drop oldest (shift)
    for (i = 1; i < 64; ++i)
        g_DmAcks[i - 1] = g_DmAcks[i];
    g_DmAcks[63].used = true;
    g_DmAcks[63].packetId = packetId;
    g_DmAcks[63].toNode = toNode;
    g_DmAcks[63].convoIdx = idx;
    g_DmAcks[63].markerPos = markerPos;
}

void Direct_OnRoutingAck(DWORD ackFromNode, DWORD requestId, bool success)
{
    if (!requestId) return;

    for (int i = 0; i < 64; ++i)
    {
        if (!g_DmAcks[i].used || g_DmAcks[i].packetId != requestId)
            continue;

        int idx = g_DmAcks[i].convoIdx;
        int pos = g_DmAcks[i].markerPos;
        if (idx < 0 || idx >= g_DmConvoCount) { g_DmAcks[i].used = false; return; }

        const TCHAR* marker = ACK_PENDING_DM;
        if (success)
        {
            if (ackFromNode != 0 && ackFromNode == g_DmAcks[i].toNode)
                marker = ACK_DIRECT_DM;
            else
                marker = ACK_MESH_DM;
        }

        // Update the in-memory history text
        int histLen = _tcslen(g_DmConvos[idx].historyRaw);
        if (pos >= 0 && pos + ACK_MARKER_LEN <= histLen)
        {
            for (int k = 0; k < ACK_MARKER_LEN; ++k)
            g_DmConvos[idx].historyRaw[pos + k] = marker[k];

            // Persist: rewrite the full DM file (simple and robust)
            MsgStore_OverwriteDMHistory(g_DmConvos[idx].nodeNum, g_DmConvos[idx].historyRaw);

            // If selected, refresh UI
            if (g_App.hDMNodeList && g_App.hDMHistory)
            {
                DWORD sel = Direct_GetSelectedNode();
                if (sel == g_DmConvos[idx].nodeNum)
                    Direct_RefreshHistoryUI(sel);
            }
        }

        g_DmAcks[i].used = false;
        return;
    }
}


static bool g_DmAckBlinkOn = true;

void Direct_TickAckBlink()
{
    // Blink only for the currently selected DM convo.
    DWORD sel = Direct_GetSelectedNode();
    if (!sel) return;
    int idx = Direct_FindIndex(sel);
    if (idx < 0) return;

    g_DmAckBlinkOn = !g_DmAckBlinkOn;

    bool changed = false;
    for (int i = 0; i < 64; ++i)
    {
        if (!g_DmAcks[i].used) continue;
        if (g_DmAcks[i].convoIdx != idx) continue;

        int pos = g_DmAcks[i].markerPos;
        int histLen = _tcslen(g_DmConvos[idx].historyRaw);
        if (pos < 0 || pos + ACK_MARKER_LEN > histLen) continue;

        // Only blink pending markers by toggling dash visibility.
        const TCHAR* src = g_DmAckBlinkOn ? ACK_PENDING_DM : ACK_BLANK_DM;
        for (int k = 0; k < ACK_MARKER_LEN; ++k)
            g_DmConvos[idx].historyRaw[pos + k] = src[k];
        changed = true;
    }

    if (changed)
        Direct_RefreshHistoryUI(sel);
}

DWORD Direct_GetSelectedNode()
{
    if (!g_App.hDMNodeList) return 0;
    int sel = ListView_GetNextItem(g_App.hDMNodeList, -1, LVNI_SELECTED);
    if (sel < 0) return 0;
    LVITEM lvi;
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.iItem = sel;
    lvi.mask = LVIF_PARAM;
    if (!ListView_GetItem(g_App.hDMNodeList, &lvi)) return 0;
    return (DWORD)lvi.lParam;
}

void Direct_OnSelectionChanged()
{
    DWORD nodeNum = Direct_GetSelectedNode();
    Direct_RefreshHistoryUI(nodeNum);

    // Tell the main UI logic (MESHTASTIC.cpp) that the selection changed so it can
    // arm the "mark as read" timer.
    UI_Unread_OnDirectSelectionChanged(nodeNum);
}

void Direct_RefreshTimestampDisplay()
{
    Direct_RefreshHistoryUI(Direct_GetSelectedNode());
}

// Called when the timestamp display setting changes.
void Direct_OnTimestampDisplayModeChanged()
{
    if (!g_App.hDMHistory) return;
    DWORD nodeNum = Direct_GetSelectedNode();
    Direct_RefreshHistoryUI(nodeNum);
}

void Nodes_RebuildList()
{
    if (!g_App.hNodesList) return;

    // Recompute online/offline based on lastHeard age.
    // We can't trust the firmware to explicitly tell us "offline", so we
    // treat nodes as online only if we've heard from them recently.
    const DWORD ONLINE_TIMEOUT_SEC = 15 * 60; // 15 minutes
    SYSTEMTIME nowSt;
    FILETIME nowFt;
    GetLocalTime(&nowSt);
    SystemTimeToFileTime(&nowSt, &nowFt);
    ULARGE_INTEGER nowUi;
    nowUi.LowPart = nowFt.dwLowDateTime;
    nowUi.HighPart = nowFt.dwHighDateTime;

    // NOTE: eVC++ (old MSVC) treats the loop variable as function-scoped,
    // so don't redeclare it in multiple for-loops.
    int i;
    for (i = 0; i < g_NodeCount; ++i)
    {
        NodeInfo* n = &g_Nodes[i];
        if (n->lastHeard.wYear == 0)
        {
            n->online = false;
            continue;
        }

        FILETIME heardFt;
        if (!SystemTimeToFileTime(&n->lastHeard, &heardFt))
        {
            n->online = false;
            continue;
        }

        ULARGE_INTEGER heardUi;
        heardUi.LowPart = heardFt.dwLowDateTime;
        heardUi.HighPart = heardFt.dwHighDateTime;

        if (nowUi.QuadPart <= heardUi.QuadPart)
        {
            n->online = true;
            continue;
        }

        // FILETIME is 100ns units
        ULONGLONG delta100ns = (nowUi.QuadPart - heardUi.QuadPart);
        // FILETIME is 100ns units; avoid ULL suffixes for the old compiler.
        DWORD deltaSec = (DWORD)(delta100ns / (ULONGLONG)10000000);
        n->online = (deltaSec <= ONLINE_TIMEOUT_SEC);
    }

    TCHAR search[64]; search[0] = 0;
    if (g_App.hNodesSearch)
        GetWindowText(g_App.hNodesSearch, search, 64);

    // Nodes list: ListView in report mode
    ListView_DeleteAllItems(g_App.hNodesList);

    int outIndex = 0;
    for (i = 0; i < g_NodeCount; ++i)
    {
        NodeInfo* p = &g_Nodes[i];

        if (!p->nodeId[0])
        {
            _sntprintf(p->nodeId, 12, TEXT("!%08lX"), p->nodeNum);
        }

        TCHAR dispShort[16];
        TCHAR dispLong[64];
        Nodes_GetDisplayShortName(p, dispShort, 16);
        Nodes_GetDisplayLongName(p, dispLong, 64);
        // Match search across short name, long name and node id
        TCHAR combined[256];
        _sntprintf(combined, 256, TEXT("%s %s %s"),
                   dispShort,
                   dispLong,
                   p->nodeId[0] ? p->nodeId : TEXT(""));

        if (!StrContainsI(combined, search))
            continue;

        TCHAR status[16];
        _tcscpy(status, p->online ? TEXT("Online") : TEXT("Offline"));

        TCHAR batt[24];
        batt[0] = 0;
        if (p->batteryPct >= 0)
        {
            if (p->batteryMv > 0)
            {
                // Compact, but more informative than just a percent.
                // Example: "87% 4.07V" or "100% 5.01V" (USB)
                int vWhole = p->batteryMv / 1000;
                int vFrac = (p->batteryMv % 1000) / 10; // 2 decimals
                if (p->batteryMv >= 4700)
                    _sntprintf(batt, 24, TEXT("%d%% %d.%02dV"), p->batteryPct, vWhole, vFrac);
                else
                    _sntprintf(batt, 24, TEXT("%d%% %d.%02dV"), p->batteryPct, vWhole, vFrac);
            }
            else
            {
                _sntprintf(batt, 24, TEXT("%d%%"), p->batteryPct);
            }
        }

        TCHAR last[32];
        if (p->lastHeard.wYear == 0)
            _tcscpy(last, TEXT(""));
        else
            _sntprintf(last, 32, TEXT("%02d:%02d"), p->lastHeard.wHour, p->lastHeard.wMinute);

        LVITEM lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = outIndex;
        lvi.pszText = (LPTSTR)(dispShort[0] ? dispShort : TEXT("(no short)"));
        lvi.lParam = (LPARAM)p->nodeNum;

        int idx = ListView_InsertItem(g_App.hNodesList, &lvi);
        if (idx >= 0)
        {
            ListView_SetItemText(g_App.hNodesList, idx, 1, (LPTSTR)(dispLong[0] ? dispLong : TEXT("")));
            ListView_SetItemText(g_App.hNodesList, idx, 2, (LPTSTR)p->nodeId);
            ListView_SetItemText(g_App.hNodesList, idx, 3, status);
            ListView_SetItemText(g_App.hNodesList, idx, 4, batt);
            ListView_SetItemText(g_App.hNodesList, idx, 5, last);

            TCHAR conn[16]; conn[0] = 0;
            if (p->hopsAway >= 0) _sntprintf(conn, 16, TEXT("%d"), p->hopsAway);
            ListView_SetItemText(g_App.hNodesList, idx, 6, conn);

            TCHAR enc[4]; _tcscpy(enc, p->encrypted ? TEXT("Y") : TEXT("N"));
            ListView_SetItemText(g_App.hNodesList, idx, 7, enc);

            TCHAR snr[16]; snr[0] = 0;
            if (p->snr_x10 != (int)0x80000000)
            {
                int whole = p->snr_x10 / 10;
                int frac = p->snr_x10 % 10;
                if (frac < 0) frac = -frac;
                _sntprintf(snr, 16, TEXT("%d.%d"), whole, frac);
            }
            ListView_SetItemText(g_App.hNodesList, idx, 8, snr);

            ListView_SetItemText(g_App.hNodesList, idx, 9, (LPTSTR)(p->model[0] ? p->model : TEXT("")));
            ++outIndex;
        }
    }

    // Update "nodes found" label
    if (g_App.hNodesCount)
    {
        TCHAR cnt[64];
        if (search[0])
            _sntprintf(cnt, 64, TEXT("%d found"), outIndex);
        else
            _sntprintf(cnt, 64, TEXT("%d nodes"), outIndex);
        SetWindowText(g_App.hNodesCount, cnt);
    }

    // DM node list (Direct tab) - only nodes in DM conversation list
    if (g_App.hDMNodeList)
    {
        DWORD prevSelNode = Direct_GetSelectedNode();
        ListView_DeleteAllItems(g_App.hDMNodeList);

        for (int j = 0; j < g_DmConvoCount; ++j)
        {
            DWORD nodeNum = g_DmConvos[j].nodeNum;
            NodeInfo* n = Nodes_FindOrAdd(nodeNum);
            if (n && !n->nodeId[0])
                _sntprintf(n->nodeId, 12, TEXT("!%08lX"), n->nodeNum);

            LVITEM lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = j;
            TCHAR ds[16]; Nodes_GetDisplayShortName(n, ds, 16);
            lvi.pszText = (LPTSTR)(ds[0] ? ds : TEXT("(node)"));
            lvi.lParam = (LPARAM)nodeNum;

            int idx = ListView_InsertItem(g_App.hDMNodeList, &lvi);
            if (idx >= 0)
            {
                TCHAR dl[64]; Nodes_GetDisplayLongName(n, dl, 64);
                ListView_SetItemText(g_App.hDMNodeList, idx, 1, (LPTSTR)(dl[0] ? dl : TEXT("")));
                ListView_SetItemText(g_App.hDMNodeList, idx, 2, (LPTSTR)((n && n->nodeId[0]) ? n->nodeId : TEXT("")));
            }
        }

        // Restore selection
        int toSelect = -1;
        for (int k = 0; k < g_DmConvoCount; ++k)
        {
            if (g_DmConvos[k].nodeNum == prevSelNode) { toSelect = k; break; }
        }
        if (toSelect >= 0)
        {
            ListView_SetItemState(g_App.hDMNodeList, toSelect, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_App.hDMNodeList, toSelect, FALSE);
        }

        Direct_OnSelectionChanged();
    }
}

void Nodes_OnSearchChange()
{
    Nodes_RebuildList();
}

void Nodes_OnChooseStoragePath(HWND hwndOwner)
{
    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwndOwner;
    ofn.lpstrFilter = TEXT("Meshtastic data (*.dat)\0*.dat\0All files (*.*)\0*.*\0\0");
    ofn.lpstrFile   = g_szStoragePath;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = TEXT("dat");
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (GetSaveFileName(&ofn))
    {
        // Refresh map too (map lives under <storage dir>\Maps\...)
        if (g_App.hMapView)
            InvalidateRect(g_App.hMapView, NULL, TRUE);
    }
}