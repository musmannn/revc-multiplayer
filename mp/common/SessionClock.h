#pragma once

// Estimates the host's clock from ping/pong samples so every client can stamp
// events in shared "session time" (the host's local milliseconds).

#include <stdint.h>

class CSessionClock
{
public:
	enum { MAX_SAMPLES = 16, MIN_SAMPLES = 3, MAX_RTT_MS = 10000 };
private:
	struct SSample
	{
		uint32_t rtt;
		uint32_t offset;	// session = local + offset (mod 2^32)
	};
	SSample m_samples[MAX_SAMPLES];
	uint32_t m_numSamples;
	uint32_t m_next;
	uint32_t m_lastRtt;
	uint32_t m_offset;
	bool m_isHost;
public:
	CSessionClock(void) { Reset(); }

	void Reset(void);
	// The host's session time is its own local time.
	void SetIsHost(void);
	// One ping round trip: sent at localSendMs, host stamped hostMs, received at localRecvMs.
	// Samples with a negative RTT or one above MAX_RTT_MS are ignored.
	void AddSample(uint32_t localSendMs, uint32_t hostMs, uint32_t localRecvMs);
	// Host, or at least MIN_SAMPLES samples.
	bool IsSynced(void) const { return m_isHost || m_numSamples >= MIN_SAMPLES; }
	// Uses the lowest-RTT sample of the last MAX_SAMPLES. Before any sample it returns localMs.
	uint32_t ToSession(uint32_t localMs) const { return m_isHost ? localMs : localMs + m_offset; }
	// RTT of the most recent sample (0 for the host or before any sample).
	uint32_t RttMs(void) const { return m_isHost ? 0 : m_lastRtt; }
};
