#pragma once

// Race routes: an engine-free description plus a parser for .route files.
//
// File format (text, '#' comments, one directive per line, whitespace-separated):
//   name Ocean Drive Sprint
//   type sprint            # or circuit, or arena
//   laps 1                 # required for circuit (1..9); sprint forces 1
//   vehicle 132
//   grid  X Y Z HEADING    # 1..8 lines, slot order = grid order (more are ignored)
//   cp    X Y Z RADIUS     # 2..64 lines, driving order; the last cp is the finish
//                          # line (and, for a circuit, the lap line)
//
// An arena (type arena) is a place for a match instead of a race: its grid
// lines are spawn points (1..8), its cp lines are hill points (1..16) and its
// vehicle line is optional (0 = none; the stunt mode then uses its default).

#include <stdint.h>

#include "MpTypes.h"

static const uint32_t MP_MAX_CHECKPOINTS = 64;
static const uint32_t MP_MAX_ROUTE_NAME = 31;
static const uint32_t MP_MAX_HILLS = 16;		// cp lines in an arena
// Positions must be within this many metres of the origin on every axis (the
// network encoding's range; Vice City fits with room to spare).
static const float MP_ROUTE_MAX_COORD = 4000.0f;
static const float MP_ROUTE_MAX_RADIUS = 120.0f;

enum eRaceType
{
	RACE_SPRINT,
	RACE_CIRCUIT,
	RACE_ARENA		// not a race: spawns and hill points for a match
};

struct SRaceGridSlot
{
	float pos[3];
	float heading;	// degrees, as authored
};

struct SRaceCheckpoint
{
	float pos[3];
	float radius;
};

struct SRaceRoute
{
	char name[MP_MAX_ROUTE_NAME + 1];
	uint8_t type;		// eRaceType
	uint8_t laps;
	uint16_t vehicleModel;
	uint8_t numGrid;
	SRaceGridSlot grid[MP_MAX_PLAYERS];
	uint8_t numCheckpoints;
	SRaceCheckpoint cps[MP_MAX_CHECKPOINTS];
};

// Parses text into out. On failure returns false and writes a reason to err
// starting with one of: "missing name", "bad type", "bad laps", "bad vehicle",
// "no grid", "too few checkpoints", "too many checkpoints", "too many hills",
// "bad position", "line N: bad number", "line N: unknown directive". On
// success err is "". The result has passed MpValidateRoute.
bool MpParseRoute(const char *text, SRaceRoute &out, char *err, uint32_t errSize);

// The checks MpParseRoute ends with, for routes built some other way (the
// editor, the network). Same reasons as above, without the line ones.
bool MpValidateRoute(const SRaceRoute &route, char *err, uint32_t errSize);

// Writes route as .route text that MpParseRoute reads back to the same route
// (numbers to 2 decimals, headings to 1). Returns the length, or -1 if it
// doesn't fit in size.
int MpWriteRoute(const SRaceRoute &route, char *out, uint32_t size);

static inline bool MpIsArena(const SRaceRoute &route) { return route.type == RACE_ARENA; }

// Reads and parses a .route file with stdio (tools and the server; the game
// goes through CFileMgr). On failure a reason in err.
bool MpLoadRouteFile(const char *path, SRaceRoute &out, char *err, uint32_t errSize);

// Checkpoint crossings needed to finish: numCheckpoints, times laps for a circuit.
uint16_t MpRouteTotalCrossings(const SRaceRoute &route);
