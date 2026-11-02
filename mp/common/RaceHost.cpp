#include "RaceHost.h"

#include <string.h>

static int32_t
TimeDiff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

CRaceHost::CRaceHost(void)
	: m_state(STATE_IDLE), m_raceId(0), m_deadline(0), m_numAccepted(0), m_lastStandingsMs(0)
{
	memset(&m_route, 0, sizeof(m_route));
}

void
CRaceHost::Reset(void)
{
	m_state = STATE_IDLE;
	m_numAccepted = 0;
}

bool
CRaceHost::Request(uint8_t from, const SRaceRoute &route, bool starterRaces, uint32_t now, IRaceHostSink *sink)
{
	if(IsBusy()){
		SRaceCancel busy;
		busy.raceId = m_raceId;
		busy.reason = RACE_BUSY;
		sink->SendCancel(busy, from);
		return false;
	}
	m_state = STATE_INVITING;
	m_raceId++;
	m_route = route;
	m_deadline = now + INVITE_MS;
	m_numAccepted = 0;
	if(starterRaces)
		m_accepted[m_numAccepted++] = from;	// the starter takes pole

	SRaceInvite invite;
	invite.raceId = m_raceId;
	m_startedBy = from;
	GetInvite(invite);
	sink->SendInvite(invite);
	return true;
}

bool
CRaceHost::GetInvite(SRaceInvite &out) const
{
	if(m_state != STATE_INVITING)
		return false;
	out.raceId = m_raceId;
	out.startedBy = m_startedBy;
	out.route = m_route;
	out.deadline = m_deadline;
	return true;
}

void
CRaceHost::OnAccept(uint8_t from, uint16_t raceId, uint32_t now)
{
	if(m_state != STATE_INVITING || raceId != m_raceId || TimeDiff(now, m_deadline) >= 0)
		return;
	for(int i = 0; i < m_numAccepted; i++)
		if(m_accepted[i] == from)
			return;
	if(m_numAccepted < (int)MP_MAX_PLAYERS)
		m_accepted[m_numAccepted++] = from;
}

void
CRaceHost::OnCross(uint8_t from, const SRaceCross &msg)
{
	if(m_state == STATE_RUNNING && msg.raceId == m_raceId)
		m_race.OnCrossing(from, msg.seq, msg.time);
}

void
CRaceHost::SetDistance(uint8_t id, float d)
{
	if(m_state == STATE_RUNNING)
		m_race.SetDistance(id, d);
}

void
CRaceHost::OnLeft(uint8_t id)
{
	if(m_state == STATE_RUNNING)
		m_race.OnLeft(id);
	else if(m_state == STATE_INVITING)
		for(int i = 0; i < m_numAccepted; i++)
			if(m_accepted[i] == id){
				m_accepted[i] = m_accepted[--m_numAccepted];
				break;
			}
}

void
CRaceHost::FillStandings(SRaceStandings &msg) const
{
	uint8_t ids[MP_MAX_PLAYERS];
	int n = m_race.GetStandings(ids);
	msg.raceId = m_raceId;
	msg.n = (uint8_t)n;
	for(int i = 0; i < n; i++){
		const SRacer *r = m_race.GetRacer(ids[i]);
		msg.entries[i].id = r->id;
		msg.entries[i].nextSeq = r->nextSeq;
		msg.entries[i].state = r->state;
		msg.entries[i].finishTime = r->finishTime;
	}
}

void
CRaceHost::Update(uint32_t now, IRaceHostSink *sink)
{
	if(m_state == STATE_INVITING){
		if(TimeDiff(now, m_deadline) < 0)
			return;
		if(m_numAccepted == 0){
			SRaceCancel cancel;
			cancel.raceId = m_raceId;
			cancel.reason = RACE_NO_RACERS;
			sink->SendCancel(cancel, MP_PLAYER_NONE);
			m_state = STATE_IDLE;
			return;
		}
		// No more racers than grid slots.
		int n = m_numAccepted;
		if(n > m_route.numGrid)
			n = m_route.numGrid;
		SRaceStart start;
		start.raceId = m_raceId;
		start.startTime = m_deadline + COUNTDOWN_MS;
		start.n = (uint8_t)n;
		memcpy(start.ids, m_accepted, n);
		m_race.Start(m_route, start.ids, n, start.startTime);
		m_state = STATE_RUNNING;
		m_lastStandingsMs = now;
		sink->SendStart(start);
		return;
	}
	if(m_state != STATE_RUNNING)
		return;
	m_race.Update(now);
	if(m_race.IsOver()){
		SRaceStandings end;
		end.msgType = GMSG_RACE_END;
		FillStandings(end);
		sink->SendEnd(end);
		m_state = STATE_IDLE;
		return;
	}
	if(TimeDiff(now, m_lastStandingsMs) >= (int32_t)STANDINGS_MS){
		m_lastStandingsMs = now;
		SRaceStandings standings;
		FillStandings(standings);
		sink->SendStandings(standings);
	}
}
