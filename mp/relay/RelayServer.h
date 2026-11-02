#pragma once

// Invite-code relay. Groups ENet connections into sessions keyed by invite
// code, assigns player ids (host = 0) and forwards packets by route. It has no
// game logic: the host's game is the authority.

#include <stdint.h>
#include <vector>

#include "MpTypes.h"

struct _ENetHost;
struct _ENetPeer;
struct _ENetPacket;
class CByteWriter;

struct SRelayConfig
{
	uint16_t port = MP_DEFAULT_RELAY_PORT;
	uint32_t maxSessions = 256;
	uint32_t maxPacketsPerSecond = 200;
	const char *bindAddress = nullptr;	// nullptr = all interfaces
	uint32_t maxPeersPerAddress = 16;	// connections from one IP (households share one)
	// Reliable commands queued for one peer before it counts as not keeping
	// up: over half, its unreliable snapshots are dropped; over all of it,
	// it's disconnected (so a stalled client can't grow the relay's memory).
	uint32_t maxQueuedPerPeer = 2048;
	// Joins with an unknown code allowed per IP per minute (stops guessing).
	uint32_t maxFailedJoinsPerMinute = 10;
};

class CRelayServer
{
	enum ePeerState
	{
		PEER_FREE,		// no connection in this slot
		PEER_HELLO,		// connected, waiting for create/join
		PEER_IN_SESSION,
		PEER_CLOSING		// told to go away; ignore its packets
	};

	struct SRelayPeer
	{
		uint8_t state;
		uint8_t playerId;
		int32_t session;	// index into m_sessions or -1
		uint32_t connectedMs;
		uint32_t windowStartMs;
		uint32_t windowPackets;
		uint32_t lastWarnMs;
		bool warned;
		bool congested;		// too much queued: unreliable traffic skipped
		char addr[48];
	};

	struct SJoinFailures
	{
		uint32_t host;		// IPv4 address
		uint32_t windowStartMs;
		uint32_t count;
	};

	struct SRelaySession
	{
		bool used;
		char code[MP_INVITE_CODE_LEN + 1];
		_ENetPeer *members[MP_MAX_PLAYERS];
		char names[MP_MAX_PLAYERS][MP_MAX_NAME_LEN + 1];
		uint32_t numMembers;
		uint8_t flags;		// MP_SESSION_*, from the creator
	};

	_ENetHost *m_host;
	SRelayConfig m_config;
	std::vector<SRelayPeer> m_peers;
	std::vector<SRelaySession> m_sessions;
	uint32_t m_numSessions;
	uint32_t m_numPeers;
	uint32_t m_codeCounter;
	uint32_t m_lastHousekeepingMs;
	bool m_netAcquired;
	SJoinFailures m_joinFailures[64];

	SJoinFailures *JoinFailuresFor(uint32_t host, uint32_t now);

	SRelayPeer &PeerData(_ENetPeer *peer);
	void OnConnect(_ENetPeer *peer);
	void OnReceive(_ENetPeer *peer, uint8_t channel, _ENetPacket *packet);
	void OnDisconnect(_ENetPeer *peer, uint32_t data);
	void HandleCreate(_ENetPeer *peer, const uint8_t *body, uint32_t size);
	void HandleJoin(_ENetPeer *peer, const uint8_t *body, uint32_t size);
	void Forward(_ENetPeer *peer, uint8_t channel, _ENetPacket *packet, uint8_t route, uint8_t target);
	void RejectJoin(_ENetPeer *peer, uint8_t result);
	void EndSession(int32_t index, uint8_t reason);
	void SendFromRelay(_ENetPeer *peer, const CByteWriter &msg);
	void MakeUniqueName(const SRelaySession &s, const char *name, char out[MP_MAX_NAME_LEN + 1]) const;
	int32_t FindSession(const char *code) const;
	void Warn(_ENetPeer *peer, const char *what);
	void Housekeeping(uint32_t now);
public:
	CRelayServer(void);
	~CRelayServer(void);

	bool Start(const SRelayConfig &config);
	// Pumps the network. Waits up to timeoutMs for the first event.
	void Service(uint32_t timeoutMs);
	// Sends MSG_SESSION_ENDED{END_RELAY_SHUTDOWN} to everyone and closes.
	void Stop(void);
	bool IsRunning(void) const { return m_host != nullptr; }

	uint16_t Port(void) const;
	uint32_t NumSessions(void) const { return m_numSessions; }
	uint32_t NumPeers(void) const { return m_numPeers; }
};

// One timestamped line to stdout.
void MpRelayLog(const char *fmt, ...);
