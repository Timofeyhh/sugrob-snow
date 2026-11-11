#pragma once
#include "common.h"

// A small "weather" panel that opens above the tray icon (left click on the snowflake):
// light snow / snow / heavy snow / blizzard / storm / sleet, the wind, pause and "clear all snow".
enum class PanelAction { Changed, TogglePause, Clear };

namespace Panel {
bool Init(HINSTANCE inst, Settings* settings, void (*onAction)(PanelAction));
void Toggle(const RECT& anchor);  // anchor: the tray icon (screen coordinates)
void Hide();
bool Visible();
bool Contains(POINT screen);
HWND Window();
void Refresh();  // repaint after the settings were changed elsewhere (tray menu)
void Shutdown();
}  // namespace Panel
