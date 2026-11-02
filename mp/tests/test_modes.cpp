#include "doctest.h"

#include <math.h>
#include <string.h>

#include <vector>

#include "HillLogic.h"
#include "StuntLogic.h"
#include "MatchHost.h"

namespace {

class CModeSink : public IMatchHostSink
{
public:
	std::vector<SMatchInvite> invites;
	std::vector<SMatchStart> starts;
	std::vector<SMatchScores> scores;
	std::vector<SMatchScores> ends;
	std::vector<SMatchCancel> cancels;
	std::vector<uint8_t> cancelTargets;

	void SendInvite(const SMatchInvite &m) override { invites.push_back(m); }
	void SendStart(const SMatchStart &m) override { starts.push_back(m); }
	void SendScores(const SMatchScores &m) override { scores.push_back(m); }
	void SendEnd(const SMatchScores &m) override { ends.push_back(m); }
	void SendCancel(const SMatchCancel &m, uint8_t target) override { cancels.push_back(m); cancelTargets.push_back(target); }
};

SRaceRoute
Arena(int hills = 3)
{
	SRaceRoute a;
	memset(&a, 0, sizeof(a));
	strcpy(a.name, "Test Arena");
	a.type = RACE_ARENA;
	a.laps = 1;
	a.numGrid = 2;
	a.numCheckpoints = (uint8_t)hills;
	for(int i = 0; i < hills; i++){
		a.cps[i].pos[0] = 100.0f * i;
		a.cps[i].pos[1] = 0.0f;
		a.cps[i].pos[2] = 10.0f;
		a.cps[i].radius = 5.0f;
	}
	return a;
}

SHillPlayer
Player(uint8_t id, float x, float y, float z, uint8_t team = MP_TEAM_NONE, bool alive = true)
{
	SHillPlayer p;
	p.id = id;
	p.team = team;
	p.pos[0] = x;
	p.pos[1] = y;
	p.pos[2] = z;
	p.alive = alive;
	return p;
}

void
Pos(float out[3], float x, float y, float z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

}

TEST_CASE("hill: the active hill moves every minute and wraps")
{
	CHillState h;
	h.Start(Arena(3), 10000);
	CHECK(h.NumHills() == 3);
	CHECK(h.ActiveHill(5000) == 0);	// before the start
	CHECK(h.ActiveHill(10000) == 0);
	CHECK(h.ActiveHill(10000 + CHillState::MOVE_MS - 1) == 0);
	CHECK(h.ActiveHill(10000 + CHillState::MOVE_MS) == 1);
	CHECK(h.ActiveHill(10000 + 2 * CHillState::MOVE_MS) == 2);
	CHECK(h.ActiveHill(10000 + 3 * CHillState::MOVE_MS) == 0);
	CHECK(h.NextMove(10000 + 5) == 10000 + CHillState::MOVE_MS);
	CHECK(h.NextMove(10000 + CHillState::MOVE_MS) == 10000 + 2 * CHillState::MOVE_MS);

	// Session time can wrap around 2^32.
	CHillState w;
	w.Start(Arena(2), 0xFFFFFF00u);
	CHECK(w.ActiveHill(0xFFFFFF00u + CHillState::MOVE_MS) == 1);
}

TEST_CASE("hill: who's on it")
{
	CHillState h;
	h.Start(Arena(1), 0);
	float p[3];
	Pos(p, 3, 4, 10);	// 5 m away: on the edge
	CHECK(h.IsOn(p, 0));
	Pos(p, 3.1f, 4, 10);
	CHECK(!h.IsOn(p, 0));
	Pos(p, 0, 0, 10 + CHillState::HEIGHT);
	CHECK(h.IsOn(p, 0));
	Pos(p, 0, 0, 10 - CHillState::HEIGHT - 0.1f);
	CHECK(!h.IsOn(p, 0));
	CHECK(!h.IsOn(p, 3));	// no such hill
}

TEST_CASE("hill: scoring and contesting")
{
	CHillState h;
	h.Start(Arena(2), 0);
	uint8_t out[MP_MAX_PLAYERS];
	bool contested;

	SHillPlayer alone[] = { Player(1, 0, 0, 10), Player(2, 50, 0, 10) };
	REQUIRE(h.Scorers(100, false, alone, 2, out, &contested) == 1);
	CHECK(out[0] == 1);
	CHECK(!contested);

	SHillPlayer two[] = { Player(1, 0, 0, 10), Player(2, 1, 1, 10) };
	CHECK(h.Scorers(100, false, two, 2, out, &contested) == 0);
	CHECK(contested);

	SHillPlayer deadRival[] = { Player(1, 0, 0, 10), Player(2, 1, 1, 10, MP_TEAM_NONE, false) };
	CHECK(h.Scorers(100, false, deadRival, 2, out, &contested) == 1);

	// Teams: teammates score together, an enemy blocks.
	SHillPlayer team[] = { Player(1, 0, 0, 10, MP_TEAM_RED), Player(2, 1, 0, 10, MP_TEAM_RED), Player(3, 90, 0, 10, MP_TEAM_BLUE) };
	CHECK(h.Scorers(100, true, team, 3, out, &contested) == 2);
	team[2].pos[0] = 2;
	CHECK(h.Scorers(100, true, team, 3, out, &contested) == 0);
	CHECK(contested);

	// After the move, hill 1 at x=100 is the one that counts.
	CHECK(h.Scorers(CHillState::MOVE_MS, false, alone, 2, out, &contested) == 0);
	CHECK(!contested);
	SHillPlayer there[] = { Player(4, 100, 2, 11) };
	CHECK(h.Scorers(CHillState::MOVE_MS, false, there, 1, out, &contested) == 1);
}

TEST_CASE("stunt points formula")
{
	CHECK(MpStuntPoints(0, 0, 0, 0, 0) == 0);
	CHECK(MpStuntPoints(1000, 0, 0, 0, 0) == 20);		// 1 s in the air
	CHECK(MpStuntPoints(1000, 30, 5, 0, 0) == 60);		// 20 + 30 + 10
	CHECK(MpStuntPoints(1000, 30, 5, 1, 0) == 120);		// one flip doubles it
	CHECK(MpStuntPoints(1000, 30, 5, 0, 1) == 90);		// a spin is worth half
	CHECK(MpStuntPoints(1000, 30, -5, 0, 0) == 50);		// a drop isn't height
	CHECK(MpStuntPoints(60000, 500, 100, 5, 5) == CStuntTracker::MAX_POINTS);
}

namespace {

SStuntSample
Sample(uint32_t dt, bool air, float x, float z, float upAngleDeg = 0.0f, float headingDeg = 0.0f)
{
	SStuntSample s;
	s.dtMs = dt;
	s.airborne = air;
	s.pos[0] = x;
	s.pos[1] = 0.0f;
	s.pos[2] = z;
	// Roll around the y axis by upAngle.
	float a = upAngleDeg * 3.14159265f / 180.0f;
	s.up[0] = sinf(a);
	s.up[1] = 0.0f;
	s.up[2] = cosf(a);
	s.heading = headingDeg * 3.14159265f / 180.0f;
	return s;
}

}

TEST_CASE("stunt tracker: a clean jump")
{
	CStuntTracker t;
	SStuntResult r;
	CHECK(!t.Update(Sample(20, false, 0, 10), r));
	CHECK(!t.InAir());
	// 1.2 s in the air, 36 m forward, 6 m up and back down.
	for(int i = 0; i <= 60; i++){
		float z = 10.0f + 6.0f * sinf(3.14159265f * i / 60.0f);
		CHECK(!t.Update(Sample(20, true, 0.6f * i, z), r));
	}
	CHECK(t.InAir());
	CHECK(t.AirMs() == 1200);
	CHECK(t.PendingPoints() > 0);
	REQUIRE(t.Update(Sample(20, false, 36.6f, 10), r));
	CHECK(!t.InAir());
	CHECK(r.airMs == 1220);
	CHECK(r.distance == doctest::Approx(36.6f));
	CHECK(r.height == doctest::Approx(6.0f).epsilon(0.01));
	CHECK(r.flips == 0);
	CHECK(r.spins == 0);
	CHECK(r.points == MpStuntPoints(1220, 36.6f, r.height, 0, 0));
}

TEST_CASE("stunt tracker: bumps, crashes, flips and spins")
{
	SStuntResult r;
	bool crashed;
	SUBCASE("a short hop doesn't count")
	{
		CStuntTracker t;
		for(int i = 0; i < 20; i++)
			t.Update(Sample(20, true, (float)i, 10), r);
		CHECK(!t.Update(Sample(20, false, 20, 10), r, &crashed));
		CHECK(!crashed);
	}
	SUBCASE("landing on the roof banks nothing")
	{
		CStuntTracker t;
		for(int i = 0; i < 60; i++)
			t.Update(Sample(20, true, (float)i, 12, i * 3.0f), r);
		CHECK(!t.Update(Sample(20, false, 60, 10, 180.0f), r, &crashed));
		CHECK(crashed);
	}
	SUBCASE("a barrel roll")
	{
		CStuntTracker t;
		for(int i = 0; i <= 72; i++)	// 5 degrees a frame: one full turn
			t.Update(Sample(20, true, (float)i, 12, i * 5.0f), r);
		REQUIRE(t.Update(Sample(20, false, 73, 10, 365.0f), r, &crashed));
		CHECK(r.flips == 1);
	}
	SUBCASE("a flat spin")
	{
		CStuntTracker t;
		for(int i = 0; i <= 80; i++)	// 5 degrees of heading a frame: 400 in all
			t.Update(Sample(20, true, (float)i, 12, 0.0f, i * 5.0f), r);
		REQUIRE(t.Update(Sample(20, false, 81, 10, 0.0f, 405.0f), r));
		CHECK(r.spins == 1);
		CHECK(r.flips == 0);
	}
	SUBCASE("reset forgets a stunt in progress")
	{
		CStuntTracker t;
		for(int i = 0; i < 60; i++)
			t.Update(Sample(20, true, (float)i, 12), r);
		t.Reset();
		CHECK(!t.Update(Sample(20, false, 60, 10), r));
	}
}

TEST_CASE("match rules: Hold the Hill and Stunt Challenge scoring")
{
	uint8_t ids[] = { 1, 2, 3, 4 };
	uint8_t teams[4];
	SUBCASE("kills count but don't score")
	{
		CMatchState m;
		m.Start(MATCH_KOTH, ids, teams, 3, 60, 480000, 1000);
		m.OnDeath(2, 1, 2000);
		m.OnDeath(1, MP_PLAYER_NONE, 2000);
		CHECK(m.Get(1)->kills == 1);
		CHECK(m.Get(1)->score == 0);
		CHECK(m.Get(1)->deaths == 1);
		CHECK(m.Get(2)->dead);
		m.OnRespawned(2);
		CHECK(!m.Get(2)->dead);
		m.AddPoints(3, 59, 3000);
		CHECK(!m.IsOver());
		m.AddPoints(3, 1, 3000);
		CHECK(m.IsOver());
		CHECK(m.WinnerId() == 3);
	}
	SUBCASE("points before the start or in deathmatch don't count")
	{
		CMatchState m;
		m.Start(MATCH_KOTH, ids, teams, 2, 60, 480000, 1000);
		m.AddPoints(1, 5, 999);
		CHECK(m.Get(1)->score == 0);
		CMatchState dm;
		dm.Start(MATCH_DM, ids, teams, 2, 20, 600000, 0);
		dm.AddPoints(1, 5, 10);
		CHECK(dm.Get(1)->score == 0);
	}
	SUBCASE("team hill: team kills aren't kills; the team score is the sum")
	{
		CMatchState::AssignTeams(4, teams);
		CMatchState m;
		m.Start(MATCH_TKOTH, ids, teams, 4, 150, 480000, 0);
		CHECK(m.Get(1)->team == MP_TEAM_RED);
		CHECK(m.Get(2)->team == MP_TEAM_BLUE);
		m.OnDeath(3, 1, 10);	// 1 and 3 are both red
		CHECK(m.Get(1)->kills == 0);
		CHECK(m.Get(1)->score == 0);
		m.AddPoints(1, 70, 10);
		m.AddPoints(3, 70, 10);
		CHECK(m.TeamScore(MP_TEAM_RED) == 140);
		CHECK(!m.IsOver());
		m.AddPoints(3, 10, 10);
		CHECK(m.IsOver());
		CHECK(m.WinnerTeam() == MP_TEAM_RED);
	}
	SUBCASE("stunts have no score limit; time decides")
	{
		CMatchState m;
		m.Start(MATCH_STUNT, ids, teams, 2, 0, 240000, 0);
		m.AddPoints(1, 30000, 10);
		m.AddPoints(1, 30000, 10);
		CHECK(m.Get(1)->score == 32767);	// clamped
		CHECK(!m.IsOver());
		m.Update(240000);
		CHECK(m.IsOver());
		CHECK(m.WinnerId() == 1);
	}
}

TEST_CASE("match host: Hold the Hill needs an arena")
{
	CMatchHost host;
	CModeSink sink;
	CHECK(!host.Request(1, MATCH_KOTH, true, 0, &sink));
	REQUIRE(sink.cancels.size() == 1);
	CHECK(sink.cancels[0].reason == MATCH_NO_ARENA);
	CHECK(sink.cancelTargets[0] == 1);
	CHECK(!host.IsBusy());

	SRaceRoute notArena = Arena();
	notArena.type = RACE_SPRINT;
	CHECK(!host.Request(1, MATCH_STUNT, true, 0, &sink, &notArena));
	CHECK(sink.cancels.back().reason == MATCH_NO_ARENA);

	// Deathmatch doesn't need one, but carries it if given.
	SRaceRoute arena = Arena();
	REQUIRE(host.Request(1, MATCH_DM, true, 0, &sink, &arena));
	REQUIRE(sink.invites.size() == 1);
	CHECK(sink.invites[0].hasArena);
	CHECK(strcmp(sink.invites[0].arena.name, "Test Arena") == 0);

	CMatchHost other;
	REQUIRE(other.Request(1, 99, true, 0, &sink));	// unknown mode: deathmatch
	CHECK(sink.invites.back().mode == MATCH_DM);
	CHECK(!sink.invites.back().hasArena);
}

TEST_CASE("match host: a whole Hold the Hill match")
{
	CMatchHost host;
	CModeSink sink;
	SRaceRoute arena = Arena(2);
	REQUIRE(host.Request(1, MATCH_KOTH, true, 0, &sink, &arena));
	const SMatchInvite inv = sink.invites[0];
	CHECK(inv.scoreLimit == CMatchHost::KOTH_SCORE_LIMIT);
	CHECK(inv.timeLimitMs == CMatchHost::KOTH_TIME_LIMIT_MS);
	host.OnAccept(2, inv.matchId, 100);
	host.Update(inv.deadline, &sink);
	REQUIRE(sink.starts.size() == 1);
	uint32_t t = sink.starts[0].startTime;

	float onHill[3] = { 0, 0, 10 }, away[3] = { 50, 0, 10 };
	host.SetPosition(1, onHill);
	host.SetPosition(2, away);
	// Countdown: nothing scores yet.
	for(uint32_t now = inv.deadline; now < t; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(1)->score == 0);
	// 10 s alone on the hill: 10 points.
	for(uint32_t now = t; now <= t + 10000; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(1)->score == 10);
	// Contested for 5 s: nothing.
	host.SetPosition(2, onHill);
	for(uint32_t now = t + 10100; now <= t + 15000; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(1)->score == 10);
	CHECK(host.Match().Get(2)->score == 0);
	// Player 1 dies: 2 holds it alone until 1 respawns.
	host.OnDeath(1, 2, t + 15000);
	for(uint32_t now = t + 15100; now <= t + 20000; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(2)->score == 5);
	CHECK(host.Match().Get(2)->kills == 1);
	host.OnRespawned(1);
	for(uint32_t now = t + 20100; now <= t + 25000; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(2)->score == 5);
	// A hitch longer than 5 s hands out nothing.
	host.SetPosition(1, away);
	host.Update(t + 40000, &sink);
	CHECK(host.Match().Get(2)->score == 5);
	// Hill 0 moves away at one minute; standing on the old spot stops scoring.
	// (t+40100..t+59900 is 19.9 s on the hill: 19 points.)
	for(uint32_t now = t + 40100; now <= t + 60000; now += 100)
		host.Update(now, &sink);
	int16_t atMove = host.Match().Get(2)->score;
	CHECK(atMove == 5 + 19);
	for(uint32_t now = t + 60100; now <= t + 70000; now += 100)
		host.Update(now, &sink);
	CHECK(host.Match().Get(2)->score == atMove);
	// The time limit ends it.
	host.Update(t + CMatchHost::KOTH_TIME_LIMIT_MS, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(sink.ends[0].entries[0].id == 2);
	CHECK(!host.IsBusy());
}

TEST_CASE("match host: stunt reports")
{
	CMatchHost host;
	CModeSink sink;
	SRaceRoute arena = Arena(1);
	REQUIRE(host.Request(1, MATCH_STUNT, true, 0, &sink, &arena));
	const SMatchInvite inv = sink.invites[0];
	CHECK(inv.scoreLimit == 0);
	CHECK(inv.timeLimitMs == CMatchHost::STUNT_TIME_LIMIT_MS);
	host.OnAccept(2, inv.matchId, 100);
	host.Update(inv.deadline, &sink);
	uint32_t t = sink.starts[0].startTime;

	SStunt s;
	s.matchId = inv.matchId;
	s.points = 300;
	host.OnStunt(1, s, t - 1);	// before the start
	CHECK(host.Match().Get(1)->score == 0);
	host.OnStunt(1, s, t + 10);
	CHECK(host.Match().Get(1)->score == 300);
	s.points = 60000;	// capped
	host.OnStunt(2, s, t + 20);
	CHECK(host.Match().Get(2)->score == CStuntTracker::MAX_POINTS);
	s.matchId = (uint16_t)(inv.matchId + 1);	// stale
	host.OnStunt(1, s, t + 30);
	CHECK(host.Match().Get(1)->score == 300);
	host.Update(t + CMatchHost::STUNT_TIME_LIMIT_MS, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(sink.ends[0].entries[0].id == 2);
}
