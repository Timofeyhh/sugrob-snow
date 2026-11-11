# Sugrob — snow on your Windows desktop

**English** · [Русский](README.ru.md)

A transparent overlay on the primary monitor, on top of every window. Snowflakes fall and pile up on the top
edges of windows, on the taskbar and along the bottom of the screen. The cursor sweeps the snow away, and mouse
clicks pass straight through it. When a game or a video is running fullscreen, the snow hides by itself.

The build produces a single file, `build\Sugrob.exe` (about 370 KB). Nothing to install.
Windows 10 / 11, primary monitor.

## Usage

- **Start:** double-click `Sugrob.exe`. A snowflake icon appears in the notification area next to the clock.
- **Weather panel** (click the snowflake): light snow, snow, heavy snow, blizzard, snowstorm, sleet.
  The wind is set separately: strength (none / light / strong / storm) and direction (left / right), so heavy snow,
  for example, can fall straight down or slanted. A blizzard and a snowstorm switch on their wind themselves; you can change it
  afterwards. The panel also has pause and "clear all snow". It closes when you click elsewhere or press Esc.
- **Menu** (right-click the snowflake): the same as a list, plus exit.

## Physics

- Depth: far flakes are smaller, slower and fainter and fly past; only near flakes settle.
- Flakes follow the air: drag, smooth gusts, turbulence, swaying as they fall.
- Windows occlude each other: a pile on a lower window barely grows under a window above it.
- A pile slumps when its slope gets too steep and slowly settles. Ends are rounded, extra snow slides off corners.
- Snowfall piles up to 24 px on a window (48 px along the bottom of the screen). Snow that is shaken off, swept or
  smashed may heap up 2.5 times higher. Snow that has no room where it lands slides along the edge to where there is room.
- Heavy snow falls as a thick wall (about eight times denser than ordinary snow) and quickly buries the window edges.
  In a strong wind snow also blows in from the side of the screen, so a storm fills the whole screen.
- Drag a window slowly and the snow rides along. Yank it and the snow flies off with the window's inertia.
  Close or minimize a window and its snow crumbles and falls down.
- Make a window narrower and only the snow beyond its new edge falls; the rest stays where it is.
  The pile is rounded off at the new end and the excess slides down.
- The cursor works like a mitten: sweep it over a pile and the snow flies up and lands nearby.
  A sideways swipe also pushes part of the snow ahead of the cursor as a heap, like a plough.
  Flying flakes are pulled along in the cursor's wake.
- Snow passes its momentum on: lumps in the air that are close to each other exchange velocity and move as one body,
  a fast lump drags the flakes it passes, and a lump landing with sideways speed shoves the snow ahead of it and kicks up powder.
- Heavy clumps burst into spray when they hit hard.
- Snow is drawn as one mass, not as separate particles: a pile and the lumps flying close together share one outline,
  and a big lump is irregular and tumbles as it flies.
- Sleet: raindrops fall fast and slant with the wind, splash, and slowly melt the piles; wet snow falls faster and hardly flutters.

## Snowballs and snowmen

- Press on a pile on a window edge (slightly above the snow works too) and drag along it: a snowball rolls and grows.
  There is no size limit: it grows as long as there is snow to roll it through (it only has to fit on the screen).
- A snowball looks hand-packed: a lumpy outline, light and shadow, little lumps of snow stuck to it, all turning as it rolls.
- Snow is sticky: a ball you are not holding never rolls on its own. A released ball stays where you left it,
  and a thrown ball lands and stays put.
- The cursor cannot pass through a snowman and never digs out the snow it stands on, so hovering over it does not make it sink.
- A fall from roughly 650 px or a hard throw smashes the ball. Its snow is not lost: smashed onto an edge, it leaves
  a heap of snow there, and the rest flies apart in big chunks that land again.
- Balls and snowmen never disappear off the screen: they bounce off the screen edges, and a ball let go below every
  edge (for example behind the taskbar) is put on the lowest visible edge.
- A ball placed on top of another ball sticks. Two or three balls make a snowman that looks at the cursor.
- You can move or resize a window with balls on it: they ride along and only fly off if the window is yanked violently.
  If the window is closed or minimized the balls drop to the nearest edge below, and its snow crumbles away gradually.
- Clicks that miss the snow and the balls go to the windows underneath as usual.

## Command-line flags (for testing)

| Flag | What it does |
|---|---|
| `--fast` | piles grow 15 times faster (for demos) |
| `--heavy` / `--blizzard` / `--buran` / `--sleet` | start with heavy snow / a blizzard / a snowstorm / sleet |
| `--stats` | once a second writes fps and frame time to `%TEMP%\sugrob_stats.txt` |
| `--bench` | 8 seconds of invisible running to measure the load, then exits |
| `--snapshot` | a 90 s simulation on fake windows in memory, pictures saved to `%TEMP%\sugrob_snapshot*.bmp`, plus self-tests for snowballs, window resizing and sleet (`sugrob_grab.txt`, `sugrob_balls.txt`) |

## Building

You need MinGW-w64 g++ (for example the portable [WinLibs](https://winlibs.com/) archive). Visual Studio is not required.

```
set MINGW_BIN=C:\path\to\mingw64\bin
build.bat
```

The result is `build\Sugrob.exe`. It uses Direct3D 11, Direct2D, DirectWrite and DirectComposition, which ship with Windows.

## Code layout

- `src/main.cpp`: windows, tray icon and menu, window tracking (WinEvent hooks), the mouse hook for snowballs, the 60 fps loop.
- `src/panel.cpp`: the weather panel (Direct2D + DirectWrite).
- `src/scene.cpp`: visible windows in z-order, primary monitor, auto-pause for fullscreen apps.
- `src/sim.cpp`: physics of flakes, clumps, piles and snowballs.
- `src/render.cpp`: drawing with Direct2D + DirectComposition (a transparent alpha-blended layer, snow drawn in sprite batches).

## Not done yet

Autostart with Windows, saving settings between runs, an icon for the exe, an installer, code signing.
While the right-click menu is open the snow is frozen (with the weather panel open it keeps falling).

## Antivirus and SmartScreen

The program draws a transparent window over all other windows and installs a global mouse hook (so snowballs can be
picked up). The exe is not code-signed, so Windows SmartScreen may warn when it is downloaded, and some antivirus
products may treat this behaviour with suspicion. The source is open and you can build it yourself.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE).
