#include "HillLogic.h"

#include <math.h>
#include <string.h>

#include "MatchLogic.h"

const float CHillState::HEIGHT = 4.0f;

CHillState::CHillState(void)
	: m_numHills(0), m_startTime(0)
{
	memset(m_hills, 0, sizeof(m_hills));
}

void
CHillState::Start(const SRaceRoute &arena, uint32_t startTime)
{
	m_numHills = arena.numCheckpoints > MP_MAX_HILLS ? (int)MP_MAX_HILLS : arena.numCheckpoints;
	memcpy(m_hills, arena.cps, sizeof(m_hills[0]) * m_numHills);
	m_startTime = startTime;
}

int
CHillState::ActiveHill(uint32_t time) const
{
	if(m_numHills == 0)
		return -1;
	int32_t elapsed = (int32_t)(time - m_startTime);
	if(elapsed < 0)
		return 0;
	return (int)(((uint32_t)elapsed / MOVE_MS) % (uint32_t)m_numHills);
}

uint32_t
CHillState::NextMove(uint32_t time) const
{
	int32_t elapsed = (int32_t)(time - m_startTime);
	if(elapsed < 0)
		return m_startTime + MOVE_MS;
	return m_startTime + ((uint32_t)elapsed / MOVE_MS + 1) * MOVE_MS;
}

bool
CHillState::IsOn(const float pos[3], int hill) const
{
	const SRaceCheckpoint *h = Hill(hill);
	if(h == nullptr)
		return false;
	float dx = pos[0] - h->pos[0], dy = pos[1] - h->pos[1], dz = pos[2] - h->pos[2];
	return dx * dx + dy * dy <= h->radius * h->radius && fabsf(dz) <= HEIGHT;
}

int
CHillState::Scorers(uint32_t time, bool teams, const SHillPlayer *players, int n, uint8_t *out, bool *contested) const
{
	if(contested)
		*contested = false;
	int hill = ActiveHill(time);
	int on = 0;
	uint8_t team = MP_TEAM_NONE;
	bool mixed = false;
	for(int i = 0; i < n; i++){
		const SHillPlayer &p = players[i];
		if(!p.alive || !IsOn(p.pos, hill))
			continue;
		if(on == 0)
			team = p.team;
		else if(p.team != team)
			mixed = true;
		out[on++] = p.id;
	}
	bool blocked = teams ? mixed : on > 1;
	if(blocked){
		if(contested)
			*contested = true;
		return 0;
	}
	return on;
}
