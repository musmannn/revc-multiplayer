#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"
#include "RaceRoute.h"

class CByteReader;

// In-game races: invites, grid, countdown, checkpoints, HUD, respawn at the
// last checkpoint and results. The host runs CRaceHost (mp/common); every
// racer detects its own crossings and stamps them with session time.
class CNetRace
{
public:
	// A free CPad::DisablePlayerControls bit (chat 0x200, free-roam menu 0x400).
	enum { PLAYERCONTROL_MP_RACE = 0x800, MAX_ROUTES = 16 };

	// Loads data/mp/races/*.route (listed in data/mp/races/index.txt).
	static void Init(void);
	static void Process(void);
	static void Render(void);
	static bool IsRacing(void);
	static bool HostBusy(void);	// host: a race is pending or running
	static void OnMessage(uint8 from, uint8 type, CByteReader &body);
	static void OnPlayerJoined(uint8 id);
	static void OnPlayerLeft(uint8 id);
	static void Reset(void);

	static int NumRoutes(void);
	static const SRaceRoute *GetRoute(int i);
	static const SRaceRoute *FindRoute(const char *name);
	static void RequestRace(int routeIndex);
	// Asks the host for a race on route, which travels with the request
	// (a loaded file or the route editor's unsaved one).
	static void RequestRoute(const SRaceRoute &route);
	static bool IsLandVehicle(int32 model);
	// Loads data/mp/<dir>/*.route listed in its index.txt: races, or arenas.
	static int LoadRouteDir(const char *dir, SRaceRoute *out, int max, bool arenas);
};

// Reads a small text file (bounded, NUL-terminated). Returns length or -1.
int MpReadTextFile(const char *path, char *buf, int size);

#endif
