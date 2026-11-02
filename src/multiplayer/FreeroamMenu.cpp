#include "common.h"

#ifdef MULTIPLAYER

#include "FreeroamMenu.h"
#include "NetGame.h"
#include "NetChat.h"
#include "NetVehicles.h"
#include "NetRace.h"
#include "NetMatch.h"
#include "SpawnPoints.h"
#include "MpText.h"
#include "Automobile.h"
#include "Bike.h"
#include "Boat.h"
#include "CarCtrl.h"
#include "Font.h"
#include "Frontend.h"
#include "Messages.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Pad.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Sprite2d.h"
#include "Streaming.h"
#include "Timer.h"
#include "WaterLevel.h"
#include "World.h"
#include "Zones.h"
#include "Script.h"

struct SMenuVehicle { const char *label; int32 model; };
struct SMenuWeapon { const char *label; eWeaponType weapon; };

// Model ids are the engine's own MI_* indices.
static const SMenuVehicle sSports[] = {
	{ "Infernus", MI_INFERNUS }, { "Cheetah", MI_CHEETAH }, { "Banshee", MI_BANSHEE },
	{ "Comet", MI_COMET }, { "Stinger", MI_STINGER }, { "Deluxo", MI_DELUXO },
};
static const SMenuVehicle sMuscle[] = {
	{ "Sabre", MI_SABRE }, { "Stallion", MI_STALLION }, { "Voodoo", MI_VOODOO },
	{ "Virgo", MI_VIRGO }, { "Hotring Racer", MI_HOTRING }, { "Blista Compact", MI_BLISTAC },
};
static const SMenuVehicle sBikes[] = {
	{ "PCJ-600", MI_PCJ600 }, { "Freeway", MI_FREEWAY }, { "Sanchez", MI_SANCHEZ },
	{ "Angel", MI_ANGEL }, { "Faggio", MI_FAGGIO },
};
static const SMenuVehicle sOffroad[] = {
	{ "Sandking", MI_SANDKING }, { "Landstalker", MI_LANDSTAL }, { "Patriot", MI_PATRIOT },
	{ "Mesa", MI_MESA }, { "BF Injection", MI_BFINJECT },
};
static const SMenuVehicle sBoats[] = {
	{ "Speeder", MI_SPEEDER }, { "Jetmax", MI_JETMAX }, { "Squalo", MI_SQUALO }, { "Dinghy", MI_DINGHY },
};
static const SMenuVehicle sHelis[] = {
	{ "Maverick", MI_MAVERICK }, { "Sparrow", MI_SPARROW }, { "Sea Sparrow", MI_SEASPAR }, { "Hunter", MI_HUNTER },
};

struct SVehicleCategory { const char *label; const SMenuVehicle *list; int count; };
static const SVehicleCategory sVehicleCategories[] = {
	{ "Sports", sSports, ARRAY_SIZE(sSports) },
	{ "Muscle", sMuscle, ARRAY_SIZE(sMuscle) },
	{ "Bikes", sBikes, ARRAY_SIZE(sBikes) },
	{ "Off-road", sOffroad, ARRAY_SIZE(sOffroad) },
	{ "Boats", sBoats, ARRAY_SIZE(sBoats) },
	{ "Helicopters", sHelis, ARRAY_SIZE(sHelis) },
};

static const SMenuWeapon sMelee[] = {
	{ "Baseball Bat", WEAPONTYPE_BASEBALLBAT }, { "Knife", WEAPONTYPE_KNIFE }, { "Machete", WEAPONTYPE_MACHETE },
	{ "Katana", WEAPONTYPE_KATANA }, { "Chainsaw", WEAPONTYPE_CHAINSAW },
};
static const SMenuWeapon sPistols[] = { { "Pistol", WEAPONTYPE_COLT45 }, { "Python", WEAPONTYPE_PYTHON } };
static const SMenuWeapon sSmgs[] = {
	{ "Uzi", WEAPONTYPE_UZI }, { "Tec-9", WEAPONTYPE_TEC9 }, { "Ingram", WEAPONTYPE_SILENCED_INGRAM }, { "MP5", WEAPONTYPE_MP5 },
};
static const SMenuWeapon sShotguns[] = {
	{ "Shotgun", WEAPONTYPE_SHOTGUN }, { "SPAS-12", WEAPONTYPE_SPAS12_SHOTGUN }, { "Stubby", WEAPONTYPE_STUBBY_SHOTGUN },
};
static const SMenuWeapon sRifles[] = {
	{ "M4", WEAPONTYPE_M4 }, { "Ruger", WEAPONTYPE_RUGER }, { "Sniper Rifle", WEAPONTYPE_SNIPERRIFLE },
	{ "Laser Scope", WEAPONTYPE_LASERSCOPE },
};
static const SMenuWeapon sHeavy[] = {
	{ "Rocket Launcher", WEAPONTYPE_ROCKETLAUNCHER }, { "Flamethrower", WEAPONTYPE_FLAMETHROWER },
	{ "M60", WEAPONTYPE_M60 }, { "Minigun", WEAPONTYPE_MINIGUN }, { "Grenades", WEAPONTYPE_GRENADE },
	{ "Molotovs", WEAPONTYPE_MOLOTOV },
};

struct SWeaponCategory { const char *label; const SMenuWeapon *list; int count; };
static const SWeaponCategory sWeaponCategories[] = {
	{ "Melee", sMelee, ARRAY_SIZE(sMelee) },
	{ "Pistols", sPistols, ARRAY_SIZE(sPistols) },
	{ "SMGs", sSmgs, ARRAY_SIZE(sSmgs) },
	{ "Shotguns", sShotguns, ARRAY_SIZE(sShotguns) },
	{ "Rifles", sRifles, ARRAY_SIZE(sRifles) },
	{ "Heavy", sHeavy, ARRAY_SIZE(sHeavy) },
};

enum ePage { PAGE_ROOT, PAGE_VEHICLE_CATS, PAGE_VEHICLES, PAGE_WEAPON_CATS, PAGE_WEAPONS, PAGE_EXTRA_LIST };

enum { MAX_EXTRA_ITEMS = 4 };
struct SExtraItem
{
	const char *label;
	void (*action)(void);
	// Or a list page:
	int (*count)(void);
	const char *(*itemLabel)(int);
	void (*select)(int);
};
static SExtraItem sExtra[MAX_EXTRA_ITEMS];
static int sNumExtra;

static bool sOpen;
static bool sReleaseNextFrame;
static ePage sPage;
static int sCursor;
static int sCategory;
static CVehicle *sLastSpawned;	// registered reference; max one spawned car each

// Root rows: Vehicles, Weapons, Health, Armour, <extra items>, Close.
enum { ROOT_VEHICLES, ROOT_WEAPONS, ROOT_HEALTH, ROOT_ARMOUR, ROOT_FIXED };

static int
NumRows(void)
{
	switch(sPage){
	case PAGE_ROOT: return ROOT_FIXED + sNumExtra + 1;
	case PAGE_VEHICLE_CATS: return ARRAY_SIZE(sVehicleCategories);
	case PAGE_VEHICLES: return sVehicleCategories[sCategory].count;
	case PAGE_WEAPON_CATS: return ARRAY_SIZE(sWeaponCategories);
	case PAGE_WEAPONS: return sWeaponCategories[sCategory].count;
	case PAGE_EXTRA_LIST: return Max(1, sExtra[sCategory].count());
	}
	return 0;
}

static const char*
RowLabel(int row)
{
	switch(sPage){
	case PAGE_ROOT:
		switch(row){
		case ROOT_VEHICLES: return "Vehicles >";
		case ROOT_WEAPONS: return "Weapons >";
		case ROOT_HEALTH: return "Health";
		case ROOT_ARMOUR: return "Armour";
		}
		if(row - ROOT_FIXED < sNumExtra)
			return sExtra[row - ROOT_FIXED].label;
		return "Close";
	case PAGE_VEHICLE_CATS: return sVehicleCategories[row].label;
	case PAGE_VEHICLES: return sVehicleCategories[sCategory].list[row].label;
	case PAGE_WEAPON_CATS: return sWeaponCategories[row].label;
	case PAGE_WEAPONS: return sWeaponCategories[sCategory].list[row].label;
	case PAGE_EXTRA_LIST: return sExtra[sCategory].count() > 0 ? sExtra[sCategory].itemLabel(row) : "(none)";
	}
	return "";
}

static const char*
PageTitle(void)
{
	switch(sPage){
	case PAGE_ROOT: return "FREE ROAM";
	case PAGE_VEHICLE_CATS: return "VEHICLES";
	case PAGE_VEHICLES: return sVehicleCategories[sCategory].label;
	case PAGE_WEAPON_CATS: return "WEAPONS";
	case PAGE_WEAPONS: return sWeaponCategories[sCategory].label;
	case PAGE_EXTRA_LIST: return sExtra[sCategory].label;
	}
	return "";
}

bool
CFreeroamMenu::IsOpen(void)
{
	return sOpen;
}

void
CFreeroamMenu::Close(void)
{
	if(sOpen)
		sReleaseNextFrame = true;
	sOpen = false;
}

void
CFreeroamMenu::AddItem(const char *label, void (*action)(void))
{
	if(sNumExtra < MAX_EXTRA_ITEMS && label && action){
		SExtraItem &e = sExtra[sNumExtra++];
		memset(&e, 0, sizeof(e));
		e.label = label;
		e.action = action;
	}
}

void
CFreeroamMenu::AddList(const char *label, int (*count)(void), const char *(*itemLabel)(int), void (*select)(int))
{
	if(sNumExtra < MAX_EXTRA_ITEMS && label && count && itemLabel && select){
		SExtraItem &e = sExtra[sNumExtra++];
		memset(&e, 0, sizeof(e));
		e.label = label;
		e.count = count;
		e.itemLabel = itemLabel;
		e.select = select;
	}
}

void
CFreeroamMenu::Reset(void)
{
	if(sOpen || sReleaseNextFrame)
		CPad::GetPad(0)->SetEnablePlayerControls(PLAYERCONTROL_MP_MENU);
	sOpen = false;
	sReleaseNextFrame = false;
	if(sLastSpawned)
		sLastSpawned->CleanUpOldReference((CEntity**)&sLastSpawned);
	sLastSpawned = nil;
}

static void
SetLastSpawned(CVehicle *vehicle)
{
	if(sLastSpawned)
		sLastSpawned->CleanUpOldReference((CEntity**)&sLastSpawned);
	sLastSpawned = vehicle;
	if(vehicle)
		vehicle->RegisterReference((CEntity**)&sLastSpawned);
}

static bool
IsEmpty(CVehicle *v)
{
	if(v->pDriver)
		return false;
	for(int i = 0; i < 3; i++)
		if(v->pPassengers[i])
			return false;
	return true;
}

CVehicle*
CFreeroamMenu::SpawnAndEnter(int32 model, const CVector &where, float headingRad)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || model < 0 || model >= MODELINFOSIZE)
		return nil;
	bool boat = CModelInfo::IsBoatModel(model);
	CVector pos = where;
	// Somewhere far away (a race grid): its collision isn't loaded yet.
	if((pos - player->GetPosition()).Magnitude2D() > 100.0f){
		CStreaming::LoadSceneCollision(pos);
		CStreaming::LoadScene(pos);
	}
	if(boat){
		float level;
		if(!CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &level, true)){
			wchar *text = MpGetText("MP_WATER");
			if(text)
				CMessages::AddMessageJumpQ(text, 3000, 0);
			return nil;
		}
		pos.z = level;
	}else{
		// The surface under us, not the highest one (bridges, roofs).
		bool found = false;
		float z = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
		pos.z = found ? z : CWorld::FindGroundZForCoord(pos.x, pos.y);
	}

	// Out of our current car first, so it counts as empty below.
	if(player->bInVehicle)
		CNetVehicles::WarpLocalPlayerOut();

	// Load it right now, like the script's LOAD_ALL_MODELS_NOW.
	CStreaming::RequestModel(model, STREAMFLAGS_DONT_REMOVE);
	CTimer::Suspend();
	CStreaming::LoadAllRequestedModels(false);
	CTimer::Resume();
	if(!CStreaming::HasModelLoaded(model))
		return nil;

	// One spawned car each: the previous one goes if nobody is in it.
	if(sLastSpawned && IsEmpty(sLastSpawned) && !CNetVehicles::IsRemote(sLastSpawned))
		CNetVehicles::RemoveOwned(sLastSpawned);

	CVehicle *vehicle;
	if(boat)
		vehicle = new CBoat(model, MISSION_VEHICLE);
	else if(CModelInfo::IsBikeModel(model)){
		vehicle = new CBike(model, MISSION_VEHICLE);
		((CBike*)vehicle)->bIsStanding = true;
	}else
		vehicle = new CAutomobile(model, MISSION_VEHICLE);
	pos.z += vehicle->GetDistanceFromCentreOfMassToBaseOfModel();
	vehicle->SetPosition(pos);
	vehicle->SetHeading(headingRad);
	vehicle->SetStatus(STATUS_ABANDONED);
	vehicle->bIsLocked = false;
	vehicle->AutoPilot.m_nCarMission = MISSION_NONE;
	vehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
	vehicle->bEngineOn = true;
	vehicle->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	vehicle->bHasBeenOwnedByPlayer = true;	// the constructor already counted it as a mission car
	CTheScripts::ClearSpaceForMissionEntity(pos, vehicle);
	CWorld::Add(vehicle);
	CStreaming::SetModelIsDeletable(model);

	player->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, vehicle);
	player->WarpPedIntoCar(vehicle);
	CNetVehicles::Register(vehicle);
	SetLastSpawned(vehicle);
	return vehicle;
}

static void
SpawnFromMenu(int32 model)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	float heading = player->m_fRotationCur;
	CVector forward(-Sin(heading), Cos(heading), 0.0f);
	CFreeroamMenu::SpawnAndEnter(model, player->GetPosition() + forward * 5.0f, heading);
}

static void
Activate(int row)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	switch(sPage){
	case PAGE_ROOT:
		switch(row){
		case ROOT_VEHICLES: sPage = PAGE_VEHICLE_CATS; sCursor = 0; return;
		case ROOT_WEAPONS: sPage = PAGE_WEAPON_CATS; sCursor = 0; return;
		case ROOT_HEALTH:
			player->m_fHealth = CWorld::Players[CWorld::PlayerInFocus].m_nMaxHealth;
			return;
		case ROOT_ARMOUR:
			player->m_fArmour = 100.0f;
			return;
		}
		if(row - ROOT_FIXED < sNumExtra){
			SExtraItem &e = sExtra[row - ROOT_FIXED];
			if(e.count){
				sCategory = row - ROOT_FIXED;
				sPage = PAGE_EXTRA_LIST;
				sCursor = 0;
				return;
			}
			CFreeroamMenu::Close();
			e.action();
			return;
		}
		CFreeroamMenu::Close();
		return;
	case PAGE_VEHICLE_CATS:
		sCategory = row;
		sPage = PAGE_VEHICLES;
		sCursor = 0;
		return;
	case PAGE_VEHICLES:
		CFreeroamMenu::Close();
		SpawnFromMenu(sVehicleCategories[sCategory].list[row].model);
		return;
	case PAGE_WEAPON_CATS:
		sCategory = row;
		sPage = PAGE_WEAPONS;
		sCursor = 0;
		return;
	case PAGE_EXTRA_LIST:
		if(row < sExtra[sCategory].count()){
			void (*select)(int) = sExtra[sCategory].select;
			CFreeroamMenu::Close();
			select(row);
		}
		return;
	case PAGE_WEAPONS: {
		eWeaponType w = sWeaponCategories[sCategory].list[row].weapon;
		bool melee = w <= WEAPONTYPE_CHAINSAW;
		CSpawnPoints::GiveWeaponWithModel(player, w, melee ? 1 : 500);
		player->SetCurrentWeapon(w);
		return;
	}
	}
}

static void
Back(void)
{
	switch(sPage){
	case PAGE_ROOT: CFreeroamMenu::Close(); break;
	case PAGE_VEHICLE_CATS: case PAGE_WEAPON_CATS: sPage = PAGE_ROOT; sCursor = 0; break;
	case PAGE_VEHICLES: sPage = PAGE_VEHICLE_CATS; sCursor = sCategory; break;
	case PAGE_WEAPONS: sPage = PAGE_WEAPON_CATS; sCursor = sCategory; break;
	case PAGE_EXTRA_LIST: sPage = PAGE_ROOT; sCursor = ROOT_FIXED + sCategory; break;
	}
}

void
CFreeroamMenu::Update(void)
{
	CPad *pad = CPad::GetPad(0);
	if(sReleaseNextFrame){
		sReleaseNextFrame = false;
		pad->SetEnablePlayerControls(PLAYERCONTROL_MP_MENU);
	}
	if(!CNetGame::IsActive()){
		if(sOpen)
			Reset();
		return;
	}
	CPlayerPed *player = FindPlayerPed();
	if(!sOpen){
		if(CNetChat::IsTyping() || CNetRace::IsRacing() || CNetMatch::IsPlaying() || FrontEndMenuManager.m_bMenuActive || player == nil || player->DyingOrDead())
			return;
		if(!pad->GetFJustDown(1) || pad->ArePlayerControlsDisabled())	// F2
			return;
		sOpen = true;
		sPage = PAGE_ROOT;
		sCursor = 0;
		pad->SetDisablePlayerControls(PLAYERCONTROL_MP_MENU);
		return;
	}

	if(player == nil || player->DyingOrDead() || pad->GetFJustDown(1)){
		Close();
		return;
	}
	int rows = NumRows();
	if(pad->GetUpJustDown() || pad->GetDPadUpJustDown())
		sCursor = (sCursor + rows - 1) % rows;
	if(pad->GetDownJustDown() || pad->GetDPadDownJustDown())
		sCursor = (sCursor + 1) % rows;
	if(pad->GetBackspaceJustDown() || pad->GetDPadLeftJustDown())
		Back();
	else if(pad->GetReturnJustDown() || pad->GetPadEnterJustDown() || pad->GetDPadRightJustDown())
		Activate(sCursor);
}

void
CFreeroamMenu::Render(void)
{
	if(!sOpen)
		return;
	int rows = NumRows();
	float x = SCREEN_SCALE_X(40.0f);
	float y = SCREEN_SCALE_Y(150.0f);
	float lineH = SCREEN_SCALE_Y(22.0f);
	float w = SCREEN_SCALE_X(240.0f);
	CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(10.0f), y - SCREEN_SCALE_Y(10.0f), x + w,
		y + lineH * (rows + 2) + SCREEN_SCALE_Y(10.0f)), CRGBA(0, 0, 0, 170));

	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
	CFont::SetScale(SCREEN_SCALE_X(0.6f), SCREEN_SCALE_Y(1.0f));
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
	CFont::SetColor(CRGBA(255, 200, 40, 255));
	CFont::PrintString(x, y, MpWideString(PageTitle()));
	y += lineH * 1.5f;

	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(SCREEN_SCALE_X(0.5f), SCREEN_SCALE_Y(0.9f));
	for(int i = 0; i < rows; i++){
		bool selected = i == sCursor;
		CFont::SetColor(selected ? CRGBA(255, 255, 255, 255) : CRGBA(160, 160, 160, 255));
		char label[64];
		snprintf(label, sizeof(label), "%s%s", selected ? "> " : "  ", RowLabel(i));
		CFont::PrintString(x, y, MpWideString(label));
		y += lineH;
	}
	CFont::SetDropShadowPosition(0);
}

#endif
