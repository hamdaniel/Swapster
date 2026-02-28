#include "swapping.h"

#include <vector>
#include <string>
#include <iostream>

namespace swapster {
/**
 * Swapster server (debug build): waits for a named event and swaps a single
 * WhatsApp Desktop window to the other monitor.
 *
 * Design goals:
 * - MinGW-friendly (no lambdas for WinAPI callbacks).
 * - Robust with snapped windows: overwrite WINDOWPLACEMENT.rcNormalPosition
 *   so an "unsnap" cannot jump to an old remembered restore rect.
 * - Minimize Electron/Chromium repaint artifacts using redraw + jiggle + hide/show.
 * - Avoid stealing focus: restore the original foreground window after the move.
 */

HMONITOR g_currentMonitor = NULL;
HMONITOR g_targetMonitor  = NULL;

/** EnumDisplayMonitors callback: pick the first monitor that isn't current. */
BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    if (hMon != g_currentMonitor)
    {
        g_targetMonitor = hMon;
        return FALSE; // stop enumeration
    }
    return TRUE; // continue
}

/** Best-effort: enable per-monitor DPI awareness v2 (dynamic to support older MinGW headers). */
void EnablePerMonitorDpiAwarenessV2() {
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    const HANDLE PMv2 = (HANDLE)-4;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    typedef BOOL(WINAPI *SetDpiCtxFn)(HANDLE);
    SetDpiCtxFn pSet = (SetDpiCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (pSet) {
        pSet(PMv2);
    }
}

/** Best-effort: flush DWM compositor (dynamic so we don't need -ldwmapi). */
void TryDwmFlush() {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;

    typedef HRESULT(WINAPI *DwmFlushFn)();
    DwmFlushFn pFlush = (DwmFlushFn)GetProcAddress(dwm, "DwmFlush");
    if (pFlush) {
        pFlush();
    }

    FreeLibrary(dwm);
}

/** Force a full repaint of a window, including non-client area. */
void ForceRepaint(HWND hwnd) {
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    RedrawWindow(hwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW |
                     RDW_ERASENOW);

    InvalidateRect(hwnd, NULL, TRUE);
    UpdateWindow(hwnd);
    TryDwmFlush();
}

/** Electron/Chromium often repaints more reliably after a tiny real resize. */
void ElectronSurfaceJiggle(HWND hwnd, int x, int y, int w, int h) {
    SetWindowPos(hwnd, NULL, x, y, w + 1, h, SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(16);
    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/** Hide/show without activation (lower chance of focus steal than minimize/restore). */
void ElectronHideShow(HWND hwnd) {
    ShowWindowAsync(hwnd, SW_HIDE);
    Sleep(30);
    ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
}

/** Restore focus to the provided window (best-effort). */
void RestoreForeground(HWND fg) {
    if (!fg || !IsWindow(fg)) return;

    DWORD fgThread = GetWindowThreadProcessId(fg, NULL);
    DWORD myThread = GetCurrentThreadId();

    AttachThreadInput(myThread, fgThread, TRUE);
    SetForegroundWindow(fg);
    SetFocus(fg);
    AttachThreadInput(myThread, fgThread, FALSE);
}

/**
 * EnumWindows callback: swaps the display each window is in.
 */
BOOL CALLBACK EnumWindowsSwap(HWND hwnd, LPARAM)
{
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    // Skip owned / tool windows (menus, tooltips, etc.)
    if (GetWindow(hwnd, GW_OWNER) != NULL) {
        return TRUE;
    }

    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return TRUE;
    }

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &wp);

    if (wp.showCmd == SW_SHOWMINIMIZED) {
        return TRUE;
    }

    RECT originalRect;
    if (!GetWindowRect(hwnd, &originalRect)) {
        return TRUE;
    }

    // Determine source and destination monitors
    g_currentMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    g_targetMonitor  = NULL;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    if (!g_targetMonitor) {
        return TRUE;
    }

    MONITORINFO miSrc = { sizeof(miSrc) };
    MONITORINFO miDst = { sizeof(miDst) };
    if (!GetMonitorInfo(g_currentMonitor, &miSrc)) return TRUE;
    if (!GetMonitorInfo(g_targetMonitor,  &miDst)) return TRUE;

    RECT srcMon  = miSrc.rcMonitor;
    RECT srcWork = miSrc.rcWork;

    RECT dstMon  = miDst.rcMonitor;
    RECT dstWork = miDst.rcWork;

    bool wasMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    if (wasMaximized)
        ShowWindowAsync(hwnd, SW_RESTORE);

    // ------------------------------------------------------------
    // UNSNAP WITHOUT MOVING
    // ------------------------------------------------------------

    RECT unsnappedRect = originalRect;
    const int inset = 0;

    unsnappedRect.left   += inset;
    unsnappedRect.top    += inset;
    unsnappedRect.right  -= inset;
    unsnappedRect.bottom -= inset;

    int uW = unsnappedRect.right - unsnappedRect.left;
    int uH = unsnappedRect.bottom - unsnappedRect.top;

    if (uW < 300) { unsnappedRect.right = unsnappedRect.left + 300; uW = 300; }
    if (uH < 200) { unsnappedRect.bottom = unsnappedRect.top + 200; uH = 200; }

    int srcWorkW = srcWork.right - srcWork.left;
    int srcWorkH = srcWork.bottom - srcWork.top;

    if (uW > srcWorkW) uW = srcWorkW;
    if (uH > srcWorkH) uH = srcWorkH;

    if (unsnappedRect.left < srcWork.left) unsnappedRect.left = srcWork.left;
    if (unsnappedRect.top  < srcWork.top)  unsnappedRect.top  = srcWork.top;

    if (unsnappedRect.left + uW > srcWork.right)  unsnappedRect.left = srcWork.right - uW;
    if (unsnappedRect.top  + uH > srcWork.bottom) unsnappedRect.top  = srcWork.bottom - uH;

    unsnappedRect.right  = unsnappedRect.left + uW;
    unsnappedRect.bottom = unsnappedRect.top  + uH;

    WINDOWPLACEMENT wp2 = wp;
    wp2.length = sizeof(WINDOWPLACEMENT);
    wp2.showCmd = SW_SHOWNORMAL;
    wp2.flags = 0;
    wp2.rcNormalPosition = unsnappedRect;

    SetWindowPlacement(hwnd, &wp2);
    ShowWindowAsync(hwnd, SW_SHOWNORMAL);

    SetWindowPos(hwnd, NULL,
                 unsnappedRect.left, unsnappedRect.top,
                 uW, uH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    ForceRepaint(hwnd);
    ElectronHideShow(hwnd);
    ForceRepaint(hwnd);

    RECT winRect;
    if (!GetWindowRect(hwnd, &winRect))
        return TRUE;

    // ------------------------------------------------------------
    // RELATIVE TRANSFORM
    // ------------------------------------------------------------

    int srcW = srcMon.right - srcMon.left;
    int srcH = srcMon.bottom - srcMon.top;
    int dstW = dstMon.right - dstMon.left;
    int dstH = dstMon.bottom - dstMon.top;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return TRUE;

    int winW = winRect.right - winRect.left;
    int winH = winRect.bottom - winRect.top;

    double relX = (double)(winRect.left - srcMon.left) / (double)srcW;
    double relY = (double)(winRect.top  - srcMon.top ) / (double)srcH;
    double relW = (double)winW / (double)srcW;
    double relH = (double)winH / (double)srcH;

    int newW = (int)(relW * (double)dstW);
    int newH = (int)(relH * (double)dstH);
    int newX = dstMon.left + (int)(relX * (double)dstW);
    int newY = dstMon.top  + (int)(relY * (double)dstH);

    if (newW < 300) newW = 300;
    if (newH < 200) newH = 200;

    int workW = dstWork.right - dstWork.left;
    int workH = dstWork.bottom - dstWork.top;

    if (newW > workW) newW = workW;
    if (newH > workH) newH = workH;

    if (newX < dstWork.left) newX = dstWork.left;
    if (newY < dstWork.top)  newY = dstWork.top;
    if (newX + newW > dstWork.right)  newX = dstWork.right - newW;
    if (newY + newH > dstWork.bottom) newY = dstWork.bottom - newH;

    // SetWindowPos(hwnd, NULL, newX, newY, newW, newH,
    //              SWP_NOZORDER | SWP_NOACTIVATE);

    // ForceRepaint(hwnd);
    // ElectronSurfaceJiggle(hwnd, newX, newY, newW, newH);
    // ForceRepaint(hwnd);
    // ElectronHideShow(hwnd);
    // ForceRepaint(hwnd);

    // if (wasMaximized)
    //     ShowWindowAsync(hwnd, SW_MAXIMIZE);

    // Freeze redraw (prevents visible glitches)
    SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);

    // Move + resize invisibly
    SetWindowPos(hwnd, NULL, newX, newY, newW, newH,
                SWP_NOZORDER | SWP_NOACTIVATE);

    // Restore maximized state if needed
    if (wasMaximized)
        ShowWindowAsync(hwnd, SW_MAXIMIZE);

    // Unfreeze redraw
    SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);

    // Single full redraw
    RedrawWindow(hwnd, NULL, NULL,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    // ONE gentle layout nudge (keeps your previous fixes)
    ElectronSurfaceJiggle(hwnd, newX, newY, newW, newH);

    // Final repaint to settle
    RedrawWindow(hwnd, NULL, NULL,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    return TRUE; // continue to next window
}



// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void Initialize()
{
    static bool done = false;
    if (done) return;
    done = true;
    EnablePerMonitorDpiAwarenessV2();
}

void SwapAllWindows()
{
    FILE* flog = fopen("C:\\ProgramData\\Swapster\\swapster_log.txt", "a");
    if (flog) { fprintf(flog, "SwapAllWindows() starting\n"); fclose(flog); }
    
    HWND fg = GetForegroundWindow();
	EnumWindows(EnumWindowsSwap, 0);
	RestoreForeground(fg);
    
    flog = fopen("C:\\ProgramData\\Swapster\\swapster_log.txt", "a");
    if (flog) { fprintf(flog, "SwapAllWindows() finished\n"); fclose(flog); }
}

} // namespace swapster