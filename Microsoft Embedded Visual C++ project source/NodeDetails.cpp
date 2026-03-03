#include "stdafx.h"
#include "app.h"
#include "nodes.h"
#include "NodeDetails.h"

static HWND     g_hDetailsWnd  = NULL;
static NodeInfo g_NodeSnapshot;
static HWND     g_hLastList    = NULL;
static DWORD    g_LastSelNode  = 0;
static WNDPROC  g_OldDetailsEditProc = NULL;
static bool     g_CloseForDM = false;

static LRESULT CALLBACK DetailsEdit_SubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        HWND hP = GetParent(hwnd);
        if (hP) PostMessage(hP, WM_CLOSE, 0, 0);
        return 0;
    }
    return CallWindowProc(g_OldDetailsEditProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK NodeDetails_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_CREATE:
    {
        g_CloseForDM = false;
        // Direct message button
        CreateWindowEx(
            0, TEXT("BUTTON"), TEXT("Direct Message"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0,
            hwnd, (HMENU)2, g_App.hInst, NULL);

        // Create a multiline read-only edit control to show all text
        HWND hTxt = CreateWindowEx(
            WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0,
            hwnd, (HMENU)1, g_App.hInst, NULL);

        // Subclass edit for ESC-to-close
        g_OldDetailsEditProc = (WNDPROC)SetWindowLong(hTxt, GWL_WNDPROC, (LONG)DetailsEdit_SubclassProc);

        // Force initial layout so the button never overlaps the text
        RECT rc;
        GetClientRect(hwnd, &rc);
        SendMessage(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        return 0;
    }

    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);

        // Keep the text control full height; the DM button overlays it.
        HWND hTxt = GetDlgItem(hwnd, 1);
        if (hTxt)
            MoveWindow(hTxt, 0, 0, w, h, TRUE);

        // Small DM button, positioned on the right but shifted left past the edit scrollbar.
        // Compute width so the caption fits on WinCE.
        int btnH = 22;
        int btnW = 96;
        int margin = 4;
        int sb = GetSystemMetrics(SM_CXVSCROLL);
        if (sb <= 0) sb = 16;

        // Measure caption width with the current dialog font
        {
            TCHAR cap[64]; cap[0] = 0;
            GetDlgItemText(hwnd, 2, cap, 63);
            if (cap[0])
            {
                HDC hdc = GetDC(hwnd);
                if (hdc)
                {
                    HFONT hFont = (HFONT)SendMessage(GetDlgItem(hwnd, 2), WM_GETFONT, 0, 0);
                    HFONT hOld = NULL;
                    if (hFont) hOld = (HFONT)SelectObject(hdc, hFont);
                    SIZE sz; sz.cx = 0; sz.cy = 0;
                    GetTextExtentPoint32(hdc, cap, _tcslen(cap), &sz);
                    if (hFont && hOld) SelectObject(hdc, hOld);
                    ReleaseDC(hwnd, hdc);
                    // Add padding + minimum width
                    int need = sz.cx + 18;
                    if (need > btnW) btnW = need;
                }
            }
        }

        if (btnW > (w - margin * 2)) btnW = (w - margin * 2);
        int x = w - btnW - margin - (sb + 6);
        if (x < margin) x = margin;

        HWND hBtn = GetDlgItem(hwnd, 2);
        if (hBtn)
            MoveWindow(hBtn, x, margin, btnW, btnH, TRUE);

        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 2 && HIWORD(wParam) == BN_CLICKED)
        {
            // Add this node to Direct tab conversation list and jump there
            g_CloseForDM = true;
            UI_GotoDirectConversation(g_NodeSnapshot.nodeNum);
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_DESTROY:
        // When details window closes, restore focus to last node list & selection.
        // If we just launched a DM, don't steal focus back.
        if (!g_CloseForDM && g_hLastList)
        {
            // Support both ListView and ListBox restore
            TCHAR cls[32]; cls[0] = 0;
            GetClassName(g_hLastList, cls, 31);
            if (_tcsicmp(cls, WC_LISTVIEW) == 0)
            {
                if (g_LastSelNode)
                {
                    int count = ListView_GetItemCount(g_hLastList);
                    for (int i = 0; i < count; ++i)
                    {
                        LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
                        lvi.iItem = i; lvi.mask = LVIF_PARAM;
                        if (ListView_GetItem(g_hLastList, &lvi) && (DWORD)lvi.lParam == g_LastSelNode)
                        {
                            ListView_SetItemState(g_hLastList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                            ListView_EnsureVisible(g_hLastList, i, FALSE);
                            break;
                        }
                    }
                }
            }
            else
            {
                // Best-effort for listboxes
                int count = (int)SendMessage(g_hLastList, LB_GETCOUNT, 0, 0);
                for (int i = 0; i < count; ++i)
                {
                    DWORD v = (DWORD)SendMessage(g_hLastList, LB_GETITEMDATA, i, 0);
                    if (v == g_LastSelNode) { SendMessage(g_hLastList, LB_SETCURSEL, i, 0); break; }
                }
            }
            SetFocus(g_hLastList);
        }
        g_hDetailsWnd = NULL;
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Helper: fill the edit control with the current snapshot
static void NodeDetails_FillText()
{
    if (!g_hDetailsWnd)
        return;

    TCHAR      buf[1024];
    SYSTEMTIME st = g_NodeSnapshot.lastHeard;

    TCHAR dispShort[16];
    TCHAR dispLong[64];
    Nodes_GetDisplayShortName(&g_NodeSnapshot, dispShort, 16);
    Nodes_GetDisplayLongName(&g_NodeSnapshot, dispLong, 64);
    _sntprintf(buf, 1024,
               TEXT("Node ID: %s\r\n")
               TEXT("Short: %s\r\n")
               TEXT("Long: %s\r\n")
               TEXT("Battery: %d%%\r\n")
               TEXT("Online: %s\r\n")
               TEXT("Last Heard: %02d:%02d  %02d/%02d/%04d\r\n")
               TEXT("\r\n"),
               g_NodeSnapshot.nodeId[0] ? g_NodeSnapshot.nodeId : TEXT("(unknown)"),
               dispShort[0] ? dispShort : TEXT("(none)"),
               dispLong[0] ? dispLong : TEXT("(none)"),
               g_NodeSnapshot.batteryPct,
               g_NodeSnapshot.online ? TEXT("Yes") : TEXT("No"),
               st.wHour, st.wMinute, st.wDay, st.wMonth, st.wYear);

    // Append coordinates if available
    if (g_NodeSnapshot.hasPosition)
    {
        TCHAR coord[256];
        _sntprintf(coord, 256,
                   TEXT("Latitude:  %.6f\r\n")
                   TEXT("Longitude: %.6f\r\n"),
                   g_NodeSnapshot.latitude,
                   g_NodeSnapshot.longitude);
        _tcscat(buf, coord);
    }
    else
    {
        _tcscat(buf, TEXT("Location: (no position received)\r\n"));
    }

    HWND hTxt = GetDlgItem(g_hDetailsWnd, 1);
    if (hTxt)
    {
        // Ensure it is sized to client rect the first time as well
        RECT rc;
        GetClientRect(g_hDetailsWnd, &rc);
        MoveWindow(hTxt, 0, 0,
                   rc.right - rc.left,
                   rc.bottom - rc.top,
                   TRUE);

        SetWindowText(hTxt, buf);
    }
}

//---------------------------------------------------------------------
//  Public: Show details window for a node (from a given listbox)
//---------------------------------------------------------------------
void NodeDetails_Show(HWND hwndOwner, HWND hwndList, NodeInfo* pNode)
{
    if (!pNode)
        return;

    // Remember the list and its current selection so we can restore focus later
    g_hLastList = hwndList;
    g_LastSelNode = 0;
    if (hwndList)
    {
        TCHAR cls[32]; cls[0] = 0;
        GetClassName(hwndList, cls, 31);
        if (_tcsicmp(cls, WC_LISTVIEW) == 0)
        {
            int sel = ListView_GetNextItem(hwndList, -1, LVNI_SELECTED);
            if (sel >= 0)
            {
                LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
                lvi.iItem = sel; lvi.mask = LVIF_PARAM;
                if (ListView_GetItem(hwndList, &lvi))
                    g_LastSelNode = (DWORD)lvi.lParam;
            }
        }
        else
        {
            int sel = (int)SendMessage(hwndList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR)
                g_LastSelNode = (DWORD)SendMessage(hwndList, LB_GETITEMDATA, sel, 0);
        }
    }

    // Copy values so window shows a consistent snapshot
    memcpy(&g_NodeSnapshot, pNode, sizeof(NodeInfo));

    if (!g_hDetailsWnd)
    {
        // Register class (idempotent; if already exists, this is harmless)
        WNDCLASS wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = NodeDetails_WndProc;
        wc.hInstance     = g_App.hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = TEXT("NodeDetailsWin");

        RegisterClass(&wc);

        RECT rc;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);

        int width  = (rc.right - rc.left) * 70 / 100;
        int height = (rc.bottom - rc.top) * 60 / 100;
        int x = rc.left + (rc.right - rc.left - width) / 2;
        int y = rc.top  + (rc.bottom - rc.top - height) / 2;

        g_hDetailsWnd = CreateWindowEx(
            0,
            TEXT("NodeDetailsWin"),
            TEXT("Node Details"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
            WS_MINIMIZEBOX | WS_VISIBLE,
            x, y, width, height,
            hwndOwner, NULL, g_App.hInst, NULL);

        if (!g_hDetailsWnd)
            return;
    }
    else
    {
        ShowWindow(g_hDetailsWnd, SW_SHOW);
    }

    // Bring to front
    SetForegroundWindow(g_hDetailsWnd);
    SetWindowPos(g_hDetailsWnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    // Fill text and ensure the child edit is correctly sized immediately
    NodeDetails_FillText();
}