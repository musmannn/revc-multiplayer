// Relay + CNetClient end to end, in one process over loopback.

#include "doctest.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

#include "NetClient.h"
#include "NetInit.h"
#include "Protocol.h"
#include "RelayServer.h"

static uint32_t
NowMs(void)
{
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct SChat { uint8_t from; std::string text; };
struct SGame { uint8_t from; uint8_t type; uint32_t size; };
struct SJoined { uint8_t id; std::string name; };

class CRecorder : public INetClientListener
{
public:
	bool ready = false;
	std::string code;
	uint8_t myId = MP_PLAYER_NONE;
	bool isHost = false;
	int joinFailed = -1;
	std::vector<SJoined> joined;
	std::vector<uint8_t> left;
	int ended = -1;
	bool lost = false;
	std::vector<SChat> chats;
	std::vector<SGame> games;

	void OnSessionReady(const char *c, uint8_t id, bool host) override { ready = true; code = c; myId = id; isHost = host; }
	void OnJoinFailed(eJoinResult r) override { joinFailed = r; }
	void OnPlayerJoined(uint8_t id, const char *name) override { joined.push_back(SJoined{ id, name }); }
	void OnPlayerLeft(uint8_t id) override { left.push_back(id); }
	void OnSessionEnded(eEndReason r) override { ended = r; }
	void OnConnectionLost(void) override { lost = true; }
	void OnChat(uint8_t from, const char *text) override { chats.push_back(SChat{ from, text }); }
	void OnGameMessage(uint8_t from, uint8_t type, CByteReader &body) override { games.push_back(SGame{ from, type, body.Remaining() }); }

	bool HasJoined(uint8_t id, const char *name) const
	{
		for(const SJoined &j : joined)
			if(j.id == id && j.name == name)
				return true;
		return false;
	}
	bool HasLeft(uint8_t id) const
	{
		for(uint8_t l : left)
			if(l == id)
				return true;
		return false;
	}
	int GamesOfType(uint8_t type) const
	{
		int n = 0;
		for(const SGame &g : games)
			if(g.type == type)
				n++;
		return n;
	}
};

struct SNode
{
	CNetClient client;
	CRecorder rec;
};

// A bare ENet peer that speaks raw bytes, for malformed or hand-built messages.
class CRawPeer
{
public:
	ENetHost *host = nullptr;
	ENetPeer *peer = nullptr;
	bool connected = false;
	bool disconnected = false;
	std::vector<std::vector<uint8_t>> received;

	CRawPeer(void) { MpNetAcquire(); }
	~CRawPeer(void)
	{
		if(host)
			enet_host_destroy(host);
		MpNetRelease();
	}
	void Connect(uint16_t port)
	{
		host = enet_host_create(nullptr, 1, MP_NUM_CHANNELS, 0, 0);
		ENetAddress addr;
		enet_address_set_host_ip(&addr, "127.0.0.1");
		addr.port = port;
		peer = enet_host_connect(host, &addr, MP_NUM_CHANNELS, 0);
	}
	void Send(const void *data, size_t size, bool reliable = true)
	{
		ENetPacket *p = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
		if(enet_peer_send(peer, reliable ? MP_CHANNEL_RELIABLE : MP_CHANNEL_SNAPSHOT, p) != 0)
			enet_packet_destroy(p);
	}
	// Envelope (route RELAY) + message.
	void SendToRelay(const CByteWriter &msg)
	{
		CByteWriter w;
		SEnvelope env;
		env.route = MP_ROUTE_RELAY;
		env.player = MP_PLAYER_NONE;
		env.Write(w);
		w.WriteBytes(msg.Data(), msg.Size());
		Send(w.Data(), w.Size());
	}
	void Service(void)
	{
		if(host == nullptr)
			return;
		ENetEvent ev;
		while(enet_host_service(host, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_CONNECT)
				connected = true;
			else if(ev.type == ENET_EVENT_TYPE_DISCONNECT)
				disconnected = true;
			else if(ev.type == ENET_EVENT_TYPE_RECEIVE){
				received.push_back(std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength));
				enet_packet_destroy(ev.packet);
			}
		}
	}
	// The last JOIN_RESULT from the relay, or -1.
	int JoinResult(void) const
	{
		int result = -1;
		for(const std::vector<uint8_t> &p : received)
			if(p.size() >= 4 && p[1] == MP_PLAYER_RELAY && p[2] == MSG_JOIN_RESULT)
				result = p[3];
		return result;
	}
};

class CHarness
{
public:
	CRelayServer relay;
	std::vector<std::unique_ptr<SNode>> nodes;
	std::vector<CRawPeer*> raws;
	uint16_t port = 0;

	explicit CHarness(uint32_t maxPacketsPerSecond = 200, uint32_t maxQueuedPerPeer = 0, uint32_t maxFailedJoins = 0)
	{
		SRelayConfig config;
		config.maxPacketsPerSecond = maxPacketsPerSecond;
		if(maxQueuedPerPeer)
			config.maxQueuedPerPeer = maxQueuedPerPeer;
		if(maxFailedJoins)
			config.maxFailedJoinsPerMinute = maxFailedJoins;
		config.bindAddress = "127.0.0.1";
		for(uint16_t p = 27700; p < 27800; p++){
			config.port = p;
			if(relay.Start(config)){
				port = relay.Port();
				break;
			}
		}
		REQUIRE(port != 0);
	}
	SNode &Add(void)
	{
		nodes.push_back(std::unique_ptr<SNode>(new SNode));
		return *nodes.back();
	}
	void PumpOnce(void)
	{
		relay.Service(0);
		for(size_t i = 0; i < nodes.size(); i++)
			nodes[i]->client.Service(NowMs(), &nodes[i]->rec);
		for(CRawPeer *r : raws)
			r->Service();
	}
	// sleepBetween=false spins (yielding) instead of sleeping, so OS sleep
	// granularity doesn't skew timing-sensitive tests.
	bool Pump(std::function<bool(void)> pred, uint32_t timeoutMs = 3000, bool sleepBetween = true)
	{
		uint32_t start = NowMs();
		for(;;){
			PumpOnce();
			if(pred())
				return true;
			if(NowMs() - start >= timeoutMs)
				return false;
			if(sleepBetween)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			else
				std::this_thread::yield();
		}
	}
	void PumpFor(uint32_t ms)
	{
		Pump([](){ return false; }, ms);
	}
	SNode &HostNode(const char *name = "Tommy")
	{
		SNode &n = Add();
		REQUIRE(n.client.Host("127.0.0.1", port, name));
		REQUIRE(Pump([&](){ return n.rec.ready; }));
		return n;
	}
	SNode &JoinNode(const std::string &code, const char *name)
	{
		SNode &n = Add();
		REQUIRE(n.client.Join("127.0.0.1", port, code.c_str(), name));
		REQUIRE(Pump([&](){ return n.rec.ready || n.rec.joinFailed >= 0; }));
		return n;
	}
	// A raw peer that has joined the session with code.
	void RawJoin(CRawPeer &raw, const std::string &code, const char *name, uint16_t version = MP_PROTOCOL_VERSION)
	{
		raws.push_back(&raw);
		raw.Connect(port);
		REQUIRE(Pump([&](){ return raw.connected; }));
		SMsgJoinSession join;
		join.version = version;
		strcpy(join.code, code.c_str());
		strcpy(join.name, name);
		CByteWriter w;
		join.Write(w);
		raw.SendToRelay(w);
		REQUIRE(Pump([&](){ return raw.JoinResult() >= 0; }));
	}
};

static void
SendGameMsg(CNetClient &c, uint8_t route, uint8_t target, uint8_t type, bool reliable = true)
{
	CByteWriter w;
	w.WriteU8(type);
	w.WriteU32(0x12345678);
	CHECK(c.SendGame(route, target, reliable, w));
}

static bool
IsCodeShape(const std::string &code)
{
	if(code.size() != 9 || code.compare(0, 5, "VICE-") != 0)
		return false;
	for(size_t i = 5; i < 9; i++)
		if(strchr("ABCDEFGHJKMNPQRSTUVWXYZ23456789", code[i]) == nullptr)
			return false;
	return true;
}

TEST_CASE("relay: host gets code")
{
	CHarness h;
	SNode &host = h.HostNode("Tommy");
	CHECK(IsCodeShape(host.rec.code));
	CHECK(host.rec.myId == 0);
	CHECK(host.rec.isHost);
	CHECK(host.client.IsInSession());
	CHECK(host.client.IsHost());
	CHECK(host.client.MyId() == 0);
	CHECK(host.rec.code == host.client.Code());
	CHECK(host.client.Clock().IsSynced());
	CHECK(h.relay.NumSessions() == 1);
}

TEST_CASE("relay: join and roster")
{
	CHarness h;
	SNode &host = h.HostNode("Tommy");
	SNode &lance = h.JoinNode(host.rec.code, "Lance");
	SNode &ken = h.JoinNode(host.rec.code, "Ken");
	REQUIRE(lance.rec.ready);
	REQUIRE(ken.rec.ready);
	CHECK(lance.rec.myId == 1);
	CHECK(ken.rec.myId == 2);
	CHECK(!lance.rec.isHost);
	CHECK(lance.client.Code() == host.rec.code);
	REQUIRE(h.Pump([&](){ return host.rec.joined.size() == 2 && lance.rec.joined.size() == 2 && ken.rec.joined.size() == 2; }));
	CHECK(host.rec.HasJoined(1, "Lance"));
	CHECK(host.rec.HasJoined(2, "Ken"));
	CHECK(lance.rec.HasJoined(0, "Tommy"));
	CHECK(lance.rec.HasJoined(2, "Ken"));
	CHECK(ken.rec.HasJoined(0, "Tommy"));
	CHECK(ken.rec.HasJoined(1, "Lance"));
}

TEST_CASE("relay: join with sloppy code")
{
	CHarness h;
	SNode &host = h.HostNode();
	std::string sloppy = " vice - " + host.rec.code.substr(5) + " ";
	for(char &c : sloppy)
		if(c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	SNode &j = h.JoinNode(sloppy, "Lance");
	CHECK(j.rec.ready);
	CHECK(j.rec.code == host.rec.code);
}

TEST_CASE("relay: duplicate names")
{
	CHarness h;
	SNode &host = h.HostNode("Tommy");
	h.JoinNode(host.rec.code, "Lance");
	h.JoinNode(host.rec.code, "Lance");
	h.JoinNode(host.rec.code, "LANCE");
	h.JoinNode(host.rec.code, "abcdefghijklmnopqrstuvw");	// 23 chars
	h.JoinNode(host.rec.code, "abcdefghijklmnopqrstuvw");
	REQUIRE(h.Pump([&](){ return host.rec.joined.size() == 5; }));
	CHECK(host.rec.HasJoined(1, "Lance"));
	CHECK(host.rec.HasJoined(2, "Lance (2)"));
	CHECK(host.rec.HasJoined(3, "LANCE (3)"));
	CHECK(host.rec.HasJoined(4, "abcdefghijklmnopqrstuvw"));
	CHECK(host.rec.HasJoined(5, "abcdefghijklmnopqrs (2)"));
}

TEST_CASE("relay: full session")
{
	CHarness h;
	SNode &host = h.HostNode();
	for(int i = 0; i < 7; i++){
		char name[16];
		snprintf(name, sizeof(name), "P%d", i + 1);
		SNode &n = h.JoinNode(host.rec.code, name);
		REQUIRE(n.rec.ready);
		CHECK(n.rec.myId == i + 1);
	}
	SNode &ninth = h.JoinNode(host.rec.code, "Ninth");
	CHECK(!ninth.rec.ready);
	CHECK(ninth.rec.joinFailed == JOIN_FULL);
	CHECK(!ninth.client.IsInSession());

	// A slot frees up: the next joiner gets the lowest free id.
	h.nodes[3]->client.Leave();	// id 3
	REQUIRE(h.Pump([&](){ return host.rec.HasLeft(3); }));
	SNode &late = h.JoinNode(host.rec.code, "Late");
	REQUIRE(late.rec.ready);
	CHECK(late.rec.myId == 3);
}

TEST_CASE("relay: bad code")
{
	CHarness h;
	SNode &a = h.JoinNode("VICE-ZZZZ", "Lance");
	CHECK(a.rec.joinFailed == JOIN_NOT_FOUND);
	SNode &b = h.JoinNode("VICE-AB0K", "Lance");
	CHECK(b.rec.joinFailed == JOIN_NOT_FOUND);
	SNode &c = h.JoinNode("not a code at all", "Lance");
	CHECK(c.rec.joinFailed == JOIN_NOT_FOUND);
	CHECK(!a.rec.ready);
	CHECK(!a.client.IsInSession());
}

TEST_CASE("relay: version mismatch")
{
	CHarness h;
	SNode &host = h.HostNode();
	CRawPeer raw;
	h.RawJoin(raw, host.rec.code, "Old", 999);
	CHECK(raw.JoinResult() == JOIN_VERSION_MISMATCH);

	// Create with the wrong version gets the same answer.
	CRawPeer raw2;
	h.raws.push_back(&raw2);
	raw2.Connect(h.port);
	REQUIRE(h.Pump([&](){ return raw2.connected; }));
	SMsgCreateSession create;
	create.version = 999;
	strcpy(create.name, "Old");
	CByteWriter w;
	create.Write(w);
	raw2.SendToRelay(w);
	REQUIRE(h.Pump([&](){ return raw2.JoinResult() >= 0; }));
	CHECK(raw2.JoinResult() == JOIN_VERSION_MISMATCH);
	CHECK(h.relay.NumSessions() == 1);
}

TEST_CASE("relay: bad names")
{
	CHarness h;
	SNode &empty = h.Add();
	REQUIRE(empty.client.Host("127.0.0.1", h.port, ""));
	REQUIRE(h.Pump([&](){ return empty.rec.joinFailed >= 0; }));
	CHECK(empty.rec.joinFailed == JOIN_BAD_NAME);
	CHECK(h.relay.NumSessions() == 0);

	SNode &host = h.HostNode();
	SNode &ctrl = h.JoinNode(host.rec.code, "\x01\x02   ");
	CHECK(ctrl.rec.joinFailed == JOIN_BAD_NAME);

	// A name longer than MP_MAX_NAME_LEN on the wire.
	CRawPeer raw;
	h.raws.push_back(&raw);
	raw.Connect(h.port);
	REQUIRE(h.Pump([&](){ return raw.connected; }));
	CByteWriter w;
	w.WriteU8(MSG_JOIN_SESSION);
	w.WriteU16(MP_PROTOCOL_VERSION);
	w.WriteString(host.rec.code.c_str(), 9);
	w.WriteString("0123456789012345678901234567890123456789", 64);
	raw.SendToRelay(w);
	REQUIRE(h.Pump([&](){ return raw.JoinResult() >= 0; }));
	CHECK(raw.JoinResult() == JOIN_BAD_NAME);
}

TEST_CASE("relay: chat to all")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	SNode &c2 = h.JoinNode(host.rec.code, "Ken");
	CHECK(c1.client.SendChat("hi"));
	REQUIRE(h.Pump([&](){ return host.rec.chats.size() == 1 && c1.rec.chats.size() == 1 && c2.rec.chats.size() == 1; }));
	for(SNode *n : { &host, &c1, &c2 }){
		CHECK(n->rec.chats[0].from == 1);
		CHECK(n->rec.chats[0].text == "hi");
	}
	CHECK(!c1.client.SendChat(""));
}

TEST_CASE("relay: routing")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	SNode &c2 = h.JoinNode(host.rec.code, "Ken");

	SUBCASE("to host")
	{
		SendGameMsg(c2.client, MP_ROUTE_HOST, 0, 64);
		REQUIRE(h.Pump([&](){ return host.rec.GamesOfType(64) == 1; }));
		h.PumpFor(100);
		CHECK(host.rec.games[0].from == 2);
		CHECK(host.rec.games[0].size == 4);
		CHECK(c1.rec.games.empty());
		CHECK(c2.rec.games.empty());
	}
	SUBCASE("to one player")
	{
		SendGameMsg(c2.client, MP_ROUTE_PLAYER, 1, 65);
		REQUIRE(h.Pump([&](){ return c1.rec.GamesOfType(65) == 1; }));
		h.PumpFor(100);
		CHECK(c1.rec.games[0].from == 2);
		CHECK(host.rec.games.empty());
		CHECK(c2.rec.games.empty());
	}
	SUBCASE("to everyone but self")
	{
		SendGameMsg(c1.client, MP_ROUTE_ALL_BUT_SELF, 0, 66, false);
		REQUIRE(h.Pump([&](){ return host.rec.GamesOfType(66) == 1 && c2.rec.GamesOfType(66) == 1; }));
		h.PumpFor(100);
		CHECK(host.rec.games[0].from == 1);
		CHECK(c2.rec.games[0].from == 1);
		CHECK(c1.rec.games.empty());
	}
	SUBCASE("to all")
	{
		SendGameMsg(host.client, MP_ROUTE_ALL, 0, 67);
		REQUIRE(h.Pump([&](){ return host.rec.GamesOfType(67) == 1 && c1.rec.GamesOfType(67) == 1 && c2.rec.GamesOfType(67) == 1; }));
		CHECK(c1.rec.games[0].from == 0);
	}
	SUBCASE("unknown target is dropped")
	{
		SendGameMsg(c1.client, MP_ROUTE_PLAYER, 6, 68);
		h.PumpFor(100);
		CHECK(host.rec.games.empty());
		CHECK(c1.rec.games.empty());
		CHECK(c2.rec.games.empty());
	}
	SUBCASE("SendGame refuses non-game types and oversize messages")
	{
		CByteWriter w;
		w.WriteU8(MSG_CHAT);
		w.WriteString("spoof", 32);
		CHECK(!c1.client.SendGame(MP_ROUTE_ALL, 0, true, w));
		CByteWriter big;
		big.WriteU8(70);
		for(uint32_t i = 1; i < MP_MAX_PACKET; i++)
			big.WriteU8(0);
		CHECK(!c1.client.SendGame(MP_ROUTE_ALL, 0, true, big));
		CByteWriter justFits;
		justFits.WriteU8(71);
		for(uint32_t i = 1; i < MP_MAX_PACKET - 2; i++)
			justFits.WriteU8(0);
		CHECK(c1.client.SendGame(MP_ROUTE_HOST, 0, true, justFits));
		REQUIRE(h.Pump([&](){ return host.rec.GamesOfType(71) == 1; }));
		CHECK(host.rec.games[0].size == MP_MAX_PACKET - 3);
	}
}

TEST_CASE("relay: host leaves ends session")
{
	CHarness h;
	SNode &host = h.HostNode();
	std::string code = host.rec.code;
	SNode &c1 = h.JoinNode(code, "Lance");
	SNode &c2 = h.JoinNode(code, "Ken");
	host.client.Leave();
	CHECK(!host.client.IsInSession());
	REQUIRE(h.Pump([&](){ return c1.rec.ended >= 0 && c2.rec.ended >= 0; }));
	CHECK(c1.rec.ended == END_HOST_LEFT);
	CHECK(c2.rec.ended == END_HOST_LEFT);
	CHECK(!c1.client.IsInSession());
	CHECK(h.relay.NumSessions() == 0);
	SNode &late = h.JoinNode(code, "Late");
	CHECK(late.rec.joinFailed == JOIN_NOT_FOUND);
	// Members are disconnected and their slots freed.
	REQUIRE(h.Pump([&](){ return h.relay.NumPeers() == 0; }));
	CHECK(!c1.rec.lost);
}

TEST_CASE("relay: member leaves")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	SNode &c2 = h.JoinNode(host.rec.code, "Ken");
	c1.client.Leave();
	REQUIRE(h.Pump([&](){ return host.rec.HasLeft(1) && c2.rec.HasLeft(1); }));
	CHECK(host.client.IsInSession());
	CHECK(h.relay.NumSessions() == 1);
	CHECK(c1.rec.left.empty());
}

TEST_CASE("relay: garbage is dropped")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	CRawPeer raw;
	h.RawJoin(raw, host.rec.code, "Garbage");
	REQUIRE(raw.JoinResult() == JOIN_OK);

	uint32_t seed = 12345;
	for(int i = 0; i < 50; i++){
		uint8_t buf[64];
		size_t len = 1 + (size_t)(i % 64);
		for(size_t j = 0; j < len; j++){
			seed = seed * 1103515245u + 12345u;
			buf[j] = (uint8_t)(seed >> 16);
		}
		raw.Send(buf, len, (i & 1) != 0);
	}
	std::vector<uint8_t> big(1500, 0);
	big[0] = MP_ROUTE_ALL;
	big[2] = 99;
	raw.Send(big.data(), big.size());
	h.PumpFor(200);

	CHECK(h.relay.IsRunning());
	CHECK(h.relay.NumSessions() == 1);
	for(const SGame &g : host.rec.games)
		CHECK(g.type != 99);
	CHECK(c1.client.SendChat("still here"));
	REQUIRE(h.Pump([&](){
		for(const SChat &c : host.rec.chats)
			if(c.from == 1 && c.text == "still here")
				return true;
		return false;
	}));
	CHECK(!host.rec.lost);
	CHECK(!c1.rec.lost);
}

TEST_CASE("relay: first message must be create or join")
{
	CHarness h;
	CRawPeer raw;
	h.raws.push_back(&raw);
	raw.Connect(h.port);
	REQUIRE(h.Pump([&](){ return raw.connected; }));
	const uint8_t junk[4] = { MP_ROUTE_ALL, 0, MSG_CHAT, 0 };
	raw.Send(junk, sizeof(junk));
	CHECK(h.Pump([&](){ return raw.disconnected; }));
	CHECK(h.relay.NumSessions() == 0);
}

TEST_CASE("relay: rate limit")
{
	CHarness h(50);
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	h.PumpFor(1100);	// let the joins' packets age out of c1's window
	for(int i = 0; i < 120; i++)
		SendGameMsg(c1.client, MP_ROUTE_HOST, 0, 64);
	h.PumpFor(500);
	CHECK(host.rec.GamesOfType(64) > 0);
	CHECK(host.rec.GamesOfType(64) <= 100);
	CHECK(h.relay.IsRunning());
}

TEST_CASE("relay: clock syncs")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	CHECK(!c1.client.Clock().IsSynced());
	// No sleeps: each ping leg then takes about the same time, which is what
	// the RTT/2 estimate assumes (macOS runners oversleep 1 ms by several ms).
	h.Pump([](){ return false; }, 4000, false);
	CHECK(c1.client.Clock().IsSynced());
	uint32_t now = NowMs();
	int32_t diff = (int32_t)(c1.client.SessionTime(now) - host.client.SessionTime(now));
	CAPTURE(diff);
	CHECK(abs(diff) <= 5);
	CHECK(host.client.SessionTime(now) == now);
}

TEST_CASE("relay: shutdown ends sessions")
{
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	h.relay.Stop();
	REQUIRE(h.Pump([&](){ return host.rec.ended >= 0 && c1.rec.ended >= 0; }));
	CHECK(host.rec.ended == END_RELAY_SHUTDOWN);
	CHECK(c1.rec.ended == END_RELAY_SHUTDOWN);
}

TEST_CASE("relay: unreachable relay times out")
{
	CHarness h;
	uint16_t deadPort = h.port;
	h.relay.Stop();	// nothing listens on deadPort now
	SNode &n = h.Add();
	REQUIRE(n.client.Join("127.0.0.1", deadPort, "VICE-AB7K", "Lance"));
	uint32_t start = NowMs();
	REQUIRE(h.Pump([&](){ return n.rec.joinFailed >= 0 || n.rec.lost; }, 8000));
	CHECK(n.rec.joinFailed == JOIN_NOT_FOUND);
	CHECK(NowMs() - start >= 4900);
	CHECK(!n.client.IsInSession());
}

// Re-hosts from inside OnSessionEnded, like a game "host again" button might.
class CRehostRecorder : public CRecorder
{
public:
	CNetClient *client = nullptr;
	uint16_t port = 0;
	bool rehosted = false;
	void OnSessionEnded(eEndReason r) override
	{
		CRecorder::OnSessionEnded(r);
		ready = false;
		code.clear();
		rehosted = client->Host("127.0.0.1", port, "Again");
	}
};

TEST_CASE("relay: host again from inside a callback")
{
	CHarness h;
	SNode &host = h.HostNode();
	CNetClient c1;
	CRehostRecorder rec;
	rec.client = &c1;
	rec.port = h.port;
	REQUIRE(c1.Join("127.0.0.1", h.port, host.rec.code.c_str(), "Lance"));
	REQUIRE(h.Pump([&](){ c1.Service(NowMs(), &rec); return rec.ready; }));
	h.Pump([&](){ c1.Service(NowMs(), &rec); return false; }, 5200);	// older than the connect timeout
	host.client.Leave();
	// First the end (which re-hosts from inside the callback), then the new session.
	REQUIRE(h.Pump([&](){ c1.Service(NowMs(), &rec); return rec.ended >= 0; }));
	CHECK(rec.ended == END_HOST_LEFT);
	CHECK(rec.rehosted);
	REQUIRE(h.Pump([&](){ c1.Service(NowMs(), &rec); return rec.ready || rec.joinFailed >= 0; }));
	CHECK(rec.joinFailed == -1);
	CHECK(rec.ready);
	CHECK(rec.isHost);
	CHECK(IsCodeShape(rec.code));
}

TEST_CASE("relay: oversized first packet closes the connection")
{
	CHarness h;
	CRawPeer raw;
	h.raws.push_back(&raw);
	raw.Connect(h.port);
	REQUIRE(h.Pump([&](){ return raw.connected; }));
	std::vector<uint8_t> big(1500, 0);
	raw.Send(big.data(), big.size());
	CHECK(h.Pump([&](){ return raw.disconnected; }));
}

TEST_CASE("relay: a peer that stops acknowledging is dropped before its queue grows")
{
	CHarness h(1000, 64);
	SNode &host = h.HostNode();
	CRawPeer raw;
	h.RawJoin(raw, host.rec.code, "Stuck");
	REQUIRE(raw.JoinResult() == JOIN_OK);
	REQUIRE(h.Pump([&](){ return host.rec.joined.size() == 1; }));
	uint8_t stuckId = host.rec.joined[0].id;
	// From now on the raw peer is never serviced, so it never acks.
	h.raws.clear();
	for(int i = 0; i < 400; i++)
		SendGameMsg(host.client, MP_ROUTE_ALL_BUT_SELF, 0, 64, true);
	REQUIRE(h.Pump([&](){ return host.rec.HasLeft(stuckId); }, 4000));
	CHECK(host.client.IsInSession());
	CHECK(h.relay.NumSessions() == 1);
}

TEST_CASE("relay: failed joins are rate limited per address")
{
	CHarness h(200, 0, 3);
	SNode &host = h.HostNode();
	for(int i = 0; i < 3; i++){
		SNode &bad = h.JoinNode("VICE-ZZZZ", "Guess");
		CHECK(bad.rec.joinFailed == JOIN_NOT_FOUND);
	}
	// Over the limit: even the right code is refused for now.
	SNode &right = h.JoinNode(host.rec.code, "Lance");
	CHECK(right.rec.joinFailed == JOIN_NOT_FOUND);
	CHECK(!right.rec.ready);
}

TEST_CASE("relay: a client that stalls for a long load stays in the session")
{
	// A game can stop servicing the network for several seconds while it
	// loads the world. That must not drop it (MP_PEER_TIMEOUT_MIN_MS).
	CHarness h;
	SNode &host = h.HostNode();
	SNode &c1 = h.JoinNode(host.rec.code, "Lance");
	uint32_t start = NowMs();
	while(NowMs() - start < 8000){
		// Everyone but the stalled host keeps going.
		h.relay.Service(0);
		c1.client.Service(NowMs(), &c1.rec);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	h.PumpFor(500);
	CHECK(host.client.IsInSession());
	CHECK(!host.rec.lost);
	CHECK(c1.client.IsInSession());
	CHECK(c1.rec.ended < 0);
	CHECK(h.relay.NumSessions() == 1);
}

TEST_CASE("relay: a dedicated session's flag reaches joiners")
{
	CHarness h;
	SNode &server = h.Add();
	REQUIRE(server.client.Host("127.0.0.1", h.port, "Server", MP_SESSION_DEDICATED | 0x80));
	REQUIRE(h.Pump([&](){ return server.rec.ready; }));
	CHECK(server.client.IsDedicatedSession());
	SNode &c1 = h.JoinNode(server.rec.code, "Lance");
	REQUIRE(c1.rec.ready);
	CHECK(c1.client.IsDedicatedSession());
	CHECK(c1.client.SessionFlags() == MP_SESSION_DEDICATED);	// unknown bits dropped

	SNode &other = h.HostNode("Tommy");
	SNode &c2 = h.JoinNode(other.rec.code, "Ken");
	CHECK(!other.client.IsDedicatedSession());
	CHECK(!c2.client.IsDedicatedSession());
}
