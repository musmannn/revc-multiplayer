# Multiplayer Plan 2: Free Roam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Friends join a hosted session by invite code and play free roam together in Vice City: they see each other walking, driving, shooting and dying, share cars, chat, and share the same time and weather.

**Architecture:** A static manager `CNetGame` (in `src/multiplayer/`, all behind `#ifdef MULTIPLAYER`) owns a `CNetClient` from Plan 1 and hooks into `CGame::Process`, `Render2dStuff` and a few engine functions. The local player stays the only `CPlayerPed`. Each remote player is a `CCivilianPed` puppet with `CharCreatedBy = UNK_CHAR`, so the engine skips its AI and physics and we drive it from interpolated snapshots. The world starts from the upstream `freeroam_miami.scm`, not `main.scm`. Game messages are defined engine-free in `mp/common/GameProtocol` so they're unit tested and usable by the fake client.

**Tech Stack:** reVC (C++11), Plan 1's `mpcommon` + ENet, CMake (Linux/macOS), premake + MSBuild (Windows).

**Spec:** `docs/superpowers/specs/2026-10-31-multiplayer-design.md`. Context hub: `docs/multiplayer/README.md`. Depends on Plan 1 (`2026-10-31-mp-1-network-foundation.md`), which must be complete.

## Global Constraints

- **Single player is untouched.** Every engine edit is inside `#ifdef MULTIPLAYER`, and every runtime hook is guarded by `CNetGame::IsActive()`. With no session, behaviour is byte-for-byte the old code path.
- `MULTIPLAYER` is defined in `src/core/config.h` for desktop builds only (`#if !defined(ANDROID) && !defined(__SWITCH__)`).
- Up to **8 players**. The host is authoritative for time and weather. Each player is authoritative for their own ped and for vehicles they drive.
- **Traffic and peds are local per player.** Never sync ambient entities.
- **Snapshots at 20 Hz** (`MP_SNAPSHOT_HZ`), rendered **100 ms** behind (`MP_INTERP_DELAY_MS`). There must be no visible stutter at ≤150 ms ping.
- **Free roam gives a spawn loadout + a menu for vehicles, weapons, health and armour.** No Ammu-Nation, and no world pickups beyond whatever `freeroam_miami.scm` places locally.
- **macOS, Linux and Windows** build and run. CI builds the game on all three.
- The game assets aren't on the dev machine. Every task must at least **compile on all three OSes in CI**. In-game checks are listed per task and collected in `docs/multiplayer/testing.md`, to be run once the owner installs the game.
- Style: `CODING_STYLE.md`, tabs, `CName` classes with static `Init/Update/Shutdown`, `nil` instead of `nullptr` in `src/` code.

## Review Focus

1. **Opening the pause menu during a session.** The game sim pauses, but networking must keep servicing, so nobody times out and remote players resume smoothly. Owned by Task 3. Test: `NetGame` services the client from the top of `CGame::Process` regardless of pause; in-game check "pause for 30 s, others still see you, no disconnect".
2. **A player leaves while driving a car others are riding in, or while being shot at.** The puppet and its blip are removed, the vehicle stays in the world (ownership passes to a passenger or it becomes local-only), and no dangling pointer remains. Owned by Task 6. Test: `test_gameprotocol` + in-game check "kill the friend's game while you're their passenger".
3. **Model not yet streamed** when a remote player or car appears (a far-away area, or a vehicle model never loaded). Create it only after `CStreaming::HasModelLoaded`, and buffer its snapshots in the meantime. Owned by Tasks 5–6. In-game check "friend spawns a Hunter across the map".
4. **Death while in a vehicle, or simultaneous deaths.** Respawn always lands at a spawn point with the loadout, with no money loss, no clock jump, and no hospital taxi. Owned by Task 7.
5. **Typing in chat.** `T` opens chat. While typing, player controls are disabled, so `W`/`F` don't drive or exit the car, and cheat strings aren't triggered. `Esc` cancels without opening the pause menu. Owned by Task 8.

---

## File Structure

```
mp/common/GameProtocol.h/.cpp     game messages (types 64..95), Write/Read, tested
mp/tests/test_gameprotocol.cpp
mp/tools/fakeclient.cpp           + --bot walk|drive mode (sends PLAYER_STATE / VEHICLE_STATE)
src/multiplayer/NetGame.h/.cpp    CNetGame: lifecycle, settings, Service/Process/Render2d hooks, message dispatch
src/multiplayer/MpText.h/.cpp     MpGetText(key): wide strings for "MP_*" keys (no GXT editing)
src/multiplayer/MpMenu.cpp        frontend Multiplayer page + text-entry widget
src/multiplayer/RemotePlayer.h/.cpp  CRemotePlayer: puppet ped, interpolation, anims, blip, name tag
src/multiplayer/NetVehicles.h/.cpp   CNetVehicles: net id <-> CVehicle*, ownership, create/remove, state send/apply
src/multiplayer/NetCombat.h/.cpp     damage interception, shot visuals, death/respawn, kill feed
src/multiplayer/NetChat.h/.cpp       chat input + chat/notice log rendering
src/multiplayer/FreeroamMenu.h/.cpp  in-game overlay: vehicles, weapons, health, armour
src/multiplayer/SpawnPoints.h/.cpp   free-roam spawn points + loadout
src/core/config.h                 #define MULTIPLAYER
src/core/Game.cpp                 hooks in Initialise / ReInitGameObjectVariables / Process / ShutDown
src/core/main.cpp                 hook in Render2dStuff
src/control/Script.cpp            force freeroam_miami.scm in a session
src/control/GameLogic.cpp         MP respawn branch
src/peds/PedFight.cpp             InflictDamage hook
src/text/Text.cpp                 MpGetText fallback in CText::Get
src/core/MenuScreensCustom.cpp, src/core/Frontend.h   Multiplayer page
src/CMakeLists.txt, premake5.lua  link mpcommon/enet, add src/multiplayer
.github/workflows/multiplayer.yml game build jobs on 3 OSes
docs/multiplayer/testing.md       in-game manual test checklist
```

---

### Task 1: Game protocol messages + fake client bot

**Files:**
- Create: `mp/common/GameProtocol.h/.cpp`, `mp/tests/test_gameprotocol.cpp`
- Modify: `mp/tools/fakeclient.cpp`

**Interfaces:**
- Consumes: `CByteWriter/CByteReader`, `MSG_GAME_FIRST` (Plan 1 Task 2), `CNetClient::SendGame`, `SessionTime` (Plan 1 Task 6).
- Produces `enum eGameMsg : uint8_t` and structs (`Write`/`Read` like Plan 1). Positions, velocities and quats are `float[3]`/`float[4]`; velocity is units/ms.
  - `GMSG_PLAYER_STATE = 64` (unreliable, all-but-self): `u32 time; u16 skin; u8 flags (PSF_IN_VEHICLE=1, PSF_DEAD=2, PSF_AIMING=4, PSF_FIRING=8, PSF_CROUCH=16, PSF_JUMPING=32); float pos[3]; float heading; float vel[3]; u8 moveState; u8 weapon; float aimPos[3]; u8 health; u8 armour; u16 vehicleId; u8 seat` (seat 0 = driver, 1..3 passengers; vehicle fields only meaningful with `PSF_IN_VEHICLE`).
  - `GMSG_VEHICLE_STATE = 65` (unreliable, all-but-self, sent by the owner): `u32 time; u16 vehicleId; float pos[3]; float quat[4]; float vel[3]; float turn[3]; float steer; float gas; float brake; u8 flags (VSF_HANDBRAKE=1, VSF_SIREN=2, VSF_LIGHTS=4, VSF_ENGINE=8); u16 health (0..1000)`.
  - `GMSG_VEHICLE_CREATE = 66` (reliable, all-but-self): `u16 vehicleId; u16 model; u8 col1; u8 col2; float pos[3]; float quat[4]`.
  - `GMSG_VEHICLE_REMOVE = 67` (reliable): `u16 vehicleId`.
  - `GMSG_VEHICLE_ENTER = 68` (reliable → host; the host rebroadcasts the accepted result to all as `GMSG_VEHICLE_SEAT`): `u16 vehicleId; u8 seat`.
  - `GMSG_VEHICLE_SEAT = 69` (reliable, host → all): `u16 vehicleId; u8 playerId; u8 seat; u8 accepted` (`seat = 0xFF` means "left the vehicle").
  - `GMSG_VEHICLE_EXIT = 70` (reliable → host): `u16 vehicleId`.
  - `GMSG_DAMAGE = 71` (reliable → victim): `u8 victimId; u8 weapon; float amount; u8 piece; u8 direction`.
  - `GMSG_DIED = 72` (reliable, all): `u8 killerId (MP_PLAYER_NONE if none); u8 weapon`.
  - `GMSG_RESPAWNED = 73` (reliable, all): `float pos[3]; float heading`.
  - `GMSG_SHOT = 74` (unreliable, all-but-self): `u8 weapon; float from[3]; float to[3]`.
  - `GMSG_WORLD = 75` (reliable, host → all, on each join and every 10 s): `u8 hour; u8 minute; u8 weather`.
  - Range `80..95` is reserved for Plan 3 (races).
  - Vehicle ids: `u16 = (ownerPlayerId << 12) | counter(1..4095)`. Helper: `uint16_t MpMakeVehicleId(uint8_t owner, uint16_t counter)`, `uint8_t MpVehicleIdOwner(uint16_t id)`.
- Fake client `--bot walk|drive --at X,Y,Z`. `walk` sends `GMSG_PLAYER_STATE` at 20 Hz moving in a 10 m circle around the point. `drive` sends `GMSG_VEHICLE_CREATE` once (model from `--model`, default 132, to be checked in-game), then `GMSG_VEHICLE_STATE` + `GMSG_PLAYER_STATE` (in vehicle, seat 0) along a 40 m circle at ~15 m/s. Model ids are verified in-game later; a wrong default only affects the bot.

- [ ] **Step 1: Write failing tests** in `test_gameprotocol.cpp`: a round trip for every struct with non-default values; `PLAYER_STATE` serialized size ≤ 64 bytes and `VEHICLE_STATE` ≤ 80 bytes (bandwidth budget: 8 players × 20 Hz × ~140 B ≈ 22 KB/s downlink per client); `MpMakeVehicleId(3, 7)` → owner 3; a truncated `VEHICLE_STATE` fails `Read`.
- [ ] **Step 2: Run, expect fail.** `cmake --build build-mp` fails because `GameProtocol.h` is missing.
- [ ] **Step 3: Implement** `GameProtocol` and the bot mode.
- [ ] **Step 4: Run, expect pass.** `ctest --test-dir build-mp --output-on-failure` is 100%. Manual check: relay + `fakeclient --host` + `fakeclient --join CODE --bot walk --at 0,0,10`; the host prints `[game] from=1 type=64` about 20 times per second.
- [ ] **Step 5: Commit.** `git commit -m "mp: game protocol messages and fake client bot"`

---

### Task 2: Build the game with MULTIPLAYER on all three OSes

**Files:**
- Modify: `src/core/config.h`, `src/CMakeLists.txt`, `premake5.lua`, `.github/workflows/multiplayer.yml`
- Create: `src/multiplayer/NetGame.h/.cpp` (stub: `Init/Shutdown/Service/Process/Render2d/IsActive` returning inactive)

**Interfaces:**
- Produces `class CNetGame` (all static): `void Init()`, `void Shutdown()`, `void Service()` (pumps the network; called every frame including when paused), `void Process()` (simulation sync; called only when not paused), `void Render2d()`, `bool IsActive()`, `bool IsHost()`, `uint8 MyId()`, `CNetClient &Client()`.
- `config.h`: `#if !defined(ANDROID) && !defined(__SWITCH__) && !defined(RW_PS2)` → `#define MULTIPLAYER`.
- CMake: `if(TARGET mpcommon) target_link_libraries(${EXECUTABLE} PRIVATE mpcommon) endif()`. The glob already picks up `src/multiplayer`.
- premake: add `project "enet"` (static, `vendor/enet/*.c`, includedir `vendor/enet/include`) and `project "mpcommon"` (static, `mp/common/*.cpp`), plus `files { addSrcFiles("src/multiplayer") }`, `includedirs { "src/multiplayer", "mp/common", "vendor/enet/include" }`, `links { "mpcommon", "enet" }`; on Windows also `links { "ws2_32", "winmm" }`.
- CI jobs added to `multiplayer.yml` (triggered additionally by `src/**`, `premake5.lua`, `CMakeLists.txt`), each with `submodules: recursive` checkout (`vendor/librw` is a submodule):
  - `game-linux` (ubuntu-latest): `apt-get install libglfw3-dev libopenal-dev libmpg123-dev libgl-dev libx11-dev`; `cmake -B build -DREVC_VENDORED_LIBRW=ON -DLIBRW_PLATFORM=GL3 -DLIBRW_GL3_GFXLIB=GLFW -DREVC_AUDIO=OAL && cmake --build build`.
  - `game-macos` (macos-latest): `brew install glfw openal-soft mpg123`, same CMake flags.
  - `game-windows` (windows-latest): `premake5.exe vs2022`; `msbuild build/reVC.sln /p:Configuration=Release /p:Platform=win-amd64-librw_gl3_glfw-oal`.
  - Executors: check the exact cache-variable names in the root `CMakeLists.txt` and `cmake/` before writing the flags. The names above are the expected ones.

- [ ] **Step 1: Make the stub and build changes.**
- [ ] **Step 2: Build locally on Linux.** `git submodule update --init vendor/librw`, then install deps (`sudo apt-get install libglfw3-dev libopenal-dev libmpg123-dev`; ask the owner to run it if `sudo` isn't available), then build as in the `game-linux` job. Expected: `build/src/reVC` links.
- [ ] **Step 3: Single-player guard check.** Build once with `MULTIPLAYER` commented out. It must compile, proving every hook is guarded.
- [ ] **Step 4: Push; all CI jobs green** (3 × mp tests, 3 × game builds).
- [ ] **Step 5: Commit.** `git commit -m "mp: build game with MULTIPLAYER on macOS, Linux and Windows"`

---

### Task 3: Session lifecycle, settings, text, and world start

**Files:**
- Modify: `src/multiplayer/NetGame.h/.cpp`; `src/core/Game.cpp` (hooks); `src/control/Script.cpp` (`CTheScripts::Init`); `src/text/Text.cpp` (`CText::Get`)
- Create: `src/multiplayer/MpText.h/.cpp`, `src/multiplayer/SpawnPoints.h/.cpp`

**Interfaces:**
- Consumes: `CNetClient`, `INetClientListener` (Plan 1).
- Produces on `CNetGame`:
  - `bool HostSession()` / `bool JoinSession(const char *code)`: read name + relay from settings, start connecting, and show "Connecting…" via `CMessages`.
  - `void LeaveSession()`.
  - `const char *InviteCode()`.
  - Settings `char ms_playerName[24]`, `char ms_relayAddress[64]` (`"host:port"`), stored in `reVC.ini` section `[Multiplayer]` keys `Name`, `Relay`, through the existing `CIniFile`/`LoadINISettings` pattern.
  - Default relay = `MP_DEFAULT_RELAY` in `NetGame.h` = `"127.0.0.1:7777"` until the owner supplies the VPS hostname. That's a one-line change; record it in `docs/multiplayer/README.md` open questions.
- Lifecycle (exact):
  1. The menu calls `HostSession`/`JoinSession`.
  2. `OnSessionReady` sets `ms_bWantWorld = true` and triggers `FrontEndMenuManager.m_bWantToRestart = true` (the same path as New Game).
  3. `CTheScripts::Init`: `#ifdef MULTIPLAYER if (CNetGame::WantsWorld()) ScriptToLoad = 1;` runs after the debug-key checks, so `freeroam_miami.scm` loads.
  4. After the world starts (end of `CGame::Initialise` / `ReInitGameObjectVariables`, after the script's first `Process`), `CNetGame::OnWorldStarted()` teleports the player to `CSpawnPoints::Pick(myId)`, gives the loadout, and marks the session as in-game. The host also starts sending `GMSG_WORLD`.
  5. `OnSessionEnded`/`OnConnectionLost`/`LeaveSession` show the reason (`MP_ENDED_HOST`, `MP_LOST`), clear all remote state, and return to the frontend start menu.
- Hooks in `CGame::Process`: `CNetGame::Service()` as the first line (before the pause checks), and `CNetGame::Process()` right after `CWorld::Process()`.
- Saving is disabled in a session (the frontend save entries are hidden or greyed while `IsActive()`).
- `MpGetText(const char *key)` returns a `wchar*` for keys starting with `"MP_"` from a static table, or `nil`. `CText::Get` checks it first under `#ifdef MULTIPLAYER`. Initial keys: `MP_MENU` "MULTIPLAYER", `MP_HOST` "HOST SESSION", `MP_JOIN` "JOIN WITH CODE", `MP_NAME` "NAME", `MP_RELAY` "RELAY SERVER", `MP_CODE` "CODE", `MP_LEAVE` "LEAVE SESSION", `MP_CONNECTING` "Connecting...", `MP_ENDED_HOST` "Session ended: host left", `MP_LOST` "Connection lost", `MP_NOTFOUND` "No session with that code", `MP_FULL` "Session is full (8 players)", `MP_VERSION` "Version mismatch: update reVC", `MP_BADNAME` "Choose a different name".
- `CSpawnPoints`: `static void Pick(uint8 playerId, CVector &pos, float &heading)` from a table of 8 points around Ocean Beach (coordinates verified in-game; placeholders are fine until then, marked `// TODO verify in-game`), and `static void GiveLoadout(CPlayerPed*)`: `WEAPONTYPE_BASEBALLBAT` 1, `WEAPONTYPE_COLT45` 68, health 100, armour 0.

- [ ] **Step 1: Implement**, with every engine edit wrapped in `#ifdef MULTIPLAYER` and `if (CNetGame::...)`.
- [ ] **Step 2: Build on Linux + CI green.**
- [ ] **Step 3: Add in-game checks** to `docs/multiplayer/testing.md` (create the file with a short "how to run two instances on one PC: relay on localhost, second instance from a copy of the game dir" section):
  - Host shows a `VICE-XXXX` code and spawns in Ocean Beach with a bat and pistol.
  - `fakeclient --join CODE` makes "Lance joined" appear.
  - Pause for 30 s and the fake client stays connected.
  - Killing the relay gives "Connection lost" and a return to the menu.
  - With no session, New Game still runs the story normally.
- [ ] **Step 4: Commit.** `git commit -m "mp: session lifecycle, settings, MP text and world start"`

---

### Task 4: Multiplayer frontend page

**Files:**
- Create: `src/multiplayer/MpMenu.cpp`
- Modify: `src/core/Frontend.h` (add `MENUPAGE_MULTIPLAYER` before `MENUPAGE_OUTRO`), `src/core/MenuScreensCustom.cpp` (start menu + pause menu entry `MP_MENU`, plus the page row)

**Interfaces:**
- Consumes: `CNetGame::HostSession/JoinSession/LeaveSession/InviteCode`, settings fields.
- Produces a text-entry widget built from `FrontendOptionAddDynamic`: `DrawFunc` returns the buffer (with a blinking `_` while editing), and `SELECT` toggles edit mode. In edit mode, read characters via `CPad::GetPad(0)->GetCharJustDown(c)` for printable ASCII, `GetBackspaceJustDown`, `GetReturnJustDown` (commit) and `GetEscapeJustDown` (cancel, restoring the old value), and swallow these inputs so the menu doesn't navigate.
- Page entries:
  - `MP_NAME` [text, max 23], `MP_RELAY` [text, max 63].
  - `MP_HOST` → `HostSession()`; `MP_CODE` [text, auto-uppercased, max 9] + `MP_JOIN` → `JoinSession(code)`.
  - `MP_LEAVE` (only while active).
  - While active, the page header shows `Code: VICE-XXXX — n/8 players`.
  - Join failures show the matching `MP_*` text for 5 s on the page.
  - Settings save on change.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build + CI green.**
- [ ] **Step 3: Add in-game checks:**
  - Type a name with backspace, Esc restores it, and the name persists after restart.
  - The wrong code `VICE-ZZZZ` shows "No session with that code".
  - The 9th player gets "Session is full".
- [ ] **Step 4: Commit.** `git commit -m "mp: multiplayer frontend page with text entry"`

---

### Task 5: Remote players on foot

**Files:**
- Create: `src/multiplayer/RemotePlayer.h/.cpp`
- Modify: `src/multiplayer/NetGame.cpp` (send local `GMSG_PLAYER_STATE` at 20 Hz; dispatch received ones), `src/core/main.cpp` (`CNetGame::Render2d()` in `Render2dStuff` before `CFont::DrawFonts`)

**Interfaces:**
- Consumes: `CPoseBuffer`, `SPose` (Plan 1), `GMSG_PLAYER_STATE` (Task 1).
- Produces `class CRemotePlayer`:
  - Members: `uint8 m_id; char m_name[24]; CCivilianPed *m_pPed; CPoseBuffer m_poses; SPlayerState m_last; int32 m_blip; bool m_bSpawned`.
  - `static CRemotePlayer *Get(uint8 id)`, `static void Add(uint8 id, const char *name)`, `static void Remove(uint8 id)` (deletes the ped via `CWorld::Remove` + `delete`, clears the blip, and drops it from any vehicle), `static void RemoveAll()`.
  - `void OnState(const SPlayerState&)`, `void Update(uint32 renderTime)`, `static void UpdateAll()` (called from `CNetGame::Process`), `static void RenderNameTags()`.
- Behaviour:
  - **Spawn:** on the first state, request the skin model (`CStreaming::RequestModel(skin, STREAMFLAGS_DEPENDENCY)`). Once `HasModelLoaded`, create `new CCivilianPed(PEDTYPE_CIVMALE, skin)` with `CharCreatedBy = UNK_CHAR` (skips `ProcessControl` and is never deleted by population; see `CivilianPed.cpp:227`), `bRespondsToThreats = false`, `CWorld::Add`. Skins for M1: `skin = 0` means "default by id" from a fixed list of 8 common ped model ids chosen in-game.
  - **Each frame:** `Sample(sessionNow − MP_INTERP_DELAY_MS)`, set position and heading (`m_fRotationCur = m_fRotationDest = heading`), `SetMoveSpeed(vel)`. If the sampled pose is more than 20 m from the ped, `Teleport` it.
  - **Animation:** compute it from `moveState` + speed: `ANIM_STD_IDLE` / `ANIM_STD_WALK` / `ANIM_STD_RUN` / `ANIM_STD_RUNFAST` via `CAnimManager::BlendAnimation(clump, m_animGroup, anim, 4.0f)` on change only; crouch / jump / aim use the matching std anims, and weapon-in-hand via `GiveWeapon` + `SetCurrentWeapon` on change. Dead → `SetDie(ANIM_STD_KO_FRONT, 4.0f, 0.0f)` once.
  - **Blip:** `CRadar::SetEntityBlip(BLIP_CHAR, CPools::GetPedPool()->GetIndex(ped), colourForId(id), BLIP_DISPLAY_BLIP_ONLY)`, scale 2. There are 8 distinct colours by id.
  - **Name tag:** project head pos + 0.5 m with `CSprite::CalcScreenCoors`. Draw only within 80 m and on screen; scale by distance; show name + a small health bar. Pattern: `CPed::DebugRenderOnePedText` (`PedDebug.cpp:300`).
  - Local sending: every 50 ms, fill `SPlayerState` from `FindPlayerPed()`: position, heading `m_fRotationCur`, `GetMoveSpeed()` converted to units/ms (`/ (1000/50)`, since engine speeds are per 1/50 s frame step), `m_nMoveState`, weapon, health/armour, flags; send unreliable, all-but-self.
- Unit-safe conversion: `float MpSpeedToNet(float engineSpeed)` / `MpSpeedFromNet` in `NetGame.h` (factor `50.0f/1000.0f`), commented.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build + CI green.**
- [ ] **Step 3: Add in-game checks:**
  - `fakeclient --bot walk --at <spawn>`: a ped walks a smooth circle with a name tag and blip.
  - Two real instances: running, sprinting, jumping and crouching all look right on the other screen.
  - The friend teleports via the menu and their ped jumps, without sliding across the map.
  - Simulate 150 ms latency with `sudo tc qdisc add dev lo root netem delay 75ms` on Linux. No stutter.
- [ ] **Step 4: Commit.** `git commit -m "mp: remote players on foot with interpolation, blips and name tags"`

---

### Task 6: Vehicles

**Files:**
- Create: `src/multiplayer/NetVehicles.h/.cpp`
- Modify: `src/multiplayer/NetGame.cpp`, `src/multiplayer/RemotePlayer.cpp`

**Interfaces:**
- Consumes: Task 1 vehicle messages, `CRemotePlayer`.
- Produces `class CNetVehicles` (static):
  - `uint16 Register(CVehicle*)`: the local player starts driving an unregistered car (spawned or hijacked ambient). It allocates an id, converts the car to `MISSION_VEHICLE` so it isn't culled, and broadcasts `GMSG_VEHICLE_CREATE`.
  - `CVehicle *Find(uint16 id)`, `uint16 IdOf(CVehicle*)`, `void OnCreate/OnRemove/OnState/OnSeat(...)`, `void Update(uint32 renderTime)`, `void SendOwned(uint32 now)`, `void RemoveAllOwnedBy(uint8 player)`, `void Clear()`.
- **Ownership:** the owner is whoever is driver. Only the owner sends `VEHICLE_STATE`, at 20 Hz while it moves, and at 2 Hz when it's still and driverless. On `GMSG_VEHICLE_SEAT` with seat 0 for another player, ownership transfers.
- **Seat arbitration:** the host processes `VEHICLE_ENTER`/`EXIT` in arrival order, rejects a taken seat, and broadcasts `VEHICLE_SEAT`.
  - Locally, when the player starts entering a networked car (`CPed::SetEnterCar` path), send `VEHICLE_ENTER` and let the anim play. If rejected, abort with `SetExitCar` / `RestorePreviousState`.
  - Non-networked cars need no arbitration.
- **Remote vehicles:**
  - Create on `GMSG_VEHICLE_CREATE` after the model streams. Use `CAutomobile`/`CBike`/`CBoat` per the model's vehicle type with `MISSION_VEHICLE`, `bIsLocked = false`, `SetStatus(STATUS_PLAYER_DISABLED)` (no AI, no player input), and `CWorld::Add`.
  - Each frame, sample the pose buffer, set matrix from pos + quat, `SetMoveSpeed`/`SetTurnSpeed`, and copy `m_fSteerAngle`, gas, brake and handbrake for wheel and lights visuals. Snap if the error is > 10 m.
  - If no state arrives for 3 s and the owner left, the vehicle becomes local (`STATUS_ABANDONED`).
- **Remote occupants:** on `VEHICLE_SEAT`, remove the puppet from the world-walking state and seat it as in `CREATE_CHAR_INSIDE_CAR` (`Script.cpp:3114`) / `CREATE_CHAR_AS_PASSENGER` (`Script2.cpp:1096`): `SetDriver`/`AddPassenger`, `m_pMyVehicle`, `bInVehicle = true`, `SetPedState(PED_DRIVING)`, `bUsesCollision = false`, `AddInCarAnims`. On leaving, reverse it, place the ped beside the door, and restore collision. Remote enter/exit is shown as a warp (the anim plays only for the local player); this is an M1 builder call.
- **Local player as passenger in a remote-owned car:** the normal enter-as-passenger anim; the car keeps following the owner's snapshots.
- **Owner leaves the session:** `RemoveAllOwnedBy(player)`. A car with the local player inside transfers to the local player (they become the owner if they're the driver, else it goes local). Other cars are removed unless someone is in them.

- [ ] **Step 1: Add the ownership-transfer rule test** to `test_gameprotocol.cpp`. Make it a pure helper `uint8_t MpNextVehicleOwner(uint8_t currentOwner, const uint8_t seats[4], uint8_t leavingPlayer)` in `GameProtocol`. It returns the driver if any, else the lowest-seat remaining occupant, else `MP_PLAYER_NONE`. Cases: driver leaves with a passenger in seat 2 → passenger; empty car → NONE; a non-occupant leaves → unchanged.
- [ ] **Step 2: Run, expect fail; implement the helper; run, expect pass.**
- [ ] **Step 3: Implement `CNetVehicles`** and its wiring.
- [ ] **Step 4: Build + CI green.**
- [ ] **Step 5: Add in-game checks:**
  - `fakeclient --bot drive`: a car drives a smooth circle with a ped in it.
  - Two instances: hijack a taxi and the friend sees it appear and drive.
  - The friend gets in as a passenger and both views match.
  - Both try the driver seat at once: one wins, one gets out.
  - Quit the game as driver with a passenger: the passenger keeps the car.
  - Drive at 150 ms latency: no stutter, and collisions between two player cars push both.
- [ ] **Step 6: Commit.** `git commit -m "mp: networked vehicles with ownership and seats"`

---

### Task 7: Combat, death and respawn

**Files:**
- Create: `src/multiplayer/NetCombat.h/.cpp`
- Modify: `src/peds/PedFight.cpp` (`CPed::InflictDamage`, top of the function), `src/weapons/Weapon.cpp` (`CWeapon::Fire`, after a local player shot), `src/control/GameLogic.cpp` (WASTED branch)

**Interfaces:**
- Consumes: `CRemotePlayer::FindByPed(CPed*)` (add it to Task 5's class), `GMSG_DAMAGE/DIED/RESPAWNED/SHOT`.
- Produces `class CNetCombat` (static): `bool InterceptDamage(CPed *victim, CEntity *by, eWeaponType w, float dmg, ePedPieceTypes piece, uint8 dir)` (returns true when the engine should skip applying it), `void OnLocalShot(eWeaponType, const CVector &from, const CVector &to)`, `void OnDamage/OnDied/OnRespawned(...)`, `void OnLocalWasted()`, `void RespawnLocal()`.
- Rules (exact):
  - **Shooter-side hit detection, victim-side application.**
    - If `victim` is a remote puppet: if `by` is the local player or the local player's vehicle, send `GMSG_DAMAGE` to that player. Either way, return true (puppets never take local damage).
    - On receiving `GMSG_DAMAGE`, call `FindPlayerPed()->InflictDamage(puppetOf(sender), weapon, amount, piece, dir)` so the engine's own armour, health and death logic run. The puppet as `damagedBy` gives the right `m_lastDamEntity`.
  - **Remote shots are visual only:** on `GMSG_SHOT`, play the muzzle flash and bullet trace (`CBulletTraces::AddTrace`), the weapon sound via `DMAudio` and the fire anim on the puppet. No damage.
  - **Death:**
    - When the local player becomes wasted in a session, send `GMSG_DIED{killer = id of m_lastDamEntity's puppet or NONE}`.
    - Kill feed via `CMessages::AddMessageJumpQ`-style text: "Lance killed Tommy" or "Tommy died", composed into a static wide buffer pool (`AddMessage` keeps the pointer).
  - **MP WASTED branch** (in `CGameLogic::Update`, under `IsActive()`): no money loss, no `ClearWeapons`, no `PassTime(720)`, no taxi. After the normal fade, `CRestart::OverrideNextRestart(spawnPos, heading)` with `CSpawnPoints::Pick`, then `RestorePlayerStuffDuringResurrection`, then `GiveLoadout`, then send `GMSG_RESPAWNED`.

- [ ] **Step 1: Implement**, with the hooks wrapped in `#ifdef MULTIPLAYER` + `IsActive()`.
- [ ] **Step 2: Build + CI green.**
- [ ] **Step 3: Add in-game checks:**
  - Shoot a friend: their health drops on their HUD.
  - Armour absorbs first.
  - A kill shows "Tommy killed Lance".
  - The victim respawns at a spawn point with a bat and pistol, no money lost, clock unchanged.
  - The friend's shots show flashes and tracers on your screen.
  - Running a friend over in a car damages them.
  - Dying in a car respawns you on foot.
- [ ] **Step 4: Commit.** `git commit -m "mp: player-vs-player damage, shots, death and respawn"`

---

### Task 8: Chat, notices, time and weather

**Files:**
- Create: `src/multiplayer/NetChat.h/.cpp`
- Modify: `src/multiplayer/NetGame.cpp`

**Interfaces:**
- Consumes: `CNetClient::SendChat`, `OnChat`, `OnPlayerJoined/Left`, `GMSG_WORLD`.
- Produces `class CNetChat` (static): `void Update()` (input), `void Render()`, `void AddLine(const char *text, uint8 colourId)`, `bool IsTyping()`.
- **Input:** `T` (when not typing, not in the menu, and controls are enabled) starts typing and calls `CPad::GetPad(0)->SetDisablePlayerControls(PLAYERCONTROL_PLAYERINFO)`; use a dedicated bit if a free one exists, and note which. Read chars like Task 4's widget. `Enter` sends (if non-empty, ≤ 127 chars), `Esc` cancels; both re-enable controls. While typing, clear `CPad`'s PC cheat string each frame so typed text can't trigger cheats, and swallow `Esc` so the pause menu doesn't open.
- **Display:** the last 8 lines, top-left, each visible for 10 s after arrival (all visible while typing), names coloured by player id. Notices: "Lance joined", "Lance left".
- **Time/weather:**
  - The host sends `GMSG_WORLD` on every join and every 10 s with `CClock::GetGameClockHours/Minutes` and `CWeather::NewWeatherType`.
  - Clients call `CClock::SetGameClock(h, m)` if off by more than 2 game minutes, and `CWeather::ForceWeatherNow(w)` on change.
  - On leaving, call `CWeather::ReleaseWeather()`.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build + CI green.**
- [ ] **Step 3: Add in-game checks:**
  - Chat both ways.
  - Typing `W`, `F` or a cheat code in chat does nothing in game.
  - `Esc` cancels chat without opening the pause menu.
  - The host changes weather via the debug menu and the friend sees it within 10 s.
  - Time matches within 2 minutes.
- [ ] **Step 4: Commit.** `git commit -m "mp: text chat, join/leave notices, time and weather sync"`

---

### Task 9: Free-roam menu (vehicles, weapons, health, armour)

**Files:**
- Create: `src/multiplayer/FreeroamMenu.h/.cpp`
- Modify: `src/multiplayer/NetGame.cpp`, `src/multiplayer/MpText.cpp`

**Interfaces:**
- Consumes: `CNetVehicles::Register`, `CPed::GiveWeapon`, `CNetChat::IsTyping`.
- Produces `class CFreeroamMenu` (static): `void Update()`, `void Render()`, `bool IsOpen()`. It also exposes `void AddItem(const char *label, void (*action)(void))` on the root list so Plan 3 can add "Start race".
- Open/close with `F2` (keyboard) while in a session and not typing. Navigate with arrows/`Enter`/`Backspace` (back); the pad's D-pad works too if simple. Player controls are disabled while open.
- **Root:** Vehicles ▸ (categories Sports, Muscle, Bikes, Off-road, Boats, Helicopters, each a list of `{label, modelId}` defined in a table, model ids verified in-game), Weapons ▸ (melee, pistols, SMGs, shotguns, rifles, heavy; each gives 500 ammo or 1 for melee), Health (to max), Armour (to 100), Close.
- **Vehicle spawn:**
  - Request + load the model with `CTimer::Suspend`/`Resume` around `LoadAllRequestedModels`, as `LOAD_ALL_MODELS_NOW` does.
  - Create it 5 m in front of the player, facing the player's heading, on the ground (`CWorld::FindGroundZForCoord`), with `MISSION_VEHICLE`. Boats spawn only if the target point is water.
  - Warp the player into the driver seat and `Register` it.
  - The player's previously spawned vehicle is removed if empty (max 1 spawned car per player).

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build + CI green.**
- [ ] **Step 3: Add in-game checks:**
  - Every vehicle entry spawns the right model, and the friend sees it.
  - Spawning a second car removes the first (if empty).
  - Every weapon category works.
  - Health and armour fill.
  - F2 while typing does nothing.
  - Spawning a boat on land shows "Must be on water".
- [ ] **Step 4: Commit.** `git commit -m "mp: free-roam menu for vehicles, weapons, health and armour"`

---

### Task 10: Wrap-up

**Files:**
- Modify: `docs/multiplayer/README.md` (Status, decisions log: freeroam script, UNK_CHAR puppets, warp enter/exit for remotes, shooter-side hits), `docs/multiplayer/testing.md`

- [ ] **Step 1: Full checklist pass** once the owner has the game installed. Run every in-game check in `testing.md` with two real instances + 1 bot and record the results in the file. If the game still isn't installed, mark the plan "code complete, in-game unverified" in the README Status.
- [ ] **Step 2: Single-player regression:** New Game, the first mission, save/load, and the pause menu all behave as before.
- [ ] **Step 3: Commit.** `git commit -m "docs: multiplayer free roam status and test results"`
