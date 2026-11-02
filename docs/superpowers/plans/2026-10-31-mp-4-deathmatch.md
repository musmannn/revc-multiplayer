# Multiplayer Plan 4: Deathmatch, Team Deathmatch and Scoreboards (milestone 2)

**Goal:** From free roam, any player can start a deathmatch or team deathmatch
that the others join, fight to a kill or time limit with a live scoreboard,
and see the same final results, then go back to free roam.

**Status of decisions:** the owner hasn't specified milestone 2 details. Every
rule below is a **builder call**, logged in `docs/multiplayer/README.md` and
open to change.

**Architecture:** same shape as races (Plan 3). Match rules live engine-free in
`mp/common` (`CMatchState` scoring and end conditions, `CMatchHost` invite →
start → scores → end) and are unit tested. Kills come from the existing
`GMSG_DIED` (killer attribution is Plan 2's shooter-side detection), so
there's no new kill message. The game side (`src/multiplayer/NetMatch`) does
invites, spawns, friendly fire, the HUD and the scoreboard.

## Rules (builder calls)

- Modes: **Deathmatch** (free for all) and **Team Deathmatch** (2 teams, Red and Blue).
- Presets: kill limit 20 (team DM: team score 30), time limit 10 min; whichever comes first.
- Score: kill +1. A suicide or death with no killer −1 (score can go negative). Killing a teammate: −1 to the killer and no kill.
- Teams: the host assigns accepted players alternately by accept order (Red first), so team sizes differ by at most 1.
- Friendly fire is off in team DM: damage between teammates isn't sent.
- Standings: deathmatch by score desc, then kills desc, then deaths asc, then id; team DM by team score, ties are a draw.
- Invite open 15 s, then 5 s countdown; players are respawned at spawn points with the **match loadout** (pistol, Uzi, shotgun, full health and armour) when it starts and after each death.
- Leavers are kept on the scoreboard with their score. If fewer than 2 players are left in deathmatch (or a team is empty in team DM), the match ends.
- One match or race at a time per session.
- Scoreboard: hold `Tab` in any session: players with id colours; in a match also kills/deaths/score (and team).

## Messages (range 88..95)

- `GMSG_MATCH_REQUEST = 88` (→ host): `u8 mode`.
- `GMSG_MATCH_INVITE = 89` (host → all): `u16 matchId; u8 mode; u8 startedBy; u16 scoreLimit; u32 timeLimitMs; u32 deadline`.
- `GMSG_MATCH_ACCEPT = 90` (→ host): `u16 matchId`.
- `GMSG_MATCH_START = 91` (host → all): `u16 matchId; u8 mode; u16 scoreLimit; u32 timeLimitMs; u32 startTime; u8 n; {u8 id; u8 team}[n]` (mode and limits repeated from the invite, so START stands alone).
- `GMSG_MATCH_SCORES = 92` (host → all, 2 Hz, unreliable) and `GMSG_MATCH_END = 93` (reliable, final): `u16 matchId; u16 teamScore[2]; u8 n; {u8 id; u8 team; i16 score; u16 kills; u16 deaths}[n]` in standings order.
- `GMSG_MATCH_CANCEL = 94`: `u16 matchId; u8 reason` (`MATCH_BUSY`, `MATCH_NOT_ENOUGH`, `MATCH_ABORTED`).

## Tasks

1. **Match rules + messages** (`mp/common/MatchLogic`, `GameProtocol`), TDD: scoring, suicides, team kills, limits, leavers, ordering, team balance; round trips, 8-player SCORES ≤ 120 bytes.
2. **`CMatchHost` + fake client** (`--bot dm` accepts invites and reports deaths; `--start-match dm|tdm` on the fake host). Manual check: relay + host + 2 bots, the host prints scores and a final END.
3. **In-game match flow** (`src/multiplayer/NetMatch`): F2 "Start match >", F3 accept (shared with races), match spawns + loadout, friendly fire, kill limit HUD (score, time left), results panel, Tab scoreboard, team colours on name tags.
4. **Docs**: README status + decisions, `testing.md` checks.
