#include "PoseBuffer.h"

#include <math.h>

// Signed difference a - b that survives uint32 wraparound.
static int32_t
TimeDiff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

void
CPoseBuffer::Push(const SPose &pose)
{
	if(m_count > 0 && TimeDiff(pose.time, At(m_count - 1).time) <= 0)
		return;
	if(m_count == CAPACITY){
		m_poses[m_head] = pose;
		m_head = (m_head + 1) % CAPACITY;
	}else
		m_poses[(m_head + m_count++) % CAPACITY] = pose;
}

bool
CPoseBuffer::Sample(uint32_t renderTime, SPose &out) const
{
	if(m_count == 0)
		return false;

	const SPose &oldest = At(0);
	const SPose &newest = At(m_count - 1);

	if(TimeDiff(renderTime, oldest.time) <= 0){
		out = oldest;
		return true;
	}

	if(TimeDiff(renderTime, newest.time) >= 0){
		uint32_t dt = (uint32_t)TimeDiff(renderTime, newest.time);
		if(dt > MP_MAX_EXTRAPOLATE_MS)
			dt = MP_MAX_EXTRAPOLATE_MS;
		out = newest;
		for(int i = 0; i < 3; i++)
			out.pos[i] = newest.pos[i] + newest.vel[i] * (float)dt;
		out.time = renderTime;
		return true;
	}

	// oldest < renderTime < newest: find a <= renderTime < b.
	uint32_t i = 0;
	while(i + 1 < m_count && TimeDiff(renderTime, At(i + 1).time) >= 0)
		i++;
	const SPose &a = At(i);
	const SPose &b = At(i + 1);
	float t = (float)TimeDiff(renderTime, a.time) / (float)TimeDiff(b.time, a.time);

	out.time = renderTime;
	for(int k = 0; k < 3; k++){
		out.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
		out.vel[k] = a.vel[k] + (b.vel[k] - a.vel[k]) * t;
	}

	// nlerp along the shortest path: q and -q are the same rotation.
	float dot = a.quat[0]*b.quat[0] + a.quat[1]*b.quat[1] + a.quat[2]*b.quat[2] + a.quat[3]*b.quat[3];
	float sign = dot < 0.0f ? -1.0f : 1.0f;
	float lenSq = 0.0f;
	for(int k = 0; k < 4; k++){
		out.quat[k] = a.quat[k] + (sign * b.quat[k] - a.quat[k]) * t;
		lenSq += out.quat[k] * out.quat[k];
	}
	if(lenSq > 1e-12f){
		float inv = 1.0f / sqrtf(lenSq);
		for(int k = 0; k < 4; k++)
			out.quat[k] *= inv;
	}else{
		for(int k = 0; k < 4; k++)
			out.quat[k] = a.quat[k];
	}
	return true;
}
