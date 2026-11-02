#include "doctest.h"

#include <string.h>

#include "MatchLogic.h"

static const uint32_t START = 5000;
static const uint8_t N = MP_PLAYER_NONE;

static void
StartDm(CMatchState &m, int players = 3, uint16_t limit = 3)
{
	uint8_t ids[MP_MAX_PLAYERS];
	uint8_t teams[MP_MAX_PLAYERS];
	for(int i = 0; i < players; i++){
		ids[i] = (uint8_t)i;
		teams[i] = MP_TEAM_NONE;
	}
	m.Start(MATCH_DM, ids, teams, players, limit, 600000, START);
}

static void
StartTdm(CMatchState &m, uint16_t limit = 3)
{
	// 0,2 red; 1,3 blue
	const uint8_t ids[4] = { 0, 1, 2, 3 };
	uint8_t teams[4];
	CMatchState::AssignTeams(4, teams);
	m.Start(MATCH_TDM, ids, teams, 4, limit, 600000, START);
}

TEST_CASE("match: kills and deaths")
{
	CMatchState m;
	StartDm(m);
	CHECK(m.NumPlayers() == 3);
	m.OnDeath(1, 0, START + 100);
	m.OnDeath(2, 0, START + 200);
	const SMatchPlayer *p0 = m.Get(0);
	const SMatchPlayer *p1 = m.Get(1);
	REQUIRE(p0);
	REQUIRE(p1);
	CHECK(p0->kills == 2);
	CHECK(p0->score == 2);
	CHECK(p0->deaths == 0);
	CHECK(p1->deaths == 1);
	CHECK(p1->score == 0);
	CHECK(!m.IsOver());
}

TEST_CASE("match: suicide and no killer cost a point")
{
	CMatchState m;
	StartDm(m);
	m.OnDeath(1, 1, START + 100);	// suicide
	m.OnDeath(1, N, START + 200);	// fell off something
	CHECK(m.Get(1)->score == -2);
	CHECK(m.Get(1)->deaths == 2);
	CHECK(m.Get(1)->kills == 0);
}

TEST_CASE("match: a killer outside the match changes no score")
{
	CMatchState m;
	StartDm(m);
	m.OnDeath(1, 7, START + 100);
	CHECK(m.Get(1)->score == 0);
	CHECK(m.Get(1)->deaths == 1);
	m.OnDeath(7, 1, START + 200);	// a non-player's death doesn't count at all
	CHECK(m.Get(1)->kills == 0);
	CHECK(m.Get(7) == nullptr);
}

TEST_CASE("match: deaths before the start or after the end are ignored")
{
	CMatchState m;
	StartDm(m, 3, 1);
	m.OnDeath(1, 0, START - 1);
	CHECK(m.Get(0)->kills == 0);
	m.OnDeath(1, 0, START);
	CHECK(m.IsOver());
	m.OnDeath(2, 1, START + 10);
	CHECK(m.Get(1)->kills == 0);
}

TEST_CASE("match: kill limit ends deathmatch")
{
	CMatchState m;
	StartDm(m, 3, 3);
	m.OnDeath(1, 2, START + 1);
	m.OnDeath(0, 2, START + 2);
	CHECK(!m.IsOver());
	m.OnDeath(1, 2, START + 3);
	CHECK(m.IsOver());
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(m.GetStandings(ids) == 3);
	CHECK(ids[0] == 2);
	CHECK(m.WinnerId() == 2);
}

TEST_CASE("match: time limit")
{
	CMatchState m;
	StartDm(m);
	m.Update(START + 600000 - 1);
	CHECK(!m.IsOver());
	m.Update(START + 600000);
	CHECK(m.IsOver());
}

TEST_CASE("match: deathmatch standings")
{
	CMatchState m;
	StartDm(m, 4, 20);
	m.OnDeath(0, 3, START + 1);	// 3: 1 kill
	m.OnDeath(1, 2, START + 2);	// 2: 1 kill
	m.OnDeath(2, 2, START + 3);	// 2: suicide -> score 0, kills 1
	m.OnDeath(3, 1, START + 4);	// 1: 1 kill, 3 has 1 death
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(m.GetStandings(ids) == 4);
	// score: 3=1 (1 death), 1=1 (1 death), 2=0 (1 kill), 0=0 (0 kills, 1 death)
	CHECK(ids[0] == 1);	// tie with 3 on score, kills and deaths: lower id
	CHECK(ids[1] == 3);
	CHECK(ids[2] == 2);	// more kills than 0
	CHECK(ids[3] == 0);
}

TEST_CASE("match: teams alternate")
{
	uint8_t teams[5];
	CMatchState::AssignTeams(5, teams);
	CHECK(teams[0] == MP_TEAM_RED);
	CHECK(teams[1] == MP_TEAM_BLUE);
	CHECK(teams[2] == MP_TEAM_RED);
	CHECK(teams[3] == MP_TEAM_BLUE);
	CHECK(teams[4] == MP_TEAM_RED);
}

TEST_CASE("match: team deathmatch scoring, team kills, limit")
{
	CMatchState m;
	StartTdm(m, 3);
	m.OnDeath(1, 0, START + 1);	// red kills blue
	m.OnDeath(3, 2, START + 2);	// red kills blue
	CHECK(m.TeamScore(MP_TEAM_RED) == 2);
	m.OnDeath(2, 0, START + 3);	// team kill: 0 loses a point, no kill
	CHECK(m.Get(0)->score == 0);
	CHECK(m.Get(0)->kills == 1);
	CHECK(m.Get(2)->deaths == 1);
	CHECK(m.TeamScore(MP_TEAM_RED) == 1);
	m.OnDeath(0, 1, START + 4);	// blue kills red
	CHECK(m.TeamScore(MP_TEAM_BLUE) == 1);
	m.OnDeath(1, 2, START + 5);
	m.OnDeath(3, 2, START + 6);
	CHECK(m.TeamScore(MP_TEAM_RED) == 3);
	CHECK(m.IsOver());
	CHECK(m.WinnerTeam() == MP_TEAM_RED);
	uint8_t ids[MP_MAX_PLAYERS];
	REQUIRE(m.GetStandings(ids) == 4);
	// Winning team first, by score inside the team.
	CHECK(m.Get(ids[0])->team == MP_TEAM_RED);
	CHECK(m.Get(ids[1])->team == MP_TEAM_RED);
	CHECK(ids[0] == 2);
	CHECK(m.Get(ids[2])->team == MP_TEAM_BLUE);
}

TEST_CASE("match: team draw at the time limit")
{
	CMatchState m;
	StartTdm(m, 10);
	m.OnDeath(1, 0, START + 1);
	m.OnDeath(0, 1, START + 2);
	m.Update(START + 600000);
	CHECK(m.IsOver());
	CHECK(m.WinnerTeam() == MP_TEAM_NONE);
}

TEST_CASE("match: leavers stay on the board; too few players ends it")
{
	CMatchState m;
	StartDm(m, 3, 20);
	m.OnDeath(1, 0, START + 1);
	m.OnLeft(0);
	CHECK(!m.IsOver());
	REQUIRE(m.Get(0));
	CHECK(m.Get(0)->left);
	CHECK(m.Get(0)->score == 1);
	m.OnDeath(0, 1, START + 2);	// a leaver can't die any more
	CHECK(m.Get(0)->deaths == 0);
	m.OnLeft(2);
	CHECK(m.IsOver());	// only player 1 left
}

TEST_CASE("match: an empty team ends team deathmatch")
{
	CMatchState m;
	StartTdm(m, 30);
	m.OnLeft(1);
	CHECK(!m.IsOver());
	m.OnLeft(3);
	CHECK(m.IsOver());
	CHECK(m.WinnerTeam() == MP_TEAM_RED);	// the team that's still there
}

TEST_CASE("match: a killer who left is an outsider (ids get reused)")
{
	CMatchState m;
	StartDm(m, 3, 20);
	m.OnLeft(0);
	m.OnDeath(1, 0, START + 1);	// a newcomer with id 0
	CHECK(m.Get(0)->kills == 0);
	CHECK(m.Get(1)->deaths == 1);
	CHECK(m.Get(1)->score == 0);
}
