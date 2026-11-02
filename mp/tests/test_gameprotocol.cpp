#include "doctest.h"

#include <string.h>

#include "GameProtocol.h"

template<typename T>
static bool
RoundTrip(const T &msg, uint8_t expectedType, T &out, uint32_t *size = nullptr)
{
	CByteWriter w;
	msg.Write(w);
	if(w.Overflowed())
		return false;
	if(size)
		*size = w.Size();
	CByteReader r(w.Data(), w.Size());
	uint8_t type = 0;
	if(!r.ReadU8(type) || type != expectedType)
		return false;
	return out.Read(r) && r.Remaining() == 0;
}

template<typename T>
static void
CheckAllPrefixesFail(const T &msg)
{
	CByteWriter w;
	msg.Write(w);
	REQUIRE(!w.Overflowed());
	for(uint32_t len = 1; len < w.Size(); len++){
		CByteReader r(w.Data(), len);
		uint8_t type = 0;
		REQUIRE(r.ReadU8(type));
		T out;
		CAPTURE(len);
		CHECK(!out.Read(r));
	}
}

static void
Fill3(float *v, float a, float b, float c)
{
	v[0] = a; v[1] = b; v[2] = c;
}

TEST_CASE("game message type values")
{
	CHECK((int)GMSG_PLAYER_STATE == (int)MSG_GAME_FIRST);
	CHECK(GMSG_PLAYER_STATE == 64);
	CHECK(GMSG_VEHICLE_STATE == 65);
	CHECK(GMSG_WORLD == 75);
}

TEST_CASE("player state round trip and size")
{
	SPlayerState in, out;
	in.time = 123456;
	in.skin = 7;
	in.flags = PSF_IN_VEHICLE | PSF_AIMING | PSF_JUMPING;
	Fill3(in.pos, 100.5f, -200.25f, 10.0f);
	in.heading = 1.25f;
	Fill3(in.vel, 0.01f, -0.02f, 0.0f);
	in.moveState = 4;
	in.weapon = 17;
	Fill3(in.aimPos, 1.0f, 2.0f, 3.0f);
	in.health = 87;
	in.armour = 42;
	in.vehicleId = MpMakeVehicleId(2, 99);
	in.seat = 1;
	uint32_t size = 0;
	REQUIRE(RoundTrip(in, GMSG_PLAYER_STATE, out, &size));
	CHECK(size <= 64);
	CHECK(out.time == 123456);
	CHECK(out.skin == 7);
	CHECK(out.flags == (PSF_IN_VEHICLE | PSF_AIMING | PSF_JUMPING));
	CHECK(out.pos[0] == 100.5f);
	CHECK(out.pos[1] == -200.25f);
	CHECK(out.pos[2] == 10.0f);
	CHECK(out.heading == 1.25f);
	CHECK(out.vel[0] == 0.01f);
	CHECK(out.vel[1] == -0.02f);
	CHECK(out.moveState == 4);
	CHECK(out.weapon == 17);
	CHECK(out.aimPos[2] == 3.0f);
	CHECK(out.health == 87);
	CHECK(out.armour == 42);
	CHECK(out.vehicleId == in.vehicleId);
	CHECK(out.seat == 1);
	CheckAllPrefixesFail(in);
}

TEST_CASE("vehicle state round trip and size")
{
	SVehicleState in, out;
	in.time = 999;
	in.vehicleId = MpMakeVehicleId(1, 5);
	Fill3(in.pos, 1.0f, 2.0f, 3.0f);
	in.quat[0] = 0.0f; in.quat[1] = 0.0f; in.quat[2] = 0.7071f; in.quat[3] = 0.7071f;
	Fill3(in.vel, 0.015f, 0.0f, 0.0f);
	Fill3(in.turn, 0.0f, 0.0f, 0.001f);
	in.steer = -0.3f;
	in.gas = 1.0f;
	in.brake = 0.25f;
	in.flags = VSF_LIGHTS | VSF_ENGINE;
	in.health = 650;
	uint32_t size = 0;
	REQUIRE(RoundTrip(in, GMSG_VEHICLE_STATE, out, &size));
	CHECK(size <= 80);
	CHECK(out.time == 999);
	CHECK(out.vehicleId == in.vehicleId);
	CHECK(out.pos[1] == 2.0f);
	CHECK(out.quat[2] == 0.7071f);
	CHECK(out.vel[0] == 0.015f);
	CHECK(out.turn[2] == 0.001f);
	CHECK(out.steer == -0.3f);
	CHECK(out.gas == 1.0f);
	CHECK(out.brake == 0.25f);
	CHECK(out.flags == (VSF_LIGHTS | VSF_ENGINE));
	CHECK(out.health == 650);
}

TEST_CASE("truncated vehicle state fails")
{
	SVehicleState in;
	in.vehicleId = 1;
	CheckAllPrefixesFail(in);
}

TEST_CASE("other game messages round trip")
{
	SUBCASE("vehicle create")
	{
		SVehicleCreate in, out;
		in.vehicleId = MpMakeVehicleId(3, 4095);
		in.model = 132;
		in.col1 = 12;
		in.col2 = 34;
		Fill3(in.pos, -5.0f, 6.0f, 7.0f);
		in.quat[3] = 1.0f;
		REQUIRE(RoundTrip(in, GMSG_VEHICLE_CREATE, out));
		CHECK(out.vehicleId == in.vehicleId);
		CHECK(out.model == 132);
		CHECK(out.col1 == 12);
		CHECK(out.col2 == 34);
		CHECK(out.pos[0] == -5.0f);
		CHECK(out.quat[3] == 1.0f);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("vehicle remove")
	{
		SVehicleRemove in, out;
		in.vehicleId = 77;
		REQUIRE(RoundTrip(in, GMSG_VEHICLE_REMOVE, out));
		CHECK(out.vehicleId == 77);
	}
	SUBCASE("vehicle enter")
	{
		SVehicleEnter in, out;
		in.vehicleId = 78;
		in.seat = 2;
		REQUIRE(RoundTrip(in, GMSG_VEHICLE_ENTER, out));
		CHECK(out.vehicleId == 78);
		CHECK(out.seat == 2);
	}
	SUBCASE("vehicle seat")
	{
		SVehicleSeat in, out;
		in.vehicleId = 79;
		in.playerId = 5;
		in.seat = MP_SEAT_NONE;
		in.accepted = 1;
		REQUIRE(RoundTrip(in, GMSG_VEHICLE_SEAT, out));
		CHECK(out.vehicleId == 79);
		CHECK(out.playerId == 5);
		CHECK(out.seat == MP_SEAT_NONE);
		CHECK(out.accepted == 1);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("vehicle exit")
	{
		SVehicleExit in, out;
		in.vehicleId = 80;
		REQUIRE(RoundTrip(in, GMSG_VEHICLE_EXIT, out));
		CHECK(out.vehicleId == 80);
	}
	SUBCASE("damage")
	{
		SDamage in, out;
		in.victimId = 4;
		in.weapon = 23;
		in.amount = 33.5f;
		in.piece = 9;
		in.direction = 2;
		REQUIRE(RoundTrip(in, GMSG_DAMAGE, out));
		CHECK(out.victimId == 4);
		CHECK(out.weapon == 23);
		CHECK(out.amount == 33.5f);
		CHECK(out.piece == 9);
		CHECK(out.direction == 2);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("died")
	{
		SDied in, out;
		in.killerId = MP_PLAYER_NONE;
		in.weapon = 6;
		REQUIRE(RoundTrip(in, GMSG_DIED, out));
		CHECK(out.killerId == MP_PLAYER_NONE);
		CHECK(out.weapon == 6);
	}
	SUBCASE("respawned")
	{
		SRespawned in, out;
		Fill3(in.pos, 10.0f, 20.0f, 30.0f);
		in.heading = 3.0f;
		REQUIRE(RoundTrip(in, GMSG_RESPAWNED, out));
		CHECK(out.pos[2] == 30.0f);
		CHECK(out.heading == 3.0f);
	}
	SUBCASE("shot")
	{
		SShot in, out;
		in.weapon = 17;
		Fill3(in.from, 1.0f, 2.0f, 3.0f);
		Fill3(in.to, 4.0f, 5.0f, 6.0f);
		REQUIRE(RoundTrip(in, GMSG_SHOT, out));
		CHECK(out.weapon == 17);
		CHECK(out.from[1] == 2.0f);
		CHECK(out.to[2] == 6.0f);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("world")
	{
		SWorldState in, out;
		in.hour = 23;
		in.minute = 59;
		in.weather = 4;
		REQUIRE(RoundTrip(in, GMSG_WORLD, out));
		CHECK(out.hour == 23);
		CHECK(out.minute == 59);
		CHECK(out.weather == 4);
	}
}

TEST_CASE("vehicle ids")
{
	CHECK(MpVehicleIdOwner(MpMakeVehicleId(3, 7)) == 3);
	CHECK(MpVehicleIdCounter(MpMakeVehicleId(3, 7)) == 7);
	CHECK(MpVehicleIdOwner(MpMakeVehicleId(7, 4095)) == 7);
	CHECK(MpVehicleIdCounter(MpMakeVehicleId(7, 4095)) == 4095);
	CHECK(MpMakeVehicleId(0, 1) != MP_VEHICLE_NONE);
	// Ids from different owners never collide.
	CHECK(MpMakeVehicleId(1, 5) != MpMakeVehicleId(2, 5));
}

TEST_CASE("vehicle ownership transfer")
{
	const uint8_t N = MP_PLAYER_NONE;
	SUBCASE("driver leaves with a passenger in seat 2")
	{
		uint8_t seats[MP_MAX_SEATS] = { 3, N, 5, N };
		CHECK(MpNextVehicleOwner(3, seats, 3) == 5);
	}
	SUBCASE("driver stays when a passenger leaves")
	{
		uint8_t seats[MP_MAX_SEATS] = { 3, 4, N, N };
		CHECK(MpNextVehicleOwner(3, seats, 4) == 3);
	}
	SUBCASE("lowest seat wins without a driver")
	{
		uint8_t seats[MP_MAX_SEATS] = { 1, N, 6, 2 };
		CHECK(MpNextVehicleOwner(1, seats, 1) == 6);
	}
	SUBCASE("empty car")
	{
		uint8_t seats[MP_MAX_SEATS] = { N, N, N, N };
		CHECK(MpNextVehicleOwner(2, seats, 2) == N);
	}
	SUBCASE("owner of a parked car leaves")
	{
		uint8_t seats[MP_MAX_SEATS] = { N, N, N, N };
		CHECK(MpNextVehicleOwner(4, seats, 4) == N);
	}
	SUBCASE("a non-occupant leaves")
	{
		uint8_t seats[MP_MAX_SEATS] = { 3, N, 5, N };
		CHECK(MpNextVehicleOwner(3, seats, 7) == 3);
	}
	SUBCASE("a passenger leaves a car someone else owns")
	{
		// The owner is still in the session, so the car stays theirs.
		uint8_t seats[MP_MAX_SEATS] = { N, 2, N, N };
		CHECK(MpNextVehicleOwner(0, seats, 2) == 0);
	}
}

static SRaceRoute
SampleRoute(uint8_t type = RACE_SPRINT, int nGrid = 2, int nCp = 3)
{
	SRaceRoute r;
	memset(&r, 0, sizeof(r));
	strcpy(r.name, type == RACE_ARENA ? "Beach Arena" : "Ocean Drive Sprint");
	r.type = type;
	r.laps = type == RACE_CIRCUIT ? 3 : 1;
	r.vehicleModel = type == RACE_ARENA ? 0 : 141;
	r.numGrid = (uint8_t)nGrid;
	for(int i = 0; i < nGrid; i++){
		r.grid[i].pos[0] = 100.0f + i * 3.125f;
		r.grid[i].pos[1] = -1200.5f;
		r.grid[i].pos[2] = 10.25f;
		r.grid[i].heading = 90.0f + i;
	}
	r.numCheckpoints = (uint8_t)nCp;
	for(int i = 0; i < nCp; i++){
		r.cps[i].pos[0] = -2000.0f + i * 50.0f;
		r.cps[i].pos[1] = 1500.875f;
		r.cps[i].pos[2] = 12.0f;
		r.cps[i].radius = 8.0f + i * 0.5f;
	}
	return r;
}

static void
CheckSameRoute(const SRaceRoute &a, const SRaceRoute &b)
{
	CHECK(strcmp(a.name, b.name) == 0);
	CHECK(a.type == b.type);
	CHECK(a.laps == b.laps);
	CHECK(a.vehicleModel == b.vehicleModel);
	REQUIRE(a.numGrid == b.numGrid);
	REQUIRE(a.numCheckpoints == b.numCheckpoints);
	for(int i = 0; i < a.numGrid; i++){
		for(int k = 0; k < 3; k++)
			CHECK(b.grid[i].pos[k] == doctest::Approx(a.grid[i].pos[k]).epsilon(0.0001));
		CHECK(b.grid[i].heading == doctest::Approx(a.grid[i].heading).epsilon(0.001));
	}
	for(int i = 0; i < a.numCheckpoints; i++){
		for(int k = 0; k < 3; k++)
			CHECK(b.cps[i].pos[k] == doctest::Approx(a.cps[i].pos[k]).epsilon(0.0001));
		CHECK(b.cps[i].radius == doctest::Approx(a.cps[i].radius));
	}
}

TEST_CASE("route data on the wire")
{
	SUBCASE("round trip, every type")
	{
		for(uint8_t type = RACE_SPRINT; type <= RACE_ARENA; type++){
			SRaceRoute in = SampleRoute(type), out;
			CByteWriter w;
			MpWriteRouteData(w, in);
			CByteReader r(w.Data(), w.Size());
			REQUIRE(MpReadRouteData(r, out));
			CheckSameRoute(in, out);
			CHECK(r.Remaining() == 0);
		}
	}
	SUBCASE("a full route fits a packet with room to spare")
	{
		SRaceRoute in = SampleRoute(RACE_CIRCUIT, MP_MAX_PLAYERS, MP_MAX_CHECKPOINTS);
		SRaceInvite invite;
		invite.route = in;
		CByteWriter w;
		invite.Write(w);
		CHECK(w.Size() <= 600);
		CHECK(w.Size() + 2 <= MP_MAX_PACKET);
		SRaceInvite out;
		CByteReader r(w.Data() + 1, w.Size() - 1);
		REQUIRE(out.Read(r));
		CheckSameRoute(in, out.route);
	}
	SUBCASE("quantized: 1/8 m positions, 1/2 m radii, headings wrap")
	{
		SRaceRoute in = SampleRoute();
		in.grid[0].pos[0] = 1.06f;	// -> 1.0
		in.grid[0].heading = -90.0f;	// -> 270
		in.cps[0].radius = 7.8f;	// -> 8.0
		CByteWriter w;
		MpWriteRouteData(w, in);
		SRaceRoute out;
		CByteReader r(w.Data(), w.Size());
		REQUIRE(MpReadRouteData(r, out));
		CHECK(out.grid[0].pos[0] == doctest::Approx(1.0f));
		CHECK(out.grid[0].heading == doctest::Approx(270.0f));
		CHECK(out.cps[0].radius == doctest::Approx(8.0f));
	}
	SUBCASE("invalid routes are refused")
	{
		SRaceRoute bad = SampleRoute(RACE_SPRINT, 1, 1);	// one checkpoint
		CByteWriter w;
		MpWriteRouteData(w, bad);
		SRaceRoute out;
		CByteReader r(w.Data(), w.Size());
		CHECK(!MpReadRouteData(r, out));

		CByteWriter w2;
		SRaceRoute noName = SampleRoute();
		noName.name[0] = '\0';
		MpWriteRouteData(w2, noName);
		CByteReader r2(w2.Data(), w2.Size());
		CHECK(!MpReadRouteData(r2, out));
	}
	SUBCASE("too many grid slots or checkpoints in the counts")
	{
		CByteWriter w;
		w.WriteString("X", MP_MAX_ROUTE_NAME);
		w.WriteU8(RACE_SPRINT);
		w.WriteU8(1);
		w.WriteU16(141);
		w.WriteU8(MP_MAX_PLAYERS + 1);
		SRaceRoute out;
		CByteReader r(w.Data(), w.Size());
		CHECK(!MpReadRouteData(r, out));
	}
}

TEST_CASE("race messages round trip")
{
	SUBCASE("request")
	{
		SRaceRequest in, out;
		in.route = SampleRoute();
		REQUIRE(RoundTrip(in, GMSG_RACE_REQUEST, out));
		CheckSameRoute(in.route, out.route);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("a request for an arena isn't a race")
	{
		SRaceRequest in, out;
		in.route = SampleRoute(RACE_ARENA);
		CHECK(!RoundTrip(in, GMSG_RACE_REQUEST, out));
	}
	SUBCASE("invite")
	{
		SRaceInvite in, out;
		in.raceId = 7;
		in.startedBy = 3;
		in.route = SampleRoute(RACE_CIRCUIT);
		in.deadline = 0xABCDEF01;
		REQUIRE(RoundTrip(in, GMSG_RACE_INVITE, out));
		CHECK(out.raceId == 7);
		CHECK(out.startedBy == 3);
		CheckSameRoute(in.route, out.route);
		CHECK(out.deadline == 0xABCDEF01);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("accept")
	{
		SRaceAccept in, out;
		in.raceId = 9;
		REQUIRE(RoundTrip(in, GMSG_RACE_ACCEPT, out));
		CHECK(out.raceId == 9);
	}
	SUBCASE("start")
	{
		SRaceStart in, out;
		in.raceId = 2;
		in.startTime = 123456;
		in.n = 3;
		in.ids[0] = 4;
		in.ids[1] = 0;
		in.ids[2] = 7;
		REQUIRE(RoundTrip(in, GMSG_RACE_START, out));
		CHECK(out.raceId == 2);
		CHECK(out.startTime == 123456);
		REQUIRE(out.n == 3);
		CHECK(out.ids[0] == 4);
		CHECK(out.ids[2] == 7);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("cross")
	{
		SRaceCross in, out;
		in.raceId = 2;
		in.seq = 17;
		in.time = 99999;
		REQUIRE(RoundTrip(in, GMSG_RACE_CROSS, out));
		CHECK(out.raceId == 2);
		CHECK(out.seq == 17);
		CHECK(out.time == 99999);
	}
	SUBCASE("standings and end, 8 racers")
	{
		SRaceStandings in, out;
		in.raceId = 5;
		in.n = MP_MAX_PLAYERS;
		for(uint8_t i = 0; i < MP_MAX_PLAYERS; i++){
			in.entries[i].id = (uint8_t)(7 - i);
			in.entries[i].nextSeq = (uint16_t)(100 + i);
			in.entries[i].state = (uint8_t)(i % 3);
			in.entries[i].finishTime = 60000u + i;
		}
		uint32_t size = 0;
		REQUIRE(RoundTrip(in, GMSG_RACE_STANDINGS, out, &size));
		CHECK(size <= 100);
		CHECK(out.raceId == 5);
		REQUIRE(out.n == MP_MAX_PLAYERS);
		CHECK(out.entries[0].id == 7);
		CHECK(out.entries[7].nextSeq == 107);
		CHECK(out.entries[5].state == 2);
		CHECK(out.entries[3].finishTime == 60003u);
		CheckAllPrefixesFail(in);

		in.msgType = GMSG_RACE_END;
		SRaceStandings end;
		REQUIRE(RoundTrip(in, GMSG_RACE_END, end));
		CHECK(end.n == MP_MAX_PLAYERS);
	}
	SUBCASE("cancel")
	{
		SRaceCancel in, out;
		in.raceId = 3;
		in.reason = RACE_BAD_ROUTE;
		REQUIRE(RoundTrip(in, GMSG_RACE_CANCEL, out));
		CHECK(out.raceId == 3);
		CHECK(out.reason == RACE_BAD_ROUTE);
	}
	SUBCASE("too many racers rejected")
	{
		CByteWriter w;
		w.WriteU8(GMSG_RACE_START);
		w.WriteU16(1);
		w.WriteU32(1);
		w.WriteU8(MP_MAX_PLAYERS + 1);
		for(uint32_t i = 0; i < MP_MAX_PLAYERS + 1; i++)
			w.WriteU8((uint8_t)i);
		CByteReader r(w.Data(), w.Size());
		uint8_t type;
		REQUIRE(r.ReadU8(type));
		SRaceStart out;
		CHECK(!out.Read(r));
	}
}

TEST_CASE("match messages round trip")
{
	SUBCASE("request")
	{
		SMatchRequest in, out;
		in.mode = MATCH_TDM;
		REQUIRE(RoundTrip(in, GMSG_MATCH_REQUEST, out));
		CHECK(out.mode == MATCH_TDM);
		CHECK(!out.hasArena);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("request with an arena")
	{
		SMatchRequest in, out;
		in.mode = MATCH_KOTH;
		in.hasArena = true;
		in.arena = SampleRoute(RACE_ARENA, 4, 3);
		REQUIRE(RoundTrip(in, GMSG_MATCH_REQUEST, out));
		CHECK(out.mode == MATCH_KOTH);
		REQUIRE(out.hasArena);
		CheckSameRoute(in.arena, out.arena);
		CheckAllPrefixesFail(in);
		// A race route isn't an arena.
		in.arena = SampleRoute(RACE_SPRINT);
		CHECK(!RoundTrip(in, GMSG_MATCH_REQUEST, out));
	}
	SUBCASE("stunt")
	{
		SStunt in, out;
		in.matchId = 12;
		in.points = 1500;
		in.airMs = 2300;
		in.flips = 2;
		in.spins = 1;
		REQUIRE(RoundTrip(in, GMSG_STUNT, out));
		CHECK(out.matchId == 12);
		CHECK(out.points == 1500);
		CHECK(out.airMs == 2300);
		CHECK(out.flips == 2);
		CHECK(out.spins == 1);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("invite")
	{
		SMatchInvite in, out;
		in.matchId = 4;
		in.mode = MATCH_DM;
		in.startedBy = 2;
		in.scoreLimit = 20;
		in.timeLimitMs = 600000;
		in.deadline = 0xFFFFFF00u;
		in.hasArena = true;
		in.arena = SampleRoute(RACE_ARENA);
		REQUIRE(RoundTrip(in, GMSG_MATCH_INVITE, out));
		REQUIRE(out.hasArena);
		CheckSameRoute(in.arena, out.arena);
		CHECK(out.matchId == 4);
		CHECK(out.mode == MATCH_DM);
		CHECK(out.startedBy == 2);
		CHECK(out.scoreLimit == 20);
		CHECK(out.timeLimitMs == 600000);
		CHECK(out.deadline == 0xFFFFFF00u);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("accept")
	{
		SMatchAccept in, out;
		in.matchId = 9;
		REQUIRE(RoundTrip(in, GMSG_MATCH_ACCEPT, out));
		CHECK(out.matchId == 9);
	}
	SUBCASE("start")
	{
		SMatchStart in, out;
		in.matchId = 3;
		in.mode = MATCH_TDM;
		in.scoreLimit = 30;
		in.timeLimitMs = 1000;
		in.startTime = 777;
		in.n = 2;
		in.ids[0] = 5;
		in.teams[0] = MP_TEAM_RED;
		in.ids[1] = 1;
		in.teams[1] = MP_TEAM_BLUE;
		REQUIRE(RoundTrip(in, GMSG_MATCH_START, out));
		CHECK(out.matchId == 3);
		CHECK(out.mode == MATCH_TDM);
		CHECK(out.scoreLimit == 30);
		CHECK(out.timeLimitMs == 1000);
		CHECK(out.startTime == 777);
		REQUIRE(out.n == 2);
		CHECK(out.ids[1] == 1);
		CHECK(out.teams[1] == MP_TEAM_BLUE);
		CheckAllPrefixesFail(in);
	}
	SUBCASE("scores with 8 players, and end")
	{
		SMatchScores in, out;
		in.matchId = 6;
		in.teamScore[0] = 12;
		in.teamScore[1] = -3;
		in.winnerTeam = MP_TEAM_BLUE;
		in.n = MP_MAX_PLAYERS;
		for(uint8_t i = 0; i < MP_MAX_PLAYERS; i++){
			in.entries[i].id = i;
			in.entries[i].team = (uint8_t)(i % 2);
			in.entries[i].score = (int16_t)(i - 3);
			in.entries[i].kills = (uint16_t)(i * 2);
			in.entries[i].deaths = (uint16_t)(i + 1);
		}
		uint32_t size = 0;
		REQUIRE(RoundTrip(in, GMSG_MATCH_SCORES, out, &size));
		CHECK(size <= 120);
		CHECK(out.matchId == 6);
		CHECK(out.teamScore[1] == -3);
		CHECK(out.winnerTeam == MP_TEAM_BLUE);
		REQUIRE(out.n == MP_MAX_PLAYERS);
		CHECK(out.entries[0].score == -3);
		CHECK(out.entries[7].kills == 14);
		CHECK(out.entries[7].deaths == 8);
		CheckAllPrefixesFail(in);
		in.msgType = GMSG_MATCH_END;
		SMatchScores end;
		REQUIRE(RoundTrip(in, GMSG_MATCH_END, end));
	}
	SUBCASE("cancel")
	{
		SMatchCancel in, out;
		in.matchId = 2;
		in.reason = MATCH_NOT_ENOUGH;
		REQUIRE(RoundTrip(in, GMSG_MATCH_CANCEL, out));
		CHECK(out.reason == MATCH_NOT_ENOUGH);
	}
	CHECK((int)GMSG_RACE_LAST < (int)GMSG_MATCH_FIRST);
	CHECK((int)GMSG_MATCH_CANCEL <= (int)GMSG_MATCH_LAST);
	CHECK((int)GMSG_STUNT <= (int)GMSG_MATCH_LAST);
	CHECK((int)GMSG_MATCH_LAST <= (int)GMSG_LAST);
}
