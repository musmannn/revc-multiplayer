#include "doctest.h"

#include <string.h>
#include <vector>

#include "RaceHost.h"

class CRecordingSink : public IRaceHostSink
{
public:
	std::vector<SRaceInvite> invites;
	std::vector<SRaceStart> starts;
	std::vector<SRaceStandings> standings;
	std::vector<SRaceStandings> ends;
	std::vector<SRaceCancel> cancels;
	std::vector<uint8_t> cancelTargets;

	void SendInvite(const SRaceInvite &m) override { invites.push_back(m); }
	void SendStart(const SRaceStart &m) override { starts.push_back(m); }
	void SendStandings(const SRaceStandings &m) override { standings.push_back(m); }
	void SendEnd(const SRaceStandings &m) override { ends.push_back(m); }
	void SendCancel(const SRaceCancel &m, uint8_t target) override { cancels.push_back(m); cancelTargets.push_back(target); }
};

static SRaceRoute
TestRoute(void)
{
	SRaceRoute r;
	memset(&r, 0, sizeof(r));
	strcpy(r.name, "Test Sprint");
	r.type = RACE_SPRINT;
	r.laps = 1;
	r.vehicleModel = 141;
	r.numGrid = 8;
	r.numCheckpoints = 2;
	r.cps[0].radius = r.cps[1].radius = 8.0f;
	return r;
}

TEST_CASE("race host: full race")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	REQUIRE(host.Request(1, route, true, 1000, &sink));
	CHECK(host.IsBusy());
	REQUIRE(sink.invites.size() == 1);
	uint16_t raceId = sink.invites[0].raceId;
	CHECK(sink.invites[0].startedBy == 1);
	CHECK(sink.invites[0].deadline == 1000 + CRaceHost::INVITE_MS);
	CHECK(strcmp(sink.invites[0].route.name, "Test Sprint") == 0);
	CHECK(sink.invites[0].route.numCheckpoints == route.numCheckpoints);

	host.OnAccept(2, raceId, 5000);
	host.OnAccept(3, raceId + 1, 5000);	// stale race id: ignored
	host.Update(1000 + CRaceHost::INVITE_MS - 1, &sink);
	CHECK(sink.starts.empty());
	host.OnAccept(4, raceId, 1000 + CRaceHost::INVITE_MS);	// at the deadline: too late
	host.Update(1000 + CRaceHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	const SRaceStart &start = sink.starts[0];
	CHECK(start.raceId == raceId);
	CHECK(start.startTime == 1000 + CRaceHost::INVITE_MS + CRaceHost::COUNTDOWN_MS);
	REQUIRE(start.n == 2);
	CHECK(start.ids[0] == 1);	// the starter takes pole
	CHECK(start.ids[1] == 2);
	CHECK(host.IsRacing());

	uint32_t t = start.startTime;
	SRaceCross c;
	c.raceId = raceId;
	c.seq = 0;
	c.time = t + 1000;
	host.OnCross(2, c);
	c.time = t + 1100;
	host.OnCross(1, c);
	c.seq = 1;
	c.time = t + 5000;
	host.OnCross(2, c);	// id 2 finishes first
	host.Update(t + 5100, &sink);
	CHECK(!sink.standings.empty());
	CHECK(sink.ends.empty());
	c.time = t + 6000;
	host.OnCross(1, c);
	host.Update(t + 6100, &sink);
	REQUIRE(sink.ends.size() == 1);
	const SRaceStandings &end = sink.ends[0];
	CHECK(end.msgType == GMSG_RACE_END);
	REQUIRE(end.n == 2);
	CHECK(end.entries[0].id == 2);
	CHECK(end.entries[0].state == RACER_FINISHED);
	CHECK(end.entries[0].finishTime == 5000);
	CHECK(end.entries[1].id == 1);
	CHECK(!host.IsBusy());
}

TEST_CASE("race host: busy and cancel")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	REQUIRE(host.Request(1, route, true, 1000, &sink));
	CHECK(!host.Request(2, route, true, 1100, &sink));
	REQUIRE(sink.cancels.size() == 1);
	CHECK(sink.cancels[0].reason == RACE_BUSY);
	CHECK(sink.cancelTargets[0] == 2);
}

TEST_CASE("race host: nobody accepts")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	REQUIRE(host.Request(0, route, false, 1000, &sink));	// a starter that doesn't race
	host.Update(1000 + CRaceHost::INVITE_MS, &sink);
	REQUIRE(sink.cancels.size() == 1);
	CHECK(sink.cancels[0].reason == RACE_NO_RACERS);
	CHECK(sink.cancelTargets[0] == MP_PLAYER_NONE);	// to everyone
	CHECK(!host.IsBusy());
	CHECK(sink.starts.empty());
}

TEST_CASE("race host: a time trial with only the starter")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	REQUIRE(host.Request(5, route, true, 1000, &sink));
	host.Update(1000 + CRaceHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	CHECK(sink.starts[0].n == 1);
	CHECK(sink.starts[0].ids[0] == 5);
}

TEST_CASE("race host: a racer leaving mid-race is DNF")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	REQUIRE(host.Request(1, route, true, 0, &sink));
	host.OnAccept(2, sink.invites[0].raceId, 100);
	host.Update(CRaceHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	uint32_t t = sink.starts[0].startTime;
	host.OnLeft(2);
	SRaceCross c;
	c.raceId = sink.starts[0].raceId;
	c.seq = 0;
	c.time = t + 100;
	host.OnCross(1, c);
	c.seq = 1;
	c.time = t + 200;
	host.OnCross(1, c);
	host.Update(t + 300, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(sink.ends[0].entries[1].id == 2);
	CHECK(sink.ends[0].entries[1].state == RACER_DNF);
}

TEST_CASE("race host: the grid is capped by the route")
{
	CRaceHost host;
	CRecordingSink sink;
	SRaceRoute route = TestRoute();
	route.numGrid = 2;
	REQUIRE(host.Request(0, route, true, 0, &sink));
	for(uint8_t id = 1; id < 6; id++)
		host.OnAccept(id, sink.invites[0].raceId, 100);
	host.Update(CRaceHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	CHECK(sink.starts[0].n == 2);
}
