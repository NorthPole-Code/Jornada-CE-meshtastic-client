#include "stdafx.h"
#include "msgstore.h"
#include "config.h"
#include <stdio.h>

// ACK marker rendering (fixed width)
#define ACK_MARKER_LEN 4
static const TCHAR* const ACK_PENDING_MARKER = TEXT("  - ");
static const TCHAR* const ACK_MESH_MARKER    = TEXT("  + ");
static const TCHAR* const ACK_DIRECT_MARKER  = TEXT(" ++ ");
static const TCHAR* const ACK_BLANK_MARKER   = TEXT("    ");

// ------------------------------------------------------------
// Edit-control scroll helpers (WinCE friendly)
// ------------------------------------------------------------
static int Edit_GetVisibleLineCount(HWND hEdit)
{
    if (!hEdit) return 1;

    RECT rc;
    GetClientRect(hEdit, &rc);
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
    // Tolerate a 1-line slack so it still counts as "at bottom".
    return (first + vis >= total - 1);
}

// Track last appended day for day-rollover separators (broadcast chat)
static bool g_HasLastChatDay = false;
static WORD g_LastChatY = 0, g_LastChatM = 0, g_LastChatD = 0;

// Timestamp formatting:
// - Storage: ALWAYS full date+time, so the UI can re-render later if the user
//   switches between time-only and date+time.
// - Display: depends on AppConfig::chatTimestampMode.
static void FormatChatTimestamp_Display(const SYSTEMTIME& st, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;

    AppConfig* cfg = Config_GetApp();
    int mode = (cfg ? cfg->chatTimestampMode : 0);
    if (mode == 1)
    {
        // DD/MM/YY HH:MM
        _sntprintf(out, cchOut, TEXT("%02d/%02d/%02d %02d:%02d"),
            (int)st.wDay, (int)st.wMonth, (int)(st.wYear % 100),
            (int)st.wHour, (int)st.wMinute);
    }
    else
    {
        // HH:MM
        _sntprintf(out, cchOut, TEXT("%02d:%02d"), (int)st.wHour, (int)st.wMinute);
    }
    out[cchOut - 1] = 0;
}

static void FormatChatTimestamp_Storage(const SYSTEMTIME& st, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;

    // Always store full date+time: DD/MM/YY HH:MM
    _sntprintf(out, cchOut, TEXT("%02d/%02d/%02d %02d:%02d"),
        (int)st.wDay, (int)st.wMonth, (int)(st.wYear % 100),
        (int)st.wHour, (int)st.wMinute);
    out[cchOut - 1] = 0;
}

// Render stored history text into display text based on mode.
// If mode==0 (time-only), convert "[DD/MM/YY HH:MM]" to "[HH:MM]".
// Day separators keep their date.
static void RenderHistoryForDisplay(const TCHAR* in, LPTSTR out, int cchOut, int mode)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    if (!in) return;

    if (mode != 0)
    {
        _tcsncpy(out, in, cchOut - 1);
        out[cchOut - 1] = 0;
        return;
    }

    int oi = 0;
    for (int i = 0; in[i] && oi < cchOut - 1; )
    {
        // Match: [DD/MM/YY HH:MM]
        if (in[i] == TEXT('[') &&
            _istdigit(in[i + 1]) && _istdigit(in[i + 2]) &&
            in[i + 3] == TEXT('/') &&
            _istdigit(in[i + 4]) && _istdigit(in[i + 5]) &&
            in[i + 6] == TEXT('/') &&
            _istdigit(in[i + 7]) && _istdigit(in[i + 8]) &&
            in[i + 9] == TEXT(' ') &&
            _istdigit(in[i + 10]) && _istdigit(in[i + 11]) &&
            in[i + 12] == TEXT(':') &&
            _istdigit(in[i + 13]) && _istdigit(in[i + 14]) &&
            in[i + 15] == TEXT(']'))
        {
            // Write "[HH:MM]"
            if (oi + 6 >= cchOut - 1) break;
            out[oi++] = TEXT('[');
            out[oi++] = in[i + 10];
            out[oi++] = in[i + 11];
            out[oi++] = TEXT(':');
            out[oi++] = in[i + 13];
            out[oi++] = in[i + 14];
            out[oi++] = TEXT(']');
            i += 16;
            continue;
        }

        out[oi++] = in[i++];
    }
    out[oi] = 0;
}

static void FormatDaySeparator(const SYSTEMTIME& st, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    _sntprintf(out, cchOut, TEXT("----- %02d/%02d/%02d -----\r\n"),
        (int)st.wDay, (int)st.wMonth, (int)(st.wYear % 100));
    out[cchOut - 1] = 0;
}

// Best-effort: parse the last date we saw in a loaded chat history.
// Accepts either:
//   "----- DD/MM/YY -----" separators
//   "[DD/MM/YY HH:MM]" timestamps
static bool ParseLastDayFromText(const TCHAR* text, WORD* y, WORD* m, WORD* d)
{
    if (!text || !text[0]) return false;

    int len = _tcslen(text);
    if (len <= 0) return false;

    // Scan backwards for a digit that could start DD/MM/YY
    for (int i = len - 1; i >= 0; --i)
    {
        if (text[i] < TEXT('0') || text[i] > TEXT('9')) continue;

        // Find a window ending at i large enough to contain DD/MM/YY
        int start = i - 7;
        if (start < 0) start = 0;

        for (int s = i; s >= start; --s)
        {
            // Expect pattern: d d / m m / y y
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

static void EnsureTrailingSlash(TCHAR* path, int cch)
{
    if (!path || cch <= 0) return;
    int len = _tcslen(path);
    if (len <= 0) return;
    if (path[len - 1] != TEXT('\\'))
    {
        if (len + 1 < cch)
        {
            path[len] = TEXT('\\');
            path[len + 1] = 0;
        }
    }
}

static void GetRootDir(TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;
    AppConfig* appCfg = Config_GetApp();
    if (appCfg && appCfg->storagePath[0])
    {
        _tcsncpy(out, appCfg->storagePath, cchOut - 1);
        out[cchOut - 1] = 0;
        EnsureTrailingSlash(out, cchOut);
        return;
    }
    // fallback
    _tcsncpy(out, TEXT("\\Storage Card\\Meshtastic\\"), cchOut - 1);
    out[cchOut - 1] = 0;
}

static void BuildMessagesDir(TCHAR* out, int cchOut)
{
    TCHAR root[MAX_PATH];
    GetRootDir(root, MAX_PATH);
    _sntprintf(out, cchOut, TEXT("%sMessages\\"), root);
}

static void BuildChatPath(TCHAR* out, int cchOut)
{
    TCHAR dir[MAX_PATH];
    BuildMessagesDir(dir, MAX_PATH);
    _sntprintf(out, cchOut, TEXT("%schat.log"), dir);
}

static void BuildDMPath(DWORD nodeNum, TCHAR* out, int cchOut)
{
    TCHAR dir[MAX_PATH];
    BuildMessagesDir(dir, MAX_PATH);
    _sntprintf(out, cchOut, TEXT("%sdm_%08lX.log"), dir, nodeNum);
}

static void EnsureDirExists(const TCHAR* dirWithTrailingSlash)
{
    if (!dirWithTrailingSlash || !dirWithTrailingSlash[0]) return;

    // CreateDirectory does not create intermediate dirs; do a simple step-wise create.
    TCHAR tmp[MAX_PATH];
    _tcsncpy(tmp, dirWithTrailingSlash, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = 0;

    int len = _tcslen(tmp);
    // remove trailing slash for iteration safety
    if (len > 0 && tmp[len - 1] == TEXT('\\'))
        tmp[len - 1] = 0;

    // Walk components
    for (TCHAR* p = tmp; *p; ++p)
    {
        if (*p == TEXT('\\'))
        {
            *p = 0;
            CreateDirectory(tmp, NULL);
            *p = TEXT('\\');
        }
    }
    CreateDirectory(tmp, NULL);
}

static void AppendTextToFile(const TCHAR* path, const TCHAR* text)
{
    if (!path || !text) return;

#ifdef UNICODE
    char buf[1024];
    WideCharToMultiByte(CP_ACP, 0, text, -1, buf, sizeof(buf), NULL, NULL);
    const char* data = buf;
    DWORD cb = (DWORD)strlen(buf);
#else
    const char* data = text;
    DWORD cb = (DWORD)strlen(text);
#endif

    HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(h, data, cb, &written, NULL);
    CloseHandle(h);
}

static bool LoadWholeFileText(const TCHAR* path, LPTSTR out, int cchOut)
{
    if (!out || cchOut <= 0) return false;
    out[0] = 0;

    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size == 0)
    {
        CloseHandle(h);
        return true;
    }

    // Cap to avoid huge allocations
    const DWORD kMax = 64 * 1024;
    if (size > kMax) size = kMax;

#ifdef UNICODE
    // Read as ANSI then convert.
    char* buf = (char*)LocalAlloc(LPTR, size + 1);
    if (!buf) { CloseHandle(h); return false; }
    DWORD rd = 0;
    ReadFile(h, buf, size, &rd, NULL);
    buf[rd] = 0;
    CloseHandle(h);

    MultiByteToWideChar(CP_ACP, 0, buf, -1, out, cchOut);
    out[cchOut - 1] = 0;
    LocalFree(buf);
#else
    DWORD rd = 0;
    ReadFile(h, out, min((DWORD)(cchOut - 1), size), &rd, NULL);
    out[rd] = 0;
    CloseHandle(h);
#endif
    return true;
}

void MsgStore_Init()
{
    TCHAR msgDir[MAX_PATH];
    BuildMessagesDir(msgDir, MAX_PATH);
    EnsureDirExists(msgDir);
}

void MsgStore_LoadChatHistory(HWND hChatHistory)
{
    if (!hChatHistory) return;
    MsgStore_Init();

    // Chat auto-scroll behavior on refresh:
    // - Off: preserve view position
    // - Follow: stay at bottom only if user was already at bottom
    // - Always: always jump to bottom
    AppConfig* cfg = Config_GetApp();
    int scrollMode = (cfg ? cfg->chatAutoScroll : 1);
    if (scrollMode < 0 || scrollMode > 2) scrollMode = 1;
    bool wasAtBottom = (scrollMode == 1) ? Edit_IsAtBottom(hChatHistory) : false;
    bool forceBottom = (scrollMode == 2);
    int savedFirst = (int)SendMessage(hChatHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
    DWORD selStart = 0, selEnd = 0;
    SendMessage(hChatHistory, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

    TCHAR path[MAX_PATH];
    BuildChatPath(path, MAX_PATH);

    // Allocate a reasonably large buffer for chat history.
    const int kBuf = 64 * 1024 / sizeof(TCHAR);
    LPTSTR buf = (LPTSTR)LocalAlloc(LPTR, kBuf * sizeof(TCHAR));
    if (!buf) return;

    if (LoadWholeFileText(path, buf, kBuf))
    {
        // Render for display based on current setting.
        AppConfig* cfg = Config_GetApp();
        int mode = (cfg ? cfg->chatTimestampMode : 0);

        LPTSTR disp = buf;
        LPTSTR dispAlloc = NULL;
        if (mode == 0)
        {
            dispAlloc = (LPTSTR)LocalAlloc(LPTR, kBuf * sizeof(TCHAR));
            if (dispAlloc)
            {
                RenderHistoryForDisplay(buf, dispAlloc, kBuf, mode);
                disp = dispAlloc;
            }
        }

        SendMessage(hChatHistory, WM_SETREDRAW, FALSE, 0);
        SetWindowText(hChatHistory, disp);

        // Restore view position
        if (forceBottom || (scrollMode == 1 && wasAtBottom))
        {
            int len = GetWindowTextLength(hChatHistory);
            SendMessage(hChatHistory, EM_SETSEL, len, len);
            SendMessage(hChatHistory, EM_SCROLLCARET, 0, 0);
        }
        else
        {
            // Best-effort: restore selection and first visible line.
            SendMessage(hChatHistory, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
            int nowFirst = (int)SendMessage(hChatHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessage(hChatHistory, EM_LINESCROLL, 0, (LPARAM)(savedFirst - nowFirst));
        }

        SendMessage(hChatHistory, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hChatHistory, NULL, TRUE);

        WORD y = 0, m = 0, d = 0;
        if (ParseLastDayFromText(buf, &y, &m, &d))
        {
            g_HasLastChatDay = true;
            g_LastChatY = y; g_LastChatM = m; g_LastChatD = d;
        }
        else
        {
            g_HasLastChatDay = false;
            g_LastChatY = g_LastChatM = g_LastChatD = 0;
        }

        if (dispAlloc)
            LocalFree(dispAlloc);
    }

    LocalFree(buf);
}

int MsgStore_AppendChatWithMarker(HWND hChatHistory, LPCTSTR prefix, LPCTSTR text, LPCTSTR marker)
{
    if (!hChatHistory) return -1;
    MsgStore_Init();

    int markerPosOut = -1;

    AppConfig* cfg = Config_GetApp();
    int scrollMode = (cfg ? cfg->chatAutoScroll : 1);
    if (scrollMode < 0 || scrollMode > 2) scrollMode = 1;
    bool wasAtBottom = (scrollMode == 1) ? Edit_IsAtBottom(hChatHistory) : false;
    bool forceBottom = (scrollMode == 2);
    int savedFirst = (int)SendMessage(hChatHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
    DWORD selStart = 0, selEnd = 0;
    SendMessage(hChatHistory, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);

    SYSTEMTIME st;
    GetLocalTime(&st);

    // Day rollover separator (IRC style)
    bool needSep = false;
    if (g_HasLastChatDay)
    {
        if (g_LastChatY != st.wYear || g_LastChatM != st.wMonth || g_LastChatD != st.wDay)
            needSep = true;
    }
    // If we have no known last day (fresh start), don't insert a separator.

    TCHAR sep[64];
    if (needSep)
        FormatDaySeparator(st, sep, 64);

    // Build display line (respects setting) and stored line (always full date+time)
    TCHAR tsDisp[64];
    TCHAR tsStore[64];
    FormatChatTimestamp_Display(st, tsDisp, 64);
    FormatChatTimestamp_Storage(st, tsStore, 64);

    TCHAR lineDisp[512];
    TCHAR lineStore[512];
    if (marker && marker[0])
    {
        _sntprintf(lineDisp, 512, TEXT("[%s] %s %s%s\r\n"),
                   tsDisp,
                   prefix ? prefix : TEXT(""),
                   text ? text : TEXT(""),
                   marker);
        _sntprintf(lineStore, 512, TEXT("[%s] %s %s%s\r\n"),
                   tsStore,
                   prefix ? prefix : TEXT(""),
                   text ? text : TEXT(""),
                   marker);
    }
    else
    {
        _sntprintf(lineDisp, 512, TEXT("[%s] %s %s\r\n"),
                   tsDisp,
                   prefix ? prefix : TEXT(""),
                   text ? text : TEXT(""));
        _sntprintf(lineStore, 512, TEXT("[%s] %s %s\r\n"),
                   tsStore,
                   prefix ? prefix : TEXT(""),
                   text ? text : TEXT(""));
    }

    // Append to UI:
    // - Off: preserve view position
    // - Follow: only scroll if user was already at bottom
    // - Always: always scroll to the latest message
    SendMessage(hChatHistory, WM_SETREDRAW, FALSE, 0);
    int len = GetWindowTextLength(hChatHistory);
    int sepLen = (needSep ? _tcslen(sep) : 0);
    int lineLen = _tcslen(lineDisp);
    SendMessage(hChatHistory, EM_SETSEL, len, len);
    if (needSep)
        SendMessage(hChatHistory, EM_REPLACESEL, FALSE, (LPARAM)sep);
    SendMessage(hChatHistory, EM_REPLACESEL, FALSE, (LPARAM)lineDisp);

    if (marker && marker[0])
    {
        // marker is a fixed-width suffix right before "\r\n"
        if (lineLen >= 5)
            markerPosOut = len + sepLen + (lineLen - 2 - (int)_tcslen(marker));
    }

    if (forceBottom || (scrollMode == 1 && wasAtBottom))
    {
        int newLen = GetWindowTextLength(hChatHistory);
        SendMessage(hChatHistory, EM_SETSEL, newLen, newLen);
        SendMessage(hChatHistory, EM_SCROLLCARET, 0, 0);
    }
    else
    {
        // Restore selection and view position (best-effort)
        SendMessage(hChatHistory, EM_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
        int nowFirst = (int)SendMessage(hChatHistory, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessage(hChatHistory, EM_LINESCROLL, 0, (LPARAM)(savedFirst - nowFirst));
    }

    SendMessage(hChatHistory, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hChatHistory, NULL, TRUE);

    // Persist
    TCHAR path[MAX_PATH];
    BuildChatPath(path, MAX_PATH);
    if (needSep)
        AppendTextToFile(path, sep);
    AppendTextToFile(path, lineStore);

    // Update last day marker
    g_HasLastChatDay = true;
    g_LastChatY = st.wYear; g_LastChatM = st.wMonth; g_LastChatD = st.wDay;

    return markerPosOut;
}

void MsgStore_AppendChat(HWND hChatHistory, LPCTSTR prefix, LPCTSTR text)
{
    (void)MsgStore_AppendChatWithMarker(hChatHistory, prefix, text, NULL);
}

void MsgStore_AppendDMLine(DWORD nodeNum, LPCTSTR line)
{
    if (!line) return;
    MsgStore_Init();

    TCHAR path[MAX_PATH];
    BuildDMPath(nodeNum, path, MAX_PATH);
    AppendTextToFile(path, line);
}

void MsgStore_OverwriteDMHistory(DWORD nodeNum, LPCTSTR fullText)
{
    MsgStore_Init();
    TCHAR path[MAX_PATH];
    BuildDMPath(nodeNum, path, MAX_PATH);

    HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    if (fullText && fullText[0])
    {
#ifdef UNICODE
        // Keep on-disk encoding consistent with AppendTextToFile/LoadWholeFileText (ANSI/CP_ACP).
        char* buf = NULL;
        int need = WideCharToMultiByte(CP_ACP, 0, fullText, -1, NULL, 0, NULL, NULL);
        if (need > 0)
        {
            buf = (char*)LocalAlloc(LPTR, need);
            if (buf)
            {
                WideCharToMultiByte(CP_ACP, 0, fullText, -1, buf, need, NULL, NULL);
                DWORD cb = (DWORD)strlen(buf);
                DWORD written = 0;
                WriteFile(h, buf, cb, &written, NULL);
                LocalFree(buf);
            }
        }
#else
        DWORD written = 0;
        WriteFile(h, fullText, (DWORD)strlen(fullText), &written, NULL);
#endif
    }

    CloseHandle(h);
}
bool MsgStore_LoadDMHistory(DWORD nodeNum, LPTSTR out, int cchOut)
{
    MsgStore_Init();

    TCHAR path[MAX_PATH];
    BuildDMPath(nodeNum, path, MAX_PATH);
    DWORD attr = GetFileAttributes(path);
    if (attr == 0xFFFFFFFF) return false;

    return LoadWholeFileText(path, out, cchOut);
}

void MsgStore_EnumDMConversations(MSGSTORE_ENUM_DM_CB cb, void* ctx)
{
    if (!cb) return;
    MsgStore_Init();

    TCHAR dir[MAX_PATH];
    BuildMessagesDir(dir, MAX_PATH);

    TCHAR pattern[MAX_PATH];
    _sntprintf(pattern, MAX_PATH, TEXT("%sdm_*.log"), dir);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        // Expect dm_%08X.log
        const TCHAR* name = fd.cFileName;
        if (!name) continue;

        if (_tcslen(name) >= 7 && _tcsnicmp(name, TEXT("dm_"), 3) == 0)
        {
            TCHAR hex[16];
            int i = 0;
            const TCHAR* p = name + 3;
            while (*p && *p != TEXT('.') && i < 15)
                hex[i++] = *p++;
            hex[i] = 0;

            DWORD nodeNum = 0;
#ifdef UNICODE
            nodeNum = (DWORD)wcstoul(hex, NULL, 16);
#else
            nodeNum = (DWORD)strtoul(hex, NULL, 16);
#endif
            if (nodeNum)
                cb(nodeNum, ctx);
        }

    } while (FindNextFile(h, &fd));

    FindClose(h);
}
