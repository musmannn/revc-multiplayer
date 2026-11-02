#include "common.h"

#ifdef MULTIPLAYER

#include <ctype.h>

#include "RouteRecorder.h"
#include "NetChat.h"
#include "NetRace.h"
#include "NetMatch.h"
#include "RouteEditor.h"
#include "FileMgr.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "SpecialFX.h"
#include "Vehicle.h"

static const float PREVIEW_DIST = 200.0f;
static const int PREVIEW_POINTS = 10;	// nearest checkpoints/hills shown
static const uint32 MARKER_ID = 0x4D505300;	// + index; races use 0x4D505200..

static CRouteEditor sEditor;

static void
Say(const char *text)
{
	CNetChat::AddLine(text, CNetChat::NOTICE);
}

static bool
PlayerPlace(SEditorPlace &out)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return false;
	CVehicle *v = player->bInVehicle ? player->m_pMyVehicle : nil;
	CVector pos = v ? v->GetPosition() : player->GetPosition();
	float heading = RADTODEG(v ? v->GetForward().Heading() : player->m_fRotationCur);
	if(heading < 0.0f)
		heading += 360.0f;
	out.pos[0] = pos.x;
	out.pos[1] = pos.y;
	out.pos[2] = pos.z;
	out.heading = heading;
	out.vehicleModel = v ? (uint16)v->GetModelIndex() : 0;
	return true;
}

// "Ocean Drive Sprint" -> "ocean_drive_sprint"
static void
Slug(const char *name, char *out, int size)
{
	int n = 0;
	for(const char *p = name; *p && n < size - 1; p++){
		char c = *p;
		if(c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out[n++] = c;
		else if(n > 0 && out[n - 1] != '_')
			out[n++] = '_';
	}
	while(n > 0 && out[n - 1] == '_')
		n--;
	out[n] = '\0';
}

static bool
WriteFile(const char *path, const char *text)
{
	int fd = CFileMgr::OpenFileForWriting(path);
	if(fd == 0)
		return false;
	size_t len = strlen(text);
	bool ok = CFileMgr::Write(fd, text, len) == len;
	CFileMgr::CloseFile(fd);
	return ok;
}

static bool
SameName(const char *a, const char *b)
{
	for(; *a && *b; a++, b++)
		if(tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

class CGameEditorHost : public IRouteEditorHost
{
public:
	bool LoadRoute(const char *name, SRaceRoute &out, char *err, uint32 errSize) override
	{
		for(int i = 0; i < CNetRace::NumRoutes(); i++)
			if(SameName(CNetRace::GetRoute(i)->name, name)){
				out = *CNetRace::GetRoute(i);
				return true;
			}
		for(int i = 0; i < CNetMatch::NumArenas(); i++)
			if(SameName(CNetMatch::GetArena(i)->name, name)){
				out = *CNetMatch::GetArena(i);
				return true;
			}
		snprintf(err, errSize, "no race or arena with that name");
		return false;
	}

	bool SaveRoute(const SRaceRoute &route, char *err, uint32 errSize) override
	{
		const char *dir = route.type == RACE_ARENA ? "arenas" : "races";
		static char text[16 * 1024];
		int n = snprintf(text, sizeof(text), "# Made in-game with /rec\n");
		if(MpWriteRoute(route, text + n, sizeof(text) - n) < 0){
			snprintf(err, errSize, "too big");
			return false;
		}
		char slug[64], file[80], path[128];
		Slug(route.name, slug, sizeof(slug));
		if(slug[0] == '\0')
			strcpy(slug, "route");
		snprintf(file, sizeof(file), "%s.route", slug);
		snprintf(path, sizeof(path), "data\\mp\\%s\\%s", dir, file);
		CFileMgr::SetDir("");
		if(!WriteFile(path, text)){
			snprintf(err, errSize, "can't write data/mp/%s (does the folder exist?)", dir);
			return false;
		}

		// Add it to the index unless it's there already.
		static char index[8 * 1024];
		char indexPath[64];
		snprintf(indexPath, sizeof(indexPath), "data\\mp\\%s\\index.txt", dir);
		int len = MpReadTextFile(indexPath, index, sizeof(index) - 128);
		if(len < 0)
			len = 0;
		bool listed = false;
		for(const char *p = index; *p; ){
			const char *end = p;
			while(*end && *end != '\n' && *end != '\r')
				end++;
			if((size_t)(end - p) == strlen(file) && strncmp(p, file, end - p) == 0)
				listed = true;
			p = *end ? end + 1 : end;
		}
		if(!listed){
			if(len > 0 && index[len - 1] != '\n')
				index[len++] = '\n';
			snprintf(index + len, sizeof(index) - len, "%s\n", file);
			if(!WriteFile(indexPath, index)){
				snprintf(err, errSize, "saved %s but couldn't list it in data/mp/%s/index.txt", file, dir);
				return false;
			}
		}
		if(route.type == RACE_ARENA)
			CNetMatch::LoadArenas();
		else
			CNetRace::Init();
		return true;
	}

	void TestRoute(const SRaceRoute &route) override
	{
		CNetRace::RequestRoute(route);
	}
};
static CGameEditorHost sHost;

bool
CRouteRecorder::OnCommand(const char *text)
{
	if(text[0] != '/')
		return false;
	SEditorPlace here;
	bool placed = PlayerPlace(here);
	char line[160];

	if(strncmp(text, "/pos", 4) == 0 && (text[4] == '\0' || text[4] == ' ')){
		if(placed){
			snprintf(line, sizeof(line), "%.1f %.1f %.1f  heading %.0f", here.pos[0], here.pos[1], here.pos[2], here.heading);
			Say(line);
		}
		return true;
	}
	if(strncmp(text, "/rec", 4) != 0 || (text[4] != '\0' && text[4] != ' ')){
		Say("Commands: /pos, /rec (route and arena editor: /rec help)");
		return true;
	}
	const char *args = text + 4;
	while(*args == ' ')
		args++;
	if(*args == '\0' || strcmp(args, "help") == 0){
		Say("/rec new NAME | arena NAME | load NAME | grid | cp [R] | hill [R] | ins N [R] | move N | del N");
		Say("/rec radius N R | movegrid N | delgrid N | type T | laps N | vehicle | name NAME | list | undo | save | test | close");
		return true;
	}
	sEditor.Command(args, placed ? &here : nil, &sHost, line, sizeof(line));
	Say(line);
	return true;
}

void
CRouteRecorder::Process(void)
{
	if(!sEditor.IsActive() || CNetRace::IsRacing())
		return;
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	CVector me = player->GetPosition();
	const SRaceRoute &r = sEditor.Route();
	bool arena = r.type == RACE_ARENA;

	// The nearest checkpoints/hills: the first is green, the finish red,
	// the rest yellow (hills: blue).
	int order[MP_MAX_CHECKPOINTS];
	float dist[MP_MAX_CHECKPOINTS];
	int n = 0;
	for(int i = 0; i < r.numCheckpoints; i++){
		CVector p(r.cps[i].pos[0], r.cps[i].pos[1], r.cps[i].pos[2]);
		float d = (p - me).Magnitude();
		if(d > PREVIEW_DIST)
			continue;
		int j = n++;
		while(j > 0 && dist[j - 1] > d){
			dist[j] = dist[j - 1];
			order[j] = order[j - 1];
			j--;
		}
		dist[j] = d;
		order[j] = i;
	}
	uint32 id = MARKER_ID;
	for(int k = 0; k < n && k < PREVIEW_POINTS; k++){
		int i = order[k];
		CVector p(r.cps[i].pos[0], r.cps[i].pos[1], r.cps[i].pos[2]);
		uint8 red = 255, green = 220, blue = 0;
		if(arena){
			red = 60; green = 140; blue = 255;
		}else if(i == 0){
			red = 60; green = 255; blue = 60;
		}else if(i == r.numCheckpoints - 1){
			red = 255; green = 40; blue = 40;
		}
		C3dMarkers::PlaceMarkerSet(id++, MARKERTYPE_CYLINDER, p, r.cps[i].radius, red, green, blue, 110, 2048, 0.1f, 0);
	}
	// Grid slots / spawns: small white rings.
	for(int i = 0; i < r.numGrid; i++){
		CVector p(r.grid[i].pos[0], r.grid[i].pos[1], r.grid[i].pos[2]);
		if((p - me).Magnitude() > PREVIEW_DIST)
			continue;
		C3dMarkers::PlaceMarkerSet(id++, MARKERTYPE_CYLINDER, p, 1.2f, 255, 255, 255, 120, 1024, 0.2f, 0);
	}
}

bool
CRouteRecorder::IsEditing(void)
{
	return sEditor.IsActive();
}

#endif
