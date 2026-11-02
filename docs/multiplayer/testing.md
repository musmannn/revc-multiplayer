# Multiplayer in-game test checklist

These checks need the original GTA Vice City files, so CI can't run them.
Run them once the game is installed and record the result and date next to
each item (`✅ 2026-11-05`, or `❌` plus a note).

The engine-free layer (protocol, relay, client, race rules) is covered by
`ctest --test-dir build-mp`. This file is for everything that needs the game
running.

## Setup: two instances on one PC

1. Build `revc-relay` and `revc-fakeclient` (see `README.md` in this folder) and
   start a relay: `./build-mp/revc-relay --port 7777`.
2. Make a second copy of the game directory (e.g. `GTAVC` → `GTAVC-2`) with the
   same reVC binary, so the two instances don't share `reVC.ini`.
3. In each copy, set *Multiplayer → Relay server* to `127.0.0.1:7777` and give
   each a different *Name*.
4. Start both. Host in one, join with the code in the other. Run them windowed.
5. A third "player" is a bot:
   `./build-mp/revc-fakeclient --relay 127.0.0.1 --join VICE-XXXX --name Bot --bot walk --at X,Y,Z`.

Simulated latency on Linux (remove afterwards with `sudo tc qdisc del dev lo root`):

```bash
sudo tc qdisc add dev lo root netem delay 75ms
```

## Plan 2, Task 3: session lifecycle and world start

- [ ] The host gets a `VICE-XXXX` code and spawns on Washington Beach (around 79.7, -867.3) with a bat and a pistol.
- [ ] `revc-fakeclient --join CODE --name Lance` makes Lance appear in the session.
- [ ] Pause for 30 s: the fake client stays connected and nobody times out.
- [ ] Killing the relay shows "Connection lost" and opens the frontend menu.
- [ ] With no session, New Game still runs the story normally (main.scm, intro).
- [ ] Holding `R`/`D` at New Game still picks the debug scripts as before.
- [ ] Spawn points (`src/multiplayer/SpawnPoints.cpp`, a 3 m ring around the start `freeroam_miami.scm` uses) are on open ground: nobody spawns inside or on top of anything, and 8 players all get a free spot.
- [ ] The Multiplayer page opens with *Name* selected; the *Session* status row is at the bottom.
- [ ] Hosting on a cold start (first run, no shader cache) doesn't lose the session while the world loads; the relay log shows no `lost` for the host.

## Plan 2, Task 4: Multiplayer frontend page

- [ ] *Multiplayer* appears on the start menu and the pause menu.
- [ ] Type a name with Backspace; Esc restores the old value; the name persists after restarting the game (`reVC.ini` `[Multiplayer] Name`).
- [ ] While typing, arrow keys/Enter/Esc don't navigate the menu, and Esc doesn't close it.
- [ ] Shift gives capitals in the name; the code field is always upper case.
- [ ] Join with the wrong code `VICE-ZZZZ` shows "No session with that code" in the *Session* row.
- [ ] The 9th player gets "Session is full (8 players)".
- [ ] In a session, the *Session* row shows `VICE-XXXX - n/8 players`; Name/Relay/Host/Join are greyed out and Leave works.

## Plan 2, Task 5: remote players on foot

- [ ] `revc-fakeclient --join CODE --bot walk --at <spawn x,y,z>`: a ped walks a smooth 10 m circle with a name tag, a health bar and a coloured radar blip.
- [ ] Two real instances: walking, running, sprinting, jumping and crouching look right on the other screen.
- [ ] A friend teleports (e.g. with the free-roam menu or a respawn) and their ped jumps there instead of sliding across the map.
- [ ] With 150 ms simulated latency (`tc netem delay 75ms`), movement has no visible stutter.
- [ ] A friend's weapon in hand matches what they hold.
- [ ] Remote ped models (`sDefaultSkins` in `src/multiplayer/RemotePlayer.cpp`) are sensible civilians (placeholder ids).
- [ ] Peds/traffic AI ignore remote players (no fleeing or fighting), and remote players are never despawned when far away.

## Plan 2, Task 6: vehicles

- [ ] `revc-fakeclient --join CODE --bot drive --at <x,y,z>`: a car drives a smooth 40 m circle with the bot's ped at the wheel (`--model` default 132 is a placeholder).
- [ ] Two instances: hijack a taxi; the friend sees it appear and drive smoothly.
- [ ] Press enter next to a friend's car: you warp in as a passenger (you never drag them out); both views match while they drive.
- [ ] As the passenger, your controls don't steer the friend's car.
- [ ] Both players try the driver seat of an empty networked car at once: one gets it, the other is put back outside.
- [ ] Quit the game while driving with a passenger: the passenger keeps the car (it becomes theirs/local) and doesn't fall through the map.
- [ ] A friend leaves while their parked car is nearby: it disappears unless someone is in it.
- [ ] Drive at 150 ms latency: no stutter; driving into the friend's car pushes your car.
- [ ] A friend spawns a vehicle far away / a model you haven't loaded (e.g. a Hunter): it appears once streamed, without a crash.
- [ ] A friend's car blows up on your screen when it blows up on theirs.

## Plan 2, Task 7: combat, death and respawn

- [ ] Shoot a friend: their health drops on their HUD (not just on yours).
- [ ] Armour absorbs damage first.
- [ ] A kill shows "Tommy killed Lance" on both screens; dying alone shows "Lance died".
- [ ] The victim respawns at a spawn point with a bat and pistol, no money lost, clock unchanged, no hospital taxi.
- [ ] The friend's shots show tracers and gunshot sounds on your screen, but never hurt anything there.
- [ ] Running a friend over in a car damages them (on their screen); their car hitting you doesn't double the damage.
- [ ] Dying in a car respawns you on foot; the car stays behind.
- [ ] Both players die at the same time: both respawn normally.
- [ ] Single player: dying still costs $100 and weapons, passes time, and takes you to hospital.

## Plan 2, Task 8: chat, notices, time and weather

- [ ] `T` opens chat; messages go both ways and show with coloured names, top left, for 10 s (all lines while typing).
- [ ] While typing, `W`/`F`/arrow keys don't move or exit the car, and typing a cheat (e.g. `THUGSTOOLS`) does nothing.
- [ ] `Esc` cancels chat without opening the pause menu; `Enter` on an empty line just closes it.
- [ ] Pressing `Enter` to send doesn't also make you get out of the car.
- [ ] "Lance joined" / "Lance left" notices appear.
- [ ] The host changes the weather (debug menu) and the friend sees it within 10 s; game time matches within 2 minutes.
- [ ] After leaving the session the weather goes back to normal.

## Plan 2, Task 9: free-roam menu

- [ ] `F2` opens the menu; arrows/Enter/Backspace navigate; `F2` or *Close* closes it; player controls are off while it's open.
- [ ] Every vehicle entry spawns the right model 5 m ahead, with you in the driver seat, and the friend sees it.
- [ ] Spawning a second car removes the first if nobody is in it.
- [ ] Every weapon category gives the weapon (500 ammo, melee 1) and puts it in hand.
- [ ] *Health* and *Armour* fill up.
- [ ] `F2` while typing chat does nothing; `T` while the menu is open does nothing.
- [ ] Spawning a boat on land shows "Must be on water".

## Plan 3, Task 4: in-game races

Engine-free race rules and the race protocol are unit tested (`ctest`), and a
whole race runs without the game: relay + `revc-fakeclient --host --start-race "Test"` + two
`revc-fakeclient --join CODE --bot race`. These checks need the game:

- [ ] F2 → *Start race >* lists the routes from `data/mp/races/index.txt`; picking one posts "Tommy started … press F3 to join (15s)" to everyone, with a countdown top right.
- [ ] A full race against 2 `--bot race` bots: grid, frozen 3-2-1-GO with beeps, checkpoints, results.
- [ ] Two real players race; both see identical final results.
- [ ] Checkpoints: yellow cylinder + radar blip for the current one, a fainter one for the next, red for the finish; cutting a corner past a checkpoint doesn't count it.
- [ ] HUD shows POS n/m, LAP (circuits), TIME and the checkpoint counter.
- [ ] Blow up the car mid-race: respawn at the last checkpoint in a fresh car, facing the next one, frozen for 1 s; the timer keeps running.
- [ ] Hold `H` for 1.5 s while flipped: respawn the same way. Leave the car for 5 s: respawn too.
- [ ] Die mid-race: respawn at the last checkpoint (not a free-roam spawn point), then get a fresh car.
- [ ] Kill a racer's game mid-race: they're DNF and the race still ends.
- [ ] Nobody accepts: only the starter races (a time trial).
- [ ] Start a race while one is running: "A race is already running".
- [ ] Non-racers see the invite and "Lance won … in mm:ss.hh" only.

## Plan 3, Task 5: route recorder and built-in routes

- [ ] `/pos` prints your coordinates and heading; `/` lines never reach other players.
- [ ] `/rec new Test`, `/rec type sprint`, drive a car and `/rec vehicle`, `/rec grid` ×2, `/rec cp` ×3, `/rec undo`, `/rec save` writes `data/mp/races/test.route`, adds it to `index.txt`, and it appears under *Start race >* right away.
- [ ] `/rec save` with too few checkpoints says why and saves nothing.
- [ ] Record and drive the 4 built-in routes (not done yet: needs the game):
  - [ ] **Ocean Drive Sprint**: sprint along Ocean Drive, north to south.
  - [ ] **Starfish Loop**: circuit, 3 laps, around Starfish Island.
  - [ ] **Bridge Run**: sprint from Ocean Beach across a bridge to Downtown.
  - [ ] **Little Havana Circuit**: circuit, 2 laps, through Little Havana / Little Haiti.
  - Each: 8 grid slots about 4 m apart sideways and 8 m back, 8–20 checkpoints, a sports car; every checkpoint reachable and no grid slot inside a building.

## Plan 4: deathmatch, team deathmatch, scoreboards (milestone 2)

Match rules and the match host are unit tested, and whole matches run with fake
clients: relay + `revc-fakeclient --host --start-match tdm` + several
`revc-fakeclient --join CODE --bot dm`. These checks need the game:

- [ ] F2 → *Start match >* → Deathmatch / Team Deathmatch posts an invite to everyone; F3 joins; fewer than 2 players cancels it.
- [ ] At the start everyone is put on the spawn points with pistol, Uzi, shotgun, full health and armour, frozen through 3-2-1-FIGHT.
- [ ] Kills count on the HUD (SCORE n/20, POS) and the Tab scoreboard; dying respawns you with the match loadout.
- [ ] Suicide (e.g. your own grenade) costs a point.
- [ ] Team deathmatch: name tags are red/blue; shooting a teammate does nothing; the HUD shows RED n / BLUE n.
- [ ] The match ends at the kill limit or after 10 minutes, everyone sees the same results panel, and the chat says who won.
- [ ] A player leaving mid-match keeps their score on the board; the last opponent leaving ends it.
- [ ] Starting a race during a match (or the other way round) says "already on".
- [ ] Tab outside a match shows the session code and who's in it.
- [ ] Tab is also the on-foot "action" key in the default controls (pick up / answer phone). Check that holding it for the scoreboard doesn't get in the way; if it does, move the scoreboard to another key.
- [ ] F2 is blocked while you're in a match.

## Plan 5: dedicated server, route editor, more modes (milestone 3)

Engine-free parts are unit tested (editor commands, route wire format, hill and
stunt scoring, seat table, the dedicated host with real clients over loopback).
Headless checks with no game:

```bash
./build-mp/revc-server --with-relay 7777 --code-file code.txt
./build-mp/revc-fakeclient --relay 127.0.0.1 --join $(cat code.txt) --name Tommy --bot koth --start-match koth --arena gamefiles/data/mp/arenas/washington_beach.route
./build-mp/revc-fakeclient --relay 127.0.0.1 --join $(cat code.txt) --name Lance --bot stunt --at 0,-500,10
```

Tommy should end with about one point per second he held the hill. These
checks need the game:

**Dedicated server**

- [ ] Join a `revc-server` session from the Multiplayer menu: no ped, blip or name tag for the server; the *Session* row and Tab show `n/7`; the server isn't on the Tab list.
- [ ] Time and weather follow `--hour`/`--weather`; the clock moves a minute per second.
- [ ] Two players fight over one car's driver seat: one gets it, the other is put back out.
- [ ] Races and every match mode started from F2 run through the server the same as with a player host.
- [ ] `--playlist "race:…,koth"`: with 2 players in, an invite appears by itself ("Next up: …"), and the next one after the gap.
- [ ] The `--motd` welcome shows in chat when you join.
- [ ] Stop the server: everyone sees the session end.

**Route editor**

- [ ] `/rec help` lists the commands. `/rec new Test`, `/rec grid`, `/rec cp` ×3: the checkpoints and grid slots show as markers (first green, last red) within 200 m.
- [ ] `/rec ins 2`, `/rec move 3`, `/rec del 1`, `/rec radius 1 15`, `/rec undo` do what they say, and the markers follow.
- [ ] `/rec test` starts a race on the unsaved route; a friend who doesn't have the file can join and race it.
- [ ] `/rec save` writes `data/mp/races/test.route` and lists it in `index.txt`; `/rec load Test` brings it back.
- [ ] `/rec arena Beach`, `/rec spawn` ×2, `/rec hill` ×2, `/rec save` writes `data/mp/arenas/beach.route`; it appears under F2 → *Match arena >*.
- [ ] Fix the placeholder **Washington Beach** arena (`data/mp/arenas/washington_beach.route`): spawns on open ground, three hills that are reachable on foot and fair to fight over, a stunt vehicle (PCJ-600, 191) that fits. `/rec load Washington Beach`, then `/rec movegrid N`, `/rec move N`, `/rec save`.

**Hold the Hill / Team Hold the Hill**

- [ ] F2 → *Start match >* → Hold the Hill with an arena: everyone spawns at its spawns; the hill is a blue cylinder with a radar blip.
- [ ] Alone in the hill: it turns green, "HILL: HOLDING", and the score goes up a point a second. With an enemy in it: red, "CONTESTED", no points. Dead players don't count.
- [ ] After a minute the hill moves to the next point ("The hill has moved"), and the HUD's MOVES timer counts down to it.
- [ ] Team version: teammates in the hill together all score; one enemy blocks. Friendly fire is off.
- [ ] Kills show on the board but don't add points. First to 60 (team: 150) or 8 minutes ends it.
- [ ] Without any arena installed, asking for Hold the Hill says it needs an arena.

**Stunt Challenge**

- [ ] Everyone spawns on the arena's spawns in the arena's vehicle, frozen through 3-2-1-GO.
- [ ] A jump of under 0.7 s scores nothing; a bigger one shows "AIR n" while in the air and "STUNT +n" on a clean landing; landing on the roof or side shows "CRASHED!" and scores nothing.
- [ ] A barrel roll or flip shows "1x FLIP" and roughly doubles the points; a flat 360 in the air shows "1x SPIN".
- [ ] Getting out, or wrecking the car, gives you a new one at your spawn after 3 s.
- [ ] After 4 minutes the most points wins.
