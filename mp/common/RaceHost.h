#pragma once

// The host's side of a race session: invite -> accepts -> start -> standings
// -> end. Engine-free, so the game host (src/multiplayer/NetRace) and the fake
// client's --start-race share it. Sending goes through IRaceHostSink.

#include <stdint.h>

#include "GameProtocol.h"
#include "RaceLogic.h"

class IRaceHostSink
{
public:
	virtual ~IRaceHostSink(void) {}
	virtual void SendInvite(const SRaceInvite &msg) = 0;		// to all
	virtual void SendStart(const SRaceStart &msg) = 0;		// to all
	virtual void SendStandings(const SRaceStandings &msg) = 0;	// to all, unreliable
	virtual void SendEnd(const SRaceStandings &msg) = 0;		// to all, reliable
	// target MP_PLAYER_NONE = everyone
	virtual void SendCancel(const SRaceCancel &msg, uint8_t target) = 0;
};

class CRaceHost
{
public:
	static const uint32_t INVITE_MS = 15000;	// invites stay open this long
	static const uint32_t COUNTDOWN_MS = 5000;	// deadline -> GO
	static const uint32_t STANDINGS_MS = 250;	// 4 Hz
private:
	enum eState { STATE_IDLE, STATE_INVITING, STATE_RUNNING };
	eState m_state;
	uint16_t m_raceId;
	uint8_t m_startedBy;
	SRaceRoute m_route;
	uint32_t m_deadline;
	uint8_t m_accepted[MP_MAX_PLAYERS];
	int m_numAccepted;
	CRaceState m_race;
	uint32_t m_lastStandingsMs;

	void FillStandings(SRaceStandings &msg) const;
public:
	CRaceHost(void);

	void Reset(void);
	bool IsBusy(void) const { return m_state != STATE_IDLE; }
	bool IsRacing(void) const { return m_state == STATE_RUNNING; }
	uint16_t RaceId(void) const { return m_raceId; }
	const CRaceState &Race(void) const { return m_race; }
	const SRaceRoute &Route(void) const { return m_route; }

	// A player asks for a race on route. If one is already pending or
	// running, the requester gets RACE_BUSY and this returns false.
	// starterRaces: the requester is auto-accepted (a fake host isn't).
	bool Request(uint8_t from, const SRaceRoute &route, bool starterRaces, uint32_t now, IRaceHostSink *sink);
	// The open invite, for players who join while it's open.
	bool GetInvite(SRaceInvite &out) const;
	void OnAccept(uint8_t from, uint16_t raceId, uint32_t now);
	void OnCross(uint8_t from, const SRaceCross &msg);
	void SetDistance(uint8_t id, float d);
	void OnLeft(uint8_t id);
	void Update(uint32_t now, IRaceHostSink *sink);
};
