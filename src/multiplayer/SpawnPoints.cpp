#include "common.h"

#ifdef MULTIPLAYER

#include "SpawnPoints.h"
#include "PlayerPed.h"
#include "Streaming.h"
#include "WeaponInfo.h"
#include "General.h"
#include "World.h"

// The free-roam start that freeroam_miami.scm itself uses (CREATE_PLAYER at
// 79.69 -867.25 with "find the ground", LOAD_SCENE at z 10.5): open ground on
// Washington Beach that the original script picked. Players spread out on a
// ring around it; each candidate spot is checked against the world, so a spot
// that lands in or on something falls through to the next one.
static const CVector sAnchor(79.69f, -867.25f, 10.5f);
static const float SPAWN_RADIUS = 3.0f;	// metres from the anchor
static const int NUM_SLOTS = 8;
// Ground further than this from the anchor's is a roof or a hole, not the beach.
static const float MAX_GROUND_DIFF = 2.0f;

// Ground z under (x, y) near refZ, or false if there's no usable ground or
// the spot is blocked.
static bool
TrySpot(float x, float y, float refZ, CVector &out)
{
	bool found = false;
	float z = CWorld::FindGroundZFor3DCoord(x, y, refZ + MAX_GROUND_DIFF, &found);
	if(!found || Abs(z - refZ) > MAX_GROUND_DIFF)
		return false;
	CVector pos(x, y, z + 1.0f);	// ped centre
	if(CWorld::TestSphereAgainstWorld(pos, 0.6f, nil, true, true, false, true, false, false))
		return false;
	out = pos;
	return true;
}

void
CSpawnPoints::Pick(uint8 playerId, CVector &pos, float &heading)
{
	// Respawns can happen far from here, so make sure the ground is loaded.
	CStreaming::LoadSceneCollision(sAnchor);
	int first = playerId % NUM_SLOTS;
	// This player's own slot first, then the others, then the anchor itself.
	for(int i = 0; i < NUM_SLOTS; i++){
		float angle = (float)((first + i) % NUM_SLOTS) * (TWOPI / NUM_SLOTS);
		float dx = Cos(angle) * SPAWN_RADIUS;
		float dy = Sin(angle) * SPAWN_RADIUS;
		if(TrySpot(sAnchor.x + dx, sAnchor.y + dy, sAnchor.z, pos)){
			// Face the anchor: a ped faces (-sin h, cos h).
			heading = CGeneral::LimitRadianAngle(Atan2(dx, -dy));
			return;
		}
	}
	if(!TrySpot(sAnchor.x, sAnchor.y, sAnchor.z, pos))
		pos = CVector(sAnchor.x, sAnchor.y, sAnchor.z + 1.0f);
	heading = 0.0f;
}

void
CSpawnPoints::PickNear(const CVector &want, CVector &pos)
{
	CStreaming::LoadSceneCollision(want);
	// want is a ped centre (about 1 m up); look for the ground below it.
	float refZ = want.z - 1.0f;
	if(TrySpot(want.x, want.y, refZ, pos))
		return;
	for(int i = 0; i < NUM_SLOTS; i++){
		float angle = (float)i * (TWOPI / NUM_SLOTS);
		if(TrySpot(want.x + Cos(angle) * 2.0f, want.y + Sin(angle) * 2.0f, refZ, pos))
			return;
	}
	pos = want;	// trust the author
}

void
CSpawnPoints::GiveWeaponWithModel(CPlayerPed *player, eWeaponType weapon, uint32 ammo)
{
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(weapon);
	if(info->m_nModelId > 0)
		CStreaming::RequestModel(info->m_nModelId, STREAMFLAGS_DONT_REMOVE);
	if(info->m_nModel2Id > 0)
		CStreaming::RequestModel(info->m_nModel2Id, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);
	player->GiveWeapon(weapon, ammo, true);
}

void
CSpawnPoints::GiveLoadout(CPlayerPed *player)
{
	if(player == nil)
		return;
	GiveWeaponWithModel(player, WEAPONTYPE_BASEBALLBAT, 1);
	GiveWeaponWithModel(player, WEAPONTYPE_COLT45, 68);
	player->SetCurrentWeapon(WEAPONTYPE_COLT45);
	player->m_fHealth = 100.0f;
	player->m_fArmour = 0.0f;
}

#endif
