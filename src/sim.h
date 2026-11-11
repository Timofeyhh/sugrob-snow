#pragma once
#include "common.h"

// A falling particle: either a drifting snowflake or a ballistic clump
// (snow shaken off a window, thrown by the cursor, sliding off an edge, splash).
struct Flake {
    float x, y;
    float vx, vy;   // velocity, px/s
    float vt;       // flakes: terminal fall speed in still air
    float drag;     // flakes: how fast the flake follows the air (1/s)
    float r;        // radius, px
    float z;        // depth: 0 = far away, 1 = right in front of the screen
    float phase, freq, sway;
    float alpha;
    float vol;      // snow area (px^2) added to a pile on landing
    bool clump;
    bool lands;     // near flakes and clumps settle on windows; far flakes fly past
    bool rain;      // a raindrop (sleet): falls fast, does not pile up, slowly melts the snow it hits
    float life;     // raindrop splashes: seconds left (0 = not a splash)
};

// Snow lying on a horizontal edge: the top of a window, the taskbar,
// or the bottom of the screen ("floor").
struct Ledge {
    uint64_t key = 0;
    bool floor = false;
    bool seen = false;
    int left = 0, width = 0, y = 0;  // overlay px
    double t = 0.0;                  // time of the last scan (for window velocity)
    std::vector<float> h;            // snow height per cell
    std::vector<float> cap;          // max height per cell (room below a window above, end taper, heaps)
    float fall = 0.f;                // snowfall piles up only to this depth; heaps of thrown / broken snow up to cap
    std::vector<uint8_t> vis;        // cell not covered by a window above
    float spill = 0.f;               // overflow waiting to slide off as a clump
    bool dying = false;              // ghost of a closed / resized window's snow: crumbles away gradually
    float age = 0.f;                 // ghosts: seconds since the window went away
    int dir = 1;                     // ghosts: the crumbling wave runs left->right (1) or right->left (-1)
    float yOff = 0.f;                // ghosts: how far the falling slab of snow has dropped
    float vy = 0.f;                  // ghosts: its fall speed
};

// A snowball. Rolled from a pile with the mouse, thrown, stacked into a snowman.
struct Ball {
    enum class State : uint8_t { Free, OnLedge, OnBall, Held };
    uint32_t id = 0;
    float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f;
    float vol = 0.f;       // snow area, px^2 (r = sqrt(vol / pi))
    float r = 0.f;
    float spin = 0.f;      // rolling angle (for the speckles)
    State state = State::Free;
    uint64_t ledgeKey = 0; // OnLedge (and Held while rolling): the edge it rests on
    uint32_t below = 0;    // OnBall: the ball it sits on
    float stackOff = 0.f;  // OnBall: x offset from the ball below
    float grabDx = 0.f, grabDy = 0.f;
    bool rolling = false;  // Held and rolling along a pile (gathers snow)
    uint8_t fate = 0;      // 1 = bursts in the air, 2 = lost off screen, 3 = smashed onto the edge ledgeKey
};

class World {
public:
    void Init(float scale, int width, int height);
    void Resize(int width, int height);
    void ApplyScene(const Scene& scene, double now);
    void Update(float dt, const Settings& s, POINT cursor);
    void ClearSnow();
    void ResetInput() { cursorInit_ = false; }

    // Mouse: press on a snowball grabs it; press on a pile starts a new snowball.
    // Returns false when there is nothing to grab (the click then goes to the window below).
    bool TryGrab(POINT p);
    void Release();  // let go: the ball drops, or flies with the cursor's speed
    bool Holding() const { return heldId_ != 0; }
    void DebugDropBall(float x, float y, float radiusDip);  // test aid

    const std::vector<Flake>& Flakes() const { return flakes_; }
    const std::vector<Ledge>& Ledges() const { return ledges_; }
    const std::vector<Ball>& Balls() const { return balls_; }
    POINT Cursor() const { return prevCursor_; }
    int CellW() const { return cellW_; }
    float Scale() const { return scale_; }
    int Height() const { return height_; }

private:
    void UpdateBalls(float dt);
    void UpdateDying(float dt);
    bool AddGhost(const Ledge& src, std::vector<Ledge>& out, int dir = 0);  // dir 0 = random crumbling direction
    bool ResizeLedge(Ledge& l, int left, int width, std::vector<Ledge>& ghosts);  // keeps the snow that still has support
    void DropCells(const Ledge& l, int a, int b, int dir, std::vector<Ledge>& ghosts);  // snow of cells [a, b) falls off
    bool PilePick(float px, float py, Ledge*& out, float& vol, int& c0, int& c1);
    Ball* FindBall(uint32_t id);
    bool IsAbove(const Ball& b, uint32_t baseId);  // b sits (directly or not) on baseId
    void BurstBall(const Ball& b);
    void FreeBallsOn(uint64_t ledgeKey, float vx, float vy);
    void ShiftBallsOn(uint64_t ledgeKey, float dx);
    Ledge* FindLedge(uint64_t key);
    void PlaceLedge(Ledge& l, int left, int width, int y);
    void BuildMask(Ledge& l, const Scene& scene, int zIndex, float maxH);
    void Shed(Ledge& l, float fraction, float vx, float vy);
    bool Deposit(Ledge& l, int cell, float vol, bool heap = false);  // heap: may pile up above the snowfall depth
    void DepositHeap(Ledge& l, float x, float vol, float halfWidth);  // a smashed snowball's heap
    void Land(Ledge& l, int cell, const Flake& f);
    void Rain(Ledge& l, int cell, const Flake& f);  // a raindrop hits a pile: melts a little snow, splashes
    void Impact(Ledge& l, int cell, const Flake& f);  // a landing lump shoves the snow ahead of it
    void CoupleSnow(float dt);                        // airborne lumps exchange momentum (move as one body)
    void DragByClumps(Flake& f, float dt);            // a fast lump drags the flakes it passes
    int GridX(float x) const;
    int GridY(float y) const;
    void Relax(Ledge& l);
    void Spawn(float dt, const Settings& s);
    void Sweep(float dt, POINT cursor);
    Flake MakeClump(float x, float y, float vol, float vx, float vy);
    float Rand();
    float Rand(float a, float b) { return a + (b - a) * Rand(); }

    float scale_ = 1.f;
    int width_ = 0, height_ = 0;
    int cellW_ = 2;
    float time_ = 0.f;
    float spawnAcc_ = 0.f;
    float rainAcc_ = 0.f;
    float sideAcc_ = 0.f;  // flakes blown in through the upwind edge of the screen
    uint32_t rng_ = 0x9E3779B9u;
    std::vector<Flake> flakes_;
    std::vector<Flake> pending_;  // particles spawned while iterating (splashes)
    std::vector<Ledge> ledges_;
    std::vector<Ball> balls_;
    uint32_t nextBallId_ = 1;
    uint32_t heldId_ = 0;
    float heldVx_ = 0.f, heldVy_ = 0.f;  // smoothed velocity of the held ball
    bool cursorInit_ = false;
    POINT prevCursor_{};
    float cursorVx_ = 0.f, cursorVy_ = 0.f;

    // Snapshot of the airborne lumps (one entry per lump, in flakes_ order) in a spatial hash, rebuilt every frame.
    std::vector<float> clX_, clY_, clVx_, clVy_, clM_, clR_;
    std::vector<float> dvx_, dvy_;  // velocity change of each lump from its neighbours this frame
    std::vector<int> gridHead_, gridNext_;
    int gridW_ = 0, gridH_ = 0;
    float gridCell_ = 32.f;
};
