#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"

class CVehicle;
class CPed;

// Networked vehicles: net id <-> CVehicle*, ownership, create/remove, state.
//
// The owner is whoever drives it (initially whoever registered it) and is
// the only one sending GMSG_VEHICLE_STATE. Everyone else shows a copy with
// STATUS_PLAYER_DISABLED (no AI, no input) placed from interpolated
// snapshots. The host arbitrates seats (VEHICLE_ENTER/EXIT -> VEHICLE_SEAT).
// Remote players' seating itself follows their PLAYER_STATE, so it heals
// after lost messages and works for late joiners.
class CNetVehicles
{
public:
	enum { MAX_NET_VEHICLES = 64 };

	// The local player started driving an unregistered car (spawned or
	// hijacked): give it an id, keep it from being culled, tell everyone.
	static uint16 Register(CVehicle *vehicle);
	// Removes a vehicle the local player owns, from the world too.
	static void RemoveOwned(CVehicle *vehicle);
	static CVehicle *Find(uint16 id);
	static uint16 IdOf(CVehicle *vehicle);
	static bool IsRemote(CVehicle *vehicle);	// networked and owned by someone else
	// The remote player who owns vehicle (pointer comparison only, so it's
	// safe on a stale pointer), or MP_PLAYER_NONE.
	static uint8 RemoteOwnerOf(const void *vehicle);

	static void OnCreate(uint8 from, const SVehicleCreate &msg);
	static void OnRemove(uint8 from, const SVehicleRemove &msg);
	static void OnState(uint8 from, const SVehicleState &msg);
	static void OnSeat(const SVehicleSeat &msg);
	static void OnEnterRequest(uint8 from, const SVehicleEnter &msg);	// host only
	static void OnExitRequest(uint8 from, const SVehicleExit &msg);		// host only

	// Per frame, from CNetGame::Process: create pending vehicles, place
	// remote ones, detect the local player getting in/out.
	static void Update(uint32 renderTime);
	static void SendOwned(uint32 netNow);

	static void OnPlayerJoined(uint8 player);	// resend our vehicles
	static void RemoveAllOwnedBy(uint8 player);	// the player left the session
	static void Clear(void);			// forget everything (world shutdown)

	// CPed::SetCarJack hook: the local player would drag a remote player out
	// of their car. Instead they get in as a passenger (or nothing happens).
	// Returns true when the jack must not happen.
	static bool OnLocalPlayerWantsToJack(CPed *player, CVehicle *vehicle);

	// Local player's seat for PLAYER_STATE: vehicle id (or MP_VEHICLE_NONE).
	static uint16 LocalVehicleId(uint8 &seat);
	// Out of the current vehicle on the driver's side, no animation.
	static void WarpLocalPlayerOut(void);
};

#endif
