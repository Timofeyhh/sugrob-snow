#pragma once
#include "common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <dcomp.h>
#include <vector>

class World;

// Minimal COM smart pointer (works the same with MSVC and MinGW).
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { Reset(); }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** Put() {
        Reset();
        return &p_;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }
    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    template <class U>
    HRESULT As(Com<U>& out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), out.PutVoid()) : E_POINTER;
    }

private:
    T* p_ = nullptr;
};

// Draws the snow into a transparent DirectComposition swap chain bound to the overlay window.
class Renderer {
public:
    bool Init(HWND hwnd, int width, int height);
    bool Resize(int width, int height);
    bool Draw(const World* world);  // nullptr = clear (hidden / paused)
    void Shutdown() { ReleaseDevice(); }
    bool UsesSpriteBatch() const { return (bool)dc3_; }
    // Test aid: draws the given scene (fake windows) + snow into an offscreen bitmap, saves a .bmp.
    bool Snapshot(const World& world, const Scene& scene, const wchar_t* path);

private:
    bool CreateDevice();
    bool CreateDeviceImpl();
    bool CreateTarget();
    bool CreateResources();
    void ReleaseDevice();
    void DrawLedges(const World& world);
    void DrawFlakes(const World& world);
    void DrawLumps(const World& world);
    void DrawRain(const World& world);
    void DrawBalls(const World& world);
    void DrawSprites(const std::vector<D2D1_RECT_F>& rc, const std::vector<D2D1_COLOR_F>& col, Com<ID2D1SpriteBatch>& batch,
                     ID2D1Bitmap1* bitmap);
    // Snow is drawn as one body: first the outline of every piece (in a layer, so overlapping outlines don't
    // add up), then all the white on top. The outline then only shows around the outside of the union.
    void PushOutline(const D2D1_RECT_F& bounds);
    Com<ID2D1SpriteBatch> batch2_, batch3_;  // pile crest fluff: outline, white
    Com<ID2D1SpriteBatch> batch4_, batch5_;  // airborne lumps: outline, white
    std::vector<D2D1_RECT_F> fluffHalo_, fluffCore_;
    std::vector<D2D1_COLOR_F> fluffHaloCol_, fluffCoreCol_;
    std::vector<D2D1_RECT_F> lumpHalo_, lumpCore_;
    std::vector<D2D1_COLOR_F> lumpCol_;
    Com<ID2D1Bitmap1> core_, halo_, rim_;  // soft disks: white, outline colour, ball-edge colour
    Com<ID2D1RadialGradientBrush> ballFill_;
    Com<ID2D1LinearGradientBrush> ballShade_;  // the shadow side of a snowball
    Com<ID2D1SolidColorBrush> ink_;

    HWND hwnd_ = nullptr;
    int width_ = 1, height_ = 1;
    Com<ID2D1Factory1> d2dFactory_;
    Com<ID3D11Device> d3d_;
    Com<IDXGIDevice1> dxgi_;
    Com<IDXGISwapChain1> swap_;
    Com<ID2D1Device> d2dDevice_;
    Com<ID2D1DeviceContext> dc_;
    Com<ID2D1DeviceContext3> dc3_;   // sprite batches (Windows 10 1607+), optional
    Com<ID2D1SpriteBatch> batch_;
    Com<ID2D1Bitmap1> target_;
    Com<ID2D1Bitmap1> sprite_;
    Com<ID2D1SolidColorBrush> outline_;
    Com<ID2D1LinearGradientBrush> fill_;
    Com<IDCompositionDevice> dcomp_;
    Com<IDCompositionTarget> dcompTarget_;
    Com<IDCompositionVisual> visual_;
    std::vector<D2D1_POINT_2F> pts_;
    std::vector<D2D1_RECT_F> rects_;
    std::vector<D2D1_COLOR_F> colors_;
};
