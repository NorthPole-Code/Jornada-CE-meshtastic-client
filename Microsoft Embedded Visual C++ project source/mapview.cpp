// mapview.cpp
// Windows CE 3 / eVC++ compatible (No C++11 features)

#include "stdafx.h"
#include <windows.h>
#include <tchar.h>
#include <commdlg.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h> 

#include "app.h"
#include "nodes.h"
#include "config.h"
#include "NodeDetails.h"
#include "mapview.h"

#ifndef LR_LOADFROMFILE
#define LR_LOADFROMFILE 0x0010
#endif

#define IDC_LOADMAP  1001
#define IDC_ZOOMIN   1002
#define IDC_ZOOMOUT  1003
#define IDC_ZOOMFIT  1004
#define IDC_INVERTCL 1005

#ifndef NOTSRCCOPY
#define NOTSRCCOPY 0x00330008
#endif

// ---- Struct Definitions ----

struct WORLD
{
    double A, D, B, E, C, F;
    bool valid;
};

// ---- Global File State ----

static HBITMAP s_hBmp       = NULL;
static int     s_bmpW       = 0;
static int     s_bmpH       = 0;
static WORLD   s_world;         
static bool    s_triedLoad  = false;

static double  s_zoom       = 1.0; 
static int     s_scrollX    = 0; 
static int     s_scrollY    = 0; 

static bool    s_isDragging = false;
static POINT   s_lastMouse  = {0};
static POINT   s_downMouse  = {0};

static HWND    s_hBtnLoad   = NULL;
static HWND    s_hBtnIn     = NULL;
static HWND    s_hBtnOut    = NULL;
static HWND    s_hBtnFit    = NULL;
static HWND    s_hChkInvert = NULL;

static TCHAR   s_userBmpPath[MAX_PATH] = TEXT("");
static TCHAR   s_userWldPath[MAX_PATH] = TEXT("");
static TCHAR   s_lastLoadMsg[512]      = TEXT("");

// ---- Helper Functions ----

static int MapToScreenX(int mapX) 
{ 
    return (int)((mapX - s_scrollX) * s_zoom); 
}

static int MapToScreenY(int mapY) 
{ 
    return (int)((mapY - s_scrollY) * s_zoom); 
}

// Map coordinate -> Map Pixel
static int CoordToMapX(double lon)
{
    if (!s_world.valid) return 0;
    return (int)((lon - s_world.C) / s_world.A);
}

static int CoordToMapY(double lat)
{
    if (!s_world.valid) return 0;
    return (int)((lat - s_world.F) / s_world.E);
}

// Centers the map so that 'mapX/Y' is in the middle of the 'rc' viewport
static void CenterMapAtPoint(int mapX, int mapY, RECT* rc) 
{
    int screenW = rc->right - rc->left;
    int screenH = rc->bottom - rc->top;
    
    // Calculate the top-left map pixel that would put mapX in the center
    s_scrollX = mapX - (int)((screenW / 2) / s_zoom);
    s_scrollY = mapY - (int)((screenH / 2) / s_zoom);
}

// Centers the entire map image in the screen
static void CenterWholeMap(RECT* rc)
{
    if (s_bmpW <= 0 || s_bmpH <= 0) return;
    CenterMapAtPoint(s_bmpW / 2, s_bmpH / 2, rc);
}

// ---- File IO Helpers ----

static bool LoadWorldFile(LPCTSTR path, WORLD* w)
{
    if (!w) return false;
    w->valid = false;

    FILE* f = _tfopen(path, TEXT("rt"));
    if (!f) return false;

    if (fscanf(f, "%lf", &w->A) != 1) { fclose(f); return false; }
    if (fscanf(f, "%lf", &w->D) != 1) { fclose(f); return false; }
    if (fscanf(f, "%lf", &w->B) != 1) { fclose(f); return false; }
    if (fscanf(f, "%lf", &w->E) != 1) { fclose(f); return false; }
    if (fscanf(f, "%lf", &w->C) != 1) { fclose(f); return false; }
    if (fscanf(f, "%lf", &w->F) != 1) { fclose(f); return false; }

    fclose(f);
    w->valid = true;
    return true;
}

static void BuildMapPaths(TCHAR* bmpPath, TCHAR* wldPath, int cch)
{
    // Map files live under the app storage directory (Settings -> Storage path)
    AppConfig* appCfg = Config_GetApp();
    TCHAR dir[MAX_PATH];
    dir[0] = 0;

    if (appCfg && appCfg->storagePath[0])
    {
        _tcsncpy(dir, appCfg->storagePath, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
    }
    else
    {
        _tcscpy(dir, TEXT("\\Storage Card\\Meshtastic"));
    }

    // ensure trailing slash
    int len = _tcslen(dir);
    if (len > 0 && dir[len - 1] != TEXT('\\'))
        _tcscat(dir, TEXT("\\"));

    _sntprintf(bmpPath, cch, TEXT("%sMaps\\map.bmp"), dir);
    _sntprintf(wldPath, cch, TEXT("%sMaps\\map.bpw"), dir);

    if (GetFileAttributes(bmpPath) == 0xFFFFFFFF)
    {
        _sntprintf(bmpPath, cch, TEXT("%smap.bmp"), dir);
        _sntprintf(wldPath, cch, TEXT("%smap.bpw"), dir);
    }
}

// Draws an 8x8 circle centered at x,y using current brush/pen
static void DrawDot(HDC hdc, int x, int y)
{
    Ellipse(hdc, x - 4, y - 4, x + 4, y + 4);
}

static int Dist2(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2;
    int dy = y1 - y2;
    return dx*dx + dy*dy;
}

static void DrawHint(HDC hdc, LPCTSTR text)
{
    RECT r;
    SetRect(&r, 4, 4, 32000, 240);
    DrawText(hdc, text, -1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
}

// ---- Scale Indicator ----

static double DegToRad(double d) { return d * 3.14159265358979323846 / 180.0; }

// Great-circle distance (meters) between 2 lat/lon points (degrees)
static double HaversineMeters(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0; // mean Earth radius (m)
    double p1 = DegToRad(lat1);
    double p2 = DegToRad(lat2);
    double dphi = DegToRad(lat2 - lat1);
    double dlambda = DegToRad(lon2 - lon1);

    double a = sin(dphi * 0.5) * sin(dphi * 0.5) +
               cos(p1) * cos(p2) * sin(dlambda * 0.5) * sin(dlambda * 0.5);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

static void FormatScaleText(double meters, TCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = 0;

    if (meters >= 1000.0)
    {
        // Use km, keep it compact
        double km = meters / 1000.0;
        int ikm = (int)(km + 0.5);
        // If < 10 km, show one decimal for better usefulness
        if (km < 10.0)
        {
            int tenths = (int)(km * 10.0 + 0.5);
            _sntprintf(out, cchOut, TEXT("%d.%d km"), tenths / 10, tenths % 10);
        }
        else
        {
            _sntprintf(out, cchOut, TEXT("%d km"), ikm);
        }
    }
    else
    {
        int im = (int)(meters + 0.5);
        _sntprintf(out, cchOut, TEXT("%d m"), im);
    }

    out[cchOut - 1] = 0;
}

static double NiceDistanceMeters(double rawMeters)
{
    if (rawMeters <= 0.0) return 0.0;

    // Choose 1, 2, 5 * 10^n
    double p = pow(10.0, floor(log10(rawMeters)));
    double m = rawMeters / p;
    double n = 1.0;
    if (m <= 1.0) n = 1.0;
    else if (m <= 2.0) n = 2.0;
    else if (m <= 5.0) n = 5.0;
    else n = 10.0;
    return n * p;
}

static void DrawScaleIndicator(HDC hdc, const RECT* prcClient)
{
    if (!hdc || !prcClient) return;
    if (!s_hBmp || !s_world.valid) return;
    if (s_zoom <= 0.0) return;

    AppConfig* appCfg = Config_GetApp();
    bool inverted = (appCfg && appCfg->mapInvertColors) ? true : false;
    COLORREF clr = inverted ? RGB(0, 255, 0) : RGB(0, 0, 0);

    int screenW = prcClient->right - prcClient->left;
    int screenH = prcClient->bottom - prcClient->top;

    // Anchor a bit above bottom-left (avoid edge/touch noise)
    int padX = 8;
    int padY = 10;
    int maxBarPx = (screenW > 0) ? (screenW / 3) : 120;
    if (maxBarPx < 80) maxBarPx = 80;
    if (maxBarPx > 160) maxBarPx = 160;

    // Pick a reference latitude near the indicator position
    int refScrX = padX;
    int refScrY = screenH - padY;
    int refMapX = s_scrollX + (int)(refScrX / s_zoom);
    int refMapY = s_scrollY + (int)(refScrY / s_zoom);

    double lat = s_world.E * (double)refMapY + s_world.F;
    double lon = s_world.A * (double)refMapX + s_world.C;

    // Distance per *map pixel* horizontally at this latitude
    double metersPerMapPx = HaversineMeters(lat, lon, lat, lon + s_world.A);
    if (metersPerMapPx <= 0.0) return;

    // Distance per *screen pixel*
    double metersPerScreenPx = metersPerMapPx / s_zoom;
    if (metersPerScreenPx <= 0.0) return;

    // Aim for a "nice" distance that fits in maxBarPx
    double targetMeters = metersPerScreenPx * (double)maxBarPx;
    double niceMeters = NiceDistanceMeters(targetMeters);
    if (niceMeters <= 0.0) return;

    int barPx = (int)(niceMeters / metersPerScreenPx + 0.5);
    if (barPx < 30) barPx = 30;
    if (barPx > maxBarPx) barPx = maxBarPx;

    // Recompute niceMeters to match the clamped pixel length, and re-nice it
    double clampedMeters = metersPerScreenPx * (double)barPx;
    niceMeters = NiceDistanceMeters(clampedMeters);
    barPx = (int)(niceMeters / metersPerScreenPx + 0.5);
    if (barPx > maxBarPx) barPx = maxBarPx;

    // Draw: baseline with end ticks + label above
    int x0 = padX;
    int y0 = screenH - padY;
    int x1 = x0 + barPx;
    int tick = 6;

    HPEN pen = CreatePen(PS_SOLID, 1, clr);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int oldBk = SetBkMode(hdc, TRANSPARENT);
    COLORREF oldText = SetTextColor(hdc, clr);

    // WinCE/eVC headers sometimes omit MoveToEx/LineTo declarations;
    // use Polyline() for maximum compatibility.
    {
        POINT pts[2];
        // baseline
        pts[0].x = x0; pts[0].y = y0;
        pts[1].x = x1; pts[1].y = y0;
        Polyline(hdc, pts, 2);

        // left tick
        pts[0].x = x0; pts[0].y = y0 - tick;
        pts[1].x = x0; pts[1].y = y0 + 1;
        Polyline(hdc, pts, 2);

        // right tick
        pts[0].x = x1; pts[0].y = y0 - tick;
        pts[1].x = x1; pts[1].y = y0 + 1;
        Polyline(hdc, pts, 2);
    }

    TCHAR label[32];
    FormatScaleText(niceMeters, label, 32);

    RECT tr;
    SetRect(&tr, x0, y0 - tick - 18, x1 + 60, y0 - tick);
    DrawText(hdc, label, -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);

    SetTextColor(hdc, oldText);
    SetBkMode(hdc, oldBk);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void MakeWorldPathFromBmp(LPCTSTR bmpPath, TCHAR* outWld, int cchOut)
{
    _tcsncpy(outWld, bmpPath, cchOut);
    outWld[cchOut - 1] = 0;

    TCHAR* lastSlash = _tcsrchr(outWld, TEXT('\\'));
    TCHAR* lastDot   = _tcsrchr(outWld, TEXT('.'));

    if (lastDot && (!lastSlash || lastDot > lastSlash))
        *lastDot = 0;

    if ((int)_tcslen(outWld) + 4 < cchOut)
        _tcscat(outWld, TEXT(".bpw"));
}

static bool BrowseForBmp(HWND hwndOwner, TCHAR* outPath, int cchOut)
{
    if (!outPath || cchOut <= 0) return false;
    outPath[0] = 0;

    OPENFILENAME ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwndOwner;
    ofn.lpstrFile    = outPath;
    ofn.nMaxFile     = cchOut;


    // Start browsing in the app storage directory
    TCHAR initDir[MAX_PATH];
    initDir[0] = 0;
    AppConfig* appCfg = Config_GetApp();
    if (appCfg && appCfg->storagePath[0])
    {
        _tcsncpy(initDir, appCfg->storagePath, MAX_PATH - 1);
        initDir[MAX_PATH - 1] = 0;
        ofn.lpstrInitialDir = initDir;
    }
    static const TCHAR s_filter[] =
        TEXT("Bitmap files (*.bmp)\0*.bmp\0All files (*.*)\0*.*\0\0");

    ofn.lpstrFilter  = s_filter;
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    return (GetOpenFileName(&ofn) != 0);
}

// ---- Optimized BMP loader ----
static HBITMAP LoadBmpManual(LPCTSTR path)
{
    FILE* f = _tfopen(path, TEXT("rb"));
    if (!f) return NULL;

    BITMAPFILEHEADER bfh;
    if (fread(&bfh, sizeof(BITMAPFILEHEADER), 1, f) != 1) { fclose(f); return NULL; }

    if (bfh.bfType != 0x4D42) { fclose(f); return NULL; }

    BITMAPINFOHEADER bih;
    if (fread(&bih, sizeof(BITMAPINFOHEADER), 1, f) != 1) { fclose(f); return NULL; }

    // Support any bit-depth CreateDIBSection supports, but 24-bit is optimal.
    // If it works for you without checks, great. Keeping minimal checks:
    if (bih.biCompression != BI_RGB) { fclose(f); return NULL; }

    struct {
        BITMAPINFOHEADER bmiHeader;
        DWORD            bmiColors[3]; 
    } bi;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader = bih;

    HDC hdc = GetDC(NULL);
    void* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    ReleaseDC(NULL, hdc);

    if (!hBmp || !pBits) { fclose(f); return NULL; }

    fseek(f, bfh.bfOffBits, SEEK_SET);

    // Calculate stride
    int stride = ((bih.biWidth * bih.biBitCount + 31) / 32) * 4;
    int height = abs(bih.biHeight);
    long dataSize = stride * height;

    if (fread(pBits, 1, dataSize, f) != (size_t)dataSize)
    {
        DeleteObject(hBmp);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return hBmp;
}

static void ClearMapState(HBITMAP* phBmp, int* pW, int* pH, WORLD* pWorld)
{
    if (phBmp && *phBmp)
    {
        DeleteObject(*phBmp);
        *phBmp = NULL;
    }
    if (pW) *pW = 0;
    if (pH) *pH = 0;
    if (pWorld) pWorld->valid = false;
}

static bool TryLoadMapState(LPCTSTR bmpPath, LPCTSTR wldPath,
                            HBITMAP* phBmp, int* pW, int* pH, WORLD* pWorld,
                            TCHAR* lastMsg, int lastMsgCch)
{
    DWORD attr = GetFileAttributes(bmpPath);
    
    *phBmp = LoadBmpManual(bmpPath);

    if (!*phBmp)
    {
        *phBmp = (HBITMAP)LoadImage(NULL, bmpPath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
    }

    if (*phBmp)
    {
        BITMAP bm;
        GetObject(*phBmp, sizeof(bm), &bm);
        *pW = bm.bmWidth;
        *pH = bm.bmHeight;

        if (lastMsg && lastMsgCch > 0) lastMsg[0] = 0;
        LoadWorldFile(wldPath, pWorld);
        return true;
    }

    if (lastMsg && lastMsgCch > 0)
    {
        _sntprintf(lastMsg, lastMsgCch,
            TEXT("Failed to load map.\nPath: %s\nCheck format."),
            bmpPath
        );
        lastMsg[lastMsgCch - 1] = 0;
    }

    LoadWorldFile(wldPath, pWorld);
    return false;
}

// ---- Main Window Procedure ----

LRESULT CALLBACK MapView_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        s_world.valid = false;
        s_triedLoad = false;
        
        s_hBtnLoad = CreateWindow(TEXT("BUTTON"), TEXT("Load"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,0,0,0, hwnd, (HMENU)IDC_LOADMAP, g_App.hInst, NULL);
        s_hBtnIn   = CreateWindow(TEXT("BUTTON"), TEXT("+"),    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,0,0,0, hwnd, (HMENU)IDC_ZOOMIN,  g_App.hInst, NULL);
        s_hBtnOut  = CreateWindow(TEXT("BUTTON"), TEXT("-"),    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,0,0,0, hwnd, (HMENU)IDC_ZOOMOUT, g_App.hInst, NULL);
        s_hBtnFit  = CreateWindow(TEXT("BUTTON"), TEXT("Fit"),  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0,0,0,0, hwnd, (HMENU)IDC_ZOOMFIT, g_App.hInst, NULL);

        // Top-left: pseudo dark mode toggle (invert map bitmap colors)
        s_hChkInvert = CreateWindow(TEXT("BUTTON"), TEXT("Invert cl."),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_INVERTCL, g_App.hInst, NULL);

        // Apply initial state from persisted app config
        {
            AppConfig* appCfg = Config_GetApp();
            if (appCfg && appCfg->mapInvertColors)
                SendMessage(s_hChkInvert, BM_SETCHECK, BST_CHECKED, 0);
            else
                SendMessage(s_hChkInvert, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        
        return 0;
    }

    case WM_SIZE:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        int btnW = 30, btnH = 30; 
        int loadW = 50;
        int pad = 2;

        MoveWindow(s_hBtnLoad, w - loadW - pad, pad, loadW, 22, TRUE);

        // Top-left toggle (keep compact, avoid stealing map area)
        // Slightly taller so it is easy to tap on CE.
        if (s_hChkInvert)
            MoveWindow(s_hChkInvert, pad, pad, 90, 22, TRUE);

        int x = w - pad - btnW;
        int y = h - pad - btnH;
        
        MoveWindow(s_hBtnFit, x, y, btnW, btnH, TRUE); 
        y -= (btnH + pad);
        MoveWindow(s_hBtnOut, x, y, btnW, btnH, TRUE);
        y -= (btnH + pad);
        MoveWindow(s_hBtnIn,  x, y, btnW, btnH, TRUE);
        
        return 0;
    }

    case WM_DESTROY:
        ClearMapState(&s_hBmp, &s_bmpW, &s_bmpH, &s_world);
        return 0;

    case WM_LBUTTONDOWN:
    {
        SetCapture(hwnd);
        s_isDragging = false;
        s_lastMouse.x = (short)LOWORD(lParam);
        s_lastMouse.y = (short)HIWORD(lParam);
        s_downMouse = s_lastMouse;
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (GetCapture() == hwnd)
        {
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            
            int dx = mx - s_lastMouse.x;
            int dy = my - s_lastMouse.y;

            if (!s_isDragging && (abs(mx - s_downMouse.x) > 5 || abs(my - s_downMouse.y) > 5))
            {
                s_isDragging = true;
            }

            if (s_isDragging)
            {
                s_scrollX -= (int)(dx / s_zoom); 
                s_scrollY -= (int)(dy / s_zoom);
                
                s_lastMouse.x = mx;
                s_lastMouse.y = my;
                
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        ReleaseCapture();
        
        if (!s_isDragging)
        {
            // Simple single tap logic for node selection
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            int bestIdx = -1;
            int bestD2 = 15 * 15; // Hit radius

            int count = Nodes_GetCount();
            for (int i = 0; i < count; ++i)
            {
                NodeInfo* n = Nodes_GetAt(i);
                if (!n || !n->hasPosition) continue;

                int mapX = CoordToMapX(n->longitude);
                int mapY = CoordToMapY(n->latitude);

                int sx = MapToScreenX(mapX);
                int sy = MapToScreenY(mapY);

                int d2 = Dist2(mx, my, sx, sy);
                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestIdx = i;
                }
            }

            if (bestIdx >= 0) {
                NodeDetails_Show(g_App.hMain, hwnd, Nodes_GetAt(bestIdx));
            }
        }
        s_isDragging = false;
        return 0;
    }

    // ---- Double Tap to Zoom ----
    case WM_LBUTTONDBLCLK:
    {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);

        // Calculate where on the map we clicked
        int clickMapX = s_scrollX + (int)(mx / s_zoom);
        int clickMapY = s_scrollY + (int)(my / s_zoom);

        // Zoom in
        s_zoom *= 1.5;
        if (s_zoom > 8.0) s_zoom = 8.0;

        // Center on the point we clicked
        RECT rc;
        GetClientRect(hwnd, &rc);
        CenterMapAtPoint(clickMapX, clickMapY, &rc);

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        RECT rc; 
        GetClientRect(hwnd, &rc);
        int centerScrX = (rc.right - rc.left) / 2;
        int centerScrY = (rc.bottom - rc.top) / 2;

        int centerMapX = s_scrollX + (int)(centerScrX / s_zoom);
        int centerMapY = s_scrollY + (int)(centerScrY / s_zoom);

        if (id == IDC_INVERTCL && code == BN_CLICKED)
        {
            AppConfig* appCfg = Config_GetApp();
            if (appCfg)
            {
                LRESULT chk = SendMessage(s_hChkInvert, BM_GETCHECK, 0, 0);
                appCfg->mapInvertColors = (chk == BST_CHECKED) ? 1 : 0;
                Config_SaveApp();
            }

            // Immediate apply: repaint (no need to reload bitmap)
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
            return 0;
        }

        if (id == IDC_LOADMAP)
        {
            TCHAR chosen[MAX_PATH];
            if (BrowseForBmp(hwnd, chosen, MAX_PATH))
            {
                _tcsncpy(s_userBmpPath, chosen, MAX_PATH);
                MakeWorldPathFromBmp(s_userBmpPath, s_userWldPath, MAX_PATH);
                s_triedLoad = true;
                ClearMapState(&s_hBmp, &s_bmpW, &s_bmpH, &s_world);
                TryLoadMapState(s_userBmpPath, s_userWldPath, &s_hBmp, &s_bmpW, &s_bmpH, &s_world, s_lastLoadMsg, 512);
                
                // Reset to Center
                s_zoom = 1.0;
                CenterWholeMap(&rc);
                
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
        else if (id == IDC_ZOOMIN)
        {
            s_zoom *= 1.5; 
            if (s_zoom > 8.0) s_zoom = 8.0;
            CenterMapAtPoint(centerMapX, centerMapY, &rc);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (id == IDC_ZOOMOUT)
        {
            s_zoom /= 1.5;
            if (s_zoom < 0.1) s_zoom = 0.1;
            CenterMapAtPoint(centerMapX, centerMapY, &rc);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (id == IDC_ZOOMFIT)
        {
            if (s_bmpW > 0 && s_bmpH > 0)
            {
                double ratioW = (double)(rc.right - rc.left) / s_bmpW;
                double ratioH = (double)(rc.bottom - rc.top) / s_bmpH;
                // Pick the smaller ratio so entire map is visible
                s_zoom = (ratioW < ratioH) ? ratioW : ratioH; 
                
                CenterWholeMap(&rc);
            }
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int screenW = rc.right - rc.left;
        int screenH = rc.bottom - rc.top;

        // Auto-load logic
        if (!s_triedLoad) {
            s_triedLoad = true;
            TCHAR bmp[MAX_PATH], wld[MAX_PATH];
            BuildMapPaths(bmp, wld, MAX_PATH);
            ClearMapState(&s_hBmp, &s_bmpW, &s_bmpH, &s_world);
            if (TryLoadMapState(bmp, wld, &s_hBmp, &s_bmpW, &s_bmpH, &s_world, s_lastLoadMsg, 512))
            {
                // Auto-center on startup
                CenterWholeMap(&rc);
            }
        }

        // Fill background Black to handle edges
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

        if (s_hBmp)
        {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, s_hBmp);

            DWORD rop = SRCCOPY;
            {
                AppConfig* appCfg = Config_GetApp();
                if (appCfg && appCfg->mapInvertColors)
                    rop = NOTSRCCOPY;
            }

            int srcW = (int)(screenW / s_zoom);
            int srcH = (int)(screenH / s_zoom);

            if (s_zoom == 1.0)
            {
                BitBlt(hdc, 0, 0, screenW, screenH, mem, s_scrollX, s_scrollY, rop);
            }
            else
            {
                StretchBlt(hdc, 0, 0, screenW, screenH, mem, s_scrollX, s_scrollY, srcW, srcH, rop);
            }

            SelectObject(mem, old);
            DeleteDC(mem);
        }
        else
        {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);
            DrawHint(hdc, s_lastLoadMsg[0] ? s_lastLoadMsg : TEXT("No map loaded."));
        }

        int count = Nodes_GetCount();
        if (count > 0)
        {
            // Tools for Outer Red Circle
            HPEN redPen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0)); 
            HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0)); 

            // Tools for Inner Black Dot
            HPEN blackPen = (HPEN)GetStockObject(BLACK_PEN);
            HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);

            HGDIOBJ oldPen = SelectObject(hdc, redPen);
            HGDIOBJ oldBr  = SelectObject(hdc, redBrush);
            
            // Text Settings: Opaque Black Background, Green Text
            SetBkMode(hdc, OPAQUE); 
            SetBkColor(hdc, RGB(0, 0, 0)); 
            SetTextColor(hdc, RGB(0, 255, 0)); 

            for (int i = 0; i < count; ++i)
            {
                NodeInfo* n = Nodes_GetAt(i);
                if (!n || !n->hasPosition) continue;

                int mapX = 0, mapY = 0;
                
                if (s_world.valid)
                {
                    mapX = CoordToMapX(n->longitude);
                    mapY = CoordToMapY(n->latitude);
                }
                else
                {
                    continue; 
                }

                int screenX = MapToScreenX(mapX);
                int screenY = MapToScreenY(mapY);

                if (screenX < -20 || screenX > screenW + 20 || screenY < -20 || screenY > screenH + 20)
                    continue;

                // 1. Draw Outer Red Circle (using existing helper)
                SelectObject(hdc, redPen);
                SelectObject(hdc, redBrush);
                DrawDot(hdc, screenX, screenY);

                // 2. Draw Inner Black Dot (small 3x3 center)
                SelectObject(hdc, blackPen);
                SelectObject(hdc, blackBrush);
                Ellipse(hdc, screenX - 1, screenY - 1, screenX + 2, screenY + 2);
                
                // ---- DRAW NAME (fallback to last 4 of USER ID if short name missing) ----
                {
                    TCHAR disp[16];
                    Nodes_GetDisplayShortName(n, disp, 16);
                    if (disp[0])
                    {
                        RECT lr;
                        SetRect(&lr, screenX + 6, screenY - 8, screenX + 250, screenY + 20);
                        DrawText(hdc, disp, -1, &lr, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
                    }
                }
            }

            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBr);
            DeleteObject(redPen);
            DeleteObject(redBrush);
            // Do not delete stock objects (blackPen/Brush)
        }

        // Scale indicator (bottom-left)
        DrawScaleIndicator(hdc, &rc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void MapView_Register(HINSTANCE hInst)
{
    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = MapView_WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = MAPVIEW_CLASS;
    wc.style         = CS_DBLCLKS; 

    RegisterClass(&wc);
}