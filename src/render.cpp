#include "render.h"
#include "sim.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr float kSpriteDisk = 0.6f;  // flake disk radius as a fraction of the sprite half-size
constexpr float kCoreDisk = 0.8f;    // solid snow disks (core_ / halo_ / rim_): radius as a fraction of the half-size
constexpr float kOutline[3] = {0.40f, 0.48f, 0.60f};  // blue-grey outline that keeps snow visible on white windows
constexpr float kOutlineAlpha = 0.36f;
constexpr float kPi = 3.14159265f;

float Smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

D2D1_COLOR_F Color(float r, float g, float b, float a) { return D2D1_COLOR_F{r, g, b, a}; }
D2D1_POINT_2F Pt(float x, float y) { return D2D1_POINT_2F{x, y}; }

// Deterministic per-cell randomness (no flicker between frames).
uint32_t Hash(uint32_t a) {
    a ^= a >> 16;
    a *= 0x7feb352dU;
    a ^= a >> 15;
    a *= 0x846ca68bU;
    a ^= a >> 16;
    return a;
}

}  // namespace

bool Renderer::Init(HWND hwnd, int width, int height) {
    hwnd_ = hwnd;
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 d2dFactory_.PutVoid())))
        return false;
    return CreateDevice();
}

bool Renderer::CreateDevice() {
    if (CreateDeviceImpl()) return true;
    ReleaseDevice();
    return false;
}

bool Renderer::CreateDeviceImpl() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d_.Put(), nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d_.Put(), nullptr, nullptr);
    if (FAILED(hr)) return false;
    if (FAILED(d3d_.As(dxgi_))) return false;
    dxgi_->SetMaximumFrameLatency(1);

    Com<IDXGIAdapter> adapter;
    if (FAILED(dxgi_->GetAdapter(adapter.Put()))) return false;
    Com<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.PutVoid()))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)width_;
    sd.Height = (UINT)height_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(d3d_.Get(), &sd, nullptr, swap_.Put()))) return false;

    if (FAILED(d2dFactory_->CreateDevice(dxgi_.Get(), d2dDevice_.Put()))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.Put()))) return false;
    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    if (SUCCEEDED(dc_.As(dc3_)) &&
        (FAILED(dc3_->CreateSpriteBatch(batch_.Put())) || FAILED(dc3_->CreateSpriteBatch(batch2_.Put())) ||
         FAILED(dc3_->CreateSpriteBatch(batch3_.Put())) || FAILED(dc3_->CreateSpriteBatch(batch4_.Put())) ||
         FAILED(dc3_->CreateSpriteBatch(batch5_.Put()))))
        dc3_.Reset();
    if (!CreateTarget() || !CreateResources()) return false;

    if (FAILED(DCompositionCreateDevice(dxgi_.Get(), __uuidof(IDCompositionDevice), dcomp_.PutVoid()))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, dcompTarget_.Put()))) return false;
    if (FAILED(dcomp_->CreateVisual(visual_.Put()))) return false;
    if (FAILED(visual_->SetContent(swap_.Get()))) return false;
    if (FAILED(dcompTarget_->SetRoot(visual_.Get()))) return false;
    return SUCCEEDED(dcomp_->Commit());
}

bool Renderer::CreateTarget() {
    Com<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, __uuidof(IDXGISurface), surface.PutVoid()))) return false;
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.f;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, target_.Put()))) return false;
    dc_->SetTarget(target_.Get());
    return true;
}

bool Renderer::CreateResources() {
    // Pile fill: white on top, slightly blue at the bottom; a soft blue-grey outline
    // keeps snow visible on white title bars.
    const D2D1_GRADIENT_STOP stops[2] = {{0.f, Color(1.f, 1.f, 1.f, 1.f)}, {1.f, Color(0.84f, 0.90f, 0.97f, 1.f)}};
    Com<ID2D1GradientStopCollection> coll;
    ID2D1RenderTarget* rt = dc_.Get();
    if (FAILED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.Put())))
        return false;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp{Pt(0.f, 0.f), Pt(0.f, 1.f)};
    if (FAILED(rt->CreateLinearGradientBrush(&lp, nullptr, coll.Get(), fill_.Put()))) return false;
    const D2D1_COLOR_F outline = Color(0.40f, 0.48f, 0.60f, 0.6f);
    if (FAILED(rt->CreateSolidColorBrush(&outline, nullptr, outline_.Put()))) return false;
    if (FAILED(rt->CreateSolidColorBrush(&outline, nullptr, ink_.Put()))) return false;
    // Snowball shading: lit from the top-left (a soft highlight), with a cool blue shadow on the far side.
    const D2D1_GRADIENT_STOP bstops[4] = {{0.f, Color(1.f, 1.f, 1.f, 1.f)},
                                          {0.45f, Color(0.97f, 0.98f, 1.f, 1.f)},
                                          {0.82f, Color(0.89f, 0.93f, 0.98f, 1.f)},
                                          {1.f, Color(0.81f, 0.87f, 0.95f, 1.f)}};
    Com<ID2D1GradientStopCollection> bcoll;
    if (FAILED(rt->CreateGradientStopCollection(bstops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, bcoll.Put())))
        return false;
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp{Pt(0.f, 0.f), Pt(0.f, 0.f), 1.f, 1.f};
    if (FAILED(rt->CreateRadialGradientBrush(&rp, nullptr, bcoll.Get(), ballFill_.Put()))) return false;
    const D2D1_GRADIENT_STOP sstops[3] = {{0.f, Color(0.30f, 0.42f, 0.65f, 0.f)},
                                          {0.5f, Color(0.30f, 0.42f, 0.65f, 0.f)},
                                          {1.f, Color(0.26f, 0.38f, 0.62f, 0.30f)}};
    Com<ID2D1GradientStopCollection> scoll;
    if (FAILED(rt->CreateGradientStopCollection(sstops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, scoll.Put())))
        return false;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES sp{Pt(0.f, 0.f), Pt(1.f, 1.f)};
    if (FAILED(rt->CreateLinearGradientBrush(&sp, nullptr, scoll.Get(), ballShade_.Put()))) return false;

    // Soft solid disks for snow drawn as one body (see PushOutline): white, the outline colour, the ball edge colour.
    {
        const int M = 32;
        auto disk = [&](float cr, float cg, float cb, Com<ID2D1Bitmap1>& out) -> bool {
            std::vector<uint32_t> px((size_t)M * M);
            for (int y = 0; y < M; ++y)
                for (int x = 0; x < M; ++x) {
                    const float dx = (x + 0.5f) / (M * 0.5f) - 1.f;
                    const float dy = (y + 0.5f) / (M * 0.5f) - 1.f;
                    const float a = 1.f - Smooth(kCoreDisk - 0.08f, kCoreDisk + 0.06f, std::sqrt(dx * dx + dy * dy));
                    auto u8 = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
                    px[(size_t)y * M + x] = (u8(a) << 24) | (u8(cr * a) << 16) | (u8(cg * a) << 8) | u8(cb * a);
                }
            D2D1_BITMAP_PROPERTIES1 dp{};
            dp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            dp.dpiX = dp.dpiY = 96.f;
            return SUCCEEDED(dc_->CreateBitmap(D2D1_SIZE_U{(UINT32)M, (UINT32)M}, px.data(), M * 4, &dp, out.Put()));
        };
        if (!disk(1.f, 1.f, 1.f, core_) || !disk(kOutline[0], kOutline[1], kOutline[2], halo_) ||
            !disk(0.87f, 0.91f, 0.965f, rim_))
            return false;
    }

    // Flake sprite: white disk with a faint blue-grey halo (visible on light backgrounds too).
    const int N = 32;
    std::vector<uint32_t> px((size_t)N * N);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float dx = (x + 0.5f) / (N * 0.5f) - 1.f;
            const float dy = (y + 0.5f) / (N * 0.5f) - 1.f;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float inner = 1.f - Smooth(kSpriteDisk - 0.12f, kSpriteDisk + 0.04f, d);
            const float rim = (1.f - Smooth(kSpriteDisk + 0.02f, 0.9f, d)) * 0.2f * (1.f - inner);
            const float a = inner + rim;  // premultiplied: white * inner + rimColor * rim
            const float r = inner + rim * 0.33f;
            const float g = inner + rim * 0.42f;
            const float b = inner + rim * 0.56f;
            auto u8 = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
            px[(size_t)y * N + x] = (u8(a) << 24) | (u8(r) << 16) | (u8(g) << 8) | u8(b);
        }
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.f;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    return SUCCEEDED(dc_->CreateBitmap(D2D1_SIZE_U{(UINT32)N, (UINT32)N}, px.data(), N * 4, &bp, sprite_.Put()));
}

void Renderer::ReleaseDevice() {
    if (dc_) dc_->SetTarget(nullptr);
    sprite_.Reset();
    core_.Reset();
    halo_.Reset();
    rim_.Reset();
    batch3_.Reset();
    batch4_.Reset();
    batch5_.Reset();
    ballShade_.Reset();
    ballFill_.Reset();
    ink_.Reset();
    fill_.Reset();
    outline_.Reset();
    target_.Reset();
    batch_.Reset();
    batch2_.Reset();
    dc3_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    swap_.Reset();
    dxgi_.Reset();
    d3d_.Reset();
}

bool Renderer::Resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    if (!swap_ || !dc_) return true;  // recreated with the new size on the next Draw
    dc_->SetTarget(nullptr);
    target_.Reset();
    if (FAILED(swap_->ResizeBuffers(0, (UINT)width_, (UINT)height_, DXGI_FORMAT_UNKNOWN, 0)) || !CreateTarget()) {
        ReleaseDevice();
        return false;
    }
    return true;
}

bool Renderer::Draw(const World* world) {
    if (!dc_ && !CreateDevice()) return false;
    dc_->BeginDraw();
    const D2D1_COLOR_F clear = Color(0.f, 0.f, 0.f, 0.f);
    dc_->Clear(&clear);
    if (world) {
        DrawLedges(*world);
        DrawBalls(*world);
        DrawFlakes(*world);
    }
    HRESULT hr = dc_->EndDraw(nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = swap_->Present(1, 0);
    if (FAILED(hr)) {
        ReleaseDevice();  // device lost (driver update, GPU reset): rebuilt on the next frame
        return false;
    }
    return true;
}

bool Renderer::Snapshot(const World& world, const Scene& scene, const wchar_t* path) {
    if (!dc_) return false;
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.f;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    const D2D1_SIZE_U size{(UINT32)width_, (UINT32)height_};
    Com<ID2D1Bitmap1> off, cpu;
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &bp, off.Put()))) return false;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &bp, cpu.Put()))) return false;

    Com<ID2D1SolidColorBrush> brush;
    const D2D1_COLOR_F c0 = Color(0, 0, 0, 1);
    if (FAILED(dc_->CreateSolidColorBrush(&c0, nullptr, brush.Put()))) return false;

    dc_->SetTarget(off.Get());
    dc_->BeginDraw();
    const D2D1_COLOR_F desk = Color(0.16f, 0.30f, 0.45f, 1.f);
    dc_->Clear(&desk);
    for (size_t i = scene.windows.size(); i-- > 0;) {  // bottom-most first
        const RECT& r = scene.windows[i].r;
        const D2D1_RECT_F rc{(float)r.left, (float)r.top, (float)r.right, (float)r.bottom};
        if (scene.windows[i].taskbar) {
            brush->SetColor(Color(0.10f, 0.11f, 0.13f, 1.f));
            dc_->FillRectangle(&rc, brush.Get());
            continue;
        }
        brush->SetColor(Color(0.97f, 0.97f, 0.97f, 1.f));
        dc_->FillRectangle(&rc, brush.Get());
        const D2D1_RECT_F title{rc.left, rc.top, rc.right, rc.top + 32.f};
        brush->SetColor(Color(0.90f, 0.91f, 0.93f, 1.f));
        dc_->FillRectangle(&title, brush.Get());
        brush->SetColor(Color(0.45f, 0.47f, 0.50f, 1.f));
        dc_->DrawRectangle(&rc, brush.Get(), 1.f, nullptr);
    }
    DrawLedges(world);
    DrawBalls(world);
    DrawFlakes(world);
    HRESULT hr = dc_->EndDraw(nullptr, nullptr);
    dc_->SetTarget(target_.Get());
    if (FAILED(hr) || FAILED(cpu->CopyFromBitmap(nullptr, off.Get(), nullptr))) return false;

    D2D1_MAPPED_RECT m{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD rowBytes = (DWORD)width_ * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = width_;
    ih.biHeight = -height_;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * (DWORD)height_;
    bool ok = false;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(f, &fh, sizeof(fh), &wr, nullptr) && WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
        for (int y = 0; ok && y < height_; ++y) ok = WriteFile(f, m.bits + (size_t)y * m.pitch, rowBytes, &wr, nullptr) != 0;
        CloseHandle(f);
    }
    cpu->Unmap();
    return ok;
}

void Renderer::DrawSprites(const std::vector<D2D1_RECT_F>& rc, const std::vector<D2D1_COLOR_F>& col,
                           Com<ID2D1SpriteBatch>& batch, ID2D1Bitmap1* bitmap) {
    if (rc.empty() || !bitmap) return;
    if (dc3_ && batch) {
        batch->Clear();
        batch->AddSprites((UINT32)rc.size(), rc.data(), nullptr, col.data(), nullptr, sizeof(D2D1_RECT_F),
                          sizeof(D2D1_RECT_U), sizeof(D2D1_COLOR_F), sizeof(D2D1_MATRIX_3X2_F));
        dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // required for sprite batches
        dc3_->DrawSpriteBatch(batch.Get(), 0, batch->GetSpriteCount(), bitmap, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                              D2D1_SPRITE_OPTIONS_NONE);
        dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        return;
    }
    for (size_t i = 0; i < rc.size(); ++i)
        dc_->DrawBitmap(bitmap, &rc[i], col[i].a, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
}

void Renderer::PushOutline(const D2D1_RECT_F& bounds) {
    D2D1_LAYER_PARAMETERS1 lp{};
    lp.contentBounds = bounds;
    lp.geometricMask = nullptr;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform._11 = 1.f;
    lp.maskTransform._22 = 1.f;
    lp.opacity = kOutlineAlpha;
    lp.opacityBrush = nullptr;
    lp.layerOptions = D2D1_LAYER_OPTIONS1_NONE;
    dc_->PushLayer(&lp, nullptr);
    ink_->SetColor(Color(kOutline[0], kOutline[1], kOutline[2], 1.f));
}

namespace {
struct Bounds {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    void Add(const D2D1_RECT_F& r) {
        x0 = std::min(x0, r.left);
        y0 = std::min(y0, r.top);
        x1 = std::max(x1, r.right);
        y1 = std::max(y1, r.bottom);
    }
    void Add(float x, float y, float pad) { Add(D2D1_RECT_F{x - pad, y - pad, x + pad, y + pad}); }
    bool Empty() const { return x1 < x0; }
    D2D1_RECT_F Rect() const { return D2D1_RECT_F{std::floor(x0) - 2.f, std::floor(y0) - 2.f, std::ceil(x1) + 2.f, std::ceil(y1) + 2.f}; }
};
D2D1_RECT_F Square(float x, float y, float half) { return D2D1_RECT_F{x - half, y - half, x + half, y + half}; }
}  // namespace

void Renderer::DrawLedges(const World& world) {
    const int cw = world.CellW();
    const float sc = world.Scale();
    const float rim = std::max(1.f, sc) * 0.9f;  // width of the outline outside the snow
    const float k = 1.f / kCoreDisk;
    ID2D1Factory* factory = d2dFactory_.Get();
    fluffHalo_.clear();
    fluffCore_.clear();
    fluffHaloCol_.clear();
    fluffCoreCol_.clear();
    struct Fill {
        ID2D1PathGeometry* geo;
        float base, top;
    };
    std::vector<Fill> fills;
    Bounds bounds;
    Com<ID2D1PathGeometry> line;  // the crests of all piles, outlined in one go
    Com<ID2D1GeometrySink> lsink;
    if (FAILED(factory->CreatePathGeometry(line.Put())) || FAILED(line->Open(lsink.Put()))) return;
    bool anyLine = false;
    for (const Ledge& l : world.Ledges()) {
        const int n = (int)l.h.size();
        if (n == 0 || l.vis.size() != (size_t)n || l.cap.size() != (size_t)n) continue;
        auto H = [&](int c) -> float {
            if (c < 0 || c >= n || !l.vis[(size_t)c]) return 0.f;
            return std::min(l.h[(size_t)c], l.cap[(size_t)c]);
        };
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geo))) break;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(sink.Put()))) {
            geo->Release();
            break;
        }
        const float base = (float)l.y + l.yOff;  // yOff: a ghost slab that is falling away
        float top = 0.f;
        bool any = false;
        const size_t fluff0 = fluffCore_.size();
        int i = 0;
        while (i < n) {
            if (H(i) < 0.4f) {
                ++i;
                continue;
            }
            int j = i;
            while (j < n && H(j) >= 0.4f) ++j;
            pts_.clear();
            for (int c = i; c < j; ++c) {
                const float hs = (H(c - 1) + 2.f * H(c) + H(c + 1)) * 0.25f;
                top = std::max(top, hs);
                pts_.push_back(Pt(l.left + (c + 0.5f) * cw, base - hs));
                bounds.Add(pts_.back().x, pts_.back().y, 4.f * rim);
            }
            // Fluff: soft lumps along the crest. They are part of the same white body as the pile, so the crest
            // reads as fluffy snow, not as a row of separate balls.
            for (int c = i; c < j; c += 2) {
                const float crest = pts_[(size_t)(c - i)].y;
                const float depth = base - crest;
                if (depth < 1.f * sc) continue;
                const uint32_t hh = Hash((uint32_t)c * 9781u + (uint32_t)(l.key & 0xFFFFu) * 6271u + 17u);
                const float u1 = (hh & 255u) / 255.f, u2 = ((hh >> 8) & 255u) / 255.f, u3 = ((hh >> 16) & 255u) / 255.f;
                const float rr = (1.2f + 1.8f * u1) * sc * std::min(1.f, 0.4f + depth / (6.f * sc));
                const float cx = l.left + (c + 0.5f) * cw + (u2 - 0.5f) * 2.f * cw;
                const float cy = crest + rr * 0.15f - u3 * 1.2f * sc;
                fluffCore_.push_back(Square(cx, cy, rr * k));
                fluffHalo_.push_back(Square(cx, cy, (rr + rim) * k));
                fluffCoreCol_.push_back(Color(1.f, 1.f, 1.f, 1.f));  // tinted below, once the pile's top is known
                bounds.Add(fluffHalo_.back());
            }
            pts_.push_back(Pt((float)(l.left + j * cw), base));
            sink->BeginFigure(Pt((float)(l.left + i * cw), base), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(pts_.data(), (UINT32)pts_.size());
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (pts_.size() > 2) {  // outline only along the top, not around the whole pile
                lsink->BeginFigure(pts_[0], D2D1_FIGURE_BEGIN_HOLLOW);
                lsink->AddLines(pts_.data() + 1, (UINT32)(pts_.size() - 2));
                lsink->EndFigure(D2D1_FIGURE_END_OPEN);
                anyLine = true;
            }
            any = true;
            i = j;
        }
        sink->Close();
        if (!any) {
            geo->Release();
            continue;
        }
        // Fluff takes the colour of the pile's fill at its height (white at the top, a little blue lower down).
        for (size_t q = fluff0; q < fluffCore_.size(); ++q) {
            const float cy = (fluffCore_[q].top + fluffCore_[q].bottom) * 0.5f;
            const float t = top > 0.f ? std::clamp((cy - (base - top)) / top, 0.f, 1.f) : 0.f;
            fluffCoreCol_[q] = Color(1.f - 0.16f * t, 1.f - 0.10f * t, 1.f - 0.03f * t, 1.f);
        }
        fills.push_back(Fill{geo, base, top});
    }
    lsink->Close();
    fluffHaloCol_.assign(fluffHalo_.size(), Color(1.f, 1.f, 1.f, 1.f));
    if (!bounds.Empty()) {
        PushOutline(bounds.Rect());
        if (anyLine) dc_->DrawGeometry(line.Get(), ink_.Get(), 2.f * rim, nullptr);  // half of it ends up under the snow
        DrawSprites(fluffHalo_, fluffHaloCol_, batch2_, halo_.Get());
        dc_->PopLayer();
    }
    for (const Fill& f : fills) {
        fill_->SetStartPoint(Pt(0.f, f.base - f.top));
        fill_->SetEndPoint(Pt(0.f, f.base));
        dc_->FillGeometry(f.geo, fill_.Get(), nullptr);
        f.geo->Release();
    }
    DrawSprites(fluffCore_, fluffCoreCol_, batch3_, core_.Get());
}

void Renderer::DrawBalls(const World& world) {
    const auto& balls = world.Balls();
    if (balls.empty()) return;
    const float stroke = std::max(1.f, world.Scale());
    // Lower balls first, so a snowman's head is drawn over its body.
    std::vector<size_t> order(balls.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return balls[a].y > balls[b].y; });
    auto dot = [&](float x, float y, float r, D2D1_COLOR_F c) {
        ink_->SetColor(c);
        const D2D1_ELLIPSE e{Pt(x, y), r, r};
        dc_->FillEllipse(&e, ink_.Get());
    };
    ID2D1Factory* factory = d2dFactory_.Get();
    for (size_t idx : order) {
        const Ball& b = balls[idx];
        const float r = b.r;
        if (b.state == Ball::State::OnLedge) {  // soft contact shadow
            ink_->SetColor(Color(0.05f, 0.1f, 0.2f, 0.16f));
            const D2D1_ELLIPSE sh{Pt(b.x, b.y + r * 0.92f), r * 0.85f, std::max(1.5f, r * 0.16f)};
            dc_->FillEllipse(&sh, ink_.Get());
        }
        // A lumpy, hand-packed outline (not a perfect circle) that turns as the ball rolls.
        const uint32_t hb = Hash(b.id * 2654435761u + 11u);
        const float p1 = (float)(hb & 255u) / 40.f, p2 = (float)((hb >> 8) & 255u) / 40.f;
        const float p3 = (float)((hb >> 16) & 255u) / 40.f, p4 = (float)((hb >> 24) & 255u) / 40.f;
        const int np = std::clamp((int)(r * 1.2f), 40, 220);
        pts_.clear();
        for (int i = 0; i < np; ++i) {
            const float th = (float)i * (2.f * kPi / (float)np);
            const float t = th - b.spin;
            const float rr = r * (1.f + 0.024f * std::sin(7.f * t + p1) + 0.016f * std::sin(12.f * t + p2) +
                                  0.010f * std::sin(19.f * t + p3) + 0.006f * std::sin(31.f * t + p4));
            pts_.push_back(Pt(b.x + std::cos(th) * rr, b.y + std::sin(th) * rr));
        }
        Com<ID2D1PathGeometry> shape;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(shape.Put())) || FAILED(shape->Open(sink.Put()))) continue;
        sink->BeginFigure(pts_[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts_.data() + 1, (UINT32)(pts_.size() - 1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        ink_->SetColor(Color(kOutline[0], kOutline[1], kOutline[2], kOutlineAlpha));
        dc_->DrawGeometry(shape.Get(), ink_.Get(), stroke * 1.6f, nullptr);  // half of it ends up under the fill
        ballFill_->SetCenter(Pt(b.x, b.y));
        ballFill_->SetGradientOriginOffset(Pt(-0.38f * r, -0.42f * r));
        ballFill_->SetRadiusX(r * 1.06f);
        ballFill_->SetRadiusY(r * 1.06f);
        dc_->FillGeometry(shape.Get(), ballFill_.Get(), nullptr);
        ballShade_->SetStartPoint(Pt(b.x - 0.75f * r, b.y - 0.75f * r));
        ballShade_->SetEndPoint(Pt(b.x + 0.75f * r, b.y + 0.75f * r));
        dc_->FillGeometry(shape.Get(), ballShade_.Get(), nullptr);

        // Texture of rolled snow (it all turns with the ball): little lumps stuck to the surface, each lit from the
        // top-left with a soft shadow on its lower right, plus a fine grain. Smaller towards the edge (it is a sphere).
        if (r > 6.f) {
            auto onBall = [&](uint32_t h, float spread, float& x, float& y, float& fore) {
                const float u = (float)(h & 1023u) / 1023.f, v = (float)((h >> 10) & 1023u) / 1023.f;
                const float rho = r * spread * std::sqrt(u);
                const float th = v * 2.f * kPi + b.spin;
                x = b.x + std::cos(th) * rho;
                y = b.y + std::sin(th) * rho;
                fore = std::sqrt(std::max(0.f, 1.f - (rho / r) * (rho / r)));
            };
            const int nl = std::clamp((int)(r * 0.18f), 3, 40);
            for (int i = 0; i < nl; ++i) {
                const uint32_t h = Hash(b.id * 7919u + (uint32_t)i * 104729u + 5u);
                float x, y, fore;
                onBall(h, 0.84f, x, y, fore);
                const float w = (float)((h >> 20) & 255u) / 255.f;
                const float s = std::max(1.f, r * (0.03f + 0.045f * w) * (0.4f + 0.6f * fore));
                const D2D1_RECT_F sh = Square(x + 0.45f * s, y + 0.55f * s, s * 1.1f / kCoreDisk);
                dc_->DrawBitmap(halo_.Get(), &sh, 0.22f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
                const D2D1_RECT_F lump = Square(x, y, s / kCoreDisk);
                dc_->DrawBitmap(core_.Get(), &lump, 0.55f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            }
            const int ng = std::clamp((int)(r * 0.9f), 10, 160);
            for (int i = 0; i < ng; ++i) {
                const uint32_t h = Hash(b.id * 31337u + (uint32_t)i * 7907u + 9u);
                float x, y, fore;
                onBall(h, 0.92f, x, y, fore);
                const float w = (float)((h >> 20) & 255u) / 255.f;
                const D2D1_RECT_F gr = Square(x, y, (0.55f + 0.5f * w) * stroke / kCoreDisk);
                dc_->DrawBitmap(halo_.Get(), &gr, 0.10f + 0.10f * w, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            }
        }
    }
    // Snowman: the top ball of a stack gets a face, the ball under it buttons and twig arms.
    const POINT cur = world.Cursor();
    for (const Ball& h : balls) {
        if (h.state != Ball::State::OnBall) continue;
        bool hasTop = false;
        for (const Ball& o : balls) hasTop |= (o.state == Ball::State::OnBall && o.below == h.id);
        if (hasTop) continue;
        const Ball* body = nullptr;
        for (const Ball& o : balls)
            if (o.id == h.below) body = &o;
        const float r = h.r;
        const float look = std::clamp((cur.x - h.x) / (200.f * world.Scale()), -1.f, 1.f);
        const float dir = look >= 0.f ? 1.f : -1.f;
        const D2D1_COLOR_F coal = Color(0.08f, 0.08f, 0.10f, 1.f);
        dot(h.x - 0.28f * r + look * 0.1f * r, h.y - 0.22f * r, std::max(1.2f, 0.09f * r), coal);
        dot(h.x + 0.28f * r + look * 0.1f * r, h.y - 0.22f * r, std::max(1.2f, 0.09f * r), coal);
        for (int k = 0; k < 5; ++k) {
            const float t = 3.14159f * (0.25f + 0.125f * k);
            dot(h.x + look * 0.1f * r + std::cos(t) * 0.3f * r, h.y + 0.12f * r + std::sin(t) * 0.22f * r,
                std::max(0.8f, 0.045f * r), coal);
        }
        Com<ID2D1PathGeometry> nose;  // carrot, pointing towards the cursor
        Com<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2dFactory_.Get()->CreatePathGeometry(nose.Put())) && SUCCEEDED(nose->Open(sink.Put()))) {
            const float nx = h.x + look * 0.12f * r, ny = h.y + 0.02f * r;
            sink->BeginFigure(Pt(nx, ny - 0.09f * r), D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F p2[2] = {Pt(nx + dir * 0.55f * r, ny + 0.06f * r), Pt(nx, ny + 0.09f * r)};
            sink->AddLines(p2, 2);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            ink_->SetColor(Color(1.f, 0.55f, 0.15f, 1.f));
            dc_->FillGeometry(nose.Get(), ink_.Get(), nullptr);
        }
        if (!body) continue;
        const float R = body->r;
        for (int k = -1; k <= 1; ++k) dot(body->x, body->y + k * 0.3f * R, std::max(1.f, 0.07f * R), coal);
        ink_->SetColor(Color(0.40f, 0.26f, 0.13f, 1.f));
        const float w = std::max(1.5f, 0.07f * R);
        for (float s = -1.f; s <= 1.f; s += 2.f) {
            const D2D1_POINT_2F a = Pt(body->x + s * 0.82f * R, body->y - 0.2f * R);
            const D2D1_POINT_2F e = Pt(body->x + s * 1.55f * R, body->y - 0.75f * R);
            const D2D1_POINT_2F m = Pt(a.x + (e.x - a.x) * 0.7f, a.y + (e.y - a.y) * 0.7f);
            dc_->DrawLine(a, e, ink_.Get(), w, nullptr);
            dc_->DrawLine(m, Pt(m.x + s * 0.18f * R, m.y - 0.3f * R), ink_.Get(), w * 0.8f, nullptr);
        }
    }
}

void Renderer::DrawFlakes(const World& world) {
    const auto& flakes = world.Flakes();
    if (flakes.empty()) return;
    const float k = 1.f / kSpriteDisk;
    // Flakes fade in over the first ~100 dip so the top edge of the screen stays calm.
    const float fadeIn = 1.f / (100.f * world.Scale());
    rects_.clear();
    colors_.clear();
    bool anyRain = false;
    for (const Flake& f : flakes) {
        if (f.clump) continue;  // lumps of snow: DrawLumps
        if (f.rain) {
            anyRain = true;
            continue;
        }
        const float hs = f.r * k;
        const float a = f.alpha * std::clamp((f.y + 10.f * world.Scale()) * fadeIn, 0.f, 1.f);
        rects_.push_back(D2D1_RECT_F{f.x - hs, f.y - hs, f.x + hs, f.y + hs});
        colors_.push_back(Color(a, a, a, a));  // premultiplied fade
    }
    DrawSprites(rects_, colors_, batch_, sprite_.Get());
    if (anyRain) DrawRain(world);
    DrawLumps(world);
}

// Raindrops: thin streaks along their motion, in three depth bands (far ones fainter and thinner).
void Renderer::DrawRain(const World& world) {
    const float sc = world.Scale();
    ID2D1Factory* factory = d2dFactory_.Get();
    for (int band = 0; band < 3; ++band) {
        Com<ID2D1PathGeometry> geo;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(geo.Put())) || FAILED(geo->Open(sink.Put()))) return;
        bool any = false;
        for (const Flake& f : world.Flakes()) {
            if (!f.rain || std::min(2, (int)(f.z * 3.f)) != band) continue;
            const float sp = std::sqrt(f.vx * f.vx + f.vy * f.vy);
            if (sp < 1.f) continue;
            const float len = f.life > 0.f ? 2.5f * sc : std::clamp(sp * 0.024f, 5.f * sc, 22.f * sc);
            sink->BeginFigure(Pt(f.x, f.y), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(Pt(f.x - f.vx / sp * len, f.y - f.vy / sp * len));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            any = true;
        }
        sink->Close();
        if (!any) continue;
        const float a = 0.22f + 0.2f * (float)band;
        ink_->SetColor(Color(0.62f, 0.72f, 0.86f, a));
        dc_->DrawGeometry(geo.Get(), ink_.Get(), (0.8f + 0.3f * (float)band) * std::max(1.f, sc), nullptr);
    }
}

// Lumps of snow in the air (shaken off, thrown, crumbling). A bigger lump is an irregular cluster that tumbles
// as it flies, and lumps flying close together merge into one piece of snow with a single outline.
void Renderer::DrawLumps(const World& world) {
    const float sc = world.Scale();
    const float rim = std::max(1.f, sc) * 0.8f;
    const float k = 1.f / kCoreDisk;
    lumpHalo_.clear();
    lumpCore_.clear();
    lumpCol_.clear();
    Bounds bounds;
    auto add = [&](float x, float y, float r, float a) {
        lumpCore_.push_back(Square(x, y, r * k));
        lumpHalo_.push_back(Square(x, y, (r + rim) * k));
        lumpCol_.push_back(Color(a, a, a, a));
        bounds.Add(lumpHalo_.back());
    };
    for (const Flake& f : world.Flakes()) {
        if (!f.clump) continue;
        const float a = f.alpha;
        // Loose snow takes more room than the packed pile it came from.
        const float rv = std::clamp(std::sqrt(f.vol / kPi) * 1.15f, 0.9f * sc, 12.f * sc);
        if (rv < 2.f * sc) {
            add(f.x, f.y, rv, a);
            continue;
        }
        const uint32_t hh = Hash((uint32_t)(f.phase * 100000.f) + 77u);
        const float ang = f.phase + (f.x + f.y) * 0.012f / sc;  // tumbles as it flies
        add(f.x, f.y, rv * 0.68f, a);
        const int parts = 3 + (int)(hh & 1u) + (rv > 5.f * sc ? 1 : 0);
        for (int p = 0; p < parts; ++p) {
            const float u = (float)((hh >> (3 + p * 5)) & 31u) / 31.f;
            const float aa = ang + (float)p * (2.f * kPi / (float)parts) + (u - 0.5f) * 0.9f;
            const float d = rv * (0.38f + 0.22f * u);
            add(f.x + std::cos(aa) * d, f.y + std::sin(aa) * d, rv * (0.40f + 0.22f * (1.f - u)), a);
        }
    }
    if (lumpCore_.empty()) return;
    PushOutline(bounds.Rect());
    DrawSprites(lumpHalo_, lumpCol_, batch4_, halo_.Get());
    dc_->PopLayer();
    DrawSprites(lumpCore_, lumpCol_, batch5_, core_.Get());
}
