#include "RelayServer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <enet/enet.h>

#include "ByteStream.h"
#include "InviteCode.h"
#include "NetInit.h"
#include "Protocol.h"

// A connection that hasn't sent create/join after this long is dropped.
static const uint32_t HELLO_TIMEOUT_MS = 10000;
// At most one warning per peer per this many ms.
static const uint32_t WARN_INTERVAL_MS = 10000;
// How long Stop() keeps servicing so the goodbye messages get delivered.
static const uint32_t STOP_DRAIN_MS = 250;
// ENet-level size limits. Slightly above MP_MAX_PACKET so oversized packets
// still reach OnReceive and get logged; anything bigger is dropped by ENet.
static const uint32_t MP_RELAY_MAX_ENET_PACKET = 2048;
static const uint32_t MP_RELAY_MAX_WAITING_DATA = 64 * 1024;
// At most this many events per Service() call, so a flood can't starve the
// caller's loop (housekeeping, Ctrl+C).
static const uint32_t MAX_EVENTS_PER_SERVICE = 4096;

void
MpRelayLog(const char *fmt, ...)
{
	char stamp[32];
	time_t t = time(nullptr);
	struct tm *tm = localtime(&t);
	if(tm == nullptr || strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm) == 0)
		strcpy(stamp, "?");
	printf("[%s] ", stamp);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static bool
NamesEqual(const char *a, const char *b)
{
	for(; *a && *b; a++, b++){
		char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
		char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
		if(ca != cb)
			return false;
	}
	return *a == *b;
}

CRelayServer::CRelayServer(void)
	: m_host(nullptr), m_numSessions(0), m_numPeers(0), m_codeCounter(0), m_lastHousekeepingMs(0), m_netAcquired(false)
{
}

CRelayServer::~CRelayServer(void)
{
	Stop();
}

CRelayServer::SRelayPeer&
CRelayServer::PeerData(ENetPeer *peer)
{
	return m_peers[peer - m_host->peers];
}

bool
CRelayServer::Start(const SRelayConfig &config)
{
	if(m_host)
		return false;
	if(!MpNetAcquire())
		return false;
	m_netAcquired = true;
	m_config = config;
	if(m_config.maxSessions == 0)
		m_config.maxSessions = 1;

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = m_config.port;
	if(m_config.bindAddress && m_config.bindAddress[0] &&
	   enet_address_set_host_ip(&addr, m_config.bindAddress) != 0 &&
	   enet_address_set_host(&addr, m_config.bindAddress) != 0){
		MpRelayLog("error: bad bind address '%s'", m_config.bindAddress);
		MpNetRelease();
		m_netAcquired = false;
		return false;
	}

	// Every session member plus some slack for connections still saying hello.
	uint32_t peers = m_config.maxSessions * MP_MAX_PLAYERS + 64;
	if(peers > ENET_PROTOCOL_MAXIMUM_PEER_ID)
		peers = ENET_PROTOCOL_MAXIMUM_PEER_ID;
	m_host = enet_host_create(&addr, peers, MP_NUM_CHANNELS, 0, 0);
	if(m_host == nullptr){
		MpNetRelease();
		m_netAcquired = false;
		return false;
	}

	// Don't let ENet buffer more than we'd ever accept: by default it would
	// reassemble packets up to 32 MB per peer. Also cap connections per IP.
	m_host->maximumPacketSize = MP_RELAY_MAX_ENET_PACKET;
	m_host->maximumWaitingData = MP_RELAY_MAX_WAITING_DATA;
	m_host->duplicatePeers = m_config.maxPeersPerAddress ? m_config.maxPeersPerAddress : 1;

	m_peers.assign(m_host->peerCount, SRelayPeer());
	for(size_t i = 0; i < m_peers.size(); i++){
		memset(&m_peers[i], 0, sizeof(SRelayPeer));
		m_peers[i].state = PEER_FREE;
		m_peers[i].session = -1;
	}
	m_sessions.assign(m_config.maxSessions, SRelaySession());
	for(size_t i = 0; i < m_sessions.size(); i++)
		memset(&m_sessions[i], 0, sizeof(SRelaySession));
	m_numSessions = 0;
	m_numPeers = 0;
	m_lastHousekeepingMs = enet_time_get();
	memset(m_joinFailures, 0, sizeof(m_joinFailures));
	return true;
}

// The failure counter for an address, reset each minute. A new address takes
// a free or expired slot, else the one with the oldest window.
CRelayServer::SJoinFailures*
CRelayServer::JoinFailuresFor(uint32_t host, uint32_t now)
{
	const size_t n = sizeof(m_joinFailures) / sizeof(m_joinFailures[0]);
	SJoinFailures *slot = nullptr;
	for(size_t i = 0; i < n && slot == nullptr; i++)
		if(m_joinFailures[i].count > 0 && m_joinFailures[i].host == host)
			slot = &m_joinFailures[i];
	if(slot == nullptr){
		for(size_t i = 0; i < n; i++){
			SJoinFailures &f = m_joinFailures[i];
			if(f.count == 0 || now - f.windowStartMs >= 60000){
				slot = &f;
				break;
			}
			if(slot == nullptr || (int32_t)(f.windowStartMs - slot->windowStartMs) < 0)
				slot = &f;
		}
		slot->host = host;
		slot->windowStartMs = now;
		slot->count = 0;
	}
	if(now - slot->windowStartMs >= 60000){
		slot->windowStartMs = now;
		slot->count = 0;
	}
	return slot;
}

uint16_t
CRelayServer::Port(void) const
{
	return m_host ? m_host->address.port : 0;
}

void
CRelayServer::Service(uint32_t timeoutMs)
{
	if(m_host == nullptr)
		return;
	ENetEvent ev;
	uint32_t wait = timeoutMs;
	uint32_t events = 0;
	while(m_host && events++ < MAX_EVENTS_PER_SERVICE && enet_host_service(m_host, &ev, wait) > 0){
		wait = 0;
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:
			OnConnect(ev.peer);
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			OnReceive(ev.peer, ev.channelID, ev.packet);
			if(ev.packet->referenceCount == 0)
				enet_packet_destroy(ev.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			OnDisconnect(ev.peer, ev.data);
			break;
		default:
			break;
		}
	}
	if(m_host)
		Housekeeping(enet_time_get());
}

void
CRelayServer::Stop(void)
{
	if(m_host){
		SMsgSessionEnded end;
		end.reason = END_RELAY_SHUTDOWN;
		CByteWriter w;
		end.Write(w);
		for(size_t i = 0; i < m_sessions.size(); i++){
			SRelaySession &s = m_sessions[i];
			if(!s.used)
				continue;
			for(uint32_t id = 0; id < MP_MAX_PLAYERS; id++)
				if(s.members[id])
					SendFromRelay(s.members[id], w);
			MpRelayLog("end %s: relay shutdown", s.code);
		}
		for(size_t i = 0; i < m_host->peerCount; i++){
			ENetPeer *peer = &m_host->peers[i];
			if(peer->state == ENET_PEER_STATE_CONNECTED)
				enet_peer_disconnect_later(peer, 0);
		}

		// Give the goodbyes a moment to be delivered and acknowledged.
		uint32_t start = enet_time_get();
		ENetEvent ev;
		while(enet_time_get() - start < STOP_DRAIN_MS){
			int r = enet_host_service(m_host, &ev, 10);
			if(r < 0)
				break;
			if(r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE)
				enet_packet_destroy(ev.packet);
		}
		// Peers that acknowledged in time were disconnected gracefully above.
		// Don't force a disconnect on the rest: ENet drops a peer's
		// undelivered packets when a disconnect arrives, which would eat the
		// SESSION_ENDED that is already on the wire. They leave on receiving
		// it, or time out if it was lost.
		enet_host_destroy(m_host);
		m_host = nullptr;
		m_peers.clear();
		m_sessions.clear();
		m_numSessions = 0;
		m_numPeers = 0;
	}
	if(m_netAcquired){
		MpNetRelease();
		m_netAcquired = false;
	}
}

void
CRelayServer::OnConnect(ENetPeer *peer)
{
	SRelayPeer &p = PeerData(peer);
	memset(&p, 0, sizeof(p));
	p.state = PEER_HELLO;
	p.session = -1;
	p.playerId = MP_PLAYER_NONE;
	p.connectedMs = enet_time_get();
	p.windowStartMs = p.connectedMs;
	if(enet_address_get_host_ip(&peer->address, p.addr, sizeof(p.addr)) != 0)
		strcpy(p.addr, "?");
	size_t len = strlen(p.addr);
	snprintf(p.addr + len, sizeof(p.addr) - len, ":%u", (unsigned)peer->address.port);
	// A crashed host is noticed within MP_PEER_TIMEOUT_MAX_MS.
	enet_peer_timeout(peer, 0, MP_PEER_TIMEOUT_MIN_MS, MP_PEER_TIMEOUT_MAX_MS);
	m_numPeers++;
}

void
CRelayServer::OnDisconnect(ENetPeer *peer, uint32_t data)
{
	SRelayPeer &p = PeerData(peer);
	if(p.state == PEER_FREE)
		return;
	if(p.state == PEER_IN_SESSION && p.session >= 0){
		int32_t index = p.session;
		SRelaySession &s = m_sessions[index];
		if(p.playerId == 0){
			MpRelayLog("leave %s: id=0 '%s' (host) %s", s.code, s.names[0], data == MP_DISCONNECT_QUIT ? "quit" : "lost");
			s.members[0] = nullptr;
			s.numMembers--;
			p.state = PEER_FREE;
			p.session = -1;
			EndSession(index, END_HOST_LEFT);
		}else{
			uint8_t id = p.playerId;
			MpRelayLog("leave %s: id=%u '%s' %s", s.code, (unsigned)id, s.names[id], data == MP_DISCONNECT_QUIT ? "quit" : "lost");
			s.members[id] = nullptr;
			s.names[id][0] = '\0';
			s.numMembers--;
			SMsgPlayerLeft left;
			left.playerId = id;
			left.reason = data == MP_DISCONNECT_QUIT ? LEAVE_QUIT : LEAVE_LOST;
			CByteWriter w;
			left.Write(w);
			for(uint32_t i = 0; i < MP_MAX_PLAYERS; i++)
				if(s.members[i])
					SendFromRelay(s.members[i], w);
		}
	}
	memset(&p, 0, sizeof(p));
	p.state = PEER_FREE;
	p.session = -1;
	if(m_numPeers > 0)
		m_numPeers--;
}

void
CRelayServer::Warn(ENetPeer *peer, const char *what)
{
	SRelayPeer &p = PeerData(peer);
	uint32_t now = enet_time_get();
	if(p.warned && now - p.lastWarnMs < WARN_INTERVAL_MS)
		return;
	p.warned = true;
	p.lastWarnMs = now;
	MpRelayLog("warning: %s from %s", what, p.addr);
}

void
CRelayServer::OnReceive(ENetPeer *peer, uint8_t channel, ENetPacket *packet)
{
	SRelayPeer &p = PeerData(peer);
	if(p.state == PEER_FREE || p.state == PEER_CLOSING)
		return;

	uint32_t now = enet_time_get();
	if(now - p.windowStartMs >= 1000){
		p.windowStartMs = now;
		p.windowPackets = 0;
	}
	const char *problem = nullptr;
	if(++p.windowPackets > m_config.maxPacketsPerSecond)
		problem = "packet rate limit exceeded";
	else if(packet->dataLength > MP_MAX_PACKET)
		problem = "oversized packet";
	if(problem){
		Warn(peer, problem);
		// Before create/join, anything unexpected closes the connection.
		if(p.state == PEER_HELLO){
			p.state = PEER_CLOSING;
			enet_peer_disconnect(peer, 0);
		}
		return;
	}

	CByteReader r(packet->data, (uint32_t)packet->dataLength);
	SEnvelope env;
	uint8_t type;
	if(!env.Read(r) || !r.ReadU8(type)){
		Warn(peer, "malformed envelope");
		if(p.state == PEER_HELLO){
			p.state = PEER_CLOSING;
			enet_peer_disconnect(peer, 0);
		}
		return;
	}

	if(p.state == PEER_HELLO){
		if(env.route == MP_ROUTE_RELAY && type == MSG_CREATE_SESSION)
			HandleCreate(peer, r.Current(), r.Remaining());
		else if(env.route == MP_ROUTE_RELAY && type == MSG_JOIN_SESSION)
			HandleJoin(peer, r.Current(), r.Remaining());
		else{
			Warn(peer, "first message was not create/join");
			p.state = PEER_CLOSING;
			enet_peer_disconnect(peer, 0);
		}
		return;
	}

	// In a session. Relay-control types only ever come from the relay itself,
	// so members can't spoof joins, leaves or session ends.
	if(env.route == MP_ROUTE_RELAY || type < MSG_CHAT){
		Warn(peer, "unexpected relay message");
		return;
	}
	Forward(peer, channel, packet, env.route, env.player);
}

void
CRelayServer::Forward(ENetPeer *peer, uint8_t channel, ENetPacket *packet, uint8_t route, uint8_t target)
{
	SRelayPeer &p = PeerData(peer);
	SRelaySession &s = m_sessions[p.session];
	ENetPeer *dest[MP_MAX_PLAYERS];
	uint32_t numDest = 0;

	switch(route){
	case MP_ROUTE_HOST:
		if(s.members[0])
			dest[numDest++] = s.members[0];
		break;
	case MP_ROUTE_ALL:
	case MP_ROUTE_ALL_BUT_SELF:
		for(uint32_t i = 0; i < MP_MAX_PLAYERS; i++)
			if(s.members[i] && (route == MP_ROUTE_ALL || i != p.playerId))
				dest[numDest++] = s.members[i];
		break;
	case MP_ROUTE_PLAYER:
		if(target < MP_MAX_PLAYERS && s.members[target])
			dest[numDest++] = s.members[target];
		break;
	default:
		Warn(peer, "unknown route");
		return;
	}
	if(numDest == 0)
		return;

	// Same payload; the downlink envelope carries the sender's id.
	uint32_t flags = packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED);
	bool reliable = (flags & ENET_PACKET_FLAG_RELIABLE) != 0;
	ENetPacket *out = enet_packet_create(packet->data, packet->dataLength, flags);
	if(out == nullptr)
		return;
	out->data[1] = p.playerId;
	for(uint32_t i = 0; i < numDest; i++){
		// A peer that isn't keeping up skips snapshots; they'd be stale anyway.
		if(!reliable && PeerData(dest[i]).congested)
			continue;
		enet_peer_send(dest[i], channel, out);
	}
	if(out->referenceCount == 0)
		enet_packet_destroy(out);
}

void
CRelayServer::SendFromRelay(ENetPeer *peer, const CByteWriter &msg)
{
	CByteWriter w;
	SEnvelope env;
	env.route = MP_ROUTE_RELAY;
	env.player = MP_PLAYER_RELAY;
	env.Write(w);
	w.WriteBytes(msg.Data(), msg.Size());
	if(w.Overflowed())
		return;
	ENetPacket *packet = enet_packet_create(w.Data(), w.Size(), ENET_PACKET_FLAG_RELIABLE);
	if(packet && enet_peer_send(peer, MP_CHANNEL_RELIABLE, packet) != 0)
		enet_packet_destroy(packet);
}

void
CRelayServer::RejectJoin(ENetPeer *peer, uint8_t result)
{
	SMsgJoinResult res;
	res.result = result;
	res.playerId = MP_PLAYER_NONE;
	CByteWriter w;
	res.Write(w);
	SendFromRelay(peer, w);
	// Deliver the result first, then close.
	PeerData(peer).state = PEER_CLOSING;
	enet_peer_disconnect_later(peer, 0);
}

int32_t
CRelayServer::FindSession(const char *code) const
{
	for(size_t i = 0; i < m_sessions.size(); i++)
		if(m_sessions[i].used && strcmp(m_sessions[i].code, code) == 0)
			return (int32_t)i;
	return -1;
}

void
CRelayServer::HandleCreate(ENetPeer *peer, const uint8_t *body, uint32_t size)
{
	SRelayPeer &p = PeerData(peer);
	CByteReader r(body, size);
	SMsgCreateSession msg;
	// Check the version before the rest, so a future layout still gets a clean answer.
	if(!r.ReadU16(msg.version)){
		Warn(peer, "malformed create");
		p.state = PEER_CLOSING;
		enet_peer_disconnect(peer, 0);
		return;
	}
	if(msg.version != MP_PROTOCOL_VERSION){
		RejectJoin(peer, JOIN_VERSION_MISMATCH);
		return;
	}
	char name[MP_MAX_NAME_LEN + 1];
	if(!r.ReadString(msg.name, sizeof(msg.name)) || !MpSanitizeName(msg.name, name)){
		RejectJoin(peer, JOIN_BAD_NAME);
		return;
	}
	if(!r.ReadU8(msg.flags)){
		Warn(peer, "malformed create");
		p.state = PEER_CLOSING;
		enet_peer_disconnect(peer, 0);
		return;
	}

	int32_t index = -1;
	for(size_t i = 0; i < m_sessions.size(); i++)
		if(!m_sessions[i].used){
			index = (int32_t)i;
			break;
		}
	if(index < 0){
		MpRelayLog("create refused for %s: relay full (%u sessions)", p.addr, (unsigned)m_numSessions);
		RejectJoin(peer, JOIN_FULL);
		return;
	}

	char code[MP_INVITE_CODE_LEN + 1];
	bool unique = false;
	for(int attempt = 0; attempt < 64 && !unique; attempt++){
		uint32_t seed = enet_time_get() ^ (++m_codeCounter * 0x9E3779B9u) ^ peer->address.host ^ ((uint32_t)peer->address.port << 16);
		MpMakeInviteCode(seed, code);
		unique = FindSession(code) < 0;
	}
	if(!unique){
		RejectJoin(peer, JOIN_FULL);
		return;
	}

	SRelaySession &s = m_sessions[index];
	memset(&s, 0, sizeof(s));
	s.used = true;
	strcpy(s.code, code);
	s.members[0] = peer;
	strcpy(s.names[0], name);
	s.numMembers = 1;
	s.flags = msg.flags & MP_SESSION_DEDICATED;	// unknown bits are dropped
	m_numSessions++;

	p.state = PEER_IN_SESSION;
	p.session = index;
	p.playerId = 0;

	SMsgSessionCreated created;
	strcpy(created.code, code);
	created.playerId = 0;
	CByteWriter w;
	created.Write(w);
	SendFromRelay(peer, w);
	MpRelayLog("create %s: %s '%s' from %s (%u sessions)", code, (s.flags & MP_SESSION_DEDICATED) ? "server" : "host", name, p.addr, (unsigned)m_numSessions);
}

void
CRelayServer::MakeUniqueName(const SRelaySession &s, const char *name, char out[MP_MAX_NAME_LEN + 1]) const
{
	strcpy(out, name);
	for(uint32_t n = 2; ; n++){
		bool taken = false;
		for(uint32_t i = 0; i < MP_MAX_PLAYERS && !taken; i++)
			if(s.members[i] && NamesEqual(s.names[i], out))
				taken = true;
		if(!taken)
			return;
		// "name (n)", cutting the base so the whole thing fits.
		char suffix[8];
		snprintf(suffix, sizeof(suffix), " (%u)", (unsigned)n);
		size_t keep = strlen(name);
		if(keep + strlen(suffix) > MP_MAX_NAME_LEN)
			keep = MP_MAX_NAME_LEN - strlen(suffix);
		memcpy(out, name, keep);
		strcpy(out + keep, suffix);
	}
}

void
CRelayServer::HandleJoin(ENetPeer *peer, const uint8_t *body, uint32_t size)
{
	SRelayPeer &p = PeerData(peer);
	CByteReader r(body, size);
	SMsgJoinSession msg;
	if(!r.ReadU16(msg.version)){
		Warn(peer, "malformed join");
		p.state = PEER_CLOSING;
		enet_peer_disconnect(peer, 0);
		return;
	}
	if(msg.version != MP_PROTOCOL_VERSION){
		RejectJoin(peer, JOIN_VERSION_MISMATCH);
		return;
	}
	SJoinFailures *fails = JoinFailuresFor(peer->address.host, enet_time_get());
	if(fails->count >= m_config.maxFailedJoinsPerMinute){
		// Too many wrong codes from this address: refuse without looking.
		Warn(peer, "too many failed joins");
		RejectJoin(peer, JOIN_NOT_FOUND);
		return;
	}
	char code[MP_INVITE_CODE_LEN + 1];
	int32_t index = -1;
	if(r.ReadString(msg.code, sizeof(msg.code)) && MpNormalizeInviteCode(msg.code, code))
		index = FindSession(code);
	if(index < 0){
		fails->count++;
		RejectJoin(peer, JOIN_NOT_FOUND);
		return;
	}
	char clean[MP_MAX_NAME_LEN + 1];
	if(!r.ReadString(msg.name, sizeof(msg.name)) || !MpSanitizeName(msg.name, clean)){
		RejectJoin(peer, JOIN_BAD_NAME);
		return;
	}
	SRelaySession &s = m_sessions[index];
	if(s.numMembers >= MP_MAX_PLAYERS){
		RejectJoin(peer, JOIN_FULL);
		return;
	}
	uint8_t id = MP_PLAYER_NONE;
	for(uint32_t i = 1; i < MP_MAX_PLAYERS; i++)
		if(s.members[i] == nullptr){
			id = (uint8_t)i;
			break;
		}
	if(id == MP_PLAYER_NONE){
		RejectJoin(peer, JOIN_FULL);
		return;
	}

	char name[MP_MAX_NAME_LEN + 1];
	MakeUniqueName(s, clean, name);

	SMsgJoinResult res;
	res.result = JOIN_OK;
	res.playerId = id;
	res.flags = s.flags;
	CByteWriter w;
	res.Write(w);
	SendFromRelay(peer, w);

	// Tell the joiner who's already here, then tell everyone about the joiner.
	for(uint32_t i = 0; i < MP_MAX_PLAYERS; i++){
		if(s.members[i] == nullptr)
			continue;
		SMsgPlayerJoined other;
		other.playerId = (uint8_t)i;
		strcpy(other.name, s.names[i]);
		w.Clear();
		other.Write(w);
		SendFromRelay(peer, w);
	}
	SMsgPlayerJoined joined;
	joined.playerId = id;
	strcpy(joined.name, name);
	w.Clear();
	joined.Write(w);
	for(uint32_t i = 0; i < MP_MAX_PLAYERS; i++)
		if(s.members[i])
			SendFromRelay(s.members[i], w);

	s.members[id] = peer;
	strcpy(s.names[id], name);
	s.numMembers++;
	p.state = PEER_IN_SESSION;
	p.session = index;
	p.playerId = id;
	MpRelayLog("join %s: id=%u '%s' from %s (%u/%u players)", s.code, (unsigned)id, name, p.addr,
		(unsigned)s.numMembers, (unsigned)MP_MAX_PLAYERS);
}

void
CRelayServer::EndSession(int32_t index, uint8_t reason)
{
	SRelaySession &s = m_sessions[index];
	if(!s.used)
		return;
	SMsgSessionEnded end;
	end.reason = reason;
	CByteWriter w;
	end.Write(w);
	for(uint32_t i = 0; i < MP_MAX_PLAYERS; i++){
		ENetPeer *member = s.members[i];
		if(member == nullptr)
			continue;
		SendFromRelay(member, w);
		SRelayPeer &mp = PeerData(member);
		mp.state = PEER_CLOSING;
		mp.session = -1;
		mp.playerId = MP_PLAYER_NONE;
		enet_peer_disconnect_later(member, 0);
	}
	MpRelayLog("end %s: %s", s.code, reason == END_HOST_LEFT ? "host left" : "relay shutdown");
	memset(&s, 0, sizeof(s));
	if(m_numSessions > 0)
		m_numSessions--;
}

void
CRelayServer::Housekeeping(uint32_t now)
{
	if(now - m_lastHousekeepingMs < 1000)
		return;
	m_lastHousekeepingMs = now;
	for(size_t i = 0; i < m_host->peerCount; i++){
		SRelayPeer &p = m_peers[i];
		ENetPeer *peer = &m_host->peers[i];
		if(p.state == PEER_HELLO && now - p.connectedMs >= HELLO_TIMEOUT_MS){
			Warn(peer, "no create/join in time");
			p.state = PEER_CLOSING;
			enet_peer_disconnect(peer, 0);
			continue;
		}
		if(p.state != PEER_IN_SESSION || peer->state != ENET_PEER_STATE_CONNECTED)
			continue;
		size_t queued = enet_list_size(&peer->outgoingCommands) + enet_list_size(&peer->sentReliableCommands);
		p.congested = queued > m_config.maxQueuedPerPeer / 2;
		if(queued > m_config.maxQueuedPerPeer){
			// Not acknowledging: drop it now instead of buffering for its
			// whole ENet timeout. OnDisconnect tells the session.
			MpRelayLog("dropping %s: %u commands queued", p.addr, (unsigned)queued);
			OnDisconnect(peer, 0);
			enet_peer_disconnect_now(peer, 0);
		}
	}
}
