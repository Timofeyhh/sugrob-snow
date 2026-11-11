#include "sim.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr size_t kMaxFlakes = 9000;
constexpr float kWindowMaxH = 24.f;   // snowfall piles up on a window to this depth, dip
constexpr float kFloorMaxH = 48.f;    // ...and along the bottom of the screen, dip
constexpr float kMinGapTop = 24.f;    // windows closer than this to the screen top get no snow
constexpr float kMittenR = 22.f;      // cursor ("mitten") radius, dip
constexpr float kSlope = 0.7f;        // how fast snow thins out towards window corners (per px)
constexpr float kRepose = 0.7f;      // angle of repose: steeper slopes slide down
constexpr float kSettle = 0.012f;      // slow settling that rounds off bumps
constexpr float kFlakeVolume = 5.f;   // snow area (dip^2) added by a near flake of average size
constexpr float kLandDepth = 0.45f;   // flakes nearer than this settle; farther ones fly past
// Weather presets (index = Intensity). The wind is a separate setting (Settings::windPower / wind).
struct Weather {
    float snow;     // flakes per second per 1000 dip of width
    float rain;     // raindrops per second per 1000 dip of width (they cross the screen in ~1.5 s, so many more)
    float turb;     // turbulence amplitude, dip/s
    float size;     // flake size multiplier
    float wet;      // wet snow: falls faster, sways less (1 = dry)
};
constexpr Weather kWeather[kWeatherCount] = {
    {5.f, 0.f, 18.f, 1.f, 1.f},       // light snow
    {12.f, 0.f, 18.f, 1.f, 1.f},      // snow
    {95.f, 0.f, 22.f, 1.25f, 1.1f},   // heavy snow: a thick wall of big flakes
    {45.f, 0.f, 50.f, 1.f, 1.f},      // blizzard (метель): fine snow in swirling wind
    {80.f, 0.f, 85.f, 1.05f, 1.f},    // storm (буран)
    {9.f, 110.f, 16.f, 1.2f, 1.6f},   // sleet (снег с дождём)
};
constexpr float kWindPower[kWindPowers] = {0.f, 45.f, 110.f, 220.f};  // calm, light, strong, storm (dip/s)
constexpr float kHeapFactor = 2.5f;  // heaps of thrown / broken snow may be this many times deeper than snowfall piles
constexpr float kRainMelt = 1.6f;  // snow area (dip^2) a raindrop melts where it hits

const Weather& WeatherOf(const Settings& s) { return kWeather[std::clamp((int)s.intensity, 0, kWeatherCount - 1)]; }
constexpr float kTwoPi = 6.2831853f;
constexpr float kPi = 3.14159265f;

constexpr size_t kMaxBalls = 24;
constexpr float kBallMinStartR = 3.5f; // a pile must give at least this much to start a ball, dip
constexpr float kBallGravity = 1400.f; // dip/s^2 — balls are heavy compared to flakes
constexpr float kBreakSpeed = 1350.f;  // impacts faster than this burst a ball, dip/s (~650 px fall)
constexpr float kStackSquash = 0.86f;  // stacked balls press into each other a little

// Wind: chosen direction with slow gusts, plus a light drift so calm air is never frozen.
float WindSpeed(const Settings& s, float t, float sc) {
    const float dir = s.wind == WindDir::Left ? -1.f : 1.f;
    const float mag = kWindPower[std::clamp(s.windPower, 0, kWindPowers - 1)];
    const float gust = 0.55f + 0.45f * (0.5f + 0.3f * std::sin(t * 0.21f) + 0.2f * std::sin(t * 0.57f + 1.7f));
    const float drift = 6.f * std::sin(t * 0.07f + 0.5f);
    return (dir * mag * gust + drift) * sc;
}

// Cheap coherent turbulence (a few sines, dip coordinates): neighbouring flakes swirl together.
void Turbulence(float x, float y, float t, float& ux, float& uy) {
    const float a = std::sin(x * 0.0040f + t * 0.31f) * std::cos(y * 0.0050f - t * 0.23f);
    const float b = std::sin(y * 0.0090f + x * 0.0020f + t * 0.67f);
    const float c = std::cos(x * 0.0070f - y * 0.0030f - t * 0.41f);
    ux = a + 0.5f * b;
    uy = 0.35f * c * a;
}

}  // namespace

void World::Init(float scale, int width, int height) {
    scale_ = std::max(0.5f, scale);
    cellW_ = std::max(2, (int)std::lround(2.f * scale_));
    rng_ = GetTickCount() | 1u;
    Resize(width, height);
}

void World::Resize(int width, int height) {
    width_ = width;
    height_ = height;
    flakes_.clear();
    pending_.clear();
    ledges_.clear();
    balls_.clear();
    heldId_ = 0;
    flakes_.reserve(kMaxFlakes);
}

float World::Rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ >> 8) * (1.f / 16777216.f);
}

Ledge* World::FindLedge(uint64_t key) {
    for (Ledge& l : ledges_)
        if (!l.dying && l.key == key) return &l;
    return nullptr;
}

// A closed / resized / snapped window leaves its snow behind as a "ghost" that crumbles away gradually
// (see UpdateDying). Tiny piles just vanish; the number of ghosts is capped.
bool World::AddGhost(const Ledge& src, std::vector<Ledge>& out, int dir) {
    const int n = (int)src.h.size();
    if (n == 0 || src.vis.size() != (size_t)n || src.cap.size() != (size_t)n) return false;
    float area = 0.f;
    for (int k = 0; k < n; ++k)
        if (src.vis[(size_t)k]) area += std::min(src.h[(size_t)k], src.cap[(size_t)k]) * cellW_;
    if (area < 6.f * scale_ * scale_) return false;
    size_t count = out.size();
    for (const Ledge& l : ledges_) count += l.dying ? 1 : 0;
    if (count >= 16) return false;
    Ledge g = src;
    g.key = 0;
    g.dying = true;
    g.floor = false;
    g.age = 0.f;
    g.yOff = 0.f;
    g.vy = 0.f;
    g.spill = 0.f;
    g.dir = dir != 0 ? dir : (Rand() < 0.5f ? -1 : 1);
    for (int k = 0; k < n; ++k)
        g.h[(size_t)k] = src.vis[(size_t)k] ? std::min(src.h[(size_t)k], src.cap[(size_t)k]) : 0.f;
    out.push_back(std::move(g));
    return true;
}

// The snow of cells [a, b) lost its support (the window got narrower): a wide strip falls as a slab that
// crumbles on the way down, a narrow one (the usual case while a window border is dragged) as a few lumps
// right at the edge. dir: which way is "away from the window".
void World::DropCells(const Ledge& l, int a, int b, int dir, std::vector<Ledge>& ghosts) {
    const int n = (int)l.h.size();
    a = std::max(a, 0);
    b = std::min(b, n);
    if (b <= a || l.vis.size() != (size_t)n || l.cap.size() != (size_t)n) return;
    const float sc = scale_;
    if ((float)((b - a) * cellW_) >= 40.f * sc) {
        Ledge part;
        part.left = l.left + a * cellW_;
        part.width = (b - a) * cellW_;
        part.y = l.y;
        part.key = l.key;
        part.h.assign(l.h.begin() + a, l.h.begin() + b);
        part.cap.assign(l.cap.begin() + a, l.cap.begin() + b);
        part.vis.assign(l.vis.begin() + a, l.vis.begin() + b);
        if (AddGhost(part, ghosts, dir)) return;
    }
    for (int i = a; i < b; i += 3) {
        const int e = std::min(b, i + 3);
        float area = 0.f, top = 0.f;
        for (int k = i; k < e; ++k) {
            if (!l.vis[(size_t)k]) continue;
            const float hv = std::min(l.h[(size_t)k], l.cap[(size_t)k]);
            area += hv * cellW_;
            top = std::max(top, hv);
        }
        if (area < 0.6f * sc * sc || flakes_.size() >= kMaxFlakes) continue;
        flakes_.push_back(MakeClump(l.left + (i + (e - i) * 0.5f) * cellW_, l.y - top * Rand(0.3f, 0.8f), area,
                                    (float)dir * Rand(5.f, 35.f) * sc, Rand(0.f, 30.f) * sc));
    }
}

// The window was resized. The snow over the part of its top edge that is still there stays where it is
// (in screen coordinates), only the snow beyond the new end falls, and new room starts empty.
// Returns false when nothing is left of the old edge (then the whole pile falls, as before).
bool World::ResizeLedge(Ledge& l, int left, int width, std::vector<Ledge>& ghosts) {
    const int oldN = (int)l.h.size();
    if (oldN == 0 || l.vis.size() != (size_t)oldN || l.cap.size() != (size_t)oldN) return false;
    const float sc = scale_;
    const int newN = (width + cellW_ - 1) / cellW_;
    const int off = (int)std::lround((float)(left - l.left) / (float)cellW_);  // old cell under new cell 0
    const int o0 = std::max(0, off), o1 = std::min(oldN, off + newN);          // old cells still on the edge
    if (o1 - o0 < 2) return false;
    DropCells(l, 0, o0, -1, ghosts);     // cut off on the left
    DropCells(l, o1, oldN, 1, ghosts);   // cut off on the right
    std::vector<float> h((size_t)newN, 0.f);
    for (int o = o0; o < o1; ++o) h[(size_t)(o - off)] = l.h[(size_t)o];
    // The corners are rounded (see BuildMask): snow that no longer fits next to a new end slides off it.
    float exL = 0.f, exR = 0.f, topL = 0.f, topR = 0.f;
    for (int c = 0; c < newN; ++c) {
        const int o = c + off;
        if (o < o0 || o >= o1 || !l.vis[(size_t)o]) continue;
        const float cx = (c + 0.5f) * cellW_;
        const float cap = 1.5f * sc + std::min(cx, (float)width - cx) * kSlope;
        const float hv = std::min(h[(size_t)c], l.cap[(size_t)o]);
        if (hv <= cap) continue;
        const float ex = (hv - cap) * cellW_;
        h[(size_t)c] -= hv - cap;
        if (cx < width * 0.5f) {
            exL += ex;
            topL = std::max(topL, hv);
        } else {
            exR += ex;
            topR = std::max(topR, hv);
        }
    }
    auto slide = [&](float vol, float top, float x, float dir) {
        while (vol > 0.6f * sc * sc && flakes_.size() < kMaxFlakes) {
            const float v = std::min(vol, 40.f * sc * sc);
            vol -= v;
            flakes_.push_back(MakeClump(x + dir * Rand(0.f, 4.f) * sc, (float)l.y - top * Rand(0.4f, 0.9f), v,
                                        dir * Rand(15.f, 45.f) * sc, Rand(-10.f, 20.f) * sc));
        }
    };
    slide(exL, topL, (float)left, -1.f);
    slide(exR, topR, (float)(left + width), 1.f);
    l.h = std::move(h);
    l.left = left;
    l.width = width;
    return true;
}

// Ghost piles crumble from one end to the other over about a second, a few lumps at a time.
void World::UpdateDying(float dt) {
    const float sc = scale_;
    const float f = 1.f - std::exp(-6.f * dt);  // share of the snow released per frame once the wave arrives
    const float q = std::min(1.f, f * 5.f);      // each 3-cell group lets go only now and then (staggered)
    for (size_t i = 0; i < ledges_.size();) {
        Ledge& l = ledges_[i];
        if (!l.dying) {
            ++i;
            continue;
        }
        l.age += dt;
        l.vy += 800.f * sc * dt;  // the whole slab of snow starts to fall, it does not hang in the air
        l.yOff += l.vy * dt;
        const int n = (int)l.h.size();
        float remaining = 0.f;
        constexpr int kGroup = 5;  // cells that come off together: chunks of snow, not grains
        for (int g0 = 0; g0 < n; g0 += kGroup) {
            const float pos = (g0 + kGroup * 0.5f) * cellW_ / (float)std::max(1, l.width);
            const float delay = (l.dir > 0 ? pos : 1.f - pos) * 0.45f;
            const bool go = l.age >= delay && Rand() < q;
            float area = 0.f, top = 0.f;
            for (int k = g0; k < std::min(n, g0 + kGroup); ++k) {
                if (go) {
                    const float rm = l.h[(size_t)k] * std::min(1.f, f / q);
                    top = std::max(top, l.h[(size_t)k]);
                    l.h[(size_t)k] -= rm;
                    area += rm * cellW_;
                }
                remaining += l.h[(size_t)k] * cellW_;
            }
            if (area > 0.6f * sc * sc && flakes_.size() < kMaxFlakes)
                flakes_.push_back(MakeClump(l.left + (g0 + kGroup * 0.5f) * cellW_, l.y + l.yOff - top * Rand(0.2f, 1.f), area,
                                            (Rand(-25.f, 25.f) + (float)l.dir * Rand(0.f, 25.f)) * sc,
                                            l.vy * 0.9f + Rand(-15.f, 25.f) * sc));
        }
        if (remaining < 1.5f * sc * sc || l.age > 4.f || l.y + l.yOff > height_ + 50.f * sc) ledges_.erase(ledges_.begin() + (ptrdiff_t)i);
        else ++i;
    }
}

void World::PlaceLedge(Ledge& l, int left, int width, int y) {
    if (l.left == left && l.width == width && l.y == y && !l.h.empty()) return;
    l.left = left;
    l.width = width;
    l.y = y;
    l.h.assign((size_t)((width + cellW_ - 1) / cellW_), 0.f);
    l.spill = 0.f;
}

// ---------------------------------------------------------------------------
// Scene -> ledges

void World::ApplyScene(const Scene& scene, double now) {
    const float sc = scale_;
    const int minGap = (int)(kMinGapTop * sc);
    for (Ledge& l : ledges_) l.seen = false;
    std::vector<Ledge> ghosts;  // piles left behind by windows that went away (added at the end)

    // Top edges of windows.
    for (size_t i = 0; i < scene.windows.size(); ++i) {
        const WinInfo& w = scene.windows[i];
        const RECT& r = w.r;
        const int midX = (r.left + r.right) / 2;
        const RECT* mon = nullptr;
        for (const RECT& m : scene.monitors)
            if (midX >= m.left && midX < m.right && r.top >= m.top && r.top < m.bottom) {
                mon = &m;
                break;
            }
        if (!mon || r.top < mon->top + minGap) continue;  // maximized / at the very top
        const int left = std::max<int>(r.left, 0);
        const int right = std::min<int>(r.right, width_);
        const int width = right - left;
        if (width < 40) continue;

        const uint64_t key = (uint64_t)(uintptr_t)w.hwnd;
        Ledge* l = FindLedge(key);
        if (l) {
            const int dx = left - l->left, dy = r.top - l->y;
            if (width != l->width) {
                // Resized: only the snow beyond the new end falls, the rest stays where it is.
                // Snowballs / snowmen stay on the window (UpdateBalls keeps them within its new edge).
                const bool jumped = std::abs(dy) > 250.f * sc;  // snapped / restored somewhere else
                if (!jumped && dy != 0) {
                    // The top border dragged: a slow drag carries the snow, a fast one shakes some of it off.
                    const float dts = (float)std::max(1.0 / 60.0, now - l->t);
                    const float wvy = dy / dts;
                    const float rate = std::clamp((std::fabs(wvy) - 50.f * sc) / (450.f * sc), 0.f, 1.f) * 10.f;
                    float frac = 1.f - std::exp(-rate * dts);
                    if (wvy > 500.f * sc) frac = std::max(frac, 0.3f);
                    Shed(*l, frac, 0.f, std::min(wvy, 0.f) * 0.7f);
                }
                if (jumped || !ResizeLedge(*l, left, width, ghosts)) AddGhost(*l, ghosts);  // PlaceLedge clears it
                l->y = r.top;
            } else if (dx != 0 || dy != 0) {
                // Moved. A slow drag carries the snow along; a faster one shakes it off bit by bit
                // (a little each scan, unevenly along the edge) with the window's inertia.
                const float dts = (float)std::max(1.0 / 60.0, now - l->t);
                const float wvx = dx / dts, wvy = dy / dts;
                const float speed = std::sqrt(wvx * wvx + wvy * wvy);
                const float rate = std::clamp((speed - 50.f * sc) / (450.f * sc), 0.f, 1.f) * 10.f;
                float frac = 1.f - std::exp(-rate * dts);
                const bool jumped = std::abs(dx) > 250.f * sc || std::abs(dy) > 250.f * sc;  // snapped / teleported
                if (!jumped && wvy > 500.f * sc) frac = std::max(frac, 0.3f);  // pulled down from under the snow
                if (jumped) {
                    AddGhost(*l, ghosts);
                    std::fill(l->h.begin(), l->h.end(), 0.f);
                    l->spill = 0.f;
                    ShiftBallsOn(key, (float)dx);  // snowmen ride along with a snapped window
                } else {
                    Shed(*l, frac, wvx * 0.7f, std::min(wvy, 0.f) * 0.7f);
                    // Snowballs / snowmen are heavy and sticky: they ride along with the window and only
                    // fly off when it is violently yanked.
                    if (speed > 2400.f * sc) FreeBallsOn(key, wvx * 0.5f, std::min(wvy, 0.f) * 0.5f);
                    else ShiftBallsOn(key, (float)dx);
                }
                l->left = left;  // whatever is left rides along with the window
                l->y = r.top;
            }
        } else {
            ledges_.emplace_back();
            l = &ledges_.back();
            l->key = key;
        }
        l->floor = false;
        l->seen = true;
        l->t = now;
        PlaceLedge(*l, left, width, r.top);
        BuildMask(*l, scene, (int)i, kWindowMaxH * sc);
    }

    // Bottom of the screen.
    for (size_t m = 0; m < scene.monitors.size(); ++m) {
        const RECT& mr = scene.monitors[m];
        const uint64_t key = 0xF100000000000000ull | (uint64_t)m;
        Ledge* l = FindLedge(key);
        if (!l) {
            ledges_.emplace_back();
            l = &ledges_.back();
            l->key = key;
        }
        l->floor = true;
        l->seen = true;
        l->t = now;
        const int left = std::max<int>(mr.left, 0);
        const int right = std::min<int>(mr.right, width_);
        PlaceLedge(*l, left, std::max(1, right - left), std::min<int>(mr.bottom, height_));
        BuildMask(*l, scene, -1, kFloorMaxH * sc);
    }

    // Closed or minimized windows: their snow crumbles and falls away gradually, not all in one frame.
    for (size_t i = 0; i < ledges_.size();) {
        Ledge& l = ledges_[i];
        if (l.dying || l.seen) {
            ++i;
            continue;
        }
        FreeBallsOn(l.key, 0.f, 0.f);
        if (!l.floor) AddGhost(l, ghosts);
        ledges_.erase(ledges_.begin() + (ptrdiff_t)i);
    }
    for (Ledge& g : ghosts) ledges_.push_back(std::move(g));
}

void World::BuildMask(Ledge& l, const Scene& scene, int z, float fallH) {
    const int n = (int)l.h.size();
    const float sc = scale_;
    const float maxH = fallH * kHeapFactor;  // heaps may be deeper than what snowfall builds up
    l.fall = fallH;
    l.cap.assign((size_t)n, maxH);
    l.vis.assign((size_t)n, 1);
    const int right = l.left + l.width;

    // Windows above this one in z-order hide its edge or limit the pile height.
    // The floor is only hidden by taskbars (snow along the bottom is "in front").
    const size_t count = z < 0 ? scene.windows.size() : (size_t)z;
    for (size_t j = 0; j < count; ++j) {
        const WinInfo& o = scene.windows[j];
        if (z < 0 && !o.taskbar) continue;
        const RECT& R = o.r;
        if (R.right <= l.left || R.left >= right) continue;
        const int c0 = std::max(0, (int)(R.left - l.left) / cellW_);
        const int c1 = std::min(n, (int)(R.right - l.left + cellW_ - 1) / cellW_);
        if (R.top <= l.y && R.bottom >= l.y) {
            for (int c = c0; c < c1; ++c) l.vis[(size_t)c] = 0;
        } else if (R.bottom < l.y && R.bottom > l.y - maxH) {
            const float lim = (float)(l.y - R.bottom) - 1.f;
            for (int c = c0; c < c1; ++c) l.cap[(size_t)c] = std::min(l.cap[(size_t)c], lim);
        }
    }

    // Rounded ends: snow thins out towards the window corners.
    for (int c = 0; c < n; ++c) {
        if (!l.floor) {
            const float cx = (c + 0.5f) * cellW_;
            const float d = std::min(cx, (float)l.width - cx);
            l.cap[(size_t)c] = std::min(l.cap[(size_t)c], 1.5f * sc + d * kSlope);
        }
        if (!l.vis[(size_t)c] || l.cap[(size_t)c] < 1.f) {
            l.vis[(size_t)c] = 0;
            l.cap[(size_t)c] = 0.f;
        }
    }
    // ...and next to the parts hidden behind other windows.
    const float step = kSlope * cellW_;
    for (int c = 1; c < n; ++c)
        l.cap[(size_t)c] = std::min(l.cap[(size_t)c], l.cap[(size_t)c - 1] + step + (l.vis[(size_t)c - 1] ? 0.f : 1.5f * sc));
    for (int c = n - 2; c >= 0; --c)
        l.cap[(size_t)c] = std::min(l.cap[(size_t)c], l.cap[(size_t)c + 1] + step + (l.vis[(size_t)c + 1] ? 0.f : 1.5f * sc));
}

// Throws a fraction of the visible snow off a ledge as clumps with the given velocity.
void World::Shed(Ledge& l, float fraction, float vx, float vy) {
    const int n = (int)l.h.size();
    if (fraction <= 0.f || n == 0) return;
    const float sc = scale_;
    const bool hasMask = l.vis.size() == (size_t)n && l.cap.size() == (size_t)n;
    if (!hasMask) {
        std::fill(l.h.begin(), l.h.end(), 0.f);
        l.spill = 0.f;
        return;
    }
    for (int i = 0; i < n; i += 3) {
        // Each group of cells lets go of a different share, so the snow comes off unevenly, not in one clean sheet.
        const float fr = fraction >= 1.f ? 1.f : std::min(1.f, fraction * Rand(0.15f, 1.85f));
        float area = 0.f, top = 0.f;
        for (int k = i; k < std::min(n, i + 3); ++k) {
            if (!l.vis[(size_t)k]) continue;
            const float hv = std::min(l.h[(size_t)k], l.cap[(size_t)k]);
            const float take = hv * fr;
            l.h[(size_t)k] -= take;
            area += take * cellW_;
            top = std::max(top, hv);
        }
        if (area < 0.8f * sc * sc || flakes_.size() >= kMaxFlakes) continue;
        flakes_.push_back(MakeClump(l.left + (i + 1.5f) * cellW_, l.y - top * Rand(0.3f, 1.f), area,
                                    vx + Rand(-30.f, 30.f) * sc, vy + Rand(-40.f, 10.f) * sc));
    }
    if (fraction >= 1.f) {
        std::fill(l.h.begin(), l.h.end(), 0.f);
        l.spill = 0.f;
    }
}

// ---------------------------------------------------------------------------
// Piles

bool World::Deposit(Ledge& l, int c, float vol, bool heap) {
    const int n = (int)l.h.size();
    if (n == 0 || l.vis.size() != (size_t)n || l.cap.size() != (size_t)n) return false;
    const float fall = l.fall > 0.f ? l.fall : 1e9f;
    auto room = [&](int i) -> float {
        if (i < 0 || i >= n || !l.vis[(size_t)i]) return -1.f;
        const float lim = heap ? l.cap[(size_t)i] : std::min(l.cap[(size_t)i], fall);
        return lim - l.h[(size_t)i];
    };
    if (room(c) <= 0.f) {  // full here: it rolls / slides along to where there is room
        int best = -1;
        for (int k = 1; k <= 12 && best < 0; ++k) {
            if (room(c - k) > 0.f) best = c - k;
            else if (room(c + k) > 0.f) best = c + k;
        }
        if (best < 0) return false;  // everything is full: the flake melts
        c = best;
    }
    // Spread over a small mound; bigger lumps spread wider so they don't leave spikes.
    const float dh = vol / cellW_;
    const int hw = std::clamp((int)std::lround(std::sqrt(dh) * 0.8f), 1, 5);  // narrow: piles stay lumpy, not doughy
    const float norm = dh / (float)((hw + 1) * (hw + 1));  // triangular weights sum to (hw+1)^2
    for (int k = -hw; k <= hw; ++k) {
        const int i = room(c + k) > 0.f ? c + k : c;
        l.h[(size_t)i] += norm * (float)(hw + 1 - std::abs(k));
    }
    return true;
}

// Momentum transfer on landing: a lump that arrives with sideways speed shoves the snow it lands on ahead of it
// and kicks some powder along, so a thrown lump pushes its neighbours instead of only adding to the pile.
void World::Impact(Ledge& l, int c, const Flake& f) {
    const float sc = scale_;
    const float sp = std::fabs(f.vx);
    if (!f.clump || sp < 70.f * sc || f.vol < 1.2f * sc * sc) return;
    const int n = (int)l.h.size();
    const int dir = f.vx > 0.f ? 1 : -1;
    const float mom = (f.vol / (sc * sc)) * (sp / sc);
    const float frac = std::clamp(mom / 5000.f, 0.f, 0.5f);
    float moved = 0.f;
    for (int k = -1; k <= 2; ++k) {  // the cells at and just ahead of the landing spot
        const int i = c + dir * k;
        if (i < 0 || i >= n || !l.vis[(size_t)i]) continue;
        const float take = std::min(l.h[(size_t)i], l.cap[(size_t)i]) * frac * 0.5f;
        l.h[(size_t)i] -= take;
        moved += take * cellW_;
    }
    if (moved < 0.3f * sc * sc) return;
    // Part of it slumps a few cells ahead, part is kicked up as powder that carries the lump's speed on.
    const float kick = moved * 0.4f;
    const int dest = c + dir * (3 + (int)(Rand() * 3.f));
    const bool placed = dest >= 0 && dest < n && l.vis[(size_t)dest] && Deposit(l, dest, moved - kick, true);
    const float flung = placed ? kick : moved;
    const int bits = 1 + (int)(Rand() * 2.f);
    const float surf = l.y - std::min(l.h[(size_t)c], l.cap[(size_t)c]) - 2.f * sc;
    for (int b = 0; b < bits && flakes_.size() + pending_.size() < kMaxFlakes; ++b)
        pending_.push_back(MakeClump(f.x, surf, flung / bits, f.vx * Rand(0.35f, 0.6f), -Rand(40.f, 140.f) * sc));
}

void World::Land(Ledge& l, int c, const Flake& f) {
    const float sc = scale_;
    Impact(l, c, f);
    if (f.clump && f.vy > 260.f * sc && f.vol > 4.f * sc * sc) {
        // Hard landing: part of the clump bursts into powder that bounces up.
        const float burst = std::min(0.45f, 0.2f + (f.vy - 260.f * sc) / (1500.f * sc));
        const int bits = 2 + (int)(Rand() * 3.f);
        const float bitVol = f.vol * burst / bits;
        const float surf = l.y - std::min(l.h[(size_t)c], l.cap[(size_t)c]) - 1.f;
        for (int b = 0; b < bits && flakes_.size() + pending_.size() < kMaxFlakes; ++b)
            pending_.push_back(MakeClump(f.x + Rand(-2.f, 2.f) * sc, surf, bitVol,
                                         f.vx * 0.3f + Rand(-1.f, 1.f) * f.vy * 0.25f, -Rand(0.15f, 0.32f) * f.vy));
        Deposit(l, c, f.vol * (1.f - burst), true);
        return;
    }
    Deposit(l, c, f.vol, f.clump);  // lumps of snow may heap up above a snowfall pile
}

void World::DepositHeap(Ledge& l, float x, float vol, float halfW) {
    const int n = (int)l.h.size();
    if (n == 0 || l.vis.size() != (size_t)n || l.cap.size() != (size_t)n || vol <= 0.f) return;
    const int c0 = std::clamp((int)((x - l.left) / cellW_), 0, n - 1);
    const int hw = std::max(2, (int)(halfW / cellW_));
    float wsum = 0.f;
    for (int k = -hw; k <= hw; ++k) {
        const float t = (float)k / (float)(hw + 1);
        wsum += 1.f - t * t;
    }
    // A rounded mound where it hit...
    float left = 0.f;
    for (int k = -hw; k <= hw; ++k) {
        const int i = c0 + k;
        const float t = (float)k / (float)(hw + 1);
        const float amt = vol * (1.f - t * t) / wsum;  // area
        if (i < 0 || i >= n || !l.vis[(size_t)i]) {
            left += amt;
            continue;
        }
        const float add = std::min(std::max(0.f, l.cap[(size_t)i] - l.h[(size_t)i]) * cellW_, amt);
        l.h[(size_t)i] += add / cellW_;
        left += amt - add;
    }
    // ...and whatever did not fit there spreads further along the edge.
    for (int step = hw + 1; left > 0.3f * scale_ * scale_ && step < n; ++step)
        for (int side = -1; side <= 1; side += 2) {
            const int i = c0 + side * step;
            if (i < 0 || i >= n || !l.vis[(size_t)i]) continue;
            const float add = std::min(std::max(0.f, l.cap[(size_t)i] - l.h[(size_t)i]) * cellW_, left * 0.5f);
            l.h[(size_t)i] += add / cellW_;
            left -= add;
        }
}

void World::Relax(Ledge& l) {
    const int n = (int)l.h.size();
    if (n < 2 || l.vis.size() != (size_t)n) return;
    // Avalanching (slopes steeper than the angle of repose slide down) plus slow settling.
    const float limit = kRepose * cellW_;
    for (int i = 0; i + 1 < n; ++i) {
        if (!l.vis[(size_t)i] || !l.vis[(size_t)i + 1]) continue;
        const float d = l.h[(size_t)i] - l.h[(size_t)i + 1];
        float t = kSettle * d;
        if (d > limit) t += (d - limit) * 0.5f;
        else if (d < -limit) t += (d + limit) * 0.5f;
        l.h[(size_t)i] -= t;
        l.h[(size_t)i + 1] += t;
    }
    // Anything above the cap slides off the ends of the window as a clump.
    int at = -1;
    for (int i = 0; i < n; ++i) {
        if (l.vis[(size_t)i] && l.h[(size_t)i] > l.cap[(size_t)i] + 0.25f) {
            l.spill += (l.h[(size_t)i] - l.cap[(size_t)i]) * cellW_;
            l.h[(size_t)i] = l.cap[(size_t)i];
            at = i;
        }
    }
    const float minClump = 6.f * scale_ * scale_;
    if (l.spill > minClump && at >= 0) {
        const bool nearEnd = at < 4 || at > n - 5;
        if (nearEnd && !l.floor && flakes_.size() < kMaxFlakes) {
            const float dir = at < n / 2 ? -1.f : 1.f;
            const float x = dir < 0 ? (float)l.left - cellW_ : (float)(l.left + l.width + cellW_);
            flakes_.push_back(MakeClump(x, l.y - l.cap[(size_t)at] - 1.f, l.spill, dir * Rand(15.f, 45.f) * scale_, 0.f));
        }
        l.spill = 0.f;  // in the middle (under another window) the excess just melts
    }
}

void World::ClearSnow() {
    for (Ledge& l : ledges_) {
        std::fill(l.h.begin(), l.h.end(), 0.f);
        l.spill = 0.f;
    }
    balls_.clear();
    heldId_ = 0;
    ledges_.erase(std::remove_if(ledges_.begin(), ledges_.end(), [](const Ledge& l) { return l.dying; }), ledges_.end());
    flakes_.erase(std::remove_if(flakes_.begin(), flakes_.end(), [](const Flake& f) { return f.clump; }), flakes_.end());
}

// ---------------------------------------------------------------------------
// Particles

Flake World::MakeClump(float x, float y, float vol, float vx, float vy) {
    Flake f{};
    f.x = x;
    f.y = y;
    f.vx = vx;
    f.vy = vy;
    f.r = std::clamp(std::sqrt(vol) * 0.38f, 1.1f * scale_, 4.f * scale_);
    f.z = 1.f;
    f.alpha = 1.f;
    f.vol = vol;
    f.phase = Rand(0.f, kTwoPi);  // the renderer uses it for the lump's irregular shape
    f.clump = true;
    f.lands = true;
    return f;
}

void World::Spawn(float dt, const Settings& s) {
    const float sc = scale_;
    const float wind = WindSpeed(s, time_, sc);
    const Weather& w = WeatherOf(s);
    auto spawn = [&](float x, float y) {
        Flake f{};
        f.z = Rand();
        const float u = Rand();
        const float size = (1.f + 2.2f * u * u) * w.size;  // dip; mostly small flakes, a few big ones
        const float persp = 0.45f + 0.55f * f.z;    // far flakes look smaller...
        f.r = size * persp * sc;
        f.x = x;
        f.y = y;
        f.vt = (22.f + 8.f * size + Rand(0.f, 10.f)) * (0.55f + 0.45f * f.z) * sc * w.wet;  // ...and slower
        f.drag = Rand(1.4f, 2.6f) / (0.7f + 0.3f * size);  // big flakes react to the air more lazily
        f.vx = wind;
        f.vy = f.vt;
        f.phase = Rand(0.f, kTwoPi);
        f.freq = Rand(0.25f, 0.7f);
        f.sway = Rand(10.f, 30.f) * (0.6f + 0.4f * f.z) * sc / w.wet;  // wet snow hardly flutters
        f.alpha = (0.35f + 0.65f * f.z) * Rand(0.8f, 1.f);  // ...and fainter
        f.lands = f.z >= kLandDepth;
        f.vol = kFlakeVolume * sc * sc * (size * size) / (1.8f * 1.8f);
        f.clump = false;
        flakes_.push_back(f);
    };
    // Snow comes in through the top of the screen, and with a side wind also through the upwind edge
    // (in the same proportion as the wind to the fall speed), so a storm fills the whole screen evenly.
    const float upwind = 40.f * sc;
    spawnAcc_ += w.snow * (((float)width_ + upwind) / (1000.f * sc)) * dt;
    while (spawnAcc_ >= 1.f) {
        spawnAcc_ -= 1.f;
        if (flakes_.size() >= kMaxFlakes) {
            spawnAcc_ = 0.f;
            break;
        }
        spawn(wind >= 0.f ? Rand(-upwind, (float)width_) : Rand(0.f, (float)width_ + upwind), -Rand(4.f, 30.f) * sc);
    }
    const float sideRatio = std::min(8.f, std::fabs(wind) / (30.f * sc));
    sideAcc_ += w.snow * sideRatio * ((float)height_ / (1000.f * sc)) * dt;
    while (sideAcc_ >= 1.f) {
        sideAcc_ -= 1.f;
        if (flakes_.size() >= kMaxFlakes) {
            sideAcc_ = 0.f;
            break;
        }
        spawn(wind > 0.f ? -Rand(4.f, 30.f) * sc : (float)width_ + Rand(4.f, 30.f) * sc, Rand(0.f, (float)height_));
    }
    // Raindrops (sleet): they fall fast and slant with the wind.
    rainAcc_ += w.rain * ((float)width_ / (1000.f * sc)) * dt;
    while (rainAcc_ >= 1.f) {
        rainAcc_ -= 1.f;
        if (flakes_.size() >= kMaxFlakes) {
            rainAcc_ = 0.f;
            break;
        }
        Flake f{};
        f.rain = true;
        f.z = Rand();
        f.r = (0.6f + 0.5f * f.z) * sc;
        f.vt = (560.f + 300.f * f.z) * sc;
        f.vx = wind * 1.4f;
        f.vy = f.vt * Rand(0.8f, 1.f);
        // Spawn where the slanted drop will cross the screen.
        f.x = Rand(0.f, (float)width_) - f.vx * ((float)height_ / f.vt) * 0.5f;
        f.y = -Rand(4.f, 60.f) * sc;
        f.alpha = 0.3f + 0.55f * f.z;
        f.lands = f.z >= kLandDepth;
        f.vol = kRainMelt * sc * sc;
        flakes_.push_back(f);
    }
}

// A raindrop hits a pile: it melts (packs down) a little snow where it lands and splashes up.
void World::Rain(Ledge& l, int c, const Flake& f) {
    const float sc = scale_;
    const int n = (int)l.h.size();
    for (int k = -1; k <= 1; ++k) {
        const int i = c + k;
        if (i < 0 || i >= n || !l.vis[(size_t)i]) continue;
        l.h[(size_t)i] = std::max(0.f, l.h[(size_t)i] - f.vol / cellW_ * (k == 0 ? 0.5f : 0.25f));
    }
    if (f.z < 0.6f || flakes_.size() + pending_.size() + 2 >= kMaxFlakes) return;
    const float surf = l.y - std::min(l.h[(size_t)c], l.cap[(size_t)c]) - 1.f;
    for (int b = 0; b < 2; ++b) {
        Flake d{};
        d.rain = true;
        d.life = Rand(0.12f, 0.25f);
        d.x = f.x;
        d.y = surf;
        d.vx = f.vx * 0.2f + Rand(-70.f, 70.f) * sc;
        d.vy = -Rand(70.f, 170.f) * sc;
        d.r = 0.5f * sc;
        d.z = f.z;
        d.alpha = f.alpha * 0.8f;
        pending_.push_back(d);
    }
}

// The cursor is a "mitten": moving it through a pile throws the snow away
// (it lands again nearby, like a plough). No clicks needed, so the overlay stays click-through.
void World::Sweep(float dt, POINT cur) {
    const float sc = scale_;
    if (!cursorInit_) {
        prevCursor_ = cur;
        cursorInit_ = true;
        cursorVx_ = cursorVy_ = 0.f;
        return;
    }
    const float inv = 1.f / std::max(dt, 1e-3f);
    const float vx = (cur.x - prevCursor_.x) * inv;
    const float vy = (cur.y - prevCursor_.y) * inv;
    prevCursor_ = cur;
    cursorVx_ = cursorVx_ * 0.5f + vx * 0.5f;
    cursorVy_ = cursorVy_ * 0.5f + vy * 0.5f;
    if (heldId_) return;  // carrying a snowball: the cursor is busy
    const float speed = std::sqrt(cursorVx_ * cursorVx_ + cursorVy_ * cursorVy_);
    if (speed < 60.f * sc) return;

    const float R = kMittenR * sc;
    const float strength = std::min(1.f, speed / (500.f * sc));
    // Snow is scooped away at a rate (per second), not all at once: the faster the swipe, the more it takes,
    // and a slow swipe wears the pile down gradually.
    const float fSweep = 1.f - std::exp(-(4.f + 12.f * strength) * dt);
    const float px = (float)cur.x, py = (float)cur.y;

    // The mitten cannot pass through a snowman, and it never digs out the snow a ball sits on
    // (otherwise a snowman slowly sinks while the cursor hovers over it).
    for (const Ball& b : balls_) {
        const float dx = px - b.x, dy = py - b.y, rr = b.r + 4.f * sc;
        if (dx * dx + dy * dy < rr * rr) return;
    }
    auto underBall = [&](const Ledge& l, float x) {
        for (const Ball& b : balls_)
            if (b.state == Ball::State::OnLedge && b.ledgeKey == l.key && std::fabs(x - b.x) < b.r * 0.95f) return true;
        return false;
    };
    // A mostly sideways swipe pushes part of the snow ahead of the mitten as a heap (a plough);
    // the rest is thrown.
    const bool sideways = std::fabs(cursorVx_) > 0.5f * speed;
    const float dir = cursorVx_ >= 0.f ? 1.f : -1.f;
    const float throwVx = std::clamp(cursorVx_, -1800.f * sc, 1800.f * sc);  // a wild flick doesn't launch snow off the screen
    const float throwVy = std::clamp(cursorVy_, -1800.f * sc, 1800.f * sc);
    int budget = 70;
    for (Ledge& l : ledges_) {
        const int n = (int)l.h.size();
        if (n == 0 || l.vis.size() != (size_t)n) continue;
        if (px < l.left - R || px > l.left + l.width + R) continue;
        const float maxH = (l.floor ? kFloorMaxH : kWindowMaxH) * sc * kHeapFactor;
        if (py > l.y + R * 0.5f || py < l.y - maxH - R) continue;
        const int c0 = std::max(0, (int)((px - R - l.left) / cellW_));
        const int c1 = std::min(n, (int)((px + R - l.left) / cellW_) + 1);
        float removed = 0.f;
        for (int c = c0; c < c1; ++c) {
            if (!l.vis[(size_t)c]) continue;
            const float hv = std::min(l.h[(size_t)c], l.cap[(size_t)c]);
            if (hv < 0.05f) continue;
            if (underBall(l, l.left + (c + 0.5f) * cellW_)) continue;
            const float dx = std::fabs(l.left + (c + 0.5f) * cellW_ - px) / R;
            if (dx >= 1.f) continue;
            if (py < l.y - hv - R * 0.6f) continue;  // the mitten passes above the snow
            const float take = hv * (1.f - dx * dx) * fSweep;
            l.h[(size_t)c] -= take;
            removed += take * cellW_;
        }
        if (sideways && removed > 1.f * sc * sc) {  // the plough heap, just ahead of the mitten
            const float heap = removed * (0.25f + 0.3f * strength);
            const float hx = px + dir * R * 1.1f;
            const int hc = (int)((hx - l.left) / cellW_);
            if (hc >= 0 && hc < n && l.vis[(size_t)hc] && !underBall(l, hx) && Deposit(l, hc, heap, true)) removed -= heap;
        }
        // The rest is thrown as lumps that all carry the swipe's direction (and, through CoupleSnow,
        // keep moving together as one body), not scattered in random directions.
        while (removed > 0.7f * sc * sc && budget > 0 && flakes_.size() < kMaxFlakes) {
            const float vol = std::min(removed, 8.f * sc * sc);
            removed -= vol;
            --budget;
            flakes_.push_back(MakeClump(px + Rand(-0.7f, 0.7f) * R, l.y - Rand(2.f, 10.f) * sc, vol,
                                        throwVx * Rand(0.35f, 0.6f) + Rand(-18.f, 18.f) * sc,
                                        std::min(0.f, throwVy * 0.3f) - Rand(60.f, 200.f) * sc));
        }
    }
}

// ---------------------------------------------------------------------------
// Momentum between airborne snow. Lumps close to each other exchange velocity, so a handful of snow
// moves as one body instead of as independent particles, and a fast lump drags the flakes it passes.

int World::GridX(float x) const { return std::clamp((int)std::floor(x / gridCell_) + 1, 0, gridW_ - 1); }
int World::GridY(float y) const { return std::clamp((int)std::floor(y / gridCell_) + 1, 0, gridH_ - 1); }

void World::CoupleSnow(float dt) {
    const float sc = scale_;
    clX_.clear();
    clY_.clear();
    clVx_.clear();
    clVy_.clear();
    clM_.clear();
    clR_.clear();
    for (const Flake& f : flakes_) {
        if (!f.clump) continue;
        clX_.push_back(f.x);
        clY_.push_back(f.y);
        clVx_.push_back(f.vx);
        clVy_.push_back(f.vy);
        clM_.push_back(std::max(f.vol, 0.5f * sc * sc));
        clR_.push_back(f.r);
    }
    const size_t nc = clX_.size();
    dvx_.assign(nc, 0.f);
    dvy_.assign(nc, 0.f);
    if (nc == 0) return;
    gridCell_ = 32.f * sc;
    gridW_ = (int)((float)width_ / gridCell_) + 3;
    gridH_ = (int)((float)height_ / gridCell_) + 3;
    gridHead_.assign((size_t)gridW_ * (size_t)gridH_, -1);
    gridNext_.assign(nc, -1);
    for (size_t i = 0; i < nc; ++i) {
        const size_t g = (size_t)GridY(clY_[i]) * (size_t)gridW_ + (size_t)GridX(clX_[i]);
        gridNext_[i] = gridHead_[g];
        gridHead_[g] = (int)i;
    }
    const float kc = 18.f;  // how quickly neighbouring lumps match their velocities (1/s)
    for (size_t i = 0; i < nc; ++i) {
        const int gx = GridX(clX_[i]), gy = GridY(clY_[i]);
        for (int yy = std::max(0, gy - 1); yy <= std::min(gridH_ - 1, gy + 1); ++yy)
            for (int xx = std::max(0, gx - 1); xx <= std::min(gridW_ - 1, gx + 1); ++xx)
                for (int j = gridHead_[(size_t)yy * (size_t)gridW_ + (size_t)xx]; j >= 0; j = gridNext_[(size_t)j]) {
                    if ((size_t)j <= i) continue;
                    const float dx = clX_[(size_t)j] - clX_[i], dy = clY_[(size_t)j] - clY_[i];
                    const float d2 = dx * dx + dy * dy;
                    const float D = (clR_[i] + clR_[(size_t)j]) * 2.6f + 3.f * sc;
                    if (d2 >= D * D) continue;
                    const float w = 1.f - std::sqrt(d2) / D;
                    const float a = std::min(0.5f, 1.f - std::exp(-kc * w * w * dt));
                    const float mi = clM_[i], mj = clM_[(size_t)j], tot = mi + mj;
                    const float rvx = clVx_[(size_t)j] - clVx_[i], rvy = clVy_[(size_t)j] - clVy_[i];
                    dvx_[i] += 2.f * a * mj / tot * rvx;  // equal and opposite momentum change
                    dvy_[i] += 2.f * a * mj / tot * rvy;
                    dvx_[(size_t)j] -= 2.f * a * mi / tot * rvx;
                    dvy_[(size_t)j] -= 2.f * a * mi / tot * rvy;
                }
    }
}

void World::DragByClumps(Flake& f, float dt) {
    if (clX_.empty()) return;
    const float sc = scale_;
    const int gx = GridX(f.x), gy = GridY(f.y);
    const float minV2 = 120.f * sc * 120.f * sc;
    for (int yy = std::max(0, gy - 1); yy <= std::min(gridH_ - 1, gy + 1); ++yy)
        for (int xx = std::max(0, gx - 1); xx <= std::min(gridW_ - 1, gx + 1); ++xx)
            for (int j = gridHead_[(size_t)yy * (size_t)gridW_ + (size_t)xx]; j >= 0; j = gridNext_[(size_t)j]) {
                const size_t k = (size_t)j;
                if (clVx_[k] * clVx_[k] + clVy_[k] * clVy_[k] < minV2) continue;  // slow lumps don't stir the air
                const float dx = f.x - clX_[k], dy = f.y - clY_[k];
                const float d2 = dx * dx + dy * dy;
                const float D = (clR_[k] + f.r) * 3.f + 6.f * sc;
                if (d2 >= D * D) continue;
                const float w = 1.f - std::sqrt(d2) / D;
                const float a = std::min(0.6f, 1.f - std::exp(-9.f * w * w * dt));
                f.vx += (clVx_[k] - f.vx) * a;
                f.vy += (clVy_[k] - f.vy) * a * 0.6f;
            }
}

void World::Update(float dt, const Settings& s, POINT cursor) {
    time_ += dt;
    const float sc = scale_;
    const float inv = 1.f / sc;
    Spawn(dt, s);
    Sweep(dt, cursor);
    CoupleSnow(dt);

    const float wind = WindSpeed(s, time_, sc);
    const float turbAmp = (WeatherOf(s).turb + 10.f * (float)std::clamp(s.windPower, 0, kWindPowers - 1)) * sc;
    const float gravity = 950.f * sc;
    const float clumpDrag = 0.00095f * inv;  // quadratic drag: terminal speed ~1000 dip/s
    const float wakeR = kMittenR * 2.2f * sc;
    const float cx = (float)cursor.x, cy = (float)cursor.y;
    const float margin = (float)width_ + 200.f * sc;
    pending_.clear();

    const float cursorSpeed = std::sqrt(cursorVx_ * cursorVx_ + cursorVy_ * cursorVy_);
    size_t ci = 0;  // index of the current lump in the CoupleSnow snapshot
    size_t keep = 0;
    for (size_t i = 0; i < flakes_.size(); ++i) {
        Flake f = flakes_[i];
        const float oy = f.y;
        if (f.clump) {
            if (ci < dvx_.size()) {  // momentum from neighbouring lumps
                f.vx += dvx_[ci];
                f.vy += dvy_[ci];
            }
            ++ci;
            const float wdx = f.x - cx, wdy = f.y - cy, wd2 = wdx * wdx + wdy * wdy;
            if (cursorSpeed > 150.f * sc && wd2 < wakeR * wakeR) {  // the mitten drags nearby lumps along
                const float w = 1.f - std::sqrt(wd2) / wakeR;
                const float a = 0.5f * (1.f - std::exp(-5.f * w * w * dt));
                f.vx += (std::clamp(cursorVx_, -1800.f * sc, 1800.f * sc) - f.vx) * a;
                f.vy += (std::clamp(cursorVy_, -1800.f * sc, 1800.f * sc) - f.vy) * a;
            }
            f.vy += gravity * dt;
            f.vy -= clumpDrag * f.vy * std::fabs(f.vy) * dt;
            f.vx += (wind - f.vx) * std::min(1.f, 0.8f * dt);
            f.x += f.vx * dt;
            f.y += f.vy * dt;
        } else if (f.rain) {
            if (f.life > 0.f) {  // a splash droplet: a short hop, then gone
                f.life -= dt;
                if (f.life <= 0.f) continue;
                f.vy += gravity * dt;
            } else {  // heavy and fast: it barely notices the swirls, only the wind slants it
                const float a = 1.f - std::exp(-3.f * dt);
                f.vx += (wind * 1.4f - f.vx) * a;
                f.vy += (f.vt - f.vy) * a;
            }
            f.x += f.vx * dt;
            f.y += f.vy * dt;
        } else {
            // The flake follows the local air (wind + swirls + the cursor's wake) through drag,
            // and flutters sideways as it tumbles.
            float ux, uy;
            Turbulence(f.x * inv, f.y * inv, time_, ux, uy);
            float ax = wind + ux * turbAmp * (0.4f + 0.6f * f.z);
            float ay = uy * turbAmp;
            const float dx = f.x - cx, dy = f.y - cy;
            const float d2 = dx * dx + dy * dy;
            if (f.z > 0.3f && d2 < wakeR * wakeR) {
                const float d = std::sqrt(d2) + 1e-3f;
                const float w = 1.f - d / wakeR;
                const float w2 = w * w;
                ax += cursorVx_ * 0.8f * w2 + dx / d * 260.f * sc * w2;
                ay += cursorVy_ * 0.8f * w2 + dy / d * 260.f * sc * w2;
            }
            const float flutter = std::sin(f.phase + time_ * f.freq * kTwoPi) * f.sway;
            const float a = 1.f - std::exp(-f.drag * dt);
            f.vx += (ax + flutter - f.vx) * a;
            f.vy += (ay + f.vt - f.vy) * a;
            if (!dvx_.empty() && f.z > 0.3f) DragByClumps(f, dt);
            f.x += f.vx * dt;
            f.y += f.vy * dt;
        }

        if (f.lands && !f.clump && !balls_.empty()) {  // flakes that hit a snowball stick to it
            bool absorbed = false;
            for (const Ball& b : balls_) {
                const float bx = f.x - b.x, by = f.y - b.y;
                if (b.state != Ball::State::Held && bx * bx + by * by < b.r * b.r) {
                    absorbed = true;
                    break;
                }
            }
            if (absorbed) continue;
        }
        if (f.lands && f.y > oy) {  // only near, falling particles settle
            Ledge* hit = nullptr;
            int hitCell = 0;
            float best = 1e30f;
            for (Ledge& l : ledges_) {
                if (l.dying || f.x < l.left || f.x >= l.left + l.width || oy > l.y) continue;
                const int c = (int)((f.x - l.left) / cellW_);
                if (c < 0 || c >= (int)l.h.size() || !l.vis[(size_t)c]) continue;
                const float surf = l.y - std::min(l.h[(size_t)c], l.cap[(size_t)c]);
                if (oy <= surf && f.y >= surf && surf < best) {
                    best = surf;
                    hit = &l;
                    hitCell = c;
                }
            }
            if (hit) {
                if (f.rain) {
                    Flake drop = f;
                    drop.vol *= s.accumulation / 3.f;  // melts in proportion to how fast snow piles up
                    Rain(*hit, hitCell, drop);
                    continue;
                }
                if (!f.clump) f.vol *= s.accumulation;
                Land(*hit, hitCell, f);
                continue;
            }
        }
        if (f.y > height_ + 40.f * sc || f.x < -margin || f.x > width_ + margin) continue;
        flakes_[keep++] = f;
    }
    flakes_.resize(keep);
    flakes_.insert(flakes_.end(), pending_.begin(), pending_.end());
    pending_.clear();
    UpdateDying(dt);
    UpdateBalls(dt);
    for (Ledge& l : ledges_)
        if (!l.dying) Relax(l);
}

// ---------------------------------------------------------------------------
// Snowballs

Ball* World::FindBall(uint32_t id) {
    if (!id) return nullptr;
    for (Ball& b : balls_)
        if (b.id == id) return &b;
    return nullptr;
}

bool World::IsAbove(const Ball& b, uint32_t baseId) {
    const Ball* cur = &b;
    for (int depth = 0; depth < 12 && cur->state == Ball::State::OnBall; ++depth) {
        if (cur->below == baseId) return true;
        cur = FindBall(cur->below);
        if (!cur) return false;
    }
    return false;
}

void World::FreeBallsOn(uint64_t ledgeKey, float vx, float vy) {
    const float sc = scale_;
    for (Ball& b : balls_) {
        if (b.state != Ball::State::OnLedge || b.ledgeKey != ledgeKey) continue;
        b.state = Ball::State::Free;
        b.vx = vx + Rand(-20.f, 20.f) * sc;
        b.vy = vy;
    }
}

void World::ShiftBallsOn(uint64_t ledgeKey, float dx) {
    for (Ball& b : balls_)
        if (b.state == Ball::State::OnLedge && b.ledgeKey == ledgeKey) b.x += dx;
}

void World::DebugDropBall(float x, float y, float radiusDip) {
    Ball b;
    b.id = nextBallId_++;
    b.r = radiusDip * scale_;
    b.vol = kPi * b.r * b.r;
    b.x = x;
    b.y = y;
    balls_.push_back(b);
}

bool World::TryGrab(POINT p) {
    const float sc = scale_;
    const float px = (float)p.x, py = (float)p.y;

    // 1) An existing snowball under the cursor.
    int best = -1;
    float bestD = 1e30f;
    for (size_t i = 0; i < balls_.size(); ++i) {
        const Ball& b = balls_[i];
        const float dx = px - b.x, dy = py - b.y, d2 = dx * dx + dy * dy;
        const float rr = b.r + 6.f * sc;
        if (d2 < rr * rr && d2 < bestD) {
            bestD = d2;
            best = (int)i;
        }
    }
    if (best >= 0) {
        Ball& b = balls_[(size_t)best];
        b.state = Ball::State::Held;  // anything stacked on it comes along
        b.below = 0;
        b.ledgeKey = 0;
        b.grabDx = b.x - px;
        b.grabDy = b.y - py;
        b.vx = b.vy = 0.f;
        b.rolling = false;
        heldId_ = b.id;
        return true;
    }

    // 2) A pile under (or just above) the cursor: pack a small snowball from the snow around.
    if (balls_.size() >= kMaxBalls) return false;
    Ledge* l = nullptr;
    float vol = 0.f;
    int c0 = 0, c1 = 0;
    if (!PilePick(px, py, l, vol, c0, c1)) return false;
    const int n = (int)l->h.size();
    float hv = 0.f;
    {
        const int c = std::clamp((int)((px - l->left) / cellW_), 0, n - 1);
        if (l->vis[(size_t)c]) hv = std::min(l->h[(size_t)c], l->cap[(size_t)c]);
    }
    for (int k = c0; k < c1; ++k)
        if (l->vis[(size_t)k]) l->h[(size_t)k] -= std::min(l->h[(size_t)k], l->cap[(size_t)k]) * 0.8f;

    Ball b;
    b.id = nextBallId_++;
    b.vol = vol;
    b.r = std::sqrt(vol / kPi);
    b.x = px;
    b.y = l->y - hv * 0.2f - b.r * 0.95f;
    b.state = Ball::State::Held;
    b.grabDx = b.x - px;
    b.grabDy = std::clamp(b.y - py, -b.r * 1.5f, b.r * 1.5f);  // never far from the cursor
    b.ledgeKey = l->key;
    balls_.push_back(b);
    heldId_ = b.id;
    return true;
}

// Finds a pile of snow the cursor is on or close to above (generous: the pile itself is only a few pixels thick).
// vol = snow area available for a snowball, [c0, c1) = the cells it would be taken from.
bool World::PilePick(float px, float py, Ledge*& out, float& vol, int& c0, int& c1) {
    const float sc = scale_;
    const float reach = 10.f * sc;  // how far around the cursor snow is gathered, dip
    for (Ledge& l : ledges_) {
        if (l.dying) continue;
        const int n = (int)l.h.size();
        if (n == 0 || l.vis.size() != (size_t)n || l.cap.size() != (size_t)n) continue;
        if (px < l.left || px >= l.left + l.width) continue;
        const int a = std::max(0, (int)((px - reach - l.left) / cellW_));
        const int b = std::min(n, (int)((px + reach - l.left) / cellW_) + 1);
        float hmax = 0.f, v = 0.f;
        for (int k = a; k < b; ++k) {
            if (!l.vis[(size_t)k]) continue;
            const float hv = std::min(l.h[(size_t)k], l.cap[(size_t)k]);
            hmax = std::max(hmax, hv);
            v += hv * 0.8f * cellW_;
        }
        const float minR = kBallMinStartR * sc;
        if (hmax < 2.f * sc || v < kPi * minR * minR) continue;
        // The press may be on the snow, a bit above it, or a hair into the window's top border.
        if (py < l.y - hmax - 12.f * sc || py > l.y + 3.f * sc) continue;
        out = &l;
        vol = v;
        c0 = a;
        c1 = b;
        return true;
    }
    return false;
}


void World::Release() {
    Ball* b = FindBall(heldId_);
    heldId_ = 0;
    if (!b) return;
    const float sc = scale_;
    const float maxV = 2200.f * sc;
    // The ball's own (smoothed) motion blended with the cursor's: a flick throws it, a slow drop just sets it down.
    b->vx = std::clamp(0.5f * (cursorVx_ + heldVx_), -maxV, maxV);
    b->vy = std::clamp(0.5f * (cursorVy_ + heldVy_), -maxV, maxV);
    heldVx_ = heldVy_ = 0.f;
    const bool gentle = std::fabs(b->vx) < 320.f * sc && std::fabs(b->vy) < 320.f * sc;
    if (b->rolling && gentle && FindLedge(b->ledgeKey)) {
        b->state = Ball::State::OnLedge;  // set down where it was rolled: it stays there, it does not roll on its own
        b->vx = 0.f;
        b->vy = 0.f;
    } else {
        b->state = Ball::State::Free;  // dropped or thrown
    }
    b->rolling = false;
}

void World::BurstBall(const Ball& b) {
    // The snow of a broken ball is not lost. Smashed onto an edge, most of it stays right there as a heap of
    // broken snow; the rest (all of it, when it breaks in the air) flies apart as big chunks that land again.
    const float sc = scale_;
    float vol = b.vol;
    Ledge* l = b.fate == 3 ? FindLedge(b.ledgeKey) : nullptr;
    if (l) {
        const float keep = vol * 0.65f;
        DepositHeap(*l, b.x, keep, b.r * 1.2f);
        vol -= keep;
    }
    const float pop = l ? 0.55f : 1.f;  // smashed onto an edge, the pieces don't fly as far
    const int n = std::clamp((int)(vol / (35.f * sc * sc)), 6, 60);
    const float part = vol / n;
    for (int i = 0; i < n && flakes_.size() < kMaxFlakes; ++i) {
        const float a = Rand(0.f, kTwoPi), rr = std::sqrt(Rand()) * b.r * 0.8f;
        float y = b.y + std::sin(a) * rr;
        if (l) y = std::min(y, (float)l->y - 2.f * sc);  // start above the edge, not inside the window
        flakes_.push_back(MakeClump(b.x + std::cos(a) * rr, y, part, b.vx * 0.3f + std::cos(a) * Rand(40.f, 220.f) * sc * pop,
                                    -std::fabs(b.vy) * 0.12f - Rand(60.f, 280.f) * sc * pop));
    }
}

void World::UpdateBalls(float dt) {
    if (balls_.empty()) return;
    const float sc = scale_;
    const float g = kBallGravity * sc;
    const float breakV = kBreakSpeed * sc;
    // No real size limit: a ball grows as long as there is snow to roll it through (it only has to fit on the screen).
    const float maxR = 0.45f * (float)std::min(width_, height_);

    // Visible snow height of a ledge at x (or -1 if x is outside it).
    auto heightAt = [&](const Ledge& l, float x) -> float {
        const int c = (int)((x - l.left) / cellW_);
        if (x < l.left || c < 0 || c >= (int)l.h.size() || l.vis.size() != l.h.size()) return -1.f;
        return l.vis[(size_t)c] ? std::min(l.h[(size_t)c], l.cap[(size_t)c]) : 0.f;
    };

    // Held: follows the cursor. Dragged along a pile, it rolls and packs up the snow it passes.
    if (Ball* b = FindBall(heldId_)) {
        const float ox = b->x, oy = b->y;
        // The ball follows the cursor with a little weight (a soft spring), not rigidly.
        const float follow = 1.f - std::exp(-28.f * dt);
        b->x += ((float)prevCursor_.x + b->grabDx - b->x) * follow;
        b->y += ((float)prevCursor_.y + b->grabDy - b->y) * follow;
        b->rolling = false;
        Ledge* on = nullptr;
        float surf = 0.f;
        for (Ledge& l : ledges_) {
            if (l.dying) continue;
            const float hv = heightAt(l, b->x);
            if (hv < 0.f || !l.vis[(size_t)((b->x - l.left) / cellW_)]) continue;
            const float s = l.y - hv;
            // The ball straddles this edge: keep it on top instead of sinking into the window.
            if (b->y + b->r > s && b->y - b->r < l.y && (!on || s < surf)) {
                on = &l;
                surf = s;
            }
        }
        if (on) {
            b->rolling = true;
            b->ledgeKey = on->key;
            const float moved = std::fabs(b->x - ox);
            if (moved > 0.01f && b->r < maxR) {
                const float f = std::min(1.f, moved / b->r) * 0.8f;
                const int n = (int)on->h.size();
                const int c0 = std::max(0, (int)((b->x - 0.7f * b->r - on->left) / cellW_));
                const int c1 = std::min(n, (int)((b->x + 0.7f * b->r - on->left) / cellW_) + 1);
                for (int k = c0; k < c1; ++k) {
                    if (!on->vis[(size_t)k]) continue;
                    const float take = std::min(on->h[(size_t)k], on->cap[(size_t)k]) * f;
                    on->h[(size_t)k] -= take;
                    b->vol += take * cellW_;
                }
                b->vol = std::min(b->vol, kPi * maxR * maxR);
                b->r = std::sqrt(b->vol / kPi);
            }
            const float hv = std::max(0.f, heightAt(*on, b->x));
            b->y = on->y - hv - b->r * 0.95f;
        }
        b->spin += (b->x - ox) / std::max(b->r, 1.f);
        const float idt = 1.f / std::max(dt, 1e-3f);
        heldVx_ = heldVx_ * 0.6f + 0.4f * (b->x - ox) * idt;
        heldVy_ = heldVy_ * 0.6f + 0.4f * (b->y - oy) * idt;
    }

    // Resting on an edge, or flying.
    for (Ball& b : balls_) {
        if (b.state == Ball::State::Held || b.state == Ball::State::OnBall) continue;
        if (b.state == Ball::State::OnLedge) {
            Ledge* l = FindLedge(b.ledgeKey);
            if (!l || l->h.empty() || l->vis.size() != l->h.size()) {  // the window is gone: the ball falls
                b.state = Ball::State::Free;
                b.vx = b.vy = 0.f;
                continue;
            }
            // Snow is sticky: a ball nobody holds does not roll. It stays where it was put, and if the
            // window shrank or moved under it, it keeps to the window's edge instead of dropping off.
            const float xEnd = (float)(l->left + (int)l->h.size() * cellW_) - 0.01f;
            b.x = std::clamp(b.x, (float)l->left, xEnd);
            b.vx = 0.f;
            b.vy = 0.f;
            b.y = l->y - std::max(0.f, heightAt(*l, b.x)) - b.r * 0.95f;
            continue;
        }
        const float oy = b.y;
        b.vy += g * dt;
        b.vx *= std::exp(-0.3f * dt);
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.spin += b.vx * dt / std::max(b.r, 1.f);
        if (b.x < b.r) {  // screen edges
            b.x = b.r;
            b.vx = std::fabs(b.vx) * 0.3f;
        } else if (b.x > width_ - b.r) {
            b.x = width_ - b.r;
            b.vx = -std::fabs(b.vx) * 0.3f;
        }
        if (b.vy > 0.f) {
            Ledge* hit = nullptr;
            float best = 1e30f;
            for (Ledge& l : ledges_) {
                if (l.dying) continue;
                const float hv = heightAt(l, b.x);
                if (hv < 0.f || !l.vis[(size_t)((b.x - l.left) / cellW_)]) continue;
                const float s = l.y - hv;
                if (oy + b.r <= s + 1.f && b.y + b.r >= s && s < best) {
                    best = s;
                    hit = &l;
                }
            }
            if (hit) {
                if (b.vy > breakV) {
                    b.fate = 3;  // smashed onto this edge: leaves a heap there
                    b.ledgeKey = hit->key;
                    b.y = best - b.r * 0.5f;
                    continue;
                }
                if (b.vy > 350.f * sc && flakes_.size() < kMaxFlakes)  // a puff of powder
                    for (int i = 0; i < 3; ++i)
                        flakes_.push_back(MakeClump(b.x + Rand(-0.6f, 0.6f) * b.r, best - 1.f, 2.f * sc * sc,
                                                    Rand(-120.f, 120.f) * sc, -Rand(60.f, 160.f) * sc));
                b.state = Ball::State::OnLedge;
                b.ledgeKey = hit->key;
                b.y = best - b.r * 0.95f;
                b.vx = 0.f;  // it lands and stays: no rolling away
                b.vy = 0.f;
            }
        }
        // A ball never leaves the screen: if it reached the bottom without landing (it was let go below
        // every edge, e.g. behind the taskbar), it is put on the lowest visible edge under it.
        if (b.state == Ball::State::Free && b.y + b.r >= (float)height_) {
            Ledge* low = nullptr;
            float lowS = -1e30f;
            for (Ledge& l : ledges_) {
                if (l.dying) continue;
                const float hv = heightAt(l, b.x);
                if (hv < 0.f || !l.vis[(size_t)((b.x - l.left) / cellW_)]) continue;
                const float s = l.y - hv;
                if (s > lowS) {
                    lowS = s;
                    low = &l;
                }
            }
            if (low) {
                b.state = Ball::State::OnLedge;
                b.ledgeKey = low->key;
                b.y = lowS - b.r * 0.95f;
            } else {
                b.y = (float)height_ - b.r;  // no edge at all: rest on the bottom of the screen
            }
            b.vx = b.vy = 0.f;
        }
    }

    // Flying balls hitting other balls: land on top (stack), bounce off the side, or burst.
    for (Ball& B : balls_) {
        if (B.state != Ball::State::Free || B.fate) continue;
        for (Ball& A : balls_) {
            if (&A == &B || A.fate || A.state == Ball::State::Held) continue;
            if (IsAbove(A, B.id) || IsAbove(B, A.id)) continue;
            const float dx = B.x - A.x, dy = B.y - A.y;
            const float minD = A.r + B.r;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= minD * minD || d2 < 1e-4f) continue;
            const float d = std::sqrt(d2);
            const float rvx = B.vx - A.vx, rvy = B.vy - A.vy;
            const float relSpeed = std::sqrt(rvx * rvx + rvy * rvy);
            if (relSpeed > breakV * 1.1f) {
                B.fate = 1;
                break;
            }
            if (dy < 0.f && std::fabs(dx) < minD * 0.6f && A.state != Ball::State::Free) {
                B.state = Ball::State::OnBall;  // sticky snow: it stays on top
                B.below = A.id;
                B.stackOff = std::clamp(dx, -0.35f * A.r, 0.35f * A.r);
                B.vx = B.vy = 0.f;
                break;
            }
            const float nx = dx / d, ny = dy / d;
            B.x += nx * (minD - d);
            B.y += ny * (minD - d);
            const float vn = rvx * nx + rvy * ny;
            if (vn < 0.f) {
                B.vx -= 1.25f * vn * nx;  // soft bounce (restitution 0.25)
                B.vy -= 1.25f * vn * ny;
            }
        }
    }

    // Balls resting side by side don't overlap.
    for (size_t i = 0; i < balls_.size(); ++i)
        for (size_t j = i + 1; j < balls_.size(); ++j) {
            Ball& a = balls_[i];
            Ball& b = balls_[j];
            if (a.state != Ball::State::OnLedge || b.state != Ball::State::OnLedge) continue;
            const float dx = b.x - a.x, minD = (a.r + b.r) * 0.95f;
            if (std::fabs(dx) >= minD || std::fabs(b.y - a.y) > minD) continue;
            const float push = (minD - std::fabs(dx)) * 0.5f * (dx >= 0.f ? 1.f : -1.f);
            a.x -= push;
            b.x += push;
        }

    // Stacked balls follow the ball below (several passes for tall stacks).
    for (Ball& b : balls_)
        if (b.state == Ball::State::OnBall) b.stackOff *= std::exp(-0.6f * dt);  // slowly settles centred
    for (int pass = 0; pass < 4; ++pass)
        for (Ball& b : balls_) {
            if (b.state != Ball::State::OnBall) continue;
            Ball* a = FindBall(b.below);
            if (!a || a->fate) {
                b.state = Ball::State::Free;
                b.below = 0;
                continue;
            }
            b.x = a->x + b.stackOff;
            b.y = a->y - (a->r + b.r) * kStackSquash;
            b.vx = a->vx;
            b.vy = a->vy;
        }

    // Remove burst / lost balls.
    for (size_t i = 0; i < balls_.size();) {
        if (!balls_[i].fate) {
            ++i;
            continue;
        }
        const Ball dead = balls_[i];
        if (dead.fate == 1 || dead.fate == 3) BurstBall(dead);
        if (heldId_ == dead.id) heldId_ = 0;
        balls_.erase(balls_.begin() + (ptrdiff_t)i);
        for (Ball& b : balls_)
            if (b.state == Ball::State::OnBall && b.below == dead.id) {
                b.state = Ball::State::Free;
                b.below = 0;
            }
    }
}
