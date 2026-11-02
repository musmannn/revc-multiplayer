#pragma once

// Client side of a multiplayer session: one ENet connection to the relay,
// plus the session state machine (host/join, roster, chat, clock sync).
// The game (Plan 2), the fake client and the tests all use this.
//
// Single-threaded: call Service() regularly (every frame) from one thread.
// Callbacks run from inside Service().

#include <stdint.h>

#include "MpTypes.h"
#include "ByteStream.h"
#include "Protocol.h"
#include "SessionClock.h"

struct _ENetHost;
struct _ENetPeer;

class INetClientListener
{
public:
	virtual ~INetClientListener(void) {}
	virtual void OnSessionReady(const char *code, uint8_t myId, bool isHost) { (void)code; (void)myId; (void)isHost; }
	virtual void OnJoinFailed(eJoinResult result) { (void)result; }
	virtual void OnPlayerJoined(uint8_t id, const char *name) { (void)id; (void)name; }
	virtual void OnPlayerLeft(uint8_t id) { (void)id; }
	virtual void OnSessionEnded(eEndReason reason) { (void)reason; }
	virtual void OnConnectionLost(void) {}
	virtual void OnChat(uint8_t from, const char *text) { (void)from; (void)text; }
	// type >= MSG_GAME_FIRST; body is positioned just after the type byte.
	virtual void OnGameMessage(uint8_t from, uint8_t type, CByteReader &body) { (void)from; (void)type; (void)body; }
};

class CNetClient
{
public:
	enum { CONNECT_TIMEOUT_MS = 5000, PING_INTERVAL_MS = 1000 };
private:
	enum eState
	{
		STATE_IDLE,
		STATE_CONNECTING,	// ENet handshake with the relay
		STATE_WAITING,		// connected, create/join sent, waiting for the answer
		STATE_IN_SESSION
	};

	_ENetHost *m_host;
	_ENetPeer *m_peer;
	eState m_state;
	bool m_wantHost;
	bool m_isHost;
	bool m_haveStartTime;
	bool m_netAcquired;
	uint8_t m_myId;
	uint8_t m_hostFlags;	// MP_SESSION_* asked for by Host()
	uint8_t m_sessionFlags;	// MP_SESSION_* of the current/last session
	uint32_t m_startMs;
	uint32_t m_lastPingMs;
	uint32_t m_generation;	// bumped per connection; detects re-entrant Host()/Join()
	char m_code[MP_INVITE_CODE_LEN + 1];
	char m_joinCode[MP_INVITE_CODE_LEN + 1];
	char m_name[MP_MAX_NAME_LEN + 1];
	CSessionClock m_clock;

	bool Connect(const char *relayAddr, uint16_t port);
	void Close(bool sendGoodbye);
	bool SendRaw(uint8_t route, uint8_t target, bool reliable, const CByteWriter &msg);
	void HandlePacket(uint32_t nowMs, const uint8_t *data, uint32_t size, INetClientListener *l);
	void HandleRelayMessage(uint8_t type, CByteReader &r, INetClientListener *l);
	void Fail(INetClientListener *l);

	CNetClient(const CNetClient&);
	CNetClient &operator=(const CNetClient&);
public:
	CNetClient(void);
	~CNetClient(void);

	// Start an async connect. False only on an immediate error (already
	// active, ENet init failure, unresolvable address).
	// flags: MP_SESSION_* (MP_SESSION_DEDICATED for revc-server).
	bool Host(const char *relayAddr, uint16_t port, const char *name, uint8_t flags = 0);
	bool Join(const char *relayAddr, uint16_t port, const char *code, const char *name);
	// Pumps ENet and dispatches callbacks. nowMs is the caller's millisecond
	// clock; the host's nowMs is the session time.
	void Service(uint32_t nowMs, INetClientListener *l);
	// Leaves at once. No callbacks fire. Host()/Join() may be called again
	// right after, including from inside a callback.
	void Leave(void);

	// msg starts with a type >= MSG_GAME_FIRST and is at most MP_MAX_PACKET-2
	// bytes. Reliable goes on MP_CHANNEL_RELIABLE; unreliable on
	// MP_CHANNEL_SNAPSHOT (sequenced). target is only used for MP_ROUTE_PLAYER.
	bool SendGame(uint8_t route, uint8_t target, bool reliable, const CByteWriter &msg);
	// Route ALL: the sender gets its own message back too. That echo is the
	// single source of on-screen chat.
	bool SendChat(const char *text);

	bool IsActive(void) const { return m_state != STATE_IDLE; }
	bool IsInSession(void) const { return m_state == STATE_IN_SESSION; }
	bool IsHost(void) const { return m_state == STATE_IN_SESSION && m_isHost; }
	// MyId() and Code() keep the last session's values after it ends.
	uint8_t MyId(void) const { return m_myId; }
	const char *Code(void) const { return m_code; }
	uint8_t SessionFlags(void) const { return m_sessionFlags; }
	// Hosted by revc-server: id 0 is the server, not a player.
	bool IsDedicatedSession(void) const { return (m_sessionFlags & MP_SESSION_DEDICATED) != 0; }
	const CSessionClock &Clock(void) const { return m_clock; }
	uint32_t SessionTime(uint32_t nowMs) const { return m_clock.ToSession(nowMs); }
};
