#include "RaceLogic.h"

#include <string.h>

// a - b in session time, surviving uint32 wraparound.
static int32_t
TimeDiff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

CRaceState::CRaceState(void)
	: m_numRacers(0), m_startTime(0), m_totalCrossings(0), m_anyFinished(false), m_firstFinishTime(0)
{
	memset(&m_route, 0, sizeof(m_route));
	memset(m_racers, 0, sizeof(m_racers));
}

void
CRaceState::Start(const SRaceRoute &route, const uint8_t *ids, int numRacers, uint32_t startTime)
{
	m_route = route;
	m_startTime = startTime;
	m_totalCrossings = MpRouteTotalCrossings(route);
	m_anyFinished = false;
	m_firstFinishTime = 0;
	m_numRacers = 0;
	for(int i = 0; i < numRacers && m_numRacers < (int)MP_MAX_PLAYERS; i++){
		if(Find(ids[i]))
			continue;	// listed twice
		SRacer &r = m_racers[m_numRacers++];
		r.id = ids[i];
		r.nextSeq = 0;
		r.lastCrossTime = startTime;
		r.finishTime = 0;
		r.distToNext = 0.0f;
		r.state = RACER_RACING;
	}
}

SRacer*
CRaceState::Find(uint8_t id)
{
	for(int i = 0; i < m_numRacers; i++)
		if(m_racers[i].id == id)
			return &m_racers[i];
	return nullptr;
}

const SRacer*
CRaceState::GetRacer(uint8_t id) const
{
	return const_cast<CRaceState*>(this)->Find(id);
}

bool
CRaceState::OnCrossing(uint8_t id, uint16_t seq, uint32_t time)
{
	SRacer *r = Find(id);
	if(r == nullptr || r->state != RACER_RACING || seq != r->nextSeq)
		return false;
	if(TimeDiff(time, m_startTime) < 0 || TimeDiff(time, r->lastCrossTime) < 0)
		return false;
	r->nextSeq++;
	r->lastCrossTime = time;
	if(r->nextSeq >= m_totalCrossings){
		r->state = RACER_FINISHED;
		r->finishTime = time - m_startTime;
		if(!m_anyFinished || TimeDiff(time, m_firstFinishTime) < 0)
			m_firstFinishTime = time;
		m_anyFinished = true;
	}
	return true;
}

void
CRaceState::SetDistance(uint8_t id, float d)
{
	SRacer *r = Find(id);
	if(r)
		r->distToNext = d;
}

void
CRaceState::OnLeft(uint8_t id)
{
	SRacer *r = Find(id);
	if(r && r->state == RACER_RACING)
		r->state = RACER_DNF;
}

void
CRaceState::Update(uint32_t now)
{
	bool timeUp = TimeDiff(now, m_startTime) >= (int32_t)TIME_LIMIT_MS ||
		(m_anyFinished && TimeDiff(now, m_firstFinishTime) >= (int32_t)FINISH_GRACE_MS);
	if(!timeUp)
		return;
	for(int i = 0; i < m_numRacers; i++)
		if(m_racers[i].state == RACER_RACING)
			m_racers[i].state = RACER_DNF;
}

// True if a ranks ahead of b.
static bool
Ahead(const SRacer &a, const SRacer &b)
{
	if(a.state != b.state){
		// FINISHED < RACING < DNF in the enum's sense of rank.
		static const int rank[3] = { 1, 0, 2 };	// RACING, FINISHED, DNF
		return rank[a.state] < rank[b.state];
	}
	switch(a.state){
	case RACER_FINISHED:
		if(a.finishTime != b.finishTime)
			return a.finishTime < b.finishTime;
		break;
	case RACER_RACING:
		if(a.nextSeq != b.nextSeq)
			return a.nextSeq > b.nextSeq;
		if(a.distToNext != b.distToNext)
			return a.distToNext < b.distToNext;
		break;
	default:
		break;
	}
	return a.id < b.id;
}

int
CRaceState::GetStandings(uint8_t outIds[MP_MAX_PLAYERS]) const
{
	// Insertion sort; at most 8 racers.
	const SRacer *order[MP_MAX_PLAYERS];
	int n = 0;
	for(int i = 0; i < m_numRacers; i++){
		const SRacer *r = &m_racers[i];
		int j = n++;
		while(j > 0 && Ahead(*r, *order[j - 1])){
			order[j] = order[j - 1];
			j--;
		}
		order[j] = r;
	}
	for(int i = 0; i < n; i++)
		outIds[i] = order[i]->id;
	return n;
}

bool
CRaceState::IsOver(void) const
{
	for(int i = 0; i < m_numRacers; i++)
		if(m_racers[i].state == RACER_RACING)
			return false;
	return true;
}

uint8_t
CRaceState::LapOf(uint8_t id) const
{
	const SRacer *r = GetRacer(id);
	if(r == nullptr || m_route.type != RACE_CIRCUIT || m_route.numCheckpoints == 0)
		return 1;
	int lap = 1 + r->nextSeq / m_route.numCheckpoints;
	if(lap > m_route.laps)
		lap = m_route.laps;
	return (uint8_t)lap;
}
