#pragma once

#ifdef MULTIPLAYER

class CVehicle;

// In-game free-roam overlay (F2): vehicles, weapons, health, armour.
class CFreeroamMenu
{
public:
	// A free CPad::DisablePlayerControls bit (see also CNetChat's).
	enum { PLAYERCONTROL_MP_MENU = 0x400 };

	static void Update(void);
	static void Render(void);
	static bool IsOpen(void);
	static void Close(void);
	// Extra root entries for other modes (Plan 3 adds "Start race"). The
	// label must stay valid; action runs with the menu closed.
	static void AddItem(const char *label, void (*action)(void));
	// A root entry opening a list (e.g. "Start race >" with the routes).
	static void AddList(const char *label, int (*count)(void), const char *(*itemLabel)(int), void (*select)(int));

	// Spawns model at pos (ground or water level found here), facing
	// headingRad, warps the local player in as driver and networks it.
	// Returns nil if it couldn't (e.g. a boat on land).
	static CVehicle *SpawnAndEnter(int32 model, const CVector &pos, float headingRad);
	static void Reset(void);
};

#endif
