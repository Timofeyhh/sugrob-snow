#include "panel.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Layout, in dip.
constexpr float kW = 316.f;
constexpr float kPad = 14.f;
constexpr float kGap = 8.f;
constexpr float kTileY = 70.f, kTileH = 80.f;
constexpr float kWindLabelY = kTileY + 2.f * kTileH + kGap + 14.f;
constexpr float kWindY = kWindLabelY + 22.f, kWindH = 34.f;
constexpr float kDirY = kWindY + kWindH + 6.f, kDirH = 30.f;
constexpr float kButtonsY = kDirY + kDirH + 14.f, kButtonH = 34.f;
constexpr float kH = kButtonsY + kButtonH + kPad;

// Colours: a dark, frosty flyout.
constexpr uint32_t kBg = 0x18202C, kTile = 0x222D3D, kTileHover = 0x2B394D, kTileOn = 0x24496E;
constexpr uint32_t kAccent = 0x7CC4FF, kText = 0xF2F6FA, kMuted = 0x93A4B8, kGust = 0xB9C8DA;

const wchar_t* const kNames[kWeatherCount] = {L"Слабый снег", L"Снег", L"Сильный снег", L"Метель", L"Буран", L"Снег с дождём"};
const wchar_t* const kPowerNames[kWindPowers] = {L"Нет", L"Слабый", L"Сильный", L"Ураган"};
const wchar_t* const kDirNames[2] = {L"← Влево", L"Вправо →"};

struct Hit {
    enum Kind { None, Close, Weather, Wind, Dir, Pause, Clear } kind = None;
    int index = 0;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
};

struct State {
    HWND hwnd = nullptr;
    Settings* settings = nullptr;
    void (*action)(PanelAction) = nullptr;
    float s = 1.f;  // DPI scale
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1StrokeStyle* round = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* label = nullptr;
    IDWriteTextFormat* section = nullptr;
    IDWriteTextFormat* button = nullptr;
    float fontScale = 0.f;
    Hit hover;
    bool tracking = false;
    DWORD hiddenAt = 0;
};
State P;

D2D1_COLOR_F Rgb(uint32_t c, float a = 1.f) {
    return D2D1_COLOR_F{((c >> 16) & 255u) / 255.f, ((c >> 8) & 255u) / 255.f, (c & 255u) / 255.f, a};
}
D2D1_RECT_F Rc(float x, float y, float w, float h) { return D2D1_RECT_F{x * P.s, y * P.s, (x + w) * P.s, (y + h) * P.s}; }
D2D1_POINT_2F Pt(float x, float y) { return D2D1_POINT_2F{x, y}; }
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

D2D1_RECT_F CloseRect() { return Rc(kW - kPad - 28.f, 10.f, 28.f, 28.f); }
D2D1_RECT_F TileRect(int i) {
    const float tw = (kW - 2.f * kPad - 2.f * kGap) / 3.f;
    return Rc(kPad + (float)(i % 3) * (tw + kGap), kTileY + (float)(i / 3) * (kTileH + kGap), tw, kTileH);
}
D2D1_RECT_F WindRect(int i) {
    const float sw = (kW - 2.f * kPad) / (float)kWindPowers;
    return Rc(kPad + (float)i * sw, kWindY, sw, kWindH);
}
D2D1_RECT_F DirRect(int i) {
    const float sw = (kW - 2.f * kPad) / 2.f;
    return Rc(kPad + (float)i * sw, kDirY, sw, kDirH);
}
D2D1_RECT_F ButtonRect(int i) {
    const float bw = (kW - 2.f * kPad - kGap) / 2.f;
    return Rc(kPad + (float)i * (bw + kGap), kButtonsY, bw, kButtonH);
}

Hit HitTest(float x, float y) {
    Hit h;
    if (Inside(CloseRect(), x, y)) h.kind = Hit::Close;
    for (int i = 0; i < kWeatherCount; ++i)
        if (Inside(TileRect(i), x, y)) h = Hit{Hit::Weather, i};
    for (int i = 0; i < kWindPowers; ++i)
        if (Inside(WindRect(i), x, y)) h = Hit{Hit::Wind, i};
    for (int i = 0; i < 2; ++i)
        if (Inside(DirRect(i), x, y)) h = Hit{Hit::Dir, i};
    if (Inside(ButtonRect(0), x, y)) h.kind = Hit::Pause;
    if (Inside(ButtonRect(1), x, y)) h.kind = Hit::Clear;
    return h;
}

IDWriteTextFormat* MakeFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* f = nullptr;
    if (FAILED(P.dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      size * P.s, L"ru-ru", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return f;
}

bool EnsureResources() {
    if (!P.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                           reinterpret_cast<void**>(&P.d2d))))
        return false;
    if (!P.dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown**>(&P.dw))))
        return false;
    if (!P.round) {
        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        sp.miterLimit = 10.f;
        sp.dashStyle = D2D1_DASH_STYLE_SOLID;
        P.d2d->CreateStrokeStyle(&sp, nullptr, 0, &P.round);
    }
    if (P.fontScale != P.s) {
        SafeRelease(P.title);
        SafeRelease(P.label);
        SafeRelease(P.section);
        SafeRelease(P.button);
        P.title = MakeFormat(17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        P.label = MakeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        P.section = MakeFormat(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        P.button = MakeFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        P.fontScale = P.s;
    }
    if (!P.title || !P.label || !P.section || !P.button) return false;
    if (!P.rt) {
        RECT rc;
        GetClientRect(P.hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp{};
        rp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        rp.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
        rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;
        rp.dpiX = rp.dpiY = 96.f;  // we work in pixels and scale ourselves
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp{};
        hp.hwnd = P.hwnd;
        hp.pixelSize = D2D1_SIZE_U{(UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)};
        hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
        if (FAILED(P.d2d->CreateHwndRenderTarget(&rp, &hp, &P.rt))) return false;
        const D2D1_COLOR_F c = Rgb(kText);
        if (FAILED(P.rt->CreateSolidColorBrush(&c, nullptr, &P.brush))) {
            SafeRelease(P.rt);
            return false;
        }
    }
    return true;
}

void FillRound(const D2D1_RECT_F& r, float radius, uint32_t color, float a = 1.f) {
    P.brush->SetColor(Rgb(color, a));
    const D2D1_ROUNDED_RECT rr{r, radius * P.s, radius * P.s};
    P.rt->FillRoundedRectangle(&rr, P.brush);
}
void StrokeRound(const D2D1_RECT_F& r, float radius, uint32_t color, float width) {
    P.brush->SetColor(Rgb(color));
    const float h = width * P.s * 0.5f;
    const D2D1_ROUNDED_RECT rr{D2D1_RECT_F{r.left + h, r.top + h, r.right - h, r.bottom - h}, radius * P.s, radius * P.s};
    P.rt->DrawRoundedRectangle(&rr, P.brush, width * P.s, nullptr);
}
void Text(const wchar_t* s, IDWriteTextFormat* f, const D2D1_RECT_F& r, uint32_t color) {
    P.brush->SetColor(Rgb(color));
    P.rt->DrawText(s, (UINT32)wcslen(s), f, &r, P.brush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}
void Dot(float x, float y, float r, uint32_t color, float a = 1.f) {
    P.brush->SetColor(Rgb(color, a));
    const D2D1_ELLIPSE e{Pt(x, y), r, r};
    P.rt->FillEllipse(&e, P.brush);
}
void Line(float x0, float y0, float x1, float y1, uint32_t color, float w, float a = 1.f) {
    P.brush->SetColor(Rgb(color, a));
    P.rt->DrawLine(Pt(x0, y0), Pt(x1, y1), P.brush, w, P.round);
}
void Gust(float x, float y, float w, float s) {  // a curl of wind
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* k = nullptr;
    if (SUCCEEDED(P.d2d->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&k))) {
        k->BeginFigure(Pt(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        const D2D1_BEZIER_SEGMENT b1{Pt(x + w * 0.3f, y - 3.f * s), Pt(x + w * 0.6f, y + 3.f * s), Pt(x + w * 0.85f, y)};
        k->AddBezier(&b1);
        const D2D1_BEZIER_SEGMENT b2{Pt(x + w * 1.0f, y - 2.f * s), Pt(x + w * 0.95f, y - 6.f * s), Pt(x + w * 0.82f, y - 5.f * s)};
        k->AddBezier(&b2);
        k->EndFigure(D2D1_FIGURE_END_OPEN);
        k->Close();
        P.brush->SetColor(Rgb(kGust, 0.9f));
        P.rt->DrawGeometry(g, P.brush, 1.6f * s, P.round);
    }
    SafeRelease(k);
    SafeRelease(g);
}

// A little picture for each kind of weather.
void Icon(int w, float cx, float top) {
    static const float pts[12][2] = {{0.16f, 0.30f}, {0.50f, 0.14f}, {0.84f, 0.36f}, {0.32f, 0.70f}, {0.68f, 0.78f}, {0.06f, 0.86f},
                                     {0.94f, 0.88f}, {0.48f, 0.46f}, {0.26f, 0.04f}, {0.76f, 0.06f}, {0.60f, 0.58f}, {0.10f, 0.56f}};
    const float s = P.s, bw = 46.f * s, bh = 28.f * s;
    const float x0 = cx - bw * 0.5f, y0 = top;
    int n = 6;
    float rad = 1.8f, slant = 0.f;
    int gusts = 0;
    bool rain = false;
    switch (w) {
        case 0: n = 3; rad = 1.7f; break;
        case 1: n = 6; rad = 1.9f; break;
        case 2: n = 12; rad = 2.3f; break;
        case 3: n = 7; rad = 1.8f; gusts = 2; slant = 0.25f; break;
        case 4: n = 12; rad = 2.0f; gusts = 3; slant = 0.5f; break;
        default: n = 5; rad = 2.0f; rain = true; break;
    }
    for (int i = 0; i < n; ++i) {
        const float px = x0 + (pts[i][0] + slant * (pts[i][1] - 0.5f) * -0.4f) * bw;
        Dot(px, y0 + pts[i][1] * bh, rad * s, kText, 0.95f);
    }
    if (rain) {
        static const float rp[4][2] = {{0.30f, 0.20f}, {0.62f, 0.34f}, {0.92f, 0.12f}, {0.80f, 0.62f}};
        for (const auto& p : rp) {
            const float x = x0 + p[0] * bw, y = y0 + p[1] * bh;
            Line(x, y, x - 3.5f * s, y + 9.f * s, kAccent, 1.6f * s);
        }
    }
    for (int i = 0; i < gusts; ++i) Gust(x0 - 2.f * s, y0 + bh * (0.28f + 0.30f * (float)i), bw * 0.8f, s);
}

void Paint() {
    if (!EnsureResources()) return;
    const float s = P.s;
    P.rt->BeginDraw();
    const D2D1_COLOR_F bg = Rgb(kBg);
    P.rt->Clear(&bg);
    // Header.
    Text(L"Погода", P.title, Rc(kPad + 2.f, 8.f, 200.f, 32.f), kText);
    {
        const D2D1_RECT_F c = CloseRect();
        if (P.hover.kind == Hit::Close) FillRound(c, 6.f, kTileHover);
        const float cx = (c.left + c.right) * 0.5f, cy = (c.top + c.bottom) * 0.5f, d = 5.f * s;
        Line(cx - d, cy - d, cx + d, cy + d, kMuted, 1.5f * s);
        Line(cx - d, cy + d, cx + d, cy - d, kMuted, 1.5f * s);
    }
    // Weather tiles.
    Text(L"Осадки", P.section, Rc(kPad + 2.f, 44.f, 200.f, 20.f), kMuted);
    for (int i = 0; i < kWeatherCount; ++i) {
        const D2D1_RECT_F r = TileRect(i);
        const bool on = (int)P.settings->intensity == i;
        const bool hot = P.hover.kind == Hit::Weather && P.hover.index == i;
        FillRound(r, 8.f, on ? kTileOn : (hot ? kTileHover : kTile));
        if (on) StrokeRound(r, 8.f, kAccent, 1.5f);
        Icon(i, (r.left + r.right) * 0.5f, r.top + 11.f * s);
        Text(kNames[i], P.label, D2D1_RECT_F{r.left + 4.f * s, r.top + 42.f * s, r.right - 4.f * s, r.bottom - 4.f * s}, kText);
    }
    // Wind: how hard (a segmented switch) and which way (dimmed while there is no wind).
    Text(L"Ветер", P.section, Rc(kPad + 2.f, kWindLabelY, 200.f, 20.f), kMuted);
    auto segments = [&](int count, D2D1_RECT_F (*rect)(int), Hit::Kind kind, int selected, bool dim) {
        const D2D1_RECT_F a = rect(0), b = rect(count - 1);
        FillRound(D2D1_RECT_F{a.left, a.top, b.right, b.bottom}, 8.f, kTile);
        for (int i = 0; i < count; ++i) {
            const D2D1_RECT_F r = rect(i);
            const bool on = i == selected;
            const bool hot = P.hover.kind == kind && P.hover.index == i;
            const D2D1_RECT_F in{r.left + 3.f * s, r.top + 3.f * s, r.right - 3.f * s, r.bottom - 3.f * s};
            if (on && !dim) {
                FillRound(in, 6.f, kTileOn);
                StrokeRound(in, 6.f, kAccent, 1.2f);
            } else if (on || hot) {
                FillRound(in, 6.f, kTileHover);
            }
            Text(kind == Hit::Wind ? kPowerNames[i] : kDirNames[i], P.button, r, on && !dim ? kText : kMuted);
        }
    };
    const int power = std::clamp(P.settings->windPower, 0, kWindPowers - 1);
    segments(kWindPowers, WindRect, Hit::Wind, power, false);
    segments(2, DirRect, Hit::Dir, P.settings->wind == WindDir::Left ? 0 : 1, power == 0);
    // Pause / clear.
    {
        const bool paused = P.settings->paused;
        const D2D1_RECT_F pr = ButtonRect(0), cr = ButtonRect(1);
        FillRound(pr, 8.f, P.hover.kind == Hit::Pause ? kTileHover : kTile);
        if (paused) StrokeRound(pr, 8.f, kAccent, 1.2f);
        Text(paused ? L"▶  Продолжить" : L"❚❚  Пауза", P.button, pr, kText);
        FillRound(cr, 8.f, P.hover.kind == Hit::Clear ? kTileHover : kTile);
        Text(L"Убрать весь снег", P.button, cr, kText);
    }
    if (P.rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        SafeRelease(P.brush);
        SafeRelease(P.rt);
    }
}

void Act(const Hit& h) {
    switch (h.kind) {
        case Hit::Close: Panel::Hide(); return;
        case Hit::Weather:
            ApplyWeather(*P.settings, (Intensity)h.index);
            P.action(PanelAction::Changed);
            break;
        case Hit::Wind:
            P.settings->windPower = h.index;
            P.action(PanelAction::Changed);
            break;
        case Hit::Dir:
            P.settings->wind = h.index == 0 ? WindDir::Left : WindDir::Right;
            if (P.settings->windPower == 0) P.settings->windPower = 1;  // picking a direction means "some wind"
            P.action(PanelAction::Changed);
            break;
        case Hit::Pause: P.action(PanelAction::TogglePause); break;
        case Hit::Clear: P.action(PanelAction::Clear); break;
        default: return;
    }
    InvalidateRect(P.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (P.rt) {
                const D2D1_SIZE_U sz{(UINT32)LOWORD(lp), (UINT32)HIWORD(lp)};
                P.rt->Resize(&sz);
            }
            return 0;
        case WM_MOUSEMOVE: {
            const Hit hit = HitTest((float)(short)LOWORD(lp), (float)(short)HIWORD(lp));
            if (!(hit == P.hover)) {
                P.hover = hit;
                InvalidateRect(h, nullptr, FALSE);
            }
            if (!P.tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                P.tracking = TrackMouseEvent(&t) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            P.tracking = false;
            P.hover = Hit{};
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && P.hover.kind != Hit::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            Act(HitTest((float)(short)LOWORD(lp), (float)(short)HIWORD(lp)));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Panel::Hide();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) Panel::Hide();  // closes like a flyout when you click elsewhere
            return 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace Panel {

bool Init(HINSTANCE inst, Settings* settings, void (*onAction)(PanelAction)) {
    P.settings = settings;
    P.action = onAction;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SugrobPanel";
    RegisterClassExW(&wc);
    // A tool window: no taskbar button, and the snow does not pile up on it.
    P.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SugrobPanel", L"Сугроб — погода", WS_POPUP, 0, 0, 10, 10,
                             nullptr, nullptr, inst, nullptr);
    if (!P.hwnd) return false;
    const DWORD round = 2;  // DWMWCP_ROUND: rounded corners on Windows 11 (ignored on Windows 10)
    DwmSetWindowAttribute(P.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    return true;
}

void Toggle(const RECT& icon) {
    if (!P.hwnd) return;
    if (IsWindowVisible(P.hwnd)) {
        Hide();
        return;
    }
    if (GetTickCount() - P.hiddenAt < 350) return;  // this click on the icon is what just closed the panel
    P.s = (float)GetDpiForSystem() / 96.f;
    const int w = (int)std::lround(kW * P.s), h = (int)std::lround(kH * P.s), m = (int)std::lround(10.f * P.s);
    const POINT c{(icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT wa = mi.rcWork;
    int x = c.x - w / 2, y = c.y - h - m;
    if (icon.top >= wa.bottom - 2) y = wa.bottom - h - m;        // taskbar at the bottom
    else if (icon.bottom <= wa.top + 2) y = wa.top + m;          // ... at the top
    else if (icon.left >= wa.right - 2) {                        // ... on the right
        x = wa.right - w - m;
        y = c.y - h / 2;
    } else if (icon.right <= wa.left + 2) {                      // ... on the left
        x = wa.left + m;
        y = c.y - h / 2;
    }
    x = std::clamp(x, (int)wa.left + m, std::max((int)wa.left + m, (int)wa.right - w - m));
    y = std::clamp(y, (int)wa.top + m, std::max((int)wa.top + m, (int)wa.bottom - h - m));
    P.hover = Hit{};
    SetWindowPos(P.hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(P.hwnd);
    InvalidateRect(P.hwnd, nullptr, FALSE);
}

void Hide() {
    if (!P.hwnd || !IsWindowVisible(P.hwnd)) return;
    ShowWindow(P.hwnd, SW_HIDE);
    P.hiddenAt = GetTickCount();
}

bool Visible() { return P.hwnd && IsWindowVisible(P.hwnd); }

bool Contains(POINT pt) {
    if (!Visible()) return false;
    RECT r;
    GetWindowRect(P.hwnd, &r);
    return PtInRect(&r, pt) != FALSE;
}

HWND Window() { return P.hwnd; }

void Refresh() {
    if (Visible()) InvalidateRect(P.hwnd, nullptr, FALSE);
}

void Shutdown() {
    SafeRelease(P.brush);
    SafeRelease(P.rt);
    SafeRelease(P.round);
    SafeRelease(P.title);
    SafeRelease(P.label);
    SafeRelease(P.section);
    SafeRelease(P.button);
    SafeRelease(P.dw);
    SafeRelease(P.d2d);
    if (P.hwnd) DestroyWindow(P.hwnd);
    P.hwnd = nullptr;
}

}  // namespace Panel
