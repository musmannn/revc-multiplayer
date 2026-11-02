#include "StuntLogic.h"

#include <math.h>
#include <string.h>

const float CStuntTracker::MIN_LANDING_UP = 0.5f;
const float CStuntTracker::TURN_SLACK = 20.0f;

static const float PI_F = 3.14159265f;

uint16_t
MpStuntPoints(uint32_t airMs, float distance, float height, int flips, int spins)
{
	if(distance < 0.0f)
		distance = 0.0f;
	if(height < 0.0f)
		height = 0.0f;
	float base = (float)airMs / 50.0f + distance + 2.0f * height;
	float mult = 1.0f + (float)flips + (float)spins / 2.0f;
	float points = base * mult;
	if(!(points >= 0.0f))
		return 0;
	if(points > (float)CStuntTracker::MAX_POINTS)
		return (uint16_t)CStuntTracker::MAX_POINTS;
	return (uint16_t)(points + 0.5f);
}

void
CStuntTracker::Reset(void)
{
	m_inAir = false;
	m_airMs = 0;
	memset(m_start, 0, sizeof(m_start));
	memset(m_last, 0, sizeof(m_last));
	m_maxZ = 0.0f;
	m_rotation = 0.0f;
	m_spin = 0.0f;
	m_lastUp[0] = m_lastUp[1] = 0.0f;
	m_lastUp[2] = 1.0f;
	m_lastHeading = 0.0f;
}

static float
Dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float
HorizontalDistance(const float *a, const float *b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1];
	return sqrtf(dx * dx + dy * dy);
}

// Whole turns in degrees, with some slack: a frame-sampled full turn often
// adds up to a little under 360.
static int
Turns(float degrees)
{
	return (int)((degrees + CStuntTracker::TURN_SLACK) / 360.0f);
}

uint16_t
CStuntTracker::PendingPoints(void) const
{
	if(!m_inAir || m_airMs < MIN_AIR_MS)
		return 0;
	return MpStuntPoints(m_airMs, HorizontalDistance(m_last, m_start), m_maxZ - m_start[2],
		Turns(m_rotation), Turns(m_spin));
}

bool
CStuntTracker::Update(const SStuntSample &s, SStuntResult &out, bool *crashed)
{
	if(crashed)
		*crashed = false;
	if(s.airborne){
		if(!m_inAir){
			m_inAir = true;
			m_airMs = 0;
			memcpy(m_start, s.pos, sizeof(m_start));
			m_maxZ = s.pos[2];
			m_rotation = 0.0f;
			m_spin = 0.0f;
		}else{
			m_airMs += s.dtMs;
			float d = Dot(m_lastUp, s.up);
			d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
			m_rotation += acosf(d) * (180.0f / PI_F);
			float dh = s.heading - m_lastHeading;
			while(dh > PI_F)
				dh -= 2.0f * PI_F;
			while(dh < -PI_F)
				dh += 2.0f * PI_F;
			// Only count yaw while roughly upright; tumbling makes heading meaningless.
			if(s.up[2] > 0.5f)
				m_spin += fabsf(dh) * (180.0f / PI_F);
		}
		if(s.pos[2] > m_maxZ)
			m_maxZ = s.pos[2];
		memcpy(m_last, s.pos, sizeof(m_last));
		memcpy(m_lastUp, s.up, sizeof(m_lastUp));
		m_lastHeading = s.heading;
		return false;
	}

	memcpy(m_lastUp, s.up, sizeof(m_lastUp));
	m_lastHeading = s.heading;
	if(!m_inAir)
		return false;
	// Landed.
	m_inAir = false;
	m_airMs += s.dtMs;
	if(m_airMs < MIN_AIR_MS)
		return false;
	if(s.up[2] < MIN_LANDING_UP){
		if(crashed)
			*crashed = true;
		return false;
	}
	out.airMs = (uint16_t)(m_airMs > 65535 ? 65535 : m_airMs);
	out.distance = HorizontalDistance(s.pos, m_start);
	out.height = m_maxZ - m_start[2];
	if(out.height < 0.0f)
		out.height = 0.0f;
	int flips = Turns(m_rotation);
	int spins = Turns(m_spin);
	out.flips = (uint8_t)(flips > 255 ? 255 : flips);
	out.spins = (uint8_t)(spins > 255 ? 255 : spins);
	out.points = MpStuntPoints(m_airMs, out.distance, out.height, flips, spins);
	return out.points > 0;
}
