#include "doctest.h"

#include <string.h>

#include "RaceLogic.h"

static SRaceRoute
MakeRoute(uint8_t type, int numCps, int laps)
{
	SRaceRoute r;
	memset(&r, 0, sizeof(r));
	strcpy(r.name, "Test");
	r.type = type;
	r.laps = (uint8_t)laps;
	r.vehicleModel = 141;
	r.numGrid = 3;
	r.numCheckpoints = (uint8_t)numCps;
	for(int i = 0; i < numCps; i++){
		r.cps[i].pos[0] = (float)(i * 100);
		r.cps[i].radius = 8.0f;
	}
	return r;
}

static const uint32_t START = 10000;

struct SRaceFixture
{
	CRaceState race;
	SRaceFixture(void)
	{
		SRaceRoute route = MakeRoute(RACE_SPRINT, 3, 1);
		const uint8_t ids[3] = { 0, 1, 2 };
		race.Start(route, ids, 3, START);
	}
	void Finish(uint8_t id, uint32_t t0, uint32_t finishAt)
	{
		REQUIRE(race.OnCrossing(id, 0, t0));
		REQUIRE(race.OnCrossing(id, 1, t0 + 1000));
		REQUIRE(race.OnCrossing(id, 2, finishAt));
	}
};

TEST_CASE("race: in order")
{
	SRaceFixture f;
	CHECK(f.race.NumRacers() == 3);
	CHECK(f.race.OnCrossing(1, 0, 20000));
	CHECK(f.race.OnCrossing(1, 1, 30000));
	CHECK(f.race.GetRacer(1)->state == RACER_RACING);
	CHECK(f.race.OnCrossing(1, 2, 41234));
	const SRacer *r = f.race.GetRacer(1);
	REQUIRE(r);
	CHECK(r->state == RACER_FINISHED);
	CHECK(r->finishTime == 41234 - START);
	CHECK(r->nextSeq == 3);
	CHECK(!f.race.IsOver());
}

TEST_CASE("race: skip rejected")
{
	SRaceFixture f;
	CHECK(!f.race.OnCrossing(0, 1, 20000));
	CHECK(f.race.GetRacer(0)->nextSeq == 0);
	CHECK(f.race.OnCrossing(0, 0, 20001));
}

TEST_CASE("race: duplicate rejected")
{
	SRaceFixture f;
	CHECK(f.race.OnCrossing(0, 0, 20000));
	CHECK(!f.race.OnCrossing(0, 0, 20000));
	CHECK(f.race.GetRacer(0)->nextSeq == 1);
}

TEST_CASE("race: before start rejected")
{
	SRaceFixture f;
	CHECK(!f.race.OnCrossing(0, 0, START - 1));
	CHECK(f.race.GetRacer(0)->nextSeq == 0);
	CHECK(f.race.OnCrossing(0, 0, START));
}

TEST_CASE("race: unknown racer and finished racer rejected")
{
	SRaceFixture f;
	CHECK(!f.race.OnCrossing(5, 0, 20000));
	CHECK(f.race.GetRacer(5) == nullptr);
	f.Finish(0, 20000, 30000);
	CHECK(!f.race.OnCrossing(0, 3, 31000));
}

TEST_CASE("race: photo finish ordered by timestamp, not arrival")
{
	SRaceFixture f;
	f.Finish(2, 20000, 50003);	// arrives first...
	f.Finish(1, 20000, 50000);	// ...but crossed 3 ms earlier
	uint8_t ids[MP_MAX_PLAYERS];
	int n = f.race.GetStandings(ids);
	REQUIRE(n == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 2);
	CHECK(ids[2] == 0);
}

TEST_CASE("race: exact tie goes to the lower id")
{
	SRaceFixture f;
	f.Finish(2, 20000, 50000);
	f.Finish(1, 20000, 50000);
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(f.race.GetStandings(ids) == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 2);
}

TEST_CASE("race: racing order by progress then distance")
{
	SRaceFixture f;
	REQUIRE(f.race.OnCrossing(0, 0, 20000));
	REQUIRE(f.race.OnCrossing(0, 1, 21000));
	REQUIRE(f.race.OnCrossing(2, 0, 20500));
	REQUIRE(f.race.OnCrossing(2, 1, 21500));
	f.race.SetDistance(0, 50.0f);
	f.race.SetDistance(2, 10.0f);
	f.race.SetDistance(1, 1.0f);	// closest, but a checkpoint behind
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(f.race.GetStandings(ids) == 3);
	CHECK(ids[0] == 2);
	CHECK(ids[1] == 0);
	CHECK(ids[2] == 1);
	// Same progress and distance: lower id first.
	f.race.SetDistance(0, 10.0f);
	f.race.GetStandings(ids);
	CHECK(ids[0] == 0);
	CHECK(ids[1] == 2);
}

TEST_CASE("race: dnf on leave")
{
	SRaceFixture f;
	f.race.OnLeft(0);
	CHECK(f.race.GetRacer(0)->state == RACER_DNF);
	CHECK(!f.race.OnCrossing(0, 0, 20000));
	f.Finish(1, 20000, 40000);
	CHECK(!f.race.IsOver());
	f.Finish(2, 20000, 45000);
	CHECK(f.race.IsOver());
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(f.race.GetStandings(ids) == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 2);
	CHECK(ids[2] == 0);
}

TEST_CASE("race: dnf racers sorted by id after finishers")
{
	SRaceFixture f;
	f.race.OnLeft(2);
	f.race.OnLeft(0);
	f.Finish(1, 20000, 40000);
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(f.race.GetStandings(ids) == 3);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 0);
	CHECK(ids[2] == 2);
	CHECK(f.race.IsOver());
}

TEST_CASE("race: timeout after first finish")
{
	SRaceFixture f;
	f.Finish(1, 20000, 40000);
	f.race.Update(99999);
	CHECK(!f.race.IsOver());
	f.race.Update(100001);
	CHECK(f.race.IsOver());
	CHECK(f.race.GetRacer(0)->state == RACER_DNF);
	CHECK(f.race.GetRacer(2)->state == RACER_DNF);
	CHECK(f.race.GetRacer(1)->state == RACER_FINISHED);
}

TEST_CASE("race: overall time limit")
{
	SRaceFixture f;
	REQUIRE(f.race.OnCrossing(0, 0, 20000));
	f.race.Update(START + 20 * 60 * 1000 - 1);
	CHECK(!f.race.IsOver());
	f.race.Update(START + 20 * 60 * 1000);
	CHECK(f.race.IsOver());
	for(uint8_t id = 0; id < 3; id++)
		CHECK(f.race.GetRacer(id)->state == RACER_DNF);
}

TEST_CASE("race: circuit laps")
{
	CRaceState race;
	SRaceRoute route = MakeRoute(RACE_CIRCUIT, 2, 3);
	const uint8_t ids[1] = { 4 };
	race.Start(route, ids, 1, START);
	CHECK(race.LapOf(4) == 1);
	CHECK(race.OnCrossing(4, 0, 11000));
	CHECK(race.LapOf(4) == 1);
	CHECK(race.OnCrossing(4, 1, 12000));	// the lap line
	CHECK(race.LapOf(4) == 2);
	CHECK(race.OnCrossing(4, 2, 13000));
	CHECK(race.OnCrossing(4, 3, 14000));
	CHECK(race.LapOf(4) == 3);
	CHECK(race.OnCrossing(4, 4, 15000));
	CHECK(race.GetRacer(4)->state == RACER_RACING);
	CHECK(race.OnCrossing(4, 5, 16000));
	CHECK(race.GetRacer(4)->state == RACER_FINISHED);
	CHECK(race.GetRacer(4)->finishTime == 6000);
	CHECK(race.LapOf(4) == 3);
	CHECK(race.IsOver());
}

TEST_CASE("race: crossings keep time order per racer")
{
	SRaceFixture f;
	CHECK(f.race.OnCrossing(0, 0, 20000));
	CHECK(!f.race.OnCrossing(0, 1, 19000));	// earlier than the last crossing
	CHECK(f.race.OnCrossing(0, 1, 20000));
}

TEST_CASE("race: session time wraparound")
{
	CRaceState race;
	SRaceRoute route = MakeRoute(RACE_SPRINT, 2, 1);
	const uint8_t ids[1] = { 0 };
	const uint32_t start = 0xFFFFF000u;
	race.Start(route, ids, 1, start);
	CHECK(race.OnCrossing(0, 0, 0xFFFFFF00u));
	CHECK(race.OnCrossing(0, 1, 0x00000100u));
	CHECK(race.GetRacer(0)->finishTime == 0x1100u);
}
