#include "scene.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <cwchar>

namespace {

struct EnumCtx {
    HWND overlay;
    HWND tray;
    POINT origin;
    Scene* out;
};

bool ClassIs(const wchar_t* cls, const wchar_t* name) { return wcscmp(cls, name) == 0; }

bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (h == ctx->overlay || h == ctx->tray) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;

    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (style & WS_CHILD) return TRUE;
    if (ex & WS_EX_TRANSPARENT) return TRUE;  // other click-through overlays
    if (IsCloaked(h)) return TRUE;            // hidden UWP windows, other virtual desktops

    wchar_t cls[96] = {};
    GetClassNameW(h, cls, 96);
    const bool taskbar = ClassIs(cls, L"Shell_TrayWnd") || ClassIs(cls, L"Shell_SecondaryTrayWnd");
    if (!taskbar) {
        if (ClassIs(cls, L"Progman") || ClassIs(cls, L"WorkerW")) return TRUE;  // desktop
        if (ex & WS_EX_TOOLWINDOW) return TRUE;  // tooltips, shadows, popups
    }
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        if (GetLayeredWindowAttributes(h, &key, &alpha, &flags) && (flags & LWA_ALPHA) && alpha < 64)
            return TRUE;  // (almost) invisible
    }

    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    if (r.right - r.left < 60 || r.bottom - r.top < 24) return TRUE;

    OffsetRect(&r, -ctx->origin.x, -ctx->origin.y);
    ctx->out->windows.push_back({h, r, taskbar});
    return TRUE;
}

}  // namespace

RECT PrimaryMonitorRect() {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) return mi.rcMonitor;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void ScanScene(HWND overlay, HWND trayWnd, POINT origin, Scene& out) {
    out.windows.clear();
    out.monitors.clear();
    EnumCtx ctx{overlay, trayWnd, origin, &out};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));  // top to bottom
    // Snow falls on the main (primary) screen only.
    RECT r = PrimaryMonitorRect();
    OffsetRect(&r, -origin.x, -origin.y);
    out.monitors.push_back(r);
}

bool IsFullscreenAppActive(HWND overlay) {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) return true;
    }

    HWND fg = GetForegroundWindow();
    if (!fg || fg == overlay) return false;
    wchar_t cls[96] = {};
    GetClassNameW(fg, cls, 96);
    if (ClassIs(cls, L"Progman") || ClassIs(cls, L"WorkerW") || ClassIs(cls, L"Shell_TrayWnd"))
        return false;

    // A normal maximized window (with a title bar) is not "fullscreen".
    const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if (IsZoomed(fg) && (style & WS_CAPTION) == WS_CAPTION) return false;

    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
