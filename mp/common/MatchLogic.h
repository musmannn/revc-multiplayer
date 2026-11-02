#pragma once

// Match rules: deathmatch / team deathmatch (milestone 2), Hold the Hill and
// Stunt Challenge (milestone 3). Engine-free; the host runs it from the
// GMSG_DIED reports it already gets, plus hill time and stunt points.

#include <stdint.h>

#include "MpTypes.h"

enum eMatchMode
{
	MATCH_DM,	// free for all
	MATCH_TDM,	// two teams
	MATCH_KOTH,	// Hold the Hill, free for all
	MATCH_TKOTH,	// Hold the Hill, two teams
	MATCH_STUNT,	// Stunt Challenge, free for all
	MATCH_NUM_MODES
};

// Team modes: TDM and team Hold the Hill.
static inline bool MpIsTeamMode(uint8_t mode) { return mode == MATCH_TDM || mode == MATCH_TKOTH; }
// Hold the Hill and Stunt Challenge need an arena (hills / spawns + vehicle).
static inline bool MpModeNeedsArena(uint8_t mode) { return mode == MATCH_KOTH || mode == MATCH_TKOTH || mode == MATCH_STUNT; }

static const uint8_t MP_TEAM_RED = 0;
static const uint8_t MP_TEAM_BLUE = 1;
static const uint8_t MP_TEAM_NONE = 0xFF;

struct SMatchPlayer
{
	uint8_t id;
	uint8_t team;		// MP_TEAM_* (NONE in deathmatch)
	int16_t score;
	uint16_t kills;
	uint16_t deaths;
	bool left;		// left the session; stays on the board
	bool dead;		// died, not respawned yet (Hold the Hill ignores the dead)
};

class CMatchState
{
	uint8_t m_mode;
	SMatchPlayer m_players[MP_MAX_PLAYERS];
	int m_numPlayers;
	uint16_t m_scoreLimit;
	uint32_t m_timeLimitMs;
	uint32_t m_startTime;
	bool m_over;

	SMatchPlayer *Find(uint8_t id);
	void CheckOver(void);
	int PresentOnTeam(uint8_t team) const;
public:
	CMatchState(void);

	// Red/Blue alternately in order, so sizes differ by at most one.
	static void AssignTeams(int n, uint8_t *outTeams);

	void Start(uint8_t mode, const uint8_t *ids, const uint8_t *teams, int n, uint16_t scoreLimit, uint32_t timeLimitMs, uint32_t startTime);
	// Deathmatch modes: kill +1. Suicide or no killer: -1 for the victim.
	// Team kill: -1 for the killer, no kill. A killer outside the match (or
	// who left) changes no score. Other modes count kills and deaths only
	// (a team kill isn't a kill).
	void OnDeath(uint8_t victim, uint8_t killer, uint32_t time);
	void OnRespawned(uint8_t id);
	// Hill seconds, stunt points. Ignored before the start, once over, for
	// players who left and in deathmatch modes.
	void AddPoints(uint8_t id, int points, uint32_t time);
	void OnLeft(uint8_t id);
	void Update(uint32_t now);

	bool IsOver(void) const { return m_over; }
	// Score desc, kills desc, deaths asc, id. Team modes: the winning team's
	// players first (Red on a tie), same order inside a team.
	int GetStandings(uint8_t outIds[MP_MAX_PLAYERS]) const;
	int16_t TeamScore(uint8_t team) const;
	uint8_t WinnerId(void) const;	// free for all: the leader
	uint8_t WinnerTeam(void) const;	// team modes: MP_TEAM_NONE on a draw
	const SMatchPlayer *Get(uint8_t id) const;
	int NumPlayers(void) const { return m_numPlayers; }
	uint8_t Mode(void) const { return m_mode; }
	uint16_t ScoreLimit(void) const { return m_scoreLimit; }	// 0 = none
	uint32_t TimeLimitMs(void) const { return m_timeLimitMs; }
	uint32_t StartTime(void) const { return m_startTime; }
};
