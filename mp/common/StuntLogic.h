#pragma once

// Stunt Challenge scoring (Plan 5). Engine-free: the stunter's game feeds it
// one sample per frame (is the vehicle off the ground, where is it, which way
// is up) and it says when a stunt has landed and what it scored.

#include <stdint.h>

struct SStuntSample
{
	uint32_t dtMs;		// since the last sample
	bool airborne;		// no wheel on the ground
	float pos[3];
	float up[3];		// the vehicle's up axis, unit length
	float heading;		// radians
};

struct SStuntResult
{
	uint16_t points;
	uint16_t airMs;
	uint8_t flips;
	uint8_t spins;
	float distance;		// horizontal metres
	float height;		// metres above the take-off point at the highest
};

// (airMs / 50 + distance + 2 x height) x (1 + flips + spins / 2), capped.
uint16_t MpStuntPoints(uint32_t airMs, float distance, float height, int flips, int spins);

class CStuntTracker
{
public:
	enum : uint32_t { MIN_AIR_MS = 700, MAX_POINTS = 2000 };
	static const float MIN_LANDING_UP;	// up.z at landing: 0.5 = within 60 degrees of upright
	static const float TURN_SLACK;		// degrees short of 360 that still make a flip or spin
private:
	bool m_inAir;
	uint32_t m_airMs;
	float m_start[3];
	float m_last[3];
	float m_maxZ;
	float m_rotation;	// degrees the up axis has turned through
	float m_spin;		// degrees of heading turned
	float m_lastUp[3];
	float m_lastHeading;
public:
	CStuntTracker(void) { Reset(); }

	// Forget any stunt in progress (respawn, teleport, new car).
	void Reset(void);
	// One frame. Returns true when a stunt has just landed cleanly and is
	// worth points; then out is filled. crashed (optional) is set when a
	// stunt that would have counted ended upside down or on its side.
	bool Update(const SStuntSample &s, SStuntResult &out, bool *crashed = nullptr);
	bool InAir(void) const { return m_inAir; }
	uint32_t AirMs(void) const { return m_airMs; }
	// Points if it landed cleanly now (for the HUD).
	uint16_t PendingPoints(void) const;
};
