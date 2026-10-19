# Click Buffer (Geode mod for Geometry Dash 2.2081)

An input buffer. Click up to **N ticks early** and the mod fires the click on the first tick it would actually work.

## Settings (Geode menu > Mods > Click Buffer > gear icon)

| Setting | What it does |
|---|---|
| Enabled | Master switch |
| Buffer Ticks (0-30, default 5) | How early a click may be. 1 tick = ~4.17 ms at 240 TPS (5 = ~21 ms, 30 = ~125 ms) |
| Buffer Ground Jumps | Cube / ball / robot / spider clicks made just before landing |
| Buffer Orbs / Rings | Clicks made just before touching an orb or ring |
| Only In Practice / Test Mode | Keeps the buffer out of normal runs |
| Debug Log | Writes buffer events to the Geode log (handy for checking the window) |

## Build option A: GitHub Actions (no local setup)

1. Create a new GitHub repository and upload everything in this folder (keep the `.github` folder).
2. Open the **Actions** tab, run **Build Click Buffer** (it also runs on every push).
3. When it finishes, download the **Build Output** artifact. Inside is `local.click_buffer.geode`.
4. Put that file in `<your Geometry Dash folder>\geode\mods\` and restart the game.

## Build option B: locally on Windows

1. Install Visual Studio 2022 (or Build Tools) with the C++ desktop workload, plus CMake.
2. Install the Geode CLI, then run `geode sdk install` and `geode sdk install-binaries`.
3. In this folder run `geode build`. The `.geode` file appears in `build\`.

## How it works

- It hooks `GJBaseGameLayer::handleButton` to see each real press and decide if it was "used" (you were on the ground, touching an unused orb, or in a ship/UFO/wave/swing).
- If it was not used, the click is remembered for N ticks. `GJBaseGameLayer::processCommands` runs once per physics tick, before the player moves, and fires the click as soon as you are on the ground or touching an unused orb.
- If you were still holding when it fires, the mod releases and re-presses so the game sees a fresh click. If you had already let go, it releases again after the same hold time you used.
- Pending clicks are dropped on death, level reset, quitting, or if you enter a ship/UFO/wave/swing.

## Limits

- Only the jump button is buffered (not platformer left/right).
- Ticks are physics ticks, so a TPS bypass changes how many milliseconds they are.
- Dual mode: on normal levels, one click drives both icons, so the buffer fires when either ground-type icon can use it.
- This is an input advantage. Use *Only In Practice / Test Mode* if you want to keep it out of normal runs.
