#include "SessionClock.h"

void
CSessionClock::Reset(void)
{
	m_numSamples = 0;
	m_next = 0;
	m_lastRtt = 0;
	m_offset = 0;
	m_isHost = false;
}

void
CSessionClock::SetIsHost(void)
{
	Reset();
	m_isHost = true;
}

void
CSessionClock::AddSample(uint32_t localSendMs, uint32_t hostMs, uint32_t localRecvMs)
{
	// Unsigned arithmetic so local or host time wrapping past 2^32 still works.
	uint32_t rtt = localRecvMs - localSendMs;
	// A pong that "arrived before it was sent" or took absurdly long is bogus.
	if((int32_t)rtt < 0 || rtt > MAX_RTT_MS)
		return;
	SSample &s = m_samples[m_next];
	s.rtt = rtt;
	s.offset = hostMs + rtt / 2 - localRecvMs;
	m_next = (m_next + 1) % MAX_SAMPLES;
	if(m_numSamples < MAX_SAMPLES)
		m_numSamples++;
	m_lastRtt = rtt;

	// The lowest RTT has the least queueing delay, so its offset is the most
	// accurate. Ties go to the newer sample (less drift).
	uint32_t best = 0;
	bool found = false;
	for(uint32_t i = 0; i < m_numSamples; i++){
		// Walk oldest to newest so "<=" prefers the newer sample on ties.
		const SSample &c = m_samples[(m_next + MAX_SAMPLES - m_numSamples + i) % MAX_SAMPLES];
		if(!found || c.rtt <= best){
			best = c.rtt;
			m_offset = c.offset;
			found = true;
		}
	}
}
