#pragma once

// Hold the Hill (Plan 5): which hill is active and who's scoring on it.
// Engine-free. The host feeds it the positions it already has; every client
// can work out the active hill itself from the arena and the start time.

#include <stdint.h>

#include "RaceRoute.h"

struct SHillPlayer
{
	uint8_t id;
	uint8_t team;		// MP_TEAM_* (NONE in free for all)
	float pos[3];
	bool alive;
};

class CHillState
{
public:
	enum : uint32_t { MOVE_MS = 60000 };	// the hill moves on this often
	static const float HEIGHT;		// metres above or below the hill point that still count
private:
	SRaceCheckpoint m_hills[MP_MAX_HILLS];
	int m_numHills;
	uint32_t m_startTime;
public:
	CHillState(void);

	// arena: its cp lines are the hills, in order.
	void Start(const SRaceRoute &arena, uint32_t startTime);
	int NumHills(void) const { return m_numHills; }
	// Index of the hill that's up at time (the first one before the start).
	int ActiveHill(uint32_t time) const;
	const SRaceCheckpoint *Hill(int i) const { return i >= 0 && i < m_numHills ? &m_hills[i] : nullptr; }
	// Session time the active hill moves on.
	uint32_t NextMove(uint32_t time) const;
	// Inside the radius horizontally and within HEIGHT vertically.
	bool IsOn(const float pos[3], int hill) const;
	// The players scoring at time: the alive ones on the active hill, if it
	// isn't contested. Free for all: exactly one player. Teams: all on one
	// team. Returns how many ids went to out; contested says why it's 0.
	int Scorers(uint32_t time, bool teams, const SHillPlayer *players, int n, uint8_t *out, bool *contested) const;
};
