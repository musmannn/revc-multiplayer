# reVC Multiplayer — Design

Date: 2026-10-31
Status: Approved by owner 2026-10-31

## Intent

Play Vice City with friends, the way GTA Online feels. One player hosts a
session. Friends join with a short invite code from anywhere on the internet.
Everyone drives, walks and fights in the same city and can see each other.
From free roam, anyone can start a race that the others join. More modes, like
deathmatch, come later on the same foundation.

The goal is that it's fun with a small group of friends, not that it matches
Rockstar's scale. Something playable early beats a big design that never ships.

## Decisions made by the owner

| Topic | Decision |
|---|---|
| Joining | **Invite code.** The host gets a short code (e.g. `VICE-4821`). Friends type it in and join over the internet with no port forwarding. A small lobby/relay server handles this. |
| Hosting | **A player hosts.** The host's game is in charge of the session. If the host quits, the session ends. A standalone dedicated server is a possible later addition, not part of this work. |
| First milestone | **Free roam + races.** Deathmatch and other modes come in a later milestone. |
| Traffic & pedestrians | **Local per player.** Each client spawns its own ambient traffic and peds as in single player. Only players, the vehicles players are using, and mode objects (checkpoints etc.) are synced. |
| Where the relay runs | **Both a cheap/free cloud VPS and the owner's own PC.** The relay is one program that runs on either. The game ships with the owner's cloud relay address built in as the default, so friends only type the code. The Multiplayer menu has a *Relay server* setting to switch to another address (e.g. the owner's PC, with one forwarded port) without editing files. Deployment notes cover both: a Linux VPS (e.g. Oracle Cloud free tier) and a home PC with port forwarding. |
| Platforms | **macOS, Linux and Windows**, all first-class, cross-play between them. |
| Weapons, health, armour | **Spawn loadout + weapon menu.** Players spawn and respawn with a basic loadout (e.g. pistol + bat). The same in-game menu that spawns vehicles also gives weapons, health and armour. No world pickups or Ammu-Nation in milestone 1. |

## Goals (milestone 1)

1. **Multiplayer menu** in the frontend: *Host Session*, *Join with Code*,
   and player name.
2. **Sessions of up to 8 players** (host included).
3. **Free roam:**
   - See other players as peds with their chosen skin, name tag above head,
     and radar blip.
   - Their on-foot movement, animations (walk/run/sprint/jump/crouch/aim)
     and vehicle driving look smooth (interpolated) on everyone's screen.
   - Entering/exiting vehicles, driver and passenger seats (a friend can
     ride in your car).
   - Weapons: other players see you aim and fire, and damage between players
     works (health, armour, death, respawn).
   - An in-game free-roam menu to spawn a vehicle and to get weapons, health
     and armour (free roam has no story progression or script pickups).
     Spawn/respawn gives a basic loadout.
   - Text chat (`T` to type, messages shown on screen) and join/leave
     notices.
   - Same time of day and weather for everyone, set by the host.
4. **Races:**
   - Any player in free roam can open a race menu, pick a built-in route,
     and send the session an invitation; others accept or ignore.
   - Racers are teleported to a starting grid in a chosen car, frozen,
     then a 3-2-1-GO countdown.
   - Checkpoints shown as the game's standard checkpoint markers/coronas plus
     radar blips; next checkpoint previewed.
   - Live position (1st/2nd/…), lap counter for circuit routes, race timer.
   - Finish screen with ordering and times; everyone returns to free roam.
   - Wrecked or stuck cars: respawn at the last checkpoint.
   - Ships with at least 4 routes (mix of sprint and circuit, land-only),
     stored as data files so more can be added without recompiling.
5. **Invite-code lobby/relay server:** small standalone program that hands out
   codes, matches joiners to hosts, and relays game traffic when players
   can't reach each other directly.
6. **Platforms:** macOS, Linux and Windows desktop are all first-class, and
   any mix of them can play in the same session. The game, relay and fake
   client build on all three, and CI builds and tests on all three. Android
   is out of scope.

## Non-goals (milestone 1)

- Story missions or co-op story. The story script does not run in a
  multiplayer session.
- Synced traffic and pedestrians (decided: local per player).
- Deathmatch, team modes, or other game modes (milestone 2).
- Persistent progression: money, XP, owned property, unlocks, saves.
- Voice chat.
- Steam/Discord friend-list integration.
- Anti-cheat. Sessions are among friends and the host is trusted.
- Dedicated servers, public server browser, matchmaking with strangers.
- Android.

## Anti-goals (what would make this a failure even if it "works")

- **Breaking single player.** Single player must play exactly as before when
  no session is active. Multiplayer code paths must be inert outside a session.
- **Jittery or teleporting players.** If friends' cars visibly stutter or warp
  during normal driving on a typical home connection (≤150 ms ping), it fails
  the "fun with friends" test.
- **Setup friction.** If joining needs router config, editing ini files, or
  sharing IP addresses, it misses the "invite like GTA 5" intent.
- **Divergent race results.** Two players must never see different winners.

## Constraints and facts from the codebase

- The engine is built around a single player. `NUMPLAYERS = 1` in
  `src/core/config.h`, and about 1,000 call sites in 67 files reach "the player"
  through `FindPlayerPed()`, `FindPlayerVehicle()` and
  `CWorld::PlayerInFocus` (`src/core/PlayerInfo.cpp`). Rewriting the engine
  to have N real players is out of scope. **Approach:** the local player stays
  the one and only `CPlayerPed`. Remote players are separate ped entities
  whose state comes from the network, not from AI or the pad. This is how
  VC:MP and MTA:VC approached it.
- The main loop is `CGame::Process()` in `src/core/Game.cpp`. Networking
  needs a send/receive point each frame there.
- Frontend menus are table-driven, with custom pages already supported
  (`src/core/MenuScreensCustom.cpp`, `CUSTOM_FRONTEND_OPTIONS`).
- Starting a game normally loads and runs `main.scm` (`CTheScripts::Init` in
  `src/control/Script.cpp`). A multiplayer session needs a way to start the
  world without the story script driving it, while keeping what the world
  needs (map, streaming, zones, car generators).
- Build systems: CMake (with conan) and premake. Third-party code is vendored
  under `vendor/`. New dependencies follow that pattern.
- Coding style: follow `CODING_STYLE.md` and the surrounding code (C-style
  C++, `CName` classes, static managers).
- Feature toggles live in `src/core/config.h`. Multiplayer goes behind a
  `MULTIPLAYER` define so it can be compiled out. This is also an upstream
  rule: the README says code that isn't reversed original code must be wrapped
  in preprocessor conditions.
- The game needs original GTA Vice City assets to run, and they are not in the
  repo. They aren't installed on the dev machine yet, so early work has to be
  testable without them: unit tests for protocol/serialization, and a
  headless fake-client tool against the real relay. In-game verification waits
  until the owner installs the game.
- The relay is a public server for a reverse-engineered game. The owner
  accepts that when running it on a VPS.

## How it works (parts the owner decided, plus the architecture they imply)

**Host-authoritative, client-predicted.** The host's game is the authority for
session membership, game mode state (race start, checkpoint progress, finish
order), time and weather, and resolving player-vs-player damage. Each player
simulates their own ped and vehicle locally (so their own controls feel
instant) and sends state snapshots. Everyone else renders remote players by
interpolating between received snapshots.

**Invite-code relay.** The host registers with the lobby/relay server and gets
a code. Joiners send the code to the server and are connected to the host.
Traffic goes through the relay (simplest way to work behind any home router).
Direct connections with NAT punch-through are an optional optimisation, not a
requirement.

**Ambient world stays local.** Because traffic and peds are local per player,
a remote player's car can hit a car that only exists on the hitter's screen.
That's accepted. Collisions between two *player* vehicles are handled by each
owner simulating their own car against a locally placed copy of the other.

**Race finish order (builder call, marked as such).** The host keeps a
session clock that every client syncs to. Each racer stamps every checkpoint
and finish crossing with session time, using its own simulation, and sends
it. The host orders results by those timestamps and broadcasts the final
standings. A close finish then matches what each racer saw on their own
screen, not whichever packet reached the host first. The host is trusted and
reported times aren't validated (no anti-cheat).

## Left to the builder

- Networking library choice (ENet or similar small reliable-UDP library,
  vendored under `vendor/`), packet format, tick rates, interpolation buffer
  size, compression.
- Exactly how remote players are represented (`CPed` subclass or flagged
  `CCivilianPed`) and how they're kept from being touched by ped AI,
  population cleanup, and streaming removal.
- How remote vehicles are owned, created and removed, including vehicle
  ownership hand-off when a driver gets out.
- How the world starts without the story script.
- Route file format and the starting grid layout.
- Language/stack for the lobby/relay server (sharing protocol code with the
  game in C++ is a sensible default).
- Invite-code format and expiry.
- Spawn points for free roam and respawn after death.
- Choice of the 4+ built-in race routes.
- How to test networking without running several copies of the full game
  (e.g. a headless fake-client tool).

## Later milestones (for context, not this design)

- **Milestone 2:** Deathmatch and team deathmatch, with mode selection and
  scoreboards.
- **Milestone 3:** More modes (e.g. stunt challenges, capture-style modes), a
  race route editor, and a standalone dedicated server.

## Milestone 3 addition (2026-11-01, builder calls)

The owner asked for milestone 3 to be done ("complete the full multiplayer
game") without fixing its details, so everything in this section is a builder
call, logged in `docs/multiplayer/README.md` and open to change. Plan 5 has the
messages and tasks.

**Dedicated server.** `revc-server` is a headless program that hosts a session
instead of a player's game. It connects to a relay like any client (or runs
the relay itself, in the same process, with `--with-relay PORT`), creates the
session with a *dedicated* flag, and takes player id 0, so a dedicated session
has room for 7 players. It does everything the host's game does that isn't
about its own ped: the session clock, time and weather, seat arbitration for
vehicles, races and matches (including a playlist that starts events on its
own), and catch-up for late joiners. It never shows up in the world. Games
learn that a session is dedicated from the join result, so they don't create a
ped, blip or name tag for id 0.

**Routes travel with the race.** Race and arena data is sent inside the
request and the invite, so a route only has to exist on the requester's
machine. This is what makes the editor useful (a fresh route can be raced at
once) and lets a dedicated server run without the game's data files.

**Route editor.** The `/rec` chat recorder becomes an editor: load an existing
route, insert, move and delete checkpoints and grid slots, change radii, list
them, undo several steps, preview every checkpoint in the world while
editing, and test-race the unsaved route with the session. The same editor
authors **arenas** (`type arena`): spawn points and "hill" points for the new
modes, and spawns for deathmatch.

**More modes**, as new match modes on the deathmatch framework (invite, accept,
countdown, scores, results, scoreboard):

- **Hold the Hill** (free for all) and **Team Hold the Hill**: a hill zone moves
  between an arena's hill points every minute; standing in it with no enemy
  inside scores a point per second. Kills are counted but don't score.
- **Stunt Challenge**: everyone gets the arena's vehicle and has 4 minutes to
  score stunt points. Points come from airtime, distance, height, flips and
  spins, and only count on a clean landing. The player's own game measures
  the stunt (trusted, like race crossings) and reports it.

Still out of scope: a public server browser, matchmaking with strangers,
anti-cheat, persistent stats.
