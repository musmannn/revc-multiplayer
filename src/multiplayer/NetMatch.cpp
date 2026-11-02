#include "common.h"

#ifdef MULTIPLAYER

#include "NetMatch.h"
#include "NetGame.h"
#include "NetChat.h"
#include "NetRace.h"
#include "NetVehicles.h"
#include "FreeroamMenu.h"
#include "RemotePlayer.h"
#include "SpawnPoints.h"
#include "MpText.h"
#include "MatchHost.h"
#include "HillLogic.h"
#include "StuntLogic.h"
#include "NetCombat.h"
#include "DMAudio.h"
#include "Automobile.h"
#include "Bike.h"
#include "Radar.h"
#include "SpecialFX.h"
#include "Timer.h"
#include "Font.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Sprite2d.h"
#include "Streaming.h"
#include "World.h"

static const uint32 RESULTS_SHOW_MS = 10000;
static const uint32 STUNT_CAR_RESPAWN_MS = 3000;	// out of the stunt car this long: a new one
static const uint32 STUNT_POPUP_MS = 2500;
static const uint16 DEFAULT_STUNT_VEHICLE = 191;	// PCJ-600
static const uint32 HILL_MARKER_ID = 0x4D505210;

class CGameMatchSink : public IMatchHostSink
{
	static void Send(uint8 route, uint8 target, bool reliable, const CByteWriter &w)
	{
		CNetGame::Client().SendGame(route, target, reliable, w);
	}
public:
	void SendInvite(const SMatchInvite &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendStart(const SMatchStart &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendScores(const SMatchScores &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, false, w); }
	void SendEnd(const SMatchScores &m) override { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
	void SendCancel(const SMatchCancel &m, uint8 target) override
	{
		CByteWriter w;
		m.Write(w);
		if(target == MP_PLAYER_NONE)
			Send(MP_ROUTE_ALL, 0, true, w);
		else
			Send(MP_ROUTE_PLAYER, target, true, w);
	}
};
static CMatchHost sHost;
static CGameMatchSink sSink;

static SRaceRoute sArenas[CNetMatch::MAX_ARENAS];
static int sNumArenas;

void
CNetMatch::LoadArenas(void)
{
	sNumArenas = CNetRace::LoadRouteDir("arenas", sArenas, MAX_ARENAS, true);
}

int
CNetMatch::NumArenas(void)
{
	return sNumArenas;
}

const SRaceRoute*
CNetMatch::GetArena(int i)
{
	return i >= 0 && i < sNumArenas ? &sArenas[i] : nil;
}

struct SMatchInviteState
{
	bool open;
	bool accepted;
	SMatchInvite invite;
};
static SMatchInviteState sInvite;

enum eMatchPhase { PHASE_NONE, PHASE_COUNTDOWN, PHASE_PLAYING };
struct SLocalMatch
{
	eMatchPhase phase;
	SMatchStart start;
	uint8 myTeam;
	int countdownShown;
	int slot;			// our place in START: picks the arena spawn
	bool hasArena;
	SRaceRoute arena;
	bool overrideSet;		// we set CNetCombat's respawn point
	// Hold the Hill
	CHillState hill;
	int shownHill;
	int32 hillBlip;
	int hillStatus;			// HILL_*
	// Stunt Challenge
	CStuntTracker stunt;
	CVehicle *car;			// registered reference
	uint32 outOfCarSince;		// NetNow, 0 = in it
	uint32 retryAt;			// NetNow
	SStuntResult lastStunt;
	bool lastCrashed;
	uint32 popupUntil;		// NetNow
};
enum { HILL_NOBODY, HILL_YOURS, HILL_THEIRS, HILL_CONTESTED };
static SLocalMatch sMatch;
static int sArenaChoice;	// 0: free-roam spawns, else arena sArenaChoice-1
static SMatchScores sScores;		// latest, for the HUD and the scoreboard
static SMatchScores sResults;
static uint8 sResultsMode;
static uint32 sResultsUntil;		// NetNow
static char sNames[MP_MAX_PLAYERS][MP_MAX_NAME_LEN + 1];	// cached at START
static bool sMenuRegistered;
static uint16 sLastFinishedMatch;	// ended or cancelled: its late SCORES are dropped
static bool sHaveFinished;

static const char*
NameOf(uint8 id)
{
	// The name at the start of the match first: ids get reused.
	if(id < MP_MAX_PLAYERS && sNames[id][0])
		return sNames[id];
	const char *n = CNetGame::PlayerName(id);
	return n ? n : "?";
}

static const char*
ModeName(uint8 mode)
{
	switch(mode){
	case MATCH_TDM: return "Team Deathmatch";
	case MATCH_KOTH: return "Hold the Hill";
	case MATCH_TKOTH: return "Team Hold the Hill";
	case MATCH_STUNT: return "Stunt Challenge";
	default: return "Deathmatch";
	}
}

static const char*
ModeTitle(uint8 mode)
{
	switch(mode){
	case MATCH_TDM: return "TEAM DEATHMATCH";
	case MATCH_KOTH: return "HOLD THE HILL";
	case MATCH_TKOTH: return "TEAM HOLD THE HILL";
	case MATCH_STUNT: return "STUNT CHALLENGE";
	default: return "DEATHMATCH";
	}
}

static CRGBA
TeamRGBA(uint8 team)
{
	return team == MP_TEAM_BLUE ? CRGBA(80, 140, 255, 255) : CRGBA(255, 70, 70, 255);
}

// ---- Menu

static int
ModeCount(void)
{
	return MATCH_NUM_MODES;
}

static const char*
ModeLabel(int i)
{
	return ModeName((uint8)i);
}

// The arena list: "Free-roam spawns", then every arena; ">" marks the choice.
static int
ArenaCount(void)
{
	return 1 + sNumArenas;
}

static const char*
ArenaLabel(int i)
{
	static char labels[1 + CNetMatch::MAX_ARENAS][48];
	if(i < 0 || i > sNumArenas)
		return "";
	snprintf(labels[i], sizeof(labels[i]), "%s%s", i == sArenaChoice ? "> " : "  ",
		i == 0 ? "Free-roam spawns" : sArenas[i - 1].name);
	return labels[i];
}

static void
SelectArena(int i)
{
	if(i < 0 || i > sNumArenas)
		return;
	sArenaChoice = i;
	char line[96];
	snprintf(line, sizeof(line), "Matches will use %s", i == 0 ? "the free-roam spawns" : sArenas[i - 1].name);
	CNetChat::AddLine(line, CNetChat::NOTICE);
}

void
CNetMatch::RequestMatch(int mode)
{
	SMatchRequest msg;
	msg.mode = mode >= 0 && mode < MATCH_NUM_MODES ? (uint8)mode : (uint8)MATCH_DM;
	int arena = sArenaChoice > sNumArenas ? 0 : sArenaChoice;
	if(arena == 0 && MpModeNeedsArena(msg.mode) && sNumArenas > 0)
		arena = 1;	// these modes can't do without one
	if(arena > 0){
		msg.hasArena = true;
		msg.arena = sArenas[arena - 1];
	}
	CByteWriter w;
	msg.Write(w);
	CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);
}

void
CNetMatch::Init(void)
{
	LoadArenas();
	if(sArenaChoice > sNumArenas)
		sArenaChoice = 0;
	if(!sMenuRegistered){
		CFreeroamMenu::AddList("Start match >", ModeCount, ModeLabel, RequestMatch);
		CFreeroamMenu::AddList("Match arena >", ArenaCount, ArenaLabel, SelectArena);
		sMenuRegistered = true;
	}
}

// ---- Local player

bool
CNetMatch::IsPlaying(void)
{
	return sMatch.phase != PHASE_NONE;
}

bool
CNetMatch::HostBusy(void)
{
	return CNetGame::IsHost() && sHost.IsBusy();
}

bool
CNetMatch::IsTeammate(uint8 id)
{
	if(sMatch.phase == PHASE_NONE || !MpIsTeamMode(sMatch.start.mode))
		return false;
	for(int i = 0; i < sMatch.start.n; i++)
		if(sMatch.start.ids[i] == id)
			return sMatch.start.teams[i] == sMatch.myTeam;
	return false;
}

bool
CNetMatch::TeamColour(uint8 id, CRGBA &out)
{
	if(sMatch.phase == PHASE_NONE || !MpIsTeamMode(sMatch.start.mode))
		return false;
	for(int i = 0; i < sMatch.start.n; i++)
		if(sMatch.start.ids[i] == id){
			out = TeamRGBA(sMatch.start.teams[i]);
			return true;
		}
	return false;
}

void
CNetMatch::GiveMatchLoadout(CPlayerPed *player)
{
	if(player == nil)
		return;
	player->ClearWeapons();	// the same loadout for everyone, every life
	CSpawnPoints::GiveWeaponWithModel(player, WEAPONTYPE_COLT45, 120);
	CSpawnPoints::GiveWeaponWithModel(player, WEAPONTYPE_UZI, 300);
	CSpawnPoints::GiveWeaponWithModel(player, WEAPONTYPE_SHOTGUN, 40);
	player->SetCurrentWeapon(WEAPONTYPE_UZI);
	player->m_fHealth = CWorld::Players[CWorld::PlayerInFocus].m_nMaxHealth;
	player->m_fArmour = 100.0f;
}

static void
Freeze(bool on)
{
	CPad *pad = CPad::GetPad(0);
	if(on)
		pad->SetDisablePlayerControls(CNetMatch::PLAYERCONTROL_MP_MATCH);
	else
		pad->SetEnablePlayerControls(CNetMatch::PLAYERCONTROL_MP_MATCH);
}

static void
SetCar(CVehicle *v)
{
	if(sMatch.car)
		sMatch.car->CleanUpOldReference((CEntity**)&sMatch.car);
	sMatch.car = v;
	if(v)
		v->RegisterReference((CEntity**)&sMatch.car);
}

static void
FreezeCar(bool on)
{
	if(sMatch.car){
		sMatch.car->bIsFrozen = on;
		sMatch.car->bInfiniteMass = on;
	}
}

// Our spawn: the arena's slot for us, or a free-roam spawn point.
static void
MatchSpawnPoint(CVector &pos, float &heading)
{
	if(sMatch.hasArena && sMatch.arena.numGrid > 0){
		const SRaceGridSlot &g = sMatch.arena.grid[sMatch.slot % sMatch.arena.numGrid];
		CSpawnPoints::PickNear(CVector(g.pos[0], g.pos[1], g.pos[2]), pos);
		heading = DEGTORAD(g.heading);
		return;
	}
	CSpawnPoints::Pick(CNetGame::MyId(), pos, heading);
}

static uint16
StuntVehicle(void)
{
	uint16 m = sMatch.hasArena ? sMatch.arena.vehicleModel : 0;
	return m && CNetRace::IsLandVehicle(m) ? m : DEFAULT_STUNT_VEHICLE;
}

static void
SpawnStuntCar(void)
{
	uint32 now = CNetGame::NetNow();
	if(sMatch.retryAt && (int32)(now - sMatch.retryAt) < 0)
		return;
	CVector pos;
	float heading;
	MatchSpawnPoint(pos, heading);
	CVehicle *v = CFreeroamMenu::SpawnAndEnter(StuntVehicle(), pos, heading);
	if(v == nil){
		sMatch.retryAt = now + 2000;	// a blocking model load; don't retry every frame
		return;
	}
	sMatch.retryAt = 0;
	SetCar(v);
	sMatch.outOfCarSince = 0;
	sMatch.stunt.Reset();
	if(sMatch.phase == PHASE_COUNTDOWN)
		FreezeCar(true);
}

static void
SpawnForMatch(void)
{
	CVector pos;
	float heading;
	MatchSpawnPoint(pos, heading);
	if(sMatch.hasArena){
		// Deaths respawn here too, not at the free-roam points.
		CNetCombat::SetRespawnOverride(pos, heading);
		sMatch.overrideSet = true;
	}
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || player->DyingOrDead())
		return;	// the respawn puts us in, with the match loadout
	if(sMatch.start.mode == MATCH_STUNT){
		SpawnStuntCar();
		CNetMatch::GiveMatchLoadout(player);
		return;
	}
	if(player->bInVehicle)
		CNetVehicles::WarpLocalPlayerOut();
	CStreaming::LoadSceneCollision(pos);
	player->Teleport(pos);
	player->m_fRotationCur = player->m_fRotationDest = heading;
	player->SetHeading(heading);
	CNetMatch::GiveMatchLoadout(player);
}

static void
ClearHill(void)
{
	if(sMatch.hillBlip >= 0)
		CRadar::ClearBlip(sMatch.hillBlip);
	sMatch.hillBlip = -1;
	sMatch.shownHill = -1;
}

static void
LeaveMatch(void)
{
	if(sMatch.phase == PHASE_NONE)
		return;
	Freeze(false);
	FreezeCar(false);
	SetCar(nil);	// the car stays where it is
	ClearHill();
	if(sMatch.overrideSet)
		CNetCombat::ClearRespawnOverride();
	sMatch.overrideSet = false;
	sMatch.phase = PHASE_NONE;
}

static void
BeginMatch(const SMatchStart &m, int index)
{
	bool arenaOk = sInvite.invite.matchId == m.matchId && sInvite.invite.hasArena;
	if(MpModeNeedsArena(m.mode) && !arenaOk){
		CNetChat::AddLine("Can't join: missed this match's invite", CNetChat::NOTICE);
		return;
	}
	LeaveMatch();
	memset(&sMatch, 0, sizeof(sMatch));
	sMatch.stunt.Reset();
	sMatch.hillBlip = -1;
	sMatch.shownHill = -1;
	sMatch.start = m;
	sMatch.myTeam = m.teams[index];
	sMatch.slot = index;
	sMatch.hasArena = arenaOk;
	if(arenaOk){
		sMatch.arena = sInvite.invite.arena;
		sMatch.hill.Start(sMatch.arena, m.startTime);
	}
	sMatch.phase = PHASE_COUNTDOWN;
	sMatch.countdownShown = 4;
	memset(&sScores, 0, sizeof(sScores));
	CFreeroamMenu::Close();
	SpawnForMatch();
	Freeze(true);
	char line[128];
	const char *team = sMatch.myTeam == MP_TEAM_BLUE ? "Blue" : "Red";
	switch(m.mode){
	case MATCH_TDM:
		snprintf(line, sizeof(line), "%s: you're on %s. First team to %u wins", ModeName(m.mode), team, (unsigned)m.scoreLimit);
		break;
	case MATCH_KOTH:
		snprintf(line, sizeof(line), "Hold the Hill: stand in the hill alone. First to %u seconds wins", (unsigned)m.scoreLimit);
		break;
	case MATCH_TKOTH:
		snprintf(line, sizeof(line), "Team Hold the Hill: you're on %s. Keep the hill clear of enemies; %u to win", team, (unsigned)m.scoreLimit);
		break;
	case MATCH_STUNT:
		snprintf(line, sizeof(line), "Stunt Challenge: %u minutes. Jump, flip and spin - land on your wheels to score",
			(unsigned)(m.timeLimitMs / 60000));
		break;
	default:
		snprintf(line, sizeof(line), "%s: first to %u kills wins", ModeName(m.mode), (unsigned)m.scoreLimit);
		break;
	}
	CNetChat::AddLine(line, CNetChat::NOTICE);
}

// ---- Host

static void
HostRequest(uint8 from, const SMatchRequest &m, uint32 now)
{
	if(CNetRace::HostBusy() && !sHost.IsBusy()){
		SMatchCancel busy;
		busy.matchId = sHost.MatchId();
		busy.reason = MATCH_BUSY;
		sSink.SendCancel(busy, from);
		return;
	}
	sHost.Request(from, m.mode, true, now, &sSink, m.hasArena ? &m.arena : nil);
}

void
CNetMatch::OnRespawned(uint8 id)
{
	if(CNetGame::IsHost())
		sHost.OnRespawned(id);
}

void
CNetMatch::OnDied(uint8 victim, uint8 killer)
{
	if(CNetGame::IsHost())
		sHost.OnDeath(victim, killer, CNetGame::SessionNow());
}

void
CNetMatch::OnPlayerJoined(uint8 id)
{
	SMatchInvite invite;
	if(CNetGame::IsHost() && sHost.GetInvite(invite)){
		CByteWriter w;
		invite.Write(w);
		CNetGame::Client().SendGame(MP_ROUTE_PLAYER, id, true, w);
	}
}

void
CNetMatch::OnPlayerLeft(uint8 id)
{
	if(CNetGame::IsHost())
		sHost.OnLeft(id);
	// The relay reuses ids: a newcomer with this one isn't in our match.
	for(int i = 0; i < sMatch.start.n; i++)
		if(sMatch.start.ids[i] == id){
			sMatch.start.ids[i] = MP_PLAYER_NONE;
			sMatch.start.teams[i] = MP_TEAM_NONE;
		}
}

// ---- Per frame

static bool
PositionOf(uint8 id, CVector &pos, bool &alive)
{
	if(id == CNetGame::MyId()){
		CPlayerPed *p = FindPlayerPed();
		if(p == nil)
			return false;
		pos = p->bInVehicle && p->m_pMyVehicle ? p->m_pMyVehicle->GetPosition() : p->GetPosition();
		alive = !p->DyingOrDead();
		return true;
	}
	CRemotePlayer *rp = CRemotePlayer::Get(id);
	if(rp == nil || !rp->m_bHaveState)
		return false;
	pos = CVector(rp->m_last.pos[0], rp->m_last.pos[1], rp->m_last.pos[2]);
	alive = (rp->m_last.flags & PSF_DEAD) == 0;
	return true;
}

// Host: feed CMatchHost the positions for Hold the Hill.
static void
HostPositions(void)
{
	if(!sHost.IsRunning())
		return;
	const CMatchState &m = sHost.Match();
	if(m.Mode() != MATCH_KOTH && m.Mode() != MATCH_TKOTH)
		return;
	for(int id = 0; id < MP_MAX_PLAYERS; id++){
		CVector pos;
		bool alive;
		if(m.Get((uint8)id) && PositionOf((uint8)id, pos, alive)){
			float p[3] = { pos.x, pos.y, pos.z };
			sHost.SetPosition((uint8)id, p);
		}
	}
}

// Our view of the hill: who's on it, from the positions we have.
static void
UpdateHill(uint32 now)
{
	int hill = sMatch.hill.ActiveHill(now);
	const SRaceCheckpoint *h = sMatch.hill.Hill(hill);
	if(h == nil)
		return;
	SHillPlayer players[MP_MAX_PLAYERS];
	int n = 0;
	for(int i = 0; i < sMatch.start.n; i++){
		uint8 id = sMatch.start.ids[i];
		CVector pos;
		bool alive;
		if(id == MP_PLAYER_NONE || !PositionOf(id, pos, alive))
			continue;
		SHillPlayer &p = players[n++];
		p.id = id;
		p.team = sMatch.start.teams[i];
		p.pos[0] = pos.x;
		p.pos[1] = pos.y;
		p.pos[2] = pos.z;
		p.alive = alive;
	}
	uint8 scorers[MP_MAX_PLAYERS];
	bool contested;
	int k = sMatch.hill.Scorers(now, MpIsTeamMode(sMatch.start.mode), players, n, scorers, &contested);
	bool mine = false;
	for(int i = 0; i < k; i++)
		if(scorers[i] == CNetGame::MyId() || (MpIsTeamMode(sMatch.start.mode) && CNetMatch::IsTeammate(scorers[i])))
			mine = true;
	int status = contested ? HILL_CONTESTED : k == 0 ? HILL_NOBODY : mine ? HILL_YOURS : HILL_THEIRS;
	if(status != sMatch.hillStatus && status == HILL_YOURS)
		DMAudio.PlayFrontEndSound(SOUND_PART_MISSION_COMPLETE, 0);
	sMatch.hillStatus = status;

	if(hill != sMatch.shownHill){
		ClearHill();
		sMatch.shownHill = hill;
		sMatch.hillBlip = CRadar::SetCoordBlip(BLIP_COORD, CVector(h->pos[0], h->pos[1], h->pos[2]), 0, BLIP_DISPLAY_BOTH);
		CRadar::ChangeBlipColour(sMatch.hillBlip, RADAR_TRACE_LIGHT_BLUE);
		CRadar::ChangeBlipScale(sMatch.hillBlip, 3);
		if(sMatch.phase == PHASE_PLAYING)
			CNetChat::AddLine("The hill has moved", CNetChat::NOTICE);
	}
	uint8 r = 60, g = 140, b = 255;
	if(status == HILL_YOURS){
		r = 60; g = 255; b = 60;
	}else if(status == HILL_THEIRS || status == HILL_CONTESTED){
		r = 255; g = 60; b = 60;
	}
	CVector pos(h->pos[0], h->pos[1], h->pos[2]);
	C3dMarkers::PlaceMarkerSet(HILL_MARKER_ID, MARKERTYPE_CYLINDER, pos, h->radius, r, g, b, 120, 2048, 0.15f, 0);
}

static uint8
WheelsOnGround(CVehicle *v)
{
	if(v->IsCar())
		return ((CAutomobile*)v)->m_nWheelsOnGround;
	if(v->IsBike())
		return ((CBike*)v)->m_nWheelsOnGround;
	return 1;	// boats and the like never stunt
}

static void
UpdateStunts(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	uint32 netNow = CNetGame::NetNow();
	if(player->DyingOrDead()){
		sMatch.stunt.Reset();
		sMatch.outOfCarSince = 0;
		return;
	}
	CVehicle *v = player->bInVehicle ? player->m_pMyVehicle : nil;
	bool wrecked = sMatch.car && sMatch.car->GetStatus() == STATUS_WRECKED;
	if(v == nil || v != sMatch.car || wrecked){
		sMatch.stunt.Reset();
		if(sMatch.outOfCarSince == 0)
			sMatch.outOfCarSince = netNow | 1;
		else if(netNow - sMatch.outOfCarSince >= STUNT_CAR_RESPAWN_MS){
			if(v)
				CNetVehicles::WarpLocalPlayerOut();
			SpawnStuntCar();
		}
		return;
	}
	sMatch.outOfCarSince = 0;

	SStuntSample s;
	s.dtMs = CTimer::GetTimeStepInMilliseconds();
	s.airborne = WheelsOnGround(v) == 0;
	CVector pos = v->GetPosition(), up = v->GetUp();
	s.pos[0] = pos.x;
	s.pos[1] = pos.y;
	s.pos[2] = pos.z;
	s.up[0] = up.x;
	s.up[1] = up.y;
	s.up[2] = up.z;
	s.heading = v->GetForward().Heading();
	SStuntResult r;
	bool crashed;
	if(sMatch.stunt.Update(s, r, &crashed)){
		SStunt msg;
		msg.matchId = sMatch.start.matchId;
		msg.points = r.points;
		msg.airMs = r.airMs;
		msg.flips = r.flips;
		msg.spins = r.spins;
		CByteWriter w;
		msg.Write(w);
		CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);
		sMatch.lastStunt = r;
		sMatch.lastCrashed = false;
		sMatch.popupUntil = netNow + STUNT_POPUP_MS;
		DMAudio.PlayFrontEndSound(SOUND_PART_MISSION_COMPLETE, 0);
	}else if(crashed){
		sMatch.lastCrashed = true;
		sMatch.popupUntil = netNow + STUNT_POPUP_MS;
	}
}

void
CNetMatch::Process(void)
{
	uint32 now = CNetGame::SessionNow();
	if(CNetGame::IsHost()){
		HostPositions();
		sHost.Update(now, &sSink);
	}

	if(sInvite.open && (int32)(now - sInvite.invite.deadline) >= 0)
		sInvite.open = false;
	if(sInvite.open && !sInvite.accepted && sMatch.phase == PHASE_NONE && !CNetRace::IsRacing() &&
	   !CNetChat::IsTyping() && CPad::GetPad(0)->GetFJustDown(2)){	// F3
		SMatchAccept msg;
		msg.matchId = sInvite.invite.matchId;
		CByteWriter w;
		msg.Write(w);
		CNetGame::Client().SendGame(MP_ROUTE_HOST, 0, true, w);
		sInvite.accepted = true;
		CNetChat::AddLine("You're in the match", CNetChat::NOTICE);
	}

	CPlayerPed *player = FindPlayerPed();
	switch(sMatch.phase){
	case PHASE_COUNTDOWN: {
		int32 toGo = (int32)(sMatch.start.startTime - now);
		int shown = toGo > 2000 ? 3 : toGo > 1000 ? 2 : toGo > 0 ? 1 : 0;
		if(toGo <= 3000 && shown < sMatch.countdownShown){
			static const int16 sounds[4] = { SOUND_RACE_START_GO, SOUND_RACE_START_1, SOUND_RACE_START_2, SOUND_RACE_START_3 };
			DMAudio.PlayFrontEndSound(sounds[shown], 0);
			sMatch.countdownShown = shown;
		}
		if(toGo <= 0){
			Freeze(false);
			FreezeCar(false);
			sMatch.phase = PHASE_PLAYING;
		}else{
			Freeze(true);	// a respawn re-enables every control
			FreezeCar(true);
		}
		if(sMatch.start.mode == MATCH_KOTH || sMatch.start.mode == MATCH_TKOTH)
			UpdateHill(now);
		break;
	}
	case PHASE_PLAYING:
		if(sMatch.start.mode == MATCH_KOTH || sMatch.start.mode == MATCH_TKOTH)
			UpdateHill(now);
		else if(sMatch.start.mode == MATCH_STUNT)
			UpdateStunts();
		break;
	default:
		break;
	}
}

// ---- Messages

void
CNetMatch::OnMessage(uint8 from, uint8 type, CByteReader &body)
{
	uint8 me = CNetGame::MyId();
	uint32 now = CNetGame::SessionNow();
	switch(type){
	case GMSG_MATCH_REQUEST: {
		SMatchRequest m;
		if(CNetGame::IsHost() && m.Read(body))
			HostRequest(from, m, now);
		break;
	}
	case GMSG_MATCH_ACCEPT: {
		SMatchAccept m;
		if(CNetGame::IsHost() && m.Read(body))
			sHost.OnAccept(from, m.matchId, now);
		break;
	}
	case GMSG_STUNT: {
		SStunt m;
		if(CNetGame::IsHost() && m.Read(body))
			sHost.OnStunt(from, m, now);
		break;
	}
	case GMSG_MATCH_INVITE: {
		SMatchInvite m;
		if(from != 0 || !m.Read(body))
			return;
		if(sInvite.open && sInvite.invite.matchId == m.matchId)
			return;	// a resend for a late joiner
		sInvite.open = true;
		sInvite.accepted = m.startedBy == me;
		sInvite.invite = m;
		char line[128], where[48] = "";
		if(m.hasArena)
			snprintf(where, sizeof(where), " at %s", m.arena.name);
		if(m.startedBy == me)
			snprintf(line, sizeof(line), "You started %s%s - others have 15s to join", ModeName(m.mode), where);
		else if(m.startedBy == MP_PLAYER_NONE || (m.startedBy == 0 && CNetGame::IsDedicated()))
			snprintf(line, sizeof(line), "Next up: %s%s - press F3 to join (15s)", ModeName(m.mode), where);
		else
			snprintf(line, sizeof(line), "%s started %s%s - press F3 to join (15s)", NameOf(m.startedBy), ModeName(m.mode), where);
		CNetChat::AddLine(line, CNetChat::NOTICE);
		break;
	}
	case GMSG_MATCH_START: {
		SMatchStart m;
		if(from != 0 || !m.Read(body))
			return;
		sInvite.open = false;
		memset(sNames, 0, sizeof(sNames));
		for(int i = 0; i < m.n; i++)
			if(m.ids[i] < MP_MAX_PLAYERS && CNetGame::PlayerName(m.ids[i]))
				strcpy(sNames[m.ids[i]], CNetGame::PlayerName(m.ids[i]));
		for(int i = 0; i < m.n; i++)
			if(m.ids[i] == me && !CNetRace::IsRacing())
				BeginMatch(m, i);
		break;
	}
	case GMSG_MATCH_SCORES: {
		SMatchScores m;
		if(from == 0 && m.Read(body) && !(sHaveFinished && m.matchId == sLastFinishedMatch))
			sScores = m;
		break;
	}
	case GMSG_MATCH_END: {
		SMatchScores m;
		if(from != 0 || !m.Read(body))
			return;
		bool mine = sMatch.phase != PHASE_NONE && sMatch.start.matchId == m.matchId;
		uint8 mode;
		if(mine)
			mode = sMatch.start.mode;
		else if(sInvite.invite.matchId == m.matchId)
			mode = sInvite.invite.mode;
		else
			mode = m.n && m.entries[0].team != MP_TEAM_NONE ? (uint8)MATCH_TDM : (uint8)MATCH_DM;
		char line[128];
		if(MpIsTeamMode(mode)){
			int16 r = m.teamScore[MP_TEAM_RED], b = m.teamScore[MP_TEAM_BLUE];
			uint8 winner = m.winnerTeam;
			bool forfeit = winner != MP_TEAM_NONE && (winner == MP_TEAM_RED ? r <= b : b <= r);
			if(winner == MP_TEAM_NONE)
				snprintf(line, sizeof(line), "%s: a draw, %d-%d", ModeName(mode), (int)r, (int)b);
			else
				snprintf(line, sizeof(line), "%s team won the %s %d-%d (Red-Blue)%s",
					winner == MP_TEAM_BLUE ? "Blue" : "Red", ModeName(mode), (int)r, (int)b, forfeit ? ", the other team left" : "");
		}else if(m.n)
			snprintf(line, sizeof(line), "%s won the %s with %d", NameOf(m.entries[0].id), ModeName(mode), (int)m.entries[0].score);
		else
			snprintf(line, sizeof(line), "The %s is over", ModeName(mode));
		CNetChat::AddLine(line, CNetChat::NOTICE);
		if(mine){
			sResults = m;
			sResultsMode = mode;
			sResultsUntil = CNetGame::NetNow() + RESULTS_SHOW_MS;
			LeaveMatch();
		}
		memset(&sScores, 0, sizeof(sScores));
		sLastFinishedMatch = m.matchId;
		sHaveFinished = true;
		break;
	}
	case GMSG_MATCH_CANCEL: {
		SMatchCancel m;
		if(from != 0 || !m.Read(body))
			return;
		const char *why = m.reason == MATCH_BUSY ? "A match or race is already on" :
			m.reason == MATCH_ABORTED ? "The match was called off" :
			m.reason == MATCH_NO_ARENA ? "That mode needs an arena (data/mp/arenas, or make one with /rec arena)" :
			"Not enough players joined the match";
		CNetChat::AddLine(why, CNetChat::NOTICE);
		if(m.reason != MATCH_BUSY && m.reason != MATCH_NO_ARENA && sInvite.invite.matchId == m.matchId)
			sInvite.open = false;
		if(m.reason == MATCH_ABORTED && sMatch.phase != PHASE_NONE && sMatch.start.matchId == m.matchId)
			LeaveMatch();
		if(m.reason != MATCH_BUSY && m.reason != MATCH_NO_ARENA){
			memset(&sScores, 0, sizeof(sScores));
			sLastFinishedMatch = m.matchId;
			sHaveFinished = true;
		}
		break;
	}
	default:
		break;
	}
}

void
CNetMatch::Reset(void)
{
	if(CNetGame::IsHost() && sHost.IsBusy()){
		SMatchCancel cancel;
		cancel.matchId = sHost.MatchId();
		cancel.reason = MATCH_ABORTED;
		sSink.SendCancel(cancel, MP_PLAYER_NONE);
	}
	LeaveMatch();
	sHost.Reset();
	memset(&sInvite, 0, sizeof(sInvite));
	memset(&sScores, 0, sizeof(sScores));
	sResultsUntil = 0;
}

// ---- HUD

static void
SetupFont(float sx, float sy, int16 style)
{
	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_LOCALE(style));
	CFont::SetScale(SCREEN_SCALE_X(sx), SCREEN_SCALE_Y(sy));
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
}

static const SMatchScoreEntry*
FindEntry(const SMatchScores &s, uint8 id, int *place)
{
	for(int i = 0; i < s.n; i++)
		if(s.entries[i].id == id){
			if(place)
				*place = i + 1;
			return &s.entries[i];
		}
	return nil;
}

static void
DrawBoard(const SMatchScores &s, uint8 mode, const char *title, float y)
{
	float x = SCREEN_WIDTH / 2 - SCREEN_SCALE_X(200.0f);
	float lh = SCREEN_SCALE_Y(22.0f);
	int rows = Max(1, (int)s.n);
	CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(15.0f), y - SCREEN_SCALE_Y(12.0f), x + SCREEN_SCALE_X(415.0f),
		y + lh * (rows + 2.5f)), CRGBA(0, 0, 0, 180));
	char line[96];
	SetupFont(0.65f, 1.15f, FONT_HEADING);
	CFont::SetColor(CRGBA(255, 200, 40, 255));
	if(MpIsTeamMode(mode))
		snprintf(line, sizeof(line), "%s   Red %d - Blue %d", title, (int)s.teamScore[MP_TEAM_RED], (int)s.teamScore[MP_TEAM_BLUE]);
	else
		snprintf(line, sizeof(line), "%s", title);
	CFont::PrintString(x, y, MpWideString(line));
	y += lh * 1.4f;
	SetupFont(0.45f, 0.85f, FONT_STANDARD);
	CFont::SetColor(CRGBA(180, 180, 180, 255));
	CFont::PrintString(x, y, MpWideString("     Name                           Score   Kills  Deaths"));
	y += lh;
	for(int i = 0; i < s.n; i++){
		const SMatchScoreEntry &e = s.entries[i];
		snprintf(line, sizeof(line), "%d.  %-28s %5d   %5u  %5u", i + 1, NameOf(e.id), (int)e.score, (unsigned)e.kills, (unsigned)e.deaths);
		CFont::SetColor(MpIsTeamMode(mode) ? TeamRGBA(e.team) : CRemotePlayer::ColourForId(e.id));
		CFont::PrintString(x, y, MpWideString(line));
		y += lh;
	}
}

void
CNetMatch::Render(void)
{
	uint32 now = CNetGame::SessionNow();
	char line[96];

	if(sInvite.open && sMatch.phase == PHASE_NONE){
		int secs = Max(0, (int32)(sInvite.invite.deadline - now) / 1000);
		snprintf(line, sizeof(line), sInvite.accepted ? "%s: starting in %ds" : "%s: F3 to join (%ds)", ModeName(sInvite.invite.mode), secs);
		SetupFont(0.45f, 0.8f, FONT_STANDARD);
		CFont::SetRightJustifyOn();
		CFont::SetColor(CRGBA(255, 220, 120, 255));
		CFont::PrintString(SCREEN_WIDTH - SCREEN_SCALE_X(20.0f), SCREEN_SCALE_Y(140.0f), MpWideString(line));
		CFont::SetRightJustifyOff();
	}

	if(sMatch.phase != PHASE_NONE){
		int32 toGo = (int32)(sMatch.start.startTime - now);
		if(toGo <= 3000 && toGo > -1000){
			const char *go = sMatch.start.mode == MATCH_STUNT ? "GO!" : "FIGHT!";
			const char *text = toGo > 2000 ? "3" : toGo > 1000 ? "2" : toGo > 0 ? "1" : go;
			SetupFont(3.0f, 5.0f, FONT_HEADING);
			CFont::SetCentreOn();
			CFont::SetCentreSize(SCREEN_WIDTH);
			CFont::SetColor(toGo > 0 ? CRGBA(255, 200, 40, 255) : CRGBA(255, 80, 80, 255));
			CFont::PrintString(SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - SCREEN_SCALE_Y(60.0f), MpWideString(text));
			CFont::SetCentreOff();
		}

		SetupFont(0.6f, 1.1f, FONT_HEADING);
		CFont::SetRightJustifyOn();
		float x = SCREEN_WIDTH - SCREEN_SCALE_X(20.0f);
		float y = SCREEN_SCALE_Y(150.0f);
		float lh = SCREEN_SCALE_Y(26.0f);
		int place = 0;
		const SMatchScoreEntry *mine = FindEntry(sScores, CNetGame::MyId(), &place);
		if(MpIsTeamMode(sMatch.start.mode)){
			CFont::SetColor(TeamRGBA(MP_TEAM_RED));
			snprintf(line, sizeof(line), "RED %d", (int)sScores.teamScore[MP_TEAM_RED]);
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
			CFont::SetColor(TeamRGBA(MP_TEAM_BLUE));
			snprintf(line, sizeof(line), "BLUE %d", (int)sScores.teamScore[MP_TEAM_BLUE]);
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
		}else{
			CFont::SetColor(CRGBA(255, 255, 255, 255));
			if(sMatch.start.scoreLimit)
				snprintf(line, sizeof(line), "SCORE %d/%u", mine ? (int)mine->score : 0, (unsigned)sMatch.start.scoreLimit);
			else
				snprintf(line, sizeof(line), "SCORE %d", mine ? (int)mine->score : 0);
			CFont::PrintString(x, y, MpWideString(line));
			y += lh;
			if(place){
				snprintf(line, sizeof(line), "POS %d/%d", place, (int)sScores.n);
				CFont::PrintString(x, y, MpWideString(line));
				y += lh;
			}
		}
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		int32 left = (int32)(sMatch.start.startTime + sMatch.start.timeLimitMs - now);
		uint32 secs = left > 0 ? (uint32)left / 1000 : 0;
		snprintf(line, sizeof(line), "TIME %02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
		CFont::PrintString(x, y, MpWideString(line));
		y += lh;

		if(sMatch.start.mode == MATCH_KOTH || sMatch.start.mode == MATCH_TKOTH){
			static const char *status[] = { "HILL: EMPTY", "HILL: HOLDING", "HILL: TAKEN", "HILL: CONTESTED" };
			static const CRGBA colours[] = { CRGBA(150, 200, 255, 255), CRGBA(80, 255, 80, 255), CRGBA(255, 80, 80, 255), CRGBA(255, 160, 40, 255) };
			CFont::SetColor(colours[sMatch.hillStatus & 3]);
			CFont::PrintString(x, y, MpWideString(status[sMatch.hillStatus & 3]));
			y += lh;
			int32 moveIn = (int32)(sMatch.hill.NextMove(now) - now);
			uint32 ms = moveIn > 0 ? (uint32)moveIn / 1000 : 0;
			CFont::SetColor(CRGBA(255, 255, 255, 255));
			snprintf(line, sizeof(line), "MOVES %u:%02u", (unsigned)(ms / 60), (unsigned)(ms % 60));
			CFont::PrintString(x, y, MpWideString(line));
		}
		if(sMatch.start.mode == MATCH_STUNT && sMatch.stunt.InAir() && sMatch.stunt.PendingPoints() > 0){
			CFont::SetColor(CRGBA(255, 220, 80, 255));
			snprintf(line, sizeof(line), "AIR %u", (unsigned)sMatch.stunt.PendingPoints());
			CFont::PrintString(x, y, MpWideString(line));
		}
		CFont::SetRightJustifyOff();

		// The last stunt, big and centred.
		if(sMatch.start.mode == MATCH_STUNT && sMatch.popupUntil && (int32)(sMatch.popupUntil - CNetGame::NetNow()) > 0){
			if(sMatch.lastCrashed)
				strcpy(line, "CRASHED!");
			else{
				const SStuntResult &r = sMatch.lastStunt;
				char extra[32] = "";
				if(r.flips || r.spins)
					snprintf(extra, sizeof(extra), "  %ux FLIP %ux SPIN", (unsigned)r.flips, (unsigned)r.spins);
				snprintf(line, sizeof(line), "STUNT +%u%s", (unsigned)r.points, extra);
			}
			SetupFont(1.0f, 1.8f, FONT_HEADING);
			CFont::SetCentreOn();
			CFont::SetCentreSize(SCREEN_WIDTH);
			CFont::SetColor(sMatch.lastCrashed ? CRGBA(255, 80, 80, 255) : CRGBA(255, 220, 80, 255));
			CFont::PrintString(SCREEN_WIDTH / 2, SCREEN_SCALE_Y(90.0f), MpWideString(line));
			CFont::SetCentreOff();
		}
	}

	if(sResultsUntil && (int32)(sResultsUntil - CNetGame::NetNow()) > 0)
		DrawBoard(sResults, sResultsMode, ModeTitle(sResultsMode), SCREEN_SCALE_Y(120.0f));
	CFont::SetDropShadowPosition(0);
}

void
CNetMatch::RenderScoreboard(void)
{
	if(!CPad::NewKeyState.TAB || CNetChat::IsTyping())
		return;
	// A match in progress: its scores. Otherwise just who's here.
	if(sScores.n > 0){
		uint8 mode = sMatch.phase != PHASE_NONE ? sMatch.start.mode :
			sScores.entries[0].team == MP_TEAM_NONE ? (uint8)MATCH_DM : (uint8)MATCH_TDM;
		DrawBoard(sScores, mode, ModeTitle(mode), SCREEN_SCALE_Y(110.0f));
		CFont::SetDropShadowPosition(0);
		return;
	}
	float x = SCREEN_WIDTH / 2 - SCREEN_SCALE_X(150.0f);
	float y = SCREEN_SCALE_Y(110.0f);
	float lh = SCREEN_SCALE_Y(22.0f);
	int n = CNetGame::NumPlayers();
	CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(15.0f), y - SCREEN_SCALE_Y(12.0f), x + SCREEN_SCALE_X(315.0f),
		y + lh * (n + 2)), CRGBA(0, 0, 0, 180));
	char line[64];
	SetupFont(0.65f, 1.15f, FONT_HEADING);
	CFont::SetColor(CRGBA(255, 200, 40, 255));
	snprintf(line, sizeof(line), "%s  %d/%d", CNetGame::InviteCode(), n, CNetGame::MaxPlayers());
	CFont::PrintString(x, y, MpWideString(line));
	y += lh * 1.4f;
	SetupFont(0.5f, 0.9f, FONT_STANDARD);
	for(int id = 0; id < MP_MAX_PLAYERS; id++){
		const char *name = CNetGame::PlayerName((uint8)id);
		if(name == nil || (id == 0 && CNetGame::IsDedicated()))
			continue;
		snprintf(line, sizeof(line), "%s%s%s", name, id == 0 ? "  (host)" : "", id == CNetGame::MyId() ? "  (you)" : "");
		CFont::SetColor(CRemotePlayer::ColourForId((uint8)id));
		CFont::PrintString(x, y, MpWideString(line));
		y += lh;
	}
	CFont::SetDropShadowPosition(0);
}

#endif
