#include "NetClient.h"

#include <string.h>

#include <enet/enet.h>

#include "InviteCode.h"
#include "NetInit.h"

// Keep printable ASCII only, so received text is safe to draw or print.
// '~' is dropped too: the game's font treats it as the start of a token.
static void
FilterText(char *s)
{
	char *out = s;
	for(; *s; s++)
		if((uint8_t)*s >= 0x20 && (uint8_t)*s <= 0x7E && *s != '~')
			*out++ = *s;
	*out = '\0';
}

static void
CopyString(char *dst, const char *src, uint32_t dstSize)
{
	uint32_t i = 0;
	if(src)
		for(; i + 1 < dstSize && src[i]; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

CNetClient::CNetClient(void)
	: m_host(nullptr), m_peer(nullptr), m_state(STATE_IDLE), m_wantHost(false), m_isHost(false),
	  m_haveStartTime(false), m_netAcquired(false), m_myId(MP_PLAYER_NONE), m_hostFlags(0), m_sessionFlags(0), m_startMs(0), m_lastPingMs(0), m_generation(0)
{
	m_code[0] = '\0';
	m_joinCode[0] = '\0';
	m_name[0] = '\0';
}

CNetClient::~CNetClient(void)
{
	Leave();
}

bool
CNetClient::Connect(const char *relayAddr, uint16_t port)
{
	if(m_state != STATE_IDLE || relayAddr == nullptr)
		return false;
	if(!MpNetAcquire())
		return false;
	m_netAcquired = true;

	ENetAddress addr;
	addr.port = port;
	if(enet_address_set_host(&addr, relayAddr) != 0){
		Close(false);
		return false;
	}
	m_host = enet_host_create(nullptr, 1, MP_NUM_CHANNELS, 0, 0);
	if(m_host == nullptr){
		Close(false);
		return false;
	}
	m_host->maximumPacketSize = 2048;
	m_host->maximumWaitingData = 256 * 1024;
	m_peer = enet_host_connect(m_host, &addr, MP_NUM_CHANNELS, 0);
	if(m_peer == nullptr){
		Close(false);
		return false;
	}
	// Same timeouts as the relay uses for clients.
	enet_peer_timeout(m_peer, 0, MP_PEER_TIMEOUT_MIN_MS, MP_PEER_TIMEOUT_MAX_MS);
	m_state = STATE_CONNECTING;
	m_generation++;
	m_isHost = false;
	m_myId = MP_PLAYER_NONE;
	m_code[0] = '\0';
	m_haveStartTime = false;
	m_clock.Reset();
	return true;
}

bool
CNetClient::Host(const char *relayAddr, uint16_t port, const char *name, uint8_t flags)
{
	if(m_state != STATE_IDLE)
		return false;
	m_wantHost = true;
	m_hostFlags = flags & MP_SESSION_DEDICATED;	// the relay drops unknown bits too
	CopyString(m_name, name, sizeof(m_name));
	return Connect(relayAddr, port);
}

bool
CNetClient::Join(const char *relayAddr, uint16_t port, const char *code, const char *name)
{
	if(m_state != STATE_IDLE)
		return false;
	m_wantHost = false;
	// Normalize here so sloppy input fits the 9-char field; the relay
	// normalizes again and rejects anything invalid with JOIN_NOT_FOUND.
	if(!MpNormalizeInviteCode(code, m_joinCode))
		CopyString(m_joinCode, code, sizeof(m_joinCode));
	CopyString(m_name, name, sizeof(m_name));
	return Connect(relayAddr, port);
}

void
CNetClient::Close(bool sendGoodbye)
{
	if(m_host){
		if(m_peer && sendGoodbye && m_peer->state == ENET_PEER_STATE_CONNECTED)
			enet_peer_disconnect_now(m_peer, MP_DISCONNECT_QUIT);
		enet_host_destroy(m_host);
	}
	m_host = nullptr;
	m_peer = nullptr;
	m_state = STATE_IDLE;
	if(m_netAcquired){
		MpNetRelease();
		m_netAcquired = false;
	}
}

void
CNetClient::Leave(void)
{
	Close(true);
}

void
CNetClient::Fail(INetClientListener *l)
{
	// Never reached the relay: report it like an unknown code. Otherwise the
	// connection broke.
	bool everConnected = m_state != STATE_CONNECTING;
	Close(false);
	if(l == nullptr)
		return;
	if(everConnected)
		l->OnConnectionLost();
	else
		l->OnJoinFailed(JOIN_NOT_FOUND);
}

bool
CNetClient::SendRaw(uint8_t route, uint8_t target, bool reliable, const CByteWriter &msg)
{
	if(m_peer == nullptr || msg.Overflowed() || msg.Size() == 0)
		return false;
	CByteWriter w;
	SEnvelope env;
	env.route = route;
	env.player = target;
	env.Write(w);
	w.WriteBytes(msg.Data(), msg.Size());
	if(w.Overflowed())
		return false;
	ENetPacket *packet = enet_packet_create(w.Data(), w.Size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	if(packet == nullptr)
		return false;
	if(enet_peer_send(m_peer, reliable ? MP_CHANNEL_RELIABLE : MP_CHANNEL_SNAPSHOT, packet) != 0){
		enet_packet_destroy(packet);
		return false;
	}
	return true;
}

bool
CNetClient::SendGame(uint8_t route, uint8_t target, bool reliable, const CByteWriter &msg)
{
	if(m_state != STATE_IN_SESSION || msg.Size() == 0 || msg.Data()[0] < MSG_GAME_FIRST)
		return false;
	if(route == MP_ROUTE_RELAY)
		return false;
	return SendRaw(route, target, reliable, msg);
}

bool
CNetClient::SendChat(const char *text)
{
	if(m_state != STATE_IN_SESSION || text == nullptr || text[0] == '\0')
		return false;
	SMsgChat chat;
	CopyString(chat.text, text, sizeof(chat.text));
	CByteWriter w;
	chat.Write(w);
	return SendRaw(MP_ROUTE_ALL, 0, true, w);
}

void
CNetClient::Service(uint32_t nowMs, INetClientListener *l)
{
	if(m_state == STATE_IDLE)
		return;
	if(!m_haveStartTime){
		m_startMs = nowMs;
		m_haveStartTime = true;
	}

	// A callback may Leave() and then Host()/Join() again. If so, stop: the
	// new connection gets its own start time on the next Service(). (Compare
	// a generation, not the ENetHost pointer: malloc may hand the new host
	// the old one's address.)
	uint32_t generation = m_generation;
	ENetEvent ev;
	for(;;){
		if(m_generation != generation || m_host == nullptr)
			return;
		int r = enet_host_service(m_host, &ev, 0);
		// r < 0 is a socket error (e.g. an ICMP "port unreachable" on
		// Windows); the connect timeout below covers it.
		if(r <= 0)
			break;
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:
			if(m_state == STATE_CONNECTING){
				CByteWriter w;
				if(m_wantHost){
					SMsgCreateSession create;
					strcpy(create.name, m_name);
					create.flags = m_hostFlags;
					create.Write(w);
				}else{
					SMsgJoinSession join;
					strcpy(join.code, m_joinCode);
					strcpy(join.name, m_name);
					join.Write(w);
				}
				SendRaw(MP_ROUTE_RELAY, MP_PLAYER_NONE, true, w);
				m_state = STATE_WAITING;
			}
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			HandlePacket(nowMs, ev.packet->data, (uint32_t)ev.packet->dataLength, l);
			enet_packet_destroy(ev.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			m_peer = nullptr;
			Fail(l);
			break;
		default:
			break;
		}
		if(m_state == STATE_IDLE || m_generation != generation)
			return;
	}

	if((m_state == STATE_CONNECTING || m_state == STATE_WAITING) && m_haveStartTime &&
	   nowMs - m_startMs >= CONNECT_TIMEOUT_MS){
		Fail(l);
		return;
	}

	if(m_state == STATE_IN_SESSION && !m_isHost && nowMs - m_lastPingMs >= PING_INTERVAL_MS){
		SMsgPing ping;
		ping.clientSendMs = nowMs;
		CByteWriter w;
		ping.Write(w);
		SendRaw(MP_ROUTE_HOST, 0, false, w);
		m_lastPingMs = nowMs;
	}
	if(m_host)
		enet_host_flush(m_host);
}

void
CNetClient::HandleRelayMessage(uint8_t type, CByteReader &r, INetClientListener *l)
{
	switch(type){
	case MSG_SESSION_CREATED: {
		SMsgSessionCreated msg;
		if(m_state != STATE_WAITING || !m_wantHost || !msg.Read(r))
			return;
		m_state = STATE_IN_SESSION;
		m_isHost = true;
		m_myId = msg.playerId;
		m_sessionFlags = m_hostFlags;
		CopyString(m_code, msg.code, sizeof(m_code));
		m_clock.SetIsHost();
		if(l)
			l->OnSessionReady(m_code, m_myId, true);
		break;
	}
	case MSG_JOIN_RESULT: {
		SMsgJoinResult msg;
		if(m_state != STATE_WAITING || !msg.Read(r))
			return;
		if(msg.result == JOIN_OK && !m_wantHost && msg.playerId < MP_MAX_PLAYERS){
			m_state = STATE_IN_SESSION;
			m_isHost = false;
			m_myId = msg.playerId;
			m_sessionFlags = msg.flags;
			CopyString(m_code, m_joinCode, sizeof(m_code));
			m_lastPingMs = m_startMs - PING_INTERVAL_MS;	// ping right away
			if(l)
				l->OnSessionReady(m_code, m_myId, false);
		}else{
			eJoinResult result = msg.result == JOIN_OK ? JOIN_NOT_FOUND : (eJoinResult)msg.result;
			if(msg.result > JOIN_BAD_NAME)
				result = JOIN_NOT_FOUND;
			Close(true);
			if(l)
				l->OnJoinFailed(result);
		}
		break;
	}
	case MSG_PLAYER_JOINED: {
		SMsgPlayerJoined msg;
		if(m_state != STATE_IN_SESSION || !msg.Read(r) || msg.playerId >= MP_MAX_PLAYERS)
			return;
		FilterText(msg.name);
		if(l)
			l->OnPlayerJoined(msg.playerId, msg.name);
		break;
	}
	case MSG_PLAYER_LEFT: {
		SMsgPlayerLeft msg;
		if(m_state != STATE_IN_SESSION || !msg.Read(r) || msg.playerId >= MP_MAX_PLAYERS)
			return;
		if(l)
			l->OnPlayerLeft(msg.playerId);
		break;
	}
	case MSG_SESSION_ENDED: {
		SMsgSessionEnded msg;
		if(!msg.Read(r))
			return;
		eEndReason reason = msg.reason == END_RELAY_SHUTDOWN ? END_RELAY_SHUTDOWN : END_HOST_LEFT;
		bool wasInSession = m_state == STATE_IN_SESSION;
		Close(true);
		if(l){
			if(wasInSession)
				l->OnSessionEnded(reason);
			else
				l->OnJoinFailed(JOIN_NOT_FOUND);
		}
		break;
	}
	default:
		break;
	}
}

void
CNetClient::HandlePacket(uint32_t nowMs, const uint8_t *data, uint32_t size, INetClientListener *l)
{
	CByteReader r(data, size);
	SEnvelope env;
	uint8_t type;
	if(!env.Read(r) || !r.ReadU8(type))
		return;

	// Relay-control messages are only trusted from the relay itself.
	if(env.player == MP_PLAYER_RELAY){
		if(type < MSG_CHAT)
			HandleRelayMessage(type, r, l);
		return;
	}
	if(m_state != STATE_IN_SESSION || env.player >= MP_MAX_PLAYERS)
		return;
	uint8_t from = env.player;

	switch(type){
	case MSG_CHAT: {
		SMsgChat msg;
		if(!msg.Read(r))
			return;
		FilterText(msg.text);
		if(l && msg.text[0])
			l->OnChat(from, msg.text);
		break;
	}
	case MSG_PING: {
		SMsgPing msg;
		if(!m_isHost || !msg.Read(r))
			return;
		SMsgPong pong;
		pong.clientSendMs = msg.clientSendMs;
		pong.hostMs = nowMs;
		CByteWriter w;
		pong.Write(w);
		SendRaw(MP_ROUTE_PLAYER, from, false, w);
		break;
	}
	case MSG_PONG: {
		SMsgPong msg;
		if(m_isHost || from != 0 || !msg.Read(r))
			return;
		m_clock.AddSample(msg.clientSendMs, msg.hostMs, nowMs);
		break;
	}
	default:
		if(type >= MSG_GAME_FIRST && l)
			l->OnGameMessage(from, type, r);
		break;
	}
}
