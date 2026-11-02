#pragma once

// Seat arbitration for a dedicated server (Plan 5): the same first-come,
// first-served rule the host's game applies in CNetVehicles, from the vehicle
// traffic the server sees. Engine-free.

#include <stdint.h>

#include "GameProtocol.h"

class CSeatTable
{
public:
	enum { MAX_VEHICLES = 512 };
private:
	struct SEntry
	{
		uint16_t id;		// MP_VEHICLE_NONE = free
		uint8_t seats[MP_MAX_SEATS];
	};
	SEntry m_entries[MAX_VEHICLES];
	uint32_t m_next;	// where the next new vehicle goes when full

	SEntry *Find(uint16_t id);
	SEntry *Add(uint16_t id);
	void ClearPlayer(uint8_t player);
public:
	CSeatTable(void) { Clear(); }

	void Clear(void);
	void OnCreate(uint16_t id);
	void OnRemove(uint16_t id);
	// A player asks for a seat. True: they have it now (the caller broadcasts
	// the accept). False: someone else has it, or the seat doesn't exist.
	// A vehicle we never heard of is taken on trust.
	bool OnEnter(uint8_t from, uint16_t id, uint8_t seat);
	// The player left the vehicle (or the session: then pass MP_VEHICLE_NONE
	// to clear them from every vehicle).
	void OnExit(uint8_t from, uint16_t id);
	uint8_t Occupant(uint16_t id, uint8_t seat) const;	// MP_PLAYER_NONE if free or unknown
	bool Knows(uint16_t id) const;
	int Count(void) const;
};
