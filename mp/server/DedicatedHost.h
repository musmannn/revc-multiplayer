#pragma once

// A session hosted by a program instead of a player's game (Plan 5). It does
// what the host's game does that isn't about its own ped: the session clock
// (CNetClient), time and weather, seat arbitration, races and matches, catch-up
// for late joiners, and a playlist that starts events by itself. Engine-free,
// shared by revc-server and the tests.

#include <stdint.h>

#include "NetClient.h"
#include "GameProtocol.h"
#include "RaceHost.h"
#include "MatchHost.h"
#include "SeatTable.h"

struct SPlaylistEntry
{
	bool race;		// else a match
	uint8_t mode;		// eMatchMode, for matches
	bool hasRoute;		// races always; matches if they have an arena
	SRaceRoute route;	// the race route, or the match's arena
};

struct SServerConfig
{
	enum { MAX_PLAYLIST = 32 };
	char name[MP_MAX_NAME_LEN + 1];
	char motd[MP_MAX_CHAT_LEN + 1];	// said to the session when someone joins ("" = nothing)
	uint8_t hour;			// start time of day
	uint8_t minute;
	uint8_t weather;
	bool clockRuns;			// one game minute per real second, like the game
	SPlaylistEntry playlist[MAX_PLAYLIST];
	int playlistLength;
	uint32_t playlistGapMs;		// idle time before the next playlist event
	int playlistMinPlayers;
	bool verbose;

	SServerConfig(void);
};

class CDedicatedHost : public INetClientListener, public IRaceHostSink, public IMatchHostSink
{
public:
	enum : uint32_t { WORLD_INTERVAL_MS = 10000, RECONNECT_MS = 5000 };
private:
	SServerConfig m_config;
	CNetClient m_client;
	CRaceHost m_race;
	CMatchHost m_match;
	CSeatTable m_seats;
	char m_relay[256];
	uint16_t m_port;
	bool m_running;
	uint32_t m_now;			// NowMs of the current Service()
	uint32_t m_clockStartMs;
	uint32_t m_lastWorldMs;
	bool m_present[MP_MAX_PLAYERS];
	bool m_haveState[MP_MAX_PLAYERS];
	char m_names[MP_MAX_PLAYERS][MP_MAX_NAME_LEN + 1];
	float m_pos[MP_MAX_PLAYERS][3];
	bool m_wasBusy;
	uint32_t m_idleSince;
	int m_nextPlaylist;
	uint32_t m_retryAt;		// reconnect after the session was lost; 0 = no
	bool m_sessionChanged;

	void Log(const char *fmt, ...);
	void Send(uint8_t route, uint8_t target, bool reliable, const CByteWriter &w);
	void SendWorld(uint8_t target);		// MP_PLAYER_NONE = everyone
	void CatchUp(uint8_t id);
	void UpdateRaceDistances(void);
	void UpdatePlaylist(void);
	bool Busy(void) const { return m_race.IsBusy() || m_match.IsBusy(); }
	uint32_t SessionNow(void) const { return m_client.SessionTime(m_now); }

	// INetClientListener
	void OnSessionReady(const char *code, uint8_t myId, bool isHost) override;
	void OnJoinFailed(eJoinResult result) override;
	void OnPlayerJoined(uint8_t id, const char *name) override;
	void OnPlayerLeft(uint8_t id) override;
	void OnConnectionLost(void) override;
	void OnSessionEnded(eEndReason reason) override;
	void OnChat(uint8_t from, const char *text) override;
	void OnGameMessage(uint8_t from, uint8_t type, CByteReader &body) override;

	// IRaceHostSink / IMatchHostSink
	void SendInvite(const SRaceInvite &m) override;
	void SendStart(const SRaceStart &m) override;
	void SendStandings(const SRaceStandings &m) override;
	void SendEnd(const SRaceStandings &m) override;
	void SendCancel(const SRaceCancel &m, uint8_t target) override;
	void SendInvite(const SMatchInvite &m) override;
	void SendStart(const SMatchStart &m) override;
	void SendScores(const SMatchScores &m) override;
	void SendEnd(const SMatchScores &m) override;
	void SendCancel(const SMatchCancel &m, uint8_t target) override;

	CDedicatedHost(const CDedicatedHost&);
	CDedicatedHost &operator=(const CDedicatedHost&);
public:
	explicit CDedicatedHost(const SServerConfig &config);

	// Connects to the relay and creates a dedicated session. If the session
	// is lost later, Service() makes a new one (with a new code).
	bool Start(const char *relayHost, uint16_t port, uint32_t nowMs);
	void Service(uint32_t nowMs);
	void Stop(void);

	bool IsInSession(void) const { return m_client.IsInSession(); }
	const char *Code(void) const { return m_client.Code(); }
	// True once after each new session code (for --code-file).
	bool TakeSessionChanged(void);
	int NumPlayers(void) const;	// people, not the server
	uint8_t Hour(void) const;
	uint8_t Minute(void) const;
	const CRaceHost &Race(void) const { return m_race; }
	const CMatchHost &Match(void) const { return m_match; }
	const CSeatTable &Seats(void) const { return m_seats; }
	CNetClient &Client(void) { return m_client; }
};
