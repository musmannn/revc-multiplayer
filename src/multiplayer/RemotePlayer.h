#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"
#include "PoseBuffer.h"
#include "AnimationId.h"

class CCivilianPed;
class CPed;
class CVehicle;

// Another player in the session, shown as a CCivilianPed puppet with
// CharCreatedBy = UNK_CHAR: the engine skips its AI and physics, population
// never deletes it, and we drive it from interpolated network snapshots.
class CRemotePlayer
{
public:
	uint8 m_id;
	char m_name[MP_MAX_NAME_LEN + 1];
	CCivilianPed *m_pPed;
	CPoseBuffer m_poses;
	SPlayerState m_last;
	int32 m_blip;
	bool m_bSpawned;

	bool m_bUsed;
	bool m_bHaveState;
	int32 m_modelId;
	AnimationId m_currentAnim;
	bool m_bShownDead;
	uint8 m_currentWeapon;
	uint8 m_pendingWeapon;
	CVehicle *m_pSeatedIn;	// registered reference
	uint8 m_seat;

	static CRemotePlayer *Get(uint8 id);
	static void Add(uint8 id, const char *name);
	// Deletes the puppet, clears its blip and drops it from any vehicle.
	static void Remove(uint8 id);
	static void RemoveAll(void);
	static CRemotePlayer *FindByPed(CPed *ped);
	// Takes every remote player out of vehicle (it's about to be deleted).
	static void UnseatAllFrom(CVehicle *vehicle);

	void OnState(const SPlayerState &state);
	void Update(uint32 renderTime);
	// Called from CNetGame::Process.
	static void UpdateAll(void);
	static void RenderNameTags(void);

	// Player id -> name tag / blip colour.
	static CRGBA ColourForId(uint8 id);

private:
	void Spawn(void);
	void Despawn(void);
	void UpdateAnimation(void);
	void UpdateWeapon(void);
	void UpdateSeat(void);
	void SeatIn(CVehicle *vehicle, uint8 seat);
	void Unseat(bool placeOutside);
};

#endif
