#pragma once

// Buffers timestamped snapshots of a remote entity and samples them at a
// render time slightly in the past (MP_INTERP_DELAY_MS), so movement looks
// smooth despite jitter, loss and reordering.
//
// Units: pos in game units, vel in game units per MILLISECOND, quat is
// (x, y, z, w), time in session milliseconds.

#include <stdint.h>

#include "MpTypes.h"

struct SPose
{
	uint32_t time;
	float pos[3];
	float quat[4];	// x, y, z, w
	float vel[3];	// units per millisecond
};

class CPoseBuffer
{
public:
	enum { CAPACITY = 16 };
private:
	SPose m_poses[CAPACITY];
	uint32_t m_count;
	uint32_t m_head;	// index of the oldest pose

	const SPose &At(uint32_t i) const { return m_poses[(m_head + i) % CAPACITY]; }
public:
	CPoseBuffer(void) : m_count(0), m_head(0) {}

	void Clear(void) { m_count = 0; m_head = 0; }
	// Ignores a pose whose time is not newer than the newest stored one.
	// When full, the oldest pose is dropped.
	void Push(const SPose &pose);
	// Between two poses: lerp pos/vel, nlerp quat (shortest path).
	// After the newest: pos += vel*dt with dt capped at MP_MAX_EXTRAPOLATE_MS.
	// Before the oldest: the oldest. Returns false with no poses.
	bool Sample(uint32_t renderTime, SPose &out) const;
	uint32_t Count(void) const { return m_count; }
};
