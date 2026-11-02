# Multiplayer Plan 5: Dedicated Server, Route Editor and More Modes (milestone 3)

**Goal:** a session can be hosted by a headless `revc-server`; routes and
arenas can be authored and edited in-game and raced straight away; and there
are three more modes: Hold the Hill, Team Hold the Hill and Stunt Challenge.

**Status of decisions:** every rule below is a **builder call** (see the spec's
milestone 3 addition), logged in `docs/multiplayer/README.md`.

**Architecture:** same shape as Plans 3–4. All rules are engine-free in
`mp/common` with doctest tests: the route editor commands (`CRouteEditor`),
route wire encoding, hill scoring (`CHillState`), stunt scoring
(`CStuntTracker`) and seat arbitration (`CSeatTable`). The dedicated host is
`CDedicatedHost` in `mp/server` (lib `servercore`), driven by `revc-server` and
by integration tests with real `CNetClient`s over loopback. The game side
(`src/multiplayer`) only measures, draws and spawns.

## Protocol version 2

- `MSG_CREATE_SESSION` gains `u8 flags` (`MP_SESSION_DEDICATED = 1`).
  `MSG_JOIN_RESULT` gains `u8 flags`, the session's flags. `CNetClient::Host`
  takes the flags; `CNetClient::SessionFlags()` returns them on both sides.
- **Route data** (`MpWriteRouteData`/`MpReadRouteData`): `str name; u8 type;
  u8 laps; u16 vehicle; u8 nGrid; {i16 x,y,z (1/8 m); u16 heading (1/65536
  turn)}[nGrid]; u8 nCp; {i16 x,y,z; u8 radius (1/2 m)}[nCp]`. 64 checkpoints
  and 8 slots are 554 bytes, so a request or invite carrying a route stays
  well under `MP_MAX_PACKET` (1200). Positions outside ±4096 m don't encode.
  Every route read from the wire is checked by `MpValidateRoute` (the same
  rules as the file parser).
- `GMSG_RACE_REQUEST` (80): `route data` (was the route name).
- `GMSG_RACE_INVITE` (81): `u16 raceId; u8 startedBy; u32 deadline; route
  data` (was the name). Racers race the invite's route, so no file is needed.
- `GMSG_MATCH_REQUEST` (88): `u8 mode; u8 hasArena; [route data]`.
- `GMSG_MATCH_INVITE` (89): the Plan 4 fields, then `u8 hasArena; [route data]`.
- `GMSG_STUNT` (96, player → host, reliable): `u16 matchId; u16 points; u16
  airMs; u8 flips; u8 spins`.
- New cancel reasons: `RACE_BAD_ROUTE` now means "failed validation";
  `MATCH_NO_ARENA` (the mode needs an arena with hills / a vehicle).

## Rules (builder calls)

- **Arenas** are `.route` files with `type arena`: `grid` lines are spawn
  points (1..8), `cp` lines are hill points (1..16), `vehicle` is optional (the
  stunt vehicle; default PCJ-600, 191). They live in `data/mp/arenas/` with an
  `index.txt`, like races. One placeholder arena, "Washington Beach", sits
  around the free-roam start and needs checking in-game.
- **Hold the Hill:** one hill active at a time, moving to the next point every
  60 s (in file order, wrapping). A player is on the hill if alive and within
  the hill's radius horizontally and 4 m vertically. Free for all: a lone
  player on the hill scores 1 point per full second; two or more is contested.
  Team: every player on the hill scores if all players on it are on one team.
  Limits: 60 points (team: 150), 8 minutes. Kills and deaths are counted but
  don't score. Spawns are the arena's.
- **Stunt Challenge:** each player spawns at an arena spawn in the arena's
  vehicle and has 4 minutes (no score limit; most points wins). A stunt starts
  when all wheels leave the ground and is banked on landing with the vehicle's
  up axis at least 60° from horizontal (up.z ≥ 0.5). It counts if it lasted at
  least 700 ms. Points = (airMs / 50 + horizontal metres + 2 × height metres)
  × (1 + flips + spins / 2), where a flip is 360° of accumulated rotation of the
  up axis and a spin 360° of heading turned in the air. A stunt is capped at
  2000 points. A crash landing banks nothing. Leaving the car for 5 s, or a
  wreck, respawns a fresh one at your spawn.
- Deathmatch and team deathmatch may name an arena; then its spawns are used.
- **Dedicated server:** id 0, never a participant. The clock runs one game
  minute per real second from `--hour` (default 12), weather fixed by
  `--weather` (default 0, sunny). Seats: first come, first served, from the
  `VEHICLE_CREATE`/`REMOVE`/`SEAT` traffic it sees. The playlist runs its next
  entry 60 s (`--playlist-gap`) after the last event ended, if at least 2
  players are in. A message of the day goes to every joiner as chat.

## Tasks

1. **Protocol v2** (`Protocol`, `GameProtocol`, `RaceRoute`, relay,
   `CNetClient`), TDD: session flags through the relay, route data round trips
   and limits, validation, new request/invite layouts.
2. **Route editor** (`mp/common/RouteEditor`), TDD: every `/rec` command,
   undo, arena type, `MpWriteRoute` round trip with `MpParseRoute`. Game:
   `RouteRecorder` becomes the chat front end, previews markers, `/rec load`,
   `/rec test`, saves arenas to `data/mp/arenas`.
3. **Modes** (`MatchLogic`, `MatchHost`, `mp/common/HillLogic`,
   `mp/common/StuntLogic`), TDD: hill movement, occupancy, contesting, team
   rules, limits; stunt scoring, landings, crashes, caps. Game: arena list in
   the match menu, hill marker + blip + HUD, stunt measurement and HUD, stunt
   car respawn.
4. **Dedicated server** (`mp/server`): `CSeatTable`, `CDedicatedHost`,
   `revc-server` (options, playlist, embedded relay, code file), integration
   tests with clients over loopback. Game: no ped for a dedicated host,
   scoreboard and status show "server".
5. **Fake client**: `--bot koth` (walks to the active hill), `--bot stunt`
   (reports stunts), `--start-match koth|tkoth|stunt --arena FILE`, routes sent
   with `--start-race`.
6. **Docs**: README status, decisions and code map; `testing.md`;
   `relay-setup.md` gets a dedicated server section.
