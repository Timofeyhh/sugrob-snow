// Сугроб — snow on the Windows desktop.
// A click-through, always-on-top transparent overlay: flakes fall, pile up on the
// top edges of windows, the taskbar and the bottom of the screen; the cursor sweeps them away.
#include "common.h"
#include "panel.h"
#include "render.h"
#include "scene.h"
#include "sim.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT kTrayId = 1;
enum : UINT {
    CMD_PAUSE = 100,
    CMD_PANEL,
    CMD_LIGHT,  // CMD_LIGHT + (int)Intensity, in Intensity order
    CMD_NORMAL,
    CMD_HEAVY,
    CMD_BLIZZARD,
    CMD_BURAN,
    CMD_SLEET,
    CMD_WIND_0,  // CMD_WIND_0 + windPower
    CMD_WIND_1,
    CMD_WIND_2,
    CMD_WIND_3,
    CMD_DIR_LEFT,
    CMD_DIR_RIGHT,
    CMD_CLEAR,
    CMD_EXIT,
};

struct App {
    HINSTANCE inst = nullptr;
    HWND overlay = nullptr;  // transparent snow layer
    HWND tray = nullptr;     // hidden window: tray icon, menu, system messages
    UINT taskbarCreated = 0;
    HICON icon = nullptr;
    POINT origin{};
    int width = 0, height = 0;
    Settings settings;
    World world;
    Renderer renderer;
    Scene scene;
    bool sceneDirty = true;
    bool running = true;
    std::vector<HWINEVENTHOOK> hooks;
    HHOOK mouseHook = nullptr;
    bool swallowUp = false;
};
App g;

// The overlay is click-through, so snowballs listen to the mouse via a low-level hook:
// a press on a snowball or on a pile is taken; every other click goes to the windows as usual.
LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        const POINT p{m->pt.x - g.origin.x, m->pt.y - g.origin.y};
        if (wp == WM_LBUTTONDOWN && !Panel::Contains(m->pt) && g.world.TryGrab(p)) {
            g.swallowUp = true;
            return 1;
        }
        if (wp == WM_LBUTTONUP && g.swallowUp) {
            g.swallowUp = false;
            g.world.Release();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void SetMouseHook(bool on) {
    if (on && !g.mouseHook) {
        g.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g.inst, 0);
    } else if (!on && g.mouseHook) {
        UnhookWindowsHookEx(g.mouseHook);
        g.mouseHook = nullptr;
        if (g.swallowUp) {
            g.swallowUp = false;
            g.world.Release();
        }
    }
}

// --- tray icon --------------------------------------------------------------

float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0.f ? std::clamp((wx * vx + wy * vy) / len2, 0.f, 1.f) : 0.f;
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

HICON CreateSnowflakeIcon(int size) {
    struct Seg {
        float ax, ay, bx, by;
    };
    std::vector<Seg> segs;
    const float c = size * 0.5f, R = size * 0.44f;
    for (int k = 0; k < 6; ++k) {
        const float a = (k * 60.f - 90.f) * 3.14159265f / 180.f;
        const float ux = std::cos(a), uy = std::sin(a);
        segs.push_back({c, c, c + ux * R, c + uy * R});
        const float bx = c + ux * R * 0.55f, by = c + uy * R * 0.55f;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const float b = a + sgn * 0.75f;
            segs.push_back({bx, by, bx + std::cos(b) * R * 0.32f, by + std::sin(b) * R * 0.32f});
        }
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    const float hw = std::max(0.6f, size / 22.f);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float d = 1e9f;
            for (const Seg& s : segs) d = std::min(d, SegDist(x + 0.5f, y + 0.5f, s.ax, s.ay, s.bx, s.by));
            const float core = std::clamp(hw + 0.5f - d, 0.f, 1.f);
            const float edge = std::clamp(hw + 1.6f - d, 0.f, 1.f) * (1.f - core);  // dark outline
            const float a = core + edge;
            uint32_t argb = 0;
            if (a > 0.f) {
                const uint32_t r = (uint32_t)((core * 240.f + edge * 30.f) / a);
                const uint32_t gr = (uint32_t)((core * 248.f + edge * 70.f) / a);
                const uint32_t b = (uint32_t)((core * 255.f + edge * 130.f) / a);
                argb = ((uint32_t)std::lround(a * 255.f) << 24) | (r << 16) | (gr << 8) | b;
            }
            px[y * size + x] = argb;
        }
    std::vector<uint8_t> maskBits((size_t)((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void FillTip(NOTIFYICONDATAW& nid) {
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), g.settings.paused ? L"Сугроб — пауза (клик — панель погоды)"
                                                                : L"Сугроб — клик: погода, правый клик: меню");
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.tray;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g.icon;
    FillTip(nid);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void UpdateTrayTip() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.tray;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    FillTip(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.tray;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void SetPaused(bool paused) {
    g.settings.paused = paused;
    UpdateTrayTip();
    Panel::Refresh();
}

void OnPanelAction(PanelAction a) {
    switch (a) {
        case PanelAction::TogglePause: SetPaused(!g.settings.paused); break;
        case PanelAction::Clear: g.world.ClearSnow(); break;
        default: break;  // weather / wind: the panel already changed g.settings
    }
}

// The tray icon's rectangle on screen (the panel opens above it).
RECT TrayIconRect() {
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = g.tray;
    id.uID = kTrayId;
    RECT r{};
    if (FAILED(Shell_NotifyIconGetRect(&id, &r)) || r.right <= r.left) {
        POINT pt{};
        GetCursorPos(&pt);
        r = RECT{pt.x - 8, pt.y - 8, pt.x + 8, pt.y + 8};
    }
    return r;
}

void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Сугроб");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_PANEL, L"Панель погоды…");
    AppendMenuW(m, MF_STRING | (g.settings.paused ? MF_CHECKED : MF_UNCHECKED), CMD_PAUSE, L"Пауза");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_LIGHT, L"Слабый снег");
    AppendMenuW(m, MF_STRING, CMD_NORMAL, L"Снег");
    AppendMenuW(m, MF_STRING, CMD_HEAVY, L"Сильный снег");
    AppendMenuW(m, MF_STRING, CMD_BLIZZARD, L"Метель");
    AppendMenuW(m, MF_STRING, CMD_BURAN, L"Буран");
    AppendMenuW(m, MF_STRING, CMD_SLEET, L"Снег с дождём");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_WIND_0, L"Без ветра");
    AppendMenuW(m, MF_STRING, CMD_WIND_1, L"Слабый ветер");
    AppendMenuW(m, MF_STRING, CMD_WIND_2, L"Сильный ветер");
    AppendMenuW(m, MF_STRING, CMD_WIND_3, L"Ураган");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_DIR_LEFT, L"Ветер влево");
    AppendMenuW(m, MF_STRING, CMD_DIR_RIGHT, L"Ветер вправо");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_CLEAR, L"Убрать весь снег");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Выход");
    CheckMenuRadioItem(m, CMD_LIGHT, CMD_SLEET, CMD_LIGHT + (UINT)g.settings.intensity, MF_BYCOMMAND);
    CheckMenuRadioItem(m, CMD_WIND_0, CMD_WIND_3, CMD_WIND_0 + (UINT)std::clamp(g.settings.windPower, 0, kWindPowers - 1), MF_BYCOMMAND);
    CheckMenuRadioItem(m, CMD_DIR_LEFT, CMD_DIR_RIGHT, g.settings.wind == WindDir::Left ? CMD_DIR_LEFT : CMD_DIR_RIGHT, MF_BYCOMMAND);
    if (g.settings.windPower == 0) {  // the direction only matters when there is wind
        EnableMenuItem(m, CMD_DIR_LEFT, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(m, CMD_DIR_RIGHT, MF_BYCOMMAND | MF_GRAYED);
    }
    SetMenuDefaultItem(m, CMD_PANEL, FALSE);

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g.tray);  // so the menu closes when clicking elsewhere
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.tray, nullptr);
    PostMessageW(g.tray, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case CMD_PAUSE: SetPaused(!g.settings.paused); break;
        case CMD_PANEL: Panel::Toggle(TrayIconRect()); break;
        case CMD_DIR_LEFT: g.settings.wind = WindDir::Left; break;
        case CMD_DIR_RIGHT: g.settings.wind = WindDir::Right; break;
        case CMD_CLEAR: g.world.ClearSnow(); break;
        case CMD_EXIT: g.running = false; break;
        default:
            if (cmd >= CMD_LIGHT && cmd <= CMD_SLEET) ApplyWeather(g.settings, (Intensity)(cmd - CMD_LIGHT));
            if (cmd >= CMD_WIND_0 && cmd <= CMD_WIND_3) g.settings.windPower = (int)(cmd - CMD_WIND_0);
            break;
    }
    Panel::Refresh();
}

// --- geometry & windows -----------------------------------------------------

void UpdateGeometry() {
    const RECT pr = PrimaryMonitorRect();  // snow covers the main screen only
    const POINT origin{pr.left, pr.top};
    const int w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (origin.x == g.origin.x && origin.y == g.origin.y && w == g.width && h == g.height) return;
    g.origin = origin;
    g.width = w;
    g.height = h;
    SetWindowPos(g.overlay, HWND_TOPMOST, origin.x, origin.y, w, h, SWP_NOACTIVATE);
    g.renderer.Resize(w, h);
    g.world.Resize(w, h);
    g.sceneDirty = true;
}

LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_TRAY) {
        const UINT ev = LOWORD(lp);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            Panel::Hide();
            ShowTrayMenu();
        } else if (ev == WM_LBUTTONUP) {
            Panel::Toggle(TrayIconRect());  // a click on the snowflake opens / closes the weather panel
        }
        return 0;
    }
    if (g.taskbarCreated && msg == g.taskbarCreated) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_DISPLAYCHANGE:
            UpdateGeometry();
            return 0;
        case WM_SETTINGCHANGE:  // taskbar moved / work area changed
            g.sceneDirty = true;
            break;
        case WM_CLOSE:
            g.running = false;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

// Window moved / shown / hidden / minimized / cloaked → rescan before the next frame.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd || hwnd == g.overlay) return;
    if (event != EVENT_OBJECT_DESTROY && GetAncestor(hwnd, GA_ROOT) != hwnd) return;  // top-level only
    g.sceneDirty = true;
}

void InstallHooks() {
    const DWORD ranges[][2] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},  // create, destroy, show, hide
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (const auto& r : ranges) {
        HWINEVENTHOOK hk = SetWinEventHook(r[0], r[1], nullptr, WinEventProc, 0, 0,
                                           WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hk) g.hooks.push_back(hk);
    }
}

double Now() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// --stats: once a second appends fps / timings to %TEMP%\sugrob_stats.txt (for tuning).
struct Stats {
    bool on = false;
    wchar_t path[MAX_PATH] = {};
    double t0 = 0.0, updMs = 0.0, drawMs = 0.0;
    int frames = 0, scans = 0;

    void Init() {
        on = true;
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, MAX_PATH, L"sugrob_stats.txt");
        DeleteFileW(path);
        t0 = Now();
    }
    void Frame(double upd, double draw, double now, size_t flakes, size_t ledges, bool batch) {
        if (!on) return;
        ++frames;
        updMs += upd * 1000.0;
        drawMs += draw * 1000.0;
        if (now - t0 < 1.0) return;
        char line[256];
        const int len = snprintf(line, sizeof(line), "fps=%d flakes=%zu ledges=%zu scans=%d update=%.2fms draw=%.2fms batch=%d\r\n",
                                 frames, flakes, ledges, scans, updMs / frames, drawMs / frames, batch ? 1 : 0);
        HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, line, (DWORD)std::max(0, len), &written, nullptr);
            CloseHandle(f);
        }
        t0 = now;
        frames = scans = 0;
        updMs = drawMs = 0.0;
    }
};

}  // namespace

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SugrobSnowOverlay");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Сугроб уже запущен — его значок в области уведомлений, рядом с часами.", L"Сугроб",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const wchar_t* cmdline = GetCommandLineW();
    if (wcsstr(cmdline, L"--fast")) g.settings.accumulation = 15.f;  // demo: piles grow in seconds
    if (wcsstr(cmdline, L"--blizzard")) ApplyWeather(g.settings, Intensity::Blizzard);
    if (wcsstr(cmdline, L"--buran")) ApplyWeather(g.settings, Intensity::Buran);
    if (wcsstr(cmdline, L"--sleet")) ApplyWeather(g.settings, Intensity::Sleet);
    if (wcsstr(cmdline, L"--heavy")) ApplyWeather(g.settings, Intensity::Heavy);
    Stats stats;
    if (wcsstr(cmdline, L"--stats")) stats.Init();
    // --bench: invisible 8-second performance run (ignores fullscreen apps), writes stats and exits.
    const bool bench = wcsstr(cmdline, L"--bench") != nullptr;
    if (bench && !stats.on) stats.Init();

    g.inst = inst;
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = L"SugrobOverlay";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = TrayProc;
    wc.lpszClassName = L"SugrobTray";
    RegisterClassExW(&wc);

    const RECT pr = PrimaryMonitorRect();  // snow covers the main screen only
    g.origin = {pr.left, pr.top};
    g.width = pr.right - pr.left;
    g.height = pr.bottom - pr.top;

    g.tray = CreateWindowExW(WS_EX_TOOLWINDOW, L"SugrobTray", L"Sugrob", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             nullptr);
    // Layered + transparent = mouse clicks go straight through to the windows below.
    g.overlay = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"SugrobOverlay", L"Sugrob Snow", WS_POPUP, g.origin.x, g.origin.y, g.width, g.height, nullptr, nullptr, inst,
        nullptr);
    if (!g.tray || !g.overlay) return 1;
    SetLayeredWindowAttributes(g.overlay, 0, bench ? 0 : 255, LWA_ALPHA);
    // Tell the shell this full-screen topmost window is not a "fullscreen app"
    // (otherwise the taskbar could drop behind it).
    SetPropW(g.overlay, L"NonRudeHWND", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(TRUE)));

    g.world.Init(GetDpiForSystem() / 96.f, g.width, g.height);
    if (!g.renderer.Init(g.overlay, g.width, g.height)) {
        MessageBoxW(nullptr, L"Не удалось включить графику (Direct3D 11 / DirectComposition).", L"Сугроб",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    if (wcsstr(cmdline, L"--snapshot")) {
        // Test aid: 90 s of snowfall on fake windows (with a mitten sweep and a fast window drag),
        // rendered offscreen to %TEMP%\sugrob_snapshot.bmp. Nothing is shown on screen.
        Scene sc;
        const int W = g.width, H = g.height;
        sc.monitors.push_back(RECT{0, 0, W, H});
        auto addWin = [&](int l, int t, int r, int b, bool taskbar) {
            sc.windows.push_back(WinInfo{reinterpret_cast<HWND>((INT_PTR)sc.windows.size() + 1), RECT{l, t, r, b}, taskbar});
        };
        addWin(0, H - 40, W, H, true);                                  // taskbar (topmost)
        addWin(W * 40 / 100, H * 48 / 100, W * 81 / 100, H * 89 / 100, false);  // front window
        addWin(W * 9 / 100, H * 28 / 100, W * 51 / 100, H * 76 / 100, false);   // back window
        addWin(W * 62 / 100, H * 17 / 100, W * 88 / 100, H * 39 / 100, false);  // small window
        Settings st = g.settings;
        st.accumulation = 8.f;
        const float dt = 1.f / 60.f;
        double t = 0.0;
        const float fx = sc.windows[1].r.left + (sc.windows[1].r.right - sc.windows[1].r.left) * 0.66f;
        const float ftop = (float)sc.windows[1].r.top;
        for (int i = 0; i < 90 * 60; ++i) {
            t += dt;
            // A snowman: three balls dropped onto the front window, one after another.
            if (i == 84 * 60) g.world.DebugDropBall(fx, ftop - 140.f, 24.f);
            if (i == 85 * 60) g.world.DebugDropBall(fx + 3.f, ftop - 230.f, 17.f);
            if (i == 86 * 60) g.world.DebugDropBall(fx - 2.f, ftop - 300.f, 12.f);
            // At 89.3 s the small window is closed: its snow should crumble away gradually, not drop at once.
            if (i == (int)(89.3 * 60)) sc.windows.erase(sc.windows.begin() + 3);
            if (i == (int)(89.6 * 60)) {  // second picture, 0.3 s after the close
                wchar_t pa[MAX_PATH];
                GetTempPathW(MAX_PATH, pa);
                wcscat_s(pa, MAX_PATH, L"sugrob_snapshot_a.bmp");
                g.renderer.Snapshot(g.world, sc, pa);
            }
            if (i % 2 == 0) g.world.ApplyScene(sc, t);
            POINT cur{-10000, -10000};
            const RECT& back = sc.windows[2].r;
            if (t > 89.5 && t < 89.85) {  // the mitten sweeps along the back window's top
                const double k = (t - 89.5) / 0.35;
                cur = POINT{(LONG)(back.left + 20 + k * (back.right - back.left) * 0.45), back.top - 6};
            }
            g.world.Update(dt, st, cur);
        }
        {   // Grab self-test on the front window's pile: where does a press grab, and does a drag roll a ball?
            const RECT fw = sc.windows[1].r;
            LONG gx = fw.left + (fw.right - fw.left) * 3 / 4;
            float pileMax = 0.f;
            for (const Ledge& l : g.world.Ledges()) {  // test at the tallest spot of this window's pile
                if (l.dying || l.floor || l.y != fw.top || l.vis.size() != l.h.size()) continue;
                for (size_t k = 0; k < l.h.size(); ++k) {
                    const float hv = l.vis[k] ? std::min(l.h[k], l.cap[k]) : 0.f;
                    if (hv > pileMax) {
                        pileMax = hv;
                        gx = l.left + (LONG)((k + 0.5f) * g.world.CellW());
                    }
                }
            }
            char log[1024];
            int len = snprintf(log, sizeof(log), "pile max height %.1f px at x=%ld\r\npress this many px above the window top -> grabbed? ",
                               pileMax, (long)gx);
            for (int off = 20; off >= -4; off -= 4) {
                World probe = g.world;  // a copy: the test must not change the real scene
                const bool ok = probe.TryGrab(POINT{gx, fw.top - off});
                len += snprintf(log + len, sizeof(log) - (size_t)len, "[%d:%s] ", off, ok ? "yes" : "no");
            }
            const POINT start{gx, fw.top - 3};
            const bool grabbed = g.world.TryGrab(start);
            for (int k = 0; k < 100; ++k) g.world.Update(dt, st, POINT{start.x - k * 3, start.y});
            const bool held = g.world.Holding();
            g.world.Release();
            for (int k = 0; k < 90; ++k) g.world.Update(dt, st, POINT{start.x - 300, start.y});
            float maxR = 0.f;
            for (const Ball& b : g.world.Balls()) maxR = std::max(maxR, b.r);
            len += snprintf(log + len, sizeof(log) - (size_t)len,
                            "\r\ndrag test: grabbed=%d held_during_drag=%d balls_after=%zu biggest_radius=%.1f\r\n", grabbed, held,
                            g.world.Balls().size(), maxR);
            wchar_t lp[MAX_PATH];
            GetTempPathW(MAX_PATH, lp);
            wcscat_s(lp, MAX_PATH, L"sugrob_grab.txt");
            HANDLE lf = CreateFileW(lp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lf != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(lf, log, (DWORD)len, &wr, nullptr);
                CloseHandle(lf);
            }
        }
        {   // Ball self-test: balls are sticky (no rolling), never lost, stay on / ride along with their window.
            char lg[12288];
            int n = 0;
            auto stateName = [](const Ball& b) {
                switch (b.state) {
                    case Ball::State::Free: return "Free";
                    case Ball::State::OnLedge: return "OnLedge";
                    case Ball::State::OnBall: return "OnBall";
                    default: return "Held";
                }
            };
            auto run = [&](World& w, Scene& s, double& tt, int frames, POINT cur) {
                for (int k = 0; k < frames; ++k) {
                    tt += dt;
                    if (k % 2 == 0) w.ApplyScene(s, tt);
                    w.Update(dt, st, cur);
                }
            };
            const POINT away{-10000, -10000};
            const RECT fw0 = sc.windows[1].r;
            const float cx = fw0.left + (fw0.right - fw0.left) * 0.5f;

            {   // 1) set down after a slow drag: must stay put
                World w = g.world;
                Scene s = sc;
                double tt = t;
                w.DebugDropBall(cx, (float)fw0.top - 120.f, 20.f);
                run(w, s, tt, 120, away);
                const Ball b0 = w.Balls().back();
                const bool gr = w.TryGrab(POINT{(LONG)b0.x, (LONG)b0.y});
                for (int k = 0; k < 60; ++k) run(w, s, tt, 1, POINT{(LONG)b0.x + k * 2, (LONG)b0.y});
                w.Release();
                const float x1 = w.Balls().back().x;
                run(w, s, tt, 180, away);
                const Ball& b = w.Balls().back();
                n += snprintf(lg + n, sizeof(lg) - (size_t)n, "1 slow drag+release: grabbed=%d state=%s moved_after_release=%.2f px\r\n", gr,
                              stateName(b), b.x - x1);
            }
            {   // 2) thrown fast: lands and stays where it landed
                World w = g.world;
                Scene s = sc;
                double tt = t;
                w.DebugDropBall(cx, (float)fw0.top - 120.f, 20.f);
                run(w, s, tt, 120, away);
                const Ball b0 = w.Balls().back();
                const bool gr = w.TryGrab(POINT{(LONG)b0.x, (LONG)b0.y});
                for (int k = 0; k < 14; ++k) run(w, s, tt, 1, POINT{(LONG)b0.x + k * 22, (LONG)b0.y - k * 4});
                w.Release();
                run(w, s, tt, 120, away);
                const float x1 = w.Balls().empty() ? 0.f : w.Balls().back().x;
                run(w, s, tt, 120, away);
                if (w.Balls().empty()) {
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "2 throw: ball burst or lost (balls=0)\r\n");
                } else {
                    const Ball& b = w.Balls().back();
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "2 throw: grabbed=%d state=%s x=%.0f moved_in_last_2s=%.2f px y+r=%.0f (screen %d)\r\n", gr,
                                  stateName(b), b.x, b.x - x1, b.y + b.r, H);
                }
            }
            {   // 3) let go below every edge (behind the taskbar): must not leave the screen
                World w = g.world;
                Scene s = sc;
                double tt = t;
                w.DebugDropBall(cx, (float)H - 15.f, 15.f);
                run(w, s, tt, 240, away);
                if (w.Balls().empty()) {
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "3 below taskbar: ball LOST\r\n");
                } else {
                    const Ball& b = w.Balls().back();
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "3 below taskbar: balls=%zu state=%s y+r=%.0f screen=%d\r\n", w.Balls().size(),
                                  stateName(b), b.y + b.r, H);
                }
            }
            {   // 4) window shrinks under the ball, then closes: ball stays on it, then falls to the next edge, not off screen
                World w = g.world;
                Scene s = sc;
                double tt = t;
                const float bx = fw0.left + (fw0.right - fw0.left) * 0.9f;
                w.DebugDropBall(bx, (float)fw0.top - 120.f, 20.f);
                run(w, s, tt, 120, away);
                s.windows[1].r.right = fw0.left + (fw0.right - fw0.left) / 2;
                run(w, s, tt, 120, away);
                const Ball b1 = w.Balls().back();
                n += snprintf(lg + n, sizeof(lg) - (size_t)n, "4a shrink: state=%s x=%.0f window=[%ld..%ld]\r\n", stateName(b1), b1.x, (long)s.windows[1].r.left,
                              (long)s.windows[1].r.right);
                s.windows.erase(s.windows.begin() + 1);
                run(w, s, tt, 300, away);
                if (w.Balls().empty()) {
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "4b closed: ball LOST\r\n");
                } else {
                    const Ball& b = w.Balls().back();
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "4b closed: balls=%zu state=%s y+r=%.0f screen=%d\r\n", w.Balls().size(), stateName(b),
                                  b.y + b.r, H);
                }
            }
            {   // 5) window dragged at a normal speed: the ball rides along with it
                World w = g.world;
                Scene s = sc;
                double tt = t;
                w.DebugDropBall(cx, (float)fw0.top - 120.f, 20.f);
                run(w, s, tt, 120, away);
                const float rel0 = w.Balls().back().x - (float)s.windows[1].r.left;
                for (int k = 0; k < 40; ++k) {
                    s.windows[1].r.left += 6;
                    s.windows[1].r.right += 6;
                    run(w, s, tt, 2, away);
                }
                run(w, s, tt, 60, away);
                const Ball& b = w.Balls().back();
                n += snprintf(lg + n, sizeof(lg) - (size_t)n, "5 window drag 360px/s: state=%s ball_x_rel_to_window before=%.0f after=%.0f\r\n", stateName(b), rel0,
                              b.x - (float)s.windows[1].r.left);
            }
            {   // 6) the cursor moving past / over a resting snowman must not make it sink (it used to dig out the snow under it)
                World w = g.world;
                Scene s = sc;
                double tt = t;
                w.DebugDropBall(cx, (float)fw0.top - 120.f, 20.f);
                run(w, s, tt, 120, away);
                const Ball b0 = w.Balls().back();
                w.ResetInput();
                for (int k = 0; k < 120; ++k) run(w, s, tt, 1, POINT{(LONG)(b0.x - 60.f + (float)k * 1.5f), (LONG)(b0.y + b0.r * 0.7f)});
                run(w, s, tt, 30, away);
                const Ball& b = w.Balls().back();
                n += snprintf(lg + n, sizeof(lg) - (size_t)n, "6 cursor sweeping past/over a snowman: y before=%.2f after=%.2f (%s)\r\n", b0.y, b.y, stateName(b));
            }
            {   // 7) a sideways swipe along a pile pushes snow ahead as a heap; thrown lumps move together
                World w = g.world;
                Scene s = sc;
                double tt = t;
                run(w, s, tt, 60, away);
                const float wd = (float)(fw0.right - fw0.left);
                const float xs = fw0.left + 0.15f * wd, xe = fw0.left + 0.45f * wd;
                auto pile = [&](const World& ww, float x0, float x1, float& area) {
                    float m = 0.f;
                    area = 0.f;
                    for (const Ledge& l : ww.Ledges()) {
                        if (l.dying || l.floor || l.y != fw0.top || l.vis.size() != l.h.size()) continue;
                        for (size_t k = 0; k < l.h.size(); ++k) {
                            const float x = (float)l.left + ((float)k + 0.5f) * (float)ww.CellW();
                            if (x < x0 || x > x1 || !l.vis[k]) continue;
                            const float hv = std::min(l.h[k], l.cap[k]);
                            m = std::max(m, hv);
                            area += hv * (float)ww.CellW();
                        }
                    }
                    return m;
                };
                w.ResetInput();  // put the cursor at the start of the swipe without a jump from nowhere
                for (int k = 0; k < 40; ++k) run(w, s, tt, 1, POINT{(LONG)xs, (LONG)((float)fw0.top - 4.f)});
                float areaB0 = 0.f, areaB1 = 0.f, areaA0 = 0.f, areaA1 = 0.f;
                const float heapBefore = pile(w, xe, xe + 50.f, areaB0);
                pile(w, xs, xe, areaB1);
                double sumVx = 0.0, sumVx2 = 0.0;
                int nClump = 0;
                for (int k = 0; k <= 30; ++k) {
                    run(w, s, tt, 1, POINT{(LONG)(xs + (xe - xs) * (float)k / 30.f), (LONG)((float)fw0.top - 4.f)});
                    if (k == 24)
                        for (const Flake& f : w.Flakes())
                            if (f.clump) {
                                ++nClump;
                                sumVx += f.vx;
                                sumVx2 += (double)f.vx * f.vx;
                            }
                }
                run(w, s, tt, 6, away);
                const float heapAfter = pile(w, xe, xe + 50.f, areaA0);
                pile(w, xs, xe, areaA1);
                const double mean = nClump ? sumVx / nClump : 0.0;
                const double sd = nClump ? std::sqrt(std::max(0.0, sumVx2 / nClump - mean * mean)) : 0.0;
                n += snprintf(lg + n, sizeof(lg) - (size_t)n,
                              "7 sideways swipe %.0f->%.0f px: swept zone area %.0f -> %.0f, zone ahead area %.0f -> %.0f, ahead max height %.1f -> %.1f; "
                              "airborne lumps=%d vx mean=%.0f sd=%.0f px/s\r\n",
                              xs, xe, areaB1, areaA1, areaB0, areaA0, heapBefore, heapAfter, nClump, mean, sd);
            }
            auto edgeArea = [&](const World& ww, LONG top, float x0, float x1) {
                float area = 0.f;
                for (const Ledge& l : ww.Ledges()) {
                    if (l.dying || l.floor || l.y != top || l.vis.size() != l.h.size()) continue;
                    for (size_t k = 0; k < l.h.size(); ++k) {
                        const float x = (float)l.left + ((float)k + 0.5f) * (float)ww.CellW();
                        if (x >= x0 && x < x1 && l.vis[k]) area += std::min(l.h[k], l.cap[k]) * (float)ww.CellW();
                    }
                }
                return area;
            };
            auto snap = [&](const World& ww, const Scene& ss, const wchar_t* name) {
                wchar_t sp[MAX_PATH];
                GetTempPathW(MAX_PATH, sp);
                wcscat_s(sp, MAX_PATH, name);
                g.renderer.Snapshot(ww, ss, sp);
            };
            {   // 8) the window is made narrower (right border dragged in, then the left one): only the cut-off snow falls
                World w = g.world;
                Scene s = sc;
                double tt = t;
                run(w, s, tt, 30, away);
                const RECT r0 = s.windows[2].r;  // the back window: wide and fully snowed
                const float keep0 = (float)r0.left + 0.25f * (float)(r0.right - r0.left);
                const float keep1 = (float)r0.left + 0.55f * (float)(r0.right - r0.left);
                const float total0 = edgeArea(w, r0.top, -1e9f, 1e9f);
                const float mid0 = edgeArea(w, r0.top, keep0, keep1);
                size_t lumps0 = 0;
                for (const Flake& f : w.Flakes()) lumps0 += f.clump ? 1 : 0;
                // Drag the right border in by 4 px per scan (a normal speed), 160 px in total.
                for (int k = 0; k < 40; ++k) {
                    s.windows[2].r.right -= 4;
                    run(w, s, tt, 2, away);
                }
                size_t lumps1 = 0;
                for (const Flake& f : w.Flakes()) lumps1 += f.clump ? 1 : 0;
                snap(w, s, L"sugrob_snapshot_resize.bmp");
                const float mid1 = edgeArea(w, r0.top, keep0, keep1);
                const float total1 = edgeArea(w, r0.top, -1e9f, 1e9f);
                // Then one big jump of the left border (a slab falls off and crumbles).
                s.windows[2].r.left += 150;
                run(w, s, tt, 2, away);
                int ghosts = 0;
                for (const Ledge& l : w.Ledges()) ghosts += l.dying ? 1 : 0;
                const float mid2 = edgeArea(w, r0.top, keep0, keep1);
                run(w, s, tt, 120, away);
                const float mid3 = edgeArea(w, r0.top, keep0, keep1);
                n += snprintf(lg + n, sizeof(lg) - (size_t)n,
                              "8 resize: edge snow total %.0f -> %.0f after right border -160px; middle area %.0f -> %.0f (kept), "
                              "airborne lumps %zu -> %zu; left border +150px: ghost slabs=%d middle %.0f, 2 s later middle %.0f\r\n",
                              total0, total1, mid0, mid1, lumps0, lumps1, ghosts, mid2, mid3);
            }
            {   // 9) sleet: raindrops fall, splash and slowly melt the piles; snow keeps falling too
                World w = g.world;
                Scene s = sc;
                double tt = t;
                Settings wet = st;
                wet.intensity = Intensity::Sleet;
                const RECT r0 = s.windows[2].r;
                const float a0 = edgeArea(w, r0.top, -1e9f, 1e9f);
                int drops = 0, flakesN = 0, splashes = 0;
                for (int k = 0; k < 600; ++k) {
                    tt += dt;
                    if (k % 2 == 0) w.ApplyScene(s, tt);
                    w.Update(dt, wet, away);
                    if (k == 599)
                        for (const Flake& f : w.Flakes()) {
                            if (f.rain && f.life > 0.f) ++splashes;
                            else if (f.rain) ++drops;
                            else if (!f.clump) ++flakesN;
                        }
                }
                snap(w, s, L"sugrob_snapshot_sleet.bmp");
                const float a1 = edgeArea(w, r0.top, -1e9f, 1e9f);
                n += snprintf(lg + n, sizeof(lg) - (size_t)n,
                              "9 sleet 10 s: raindrops=%d splashes=%d snowflakes=%d; back window pile area %.0f -> %.0f\r\n", drops, splashes,
                              flakesN, a0, a1);
            }
            {   // 10) no size limit: roll a ball along the floor pile, back and forth
                World w = g.world;
                Scene s = sc;
                double tt = t;
                float fy = (float)H;
                for (const Ledge& l : w.Ledges())
                    if (l.floor) fy = (float)l.y;
                // The floor at the bottom is behind the taskbar here: use the taskbar's top edge instead.
                const float y = (float)s.windows[0].r.top - 4.f;
                LONG x0 = W / 10;
                float best = 0.f;
                for (const Ledge& l : w.Ledges()) {  // start where the taskbar's pile is deepest (left part)
                    if (l.dying || l.floor || l.y != s.windows[0].r.top || l.vis.size() != l.h.size()) continue;
                    for (size_t k = 0; k < l.h.size(); ++k) {
                        const float x = (float)l.left + ((float)k + 0.5f) * (float)w.CellW();
                        if (x > (float)W * 0.3f || !l.vis[k]) continue;
                        if (std::min(l.h[k], l.cap[k]) > best) {
                            best = std::min(l.h[k], l.cap[k]);
                            x0 = (LONG)x;
                        }
                    }
                }
                w.ResetInput();
                for (int k = 0; k < 10; ++k) run(w, s, tt, 1, POINT{x0, (LONG)y});
                const float before = edgeArea(w, s.windows[0].r.top, -1e9f, 1e9f);
                n += snprintf(lg + n, sizeof(lg) - (size_t)n, "10 snow on the taskbar edge before rolling: %.0f px^2\r\n", before);
                const bool gr = w.TryGrab(POINT{x0, (LONG)y});
                float maxR = 0.f;
                const uint32_t heldId = gr && !w.Balls().empty() ? w.Balls().back().id : 0;
                for (int pass = 0; pass < 6 && heldId; ++pass)
                    for (int k = 0; k < 240; ++k) {
                        const float u = (float)k / 239.f;
                        const float span = (float)W * 0.95f - (float)x0;
                        const float px = (pass % 2 == 0) ? (float)x0 + u * span : (float)x0 + (1.f - u) * span;
                        run(w, s, tt, 1, POINT{(LONG)px, (LONG)y});  // the mouse is held along the edge, like a user would
                        for (const Ball& b : w.Balls())
                            if (b.id == heldId) maxR = std::max(maxR, b.r);
                    }
                const float left = edgeArea(w, s.windows[0].r.top, -1e9f, 1e9f);
                w.Release();
                run(w, s, tt, 60, away);
                snap(w, s, L"sugrob_snapshot_bigball.bmp");
                n += snprintf(lg + n, sizeof(lg) - (size_t)n,
                              "10 big ball: grabbed=%d biggest radius %.1f px (old limit %.0f px), snow left on that edge %.0f px^2 "
                              "(snow area of the ball %.0f), floor y=%.0f\r\n",
                              gr, maxR, 46.f * w.Scale(), left, 3.14159f * maxR * maxR, fy);
            }
            auto allSnow = [&](const World& ww) {  // snow lying anywhere + lumps in the air
                float a = 0.f;
                for (const Ledge& l : ww.Ledges()) {
                    if (l.dying || l.vis.size() != l.h.size()) continue;
                    for (size_t k = 0; k < l.h.size(); ++k)
                        if (l.vis[k]) a += std::min(l.h[k], l.cap[k]) * (float)ww.CellW();
                }
                for (const Flake& f : ww.Flakes())
                    if (f.clump) a += f.vol;
                return a;
            };
            {   // 11) a big snowball smashed onto a window: its snow must stay (a heap), not vanish into crumbs
                World w = g.world;
                Scene s = sc;
                double tt = t;
                Settings calm = st;
                calm.intensity = Intensity::Light;
                const RECT fr = s.windows[1].r;
                const float bx = 1080.f * (float)W / 1920.f;  // over the front window, clear of the others
                const float front0 = edgeArea(w, fr.top, -1e9f, 1e9f), all0 = allSnow(w);
                w.DebugDropBall(bx, -250.f, 40.f);
                const float ballVol = w.Balls().back().vol;
                const size_t balls0 = w.Balls().size();
                for (int k = 0; k < 240; ++k) {
                    tt += dt;
                    if (k % 2 == 0) w.ApplyScene(s, tt);
                    w.Update(dt, calm, away);
                }
                const float front1 = edgeArea(w, fr.top, -1e9f, 1e9f), all1 = allSnow(w);
                float peak = 0.f;
                for (const Ledge& l : w.Ledges())
                    if (!l.dying && l.y == fr.top && l.vis.size() == l.h.size())
                        for (size_t k = 0; k < l.h.size(); ++k)
                            if (l.vis[k]) peak = std::max(peak, std::min(l.h[k], l.cap[k]));
                snap(w, s, L"sugrob_snapshot_smash.bmp");
                n += snprintf(lg + n, sizeof(lg) - (size_t)n,
                              "11 smashed ball (snow %.0f px^2): broke=%d; front window snow %.0f -> %.0f (+%.0f), all snow +%.0f "
                              "(%.0f%% of the ball kept), highest point of the heap %.1f px\r\n",
                              ballVol, w.Balls().size() < balls0 ? 1 : 0, front0, front1, front1 - front0, all1 - all0,
                              100.f * (all1 - all0) / ballVol, peak);
            }
            {   // 12) heavy snow: how much is in the air and how fast it piles up, compared with ordinary snow
                for (int mode = 0; mode < 2; ++mode) {
                    World w = g.world;
                    Scene s = sc;
                    double tt = t;
                    Settings ws = st;
                    ApplyWeather(ws, mode == 0 ? Intensity::Normal : Intensity::Heavy);
                    w.ClearSnow();
                    for (int k = 0; k < 1500; ++k) {
                        tt += dt;
                        if (k % 2 == 0) w.ApplyScene(s, tt);
                        w.Update(dt, ws, away);
                    }
                    int flakesN = 0;
                    for (const Flake& f : w.Flakes()) flakesN += f.clump ? 0 : 1;
                    const RECT br = s.windows[2].r;
                    const float a = edgeArea(w, br.top, -1e9f, 1e9f) / (float)(br.right - br.left);
                    if (mode == 1) snap(w, s, L"sugrob_snapshot_heavy.bmp");
                    n += snprintf(lg + n, sizeof(lg) - (size_t)n, "12 %s, 25 s from clean: flakes in the air=%d, back window pile mean depth %.1f px\r\n",
                                  mode == 0 ? "snow" : "heavy snow", flakesN, a);
                }
            }
            wchar_t bp[MAX_PATH];
            GetTempPathW(MAX_PATH, bp);
            wcscat_s(bp, MAX_PATH, L"sugrob_balls.txt");
            HANDLE bf = CreateFileW(bp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (bf != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(bf, lg, (DWORD)n, &wr, nullptr);
                CloseHandle(bf);
            }
        }
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, MAX_PATH, L"sugrob_snapshot.bmp");
        const bool ok = g.renderer.Snapshot(g.world, sc, path);
        g.renderer.Shutdown();
        DestroyWindow(g.overlay);
        DestroyWindow(g.tray);
        return ok ? 0 : 2;
    }

    ShowWindow(g.overlay, SW_SHOWNOACTIVATE);

    g.icon = CreateSnowflakeIcon(GetSystemMetrics(SM_CXSMICON));
    AddTrayIcon();
    InstallHooks();
    Panel::Init(inst, &g.settings, OnPanelAction);

    // Frames are paced by a high-resolution timer at 60 fps; between frames the thread sleeps
    // but still handles window messages.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const double kFrame = 1.0 / 60.0;

    double last = Now(), nextFrame = last, lastScan = 0.0, lastFsCheck = -1.0, lastTopmost = 0.0;
    const double startTime = last;
    bool fullscreen = false, hidden = false;
    MSG msg;
    while (g.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running) break;

        const double now = Now();
        if (bench && now - startTime > 8.0) break;
        if (!bench && now - lastFsCheck > 0.5) {
            lastFsCheck = now;
            fullscreen = IsFullscreenAppActive(g.overlay);
        }

        SetMouseHook(!bench && !g.settings.paused && !fullscreen);  // no hook at all while gaming
        if (g.settings.paused || fullscreen) {
            if (!hidden) {
                g.renderer.Draw(nullptr);  // show nothing, keep the piles in memory
                hidden = true;
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            last = nextFrame = Now();
            continue;
        }

        if (now < nextFrame) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)((nextFrame - now) * 1e7));
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                MsgWaitForMultipleObjects(1, &timer, FALSE, 50, QS_ALLINPUT);
            else
                Sleep(1);
            continue;
        }
        nextFrame = std::max(nextFrame + kFrame, now + kFrame * 0.5);
        const float dt = (float)std::min(now - last, 0.05);
        last = now;

        if (hidden) {
            hidden = false;
            g.sceneDirty = true;
            g.world.ResetInput();
        }

        // Rescan windows when something changed (at most ~30 times a second), and twice a second anyway.
        if ((g.sceneDirty && now - lastScan > 0.03) || now - lastScan > 0.5) {
            g.sceneDirty = false;
            lastScan = now;
            ScanScene(g.overlay, g.tray, g.origin, g.scene);
            g.world.ApplyScene(g.scene, now);
            ++stats.scans;
        }

        POINT cur{};
        GetCursorPos(&cur);
        cur.x -= g.origin.x;
        cur.y -= g.origin.y;
        const double t1 = Now();
        g.world.Update(dt, g.settings, cur);
        const double t2 = Now();
        if (!g.renderer.Draw(&g.world)) Sleep(100);  // device lost: try again shortly
        const double t3 = Now();
        stats.Frame(t2 - t1, t3 - t2, t3, g.world.Flakes().size(), g.world.Ledges().size(), g.renderer.UsesSpriteBatch());

        // Stay above windows that also want to be on top (but under the weather panel while it is open).
        if (now - lastTopmost > 2.0 && !Panel::Visible()) {
            lastTopmost = now;
            SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
    if (timer) CloseHandle(timer);
    SetMouseHook(false);

    for (HWINEVENTHOOK hk : g.hooks) UnhookWinEvent(hk);
    Panel::Shutdown();
    RemoveTrayIcon();
    g.renderer.Shutdown();
    DestroyWindow(g.overlay);
    DestroyWindow(g.tray);
    if (g.icon) DestroyIcon(g.icon);
    if (mutex) CloseHandle(mutex);
    return 0;
}
