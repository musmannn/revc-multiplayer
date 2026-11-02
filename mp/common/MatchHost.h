#pragma once

// The host's side of a deathmatch / team deathmatch: invite -> accepts ->
// start -> scores -> end. Engine-free, shared by the game host
// (src/multiplayer/NetMatch) and the fake client's --start-match.

#include <stdint.h>

#include "GameProtocol.h"
#include "MatchLogic.h"
#include "HillLogic.h"

class IMatchHostSink
{
public:
	virtual ~IMatchHostSink(void) {}
	virtual void SendInvite(const SMatchInvite &msg) = 0;
	virtual void SendStart(const SMatchStart &msg) = 0;
	virtual void SendScores(const SMatchScores &msg) = 0;	// unreliable
	virtual void SendEnd(const SMatchScores &msg) = 0;	// reliable
	virtual void SendCancel(const SMatchCancel &msg, uint8_t target) = 0;	// NONE = everyone
};

class CMatchHost
{
public:
	// Enums, not static consts, so they're never odr-used (no out-of-class definitions needed).
	enum : uint32_t
	{
		INVITE_MS = 15000,
		COUNTDOWN_MS = 5000,
		SCORES_MS = 500,	// 2 Hz
		DM_SCORE_LIMIT = 20,
		TDM_SCORE_LIMIT = 30,
		KOTH_SCORE_LIMIT = 60,		// seconds on the hill
		TKOTH_SCORE_LIMIT = 150,
		TIME_LIMIT_MS = 10 * 60 * 1000,
		KOTH_TIME_LIMIT_MS = 8 * 60 * 1000,
		STUNT_TIME_LIMIT_MS = 4 * 60 * 1000
	};
	static uint16_t ScoreLimitFor(uint8_t mode);	// 0 = none
	static uint32_t TimeLimitFor(uint8_t mode);
private:
	enum eState { STATE_IDLE, STATE_INVITING, STATE_RUNNING };
	eState m_state;
	uint16_t m_matchId;
	uint8_t m_mode;
	uint8_t m_startedBy;
	uint32_t m_deadline;
	uint8_t m_accepted[MP_MAX_PLAYERS];
	int m_numAccepted;
	CMatchState m_match;
	uint32_t m_lastScoresMs;
	bool m_hasArena;
	SRaceRoute m_arena;
	CHillState m_hill;
	bool m_havePos[MP_MAX_PLAYERS];
	float m_pos[MP_MAX_PLAYERS][3];
	uint32_t m_hillMs[MP_MAX_PLAYERS];	// time on the hill not yet turned into points
	uint32_t m_lastHillMs;

	void UpdateHill(uint32_t now);

	void FillScores(SMatchScores &msg) const;
public:
	CMatchHost(void);

	void Reset(void);
	bool IsBusy(void) const { return m_state != STATE_IDLE; }
	bool IsRunning(void) const { return m_state == STATE_RUNNING; }
	uint16_t MatchId(void) const { return m_matchId; }
	const CMatchState &Match(void) const { return m_match; }

	// arena: optional for deathmatch modes (spawns), needed by Hold the Hill
	// and Stunt Challenge (else the requester gets MATCH_NO_ARENA). A mode
	// out of range is treated as deathmatch.
	bool Request(uint8_t from, uint8_t mode, bool starterPlays, uint32_t now, IMatchHostSink *sink, const SRaceRoute *arena = nullptr);
	bool GetInvite(SMatchInvite &out) const;
	void OnAccept(uint8_t from, uint16_t matchId, uint32_t now);
	void OnDeath(uint8_t victim, uint8_t killer, uint32_t time);
	void OnRespawned(uint8_t id);
	// Where a player is (from their PLAYER_STATE), for Hold the Hill.
	void SetPosition(uint8_t id, const float pos[3]);
	// A GMSG_STUNT report. Points over CStuntTracker::MAX_POINTS are capped.
	void OnStunt(uint8_t from, const SStunt &msg, uint32_t now);
	const CHillState &Hill(void) const { return m_hill; }
	bool HasArena(void) const { return m_hasArena; }
	const SRaceRoute &Arena(void) const { return m_arena; }
	void OnLeft(uint8_t id);
	void Update(uint32_t now, IMatchHostSink *sink);
};
