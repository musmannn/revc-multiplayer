# Multiplayer: project context for contributors and agents

Read this first if you're working on reVC multiplayer. It's the single place
for where things stand, what's been decided, and where the relevant code is.
Keep it up to date as work lands.

## Documents

| Doc | What it is |
|---|---|
| [Design spec](../superpowers/specs/2026-10-31-multiplayer-design.md) | What we're building and why: goals, non-goals, anti-goals, constraints. The source of truth for scope. |
| [Plan 1: network foundation](../superpowers/plans/2026-10-31-mp-1-network-foundation.md) | Protocol, invite codes, relay server, fake client, CI. Needs no game files, so a cloud session can do all of it. |
| [Plan 2: free roam](../superpowers/plans/2026-10-31-mp-2-free-roam.md) | Game integration: menu, remote players, vehicles, combat, chat, free-roam menu. Compiles in CI; in-game checks need the game files. |
| [Plan 3: races](../superpowers/plans/2026-10-31-mp-3-races.md) | Race rules (unit tested), race protocol, in-game race flow, route recorder, 4 routes. |
| [Plan 4: deathmatch](../superpowers/plans/2026-10-31-mp-4-deathmatch.md) | Milestone 2: deathmatch, team deathmatch and scoreboards. |
| [Plan 5: milestone 3](../superpowers/plans/2026-11-01-mp-5-milestone-3.md) | Dedicated server (`revc-server`), route/arena editor, Hold the Hill, Team Hold the Hill, Stunt Challenge, protocol v2. |
| [Installation guide](../../INSTALLATION.md) | Player-facing: from a fresh machine to playing on Windows, Linux and macOS, with the downloadable installers from GitHub releases or the cross-platform installer `installer/revc_installer.py` (window or `--cli`), installing by hand, hosting a relay locally or on a VPS, hosting and joining. The older per-OS installers `install-macos.sh` and `install-windows.bat` still work. |
| [Relay setup](relay-setup.md) | Running `revc-relay` on a cloud VPS or a home PC. |
| [Testing checklist](testing.md) | In-game manual test checklist, to run once the game is installed. |

## Status

| Milestone | Scope | State |
|---|---|---|
| 1 | Invite-code sessions, free roam, races | **Code complete, in-game unverified.** Plan 1 is complete (network layer, relay, fake client). Plans 2 and 3 are written and build with `MULTIPLAYER` on Linux, macOS and Windows in CI. The engine-free race rules, route parser and race protocol are unit tested, and a whole race runs with fake clients. Still waiting on the game files: every check in [testing.md](testing.md), plus recording the 4 built-in race routes. |
| 2 | Deathmatch, team deathmatch, scoreboards | **Code complete, in-game unverified** (Plan 4). Rules and the match host are unit tested; whole matches run with fake clients. The rules are builder calls waiting for the owner's review. |
| 3 | More modes, race editor, dedicated server | **Code complete, in-game unverified** (Plan 5). Protocol v2 (dedicated-session flag; routes and arenas travel inside requests and invites). The route editor, hill and stunt scoring, seat table and dedicated host are engine-free and unit tested; `revc-server` runs whole races and matches with fake clients over loopback. Waiting on the game: the testing.md Plan 5 checks and the placeholder Washington Beach arena. |

## Decisions log

Owner decisions are final unless the owner changes them. Builder calls can be
revisited with a reason.

| Date | Decision | By |
|---|---|---|
| 2026-10-31 | Friends join with an **invite code** through a small lobby/relay server (no port forwarding). | Owner |
| 2026-10-31 | **A player hosts**; the host is authoritative. No dedicated server in milestone 1. | Owner |
| 2026-10-31 | Milestone 1 = **free roam + races**. | Owner |
| 2026-10-31 | **Ambient traffic/peds stay local** per player; only players, their vehicles and mode objects sync. | Owner |
| 2026-10-31 | Relay runs on **both** a cheap/free cloud VPS (default address built in) and the owner's own PC (switch via a *Relay server* menu setting). | Owner |
| 2026-10-31 | **macOS, Linux and Windows** are all first-class with cross-play; CI builds and tests on all three. | Owner |
| 2026-10-31 | Free roam gives a **spawn loadout + weapon/health/armour menu**; no world pickups or Ammu-Nation in milestone 1. | Owner |
| 2026-10-31 | Race results ordered by racers' own crossing timestamps on a shared session clock, not by packet arrival. | Builder call |
| 2026-10-31 | Up to 8 players per session. Story script does not run in multiplayer. | Builder call |
| 2026-10-31 | Remote players are extra ped entities fed by the network. The local player stays the only `CPlayerPed`. | Builder call |
| 2026-10-31 | Remote players are `CCivilianPed` puppets with `CharCreatedBy = UNK_CHAR`, so the engine skips their AI and physics and they are never culled. | Builder call |
| 2026-10-31 | The multiplayer world starts from upstream `gamefiles/data/freeroam_miami.scm` instead of `main.scm`. Any pickups it places are local-only, like ambient traffic. | Builder call |
| 2026-10-31 | Shooter-side hit detection, victim-side damage. Remote players' shots are visual only on your screen. | Builder call |
| 2026-10-31 | Remote players' enter/exit-vehicle is shown as a warp in milestone 1; the local player keeps the full animation. | Builder call |
| 2026-10-31 | Engine-free code lives in `mp/` (static lib `mpcommon`, relay, tools, tests) so it builds and tests without game files. | Builder call |
| 2026-10-31 | A failed *create* (wrong version, bad name, relay full) is answered with `MSG_JOIN_RESULT` and the matching `eJoinResult`, so the client reports it through `OnJoinFailed`. The plan didn't define a create-failure message. | Builder call |
| 2026-10-31 | The relay checks the protocol version before parsing the rest of create/join, so a future layout still gets a clean `JOIN_VERSION_MISMATCH`. | Builder call |
| 2026-10-31 | Anti-spoofing: the relay never forwards relay-control types (below `MSG_CHAT`), and clients only accept them from `MP_PLAYER_RELAY`. `SendGame` only sends types ≥ `MSG_GAME_FIRST`, up to `MP_MAX_PACKET − 2` bytes (the envelope takes 2). | Builder call |
| 2026-10-31 | `MSG_PLAYER_LEFT.reason` is `eLeaveReason` (`LEAVE_QUIT` / `LEAVE_LOST`). Clients send ENet disconnect data `MP_DISCONNECT_QUIT` when leaving on purpose. | Builder call |
| 2026-10-31 | Duplicate names are compared case-insensitively. The base name is shortened so `name (n)` fits in 23 chars. | Builder call |
| 2026-10-31 | Ping/pong are unreliable. `CSessionClock` ignores samples with a negative RTT or one above 10 s, and `RttMs()` is the latest sample's RTT. | Builder call |
| 2026-10-31 | The relay drops connections that don't send create/join within 10 s. The ENet peer timeout was 5–15 s (superseded below). | Builder call |
| 2026-10-31 | `MULTIPLAYER` also needs `CUSTOM_FRONTEND_OPTIONS` (the menu page is a CFO page), and the build defines `NO_MULTIPLAYER` when `mp/` isn't part of it, so `REVC_MULTIPLAYER=OFF` builds plain single player. | Builder call |
| 2026-10-31 | Multiplayer menu text keys are at most 7 characters (`CMenuEntry::m_EntryName` is `char[8]`): `MP_RLAY` and `MP_LEAV` instead of the plan's `MP_RELAY`/`MP_LEAVE`, plus `MP_STAT` for the session row. | Builder call |
| 2026-10-31 | A session becoming ready starts the world through `DoSettingsBeforeStartingAGame` (the New Game path). When it ends, the frontend opens; the offline world stays loaded instead of a forced return to the start menu (VC has no in-game "quit to main menu"). | Builder call |
| 2026-10-31 | The network keeps servicing from `LoadingScreen` too, so long loads don't time the connection out; UI reactions wait for the next frame. | Builder call |
| 2026-10-31 | Remote players' seats follow their `PLAYER_STATE` (vehicle id + seat), so seating heals after lost messages and works for late joiners; `VEHICLE_SEAT` only carries the host's accept/reject and ownership changes. | Builder call |
| 2026-10-31 | The VC player can't normally get in as a passenger. Pressing enter at a car a friend is in warps you into a free passenger seat (`CPed::SetCarJack` hook), never dragging them out. | Builder call |
| 2026-10-31 | When a vehicle owner leaves, ownership only changes if the leaver owned it (`MpNextVehicleOwner`); a passenger leaving never takes the car away from its owner. | Builder call |
| 2026-10-31 | Remote shots draw a tracer along the shooter's camera direction (`CNetCombat::OnLocalShot`), for instant-hit weapons only; projectiles show through their anims. | Builder call |
| 2026-10-31 | The multiplayer respawn is its own branch in `CGameLogic::Update` (no money/weapon loss, no `PassTime`, no taxi) instead of `CRestart::OverrideNextRestart`; `CNetCombat::SetRespawnOverride` lets races respawn at the last checkpoint. | Builder call |
| 2026-10-31 | Chat and free-roam menu disable player controls with new bits `0x200` and `0x400` (the engine uses up to `0x100`). Cheats are ignored while typing in chat or a menu field (`CPad::AddToPCCheatString`). | Builder call |
| 2026-10-31 | Saving is blocked in a session (the save point shows "Saving is disabled in multiplayer"); loading a save leaves the session. | Builder call |
| 2026-10-31 | `'~'` is dropped from names and chat: the game's font treats it as the start of a colour token. | Builder call |
| 2026-10-31 | Racers advance to the next checkpoint locally the moment they cross it; the host's standings are the truth for positions and results. | Builder call |
| 2026-10-31 | The host computes each racer's distance to their next checkpoint from the `PLAYER_STATE` positions it already gets, so racers don't send it. | Builder call |
| 2026-10-31 | At race start every racer, passengers included, is warped out of their car into a fresh race car on the grid. | Builder call |
| 2026-10-31 | Race routes are text `.route` files in `data/mp/races/`, listed in `data/mp/races/index.txt`: `CFileMgr` can't list a directory, so the index stands in for the plan's `*.route` glob. `/rec save` adds new routes to it. | Builder call |
| 2026-10-31 | The host side of a race (invite, accepts, start, standings, end) is the engine-free `CRaceHost` in `mp/common`, shared by the game host and the fake client's `--start-race`, and unit tested. Late joiners get the open invite. | Builder call |
| 2026-10-31 | Race checkpoint cylinders are drawn at the trigger radius (`C3dMarkers` size = radius, like the script's spheres), not twice it, so what you see is what counts. | Builder call |
| 2026-10-31 | The race freeze uses its own player-control bit `0x800` instead of `PLAYERCONTROL_CUTSCENE`, so it can't fight cutscenes. | Builder call |
| 2026-10-31 | Catch-up for late joiners (our vehicles, time/weather, an open race invite) goes out on their first `PLAYER_STATE`, not on the join notice: game messages sent while they're still loading the world are dropped. | Builder call |
| 2026-10-31 | Milestone 2 rules (all builder calls, see Plan 4): kill +1, suicide/no killer −1, team kill −1 for the killer; deathmatch to 20, team deathmatch to 30, 10-minute limit; teams alternate by accept order; no friendly fire; match loadout pistol + Uzi + shotgun + full armour; leavers stay on the board; one race or match at a time. | Builder call |
| 2026-10-31 | Kills are scored by the host from the `GMSG_DIED` reports players already send (shooter-side attribution), so matches add no kill message. Matches use the free-roam spawn points for now; dedicated arenas can come later. | Builder call |
| 2026-10-31 | `Tab` shows a scoreboard in any session: the match scores during a match, otherwise who's in the session. | Builder call |
| 2026-10-31 | Added `mp/common/NetInit.*` (ref-counted `enet_initialize`), which the relay and `CNetClient` share. This file isn't in the plan's file list. | Builder call |
| 2026-10-31 | On `Stop()` the relay drains for 250 ms and then destroys the host without forcing disconnects. ENet discards a peer's undelivered packets when a disconnect arrives, which would swallow `SESSION_ENDED`. | Builder call |
| 2026-10-31 | Received chat text and names are filtered to printable ASCII in `CNetClient` before they reach the game. | Builder call |
| 2026-10-31 | `REVC_MULTIPLAYER` defaults to OFF on Switch, PS2 and Android (they're out of scope, and the Switch CI builds the top-level CMake). It defaults to ON elsewhere, as the plan says. | Builder call |
| 2026-10-31 | `vendor/enet` also keeps ENet's `README` and `ChangeLog`, and `vendor/doctest` keeps `LICENSE.txt`, next to the plan's files. All are unmodified copies of the tags. | Builder call |
| 2026-10-31 | `CNetClient` detects a re-entrant `Host()`/`Join()` from inside a callback with a generation counter. Comparing `ENetHost` pointers failed on macOS, where malloc reuses the address. | Builder call |
| 2026-10-31 | Relay hardening beyond the plan: ENet `maximumPacketSize` 2 KB and `maximumWaitingData` 64 KB (the defaults allow 32 MB per peer), at most 16 connections per IP (`SRelayConfig::maxPeersPerAddress`, an added field), and at most 4096 events per `Service()`. A member that stops acknowledging skips snapshots once half of `maxQueuedPerPeer` (2048) reliable commands are queued for it, and is dropped past the limit. Each IP gets 10 joins with an unknown code per minute (`maxFailedJoinsPerMinute`), so invite codes can't be guessed. | Builder call |
| 2026-11-01 | ENet peer timeouts are 15–30 s on both the relay and the client (`MP_PEER_TIMEOUT_MIN_MS`/`MAX_MS` in `Protocol.h`), up from 5–15 s: an in-game test lost the host 6 s into its first world load. A crashed host now ends its session within 30 s. The relay's "stops acknowledging" drop (2048 queued commands) is unaffected: a lone host loading the world has almost nothing queued. | Builder call |
| 2026-11-01 | While a multiplayer world is loading (`!sInGame`), `CStreaming::LoadAllRequestedModels` services the network every 100 ms (`CNetGame::KeepAliveWhileLoading`), on top of the loading-screen servicing. It never runs once the world is up, so game messages are never dispatched mid-frame. | Builder call |
| 2026-11-01 | Spawn points are a 3 m ring around the start `freeroam_miami.scm` itself uses (79.69, −867.25, ground ≈ 10.5, Washington Beach). Each spot is checked with `FindGroundZFor3DCoord` and `TestSphereAgainstWorld`, and a blocked one falls through to the next, instead of hand-picked coordinates (the old ones put the host in a bus shelter). | Builder call |
| 2026-11-01 | The Multiplayer page's read-only *Session* row moved to the bottom, so the page opens with *Name* selected. | Builder call |
| 2026-11-01 | Milestone 3 scope (all builder calls, see Plan 5 and the spec's milestone 3 addition): a dedicated server, a route/arena editor on top of `/rec`, and three modes on the match framework: Hold the Hill, Team Hold the Hill and Stunt Challenge. | Builder call |
| 2026-11-01 | Protocol version 2. `MSG_CREATE_SESSION` carries session flags and `MSG_JOIN_RESULT` hands them to joiners (`MP_SESSION_DEDICATED`); the relay drops unknown bits. | Builder call |
| 2026-11-01 | Races and arenas travel inside `RACE_REQUEST`/`RACE_INVITE` and `MATCH_REQUEST`/`MATCH_INVITE` (positions in 1/8 m, radii in 1/2 m; a 64-checkpoint route is about 550 bytes). Only the requester needs the file; everyone races the host's copy of what was sent. `MpValidateRoute` checks everything read from the wire. Each game still checks the vehicle is a land vehicle it knows before spawning it. | Builder call |
| 2026-11-01 | Arenas are `.route` files with `type arena` in `data/mp/arenas/`: grid lines are spawns (1–8), cp lines are hills (1–16), vehicle is optional (stunt default PCJ-600, 191). One placeholder arena, Washington Beach, around the free-roam start. | Builder call |
| 2026-11-01 | The `/rec` editor's logic is engine-free (`CRouteEditor`); commands are numbered from 1, undo keeps 16 steps, `/rec test` sends the unsaved route as a race request, and the 10 nearest checkpoints plus the grid show as markers while editing. | Builder call |
| 2026-11-01 | Hold the Hill: the hill moves every 60 s through the arena's hills in order; on it = within the radius horizontally and 4 m vertically, alive. Free for all: a lone player scores 1 point per second; team: every player on it scores if they're all on one team. Limits 60 / 150 points, 8 minutes. Kills are counted but don't score (a team kill isn't a kill). The host scores it from the `PLAYER_STATE` positions it already gets; clients work out the active hill themselves. | Builder call |
| 2026-11-01 | Stunt Challenge: 4 minutes, no score limit. A stunt is time with no wheel on the ground; it counts if at least 0.7 s and it lands with up.z ≥ 0.5. Points = (airMs/50 + metres + 2 × height) × (1 + flips + spins/2), capped at 2000; a flip or spin is 360° (with 20° of slack) of up-axis or in-air heading turn. Measured by the stunter's own game and reported with `GMSG_STUNT` (96), trusted like race crossings. | Builder call |
| 2026-11-01 | Dedicated server: `revc-server` is player 0 and never a participant, so a dedicated session holds 7 players. It answers everything a host's game does except its own ped: clock, time/weather (one game minute per real second), seats (first come, first served; unknown vehicles taken on trust), races, matches, catch-up, a chat welcome and a playlist (next event 60 s after the last, with at least 2 players). It can run the relay in-process and makes a new session (new code) if the relay drops it. | Builder call |
| 2026-11-01 | Games skip the ped, blip and name tag for id 0 in a dedicated session (`CRemotePlayer::Add`), and show `n/7`. Invites from the server read "Next up: …". | Builder call |
| 2026-11-01 | Installers (`installer/`) build from source on the player's machine with the same commands as CI (CMake on macOS, premake + MSBuild on Windows, d3d9 by default), copy the game from a folder found in `~/Downloads` (any folder with `data/gta_vc.dat` and `models/gta3.img`) into `~/Games/reVC-Multiplayer`, then lay `gamefiles/` and the binaries on top. The original game folder is never modified. | Builder call |
| 2026-11-02 | `installer/revc_installer.py` is the main installer on all three OSes: Python standard library only (Tkinter window, `--cli` without a display). It clones the repository if it isn't run from a checkout, installs missing tools (apt/dnf/pacman/zypper via `pkexec`/`sudo`, Homebrew, winget for VS Build Tools and Git), builds with the same commands as the per-OS scripts, and by default installs **into the chosen game folder** after backing up the files `gamefiles/` replaces to `reVC-backup/`; copying to a new folder is an option. `gamefiles/` paths are matched case-insensitively to the game's folders. `--relay-only` builds just `mp/` for VPS hosting. `INSTALLATION.md` replaces `MULTIPLAYER.md`. Windows and macOS paths are ported from the PowerShell/shell installers but untested on those systems. | Builder call |
| 2026-11-02 | The project's repository is **github.com/musmannn/revc-multiplayer**, branch `main`. The installer clones it, and the docs link to it; the README and docs no longer link to earlier forks (only the `vendor/librw` submodule URL and the license's Proof-of-Usage work URL still name them). Android CI is dropped, and the workflows are renamed `Build: …`, `Multiplayer: …` and `Release: …`. | Owner |
| 2026-11-02 | Downloadable installers: `.github/workflows/release.yml` builds reVC on each OS and packs it as a `payload` with `revc_installer.py` (PyInstaller one-file exe on Windows, PyInstaller app on macOS arm64, tar.gz using the system `python3` on Linux, built on Ubuntu 22.04). A `v*` tag publishes them as a GitHub release; pushes to `main` touching the installer build them as artifacts only. With a payload the installer copies the included build and only checks run-time libraries (`ldd` on Linux, Homebrew formulas on macOS); `--from-source` still builds. Windows tools use the static C runtime (CMP0091); the gl3 exe ships the VC++ runtime DLLs app-local. The installers also install `revc-server`. | Builder call |

## Race routes and arenas

`.route` files live in `gamefiles/data/mp/races/` (copied into the game's
`data/mp/races/`) and are listed in `index.txt` there. Arenas use the same
format with `type arena` and live in `data/mp/arenas/`. They're plain text
with `#` comments and one directive per line:

```
name Ocean Drive Sprint
type sprint            # or circuit, or arena
laps 1                 # circuits: 1..9; sprints are always 1
vehicle 141            # model id (141 = Infernus); optional for arenas
grid  X Y Z HEADING    # 1..8 slots, in grid order; heading in degrees (arena: spawns)
cp    X Y Z RADIUS     # 2..64 checkpoints in driving order; the last one is the finish
                       # (arena: 1..16 hills)
```

A bad file is skipped with a reason in the debug log, and the rest still load.
Routes travel with race requests and invites, so only the player who starts a
race needs the file.

Make and edit routes in a session with `/rec` (`/rec help` lists it all):
`/rec new NAME` or `/rec arena NAME` or `/rec load NAME`; `/rec grid` (`spawn`),
`/rec cp [R]` (`hill`), `/rec ins N [R]`, `/rec move N`, `/rec del N`,
`/rec radius N R`, `/rec movegrid N`, `/rec delgrid N`; `/rec type`,
`/rec laps N`, `/rec vehicle [MODEL]`, `/rec name NAME`; `/rec list`,
`/rec undo`, `/rec save`, `/rec test` (race the unsaved route now) and
`/rec close`. Points are numbered from 1. `/pos` prints your coordinates.

To try a race without the game: start a relay, then
`revc-fakeclient --relay 127.0.0.1 --host --start-race "Test"` and two
`revc-fakeclient --relay 127.0.0.1 --join VICE-XXXX --bot race`.

## Ground rules

- **Single player must be untouched.** All multiplayer code goes behind
  `#ifdef MULTIPLAYER` (defined in `src/core/config.h`). This is also the
  upstream rule: the README says custom code must be wrapped in preprocessor
  conditions. With no session active, every multiplayer hook is a no-op.
- Follow `CODING_STYLE.md` and the surrounding code: C-style C++, `CName`
  classes, static manager classes with `Init/Update/Shutdown`, tabs.
- Third-party libraries are vendored under `vendor/` and wired into both
  CMake (`CMakeLists.txt`, `src/CMakeLists.txt`) and premake (`premake5.lua`).
- Testing in the real game needs original GTA Vice City files. They are
  **not** in the repo. Ask the owner for the install path if you need it.

## Code map

Where the engine touches what multiplayer needs.

| Concern | Where |
|---|---|
| Main per-frame loop (net send/receive hook point) | `CGame::Process()` in `src/core/Game.cpp` |
| Game init / start / shutdown | `CGame::Initialise`, `CGame::ReInitGameObjectVariables`, `CGame::ShutDown` in `src/core/Game.cpp` |
| "The player" lookups (~1,000 call sites, 67 files) | `FindPlayerPed`, `FindPlayerVehicle`, `FindPlayerCoors` in `src/core/PlayerInfo.cpp` |
| Player slots | `CWorld::Players[NUMPLAYERS]`, `CWorld::PlayerInFocus` in `src/core/World.h`; `NUMPLAYERS = 1` in `src/core/config.h` |
| Player state (money, wasted/busted) | `CPlayerInfo` in `src/core/PlayerInfo.h` |
| Ped classes | `CPed` (`src/peds/Ped.h`), `CPlayerPed` (`src/peds/PlayerPed.h`), `CCivilianPed` (`src/peds/CivilianPed.h`) |
| Ped spawning / cleanup (keep remote players safe from it) | `CPopulation` in `src/peds/Population.h` (`ManagePopulation`) |
| Vehicles | `src/vehicles/` (`CVehicle`, `CAutomobile`, `CBike`, `CBoat`) |
| Random traffic | `CCarCtrl::GenerateRandomCars` in `src/control/CarCtrl.cpp` |
| Input | `CPad` in `src/core/Pad.h` |
| Story script (must not run in MP) | `CTheScripts::Init/Process` in `src/control/Script.cpp` |
| Frontend menus (add Multiplayer page) | `src/core/MenuScreensCustom.cpp`, `src/core/Frontend.cpp` (`CUSTOM_FRONTEND_OPTIONS`) |
| HUD text, timers, messages | `CHud` (`src/renderer/Hud.h`), `CMessages` (`src/text/Messages.h`), `COnscreenTimer` (`src/control/OnscreenTimer.h`) |
| Radar blips | `CRadar` (`src/core/Radar.h`) |
| Checkpoint markers | `C3dMarkers` (`src/renderer/SpecialFX.h`), `CCoronas` (`src/renderer/Coronas.h`) |
| Time & weather | `CClock` (`src/core/Clock.h`), `CWeather` (`src/renderer/Weather.h`) |
| Feature toggles | `src/core/config.h` |
| **Multiplayer game code** | `src/multiplayer/`: `CNetGame` (session, dispatch), `CRemotePlayer`, `CNetVehicles`, `CNetCombat`, `CNetChat`, `CFreeroamMenu`, `MpMenu` (frontend page), `MpText`, `CSpawnPoints`, `CNetRace`, `CNetMatch` (all match modes, arenas), `CRouteRecorder` (`/rec` front end + markers) |
| **Engine-free rules** | `mp/common/`: `RaceLogic`/`RaceHost`, `MatchLogic`/`MatchHost`, `HillLogic`, `StuntLogic`, `RouteEditor`, `RaceRoute` (parse, validate, write), `SeatTable` |
| **Dedicated server** | `mp/server/`: `CDedicatedHost` (lib `servercore`) and `revc-server` (`main.cpp`) |
| Multiplayer engine hooks | `CGame::Process/Initialise/ReInitGameObjectVariables/ShutDown*` (`Game.cpp`), `FrontendIdle`/`LoadingScreen`/`Render2dStuff` (`main.cpp`), `CTheScripts::Init/OpenScript` (`Script.cpp`), `CText::Get`, `CMenuManager::Process` + save menu (`Frontend.cpp`), `CPed::InflictDamage`, `CPed::SetCarJack`, `CWeapon::Fire`, `CGameLogic::Update` (WASTED), `CPad::AddToPCCheatString`, INI load/save (`re3.cpp`). All inside `#ifdef MULTIPLAYER`. |

## Building

See the main [README](../../README.md) for the game. On Linux, CMake + conan
or premake (`premake5Linux`). Game-side multiplayer sources go under
`src/multiplayer/` (Plan 2).

The engine-free layer in `mp/` builds and tests on its own, with no game files:

```bash
cmake -S mp -B build-mp -G Ninja
cmake --build build-mp
ctest --test-dir build-mp --output-on-failure
```

It is also part of the top-level CMake build (option `REVC_MULTIPLAYER`, on by
default). The build produces:

| Target | What |
|---|---|
| `enet` | Vendored ENet 1.3.18 (`vendor/enet`, unmodified) |
| `mpcommon` | `mp/common`: protocol, byte streams, invite codes, session clock, pose buffer, `CNetClient` |
| `relaycore` | `CRelayServer` (`mp/relay/RelayServer.*`), shared by `revc-relay` and the tests |
| `revc-relay` | The invite-code relay server. See [relay-setup.md](relay-setup.md) |
| `servercore` | `CDedicatedHost` (`mp/server/DedicatedHost.*`), shared by `revc-server` and the tests |
| `revc-server` | Dedicated session host, optionally with the relay in-process. See [relay-setup.md](relay-setup.md) |
| `revc-fakeclient` | Headless client for testing (bots: walk, drive, race, dm, koth, stunt) |
| `mp_tests` | doctest suites (`mp/tests`), including relay + client and dedicated-server integration over loopback |

Try a session locally, with each command in its own terminal:

```bash
./build-mp/revc-relay --port 7777
```

```bash
./build-mp/revc-fakeclient --relay 127.0.0.1 --host --name Tommy
```

```bash
./build-mp/revc-fakeclient --relay 127.0.0.1 --join VICE-XXXX --name Lance --say hello
```

Tommy prints `[joined] 1 Lance` and `[chat] 1: hello`. Pressing Ctrl+C on Tommy
makes Lance print `[ended] host left` and exit non-zero.

Or let a dedicated server host, with the relay in the same process:

```bash
./build-mp/revc-server --with-relay 7777 --code-file code.txt --playlist dm --playlist-gap 5
```

```bash
./build-mp/revc-fakeclient --relay 127.0.0.1 --join $(cat code.txt) --name Tommy --bot dm
./build-mp/revc-fakeclient --relay 127.0.0.1 --join $(cat code.txt) --name Lance --bot dm
```

Five seconds after both are in, the server opens a deathmatch by itself.

### Network layer at a glance

- Every player connects only to the relay (ENet, 2 channels: 0 reliable, 1
  snapshots). Each packet is a 2-byte envelope `{route, player}` followed by a
  message whose first byte is its type. Uplink `player` is the target (for
  `MP_ROUTE_PLAYER`). Downlink `player` is the sender, or `MP_PLAYER_RELAY`.
- Protocol version 2 (Plan 5): sessions carry flags (`MP_SESSION_DEDICATED`) from the creator to every joiner.
- Types below 64 belong to the layer (relay control, chat, ping/pong). Types
  from `MSG_GAME_FIRST` (64) up are game messages: `CNetClient::SendGame`
  sends them and `INetClientListener::OnGameMessage` receives them unparsed.
- Session time is the host's clock. Joiners ping the host every second and
  `CSessionClock` keeps the lowest-RTT offset of the last 16 samples.

## Open questions

Track anything waiting on the owner here, and remove it once answered.

- The default relay is `MP_DEFAULT_RELAY` = `127.0.0.1:7777` in `src/multiplayer/NetGame.h` until the cloud VPS exists. It's a one-line change.
- The 4 built-in race routes (Ocean Drive Sprint, Starfish Loop, Bridge Run, Little Havana Circuit) aren't recorded yet. Record them in-game with `/rec` and list them in `gamefiles/data/mp/races/index.txt`.
- The Washington Beach arena (`gamefiles/data/mp/arenas/washington_beach.route`) is a placeholder ring around the free-roam start; its spawns and hills need placing in-game.
- Milestone 3's rules (hill timing and limits, stunt formula, 7 players on a dedicated server) are builder calls waiting for the owner's review.
- Placeholders that need the game to check: the default remote-player skins (`sDefaultSkins` in `RemotePlayer.cpp`) and the fake client's `--bot drive` model (132).

- GTA Vice City isn't installed on the dev machine yet. In-game testing waits
  for it. Until then, verify with unit tests and the headless fake client.
- The cloud relay's actual address/hostname, once the owner sets up the VPS.
