#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"
#include "WeaponType.h"
#include "Ped.h"

class CEntity;
class CPed;

// Player-vs-player combat. Shooter-side hit detection, victim-side damage:
// when the local player hurts a remote player's puppet we tell that player,
// whose game applies it through the engine's own InflictDamage (armour,
// health, death). Remote players' shots are visual only on our screen.
class CNetCombat
{
public:
	// CPed::InflictDamage hook. True means the engine must not apply it.
	static bool InterceptDamage(CPed *victim, CEntity *by, eWeaponType weapon, float damage, ePedPieceTypes piece, uint8 direction);
	// CWeapon::Fire hook, after the local player fired.
	static void OnLocalShot(CEntity *shooter, eWeaponType weapon, const CVector &from);

	static void OnDamage(uint8 from, const SDamage &msg);
	static void OnDied(uint8 from, const SDied &msg);
	static void OnRespawned(uint8 from, const SRespawned &msg);
	static void OnShot(uint8 from, const SShot &msg);

	// Per frame: notices when the local player dies and tells everyone.
	static void Update(void);
	// The multiplayer WASTED branch in CGameLogic::Update. heading in degrees.
	static void GetRespawnPoint(CVector &pos, float &headingDeg);
	static void OnLocalRespawned(void);
	// Plan 3 (races) respawns at the last checkpoint instead of a spawn point.
	static void SetRespawnOverride(const CVector &pos, float headingRad);
	static void ClearRespawnOverride(void);
	static void Reset(void);
};

#endif
