#include "MatchLogic.h"

#include <string.h>

static int32_t
TimeDiff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

CMatchState::CMatchState(void)
	: m_mode(MATCH_DM), m_numPlayers(0), m_scoreLimit(0), m_timeLimitMs(0), m_startTime(0), m_over(false)
{
	memset(m_players, 0, sizeof(m_players));
}

void
CMatchState::AssignTeams(int n, uint8_t *outTeams)
{
	for(int i = 0; i < n; i++)
		outTeams[i] = (i % 2 == 0) ? MP_TEAM_RED : MP_TEAM_BLUE;
}

void
CMatchState::Start(uint8_t mode, const uint8_t *ids, const uint8_t *teams, int n, uint16_t scoreLimit, uint32_t timeLimitMs, uint32_t startTime)
{
	m_mode = mode;
	m_scoreLimit = scoreLimit;
	m_timeLimitMs = timeLimitMs;
	m_startTime = startTime;
	m_over = false;
	m_numPlayers = 0;
	for(int i = 0; i < n && m_numPlayers < (int)MP_MAX_PLAYERS; i++){
		if(Find(ids[i]))
			continue;
		SMatchPlayer &p = m_players[m_numPlayers++];
		memset(&p, 0, sizeof(p));
		p.id = ids[i];
		p.team = MpIsTeamMode(mode) ? (teams[i] == MP_TEAM_BLUE ? MP_TEAM_BLUE : MP_TEAM_RED) : MP_TEAM_NONE;
	}
}

SMatchPlayer*
CMatchState::Find(uint8_t id)
{
	for(int i = 0; i < m_numPlayers; i++)
		if(m_players[i].id == id)
			return &m_players[i];
	return nullptr;
}

const SMatchPlayer*
CMatchState::Get(uint8_t id) const
{
	return const_cast<CMatchState*>(this)->Find(id);
}

int16_t
CMatchState::TeamScore(uint8_t team) const
{
	int sum = 0;
	for(int i = 0; i < m_numPlayers; i++)
		if(m_players[i].team == team)
			sum += m_players[i].score;
	return (int16_t)sum;
}

int
CMatchState::PresentOnTeam(uint8_t team) const
{
	int n = 0;
	for(int i = 0; i < m_numPlayers; i++)
		if(!m_players[i].left && (team == MP_TEAM_NONE || m_players[i].team == team))
			n++;
	return n;
}

void
CMatchState::CheckOver(void)
{
	if(m_over)
		return;
	bool limit = m_scoreLimit > 0;
	if(MpIsTeamMode(m_mode)){
		if((limit && (TeamScore(MP_TEAM_RED) >= m_scoreLimit || TeamScore(MP_TEAM_BLUE) >= m_scoreLimit)) ||
		   PresentOnTeam(MP_TEAM_RED) == 0 || PresentOnTeam(MP_TEAM_BLUE) == 0)
			m_over = true;
	}else{
		for(int i = 0; i < m_numPlayers; i++)
			if(limit && m_players[i].score >= m_scoreLimit)
				m_over = true;
		if(PresentOnTeam(MP_TEAM_NONE) < 2)
			m_over = true;
	}
}

void
CMatchState::OnDeath(uint8_t victim, uint8_t killer, uint32_t time)
{
	if(m_over || TimeDiff(time, m_startTime) < 0)
		return;
	SMatchPlayer *v = Find(victim);
	if(v == nullptr || v->left)
		return;
	v->deaths++;
	v->dead = true;
	SMatchPlayer *k = killer != victim ? Find(killer) : nullptr;
	if(k && k->left)
		k = nullptr;	// their id may already belong to a newcomer
	bool deathmatch = m_mode == MATCH_DM || m_mode == MATCH_TDM;
	if(!deathmatch){
		// Kills are on the board but points come from the mode.
		if(k && !(MpIsTeamMode(m_mode) && k->team == v->team))
			k->kills++;
		return;
	}
	if(killer == MP_PLAYER_NONE || killer == victim)
		v->score--;			// suicide, fall, drowning...
	else if(k == nullptr)
		;				// someone outside the match: just a death
	else if(MpIsTeamMode(m_mode) && k->team == v->team)
		k->score--;			// team kill: no kill, a point off
	else{
		k->kills++;
		k->score++;
	}
	CheckOver();
}

void
CMatchState::OnRespawned(uint8_t id)
{
	SMatchPlayer *p = Find(id);
	if(p)
		p->dead = false;
}

void
CMatchState::AddPoints(uint8_t id, int points, uint32_t time)
{
	if(m_over || m_mode == MATCH_DM || m_mode == MATCH_TDM || TimeDiff(time, m_startTime) < 0)
		return;
	SMatchPlayer *p = Find(id);
	if(p == nullptr || p->left || points <= 0)
		return;
	int score = p->score + points;
	p->score = (int16_t)(score > 32767 ? 32767 : score);
	CheckOver();
}

void
CMatchState::OnLeft(uint8_t id)
{
	SMatchPlayer *p = Find(id);
	if(p == nullptr || p->left)
		return;
	p->left = true;
	CheckOver();
}

void
CMatchState::Update(uint32_t now)
{
	if(!m_over && TimeDiff(now, m_startTime) >= (int32_t)m_timeLimitMs)
		m_over = true;
}

// True if a ranks ahead of b inside the same group.
static bool
Ahead(const SMatchPlayer &a, const SMatchPlayer &b)
{
	if(a.score != b.score)
		return a.score > b.score;
	if(a.kills != b.kills)
		return a.kills > b.kills;
	if(a.deaths != b.deaths)
		return a.deaths < b.deaths;
	return a.id < b.id;
}

uint8_t
CMatchState::WinnerTeam(void) const
{
	if(!MpIsTeamMode(m_mode))
		return MP_TEAM_NONE;
	int red = PresentOnTeam(MP_TEAM_RED), blue = PresentOnTeam(MP_TEAM_BLUE);
	if(red == 0 && blue > 0)
		return MP_TEAM_BLUE;
	if(blue == 0 && red > 0)
		return MP_TEAM_RED;
	int16_t r = TeamScore(MP_TEAM_RED), b = TeamScore(MP_TEAM_BLUE);
	if(r == b)
		return MP_TEAM_NONE;
	return r > b ? MP_TEAM_RED : MP_TEAM_BLUE;
}

int
CMatchState::GetStandings(uint8_t outIds[MP_MAX_PLAYERS]) const
{
	// Team order for team DM: the winner (Red on a draw) first.
	uint8_t firstTeam = WinnerTeam() == MP_TEAM_BLUE ? MP_TEAM_BLUE : MP_TEAM_RED;
	const SMatchPlayer *order[MP_MAX_PLAYERS];
	int n = 0;
	for(int i = 0; i < m_numPlayers; i++){
		const SMatchPlayer *p = &m_players[i];
		int j = n++;
		while(j > 0){
			const SMatchPlayer *q = order[j - 1];
			bool ahead;
			if(MpIsTeamMode(m_mode) && p->team != q->team)
				ahead = p->team == firstTeam;
			else
				ahead = Ahead(*p, *q);
			if(!ahead)
				break;
			order[j] = q;
			j--;
		}
		order[j] = p;
	}
	for(int i = 0; i < n; i++)
		outIds[i] = order[i]->id;
	return n;
}

uint8_t
CMatchState::WinnerId(void) const
{
	uint8_t ids[MP_MAX_PLAYERS];
	return GetStandings(ids) > 0 ? ids[0] : MP_PLAYER_NONE;
}
