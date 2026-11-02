#include "doctest.h"

#include <vector>

#include "MatchHost.h"

class CMatchSink : public IMatchHostSink
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

TEST_CASE("match host: full deathmatch")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(1, MATCH_DM, true, 1000, &sink));
	REQUIRE(sink.invites.size() == 1);
	const SMatchInvite &inv = sink.invites[0];
	CHECK(inv.mode == MATCH_DM);
	CHECK(inv.startedBy == 1);
	CHECK(inv.scoreLimit == CMatchHost::DM_SCORE_LIMIT);
	CHECK(inv.timeLimitMs == CMatchHost::TIME_LIMIT_MS);
	CHECK(inv.deadline == 1000 + CMatchHost::INVITE_MS);

	host.OnAccept(2, inv.matchId, 2000);
	host.OnAccept(3, (uint16_t)(inv.matchId + 1), 2000);	// stale
	host.Update(inv.deadline, &sink);
	REQUIRE(sink.starts.size() == 1);
	const SMatchStart &st = sink.starts[0];
	CHECK(st.mode == MATCH_DM);
	CHECK(st.startTime == inv.deadline + CMatchHost::COUNTDOWN_MS);
	REQUIRE(st.n == 2);
	CHECK(st.ids[0] == 1);
	CHECK(st.ids[1] == 2);
	CHECK(st.teams[0] == MP_TEAM_NONE);
	CHECK(host.IsRunning());

	uint32_t t = st.startTime;
	host.OnDeath(2, 1, t + 100);
	host.Update(t + 600, &sink);
	REQUIRE(!sink.scores.empty());
	CHECK(sink.scores.back().entries[0].id == 1);
	CHECK(sink.scores.back().entries[0].score == 1);
	for(int i = 1; i < (int)CMatchHost::DM_SCORE_LIMIT; i++)
		host.OnDeath(2, 1, t + 1000 + i);
	host.Update(t + 2000, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(sink.ends[0].msgType == GMSG_MATCH_END);
	CHECK(sink.ends[0].entries[0].id == 1);
	CHECK(sink.ends[0].entries[0].kills == CMatchHost::DM_SCORE_LIMIT);
	CHECK(sink.ends[0].entries[1].deaths == CMatchHost::DM_SCORE_LIMIT);
	CHECK(!host.IsBusy());
}

TEST_CASE("match host: team deathmatch assigns teams")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(0, MATCH_TDM, true, 0, &sink));
	uint16_t id = sink.invites[0].matchId;
	CHECK(sink.invites[0].scoreLimit == CMatchHost::TDM_SCORE_LIMIT);
	host.OnAccept(4, id, 1);
	host.OnAccept(2, id, 2);
	host.OnAccept(5, id, 3);
	host.Update(CMatchHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	const SMatchStart &st = sink.starts[0];
	REQUIRE(st.n == 4);
	CHECK(st.teams[0] == MP_TEAM_RED);
	CHECK(st.teams[1] == MP_TEAM_BLUE);
	CHECK(st.teams[2] == MP_TEAM_RED);
	CHECK(st.teams[3] == MP_TEAM_BLUE);

	uint32_t t = st.startTime;
	host.Update(t + 600, &sink);
	REQUIRE(!sink.scores.empty());
	CHECK(sink.scores.back().winnerTeam == MP_TEAM_NONE);	// 0-0
	host.OnDeath(4, 0, t + 1);	// red 0 kills blue 4
	host.Update(t + 1200, &sink);
	CHECK(sink.scores.back().teamScore[MP_TEAM_RED] == 1);
	CHECK(sink.scores.back().teamScore[MP_TEAM_BLUE] == 0);
	CHECK(sink.scores.back().winnerTeam == MP_TEAM_RED);
	// Red empties out while ahead: Blue wins by forfeit.
	host.OnLeft(0);
	host.OnLeft(2);
	host.Update(t + 1300, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(sink.ends[0].winnerTeam == MP_TEAM_BLUE);
}

TEST_CASE("match host: not enough players")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(0, MATCH_DM, true, 0, &sink));
	host.Update(CMatchHost::INVITE_MS, &sink);
	REQUIRE(sink.cancels.size() == 1);
	CHECK(sink.cancels[0].reason == MATCH_NOT_ENOUGH);
	CHECK(sink.cancelTargets[0] == MP_PLAYER_NONE);
	CHECK(!host.IsBusy());
	CHECK(sink.starts.empty());
}

TEST_CASE("match host: busy")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(0, MATCH_DM, true, 0, &sink));
	CHECK(!host.Request(3, MATCH_TDM, true, 10, &sink));
	REQUIRE(sink.cancels.size() == 1);
	CHECK(sink.cancels[0].reason == MATCH_BUSY);
	CHECK(sink.cancelTargets[0] == 3);
}

TEST_CASE("match host: a leaver can end it, and late joiners get the invite")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(1, MATCH_DM, true, 0, &sink));
	SMatchInvite again;
	REQUIRE(host.GetInvite(again));
	CHECK(again.matchId == sink.invites[0].matchId);
	host.OnAccept(2, again.matchId, 5);
	host.Update(CMatchHost::INVITE_MS, &sink);
	REQUIRE(sink.starts.size() == 1);
	CHECK(!host.GetInvite(again));
	host.OnLeft(2);
	host.Update(sink.starts[0].startTime + 10, &sink);
	REQUIRE(sink.ends.size() == 1);
	CHECK(!host.IsBusy());
}

TEST_CASE("match host: deaths are only counted while running")
{
	CMatchHost host;
	CMatchSink sink;
	REQUIRE(host.Request(1, MATCH_DM, true, 0, &sink));
	host.OnDeath(2, 1, 100);	// still inviting: ignored, no crash
	host.OnAccept(2, sink.invites[0].matchId, 5);
	host.Update(CMatchHost::INVITE_MS, &sink);
	host.Update(sink.starts[0].startTime + 600, &sink);
	REQUIRE(!sink.scores.empty());
	CHECK(sink.scores.back().entries[0].score == 0);
}
