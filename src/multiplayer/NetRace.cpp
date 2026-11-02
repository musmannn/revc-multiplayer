#include "common.h"

#ifdef MULTIPLAYER

#include "NetRace.h"
#include "NetGame.h"
#include "NetChat.h"
#include "NetCombat.h"
#include "NetVehicles.h"
#include "NetMatch.h"
#include "FreeroamMenu.h"
#include "RemotePlayer.h"
#include "MpText.h"
#include "RaceHost.h"
#include "DMAudio.h"
#include "FileMgr.h"
#include "Font.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Radar.h"
#include "SpecialFX.h"
#include "Sprite2d.h"
#include "Vehicle.h"
#include "ModelInfo.h"

static const uint32 RESULTS_SHOW_MS = 10000;
static const uint32 OUT_OF_CAR_RESPAWN_MS = 5000;
static const uint32 HOLD_H_RESPAWN_MS = 1500;
static const uint32 RESPAWN_FREEZE_MS = 1000;
static const float CROSS_HEIGHT = 6.0f;
static const float WRECKED_HEALTH = 250.0f;

static SRaceRoute sRoutes[CNetRace::MAX_ROUTES];
static int sNumRoutes;
static bool sMenuRegistered;

// Host side.
class CGameRaceSink : public IRaceHostSink
{
	static void Send(uint8 route, uint8 target, bool reliable, const CByteWriter &w)
	{
		CNetGame::Client().SendGame(route, target, reliable, w);
	}
public:
	void SendInvite(const SRaceInvite &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendStart(const SRaceStart &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendStandings(const SRaceStandings &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, false, w); }
	void SendEnd(const SRaceStandings &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendCancel(const SRaceCancel &m, uint8 target) override
	{
		CByteWriter w;
		m.Write(w);
		if(target == MP_PLAYER_NONE)
			Send(MP_ROUTE_ALL, 0, true, w);
		else
			Send(MP_ROUTE_PLAYER, target, true, w);
	}
};
static CRaceHost sHost;
static CGameRaceSink sSink;

// Everyone: the open invite.
struct SInviteState
{
	bool open;
	bool accepted;
	uint16 raceId;
	uint8 startedBy;
	SRaceRoute route;	// sent with the invite: no file needed
	uint32 deadline;
};
static SInviteState sInvite;

// The local player's race.
enum eRacePhase { PHASE_NONE, PHASE_COUNTDOWN, PHASE_RACING, PHASE_DONE };
struct SLocalRace
{
	eRacePhase phase;
	uint16 raceId;
	SRaceRoute route;
	uint32 startTime;
	int slot;
	uint16 nextSeq;
	uint16 total;
	int countdownShown;		// last number beeped (3, 2, 1, 0 = GO)
	CVehicle *vehicle;		// registered reference
	uint32 outOfCarSince;
	uint32 holdHSince;
	uint32 unfreezeAt;		// session time; 0 = not frozen by a respawn
	bool wasDead;
	uint32 retryAt;			// session time; a failed respawn tries again then
	uint32 finishTime;		// ms from the start, once we've crossed the finish
	int32 blipCurrent;
	int32 blipNext;
};
static SLocalRace sRace;

static SRaceStandings sStandings;	// latest for the race we're in or watching
static SRaceStandings sResults;
static uint32 sResultsUntil;		// NetNow
static char sResultsRoute[MP_MAX_ROUTE_NAME + 1];

static char sRacerNames[MP_MAX_PLAYERS][MP_MAX_NAME_LEN + 1];	// cached at START

static const char*
NameOf(uint8 id)
{
	const char *n = CNetGame::PlayerName(id);
	if(n)
		return n;
	// Left since the start: the name we saw then.
	return id < MP_MAX_PLAYERS && sRacerNames[id][0] ? sRacerNames[id] : "?";
}

static void
FormatTime(uint32 ms, char *out, int size)
{
	snprintf(out, size, "%02u:%02u.%02u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms / 10 % 100));
}

// ---- Routes

// Reads at most size-1 bytes and NUL-terminates. (CFileMgr::LoadFile reads in
// 16 KB chunks regardless of its maxlen.) Returns the length, or -1.
int
MpReadTextFile(const char *path, char *buf, int size)
{
	int fd = CFileMgr::OpenFile(path, "rb");
	if(fd == 0)
		return -1;
	int len = (int)CFileMgr::Read(fd, buf, size - 1);
	CFileMgr::CloseFile(fd);
	if(len < 0)
		len = 0;
	buf[len] = '\0';
	return len;
}

// dir is "races" or "arenas" under data/mp.
static bool
LoadRouteFile(const char *dir, const char *file, SRaceRoute &route, bool arena)
{
	static char text[64 * 1024];
	char path[128];
	snprintf(path, sizeof(path), "data\\mp\\%s\\%s", dir, file);
	if(MpReadTextFile(path, text, sizeof(text)) < 0){
		re3_debug("[MP] %s/%s: can't read it\n", dir, file);
		return false;
	}
	char err[128];
	if(!MpParseRoute(text, route, err, sizeof(err))){
		re3_debug("[MP] %s/%s skipped: %s\n", dir, file, err);
		return false;
	}
	if((route.type == RACE_ARENA) != arena){
		re3_debug("[MP] %s/%s skipped: %s\n", dir, file, arena ? "not an arena" : "it's an arena");
		return false;
	}
	if(route.vehicleModel != 0 && !CNetRace::IsLandVehicle(route.vehicleModel)){
		re3_debug("[MP] %s/%s skipped: vehicle %d isn't a car or bike\n", dir, file, (int)route.vehicleModel);
		return false;
	}
	return true;
}

int
CNetRace::LoadRouteDir(const char *dir, SRaceRoute *out, int max, bool arenas)
{
	int n = 0;
	CFileMgr::SetDir("");
	static char index[8 * 1024];
	char path[64];
	snprintf(path, sizeof(path), "data\\mp\\%s\\index.txt", dir);
	if(MpReadTextFile(path, index, sizeof(index)) <= 0){
		re3_debug("[MP] nothing in data/mp/%s (index.txt)\n", dir);
		return 0;
	}
	// Not strtok: LoadRouteFile is free to use it.
	for(char *p = index; *p && n < max; ){
		char *line = p;
		while(*p && *p != '\r' && *p != '\n')
			p++;
		if(*p)
			*p++ = '\0';
		while(*line == ' ' || *line == '\t')
			line++;
		if(*line == '\0' || *line == '#')
			continue;
		if(LoadRouteFile(dir, line, out[n], arenas))
			n++;
	}
	return n;
}

// Only land vehicles the engine knows; a bad id would crash every racer.
bool
CNetRace::IsLandVehicle(int32 m)
{
	CBaseModelInfo *mi = m >= 0 && m < MODELINFOSIZE ? CModelInfo::GetModelInfo(m) : nil;
	return mi && mi->GetModelType() == MITYPE_VEHICLE && !CModelInfo::IsBoatModel(m) &&
		!CModelInfo::IsHeliModel(m) && !CModelInfo::IsPlaneModel(m);
}

static int
RouteCount(void)
{
	return sNumRoutes;
}

static const char*
RouteLabel(int i)
{
	return i < sNumRoutes ? sRoutes[i].name : "";
}

void
CNetRace::Init(void)
{
	sNumRoutes = LoadRouteDir("races", sRoutes, MAX_ROUTES, false);
	if(!sMenuRegistered){
		CFreeroamMenu::AddList("Start race >", RouteCount, RouteLabel, RequestRace);
		sMenuRegistered = true;
	}
}

int
CNetRace::NumRoutes(void)
{
	return sNumRoutes;
}

const SRaceRoute*
CNetRace::GetRoute(int i)
{
	return i >= 0 && i < sNumRoutes ? &sRoutes[i] : nil;
}

const SRaceRoute*
CNetRace::FindRoute(const char *name)
{
	for(int i = 0; i < sNumRoutes; i++)
		if(strcmp(sRoutes[i].name, name) == 0)
			return &sRoutes[i];
	return nil;
}

void
CNetRace::RequestRace(int routeIndex)
{
	const SRaceRoute *route = GetRoute(routeIndex);
	if(route)
		RequestRoute(*route);
}

void
CNetRace::RequestRoute(const SRaceRoute &route)
{
	SRaceRequest msg;
	msg.route = route;
	CByteWriter w;
	msg.Write(w);
	CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);
}

// ---- Local race

bool
CNetRace::IsRacing(void)
{
	return sRace.phase != PHASE_NONE;
}

bool
CNetRace::HostBusy(void)
{
	return CNetGame::IsHost() && sHost.IsBusy();
}

static void
SetVehicle(CVehicle *v)
{
	if(sRace.vehicle)
		sRace.vehicle->CleanUpOldReference((CEntity**)&sRace.vehicle);
	sRace.vehicle = v;
	if(v)
		v->RegisterReference((CEntity**)&sRace.vehicle);
}

static void
Freeze(bool on)
{
	if(sRace.vehicle){
		sRace.vehicle->bIsFrozen = on;
		sRace.vehicle->bInfiniteMass = on;
	}
	CPad *pad = CPad::GetPad(0);
	if(on)
		pad->SetDisablePlayerControls(CNetRace::PLAYERCONTROL_MP_RACE);
	else
		pad->SetEnablePlayerControls(CNetRace::PLAYERCONTROL_MP_RACE);
}

static void
ClearBlips(void)
{
	if(sRace.blipCurrent >= 0)
		CRadar::ClearBlip(sRace.blipCurrent);
	if(sRace.blipNext >= 0)
		CRadar::ClearBlip(sRace.blipNext);
	sRace.blipCurrent = sRace.blipNext = -1;
}

static const SRaceCheckpoint*
CheckpointFor(uint16 seq)
{
	if(seq >= sRace.total || sRace.route.numCheckpoints == 0)
		return nil;
	return &sRace.route.cps[seq % sRace.route.numCheckpoints];
}

static CVector
CpPos(const SRaceCheckpoint *cp)
{
	return CVector(cp->pos[0], cp->pos[1], cp->pos[2]);
}

// Heading (radians, engine convention: forward = (-sin h, cos h)) from a to b.
static float
HeadingTo(const CVector &a, const CVector &b)
{
	return Atan2(-(b.x - a.x), b.y - a.y);
}

static void
UpdateBlips(void)
{
	ClearBlips();
	const SRaceCheckpoint *cur = CheckpointFor(sRace.nextSeq);
	const SRaceCheckpoint *next = CheckpointFor(sRace.nextSeq + 1);
	if(cur){
		sRace.blipCurrent = CRadar::SetCoordBlip(BLIP_COORD, CpPos(cur), 0xFFFF00FF, BLIP_DISPLAY_BOTH);
		CRadar::ChangeBlipColour(sRace.blipCurrent, RADAR_TRACE_YELLOW);
		CRadar::ChangeBlipScale(sRace.blipCurrent, 3);
	}
	if(next){
		sRace.blipNext = CRadar::SetCoordBlip(BLIP_COORD, CpPos(next), 0x7F7F00FF, BLIP_DISPLAY_BLIP_ONLY);
		CRadar::ChangeBlipColour(sRace.blipNext, RADAR_TRACE_YELLOW);
		CRadar::ChangeBlipBrightness(sRace.blipNext, 1);	// 1 = dim, like the script's DIM_BLIP
		CRadar::ChangeBlipScale(sRace.blipNext, 2);
	}
}

// Where a respawn puts us: the last checkpoint crossed, or our grid slot.
static void
RespawnPoint(CVector &pos, float &heading)
{
	const SRaceRoute &r = sRace.route;
	if(sRace.nextSeq == 0){
		const SRaceGridSlot &g = r.grid[sRace.slot];
		pos = CVector(g.pos[0], g.pos[1], g.pos[2]);
		heading = DEGTORAD(g.heading);
		return;
	}
	pos = CpPos(&r.cps[(sRace.nextSeq - 1) % r.numCheckpoints]);
	const SRaceCheckpoint *next = CheckpointFor(sRace.nextSeq);
	heading = next ? HeadingTo(pos, CpPos(next)) : 0.0f;
}

static void
RespawnCar(void)
{
	uint32 now = CNetGame::SessionNow();
	if(sRace.retryAt && (int32)(now - sRace.retryAt) < 0)
		return;
	CVector pos;
	float heading;
	RespawnPoint(pos, heading);
	CVehicle *v = CFreeroamMenu::SpawnAndEnter(sRace.route.vehicleModel, pos, heading);
	if(v == nil){
		sRace.retryAt = now + 2000;	// don't hammer a blocking model load every frame
		return;
	}
	sRace.retryAt = 0;
	SetVehicle(v);
	Freeze(true);
	sRace.unfreezeAt = CNetGame::SessionNow() + RESPAWN_FREEZE_MS;
	sRace.outOfCarSince = 0;
	sRace.holdHSince = 0;
}

static void
UpdateRespawnOverride(void)
{
	CVector pos;
	float heading;
	RespawnPoint(pos, heading);
	CNetCombat::SetRespawnOverride(pos, heading);
}

static void
LeaveRace(void)
{
	if(sRace.phase == PHASE_NONE)
		return;
	Freeze(false);
	ClearBlips();
	CNetCombat::ClearRespawnOverride();
	SetVehicle(nil);	// the car stays where it is
	sRace.phase = PHASE_NONE;
}

static void
BeginRace(const SRaceStart &start, int slot)
{
	const SRaceRoute *route = (sInvite.raceId == start.raceId) ? &sInvite.route : nil;
	if(route == nil || !CNetRace::IsLandVehicle(route->vehicleModel)){
		CNetChat::AddLine("Can't join: this race's car doesn't exist in your game", CNetChat::NOTICE);
		return;
	}
	LeaveRace();
	memset(&sRace, 0, sizeof(sRace));
	sRace.blipCurrent = sRace.blipNext = -1;
	sRace.raceId = start.raceId;
	sRace.route = *route;
	sRace.startTime = start.startTime;
	sRace.slot = Min(slot, (int)route->numGrid - 1);
	sRace.total = MpRouteTotalCrossings(*route);
	sRace.countdownShown = 4;
	sRace.phase = PHASE_COUNTDOWN;

	CFreeroamMenu::Close();
	if(FindPlayerPed() && FindPlayerPed()->bInVehicle)
		CNetVehicles::WarpLocalPlayerOut();
	RespawnCar();
	sRace.unfreezeAt = 0;	// frozen until GO
	Freeze(true);
	UpdateRespawnOverride();
	UpdateBlips();
}

static void
PlaceMarkers(void)
{
	const SRaceCheckpoint *cur = CheckpointFor(sRace.nextSeq);
	const SRaceCheckpoint *next = CheckpointFor(sRace.nextSeq + 1);
	if(cur){
		CVector pos = CpPos(cur);
		bool finish = sRace.nextSeq + 1 == sRace.total;
		if(finish)
			C3dMarkers::PlaceMarkerSet(0x4D505200, MARKERTYPE_CYLINDER, pos, cur->radius, 255, 40, 40, 160, 1024, 0.2f, 0);
		else
			C3dMarkers::PlaceMarkerSet(0x4D505200, MARKERTYPE_CYLINDER, pos, cur->radius, 255, 220, 0, 160, 1024, 0.2f, 0);
	}
	if(next){
		CVector pos = CpPos(next);
		C3dMarkers::PlaceMarkerSet(0x4D505201, MARKERTYPE_CYLINDER, pos, next->radius * 0.6f, 255, 220, 0, 60, 2048, 0.1f, 0);
	}
}

static void
CheckCrossing(uint32 now)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || !player->bInVehicle || player->m_pMyVehicle == nil)
		return;
	const SRaceCheckpoint *cp = CheckpointFor(sRace.nextSeq);
	if(cp == nil)
		return;
	CVector pos = player->m_pMyVehicle->GetPosition();
	CVector d = pos - CpPos(cp);
	if(d.Magnitude2D() > cp->radius || Abs(d.z) > CROSS_HEIGHT)
		return;

	SRaceCross msg;
	msg.raceId = sRace.raceId;
	msg.seq = sRace.nextSeq;
	msg.time = now;
	CByteWriter w;
	msg.Write(w);
	CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);

	// Optimistic: advance at once; the host's standings decide positions.
	sRace.nextSeq++;
	UpdateRespawnOverride();
	if(sRace.nextSeq >= sRace.total){
		DMAudio.PlayFrontEndSound(SOUND_PART_MISSION_COMPLETE, 0);
		sRace.phase = PHASE_DONE;
		sRace.finishTime = now - sRace.startTime;
		sRace.unfreezeAt = 0;
		Freeze(false);
		ClearBlips();
	}else{
		DMAudio.PlayFrontEndSound(SOUND_RACE_START_1, 0);
		UpdateBlips();
	}
}

static void
UpdateRacing(uint32 now)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	CPad *pad = CPad::GetPad(0);

	if(sRace.unfreezeAt && (int32)(now - sRace.unfreezeAt) >= 0){
		sRace.unfreezeAt = 0;
		Freeze(false);
	}
	if(player->DyingOrDead()){
		sRace.wasDead = true;	// CNetCombat respawns us at the last checkpoint
		return;
	}
	if(sRace.wasDead){
		sRace.wasDead = false;
		RespawnCar();
		return;
	}

	bool inOurCar = player->bInVehicle && player->m_pMyVehicle && player->m_pMyVehicle == sRace.vehicle;
	CVehicle *v = sRace.vehicle;
	bool wrecked = v == nil || v->GetStatus() == STATUS_WRECKED || v->m_fHealth < WRECKED_HEALTH;
	if(wrecked){
		RespawnCar();
		return;
	}
	if(!inOurCar){
		if(sRace.outOfCarSince == 0)
			sRace.outOfCarSince = now | 1;
		else if(now - sRace.outOfCarSince >= OUT_OF_CAR_RESPAWN_MS){
			RespawnCar();
			return;
		}
	}else
		sRace.outOfCarSince = 0;
	if(pad->GetChar('H') && !CNetChat::IsTyping()){
		if(sRace.holdHSince == 0)
			sRace.holdHSince = now | 1;
		else if(now - sRace.holdHSince >= HOLD_H_RESPAWN_MS){
			RespawnCar();
			return;
		}
	}else
		sRace.holdHSince = 0;

	PlaceMarkers();
	CheckCrossing(now);
}

static void
UpdateHost(uint32 now)
{
	if(!CNetGame::IsHost())
		return;
	// Distance to each racer's next checkpoint, from the positions we have.
	if(sHost.IsRacing()){
		const CRaceState &race = sHost.Race();
		const SRaceRoute &r = race.Route();
		for(int id = 0; id < MP_MAX_PLAYERS; id++){
			const SRacer *racer = race.GetRacer((uint8)id);
			if(racer == nil || racer->state != RACER_RACING || r.numCheckpoints == 0)
				continue;
			CVector pos;
			if(id == CNetGame::MyId()){
				CPlayerPed *p = FindPlayerPed();
				if(p == nil)
					continue;
				pos = p->bInVehicle && p->m_pMyVehicle ? p->m_pMyVehicle->GetPosition() : p->GetPosition();
			}else{
				CRemotePlayer *rp = CRemotePlayer::Get((uint8)id);
				if(rp == nil || !rp->m_bHaveState)
					continue;
				pos = CVector(rp->m_last.pos[0], rp->m_last.pos[1], rp->m_last.pos[2]);
			}
			const SRaceCheckpoint &cp = r.cps[racer->nextSeq % r.numCheckpoints];
			sHost.SetDistance((uint8)id, (pos - CVector(cp.pos[0], cp.pos[1], cp.pos[2])).Magnitude());
		}
	}
	sHost.Update(now, &sSink);
}

void
CNetRace::Process(void)
{
	uint32 now = CNetGame::SessionNow();
	UpdateHost(now);

	if(sInvite.open && (int32)(now - sInvite.deadline) >= 0)
		sInvite.open = false;
	if(sInvite.open && !sInvite.accepted && sRace.phase == PHASE_NONE && !CNetMatch::IsPlaying() &&
	   !CNetChat::IsTyping() && CPad::GetPad(0)->GetFJustDown(2)){	// F3
		if(!IsLandVehicle(sInvite.route.vehicleModel)){
			// Joining would leave us "racing" on the host forever.
			CNetChat::AddLine("Can't join: this race's car doesn't exist in your game", CNetChat::NOTICE);
			sInvite.accepted = true;
			return;
		}
		SRaceAccept msg;
		msg.raceId = sInvite.raceId;
		CByteWriter w;
		msg.Write(w);
		CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);
		sInvite.accepted = true;
		CNetChat::AddLine("You're in the race", CNetChat::NOTICE);
	}

	switch(sRace.phase){
	case PHASE_COUNTDOWN: {
		int32 toGo = (int32)(sRace.startTime - now);
		CPlayerPed *player = FindPlayerPed();
		if(player && player->DyingOrDead())
			sRace.wasDead = true;
		else if(sRace.wasDead && player){
			sRace.wasDead = false;
			RespawnCar();
			sRace.unfreezeAt = 0;	// held until GO
		}
		// A respawn re-enables every control; keep the grid frozen.
		if(toGo > 0)
			Freeze(true);
		int shown = toGo > 2000 ? 3 : toGo > 1000 ? 2 : toGo > 0 ? 1 : 0;
		if(toGo <= 3000 && shown < sRace.countdownShown){
			static const int16 sounds[4] = { SOUND_RACE_START_GO, SOUND_RACE_START_1, SOUND_RACE_START_2, SOUND_RACE_START_3 };
			DMAudio.PlayFrontEndSound(sounds[shown], 0);
			sRace.countdownShown = shown;
		}
		if(toGo <= 0){
			Freeze(false);
			sRace.phase = PHASE_RACING;
		}
		PlaceMarkers();
		break;
	}
	case PHASE_RACING:
		UpdateRacing(now);
		break;
	default:
		break;
	}
}

// ---- Messages

void
CNetRace::OnMessage(uint8 from, uint8 type, CByteReader &body)
{
	uint8 me = CNetGame::MyId();
	uint32 now = CNetGame::SessionNow();
	switch(type){
	case GMSG_RACE_REQUEST: {
		SRaceRequest m;
		if(!CNetGame::IsHost() || !m.Read(body))
			return;
		if(CNetMatch::HostBusy() && !sHost.IsBusy()){
			// One mode at a time: a match is on.
			SRaceCancel busy;
			busy.raceId = sHost.RaceId();
			busy.reason = RACE_BUSY;
			sSink.SendCancel(busy, from);
			return;
		}
		// The route came with the request (and passed MpValidateRoute).
		if(!IsLandVehicle(m.route.vehicleModel)){
			SRaceCancel cancel;
			cancel.raceId = sHost.RaceId();
			cancel.reason = RACE_BAD_ROUTE;
			sSink.SendCancel(cancel, from);
			return;
		}
		sHost.Request(from, m.route, true, now, &sSink);
		break;
	}
	case GMSG_RACE_ACCEPT: {
		SRaceAccept m;
		if(CNetGame::IsHost() && m.Read(body))
			sHost.OnAccept(from, m.raceId, now);
		break;
	}
	case GMSG_RACE_CROSS: {
		SRaceCross m;
		if(CNetGame::IsHost() && m.Read(body))
			sHost.OnCross(from, m);
		break;
	}
	case GMSG_RACE_INVITE: {
		SRaceInvite m;
		if(from != 0 || !m.Read(body))
			return;
		if(sInvite.open && sInvite.raceId == m.raceId)
			return;	// a resend for a late joiner
		sInvite.open = true;
		sInvite.accepted = m.startedBy == me;	// the starter is in already
		sInvite.raceId = m.raceId;
		sInvite.startedBy = m.startedBy;
		sInvite.route = m.route;
		sInvite.deadline = m.deadline;
		char line[128];
		if(m.startedBy == me)
			snprintf(line, sizeof(line), "You started %s - others have 15s to join", m.route.name);
		else if(m.startedBy == MP_PLAYER_NONE || (m.startedBy == 0 && CNetGame::IsDedicated()))
			snprintf(line, sizeof(line), "Next up: %s - press F3 to join (15s)", m.route.name);
		else
			snprintf(line, sizeof(line), "%s started %s - press F3 to join (15s)", NameOf(m.startedBy), m.route.name);
		CNetChat::AddLine(line, CNetChat::NOTICE);
		break;
	}
	case GMSG_RACE_START: {
		SRaceStart m;
		if(from != 0 || !m.Read(body))
			return;
		sInvite.open = false;
		memset(&sStandings, 0, sizeof(sStandings));
		memset(sRacerNames, 0, sizeof(sRacerNames));
		for(int i = 0; i < m.n; i++)
			if(m.ids[i] < MP_MAX_PLAYERS && CNetGame::PlayerName(m.ids[i]))
				strcpy(sRacerNames[m.ids[i]], CNetGame::PlayerName(m.ids[i]));
		for(int i = 0; i < m.n; i++)
			if(m.ids[i] == me && !CNetMatch::IsPlaying())
				BeginRace(m, i);
		break;
	}
	case GMSG_RACE_STANDINGS: {
		SRaceStandings m;
		if(from == 0 && m.Read(body))
			sStandings = m;
		break;
	}
	case GMSG_RACE_END: {
		SRaceStandings m;
		if(from != 0 || !m.Read(body))
			return;
		m.msgType = GMSG_RACE_END;
		bool mine = sRace.phase != PHASE_NONE && sRace.raceId == m.raceId;
		const char *routeName = mine ? sRace.route.name : (sInvite.raceId == m.raceId ? sInvite.route.name : "the race");
		if(m.n > 0 && m.entries[0].state == RACER_FINISHED){
			char t[16], line[128];
			FormatTime(m.entries[0].finishTime, t, sizeof(t));
			snprintf(line, sizeof(line), "%s won %s in %s", NameOf(m.entries[0].id), routeName, t);
			CNetChat::AddLine(line, CNetChat::NOTICE);
		}
		if(mine){
			sResults = m;
			strcpy(sResultsRoute, routeName);
			sResultsUntil = CNetGame::NetNow() + RESULTS_SHOW_MS;
			LeaveRace();
		}
		break;
	}
	case GMSG_RACE_CANCEL: {
		SRaceCancel m;
		if(from != 0 || !m.Read(body))
			return;
		const char *why = m.reason == RACE_BUSY ? "A race or match is already on" :
			m.reason == RACE_BAD_ROUTE ? "The host can't use that route" :
			m.reason == RACE_ABORTED ? "The race was called off" : "Nobody joined the race";
		CNetChat::AddLine(why, CNetChat::NOTICE);
		if(m.raceId == sInvite.raceId && m.reason != RACE_BUSY)
			sInvite.open = false;
		if(m.reason == RACE_ABORTED && sRace.phase != PHASE_NONE && sRace.raceId == m.raceId)
			LeaveRace();
		break;
	}
	default:
		break;
	}
}

void
CNetRace::OnPlayerJoined(uint8 id)
{
	SRaceInvite invite;
	if(CNetGame::IsHost() && sHost.GetInvite(invite)){
		CByteWriter w;
		invite.Write(w);
		CNetGame::Client().SendGame(MP_ROUTE_PLAYER, id, true, w);
	}
}

void
CNetRace::OnPlayerLeft(uint8 id)
{
	if(CNetGame::IsHost())
		sHost.OnLeft(id);
}

void
CNetRace::Reset(void)
{
	if(CNetGame::IsHost() && sHost.IsBusy()){
		SRaceCancel cancel;
		cancel.raceId = sHost.RaceId();
		cancel.reason = RACE_ABORTED;
		sSink.SendCancel(cancel, MP_PLAYER_NONE);	// a no-op if the session is gone
	}
	LeaveRace();
	sHost.Reset();
	memset(&sInvite, 0, sizeof(sInvite));
	memset(&sStandings, 0, sizeof(sStandings));
	sResultsUntil = 0;
}

// ---- HUD

static void
SetupFont(float scaleX, float scaleY, int16 style)
{
	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_LOCALE(style));
	CFont::SetScale(SCREEN_SCALE_X(scaleX), SCREEN_SCALE_Y(scaleY));
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
}

static int
MyPosition(int &total)
{
	total = sStandings.n;
	for(int i = 0; i < sStandings.n; i++)
		if(sStandings.entries[i].id == CNetGame::MyId())
			return i + 1;
	return 0;
}

void
CNetRace::Render(void)
{
	uint32 now = CNetGame::SessionNow();
	char line[96];

	// Invite countdown, top right.
	if(sInvite.open && sRace.phase == PHASE_NONE){
		int secs = Max(0, (int32)(sInvite.deadline - now) / 1000);
		snprintf(line, sizeof(line), sInvite.accepted ? "%s: starting in %ds" : "%s: F3 to join (%ds)", sInvite.route.name, secs);
		SetupFont(0.45f, 0.8f, FONT_STANDARD);
		CFont::SetRightJustifyOn();
		CFont::SetColor(CRGBA(255, 220, 120, 255));
		CFont::PrintString(SCREEN_WIDTH - SCREEN_SCALE_X(20.0f), SCREEN_SCALE_Y(120.0f), MpWideString(line));
		CFont::SetRightJustifyOff();
	}

	if(sRace.phase != PHASE_NONE){
		// 3-2-1-GO, big and centred.
		int32 toGo = (int32)(sRace.startTime - now);
		if(toGo <= 3000 && toGo > -1000){
			const char *text = toGo > 2000 ? "3" : toGo > 1000 ? "2" : toGo > 0 ? "1" : "GO!";
			SetupFont(3.0f, 5.0f, FONT_HEADING);
			CFont::SetCentreOn();
			CFont::SetCentreSize(SCREEN_WIDTH);
			CFont::SetColor(toGo > 0 ? CRGBA(255, 200, 40, 255) : CRGBA(80, 255, 80, 255));
			CFont::PrintString(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - SCREEN_SCALE_Y(60.0f), MpWideString(text));
			CFont::SetCentreOff();
		}

		// POS / LAP / TIME / checkpoint, top right.
		SetupFont(0.6f, 1.1f, FONT_HEADING);
		CFont::SetRightJustifyOn();
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		float x = SCREEN_WIDTH - SCREEN_SCALE_X(20.0f);
		float y = SCREEN_SCALE_Y(150.0f);
		float lh = SCREEN_SCALE_Y(26.0f);
		int total;
		int pos = MyPosition(total);
		if(pos){
			snprintf(line, sizeof(line), "POS %d/%d", pos, total);
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
		}
		if(sRace.route.type == RACE_CIRCUIT){
			int lap = Min((int)sRace.route.laps, 1 + sRace.nextSeq / Max(1, (int)sRace.route.numCheckpoints));
			snprintf(line, sizeof(line), "LAP %d/%d", lap, (int)sRace.route.laps);
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
		}
		char t[16];
		uint32 elapsed = sRace.phase == PHASE_DONE ? sRace.finishTime : (toGo < 0 ? (uint32)(-toGo) : 0);
		FormatTime(elapsed, t, sizeof(t));
		snprintf(line, sizeof(line), "TIME %s", t);
		CFont::PrintString(x, y, MpWideString(line));
		y += lh;
		snprintf(line, sizeof(line), "%d/%d", Min((int)sRace.nextSeq + 1, (int)sRace.total), (int)sRace.total);
		CFont::PrintString(x, y, MpWideString(line));
		CFont::SetRightJustifyOff();
	}

	// Results panel.
	if(sResultsUntil && (int32)(sResultsUntil - CNetGame::NetNow()) > 0){
		float x = SCREEN_WIDTH / 2 - SCREEN_SCALE_X(170.0f);
		float y = SCREEN_SCALE_Y(130.0f);
		float lh = SCREEN_SCALE_Y(24.0f);
		CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(15.0f), y - SCREEN_SCALE_Y(15.0f), x + SCREEN_SCALE_X(355.0f),
			y + lh * (sResults.n + 2)), CRGBA(0, 0, 0, 180));
		SetupFont(0.7f, 1.2f, FONT_HEADING);
		CFont::SetColor(CRGBA(255, 200, 40, 255));
		CFont::PrintString(x, y, MpWideString(sResultsRoute));
		y += lh * 1.4f;
		SetupFont(0.5f, 0.95f, FONT_STANDARD);
		for(int i = 0; i < sResults.n; i++){
			const SRaceStandingEntry &e = sResults.entries[i];
			char t[16];
			if(e.state == RACER_FINISHED)
				FormatTime(e.finishTime, t, sizeof(t));
			else
				strcpy(t, "DNF");
			snprintf(line, sizeof(line), "%d.  %-24s %s", i + 1, NameOf(e.id), t);
			CFont::SetColor(CRemotePlayer::ColourForId(e.id));
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
		}
	}
	CFont::SetDropShadowPosition(0);
}

#endif
