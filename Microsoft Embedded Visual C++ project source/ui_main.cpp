#include "stdafx.h"
#include "app.h"
#include "nodes.h"
#include "NodeDetails.h"
#include "mapview.h"
#include "msgstore.h"
#include "serial.h"
#include <stdio.h>
#include "config.h"   // MODEM_PRESET_* constants


// --------------------------------------------------
// Input history (Chat / Direct / Serial)
// --------------------------------------------------
struct INPUT_HISTORY
{
    TCHAR entries[20][256];
    int count;
    int pos; // 0..count, where count means "current draft"
    TCHAR draft[256];
};

static INPUT_HISTORY g_HistChat = { { {0} }, 0, 0, {0} };
static INPUT_HISTORY g_HistDM   = { { {0} }, 0, 0, {0} };
static INPUT_HISTORY g_HistSerial = { { {0} }, 0, 0, {0} };

static void History_Push(INPUT_HISTORY* h, LPCTSTR text)
{
    if (!h || !text || !text[0]) return;
    // reset browse state
    h->pos = h->count;
    h->draft[0] = 0;

    // If last entry is identical, don't duplicate
    if (h->count > 0 && _tcsicmp(h->entries[h->count - 1], text) == 0)
        return;

    // Shift if full
    if (h->count >= 20)
    {
        for (int i = 1; i < 20; ++i)
            _tcscpy(h->entries[i - 1], h->entries[i]);
        h->count = 19;
    }

    _tcsncpy(h->entries[h->count], text, 255);
    h->entries[h->count][255] = 0;
    h->count++;
    h->pos = h->count;
}

static void History_Browse(INPUT_HISTORY* h, HWND hEdit, int dir)
{
    if (!h || !hEdit || h->count <= 0) return;

    // Save draft when entering history
    if (h->pos == h->count)
        GetWindowText(hEdit, h->draft, 256);

    h->pos += dir;
    if (h->pos < 0) h->pos = 0;
    if (h->pos > h->count) h->pos = h->count;

    if (h->pos == h->count)
        SetWindowText(hEdit, h->draft);
    else
        SetWindowText(hEdit, h->entries[h->pos]);

    // put caret at end
    int len = GetWindowTextLength(hEdit);
    SendMessage(hEdit, EM_SETSEL, len, len);
}

void UI_HistoryPushChat(LPCTSTR text) { History_Push(&g_HistChat, text); }
void UI_HistoryPushDM(LPCTSTR text) { History_Push(&g_HistDM, text); }
void UI_HistoryPushSerial(LPCTSTR text) { History_Push(&g_HistSerial, text); }

// Subclass procs for Enter key / double-click on node lists
static WNDPROC g_OldNodesListProc  = NULL;
static WNDPROC g_OldDMNodeListProc = NULL;
static WNDPROC g_OldSettingsPageProc = NULL;
static WNDPROC g_OldSerialPageProc = NULL;
static WNDPROC g_OldSerialInputProc = NULL;
static WNDPROC g_OldSerialLogProc = NULL;
static WNDPROC g_OldChatInputProc = NULL;
static WNDPROC g_OldDMInputProc = NULL;
static WNDPROC g_OldChatPageProc = NULL;
static WNDPROC g_OldDirectPageProc = NULL;
static WNDPROC g_OldNodesPageProc = NULL;
static WNDPROC g_OldMapPageProc = NULL;
static int g_SettingsScrollPos = 0;

// Compact indicator text for the current public-channel preset (read from device settings)
static const TCHAR* PresetToString(int preset)
{
    switch (preset)
    {
        case MODEM_PRESET_LONG_FAST:      return TEXT("Long Range - Fast");
        case MODEM_PRESET_LONG_MODERATE:  return TEXT("Long Range - Moderate");
        case MODEM_PRESET_LONG_SLOW:      return TEXT("Long Range - Slow");
        case MODEM_PRESET_VERY_LONG_SLOW: return TEXT("Very Long Range - Slow");
        case MODEM_PRESET_MEDIUM_FAST:    return TEXT("Medium Range - Fast");
        case MODEM_PRESET_MEDIUM_SLOW:    return TEXT("Medium Range - Slow");
        case MODEM_PRESET_SHORT_TURBO:    return TEXT("Short Range - Turbo");
        case MODEM_PRESET_SHORT_FAST:     return TEXT("Short Range - Fast");
        case MODEM_PRESET_SHORT_SLOW:     return TEXT("Short Range - Slow");
        default:                          return TEXT("");
    }
}

void UI_UpdateChatPresetIndicator()
{
    if (!g_App.hChatPreset) return;
    DeviceSettings* devCfg = Config_GetDevice();
    if (!devCfg || !devCfg->hasReceivedConfig)
    {
        SetWindowText(g_App.hChatPreset, TEXT(""));
        return;
    }
    SetWindowText(g_App.hChatPreset, PresetToString(devCfg->modemPreset));
}

LRESULT CALLBACK NodesList_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DMNodesList_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SettingsPage_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SerialPage_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SerialInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SerialLog_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ChatInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DMInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PageForward_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK SettingsPage_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COMMAND)
    {
        // Forward button clicks to main window
        HWND hTab = GetParent(hwnd);
        HWND hMain = GetParent(hTab);
        if (hMain)
            return SendMessage(hMain, WM_COMMAND, wParam, lParam);
    }
    
    if (msg == WM_VSCROLL)
    {
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        
        int oldPos = si.nPos;
        
        switch (LOWORD(wParam))
        {
            case SB_LINEUP:   si.nPos -= 10; break;
            case SB_LINEDOWN: si.nPos += 10; break;
            case SB_PAGEUP:   si.nPos -= si.nPage; break;
            case SB_PAGEDOWN: si.nPos += si.nPage; break;
            case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd, SB_VERT, &si);
        
        if (si.nPos != oldPos)
        {
            // Manual scroll - move all child windows
            int delta = oldPos - si.nPos;
            HWND hChild = GetWindow(hwnd, GW_CHILD);
            while (hChild)
            {
                RECT rc;
                GetWindowRect(hChild, &rc);
                ScreenToClient(hwnd, (POINT*)&rc.left);
                ScreenToClient(hwnd, (POINT*)&rc.right);
                MoveWindow(hChild, rc.left, rc.top + delta, 
                          rc.right - rc.left, rc.bottom - rc.top, TRUE);
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }
            g_SettingsScrollPos = si.nPos;
        }
        
        return 0;
    }
    
    return CallWindowProc(g_OldSettingsPageProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SerialPage_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COMMAND)
    {
        // Forward button clicks to main window
        HWND hMain = GetParent(GetParent(hwnd)); // hwnd -> hTab -> hMain
        if (hMain)
            return SendMessage(hMain, WM_COMMAND, wParam, lParam);
    }
    
    return CallWindowProc(g_OldSerialPageProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SerialInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN))
    {
        History_Browse(&g_HistSerial, hwnd, (wParam == VK_UP) ? -1 : 1);
        return 0;
    }
    // WinCE typically sends WM_KEYDOWN(VK_RETURN) followed by WM_CHAR(VK_RETURN).
    // Trigger send on WM_KEYDOWN only, and eat WM_CHAR to prevent duplicate sends.
    if (msg == WM_CHAR && wParam == VK_RETURN)
        return 0;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        // Get main window: input -> page -> tab -> main
        HWND hPage = GetParent(hwnd);
        HWND hTab = GetParent(hPage);
        HWND hMain = GetParent(hTab);
        if (hMain)
        {
            SendMessage(hMain, WM_COMMAND, MAKEWPARAM(IDC_SERIAL_SEND, BN_CLICKED), 
                       (LPARAM)g_App.hSerialSend);
        }
        return 0;
    }
    
    return CallWindowProc(g_OldSerialInputProc, hwnd, msg, wParam, lParam);
}

// Enable Ctrl+A (Select All) for the read-only serial log edit box.
// WinCE edit control does not always implement this shortcut by default.
LRESULT CALLBACK SerialLog_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        // Ctrl + A
        if ((wParam == 'A' || wParam == 'a') && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            SendMessage(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    return CallWindowProc(g_OldSerialLogProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ChatInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN))
    {
        History_Browse(&g_HistChat, hwnd, (wParam == VK_UP) ? -1 : 1);
        return 0;
    }
    // WinCE typically sends WM_KEYDOWN(VK_RETURN) followed by WM_CHAR(VK_RETURN).
    // Trigger send on WM_KEYDOWN only, and eat WM_CHAR to prevent duplicate sends.
    if (msg == WM_CHAR && wParam == VK_RETURN)
        return 0;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        // Trigger Send on the currently active chat page
        HWND hPage = GetParent(hwnd);
        HWND hTab  = GetParent(hPage);
        HWND hMain = GetParent(hTab);
        if (hMain)
        {
            SendMessage(hMain, WM_COMMAND, MAKEWPARAM(IDC_CHAT_SEND, BN_CLICKED), (LPARAM)g_App.hChatSend);
        }
        return 0;
    }
    return CallWindowProc(g_OldChatInputProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DMInput_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN))
    {
        History_Browse(&g_HistDM, hwnd, (wParam == VK_UP) ? -1 : 1);
        return 0;
    }
    // WinCE typically sends WM_KEYDOWN(VK_RETURN) followed by WM_CHAR(VK_RETURN).
    // Trigger send on WM_KEYDOWN only, and eat WM_CHAR to prevent duplicate sends.
    if (msg == WM_CHAR && wParam == VK_RETURN)
        return 0;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        HWND hPage = GetParent(hwnd);
        HWND hTab  = GetParent(hPage);
        HWND hMain = GetParent(hTab);
        if (hMain)
        {
            SendMessage(hMain, WM_COMMAND, MAKEWPARAM(IDC_DM_SEND, BN_CLICKED), (LPARAM)g_App.hDMSend);
        }
        return 0;
    }
    return CallWindowProc(g_OldDMInputProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PageForward_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_COMMAND)
        {
            HWND hTab = GetParent(hwnd);
            HWND hMain = GetParent(hTab);
            if (hMain)
                return SendMessage(hMain, WM_COMMAND, wParam, lParam);
        }
        else if (msg == WM_NOTIFY)
        {
            // Some controls (e.g. ListView) send WM_NOTIFY to their parent (the page). Forward to main.
            HWND hTab = GetParent(hwnd);
            HWND hMain = GetParent(hTab);
            if (hMain)
                return SendMessage(hMain, WM_NOTIFY, wParam, lParam);
        }

        // Determine which old proc to call
        if (hwnd == g_App.hPageChat && g_OldChatPageProc) return CallWindowProc(g_OldChatPageProc, hwnd, msg, wParam, lParam);
        if (hwnd == g_App.hPageDirect && g_OldDirectPageProc) return CallWindowProc(g_OldDirectPageProc, hwnd, msg, wParam, lParam);
        if (hwnd == g_App.hPageNodes && g_OldNodesPageProc) return CallWindowProc(g_OldNodesPageProc, hwnd, msg, wParam, lParam);
        if (hwnd == g_App.hPageMap && g_OldMapPageProc) return CallWindowProc(g_OldMapPageProc, hwnd, msg, wParam, lParam);

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

LRESULT CALLBACK NodesList_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if ((msg == WM_KEYDOWN && wParam == VK_RETURN) ||
        (msg == WM_LBUTTONDBLCLK))
    {
        DWORD nodeNum = 0;
        int sel = -1;

        if (hwnd == g_App.hNodesList)
        {
            sel = ListView_GetNextItem(hwnd, -1, LVNI_SELECTED);
            if (sel >= 0)
            {
                LVITEM lvi;
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.iItem = sel;
                lvi.mask = LVIF_PARAM;
                if (ListView_GetItem(hwnd, &lvi))
                    nodeNum = (DWORD)lvi.lParam;
            }
        }
        else
        {
            // Backward-compat for any listbox usage
            sel = (int)SendMessage(hwnd, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR)
                nodeNum = (DWORD)SendMessage(hwnd, LB_GETITEMDATA, sel, 0);
        }

        if (nodeNum)
        {
            NodeInfo* p = Nodes_FindOrAdd(nodeNum);
            if (p)
            {
                HWND hPage = GetParent(hwnd);
                HWND hTab  = GetParent(hPage);
                HWND hMain = GetParent(hTab);
                NodeDetails_Show(hMain, hwnd, p);
            }
        }
        return 0;
    }

    return CallWindowProc(g_OldNodesListProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DMNodesList_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_LBUTTONDOWN)
    {
        // If user taps empty area, clear selection and show placeholder text
        POINT pt;
        pt.x = LOWORD(lParam);
        pt.y = HIWORD(lParam);
        LVHITTESTINFO ht;
        ZeroMemory(&ht, sizeof(ht));
        ht.pt = pt;
        int hit = ListView_HitTest(hwnd, &ht);
        if (hit < 0)
        {
            // Clear selection
            int count = ListView_GetItemCount(hwnd);
            for (int i = 0; i < count; ++i)
                ListView_SetItemState(hwnd, i, 0, LVIS_SELECTED | LVIS_FOCUSED);
            Direct_OnSelectionChanged();
        }
        // Continue default processing (so scrollbar etc. still works)
    }
    if ((msg == WM_KEYDOWN && wParam == VK_RETURN) ||
        (msg == WM_LBUTTONDBLCLK))
    {
        DWORD nodeNum = 0;
        int sel = ListView_GetNextItem(hwnd, -1, LVNI_SELECTED);
        if (sel >= 0)
        {
            LVITEM lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.iItem = sel;
            lvi.mask = LVIF_PARAM;
            if (ListView_GetItem(hwnd, &lvi))
                nodeNum = (DWORD)lvi.lParam;
        }

        if (nodeNum)
        {
            NodeInfo* p = Nodes_FindOrAdd(nodeNum);
            if (p)
            {
                HWND hPage = GetParent(hwnd);
                HWND hTab  = GetParent(hPage);
                HWND hMain = GetParent(hTab);
                NodeDetails_Show(hMain, hwnd, p);
            }
        }
        return 0;
    }

    return CallWindowProc(g_OldDMNodeListProc, hwnd, msg, wParam, lParam);
}

void CreateMainControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    g_App.hTab = CreateWindowEx(
        0, WC_TABCONTROL, TEXT(""),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, rc.right, rc.bottom,
        hwnd, (HMENU)IDC_MAIN_TAB, g_App.hInst, NULL);

    // Always-visible connect button (mirrors Settings->Connect Serial)
    // IMPORTANT: Parent to the MAIN window, not the tab.
    // Otherwise WM_COMMAND / WM_CTLCOLORSTATIC go to the Tab control and the
    // button/indicators appear "dead" (no clicks / no color changes).
    g_App.hTopConnect = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("Connect"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        hwnd, (HMENU)IDC_TOP_CONNECT, g_App.hInst, NULL);

    // Owner-draw so we can paint green when connected.
    if (g_App.hTopConnect)
    {
        LONG s = GetWindowLong(g_App.hTopConnect, GWL_STYLE);
        SetWindowLong(g_App.hTopConnect, GWL_STYLE, s | BS_OWNERDRAW);
    }

    // Simple activity indicators (RX/TX) next to the Connect button
    g_App.hTopRx = CreateWindowEx(0, TEXT("STATIC"), TEXT("RX"),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, 0, 0, 0,
        hwnd, (HMENU)3001, g_App.hInst, NULL);
    g_App.hTopTx = CreateWindowEx(0, TEXT("STATIC"), TEXT("TX"),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, 0, 0, 0,
        hwnd, (HMENU)3002, g_App.hInst, NULL);

    // Add tabs
    TCITEM tie;
    ZeroMemory(&tie, sizeof(tie));
    tie.mask = TCIF_TEXT;

    tie.pszText = TEXT("Chat");
    TabCtrl_InsertItem(g_App.hTab, TAB_CHAT, &tie);

    tie.pszText = TEXT("Direct");
    TabCtrl_InsertItem(g_App.hTab, TAB_DIRECT, &tie);

    tie.pszText = TEXT("Nodes");
    TabCtrl_InsertItem(g_App.hTab, TAB_NODES, &tie);

    tie.pszText = TEXT("Map");
    TabCtrl_InsertItem(g_App.hTab, TAB_MAP, &tie);

    tie.pszText = TEXT("Settings");
    TabCtrl_InsertItem(g_App.hTab, TAB_SETTINGS, &tie);

    tie.pszText = TEXT("Serial");
    TabCtrl_InsertItem(g_App.hTab, TAB_SERIAL, &tie);

    // Create page containers
    g_App.hPageChat = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD | WS_VISIBLE, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);
    g_App.hPageDirect = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);
    g_App.hPageNodes = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);
    g_App.hPageMap = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);
    // Settings page needs vertical scrolling
    g_App.hPageSettings = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD | WS_VSCROLL, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);
    g_App.hPageSerial = CreateWindowEx(0, TEXT("STATIC"), NULL, WS_CHILD, 0,0,0,0, g_App.hTab, NULL, g_App.hInst, NULL);

    // Forward WM_COMMAND/WM_NOTIFY from the STATIC page windows to the main window
    g_OldChatPageProc   = (WNDPROC)SetWindowLong(g_App.hPageChat,   GWL_WNDPROC, (LONG)PageForward_SubclassProc);
    g_OldDirectPageProc = (WNDPROC)SetWindowLong(g_App.hPageDirect, GWL_WNDPROC, (LONG)PageForward_SubclassProc);
    g_OldNodesPageProc  = (WNDPROC)SetWindowLong(g_App.hPageNodes,  GWL_WNDPROC, (LONG)PageForward_SubclassProc);
    g_OldMapPageProc    = (WNDPROC)SetWindowLong(g_App.hPageMap,    GWL_WNDPROC, (LONG)PageForward_SubclassProc);

    //--------------------------------------------------
    // Chat tab controls
    //--------------------------------------------------
    // Compact preset indicator (top-right of the public chat thread)
    g_App.hChatPreset = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("STATIC"), TEXT(""),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, 0, 0, 0,
        g_App.hPageChat, (HMENU)IDC_CHAT_PRESET, g_App.hInst, NULL);

    g_App.hChatHistory = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 0, 0,
        g_App.hPageChat, (HMENU)IDC_CHAT_HISTORY, g_App.hInst, NULL);

    g_App.hChatInput = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        g_App.hPageChat, (HMENU)IDC_CHAT_INPUT, g_App.hInst, NULL);

    g_OldChatInputProc = (WNDPROC)SetWindowLong(g_App.hChatInput, GWL_WNDPROC, (LONG)ChatInput_SubclassProc);

    g_App.hChatSend = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("Send"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        g_App.hPageChat, (HMENU)IDC_CHAT_SEND, g_App.hInst, NULL);

    // Ensure it reflects current device setting if we already have it
    UI_UpdateChatPresetIndicator();

    //--------------------------------------------------
    // Direct tab controls
    //--------------------------------------------------
    g_App.hDMNodeList = CreateWindowEx(
        WS_EX_CLIENTEDGE, WC_LISTVIEW, TEXT(""),
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        g_App.hPageDirect, (HMENU)IDC_DM_NODELIST, g_App.hInst, NULL);

    g_OldDMNodeListProc = (WNDPROC)SetWindowLong(g_App.hDMNodeList, GWL_WNDPROC,
                                                 (LONG)DMNodesList_SubclassProc);

    // DM node list columns
    ListView_SetExtendedListViewStyle(g_App.hDMNodeList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMN dmcol; ZeroMemory(&dmcol, sizeof(dmcol));
    dmcol.mask = LVCF_TEXT | LVCF_WIDTH;
    dmcol.pszText = TEXT("Short"); dmcol.cx = 60; ListView_InsertColumn(g_App.hDMNodeList, 0, &dmcol);
    dmcol.pszText = TEXT("Long name"); dmcol.cx = 110; ListView_InsertColumn(g_App.hDMNodeList, 1, &dmcol);
    dmcol.pszText = TEXT("Node ID"); dmcol.cx = 80; ListView_InsertColumn(g_App.hDMNodeList, 2, &dmcol);

    // Force a sane row height (WinCE ListView: attach a small imagelist)
    HIMAGELIST hDMIml = ImageList_Create(1, 18, ILC_COLOR, 1, 1);
    if (hDMIml) ListView_SetImageList(g_App.hDMNodeList, hDMIml, LVSIL_SMALL);

    g_App.hDMHistory = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 0, 0,
        g_App.hPageDirect, (HMENU)IDC_DM_HISTORY, g_App.hInst, NULL);

    g_App.hDMInput = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        g_App.hPageDirect, (HMENU)IDC_DM_INPUT, g_App.hInst, NULL);

    g_OldDMInputProc = (WNDPROC)SetWindowLong(g_App.hDMInput, GWL_WNDPROC, (LONG)DMInput_SubclassProc);

    g_App.hDMSend = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("Send"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        g_App.hPageDirect, (HMENU)IDC_DM_SEND, g_App.hInst, NULL);

    // DM node list is built dynamically from Nodes_RebuildList()

    //--------------------------------------------------
    // Nodes tab controls
    //--------------------------------------------------
    g_App.hNodesSearch = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        g_App.hPageNodes, (HMENU)IDC_NODES_SEARCH, g_App.hInst, NULL);

    g_App.hNodesClear = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("X"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        g_App.hPageNodes, (HMENU)IDC_NODES_CLEAR, g_App.hInst, NULL);

    g_App.hNodesCount = CreateWindowEx(
        0, TEXT("STATIC"), TEXT("0"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0,
        g_App.hPageNodes, (HMENU)IDC_NODES_COUNT, g_App.hInst, NULL);

    g_App.hNodesList = CreateWindowEx(
        WS_EX_CLIENTEDGE, WC_LISTVIEW, TEXT(""),
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        g_App.hPageNodes, (HMENU)IDC_NODES_LIST, g_App.hInst, NULL);


    // Configure Nodes list as a report-style ListView (columns)
    ListView_SetExtendedListViewStyle(g_App.hNodesList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMN col; ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;

    col.pszText = TEXT("Short");     col.cx = 55;  ListView_InsertColumn(g_App.hNodesList, 0, &col);
    col.pszText = TEXT("Long name"); col.cx = 105; ListView_InsertColumn(g_App.hNodesList, 1, &col);
    col.pszText = TEXT("Node ID");   col.cx = 80;  ListView_InsertColumn(g_App.hNodesList, 2, &col);
    col.pszText = TEXT("Status");    col.cx = 55;  ListView_InsertColumn(g_App.hNodesList, 3, &col);
    col.pszText = TEXT("Batt");      col.cx = 45;  ListView_InsertColumn(g_App.hNodesList, 4, &col);
    col.pszText = TEXT("Last online"); col.cx = 72;  ListView_InsertColumn(g_App.hNodesList, 5, &col);
    col.pszText = TEXT("Conn");      col.cx = 42;  ListView_InsertColumn(g_App.hNodesList, 6, &col);
    col.pszText = TEXT("Encryp");    col.cx = 50;  ListView_InsertColumn(g_App.hNodesList, 7, &col);
    col.pszText = TEXT("SNR");       col.cx = 45;  ListView_InsertColumn(g_App.hNodesList, 8, &col);
    col.pszText = TEXT("Model");     col.cx = 70;  ListView_InsertColumn(g_App.hNodesList, 9, &col);

    // Force a sane row height (WinCE ListView: attach a small imagelist)
    HIMAGELIST hNodesIml = ImageList_Create(1, 20, ILC_COLOR, 1, 1);
    if (hNodesIml) ListView_SetImageList(g_App.hNodesList, hNodesIml, LVSIL_SMALL);

    g_OldNodesListProc = (WNDPROC)SetWindowLong(g_App.hNodesList, GWL_WNDPROC,
                                                (LONG)NodesList_SubclassProc);

    //--------------------------------------------------
    // Map tab controls
    //--------------------------------------------------
    MapView_Register(g_App.hInst);
    g_App.hMapView = CreateWindowEx(
        WS_EX_CLIENTEDGE, MAPVIEW_CLASS, NULL,
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        g_App.hPageMap, (HMENU)IDC_MAP_VIEW, g_App.hInst, NULL);

    //--------------------------------------------------
    // Settings tab controls
    //--------------------------------------------------
    int yPos = 5;
    int labelH = 15;
    int ctrlH = 20;
    int gap = 5;
    int labelW = 110;
    int ctrlW = 180;

    // Settings header button (positioned properly in LayoutControls)
    g_App.hSetAbout = CreateWindowEx(0, TEXT("BUTTON"), TEXT("About"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 52, 18,
        g_App.hPageSettings, (HMENU)IDC_SET_ABOUT, g_App.hInst, NULL);

    // Storage path
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Storage Path:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetStoragePath = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        5 + labelW, yPos-2, ctrlW, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_STORAGE_PATH, g_App.hInst, NULL);
    g_App.hSetBrowse = CreateWindowEx(0, TEXT("BUTTON"), TEXT("..."),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        5 + labelW + ctrlW + 5, yPos-2, 30, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_BROWSE, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // COM Port (dropdown)
    CreateWindowEx(0, TEXT("STATIC"), TEXT("COM Port:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetComPort = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("COMBOBOX"), NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        5 + labelW, yPos-2, 130, 150,
        g_App.hPageSettings, (HMENU)IDC_SET_COM_PORT, g_App.hInst, NULL);
    SendMessage(g_App.hSetComPort, CB_ADDSTRING, 0, (LPARAM)TEXT("COM1 (Serial)"));
    SendMessage(g_App.hSetComPort, CB_ADDSTRING, 0, (LPARAM)TEXT("COM3 (Infrared)"));
    SendMessage(g_App.hSetComPort, CB_SETCURSEL, 0, 0); // Default COM1
    yPos += ctrlH + gap + 5;

    // Chat timestamp mode (dropdown)
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Chat timestamps:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetChatTsMode = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("COMBOBOX"), NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        5 + labelW, yPos-2, 130, 150,
        g_App.hPageSettings, (HMENU)IDC_SET_CHAT_TS_MODE, g_App.hInst, NULL);
    SendMessage(g_App.hSetChatTsMode, CB_ADDSTRING, 0, (LPARAM)TEXT("Time only"));
    SendMessage(g_App.hSetChatTsMode, CB_ADDSTRING, 0, (LPARAM)TEXT("Date + Time"));
    SendMessage(g_App.hSetChatTsMode, CB_SETCURSEL, 0, 0); // Default time only
    yPos += ctrlH + gap + 5;

    // Chat auto-scroll behavior
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Chat auto-scroll:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetChatFollow = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("COMBOBOX"), NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        5 + labelW, yPos - 2, 170, 150,
        g_App.hPageSettings, (HMENU)IDC_SET_CHAT_FOLLOW, g_App.hInst, NULL);
    SendMessage(g_App.hSetChatFollow, CB_ADDSTRING, 0, (LPARAM)TEXT("Off"));
    SendMessage(g_App.hSetChatFollow, CB_ADDSTRING, 0, (LPARAM)TEXT("Follow (if at end)"));
    SendMessage(g_App.hSetChatFollow, CB_ADDSTRING, 0, (LPARAM)TEXT("Always jump to latest"));
    SendMessage(g_App.hSetChatFollow, CB_SETCURSEL, 1, 0); // Default Follow
    yPos += ctrlH + gap;

    // Jornada front button LED
    CreateWindowEx(0, TEXT("STATIC"), TEXT("LED:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);

    // Two checkboxes on one line (New message / Unread blink)
    g_App.hSetLedNewMsg = CreateWindowEx(0, TEXT("BUTTON"), TEXT("New msg"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        5 + labelW, yPos - 2, 80, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_LED_NEWMSG, g_App.hInst, NULL);

    g_App.hSetLedUnread = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Unread"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        5 + labelW + 85, yPos - 2, 80, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_LED_UNREAD, g_App.hInst, NULL);

    yPos += ctrlH + gap;

    // Prevent sleep while connected
    g_App.hSetPreventSleep = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Prevent sleep while connected"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        5 + labelW, yPos - 2, ctrlW + 30, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_PREVENT_SLEEP, g_App.hInst, NULL);

    yPos += ctrlH + gap;

    // Serial monitor options
    g_App.hSetSerialMonitorEnable = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Enable serial monitor"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        5 + labelW, yPos - 2, ctrlW + 30, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_SERIAL_MONITOR_ENABLE, g_App.hInst, NULL);

    yPos += ctrlH + gap;

    CreateWindowEx(0, TEXT("STATIC"), TEXT("Serial max lines:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetSerialMaxLines = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        5 + labelW, yPos - 2, 70, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_SERIAL_MAX_LINES, g_App.hInst, NULL);
    CreateWindowEx(0, TEXT("STATIC"), TEXT("(0/-1 = no limit)"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5 + labelW + 75, yPos, ctrlW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);

    yPos += ctrlH + gap;

    yPos += 5;

    // Save settings (Jornada app settings) - directly below Chat auto-scroll.
    // Make it wide enough so the label is not clipped.
    g_App.hSetSave = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Save Settings"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        5, yPos, ctrlW + labelW + 30, 25,
        g_App.hPageSettings, (HMENU)IDC_SET_SAVE, g_App.hInst, NULL);
    yPos += 30;

    // Separator line with dashes
    CreateWindowEx(0, TEXT("STATIC"), TEXT("--------------------------------"),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        5, yPos, ctrlW + labelW + 30, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    yPos += labelH;
    
    // Section header for Meshtastic settings
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Meshtastic Node Settings:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, ctrlW + labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    yPos += labelH + gap;

    // Role (read-only indicator)
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Role:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);

    // Static text field (no user editing)
    g_App.hSetRole = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("STATIC"), TEXT(""),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5 + labelW, yPos-2, ctrlW, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_ROLE, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // GPS Mode (read-only indicator)
    CreateWindowEx(0, TEXT("STATIC"), TEXT("GPS Mode:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);

    // Static text field (no user editing)
    g_App.hSetGpsMode = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("STATIC"), TEXT(""),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5 + labelW, yPos-2, ctrlW, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_GPS_MODE, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // Region
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Region:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetRegion = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("COMBOBOX"), NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        5 + labelW, yPos-2, ctrlW, 200,
        g_App.hPageSettings, (HMENU)IDC_SET_REGION, g_App.hInst, NULL);
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("Unset"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("US"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("EU 433 MHz"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("EU 868 MHz"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("CN"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("JP"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("ANZ"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("KR"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("TW"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("RU"));
    SendMessage(g_App.hSetRegion, CB_ADDSTRING, 0, (LPARAM)TEXT("IN"));
    SendMessage(g_App.hSetRegion, CB_SETCURSEL, -1, 0); // No default
    yPos += ctrlH + gap;

    // Presets (Modem Preset)
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Presets:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetModemPreset = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("COMBOBOX"), NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        5 + labelW, yPos-2, ctrlW, 200,
        g_App.hPageSettings, (HMENU)IDC_SET_MODEM_PRESET, g_App.hInst, NULL);
    // Preset list order shown in the UI does NOT have to match Meshtastic enum values.
    // We store the Meshtastic enum value as item-data for each entry.
    int idx;
    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Long Range - Fast"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_LONG_FAST);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Long Range - Moderate"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_LONG_MODERATE);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Long Range - Slow"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_LONG_SLOW);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Very Long Range - Slow"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_VERY_LONG_SLOW);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Medium Range - Fast"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_MEDIUM_FAST);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Medium Range - Slow"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_MEDIUM_SLOW);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Short Range - Turbo"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_SHORT_TURBO);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Short Range - Fast"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_SHORT_FAST);

    idx = (int)SendMessage(g_App.hSetModemPreset, CB_ADDSTRING, 0, (LPARAM)TEXT("Short Range - Slow"));
    SendMessage(g_App.hSetModemPreset, CB_SETITEMDATA, idx, MODEM_PRESET_SHORT_SLOW);
    SendMessage(g_App.hSetModemPreset, CB_SETCURSEL, -1, 0); // No default
    yPos += ctrlH + gap;

    // Transmit Enabled
    g_App.hSetTxEnabled = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Transmit Enabled"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        5, yPos, ctrlW, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_TX_ENABLED, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // TX Power
    CreateWindowEx(0, TEXT("STATIC"), TEXT("TX Power (dBm):"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetTxPower = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        5 + labelW, yPos-2, 50, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_TX_POWER, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // Number of Hops
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Number of Hops:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetHopLimit = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        5 + labelW, yPos-2, 50, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_HOP_LIMIT, g_App.hInst, NULL);
    yPos += ctrlH + gap;

    // Frequency Slot
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Frequency Slot:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    g_App.hSetFreqSlot = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        5 + labelW, yPos-2, 50, ctrlH,
        g_App.hPageSettings, (HMENU)IDC_SET_FREQ_SLOT, g_App.hInst, NULL);
    yPos += ctrlH + gap;
    // Device action buttons - place below the device fields, above the final separator.
    // Make buttons wide enough so labels are not clipped.
    g_App.hSetApply = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Apply to Device"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        5, yPos, 135, 25,
        g_App.hPageSettings, (HMENU)IDC_SET_APPLY, g_App.hInst, NULL);

    g_App.hSetGetSettings = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Get Settings from Device"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        5 + 135 + 10, yPos, 175, 25,
        g_App.hPageSettings, (HMENU)IDC_SET_GET_SETTINGS, g_App.hInst, NULL);

    yPos += 30;

    // Separator line with dashes
    CreateWindowEx(0, TEXT("STATIC"), TEXT("--------------------------------"),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        5, yPos, ctrlW + labelW + 30, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    yPos += labelH;
    
    // Section header for Jornada controls
    CreateWindowEx(0, TEXT("STATIC"), TEXT("Jornada App Controls:"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        5, yPos, ctrlW + labelW, labelH,
        g_App.hPageSettings, NULL, g_App.hInst, NULL);
    yPos += labelH + gap;

    // Row 1: Connection and device settings buttons
    g_App.hSetConnect = CreateWindowEx(0, TEXT("BUTTON"), TEXT("Connect"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        5, yPos, 80, 25,
        g_App.hPageSettings, (HMENU)IDC_SET_CONNECT, g_App.hInst, NULL);
    
    yPos += 30; // Total content height
    
    // Set up scrolling for settings page
    SCROLLINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = yPos;
    si.nPage = 200; // Visible height (will be updated in layout)
    SetScrollInfo(g_App.hPageSettings, SB_VERT, &si, TRUE);
    
    // Subclass for scroll handling
    g_OldSettingsPageProc = (WNDPROC)SetWindowLong(g_App.hPageSettings, GWL_WNDPROC,
                                                   (LONG)SettingsPage_SubclassProc);

    //--------------------------------------------------
    // Serial Monitor tab controls
    //--------------------------------------------------
    g_App.hSerialLog = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 0, 0,
        g_App.hPageSerial, (HMENU)IDC_SERIAL_LOG, g_App.hInst, NULL);
    
    // Add initial message / disabled notice depending on settings
    UI_RefreshSerialMonitorState();

    // Apply current serial monitor state (can override the initial message)
    UI_RefreshSerialMonitorState();

    // Subclass serial log to implement Ctrl+A select-all
    g_OldSerialLogProc = (WNDPROC)SetWindowLong(g_App.hSerialLog, GWL_WNDPROC,
                                                (LONG)SerialLog_SubclassProc);

    g_App.hSerialInput = CreateWindowEx(
        WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        g_App.hPageSerial, (HMENU)IDC_SERIAL_INPUT, g_App.hInst, NULL);

    g_App.hSerialSend = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("Send"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        g_App.hPageSerial, (HMENU)IDC_SERIAL_SEND, g_App.hInst, NULL);

    g_App.hSerialClear = CreateWindowEx(
        0, TEXT("BUTTON"), TEXT("Clear"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0,
        g_App.hPageSerial, (HMENU)IDC_SERIAL_CLEAR, g_App.hInst, NULL);
    
    // Subclass serial input for Enter key
    g_OldSerialInputProc = (WNDPROC)SetWindowLong(g_App.hSerialInput, GWL_WNDPROC,
                                                  (LONG)SerialInput_SubclassProc);
    
    // Subclass serial page to forward button clicks
    g_OldSerialPageProc = (WNDPROC)SetWindowLong(g_App.hPageSerial, GWL_WNDPROC,
                                                 (LONG)SerialPage_SubclassProc);

    Nodes_Init();

    // Load persisted message history
    MsgStore_LoadChatHistory(g_App.hChatHistory);

    LayoutControls(hwnd);
    ShowTabPage(TAB_CHAT);
}

void LayoutControls(HWND hwnd)
{
    if (!g_App.hTab) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    MoveWindow(g_App.hTab, 0, 0, rc.right, rc.bottom, TRUE);

    // Position the top "Connect" button and activity indicators inside the tab header bar (right side)
    // and size tab items so labels like "Chat (12)" still fit.
    {
        RECT rItem;

        // Slightly increase the height of the always-visible top-bar controls (RX/TX + Connect)
        // without moving them. On Jornada LCDs, a couple of extra pixels makes the text sit much
        // nicer inside the button/static boxes.
        const int TOPBAR_CTRL_HEIGHT_INC = 2; // tweak: 1..4

        // Jornada-friendly: slightly shorter tab header to reclaim vertical space.
        // Adjust these constants if you want tighter/looser header sizing.
        const int TABH_TARGET = 20; // typical good-looking height on Jornada 720
        const int TABH_MIN    = 18; // don't go below this (stylus usability)

        int tabH = 22; // fallback
        if (TabCtrl_GetItemRect(g_App.hTab, 0, &rItem))
        {
            // Current measured tab item height (from control)
            int measured = (rItem.bottom - rItem.top);

            // Trim a little (CE tabs tend to be tall), then clamp.
            tabH = measured - 2;
        }

        if (tabH > TABH_TARGET) tabH = TABH_TARGET;
        if (tabH < TABH_MIN)    tabH = TABH_MIN;

        int btnW = 70;
        int btnH = (tabH - 4) + TOPBAR_CTRL_HEIGHT_INC;
        // Keep the top edge fixed (y=2). Don't exceed the available header height.
        int maxBtnH = tabH - 2;
        if (btnH > maxBtnH) btnH = maxBtnH;
        if (btnH < 14) btnH = 14;

        // Use the few spare pixels below the controls to improve vertical padding.
        btnH += TOPBAR_CTRL_HEIGHT_INC;

        int indW = 24;
        int gap = 2;

        // Tab item width: reserve room on the right for Connect + RX/TX indicators.
        // WinCE tab control uses a fixed width for all tabs.
        int tabCount = 6;
        int reservedRight = btnW + (indW * 2) + (gap * 4) + 12;
        int avail = rc.right - reservedRight;
        int itemW = (tabCount > 0) ? (avail / tabCount) : 60;
        if (itemW < 42) itemW = 42;
        TabCtrl_SetItemSize(g_App.hTab, itemW, tabH);
        int x = rc.right - 4;

        if (g_App.hTopConnect)
        {
            x -= btnW;
            MoveWindow(g_App.hTopConnect, x, 2, btnW, btnH, TRUE);
            x -= gap;
        }

        if (g_App.hTopTx)
        {
            x -= indW;
            MoveWindow(g_App.hTopTx, x, 3, indW, btnH - 2, TRUE);
            x -= gap;
        }

        if (g_App.hTopRx)
        {
            x -= indW;
            MoveWindow(g_App.hTopRx, x, 3, indW, btnH - 2, TRUE);
        }

        // Ensure the always-visible controls stay clickable (above the tab control)
        if (g_App.hTopConnect) SetWindowPos(g_App.hTopConnect, HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
        if (g_App.hTopRx)      SetWindowPos(g_App.hTopRx,      HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
        if (g_App.hTopTx)      SetWindowPos(g_App.hTopTx,      HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
    }

    RECT rTab;
    GetClientRect(g_App.hTab, &rTab);
    TabCtrl_AdjustRect(g_App.hTab, FALSE, &rTab);

    MoveWindow(g_App.hPageChat, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);
    MoveWindow(g_App.hPageDirect, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);
    MoveWindow(g_App.hPageNodes, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);
    MoveWindow(g_App.hPageMap, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);
    MoveWindow(g_App.hPageSettings, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);
    MoveWindow(g_App.hPageSerial, rTab.left, rTab.top, rTab.right-rTab.left, rTab.bottom-rTab.top, TRUE);

    int margin = 4;
    int sendBtnWidth = 60;
    int inputHeight = 20;

    RECT r;

    // Chat
    GetClientRect(g_App.hPageChat, &r);
    // Overlay the preset indicator on top of the chat history (do NOT reduce history space)
    const int presetH = 20;
    const int presetW = 128;

    MoveWindow(g_App.hChatHistory, margin, margin,
               r.right - r.left - 2*margin,
               r.bottom - r.top - 3*margin - inputHeight, TRUE);

    if (g_App.hChatPreset)
    {
        // The chat history can display a vertical scrollbar. Since the preset indicator is
        // intentionally overlaid on top of the history, keep it clear of the scrollbar area.
        int vscrollW = GetSystemMetrics(SM_CXVSCROLL);
        if (vscrollW <= 0) vscrollW = 16; // sensible fallback for some CE skins
		
		const int PRESET_EXTRA_LEFT = 2; // <-- adjust this manually
        MoveWindow(g_App.hChatPreset,
                   r.right - r.left - margin - presetW - vscrollW - PRESET_EXTRA_LEFT, margin,
                   presetW, presetH, TRUE);
        // Keep it above the history control so it renders as an overlay.
        SetWindowPos(g_App.hChatPreset, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    }

    MoveWindow(g_App.hChatInput, margin,
               r.bottom - r.top - margin - inputHeight,
               r.right - r.left - 3*margin - sendBtnWidth,
               inputHeight, TRUE);
    MoveWindow(g_App.hChatSend, r.right - r.left - margin - sendBtnWidth,
               r.bottom - r.top - margin - inputHeight,
               sendBtnWidth, inputHeight, TRUE);

    // Direct
    GetClientRect(g_App.hPageDirect, &r);
    int nodeListWidth = 120;
    MoveWindow(g_App.hDMNodeList, margin, margin,
               nodeListWidth - margin,
               r.bottom - r.top - 2*margin, TRUE);
    MoveWindow(g_App.hDMHistory, nodeListWidth + margin, margin,
               r.right - r.left - nodeListWidth - 2*margin,
               r.bottom - r.top - 3*margin - inputHeight, TRUE);
    MoveWindow(g_App.hDMInput, nodeListWidth + margin,
               r.bottom - r.top - margin - inputHeight,
               r.right - r.left - nodeListWidth - 3*margin - sendBtnWidth,
               inputHeight, TRUE);
    MoveWindow(g_App.hDMSend, r.right - r.left - margin - sendBtnWidth,
               r.bottom - r.top - margin - inputHeight,
               sendBtnWidth, inputHeight, TRUE);

    // Nodes
    GetClientRect(g_App.hPageNodes, &r);
    int searchHeight = 20;
    int clearW = 22;
    int countW = 70;
    int searchW = (r.right - r.left) - 2*margin - clearW - margin - countW;
    if (searchW < 40) searchW = 40;

    MoveWindow(g_App.hNodesSearch, margin, margin,
               searchW,
               searchHeight, TRUE);
    MoveWindow(g_App.hNodesClear, margin + searchW + margin, margin,
               clearW, searchHeight, TRUE);
    MoveWindow(g_App.hNodesCount, margin + searchW + margin + clearW + margin, margin + 2,
               countW, searchHeight, TRUE);
    MoveWindow(g_App.hNodesList, margin,
               margin + searchHeight + margin,
               r.right - r.left - 2*margin,
               r.bottom - r.top - 3*margin - searchHeight, TRUE);

    // Map
    GetClientRect(g_App.hPageMap, &r);
    MoveWindow(g_App.hMapView, margin, margin,
               r.right - r.left - 2*margin,
               r.bottom - r.top - 2*margin, TRUE);

    // Settings tab - update scroll info based on visible area
    GetClientRect(g_App.hPageSettings, &r);
    if (r.bottom > r.top)
    {
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_PAGE;
        si.nPage = r.bottom - r.top;
        SetScrollInfo(g_App.hPageSettings, SB_VERT, &si, TRUE);
    }

    // Settings "About" button: top-right corner, 2px left of the vertical scroll bar,
    // and 2px below the top border.
    if (g_App.hSetAbout)
    {
        int sbW = GetSystemMetrics(SM_CXVSCROLL);
        if (sbW <= 0) sbW = 16; // fallback

        const int aboutW = 52;
        const int aboutH = 18;
        int x = (r.right - r.left) - sbW - 2 - aboutW;
        if (x < 2) x = 2;
        MoveWindow(g_App.hSetAbout, x, 2, aboutW, aboutH, TRUE);
        SetWindowPos(g_App.hSetAbout, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    }

    // Serial Monitor
    GetClientRect(g_App.hPageSerial, &r);
    int sendBtnWidth2 = 60;
    int clearBtnWidth = 60;
    MoveWindow(g_App.hSerialLog, margin, margin,
               r.right - r.left - 2*margin,
               r.bottom - r.top - 3*margin - inputHeight, TRUE);
    MoveWindow(g_App.hSerialInput, margin,
               r.bottom - r.top - margin - inputHeight,
               r.right - r.left - 4*margin - sendBtnWidth2 - clearBtnWidth,
               inputHeight, TRUE);
    MoveWindow(g_App.hSerialSend, 
               r.right - r.left - 2*margin - sendBtnWidth2 - clearBtnWidth,
               r.bottom - r.top - margin - inputHeight,
               sendBtnWidth2, inputHeight, TRUE);
    MoveWindow(g_App.hSerialClear,
               r.right - r.left - margin - clearBtnWidth,
               r.bottom - r.top - margin - inputHeight,
               clearBtnWidth, inputHeight, TRUE);
}

void UI_UpdateTopConnectButton()
{
    if (!g_App.hTopConnect) return;

    if (Serial_IsOpen())
        SetWindowText(g_App.hTopConnect, TEXT("Connected"));
    else
        SetWindowText(g_App.hTopConnect, TEXT("Connect"));

    InvalidateRect(g_App.hTopConnect, NULL, TRUE);
    UpdateWindow(g_App.hTopConnect);
}

void ShowTabPage(int index)
{
    ShowWindow(g_App.hPageChat,     (index == TAB_CHAT)     ? SW_SHOW : SW_HIDE);
    ShowWindow(g_App.hPageDirect,   (index == TAB_DIRECT)   ? SW_SHOW : SW_HIDE);
    ShowWindow(g_App.hPageNodes,    (index == TAB_NODES)    ? SW_SHOW : SW_HIDE);
    ShowWindow(g_App.hPageMap,      (index == TAB_MAP)      ? SW_SHOW : SW_HIDE);
    ShowWindow(g_App.hPageSettings, (index == TAB_SETTINGS) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_App.hPageSerial,   (index == TAB_SERIAL)   ? SW_SHOW : SW_HIDE);

    if (index == TAB_MAP && g_App.hMapView)
        InvalidateRect(g_App.hMapView, NULL, TRUE);

    // Put the cursor in the relevant input box when switching tabs
    if (index == TAB_CHAT && g_App.hChatInput)
        SetFocus(g_App.hChatInput);
    else if (index == TAB_DIRECT && g_App.hDMInput)
        SetFocus(g_App.hDMInput);
}

void UI_GotoDirectConversation(DWORD nodeNum)
{
    if (!nodeNum || !g_App.hTab || !g_App.hDMNodeList)
        return;

    // Ensure conversation exists
    Direct_AddConversation(nodeNum);

    // Switch to Direct tab
    TabCtrl_SetCurSel(g_App.hTab, TAB_DIRECT);
    ShowTabPage(TAB_DIRECT);

    // Select the node in the DM list
    int count = ListView_GetItemCount(g_App.hDMNodeList);
    for (int i = 0; i < count; ++i)
    {
        LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_PARAM;
        lvi.iItem = i;
        if (ListView_GetItem(g_App.hDMNodeList, &lvi))
        {
            if ((DWORD)lvi.lParam == nodeNum)
            {
                ListView_SetItemState(g_App.hDMNodeList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g_App.hDMNodeList, i, FALSE);
                break;
            }
        }
    }

    // Refresh history and focus input
    Direct_OnSelectionChanged();
    if (g_App.hDMInput) SetFocus(g_App.hDMInput);
}

void AppendTextWithTimestamp(HWND hEdit, LPCTSTR pszPrefix, LPCTSTR pszText)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    TCHAR line[512];
    _sntprintf(line, 512, TEXT("[%02d:%02d] %s %s\r\n"),
               st.wHour, st.wMinute,
               pszPrefix ? pszPrefix : TEXT(""),
               pszText ? pszText : TEXT(""));

    int len = GetWindowTextLength(hEdit);
    SendMessage(hEdit, EM_SETSEL, len, len);
    SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
}

void AppendSerialLog(const char* pszLine, bool isRx)
{
    static bool s_disabledMessageShown = false;

    if (!g_App.hSerialLog)
        return;

    // If serial monitor is disabled, avoid all formatting / UI work for performance.
    AppConfig* cfg = Config_GetApp();
    if (cfg && !cfg->serialMonitorEnabled)
    {
        if (!s_disabledMessageShown)
        {
            SetWindowText(g_App.hSerialLog,
                TEXT("SERIAL MONITOR DISABLED IN SETTINGS\r\n\r\n")
                TEXT("Enable it in Settings to view the serial log.\r\n"));
            s_disabledMessageShown = true;
        }
        return;
    }

    // Serial monitor enabled
    s_disabledMessageShown = false;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char prefix[16];
    sprintf(prefix, "%s", isRx ? "RX" : "TX");

    TCHAR line[1024];
#ifdef UNICODE
    WCHAR wPrefix[16];
    WCHAR wLine[512];
    MultiByteToWideChar(CP_ACP, 0, prefix, -1, wPrefix, 16);
    MultiByteToWideChar(CP_ACP, 0, pszLine, -1, wLine, 512);
    _sntprintf(line, 1024, TEXT("[%02d:%02d:%02d] [%s] %s\r\n"),
               st.wHour, st.wMinute, st.wSecond,
               wPrefix, wLine);
#else
    _sntprintf(line, 1024, TEXT("[%02d:%02d:%02d] [%s] %s\r\n"),
               st.wHour, st.wMinute, st.wSecond,
               prefix, pszLine);
#endif

    int len = GetWindowTextLength(g_App.hSerialLog);
    SendMessage(g_App.hSerialLog, EM_SETSEL, len, len);
    SendMessage(g_App.hSerialLog, EM_REPLACESEL, FALSE, (LPARAM)line);
    
    // Auto-scroll to bottom
    SendMessage(g_App.hSerialLog, EM_SCROLLCARET, 0, 0);

    // Trim oldest lines if a max line count is configured.
    int maxLines = (cfg ? cfg->serialMonitorMaxLines : 0);
    if (maxLines > 0)
    {
        int lineCount = (int)SendMessage(g_App.hSerialLog, EM_GETLINECOUNT, 0, 0);
        if (lineCount > maxLines)
        {
            int removeLines = lineCount - maxLines;
            // Index of the first line we want to keep.
            int keepFrom = removeLines;
            LRESULT charIndex = SendMessage(g_App.hSerialLog, EM_LINEINDEX, (WPARAM)keepFrom, 0);
            if (charIndex > 0)
            {
                SendMessage(g_App.hSerialLog, EM_SETSEL, 0, charIndex);
                SendMessage(g_App.hSerialLog, EM_REPLACESEL, FALSE, (LPARAM)TEXT(""));
            }
        }
    }
}

void UI_RefreshSerialMonitorState()
{
    if (!g_App.hSerialLog)
        return;

    AppConfig* cfg = Config_GetApp();
    if (cfg && !cfg->serialMonitorEnabled)
    {
        SetWindowText(g_App.hSerialLog,
            TEXT("SERIAL MONITOR DISABLED IN SETTINGS\r\n\r\n")
            TEXT("Enable it in Settings to view the serial log.\r\n"));
    }
    else
    {
        // Only set the ready text if the control is empty or currently showing the disabled message.
        // This avoids clobbering the user's existing log.
        TCHAR head[64];
        head[0] = 0;
        GetWindowText(g_App.hSerialLog, head, 60);
        if (_tcsstr(head, TEXT("SERIAL MONITOR DISABLED")) != NULL || head[0] == 0)
        {
            SetWindowText(g_App.hSerialLog, TEXT("Serial Monitor Ready\r\nConnect to device in Settings tab first.\r\n\r\n"));
        }
    }
}
