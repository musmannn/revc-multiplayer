#include "MatchHost.h"

#include <string.h>

#include "StuntLogic.h"

static int32_t
TimeDiff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

CMatchHost::CMatchHost(void)
	: m_state(STATE_IDLE), m_matchId(0), m_mode(MATCH_DM), m_startedBy(MP_PLAYER_NONE), m_deadline(0),
	  m_numAccepted(0), m_lastScoresMs(0), m_hasArena(false), m_lastHillMs(0)
{
	memset(&m_arena, 0, sizeof(m_arena));
	memset(m_havePos, 0, sizeof(m_havePos));
	memset(m_hillMs, 0, sizeof(m_hillMs));
}

void
CMatchHost::Reset(void)
{
	m_state = STATE_IDLE;
	m_numAccepted = 0;
}

uint16_t
CMatchHost::ScoreLimitFor(uint8_t mode)
{
	switch(mode){
	case MATCH_TDM: return TDM_SCORE_LIMIT;
	case MATCH_KOTH: return KOTH_SCORE_LIMIT;
	case MATCH_TKOTH: return TKOTH_SCORE_LIMIT;
	case MATCH_STUNT: return 0;
	default: return DM_SCORE_LIMIT;
	}
}

uint32_t
CMatchHost::TimeLimitFor(uint8_t mode)
{
	switch(mode){
	case MATCH_KOTH:
	case MATCH_TKOTH: return KOTH_TIME_LIMIT_MS;
	case MATCH_STUNT: return STUNT_TIME_LIMIT_MS;
	default: return TIME_LIMIT_MS;
	}
}

static uint16_t
LimitFor(uint8_t mode)
{
	return CMatchHost::ScoreLimitFor(mode);
}

bool
CMatchHost::Request(uint8_t from, uint8_t mode, bool starterPlays, uint32_t now, IMatchHostSink *sink, const SRaceRoute *arena)
{
	if(IsBusy()){
		SMatchCancel busy;
		busy.matchId = m_matchId;
		busy.reason = MATCH_BUSY;
		sink->SendCancel(busy, from);
		return false;
	}
	if(mode >= MATCH_NUM_MODES)
		mode = MATCH_DM;
	if(arena && (arena->type != RACE_ARENA || !MpValidateRoute(*arena, nullptr, 0)))
		arena = nullptr;
	if(arena == nullptr && MpModeNeedsArena(mode)){
		SMatchCancel cancel;
		cancel.matchId = m_matchId;
		cancel.reason = MATCH_NO_ARENA;
		sink->SendCancel(cancel, from);
		return false;
	}
	m_state = STATE_INVITING;
	m_matchId++;
	m_mode = mode;
	m_hasArena = arena != nullptr;
	if(arena)
		m_arena = *arena;
	else
		memset(&m_arena, 0, sizeof(m_arena));
	m_startedBy = from;
	m_deadline = now + INVITE_MS;
	m_numAccepted = 0;
	if(starterPlays)
		m_accepted[m_numAccepted++] = from;
	SMatchInvite invite;
	GetInvite(invite);
	sink->SendInvite(invite);
	return true;
}

bool
CMatchHost::GetInvite(SMatchInvite &out) const
{
	if(m_state != STATE_INVITING)
		return false;
	out.matchId = m_matchId;
	out.mode = m_mode;
	out.startedBy = m_startedBy;
	out.scoreLimit = LimitFor(m_mode);
	out.timeLimitMs = TimeLimitFor(m_mode);
	out.deadline = m_deadline;
	out.hasArena = m_hasArena;
	out.arena = m_arena;
	return true;
}

void
CMatchHost::OnAccept(uint8_t from, uint16_t matchId, uint32_t now)
{
	if(m_state != STATE_INVITING || matchId != m_matchId || TimeDiff(now, m_deadline) >= 0)
		return;
	for(int i = 0; i < m_numAccepted; i++)
		if(m_accepted[i] == from)
			return;
	if(m_numAccepted < (int)MP_MAX_PLAYERS)
		m_accepted[m_numAccepted++] = from;
}

void
CMatchHost::OnDeath(uint8_t victim, uint8_t killer, uint32_t time)
{
	if(m_state == STATE_RUNNING)
		m_match.OnDeath(victim, killer, time);
}

void
CMatchHost::OnRespawned(uint8_t id)
{
	if(m_state == STATE_RUNNING)
		m_match.OnRespawned(id);
}

void
CMatchHost::SetPosition(uint8_t id, const float pos[3])
{
	if(id >= MP_MAX_PLAYERS)
		return;
	memcpy(m_pos[id], pos, sizeof(m_pos[id]));
	m_havePos[id] = true;
}

void
CMatchHost::OnStunt(uint8_t from, const SStunt &msg, uint32_t now)
{
	if(m_state != STATE_RUNNING || m_mode != MATCH_STUNT || msg.matchId != m_matchId)
		return;
	uint16_t points = msg.points > CStuntTracker::MAX_POINTS ? (uint16_t)CStuntTracker::MAX_POINTS : msg.points;
	m_match.AddPoints(from, points, now);
}

void
CMatchHost::UpdateHill(uint32_t now)
{
	uint32_t dt = now - m_lastHillMs;
	m_lastHillMs = now;
	if((int32_t)(now - m_match.StartTime()) < 0 || (int32_t)dt <= 0 || dt > 5000)
		return;	// not started, or a hitch: don't hand out a burst
	SHillPlayer players[MP_MAX_PLAYERS];
	int n = 0;
	for(int id = 0; id < (int)MP_MAX_PLAYERS; id++){
		const SMatchPlayer *p = m_match.Get((uint8_t)id);
		if(p == nullptr || p->left || !m_havePos[id])
			continue;
		SHillPlayer &h = players[n++];
		h.id = (uint8_t)id;
		h.team = p->team;
		memcpy(h.pos, m_pos[id], sizeof(h.pos));
		h.alive = !p->dead;
	}
	uint8_t scorers[MP_MAX_PLAYERS];
	bool contested;
	int k = m_hill.Scorers(now, MpIsTeamMode(m_mode), players, n, scorers, &contested);
	for(int i = 0; i < k; i++){
		uint8_t id = scorers[i];
		m_hillMs[id] += dt;
		if(m_hillMs[id] >= 1000){
			int points = (int)(m_hillMs[id] / 1000);
			m_hillMs[id] -= (uint32_t)points * 1000;
			m_match.AddPoints(id, points, now);
		}
	}
}

void
CMatchHost::OnLeft(uint8_t id)
{
	if(m_state == STATE_RUNNING)
		m_match.OnLeft(id);
	else if(m_state == STATE_INVITING)
		for(int i = 0; i < m_numAccepted; i++)
			if(m_accepted[i] == id){
				m_accepted[i] = m_accepted[--m_numAccepted];
				break;
			}
}

void
CMatchHost::FillScores(SMatchScores &msg) const
{
	uint8_t ids[MP_MAX_PLAYERS];
	int n = m_match.GetStandings(ids);
	msg.matchId = m_matchId;
	msg.teamScore[MP_TEAM_RED] = m_match.TeamScore(MP_TEAM_RED);
	msg.teamScore[MP_TEAM_BLUE] = m_match.TeamScore(MP_TEAM_BLUE);
	msg.winnerTeam = m_match.WinnerTeam();
	msg.n = (uint8_t)n;
	for(int i = 0; i < n; i++){
		const SMatchPlayer *p = m_match.Get(ids[i]);
		msg.entries[i].id = p->id;
		msg.entries[i].team = p->team;
		msg.entries[i].score = p->score;
		msg.entries[i].kills = p->kills;
		msg.entries[i].deaths = p->deaths;
	}
}

void
CMatchHost::Update(uint32_t now, IMatchHostSink *sink)
{
	if(m_state == STATE_INVITING){
		if(TimeDiff(now, m_deadline) < 0)
			return;
		if(m_numAccepted < 2){
			SMatchCancel cancel;
			cancel.matchId = m_matchId;
			cancel.reason = MATCH_NOT_ENOUGH;
			sink->SendCancel(cancel, MP_PLAYER_NONE);
			m_state = STATE_IDLE;
			return;
		}
		SMatchStart start;
		start.matchId = m_matchId;
		start.mode = m_mode;
		start.scoreLimit = LimitFor(m_mode);
		start.timeLimitMs = TimeLimitFor(m_mode);
		start.startTime = m_deadline + COUNTDOWN_MS;
		start.n = (uint8_t)m_numAccepted;
		for(int i = 0; i < m_numAccepted; i++)
			start.ids[i] = m_accepted[i];
		if(MpIsTeamMode(m_mode))
			CMatchState::AssignTeams(m_numAccepted, start.teams);
		else
			for(int i = 0; i < m_numAccepted; i++)
				start.teams[i] = MP_TEAM_NONE;
		m_match.Start(m_mode, start.ids, start.teams, start.n, start.scoreLimit, start.timeLimitMs, start.startTime);
		m_hill.Start(m_arena, start.startTime);
		memset(m_hillMs, 0, sizeof(m_hillMs));
		m_lastHillMs = now;
		m_state = STATE_RUNNING;
		m_lastScoresMs = now;
		sink->SendStart(start);
		return;
	}
	if(m_state != STATE_RUNNING)
		return;
	if(m_mode == MATCH_KOTH || m_mode == MATCH_TKOTH)
		UpdateHill(now);
	m_match.Update(now);
	if(m_match.IsOver()){
		SMatchScores end;
		end.msgType = GMSG_MATCH_END;
		FillScores(end);
		sink->SendEnd(end);
		m_state = STATE_IDLE;
		return;
	}
	if(TimeDiff(now, m_lastScoresMs) >= (int32_t)SCORES_MS){
		m_lastScoresMs = now;
		SMatchScores scores;
		FillScores(scores);
		sink->SendScores(scores);
	}
}
