#pragma once

// Host-side race rules. Racers stamp their own checkpoint crossings with
// session time; the host orders results by those stamps, so a close finish
// matches what each racer saw, whatever order the packets arrive in.

#include <stdint.h>

#include "MpTypes.h"
#include "RaceRoute.h"

enum eRacerState
{
	RACER_RACING,
	RACER_FINISHED,
	RACER_DNF
};

struct SRacer
{
	uint8_t id;
	uint16_t nextSeq;		// next crossing expected (0-based)
	uint32_t lastCrossTime;		// session time of the last accepted crossing
	uint32_t finishTime;		// ms from the start, once FINISHED
	float distToNext;		// to the next checkpoint, set by the host
	uint8_t state;			// eRacerState
};

class CRaceState
{
public:
	static const uint32_t FINISH_GRACE_MS = 60000;		// after the first finish
	static const uint32_t TIME_LIMIT_MS = 20 * 60 * 1000;	// after the start
private:
	SRaceRoute m_route;
	SRacer m_racers[MP_MAX_PLAYERS];
	int m_numRacers;
	uint32_t m_startTime;
	uint16_t m_totalCrossings;
	bool m_anyFinished;
	uint32_t m_firstFinishTime;	// session time

	SRacer *Find(uint8_t id);
public:
	CRaceState(void);

	void Start(const SRaceRoute &route, const uint8_t *ids, int numRacers, uint32_t startTime);
	// Accepts only seq == nextSeq, at or after the start and the racer's last
	// crossing. The last seq finishes the racer.
	bool OnCrossing(uint8_t id, uint16_t seq, uint32_t time);
	void SetDistance(uint8_t id, float d);
	void OnLeft(uint8_t id);
	// Time-outs: FINISH_GRACE_MS after the first finish, TIME_LIMIT_MS after the start.
	void Update(uint32_t now);
	// FINISHED by finishTime (tie: lower id), then RACING by nextSeq desc and
	// distToNext asc (tie: lower id), then DNF by id.
	int GetStandings(uint8_t outIds[MP_MAX_PLAYERS]) const;
	bool IsOver(void) const;
	const SRacer *GetRacer(uint8_t id) const;
	int NumRacers(void) const { return m_numRacers; }
	// 1-based current lap (circuits); 1 for sprints.
	uint8_t LapOf(uint8_t id) const;

	const SRaceRoute &Route(void) const { return m_route; }
	uint32_t StartTime(void) const { return m_startTime; }
	uint16_t TotalCrossings(void) const { return m_totalCrossings; }
};
