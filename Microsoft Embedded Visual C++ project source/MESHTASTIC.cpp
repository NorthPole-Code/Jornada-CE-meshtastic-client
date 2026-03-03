#include "stdafx.h"
#include "app.h"
#include "nodes.h"
#include "resource.h"
#include "config.h"
#include "serial.h"
#include "meshtastic_protocol.h"
#include "settings_ui.h"

// Power management (Windows CE)
extern "C" void WINAPI SystemIdleTimerReset(void);


#include "msgstore.h"
#pragma comment(lib, "commctrl.lib")

APPSTATE g_App;  // global app state defined here

// ------------------------------------------------------------
// Unread / new-message indicators
// ------------------------------------------------------------
static int  s_unreadChat = 0;

// Highlight (flash) background of history edit controls on new messages
static DWORD s_chatFlashUntil = 0;
static DWORD s_dmFlashUntil   = 0;

// "Mark as read" timers
#define TIMER_MARKREAD_CHAT   2001
#define TIMER_MARKREAD_DM     2002

// Unread DM list flash timer + colors (tuned for low-contrast Jornada LCD)
//
// Everything is adjustable from here:
//
//  - UNREAD_BLINK_HZ:     blink frequency in Hz (0.75 = ~0.75 flashes/sec)
//  - UNREAD_COLOR_ON/OFF: background colors for unread rows
//  - HISTORY_FLASH_COLOR: background flash for history boxes (incoming message)
#define TIMER_UNREAD_FLASH  2003

#define TIMER_ACKBLINK  2003
#define UNREAD_BLINK_HZ 0.75f
#define UNREAD_FLASH_TOGGLE_MS ((int)(1000.0f / (UNREAD_BLINK_HZ * 2.0f) + 0.5f))

#define IDT_ACKBLINK  0x45B2
#define ACK_BLINK_TOGGLE_MS 500

// Strong "attention" amber that stays visible on Jornada panels
#define UNREAD_COLOR_ON       RGB(255, 200, 80)
#define UNREAD_COLOR_OFF      RGB(255, 255, 255)
#define UNREAD_TEXT_COLOR     RGB(0, 0, 0)

// History flash (slightly softer than list blink, still obvious)
#define HISTORY_FLASH_COLOR   RGB(255, 230, 140)

static BOOL s_unreadFlashOn = TRUE;

static void UpdateUnreadFlashTimer()
{
    if (!g_App.hMain) return;
    
    int dmUnread = Direct_GetTotalUnread();
    
    // FIX: Calculate total unread (Direct + Chat) and update Hardware LED
    // The previous code did not link the application unread count to the LED subsystem.
    int totalUnread = dmUnread + s_unreadChat;
    LED_SetUnreadActive(totalUnread > 0);

    // Screen UI Flashing (DM List only)
    // We keep the screen list flashing specific to DMs to highlight specific rows,
    // whereas the hardware LED indicates *any* unread message.
    if (dmUnread > 0)
    {
        // keep flashing
        SetTimer(g_App.hMain, TIMER_UNREAD_FLASH, UNREAD_FLASH_TOGGLE_MS, NULL);
    }
    else
    {
        KillTimer(g_App.hMain, TIMER_UNREAD_FLASH);
        s_unreadFlashOn = TRUE;
        if (g_App.hDMNodeList) InvalidateRect(g_App.hDMNodeList, NULL, FALSE);
    }
}

static DWORD s_pendingDmReadNode = 0;

static void UpdateTabLabels()
{
    if (!g_App.hTab) return;

    TCHAR buf[64];
    TCITEM tie; ZeroMemory(&tie, sizeof(tie));
    tie.mask = TCIF_TEXT;

    // Chat
    if (s_unreadChat > 0)
        _sntprintf(buf, 64, TEXT("Chat (%d)"), s_unreadChat);
    else
        _tcscpy(buf, TEXT("Chat"));
    tie.pszText = buf;
    TabCtrl_SetItem(g_App.hTab, TAB_CHAT, &tie);

    // Direct
    int unreadDm = Direct_GetTotalUnread();
    if (unreadDm > 0)
        _sntprintf(buf, 64, TEXT("Direct (%d)"), unreadDm);
    else
        _tcscpy(buf, TEXT("Direct"));
    tie.pszText = buf;
    TabCtrl_SetItem(g_App.hTab, TAB_DIRECT, &tie);
}

// Helper to flash history background for a short time
static void FlashHistory(HWND hEdit, DWORD* untilTick, DWORD ms)
{
    if (!hEdit) return;
    DWORD now = GetTickCount();
    *untilTick = now + ms;
    InvalidateRect(hEdit, NULL, TRUE);
}

void UI_Unread_OnIncomingChat()
{
    LED_OnNewMessage();

    int curTab = g_App.hTab ? TabCtrl_GetCurSel(g_App.hTab) : -1;
    if (curTab == TAB_CHAT)
    {
        // User is on Chat already -> no unread count, just flash and arm mark-read timer.
        FlashHistory(g_App.hChatHistory, &s_chatFlashUntil, 1200);
        KillTimer(g_App.hMain, TIMER_MARKREAD_CHAT);
        SetTimer(g_App.hMain, TIMER_MARKREAD_CHAT, 3000, NULL);
        return;
    }

    if (s_unreadChat < 999) s_unreadChat++;
    UpdateTabLabels();
    
    // FIX: Ensure LED state is updated for Chat messages too
    UpdateUnreadFlashTimer();
}

void UI_Unread_OnIncomingDM(DWORD fromNode)
{
    LED_OnNewMessage();

    if (!fromNode) return;
    int curTab = g_App.hTab ? TabCtrl_GetCurSel(g_App.hTab) : -1;
    DWORD sel = (curTab == TAB_DIRECT) ? Direct_GetSelectedNode() : 0;

    if (curTab == TAB_DIRECT && sel == fromNode)
    {
        // Currently viewing this convo -> flash and arm mark-read timer.
        FlashHistory(g_App.hDMHistory, &s_dmFlashUntil, 1200);
        s_pendingDmReadNode = fromNode;
        KillTimer(g_App.hMain, TIMER_MARKREAD_DM);
        SetTimer(g_App.hMain, TIMER_MARKREAD_DM, 3000, NULL);
        return;
    }

    // Not viewing -> count as unread and highlight node entry
    Direct_IncUnread(fromNode);
    Nodes_RebuildList();
    UpdateTabLabels();
    UpdateUnreadFlashTimer();
}

void UI_Unread_OnTabChanged(int newTabIndex)
{
    // Leaving tabs cancels their pending mark-read timers.
    if (newTabIndex != TAB_CHAT)
        KillTimer(g_App.hMain, TIMER_MARKREAD_CHAT);
    if (newTabIndex != TAB_DIRECT)
        KillTimer(g_App.hMain, TIMER_MARKREAD_DM);

    // When user navigates to a tab, start a 3s "mark read" countdown.
    if (newTabIndex == TAB_CHAT)
    {
        if (s_unreadChat > 0)
        {
            KillTimer(g_App.hMain, TIMER_MARKREAD_CHAT);
            SetTimer(g_App.hMain, TIMER_MARKREAD_CHAT, 3000, NULL);
        }
    }
    else if (newTabIndex == TAB_DIRECT)
    {
        DWORD sel = Direct_GetSelectedNode();
        if (sel && Direct_GetUnreadCount(sel) > 0)
        {
            s_pendingDmReadNode = sel;
            KillTimer(g_App.hMain, TIMER_MARKREAD_DM);
            SetTimer(g_App.hMain, TIMER_MARKREAD_DM, 3000, NULL);
        }
    }
    UpdateUnreadFlashTimer();

}

void UI_Unread_OnDirectSelectionChanged(DWORD selectedNode)
{
    // Only relevant while on Direct tab
    int curTab = g_App.hTab ? TabCtrl_GetCurSel(g_App.hTab) : -1;
    if (curTab != TAB_DIRECT) return;

    if (selectedNode && Direct_GetUnreadCount(selectedNode) > 0)
    {
        s_pendingDmReadNode = selectedNode;
        KillTimer(g_App.hMain, TIMER_MARKREAD_DM);
        SetTimer(g_App.hMain, TIMER_MARKREAD_DM, 3000, NULL);
    }
}

LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);

// Create a scaled font based on the current HDC font (percent = 100 keeps the size).
static HFONT CreateScaledFontFromDC(HDC hdc, int percent)
{
    if (!hdc || percent <= 0) return NULL;
    HFONT hBase = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
    LOGFONT lf;
    ZeroMemory(&lf, sizeof(lf));
    if (hBase && GetObject(hBase, sizeof(lf), &lf) == sizeof(lf))
    {
        lf.lfHeight = (lf.lfHeight * percent) / 100;
        if (lf.lfHeight == 0) lf.lfHeight = -10; // fallback
        return CreateFontIndirect(&lf);
    }
    return NULL;
}


//---------------------------------------------------------------------
//  WinMain
//---------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE, LPTSTR, int nCmdShow)
{
    ZeroMemory(&g_App, sizeof(g_App));
    g_App.hInst = hInstance;

    // Initialize configuration and serial
    Config_Init();
    Config_LoadApp();  // Load app settings from INI (device settings loaded from device)
    Meshtastic_Init();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // Load app icons (WinCE titlebar + taskbar button)
    // Use LoadImage with explicit sizes: some WinCE builds ignore LoadIcon sizes.
    int bigW = (int)GetSystemMetrics(SM_CXICON);
    int bigH = (int)GetSystemMetrics(SM_CYICON);
    if (bigW <= 0) bigW = 32;
    if (bigH <= 0) bigH = 32;

    // WinCE doesn't always expose SM_CXSMICON/SM_CYSMICON. Default to 16x16.
    int smallW = 16;
    int smallH = 16;
#ifdef SM_CXSMICON
    int sm = (int)GetSystemMetrics(SM_CXSMICON);
    if (sm > 0) smallW = sm;
#endif
#ifdef SM_CYSMICON
    int smy = (int)GetSystemMetrics(SM_CYSMICON);
    if (smy > 0) smallH = smy;
#endif

    HICON hIcoBig = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON,
                                    bigW, bigH, LR_DEFAULTCOLOR);
    HICON hIcoSmall = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON,
                                      smallW, smallH, LR_DEFAULTCOLOR);
    if (!hIcoBig)   hIcoBig = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    if (!hIcoSmall) hIcoSmall = hIcoBig;

    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = TEXT("MeshtasticJ720Main");
	wc.hIcon = hIcoBig;


    if (!RegisterClass(&wc))
        return 0;

    RECT rc;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);

    // Resizable window with min/max buttons, fits work area initially
    DWORD dwStyle = WS_VISIBLE |
                    WS_OVERLAPPED |
                    WS_CAPTION |
                    WS_SYSMENU |
                    WS_MINIMIZEBOX |
                    WS_MAXIMIZEBOX |
                    WS_THICKFRAME |
                    WS_CLIPCHILDREN;

    HWND hwnd = CreateWindow(
        wc.lpszClassName,
        TEXT("Meshtastic Client"),
        dwStyle,
        rc.left, rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
        return 0;

    // Set application icon (titlebar + taskbar button)
    // WinCE shells vary; set via WM_SETICON AND class icon.
#ifndef ICON_SMALL
#define ICON_SMALL 0
#endif
#ifndef ICON_BIG
#define ICON_BIG   1
#endif
#ifndef GCL_HICON
#define GCL_HICON (-14)
#endif
#ifndef GCL_HICONSM
#define GCL_HICONSM (-34)
#endif

    if (hIcoBig)
        SendMessage(hwnd, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)hIcoBig);
    if (hIcoSmall)
        SendMessage(hwnd, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)hIcoSmall);

    if (hIcoBig)
        SetClassLong(hwnd, GCL_HICON, (LONG)hIcoBig);
    if (hIcoSmall)
        SetClassLong(hwnd, GCL_HICONSM, (LONG)hIcoSmall);

    g_App.hMain = hwnd;

    // Init optional Jornada notification LED support
    LED_Init();

    // Start maximized (WinCE often launches with SW_SHOWNORMAL even if the window
    // already spans the work area, which leaves it "not maximized").
    (void)nCmdShow; // unused
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;

}

//---------------------------------------------------------------------
//  Main window procedure
//---------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Brushes for RX/TX activity indicators
    // RX active: 0xC4FF21 (green), TX active: 0x21EDFF (blue)
    static HBRUSH s_brRxActive = NULL;
    static HBRUSH s_brTxActive = NULL;
    static HBRUSH s_brInactive = NULL;
    static HFONT  s_hFontTopConnectSmall = NULL;
    static HFONT  s_hFontTopConnectTiny  = NULL;



    switch (msg)
    {
    case WM_CREATE:
        // Create brushes once (WinCE: prevents "cascading" redraw artifacts)
        if (!s_brRxActive) s_brRxActive = CreateSolidBrush(RGB(0xC4, 0xFF, 0x21));
        if (!s_brTxActive) s_brTxActive = CreateSolidBrush(RGB(0x21, 0xED, 0xFF));
        if (!s_brInactive) s_brInactive = CreateSolidBrush(RGB(210, 210, 210));
        CreateMainControls(hwnd);
        UpdateTabLabels();
        Settings_LoadToUI();

        // Disable message Send buttons until the serial node is connected.
        {
            const BOOL connected = Serial_IsOpen() ? TRUE : FALSE;
            if (g_App.hChatSend) EnableWindow(g_App.hChatSend, connected);
            if (g_App.hDMSend)   EnableWindow(g_App.hDMSend, connected);
        }

        // Set a timer for serial port polling (100ms interval)
        SetTimer(hwnd, 1, 100, NULL);
        // Blink pending outgoing ACK markers
        SetTimer(hwnd, TIMER_ACKBLINK, 500, NULL);
        return 0;

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_TIMER:
    {

        // Unread "mark as read" timers
        if (wParam == TIMER_MARKREAD_CHAT)
        {
            KillTimer(hwnd, TIMER_MARKREAD_CHAT);
            s_unreadChat = 0;
            UpdateTabLabels();
            UpdateUnreadFlashTimer();
            return 0;
        }
        if (wParam == TIMER_MARKREAD_DM)
        {
            KillTimer(hwnd, TIMER_MARKREAD_DM);
            if (s_pendingDmReadNode)
            {
                Direct_ClearUnread(s_pendingDmReadNode);
                s_pendingDmReadNode = 0;
                Nodes_RebuildList();
                UpdateTabLabels();
                UpdateUnreadFlashTimer();
            }
            return 0;
        }

        if (wParam == TIMER_UNREAD_FLASH)
        {
            s_unreadFlashOn = !s_unreadFlashOn;
            if (g_App.hDMNodeList) InvalidateRect(g_App.hDMNodeList, NULL, FALSE);
            return 0;
        }

        if (wParam == TIMER_ACKBLINK)
        {
            Meshtastic_TickAckBlink();
            Direct_TickAckBlink();
            return 0;
        }

        // Poll serial port for incoming data (timer id 1)
        if (wParam != 1)
            return 0;

        Serial_ProcessIncoming();

        // Enable/disable chat send buttons based on serial connection state.
        // (User can also press Enter in the input edit; WM_COMMAND handlers also validate.)
        {
            static BOOL s_lastConnected = FALSE;
            const BOOL connected = Serial_IsOpen() ? TRUE : FALSE;
            if (connected != s_lastConnected)
            {
                if (g_App.hChatSend) EnableWindow(g_App.hChatSend, connected);
                if (g_App.hDMSend)   EnableWindow(g_App.hDMSend, connected);
                s_lastConnected = connected;
            }
        }

        // Update RX/TX activity indicators
        // Note: no background erase to reduce flicker.
        if (g_App.hTopRx) InvalidateRect(g_App.hTopRx, NULL, FALSE);
        if (g_App.hTopTx) InvalidateRect(g_App.hTopTx, NULL, FALSE);

        // Keep Meshtastic serial connection alive (heartbeat)
        static int s_hbTicks = 0;
        // Power management: keep system awake while connected (optional)
        static int s_pmTicks = 0;
        if (Serial_IsOpen())
        {
            s_hbTicks++;
            if (s_hbTicks >= 50) // 50*100ms ~= 5s
            {
                s_hbTicks = 0;
                Meshtastic_SendHeartbeat();
            }

            // If enabled in app settings, periodically reset the system idle
            // timer so the Jornada doesn't suspend while the serial link is up.
            AppConfig* cfg = Config_GetApp();
            if (cfg && cfg->preventSleepWhileConnected)
            {
                s_pmTicks++;
                if (s_pmTicks >= 300) // 300*100ms ~= 30s
                {
                    s_pmTicks = 0;
                    SystemIdleTimerReset();
                }
            }
            else
            {
                s_pmTicks = 0;
            }
        }
        else
        {
            s_hbTicks = 0;
            s_pmTicks = 0;
        }
        return 0;
    }

    case WM_CTLCOLOREDIT:
    {
        // Flash history background on new messages.
        static HBRUSH s_brFlash = NULL;
        if (!s_brFlash) s_brFlash = CreateSolidBrush(HISTORY_FLASH_COLOR);

        HWND hCtl = (HWND)lParam;
        HDC hdc = (HDC)wParam;
        DWORD now = GetTickCount();

        if (hCtl == g_App.hChatHistory && now < s_chatFlashUntil)
        {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, HISTORY_FLASH_COLOR);
            return (LRESULT)s_brFlash;
        }
        if (hCtl == g_App.hDMHistory && now < s_dmFlashUntil)
        {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, HISTORY_FLASH_COLOR);
            return (LRESULT)s_brFlash;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis && dis->CtlID == IDC_TOP_CONNECT)
        {
            const bool connected = Serial_IsOpen();

            // Background
            HBRUSH br = connected ? CreateSolidBrush(RGB(0x00, 0xE3, 0x00))
                                  : GetSysColorBrush(COLOR_BTNFACE);
            FillRect(dis->hDC, &dis->rcItem, br);
            if (connected) DeleteObject(br);

            // Text
            TCHAR text[64];
            GetWindowText(dis->hwndItem, text, 64);
            SetBkMode(dis->hDC, TRANSPARENT);
            // Respect the system theme for the *disconnected* state (this is a normal button).
            // When connected, we use a bright green background, so keep black for legibility.
            SetTextColor(dis->hDC, connected ? RGB(0, 0, 0) : GetSysColor(COLOR_BTNTEXT));
            // Choose a smaller font in the connected state so the label fits the button.
            HFONT hOldFont = NULL;
            HFONT hUseFont = NULL;
            if (connected)
            {
                // Create/cached smaller fonts based on the current DC font.
                if (!s_hFontTopConnectSmall) s_hFontTopConnectSmall = CreateScaledFontFromDC(dis->hDC, 90);
                if (!s_hFontTopConnectTiny)  s_hFontTopConnectTiny  = CreateScaledFontFromDC(dis->hDC, 75);
                hUseFont = s_hFontTopConnectSmall ? s_hFontTopConnectSmall : NULL;

                // If still too wide, fall back to a tinier font.
                if (hUseFont)
                {
                    SIZE sz;
                    ZeroMemory(&sz, sizeof(sz));
                    hOldFont = (HFONT)SelectObject(dis->hDC, hUseFont);
                    GetTextExtentPoint32(dis->hDC, text, lstrlen(text), &sz);
                    int avail = (dis->rcItem.right - dis->rcItem.left) - 6;
                    if (sz.cx > avail && s_hFontTopConnectTiny)
                    {
                        SelectObject(dis->hDC, hOldFont);
                        hUseFont = s_hFontTopConnectTiny;
                        hOldFont = (HFONT)SelectObject(dis->hDC, hUseFont);
                    }
                }
                else if (s_hFontTopConnectTiny)
                {
                    hOldFont = (HFONT)SelectObject(dis->hDC, s_hFontTopConnectTiny);
                }
            }

            DrawText(dis->hDC, text, -1, (LPRECT)&dis->rcItem,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if (hOldFont) SelectObject(dis->hDC, hOldFont);

            // Button edge
            DrawEdge(dis->hDC, (LPRECT)&dis->rcItem, EDGE_RAISED, BF_RECT);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    {
        HWND hCtl = (HWND)lParam;
        HDC hdc = (HDC)wParam;
        if (hCtl == g_App.hTopRx || hCtl == g_App.hTopTx)
        {
            DWORD now = GetTickCount();
            DWORD last = (hCtl == g_App.hTopRx) ? Serial_GetLastRxTick() : Serial_GetLastTxTick();
            // "Active" if we saw traffic in the last ~300ms
            bool active = (last != 0 && (now - last) < 300);

            // Opaque background + solid brush is required; transparent backgrounds
            // cause the classic WinCE "cascading" artifacts when the control is invalidated.
            HBRUSH br = s_brInactive;
            if (active)
                br = (hCtl == g_App.hTopRx) ? s_brRxActive : s_brTxActive;

            SetBkMode(hdc, OPAQUE);
            SetTextColor(hdc, RGB(0, 0, 0));
            // Best-effort: set BkColor to match the brush (not strictly required).
            if (br == s_brRxActive) SetBkColor(hdc, RGB(0xC4, 0xFF, 0x21));
            else if (br == s_brTxActive) SetBkColor(hdc, RGB(0x21, 0xED, 0xFF));
            else SetBkColor(hdc, RGB(210, 210, 210));
            return (LRESULT)br;
        }
        break;
    }
    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->hwndFrom == g_App.hTab &&
            ((LPNMHDR)lParam)->code == TCN_SELCHANGE)
        {
            int index = TabCtrl_GetCurSel(g_App.hTab);
            ShowTabPage(index);
            UI_Unread_OnTabChanged(index);
        }
        else if (((LPNMHDR)lParam)->hwndFrom == g_App.hNodesList &&
                 ((LPNMHDR)lParam)->code == LVN_COLUMNCLICK)
        {
            int col = ((NMLISTVIEW*)lParam)->iSubItem;
            Nodes_OnColumnClick(col);
            return 0;
        }
        else if (((LPNMHDR)lParam)->hwndFrom == g_App.hDMNodeList &&
                 ((LPNMHDR)lParam)->code == LVN_ITEMCHANGED)
        {
            // Update DM history when selection changes
            Direct_OnSelectionChanged();
        }
        else if (((LPNMHDR)lParam)->hwndFrom == g_App.hDMNodeList &&
                 ((LPNMHDR)lParam)->code == NM_CUSTOMDRAW)
        {
            // Keep selection visually highlighted even when the listview doesn't have focus
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
            {
                // If selected, paint with normal highlight colors
                int iItem = (int)cd->nmcd.dwItemSpec;
                UINT state = ListView_GetItemState(g_App.hDMNodeList, iItem, LVIS_SELECTED);
                if (state & LVIS_SELECTED)
                {
                    cd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                    cd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
                }
                else
                {
                    // Unread -> light "attention" background
                    LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
                    lvi.iItem = iItem;
                    lvi.mask = LVIF_PARAM;
                    if (ListView_GetItem(g_App.hDMNodeList, &lvi))
                    {
                        DWORD nodeNum = (DWORD)lvi.lParam;
                        int unread = Direct_GetUnreadCount(nodeNum);
                        if (unread > 0 && s_unreadFlashOn)
                        {
                            cd->clrTextBk = UNREAD_COLOR_ON;
                            cd->clrText = UNREAD_TEXT_COLOR;
                        }
                    }
                }
                return CDRF_DODEFAULT;
            }
            }
            return CDRF_DODEFAULT;
        }
        break;

    case WM_COMMAND:
    {
        WORD id   = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        // Settings: timestamp display mode changed -> immediately re-render existing histories.
        // (Do this on selection change, not only after sending a new message.)
        if (code == CBN_SELCHANGE && (HWND)lParam == g_App.hSetChatTsMode)
        {
            Settings_SaveFromUI();
            return 0;
        }

        if (id == IDC_CHAT_SEND && code == BN_CLICKED)
        {
            if (!Serial_IsOpen())
            {
                MessageBox(hwnd,
                           TEXT("Node not connected.\r\n\r\nConnect over serial to send messages."),
                           TEXT("Meshtastic"),
                           MB_OK | MB_ICONEXCLAMATION);
                return 0;
            }

            TCHAR buf[256];
            GetWindowText(g_App.hChatInput, buf, 256);
            if (buf[0])
            {
                UI_HistoryPushChat(buf);
                // Send via Meshtastic (request ACKs) and render an initial " - " marker.
                {
                    char text[512];
#ifdef UNICODE
                    WideCharToMultiByte(CP_ACP, 0, buf, -1, text, sizeof(text), NULL, NULL);
#else
                    strncpy(text, buf, sizeof(text)-1);
#endif
                    DWORD pktId = 0;
                    bool ok = Meshtastic_SendTextEx(0, text, &pktId); // 0 = broadcast

                    // Always show the outgoing message locally; if TX fails, marker remains " - "
                    int markerPos = MsgStore_AppendChatWithMarker(g_App.hChatHistory, TEXT("[Me]"), buf, TEXT("  - "));
                    if (ok && pktId && markerPos >= 0)
                        Meshtastic_RegisterChatOutgoing(pktId, markerPos);
                }
                
                SetWindowText(g_App.hChatInput, TEXT(""));
            }
            return 0;
        }
        else if (id == IDC_DM_SEND && code == BN_CLICKED)
        {
            TCHAR buf[256];
            GetWindowText(g_App.hDMInput, buf, 256);
            if (buf[0])
            {
                if (!Serial_IsOpen())
                {
                    MessageBox(hwnd,
                               TEXT("Node not connected.\r\n\r\nConnect over serial to send messages."),
                               TEXT("Meshtastic"),
                               MB_OK | MB_ICONEXCLAMATION);
                    return 0;
                }

                DWORD nodeNum = Direct_GetSelectedNode();

                if (nodeNum == 0)
                {
                    // No recipient selected -> do nothing (keep text)
                    Direct_OnSelectionChanged();
                    return 0;
                }

                UI_HistoryPushDM(buf);

                // Send via Meshtastic
                if (nodeNum)
                {
                    char text[512];
#ifdef UNICODE
                    WideCharToMultiByte(CP_ACP, 0, buf, -1, text, sizeof(text), NULL, NULL);
#else
                    strncpy(text, buf, sizeof(text)-1);
#endif

                    DWORD pktId = 0;
                    Meshtastic_SendTextEx(nodeNum, text, &pktId);

                    // Update per-node DM log + UI with initial " - " marker and ACK tracking
                    Direct_OnOutgoingTextWithAck(nodeNum, buf, pktId);
                }
                
                SetWindowText(g_App.hDMInput, TEXT(""));
            }
            return 0;
        }
        else if (id == IDC_NODES_SEARCH && code == EN_CHANGE)
        {
            Nodes_OnSearchChange();
            return 0;
        }
        else if (id == IDC_NODES_CLEAR && code == BN_CLICKED)
        {
            if (g_App.hNodesSearch)
                SetWindowText(g_App.hNodesSearch, TEXT(""));
            Nodes_OnSearchChange();
            SetFocus(g_App.hNodesSearch);
            return 0;
        }
        else if (id == IDC_SET_BROWSE && code == BN_CLICKED)
        {
            // Simple folder browse - for now just use a predefined list
            // Windows CE doesn't have SHBrowseForFolder in all versions
            TCHAR newPath[MAX_PATH];
            GetWindowText(g_App.hSetStoragePath, newPath, MAX_PATH);
            
            // Show dialog with common paths
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, 1, TEXT("\\Storage Card\\Meshtastic"));
            AppendMenu(hMenu, MF_STRING, 2, TEXT("\\My Documents\\Meshtastic"));
            AppendMenu(hMenu, MF_STRING, 3, TEXT("\\Temp\\Meshtastic"));
            AppendMenu(hMenu, MF_STRING, 4, TEXT("\\Meshtastic"));
            
            RECT rc;
            GetWindowRect(g_App.hSetBrowse, &rc);
            
            int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                                    rc.left, rc.bottom, 0, hwnd, NULL);
            
            if (cmd > 0)
            {
                const TCHAR* paths[] = {
                    TEXT("\\Storage Card\\Meshtastic"),
                    TEXT("\\My Documents\\Meshtastic"),
                    TEXT("\\Temp\\Meshtastic"),
                    TEXT("\\Meshtastic")
                };
                SetWindowText(g_App.hSetStoragePath, paths[cmd-1]);
            }
            
            DestroyMenu(hMenu);
            return 0;
        }
        else if (id == IDC_SET_CONNECT && code == BN_CLICKED)
        {
            Settings_OnConnect();
            return 0;
        }
        else if (id == IDC_TOP_CONNECT && code == BN_CLICKED)
        {
            Settings_OnConnect();
            return 0;
        }
        else if (id == IDC_SET_APPLY && code == BN_CLICKED)
        {
            Settings_OnApply();
            return 0;
        }
        else if (id == IDC_SET_SAVE && code == BN_CLICKED)
        {
            Settings_OnSave();
            return 0;
        }
        else if (id == IDC_SET_GET_SETTINGS && code == BN_CLICKED)
        {
            Settings_OnGetSettings();
            return 0;
        }
        else if (id == IDC_SET_ABOUT && code == BN_CLICKED)
        {
            Settings_OnAbout();
            return 0;
        }
        else if (id == IDC_SERIAL_SEND && code == BN_CLICKED)
        {
            TCHAR buf[256];
            GetWindowText(g_App.hSerialInput, buf, 256);
            if (buf[0])
            {
                UI_HistoryPushSerial(buf);
                char cmd[512];
#ifdef UNICODE
                WideCharToMultiByte(CP_ACP, 0, buf, -1, cmd, sizeof(cmd), NULL, NULL);
#else
                strncpy(cmd, buf, sizeof(cmd)-1);
#endif
                // Serial Monitor send: send raw ASCII line + CRLF
                char out[520];
                int l = strlen(cmd);
                if (l > 510) l = 510;
                memcpy(out, cmd, l);
                out[l] = '\r';
                out[l+1] = '\n';
                if (!Serial_SendBytes(out, l + 2))
                {
                    // If send fails, log error
                    AppendSerialLog("[ERROR: Not connected]", false);
                }
                
                SetWindowText(g_App.hSerialInput, TEXT(""));
            }
            return 0;
        }
        else if (id == IDC_SERIAL_CLEAR && code == BN_CLICKED)
        {
            SetWindowText(g_App.hSerialLog, TEXT(""));
            return 0;
        }

        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
		if (s_brRxActive) { DeleteObject(s_brRxActive); s_brRxActive = NULL; }
		if (s_brTxActive) { DeleteObject(s_brTxActive); s_brTxActive = NULL; }
		if (s_brInactive) { DeleteObject(s_brInactive); s_brInactive = NULL; }
        if (s_hFontTopConnectSmall) { DeleteObject(s_hFontTopConnectSmall); s_hFontTopConnectSmall = NULL; }
        if (s_hFontTopConnectTiny)  { DeleteObject(s_hFontTopConnectTiny);  s_hFontTopConnectTiny  = NULL; }
        Serial_Shutdown();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
//----------------------------------------------------
// Jornada/WinCE notification LED (NLED)
//----------------------------------------------------
// Jornada 720/728 have a front button LED (used by the OS for alarms/notifications).
// We try to drive it via the standard WinCE NLED API in coredll.dll.
// If unavailable on a target device, these functions safely no-op.
//
// NLED constants (commonly: COUNT=0, SUPPORTS=1, SETTINGS=2)
#define NLED_COUNT_INFO_ID    0
#define NLED_SUPPORTS_INFO_ID 1
#define NLED_SETTINGS_INFO_ID 2

typedef struct _NLED_COUNT_INFO
{
    UINT cLeds;
} NLED_COUNT_INFO;

typedef struct _NLED_SETTINGS_INFO
{
    UINT LedNum;
    INT  OffOnBlink;      // 0=off, 1=on, 2=blink
    LONG TotalCycleTime;  // usec
    LONG OnTime;          // usec
    LONG OffTime;         // usec
    INT  MetaCycleOn;
    INT  MetaCycleOff;
} NLED_SETTINGS_INFO;

typedef BOOL (WINAPI *PFN_NLedGetDeviceInfo)(INT nInfoId, PVOID pOutput);
typedef BOOL (WINAPI *PFN_NLedSetDevice)(UINT nDeviceId, PVOID pInput);

static PFN_NLedGetDeviceInfo s_pNLedGetDeviceInfo = NULL;
static PFN_NLedSetDevice     s_pNLedSetDevice     = NULL;
static BOOL                  s_ledAvail           = FALSE;
static int                   s_ledNum             = 0;

static BOOL s_ledEnableNew    = TRUE;
static BOOL s_ledEnableUnread = TRUE;

static volatile BOOL s_ledHasUnread = FALSE;
static volatile BOOL s_ledPulseActive = FALSE;
static HANDLE s_ledPulseThread = NULL;
static HANDLE s_ledUnreadThread = NULL;
static HANDLE s_ledStopEvent = NULL;
static CRITICAL_SECTION s_ledCs;
static BOOL s_ledCsInit = FALSE;

static BOOL Led_EnsureApi()
{
    if (s_pNLedGetDeviceInfo && s_pNLedSetDevice) return TRUE;

	HMODULE hCore = LoadLibrary(TEXT("coredll.dll"));
    if (!hCore) return FALSE;

	// NOTE: On many WinCE SDKs GetProcAddress maps to GetProcAddressW.
	// Use TEXT() so this compiles in both Unicode and non-Unicode builds.
	s_pNLedGetDeviceInfo = (PFN_NLedGetDeviceInfo)GetProcAddress(hCore, TEXT("NLedGetDeviceInfo"));
	s_pNLedSetDevice     = (PFN_NLedSetDevice)GetProcAddress(hCore, TEXT("NLedSetDevice"));

    if (!s_pNLedGetDeviceInfo || !s_pNLedSetDevice)
        return FALSE;

    return TRUE;
}

static void Led_SetOff()
{
    if (!s_ledAvail || !s_pNLedSetDevice) return;

    NLED_SETTINGS_INFO s; ZeroMemory(&s, sizeof(s));
    s.LedNum = (UINT)s_ledNum;
    s.OffOnBlink = 0; // off
    s_pNLedSetDevice(NLED_SETTINGS_INFO_ID, &s);
}

static void Led_SetOn()
{
    if (!s_ledAvail || !s_pNLedSetDevice) return;

    NLED_SETTINGS_INFO s; ZeroMemory(&s, sizeof(s));
    s.LedNum = (UINT)s_ledNum;
    s.OffOnBlink = 1; // on
    s_pNLedSetDevice(NLED_SETTINGS_INFO_ID, &s);
}

static void Led_SetSlowBlink_3s()
{
    if (!s_ledAvail || !s_pNLedSetDevice) return;

    // Short blink every 3 seconds (usec fields)
    NLED_SETTINGS_INFO s; ZeroMemory(&s, sizeof(s));
    s.LedNum = (UINT)s_ledNum;
    s.OffOnBlink = 2; // blink
    s.TotalCycleTime = 3000000;   // 3.0s
    s.OnTime        = 80000;      // 80ms pulse
    s.OffTime       = 2920000;    // remainder
    s.MetaCycleOn   = 0;
    s.MetaCycleOff  = 0;
    s_pNLedSetDevice(NLED_SETTINGS_INFO_ID, &s);
}

static void Led_ApplyBaseState()
{
    // Called when not in a "pulse new message" animation.
    if (!s_ledAvail) return;

    // Some Jornada/WinCE builds expose the NLED API but ignore blink timing fields.
    // To keep behavior consistent across devices, we keep the LED off here and
    // generate the "unread" blink using a lightweight software pulse thread.
    Led_SetOff();
}

static DWORD WINAPI Led_UnreadThread(LPVOID)
{
    // Short pulse every ~3 seconds while there are unread messages.
    // This avoids relying on NLED blink timing support, which is inconsistent.
    for (;;)
    {
        if (s_ledStopEvent && WaitForSingleObject(s_ledStopEvent, 0) == WAIT_OBJECT_0)
            break;

        BOOL doPulse = FALSE;
        if (s_ledAvail && s_ledCsInit)
        {
            EnterCriticalSection(&s_ledCs);
            BOOL pulsing = s_ledPulseActive;
            LeaveCriticalSection(&s_ledCs);

            if (!pulsing && s_ledEnableUnread && s_ledHasUnread)
                doPulse = TRUE;
        }

        if (doPulse)
        {
            Led_SetOn();
            Sleep(80);
            Led_SetOff();

            // Wait out the remainder of the 3s period (or stop early).
            if (s_ledStopEvent && WaitForSingleObject(s_ledStopEvent, 2920) == WAIT_OBJECT_0)
                break;
        }
        else
        {
            // Idle polling (or stop early).
            if (s_ledStopEvent && WaitForSingleObject(s_ledStopEvent, 200) == WAIT_OBJECT_0)
                break;
        }
    }
    return 0;
}

static DWORD WINAPI Led_PulseThread(LPVOID)
{
    // 3 fast short blinks whenever a new message arrives.
    // If unread mode is enabled and currently active, we restore slow-blink after the pulses.
    for (int i = 0; i < 3; ++i)
    {
        Led_SetOn();
        Sleep(70);
        Led_SetOff();
        Sleep(90);
    }

    EnterCriticalSection(&s_ledCs);
    s_ledPulseActive = FALSE;
    LeaveCriticalSection(&s_ledCs);

    Led_ApplyBaseState();
    return 0;
}

void LED_Init()
{
    if (!Led_EnsureApi())
        return;

    NLED_COUNT_INFO ci; ZeroMemory(&ci, sizeof(ci));
    if (!s_pNLedGetDeviceInfo(NLED_COUNT_INFO_ID, &ci))
        return;

    if ((int)ci.cLeds <= 0)
        return;

    // We don't know which LED index is mapped to the front button LED.
    // Try LED 0 first (most devices have a single NLED), otherwise fall back to the first usable.
    s_ledNum = 0;
    s_ledAvail = TRUE;

    if (!s_ledCsInit)
    {
        InitializeCriticalSection(&s_ledCs);
        s_ledCsInit = TRUE;
    }

    if (!s_ledStopEvent)
        s_ledStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!s_ledUnreadThread && s_ledStopEvent)
    {
        DWORD tid = 0;
        s_ledUnreadThread = CreateThread(NULL, 0, Led_UnreadThread, NULL, 0, &tid);
        // If thread creation fails, we still keep NEW-message pulses working.
    }

    // Start from "off" so we don't interfere until user enables options.
    Led_SetOff();

    LED_UpdateFromConfig();
}

void LED_Shutdown()
{
    if (s_ledStopEvent)
        SetEvent(s_ledStopEvent);

    if (s_ledUnreadThread)
    {
        WaitForSingleObject(s_ledUnreadThread, 1500);
        CloseHandle(s_ledUnreadThread);
        s_ledUnreadThread = NULL;
    }

    if (s_ledCsInit)
    {
        // best-effort stop
        EnterCriticalSection(&s_ledCs);
        s_ledPulseActive = FALSE;
        LeaveCriticalSection(&s_ledCs);
    }

    Led_SetOff();

    if (s_ledPulseThread)
    {
        CloseHandle(s_ledPulseThread);
        s_ledPulseThread = NULL;
    }

    if (s_ledStopEvent)
    {
        CloseHandle(s_ledStopEvent);
        s_ledStopEvent = NULL;
    }

    if (s_ledCsInit)
    {
        DeleteCriticalSection(&s_ledCs);
        s_ledCsInit = FALSE;
    }
}

void LED_UpdateFromConfig()
{
    AppConfig* cfg = Config_GetApp();
    if (!cfg) return;

    s_ledEnableNew    = (cfg->ledNewMessage != 0);
    s_ledEnableUnread = (cfg->ledUnread != 0);

    // Re-apply base state unless a pulse is mid-flight.
    if (!s_ledCsInit || !s_ledAvail) return;
    EnterCriticalSection(&s_ledCs);
    BOOL pulsing = s_ledPulseActive;
    LeaveCriticalSection(&s_ledCs);

    if (!pulsing)
        Led_ApplyBaseState();
}

void LED_SetUnreadActive(bool hasUnread)
{
    s_ledHasUnread = hasUnread ? TRUE : FALSE;

    if (!s_ledCsInit || !s_ledAvail) return;

    EnterCriticalSection(&s_ledCs);
    BOOL pulsing = s_ledPulseActive;
    LeaveCriticalSection(&s_ledCs);

    if (!pulsing)
        Led_ApplyBaseState();
}

void LED_OnNewMessage()
{
    if (!s_ledAvail || !s_ledCsInit) return;
    if (!s_ledEnableNew) return;

    EnterCriticalSection(&s_ledCs);
    if (s_ledPulseActive)
    {
        // Already pulsing; ignore (base state will be restored by current thread).
        LeaveCriticalSection(&s_ledCs);
        return;
    }
    s_ledPulseActive = TRUE;
    LeaveCriticalSection(&s_ledCs);

    // Kill base blink while we pulse
    Led_SetOff();

    DWORD tid = 0;
    s_ledPulseThread = CreateThread(NULL, 0, Led_PulseThread, NULL, 0, &tid);
    if (!s_ledPulseThread)
    {
        EnterCriticalSection(&s_ledCs);
        s_ledPulseActive = FALSE;
        LeaveCriticalSection(&s_ledCs);
        Led_ApplyBaseState();
    }
}