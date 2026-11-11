#pragma once
#include "common.h"

// The main (primary) monitor in screen coordinates — the overlay covers only this screen.
RECT PrimaryMonitorRect();

// Collects visible top-level windows (in z-order) and the primary monitor.
// origin is the top-left corner of the virtual screen; results are relative to it.
void ScanScene(HWND overlay, HWND trayWnd, POINT origin, Scene& out);

// True when the foreground app is fullscreen (game, video, presentation):
// snow is hidden then so it never gets in the way.
bool IsFullscreenAppActive(HWND overlay);
