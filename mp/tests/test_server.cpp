#include "doctest.h"

#include <string.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DedicatedHost.h"
#include "RelayServer.h"

// Integration tests: a real relay, a dedicated host and real clients over
// loopback. The host is fed a "virtual" clock that tests can push forward to
// skip invite windows and countdowns.

namespace {

uint32_t
RealMs(void)
{
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct SGameMsg
{
	uint8_t from;
	uint8_t type;
	std::vector<uint8_t> body;
};

class CPlayerRec : public INetClientListener
{
public:
	bool ready = false;
	std::string code;
	uint8_t myId = MP_PLAYER_NONE;
	std::vector<std::string> chats;
	std::vector<SGameMsg> games;
	int ended = -1;

	void OnSessionReady(const char *c, uint8_t id, bool) override { ready = true; code = c; myId = id; }
	void OnChat(uint8_t, const char *text) override { chats.push_back(text); }
	void OnSessionEnded(eEndReason r) override { ended = r; }
	void OnGameMessage(uint8_t from, uint8_t type, CByteReader &body) override
	{
		SGameMsg m;
		m.from = from;
		m.type = type;
		m.body.assign(body.Current(), body.Current() + body.Remaining());
		games.push_back(m);
	}
	int Count(uint8_t type) const
	{
		int n = 0;
		for(const SGameMsg &m : games)
			if(m.type == type)
				n++;
		return n;
	}
	template<typename T> bool Last(uint8_t type, T &out) const
	{
		for(size_t i = games.size(); i-- > 0; )
			if(games[i].type == type){
				CByteReader r(games[i].body.data(), (uint32_t)games[i].body.size());
				return out.Read(r);
			}
		return false;
	}
};

struct SPlayer
{
	CNetClient client;
	CPlayerRec rec;
};

struct CServerHarness
{
	CRelayServer relay;
	uint16_t port = 0;
	std::unique_ptr<CDedicatedHost> server;
	std::vector<std::unique_ptr<SPlayer>> players;
	uint32_t skew = 0;	// added to the server's clock

	explicit CServerHarness(const SServerConfig &config)
	{
		SRelayConfig rc;
		rc.port = 0;
		REQUIRE(relay.Start(rc));
		port = relay.Port();
		server.reset(new CDedicatedHost(config));
		REQUIRE(server->Start("127.0.0.1", port, ServerNow()));
		REQUIRE(Pump([&](){ return server->IsInSession(); }));
	}
	uint32_t ServerNow(void) const { return RealMs() + skew; }
	void PumpOnce(void)
	{
		relay.Service(0);
		server->Service(ServerNow());
		for(auto &p : players)
			p->client.Service(RealMs(), &p->rec);
	}
	bool Pump(std::function<bool(void)> pred, uint32_t timeoutMs = 3000)
	{
		uint32_t start = RealMs();
		for(;;){
			PumpOnce();
			if(pred())
				return true;
			if(RealMs() - start >= timeoutMs)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	void PumpFor(uint32_t ms) { Pump([](){ return false; }, ms); }
	SPlayer &Join(const char *name)
	{
		players.push_back(std::unique_ptr<SPlayer>(new SPlayer));
		SPlayer &p = *players.back();
		REQUIRE(p.client.Join("127.0.0.1", port, server->Code(), name));
		REQUIRE(Pump([&](){ return p.rec.ready; }));
		return p;
	}
	void Send(SPlayer &p, uint8_t route, bool reliable, const CByteWriter &w, uint8_t target = 0)
	{
		REQUIRE(p.client.SendGame(route, target, reliable, w));
	}
	// A PLAYER_STATE at pos: the server's catch-up trigger and position source.
	void State(SPlayer &p, float x, float y, float z, bool dead = false)
	{
		SPlayerState s;
		s.pos[0] = x;
		s.pos[1] = y;
		s.pos[2] = z;
		s.flags = dead ? PSF_DEAD : 0;
		CByteWriter w;
		s.Write(w);
		Send(p, MP_ROUTE_ALL, false, w);
	}
};

SRaceRoute
TestRoute(void)
{
	SRaceRoute r;
	memset(&r, 0, sizeof(r));
	strcpy(r.name, "Server Sprint");
	r.type = RACE_SPRINT;
	r.laps = 1;
	r.vehicleModel = 141;
	r.numGrid = 2;
	r.numCheckpoints = 2;
	r.cps[0].radius = r.cps[1].radius = 8.0f;
	r.cps[1].pos[0] = 100.0f;
	return r;
}

SRaceRoute
TestArena(void)
{
	SRaceRoute a;
	memset(&a, 0, sizeof(a));
	strcpy(a.name, "Server Arena");
	a.type = RACE_ARENA;
	a.laps = 1;
	a.numGrid = 2;
	a.numCheckpoints = 1;
	a.cps[0].pos[2] = 10.0f;
	a.cps[0].radius = 5.0f;
	return a;
}

}

TEST_CASE("server: hosts a dedicated session")
{
	SServerConfig config;
	strcpy(config.name, "Vice Server");
	strcpy(config.motd, "Be nice");
	config.hour = 21;
	config.weather = 2;
	config.verbose = false;
	CServerHarness h(config);
	CHECK(h.server->Client().MyId() == 0);
	CHECK(h.server->Client().IsDedicatedSession());

	SPlayer &tommy = h.Join("Tommy");
	CHECK(tommy.client.IsDedicatedSession());
	CHECK(tommy.rec.myId != 0);
	REQUIRE(h.Pump([&](){ return h.server->NumPlayers() == 1; }));

	// Nothing is sent before the world is up; the first state brings the
	// world and the welcome.
	h.PumpFor(100);
	CHECK(tommy.rec.Count(GMSG_WORLD) == 0);
	h.State(tommy, 0, 0, 10);
	REQUIRE(h.Pump([&](){ return tommy.rec.Count(GMSG_WORLD) == 1 && !tommy.rec.chats.empty(); }));
	SWorldState world;
	REQUIRE(tommy.rec.Last(GMSG_WORLD, world));
	CHECK(world.hour == 21);
	CHECK(world.weather == 2);
	CHECK(tommy.rec.chats[0] == "Welcome Tommy! Be nice");

	// The clock runs a game minute per second, and is resent every 10 s.
	h.skew += 90 * 1000;
	REQUIRE(h.Pump([&](){ return tommy.rec.Count(GMSG_WORLD) == 2; }));
	REQUIRE(tommy.rec.Last(GMSG_WORLD, world));
	CHECK(world.hour == 22);
	CHECK(world.minute == 30);

	// The server leaving ends the session.
	h.server->Stop();
	REQUIRE(h.Pump([&](){ return tommy.rec.ended >= 0; }));
}

TEST_CASE("server: arbitrates seats")
{
	SServerConfig config;
	config.verbose = false;
	CServerHarness h(config);
	SPlayer &a = h.Join("Tommy");
	SPlayer &b = h.Join("Lance");
	uint16_t car = MpMakeVehicleId(a.rec.myId, 1);

	SVehicleCreate create;
	create.vehicleId = car;
	create.model = 141;
	CByteWriter w;
	create.Write(w);
	h.Send(a, MP_ROUTE_ALL, true, w);
	REQUIRE(h.Pump([&](){ return h.server->Seats().Knows(car); }));

	SVehicleEnter enter;
	enter.vehicleId = car;
	enter.seat = MP_SEAT_DRIVER;
	w.Clear();
	enter.Write(w);
	h.Send(b, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_VEHICLE_SEAT) == 1 && b.rec.Count(GMSG_VEHICLE_SEAT) == 1; }));
	SVehicleSeat seat;
	REQUIRE(a.rec.Last(GMSG_VEHICLE_SEAT, seat));
	CHECK(seat.accepted == 1);
	CHECK(seat.playerId == b.rec.myId);
	CHECK(seat.seat == MP_SEAT_DRIVER);

	// Tommy wants the same seat: only he hears no.
	h.Send(a, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_VEHICLE_SEAT) == 2; }));
	REQUIRE(a.rec.Last(GMSG_VEHICLE_SEAT, seat));
	CHECK(seat.accepted == 0);
	h.PumpFor(100);
	CHECK(b.rec.Count(GMSG_VEHICLE_SEAT) == 1);

	// Lance leaves the session: the seat frees up.
	b.client.Leave();
	REQUIRE(h.Pump([&](){ return h.server->Seats().Occupant(car, MP_SEAT_DRIVER) == MP_PLAYER_NONE; }));
	h.Send(a, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_VEHICLE_SEAT) == 3; }));
	REQUIRE(a.rec.Last(GMSG_VEHICLE_SEAT, seat));
	CHECK(seat.accepted == 1);
}

TEST_CASE("server: runs a race a player asks for, on the player's route")
{
	SServerConfig config;
	config.verbose = false;
	CServerHarness h(config);
	SPlayer &a = h.Join("Tommy");
	SPlayer &b = h.Join("Lance");

	SRaceRequest req;
	req.route = TestRoute();
	CByteWriter w;
	req.Write(w);
	h.Send(a, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_RACE_INVITE) == 1; }));
	SRaceInvite invite;
	REQUIRE(b.rec.Last(GMSG_RACE_INVITE, invite));
	CHECK(invite.startedBy == a.rec.myId);
	CHECK(strcmp(invite.route.name, "Server Sprint") == 0);
	CHECK(invite.route.cps[1].pos[0] == 100.0f);

	SRaceAccept accept;
	accept.raceId = invite.raceId;
	w.Clear();
	accept.Write(w);
	h.Send(b, MP_ROUTE_HOST, true, w);
	h.PumpFor(100);
	h.skew += CRaceHost::INVITE_MS;
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_RACE_START) == 1 && b.rec.Count(GMSG_RACE_START) == 1; }));
	SRaceStart start;
	REQUIRE(a.rec.Last(GMSG_RACE_START, start));
	REQUIRE(start.n == 2);
	CHECK(start.ids[0] == a.rec.myId);	// the starter takes pole

	// Lance wins by crossing first on his own clock.
	for(uint16_t seq = 0; seq < 2; seq++){
		SRaceCross cross;
		cross.raceId = invite.raceId;
		cross.seq = seq;
		cross.time = start.startTime + 5000 + seq * 1000;
		w.Clear();
		cross.Write(w);
		h.Send(b, MP_ROUTE_HOST, true, w);
		cross.time += 500;
		w.Clear();
		cross.Write(w);
		h.Send(a, MP_ROUTE_HOST, true, w);
	}
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_RACE_END) == 1; }));
	SRaceStandings end;
	REQUIRE(a.rec.Last(GMSG_RACE_END, end));
	REQUIRE(end.n == 2);
	CHECK(end.entries[0].id == b.rec.myId);
	CHECK(!h.server->Race().IsBusy());

	// A race request that isn't a valid route gets RACE_BAD_ROUTE.
	SRaceRequest bad;
	bad.route = TestRoute();
	bad.route.numCheckpoints = 1;
	w.Clear();
	bad.Write(w);
	h.Send(a, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return a.rec.Count(GMSG_RACE_CANCEL) == 1; }));
	SRaceCancel cancel;
	REQUIRE(a.rec.Last(GMSG_RACE_CANCEL, cancel));
	CHECK(cancel.reason == RACE_BAD_ROUTE);
}

TEST_CASE("server: Hold the Hill from player positions")
{
	SServerConfig config;
	config.verbose = false;
	CServerHarness h(config);
	SPlayer &a = h.Join("Tommy");
	SPlayer &b = h.Join("Lance");
	h.State(a, 50, 0, 10);
	h.State(b, 60, 0, 10);

	SMatchRequest req;
	req.mode = MATCH_KOTH;
	req.hasArena = true;
	req.arena = TestArena();
	CByteWriter w;
	req.Write(w);
	h.Send(a, MP_ROUTE_HOST, true, w);
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_MATCH_INVITE) == 1; }));
	SMatchInvite invite;
	REQUIRE(b.rec.Last(GMSG_MATCH_INVITE, invite));
	CHECK(invite.mode == MATCH_KOTH);
	REQUIRE(invite.hasArena);
	CHECK(strcmp(invite.arena.name, "Server Arena") == 0);
	SMatchAccept accept;
	accept.matchId = invite.matchId;
	w.Clear();
	accept.Write(w);
	h.Send(b, MP_ROUTE_HOST, true, w);
	h.PumpFor(100);
	h.skew += CMatchHost::INVITE_MS + CMatchHost::COUNTDOWN_MS;
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_MATCH_START) == 1; }));

	// Tommy stands on the hill for a few real seconds; Lance stays away.
	uint32_t until = RealMs() + 2600;
	while((int32_t)(RealMs() - until) < 0){
		h.State(a, 1, 1, 10);
		h.State(b, 60, 0, 10);
		h.PumpFor(50);
	}
	const SMatchPlayer *tommy = h.server->Match().Match().Get(a.rec.myId);
	REQUIRE(tommy);
	CHECK(tommy->score >= 2);
	CHECK(tommy->score <= 3);
	CHECK(h.server->Match().Match().Get(b.rec.myId)->score == 0);
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_MATCH_SCORES) > 0; }));
}

TEST_CASE("server: the playlist starts events by itself")
{
	SServerConfig config;
	config.verbose = false;
	config.playlistGapMs = 1000;
	config.playlistLength = 2;
	config.playlist[0].race = true;
	config.playlist[0].hasRoute = true;
	config.playlist[0].route = TestRoute();
	config.playlist[1].race = false;
	config.playlist[1].mode = MATCH_STUNT;
	config.playlist[1].hasRoute = true;
	config.playlist[1].route = TestArena();
	CServerHarness h(config);
	SPlayer &a = h.Join("Tommy");

	// One player isn't enough.
	h.skew += 5000;
	h.PumpFor(100);
	CHECK(a.rec.Count(GMSG_RACE_INVITE) == 0);

	SPlayer &b = h.Join("Lance");
	h.skew += 1000;
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_RACE_INVITE) == 1; }));
	SRaceInvite invite;
	REQUIRE(b.rec.Last(GMSG_RACE_INVITE, invite));
	CHECK(invite.startedBy == 0);	// the server
	CHECK(strcmp(invite.route.name, "Server Sprint") == 0);

	// Nobody accepts: cancelled, then after the gap the next entry.
	h.skew += CRaceHost::INVITE_MS;
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_RACE_CANCEL) == 1; }));
	h.PumpFor(50);
	CHECK(b.rec.Count(GMSG_MATCH_INVITE) == 0);
	h.skew += 1000;
	REQUIRE(h.Pump([&](){ return b.rec.Count(GMSG_MATCH_INVITE) == 1; }));
	SMatchInvite m;
	REQUIRE(b.rec.Last(GMSG_MATCH_INVITE, m));
	CHECK(m.mode == MATCH_STUNT);
	CHECK(m.hasArena);
	CHECK(m.startedBy == 0);
}

TEST_CASE("seat table")
{
	CSeatTable t;
	uint16_t car = MpMakeVehicleId(1, 1), bike = MpMakeVehicleId(2, 7);
	t.OnCreate(car);
	CHECK(t.Knows(car));
	CHECK(t.OnEnter(1, car, 0));
	CHECK(t.OnEnter(1, car, 0));	// again: still theirs
	CHECK(!t.OnEnter(2, car, 0));
	CHECK(t.OnEnter(2, car, 1));
	CHECK(!t.OnEnter(3, car, MP_MAX_SEATS));
	// Moving seat or vehicle frees the old seat.
	CHECK(t.OnEnter(2, bike, 0));	// unknown: taken on trust
	CHECK(t.Knows(bike));
	CHECK(t.Occupant(car, 1) == MP_PLAYER_NONE);
	t.OnExit(1, car);
	CHECK(t.Occupant(car, 0) == MP_PLAYER_NONE);
	t.OnExit(2, MP_VEHICLE_NONE);
	CHECK(t.Occupant(bike, 0) == MP_PLAYER_NONE);
	t.OnRemove(car);
	CHECK(!t.Knows(car));
	CHECK(t.Count() == 1);
	// Full: the oldest entries make room.
	for(int i = 0; i < CSeatTable::MAX_VEHICLES + 10; i++)
		t.OnCreate((uint16_t)(0x1000 + i));
	CHECK(t.Count() == CSeatTable::MAX_VEHICLES);
}
