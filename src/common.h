#pragma once
// Windows 10+ APIs (per-monitor DPI v2, cloaked windows, DirectComposition).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <vector>

// Weather. Blizzard = метель (wind + heavy snow), Buran = буран (a storm), Sleet = снег с дождём.
enum class Intensity : int { Light = 0, Normal = 1, Heavy = 2, Blizzard = 3, Buran = 4, Sleet = 5 };
constexpr int kWeatherCount = 6;
enum class WindDir : int { None = 0, Left = 1, Right = 2 };  // direction (None is treated as Right)
constexpr int kWindPowers = 4;  // 0 = calm, 1 = light, 2 = strong, 3 = storm

struct Settings {
    Intensity intensity = Intensity::Normal;
    WindDir wind = WindDir::Right;  // which way the wind blows...
    int windPower = 0;              // ...and how hard (0 = no wind at all)
    bool paused = false;
    float accumulation = 3.f;  // multiplier for how fast snow piles up (--fast = 15, for demos)
};

// Choosing a kind of weather. A blizzard / a snowstorm come with their wind (it can be changed afterwards);
// everything else keeps the wind as it was, so e.g. heavy snow can fall straight down or with wind.
inline void ApplyWeather(Settings& s, Intensity w) {
    s.intensity = w;
    if (w == Intensity::Blizzard && s.windPower < 2) s.windPower = 2;
    if (w == Intensity::Buran) s.windPower = 3;
}

// A visible top-level window, in overlay coordinates (DWM frame bounds).
struct WinInfo {
    HWND hwnd;
    RECT r;
    bool taskbar;
};

struct Scene {
    std::vector<WinInfo> windows;  // z-order, topmost first
    std::vector<RECT> monitors;    // overlay coordinates
};
