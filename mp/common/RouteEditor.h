#pragma once

// The route editor behind the in-game /rec chat commands (Plan 5). Engine-free:
// the game hands it the player's position and does the file and network work
// through IRouteEditorHost, so every command is unit tested.
//
// Commands (the text after "/rec "), checkpoints and grid slots numbered from 1:
//   new NAME | arena NAME | load NAME | name NAME | close
//   type sprint|circuit|arena | laps N | vehicle [MODEL]
//   grid | movegrid N | delgrid N
//   cp [RADIUS] | hill [RADIUS] | ins N [RADIUS] | move N | del N | radius N RADIUS
//   list | undo | save | test

#include <stdint.h>

#include "RaceRoute.h"

// Where the player is when they type a command.
struct SEditorPlace
{
	float pos[3];
	float heading;		// degrees, 0..360
	uint16_t vehicleModel;	// 0 if on foot
};

class IRouteEditorHost
{
public:
	virtual ~IRouteEditorHost(void) {}
	// Finds a route or arena by name. On failure, a reason in err.
	virtual bool LoadRoute(const char *name, SRaceRoute &out, char *err, uint32_t errSize) = 0;
	// route has passed MpValidateRoute. On failure, a reason in err.
	virtual bool SaveRoute(const SRaceRoute &route, char *err, uint32_t errSize) = 0;
	// Ask the session for a race on route (valid, a race type).
	virtual void TestRoute(const SRaceRoute &route) = 0;
};

class CRouteEditor
{
public:
	enum { MAX_UNDO = 16 };
	enum : uint16_t { DEFAULT_RACE_VEHICLE = 141 };	// Infernus
	static const float DEFAULT_CP_RADIUS;		// 8 m
	static const float DEFAULT_HILL_RADIUS;		// 10 m
private:
	bool m_active;
	SRaceRoute m_route;
	SRaceRoute m_undo[MAX_UNDO];
	int m_numUndo;
	bool m_dirty;		// changed since load/save

	void PushUndo(void);
	uint32_t MaxPoints(void) const;
public:
	CRouteEditor(void);

	// Runs one command. here may be nil when the player isn't spawned
	// (commands that need a position then fail). Always writes a one-line
	// reply. Returns false if the command failed.
	bool Command(const char *args, const SEditorPlace *here, IRouteEditorHost *host, char *reply, uint32_t replySize);

	bool IsActive(void) const { return m_active; }
	bool IsDirty(void) const { return m_dirty; }
	const SRaceRoute &Route(void) const { return m_route; }
	void Close(void) { m_active = false; m_numUndo = 0; }
};
