#pragma once

#ifdef MULTIPLAYER

class CPlayerPed;
#include "WeaponType.h"

// Free-roam spawn points and the spawn loadout.
class CSpawnPoints
{
public:
	// Heading in radians, as CPed::m_fRotationCur.
	static void Pick(uint8 playerId, CVector &pos, float &heading);
	// A free spot on the ground at or within 2 m of want (an arena spawn).
	static void PickNear(const CVector &want, CVector &pos);
	// Bat + pistol, full health, no armour.
	static void GiveLoadout(CPlayerPed *player);
	// Loads the weapon's models first so it shows in hand.
	static void GiveWeaponWithModel(CPlayerPed *player, eWeaponType weapon, uint32 ammo);
};

#endif
