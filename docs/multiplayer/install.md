# Installing reVC multiplayer

You need your own copy of **GTA Vice City** (PC, e.g. Steam or retail). This
package contains only the reVC executable, reVC's extra game files and the
multiplayer tools; no Rockstar assets.

## Install

1. Make a copy of your GTA Vice City folder (keep the original untouched), e.g.
   `GTAVC-reVC`.
2. Extract this package **over** that folder and say yes to replacing files.
   That adds the reVC executable plus reVC's own `data`, `TEXT`, `models` and
   `neo` additions (including `data/freeroam_miami.scm` for multiplayer and
   `data/mp/races/` and `data/mp/arenas/`).
3. Start the reVC executable from that folder:
   - **Windows:** `reVC.exe`. Both builds are included; `d3d9` is usually the
     better choice on Windows, `gl3` uses OpenGL.
   - **Linux:** `./reVC`. Needs the system libraries:
     `sudo apt install libglfw3 libopenal1 libmpg123-0` (Debian/Ubuntu names).
   - **macOS:** `reVC.app`. On first launch it asks you to choose your GTA Vice
     City folder (it remembers it). The build isn't signed, so the first time
     right-click → Open, or run `xattr -dr com.apple.quarantine reVC.app`. If it
     complains about missing libraries: `brew install glfw openal-soft mpg123`.
     Still extract the rest of the package over the game folder so
     `data/mp/races/` and `data/mp/arenas/` are there for the route editor.

Single player works as before: New Game from the main menu.

## Play multiplayer

1. Someone runs the relay: `revc-relay --port 7777` (see `relay-setup.md` in
   the source repo for a VPS or home PC). For a test on one PC, run it locally.
2. In the game: **Multiplayer** on the main or pause menu.
   - *Name*: your name. *Relay server*: `host:port` of the relay
     (`127.0.0.1:7777` for a local test).
   - *Host Session* gives you a `VICE-XXXX` code. Friends type it under *Code*
     and pick *Join with Code*.
3. In a session: `T` chat, `F2` free-roam menu (cars, weapons, health,
   armour, start a race), `F3` join a race invite, hold `H` to respawn your race
   car.

No game handy for a second player? `revc-fakeclient --relay 127.0.0.1 --join
VICE-XXXX --bot walk --at X,Y,Z` adds a bot (`/pos` in chat shows coordinates).

This is the first build that has met the real game: please go through
`docs/multiplayer/testing.md` and report what breaks.
