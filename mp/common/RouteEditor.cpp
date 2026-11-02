#include "RouteEditor.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const float CRouteEditor::DEFAULT_CP_RADIUS = 8.0f;
const float CRouteEditor::DEFAULT_HILL_RADIUS = 10.0f;

static bool
Reply(char *out, uint32_t size, bool ok, const char *fmt, ...)
{
	if(out && size){
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(out, size, fmt, ap);
		va_end(ap);
	}
	return ok;
}

static void
CopyName(char *dst, const char *src)
{
	while(*src == ' ')
		src++;
	strncpy(dst, src, MP_MAX_ROUTE_NAME);
	dst[MP_MAX_ROUTE_NAME] = '\0';
	size_t n = strlen(dst);
	while(n > 0 && dst[n - 1] == ' ')
		dst[--n] = '\0';
}

// Strict positive integer; 0 on anything else.
static long
ParseIndex(const char *s)
{
	if(s == nullptr)
		return 0;
	char *end;
	long v = strtol(s, &end, 10);
	return end != s && *end == '\0' && v > 0 ? v : 0;
}

static bool
ParseRadius(const char *s, float def, float &out)
{
	if(s == nullptr){
		out = def;
		return true;
	}
	char *end;
	double v = strtod(s, &end);
	if(end == s || *end != '\0' || !(v > 0.0) || v > MP_ROUTE_MAX_RADIUS)
		return false;
	out = (float)v;
	return true;
}

static const char*
TypeName(uint8_t type)
{
	return type == RACE_CIRCUIT ? "circuit" : type == RACE_ARENA ? "arena" : "sprint";
}

static const char*
PointWord(uint8_t type)
{
	return type == RACE_ARENA ? "hill" : "checkpoint";
}

static const char*
SlotWord(uint8_t type)
{
	return type == RACE_ARENA ? "spawn" : "grid slot";
}

CRouteEditor::CRouteEditor(void)
	: m_active(false), m_numUndo(0), m_dirty(false)
{
	memset(&m_route, 0, sizeof(m_route));
}

void
CRouteEditor::PushUndo(void)
{
	if(m_numUndo == MAX_UNDO){
		memmove(&m_undo[0], &m_undo[1], sizeof(m_undo[0]) * (MAX_UNDO - 1));
		m_numUndo--;
	}
	m_undo[m_numUndo++] = m_route;
	m_dirty = true;
}

uint32_t
CRouteEditor::MaxPoints(void) const
{
	return m_route.type == RACE_ARENA ? MP_MAX_HILLS : MP_MAX_CHECKPOINTS;
}

static void
Begin(SRaceRoute &r, const char *name, uint8_t type, const SEditorPlace *here)
{
	memset(&r, 0, sizeof(r));
	CopyName(r.name, name);
	r.type = type;
	r.laps = 1;
	if(type != RACE_ARENA)
		r.vehicleModel = here && here->vehicleModel ? here->vehicleModel : (uint16_t)CRouteEditor::DEFAULT_RACE_VEHICLE;
}

bool
CRouteEditor::Command(const char *args, const SEditorPlace *here, IRouteEditorHost *host, char *reply, uint32_t replySize)
{
	char buf[160];
	strncpy(buf, args ? args : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *cmd = strtok(buf, " ");
	char *rest = strtok(nullptr, "");	// everything after the command
	char restCopy[160];
	strcpy(restCopy, rest ? rest : "");
	char *a1 = strtok(restCopy, " ");
	char *a2 = a1 ? strtok(nullptr, " ") : nullptr;
	char *a3 = a2 ? strtok(nullptr, " ") : nullptr;

	if(cmd == nullptr)
		return Reply(reply, replySize, false, "Commands: new, arena, load, grid, cp, hill, ins, move, del, radius, list, undo, save, test");

	// ---- Starting
	if(strcmp(cmd, "new") == 0 || strcmp(cmd, "arena") == 0){
		bool arena = cmd[0] == 'a';
		if(rest == nullptr || rest[0] == '\0')
			return Reply(reply, replySize, false, "Usage: /rec %s NAME", cmd);
		Begin(m_route, rest, arena ? (uint8_t)RACE_ARENA : (uint8_t)RACE_SPRINT, here);
		m_active = true;
		m_numUndo = 0;
		m_dirty = true;
		return Reply(reply, replySize, true, arena ? "New arena '%s': add spawns with /rec grid, hills with /rec hill" :
			"New route '%s': add the grid with /rec grid, checkpoints with /rec cp", m_route.name);
	}
	if(strcmp(cmd, "load") == 0){
		if(rest == nullptr || rest[0] == '\0')
			return Reply(reply, replySize, false, "Usage: /rec load NAME");
		char name[MP_MAX_ROUTE_NAME + 1];
		CopyName(name, rest);
		SRaceRoute loaded;
		char err[96];
		if(host == nullptr || !host->LoadRoute(name, loaded, err, sizeof(err)))
			return Reply(reply, replySize, false, "Can't load '%s': %s", name, host ? err : "no host");
		m_route = loaded;
		m_active = true;
		m_numUndo = 0;
		m_dirty = false;
		return Reply(reply, replySize, true, "Editing '%s' (%s, %d %ss, %d %ss)", m_route.name, TypeName(m_route.type),
			(int)m_route.numGrid, SlotWord(m_route.type), (int)m_route.numCheckpoints, PointWord(m_route.type));
	}
	if(!m_active)
		return Reply(reply, replySize, false, "Start with /rec new NAME, /rec arena NAME or /rec load NAME");

	// ---- Whole route
	if(strcmp(cmd, "close") == 0){
		bool dirty = m_dirty;
		Close();
		return Reply(reply, replySize, true, dirty ? "Closed (unsaved changes dropped)" : "Closed");
	}
	if(strcmp(cmd, "name") == 0){
		if(rest == nullptr || rest[0] == '\0')
			return Reply(reply, replySize, false, "Usage: /rec name NAME");
		PushUndo();
		CopyName(m_route.name, rest);
		return Reply(reply, replySize, true, "Renamed to '%s'", m_route.name);
	}
	if(strcmp(cmd, "type") == 0){
		uint8_t type;
		if(a1 && strcmp(a1, "sprint") == 0)
			type = RACE_SPRINT;
		else if(a1 && strcmp(a1, "circuit") == 0)
			type = RACE_CIRCUIT;
		else if(a1 && strcmp(a1, "arena") == 0)
			type = RACE_ARENA;
		else
			return Reply(reply, replySize, false, "Usage: /rec type sprint|circuit|arena");
		if(type == RACE_ARENA && m_route.numCheckpoints > MP_MAX_HILLS)
			return Reply(reply, replySize, false, "An arena has at most %u hills; delete some first", (unsigned)MP_MAX_HILLS);
		PushUndo();
		m_route.type = type;
		m_route.laps = type == RACE_CIRCUIT ? (m_route.laps > 1 ? m_route.laps : 3) : 1;
		if(type != RACE_ARENA && m_route.vehicleModel == 0)
			m_route.vehicleModel = DEFAULT_RACE_VEHICLE;
		if(type == RACE_CIRCUIT)
			return Reply(reply, replySize, true, "Type: circuit, %d laps", (int)m_route.laps);
		return Reply(reply, replySize, true, "Type: %s", TypeName(type));
	}
	if(strcmp(cmd, "laps") == 0){
		long laps = ParseIndex(a1);
		if(laps < 1 || laps > 9)
			return Reply(reply, replySize, false, "Usage: /rec laps 1..9");
		if(m_route.type != RACE_CIRCUIT)
			return Reply(reply, replySize, false, "Only circuits have laps (/rec type circuit)");
		PushUndo();
		m_route.laps = (uint8_t)laps;
		return Reply(reply, replySize, true, "Laps: %d", (int)laps);
	}
	if(strcmp(cmd, "vehicle") == 0){
		uint16_t model;
		if(a1){
			long m = ParseIndex(a1);
			if(m <= 0 || m > 65535)
				return Reply(reply, replySize, false, "Usage: /rec vehicle [MODEL]");
			model = (uint16_t)m;
		}else if(here && here->vehicleModel)
			model = here->vehicleModel;
		else
			return Reply(reply, replySize, false, "Get in the vehicle first, or give a model id");
		PushUndo();
		m_route.vehicleModel = model;
		return Reply(reply, replySize, true, "Vehicle: model %u", (unsigned)model);
	}

	// ---- Grid / spawns
	if(strcmp(cmd, "grid") == 0 || strcmp(cmd, "spawn") == 0){
		if(here == nullptr)
			return Reply(reply, replySize, false, "You need to be in the world");
		if(m_route.numGrid >= MP_MAX_PLAYERS)
			return Reply(reply, replySize, false, "Full: at most %u %ss", (unsigned)MP_MAX_PLAYERS, SlotWord(m_route.type));
		PushUndo();
		SRaceGridSlot &g = m_route.grid[m_route.numGrid++];
		memcpy(g.pos, here->pos, sizeof(g.pos));
		g.heading = here->heading;
		return Reply(reply, replySize, true, "Added %s %d at %.1f %.1f %.1f", SlotWord(m_route.type), (int)m_route.numGrid,
			g.pos[0], g.pos[1], g.pos[2]);
	}
	if(strcmp(cmd, "movegrid") == 0 || strcmp(cmd, "delgrid") == 0){
		long n = ParseIndex(a1);
		if(n < 1 || n > m_route.numGrid)
			return Reply(reply, replySize, false, "Usage: /rec %s 1..%d", cmd, (int)m_route.numGrid);
		if(cmd[0] == 'm'){
			if(here == nullptr)
				return Reply(reply, replySize, false, "You need to be in the world");
			PushUndo();
			SRaceGridSlot &g = m_route.grid[n - 1];
			memcpy(g.pos, here->pos, sizeof(g.pos));
			g.heading = here->heading;
			return Reply(reply, replySize, true, "Moved %s %ld here", SlotWord(m_route.type), n);
		}
		PushUndo();
		memmove(&m_route.grid[n - 1], &m_route.grid[n], sizeof(m_route.grid[0]) * (m_route.numGrid - n));
		m_route.numGrid--;
		return Reply(reply, replySize, true, "Deleted %s %ld (%d left)", SlotWord(m_route.type), n, (int)m_route.numGrid);
	}

	// ---- Checkpoints / hills
	if(strcmp(cmd, "cp") == 0 || strcmp(cmd, "hill") == 0 || strcmp(cmd, "ins") == 0){
		bool insert = cmd[0] == 'i';
		long at = m_route.numCheckpoints + 1;
		const char *radiusArg = a1;
		if(insert){
			at = ParseIndex(a1);
			if(at < 1 || at > m_route.numCheckpoints)
				return Reply(reply, replySize, false, "Usage: /rec ins 1..%d [RADIUS]", (int)m_route.numCheckpoints);
			radiusArg = a2;
		}
		float def = m_route.type == RACE_ARENA ? DEFAULT_HILL_RADIUS : DEFAULT_CP_RADIUS;
		float radius;
		if(!ParseRadius(radiusArg, def, radius))
			return Reply(reply, replySize, false, "The radius must be above 0 and at most %.0f", MP_ROUTE_MAX_RADIUS);
		if(here == nullptr)
			return Reply(reply, replySize, false, "You need to be in the world");
		if(m_route.numCheckpoints >= MaxPoints())
			return Reply(reply, replySize, false, "Full: at most %u %ss", (unsigned)MaxPoints(), PointWord(m_route.type));
		PushUndo();
		memmove(&m_route.cps[at], &m_route.cps[at - 1], sizeof(m_route.cps[0]) * (m_route.numCheckpoints - (at - 1)));
		SRaceCheckpoint &c = m_route.cps[at - 1];
		memcpy(c.pos, here->pos, sizeof(c.pos));
		c.radius = radius;
		m_route.numCheckpoints++;
		return Reply(reply, replySize, true, "%s %s %ld (radius %.1f, %d total)", insert ? "Inserted" : "Added",
			PointWord(m_route.type), at, radius, (int)m_route.numCheckpoints);
	}
	if(strcmp(cmd, "move") == 0 || strcmp(cmd, "del") == 0 || strcmp(cmd, "radius") == 0){
		long n = ParseIndex(a1);
		if(n < 1 || n > m_route.numCheckpoints)
			return Reply(reply, replySize, false, "Usage: /rec %s 1..%d%s", cmd, (int)m_route.numCheckpoints,
				cmd[0] == 'r' ? " RADIUS" : "");
		SRaceCheckpoint &c = m_route.cps[n - 1];
		if(cmd[0] == 'm'){
			if(here == nullptr)
				return Reply(reply, replySize, false, "You need to be in the world");
			PushUndo();
			memcpy(c.pos, here->pos, sizeof(c.pos));
			return Reply(reply, replySize, true, "Moved %s %ld here", PointWord(m_route.type), n);
		}
		if(cmd[0] == 'r'){
			float radius;
			if(a2 == nullptr || a3 || !ParseRadius(a2, 0.0f, radius))
				return Reply(reply, replySize, false, "Usage: /rec radius N RADIUS (above 0, at most %.0f)", MP_ROUTE_MAX_RADIUS);
			PushUndo();
			c.radius = radius;
			return Reply(reply, replySize, true, "%s %ld radius %.1f", PointWord(m_route.type), n, radius);
		}
		PushUndo();
		memmove(&m_route.cps[n - 1], &m_route.cps[n], sizeof(m_route.cps[0]) * (m_route.numCheckpoints - n));
		m_route.numCheckpoints--;
		return Reply(reply, replySize, true, "Deleted %s %ld (%d left)", PointWord(m_route.type), n, (int)m_route.numCheckpoints);
	}

	// ---- Review
	if(strcmp(cmd, "list") == 0){
		char laps[16] = "";
		if(m_route.type == RACE_CIRCUIT)
			snprintf(laps, sizeof(laps), ", %d laps", (int)m_route.laps);
		char vehicle[24] = "";
		if(m_route.vehicleModel)
			snprintf(vehicle, sizeof(vehicle), ", vehicle %u", (unsigned)m_route.vehicleModel);
		return Reply(reply, replySize, true, "'%s': %s%s%s, %d %ss, %d %ss%s", m_route.name, TypeName(m_route.type), laps,
			vehicle, (int)m_route.numGrid, SlotWord(m_route.type), (int)m_route.numCheckpoints, PointWord(m_route.type),
			m_dirty ? " (unsaved)" : "");
	}
	if(strcmp(cmd, "undo") == 0){
		if(m_numUndo == 0)
			return Reply(reply, replySize, false, "Nothing to undo");
		m_route = m_undo[--m_numUndo];
		m_dirty = true;
		return Reply(reply, replySize, true, "Undone (%d more step%s)", m_numUndo, m_numUndo == 1 ? "" : "s");
	}
	if(strcmp(cmd, "save") == 0 || strcmp(cmd, "test") == 0){
		bool save = cmd[0] == 's';
		char err[96];
		if(!MpValidateRoute(m_route, err, sizeof(err)))
			return Reply(reply, replySize, false, "%s: %s", save ? "Not saved" : "Can't test", err);
		if(host == nullptr)
			return Reply(reply, replySize, false, "Not available here");
		if(!save){
			if(m_route.type == RACE_ARENA)
				return Reply(reply, replySize, false, "Test an arena by starting a match on it after /rec save");
			host->TestRoute(m_route);
			return Reply(reply, replySize, true, "Asked the session to race '%s'", m_route.name);
		}
		if(!host->SaveRoute(m_route, err, sizeof(err)))
			return Reply(reply, replySize, false, "Not saved: %s", err);
		m_dirty = false;
		return Reply(reply, replySize, true, "Saved '%s' (%d %ss, %d %ss)", m_route.name, (int)m_route.numGrid,
			SlotWord(m_route.type), (int)m_route.numCheckpoints, PointWord(m_route.type));
	}
	return Reply(reply, replySize, false, "Unknown: /rec %s. Try new, arena, load, grid, cp, hill, ins, move, del, radius, list, undo, save, test", cmd);
}
