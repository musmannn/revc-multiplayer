#include "common.h"

#ifdef MULTIPLAYER

#include "NetCombat.h"
#include "NetGame.h"
#include "NetVehicles.h"
#include "RemotePlayer.h"
#include "SpawnPoints.h"
#include "NetMatch.h"
#include "MpText.h"
#include "CivilianPed.h"
#include "DMAudio.h"
#include "Messages.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "SpecialFX.h"
#include "Vehicle.h"
#include "WeaponInfo.h"
#include "World.h"
#include "Camera.h"

static bool sApplyingNetDamage;	// our InflictDamage call for a GMSG_DAMAGE
static bool sReportedDeath;
static bool sHaveOverride;
static uint32 sRespawnCount;	// spreads respawns over the spawn points
static CVector sOverridePos;
static float sOverrideHeading;

static void
Send(uint8 route, uint8 target, bool reliable, const CByteWriter &w)
{
	CNetGame::Client().SendGame(route, target, reliable, w);
}

static const char*
NameOf(uint8 id)
{
	const char *name = CNetGame::PlayerName(id);
	return name ? name : "Someone";
}

// Kill-feed line on the HUD. CMessages keeps the pointer, so it gets its own
// storage (not MpWideString's per-frame ring).
static void
KillFeed(const char *text)
{
	static wchar sLines[8][MP_TEXT_RING_LEN];
	static int sNext;
	wchar *line = sLines[sNext];
	sNext = (sNext + 1) % 8;
	int i = 0;
	for(; text[i] && i < MP_TEXT_RING_LEN - 1; i++)
		line[i] = (uint8)text[i];
	line[i] = 0;
	CMessages::AddMessageJumpQ(line, 3000, 0);
}

static bool
IsLocalAttacker(CEntity *by)
{
	CPlayerPed *player = FindPlayerPed();
	if(by == nil || player == nil)
		return false;
	if(by == player)
		return true;
	return player->bInVehicle && by == player->m_pMyVehicle && player->m_pMyVehicle->pDriver == player;
}

// The remote player behind an entity: their puppet, or a car they own.
// Pointer comparisons only: m_lastDamEntity can be stale.
static uint8
RemoteIdOf(CEntity *entity)
{
	if(entity == nil)
		return MP_PLAYER_NONE;
	CRemotePlayer *p = CRemotePlayer::FindByPed((CPed*)entity);
	if(p)
		return p->m_id;
	return CNetVehicles::RemoteOwnerOf(entity);
}

bool
CNetCombat::InterceptDamage(CPed *victim, CEntity *by, eWeaponType weapon, float damage, ePedPieceTypes piece, uint8 direction)
{
	if(sApplyingNetDamage)
		return false;

	CRemotePlayer *remote = CRemotePlayer::FindByPed(victim);
	if(remote){
		// Puppets never take local damage; their owner decides. No friendly
		// fire between team deathmatch teammates.
		if(IsLocalAttacker(by) && damage > 0.0f && !CNetMatch::IsTeammate(remote->m_id)){
			SDamage msg;
			msg.victimId = remote->m_id;
			msg.weapon = (uint8)weapon;
			msg.amount = damage;
			msg.piece = (uint8)piece;
			msg.direction = direction;
			CByteWriter w;
			msg.Write(w);
			Send(MP_ROUTE_PLAYER, remote->m_id, true, w);
		}
		return true;
	}

	// A remote player's copy hurting us here would double up with their own
	// report, so only their GMSG_DAMAGE counts.
	if(victim == FindPlayerPed() && RemoteIdOf(by) != MP_PLAYER_NONE)
		return true;
	return false;
}

void
CNetCombat::OnDamage(uint8 from, const SDamage &msg)
{
	CPlayerPed *player = FindPlayerPed();
	// Weapons plus ramming, run-overs, explosions and drive-bys; not the
	// pickup pseudo-types.
	if(player == nil || msg.victimId != CNetGame::MyId() || msg.weapon >= WEAPONTYPE_ANYMELEE ||
	   msg.weapon == WEAPONTYPE_HEALTH || msg.weapon == WEAPONTYPE_ARMOUR)
		return;
	if(msg.amount <= 0.0f || msg.amount > 1000.0f)
		return;
	if(CNetMatch::IsTeammate(from))
		return;	// no friendly fire, whatever the attacker's game thinks
	CRemotePlayer *attacker = CRemotePlayer::Get(from);
	CEntity *by = attacker ? (CEntity*)attacker->m_pPed : nil;
	sApplyingNetDamage = true;
	player->InflictDamage(by, (eWeaponType)msg.weapon, msg.amount, (ePedPieceTypes)msg.piece, msg.direction);
	sApplyingNetDamage = false;
}

void
CNetCombat::OnLocalShot(CEntity *shooter, eWeaponType weapon, const CVector &from)
{
	CPlayerPed *player = FindPlayerPed();
	if(shooter != player || player == nil)
		return;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(weapon);
	if(info->m_eWeaponFire != WEAPON_FIRE_INSTANT_HIT)
		return;	// projectiles and melee show up through the shooter's anims
	// Visual only on other screens: aim along the camera.
	CVector dir = TheCamera.Cams[TheCamera.ActiveCam].Front;
	dir.Normalise();
	CVector to = from + dir * info->m_fRange;
	SShot msg;
	msg.weapon = (uint8)weapon;
	msg.from[0] = from.x;
	msg.from[1] = from.y;
	msg.from[2] = from.z;
	msg.to[0] = to.x;
	msg.to[1] = to.y;
	msg.to[2] = to.z;
	CByteWriter w;
	msg.Write(w);
	Send(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
}

void
CNetCombat::OnShot(uint8 from, const SShot &msg)
{
	CRemotePlayer *p = CRemotePlayer::Get(from);
	if(p == nil || p->m_pPed == nil || msg.weapon >= WEAPONTYPE_TOTALWEAPONS)
		return;
	CVector start(msg.from[0], msg.from[1], msg.from[2]);
	CVector end(msg.to[0], msg.to[1], msg.to[2]);
	if((start - p->m_pPed->GetPosition()).MagnitudeSqr() > sq(10.0f))
		return;	// not plausibly from them
	CBulletTraces::AddTrace(&start, &end, msg.weapon, p->m_pPed);
	p->m_pPed->bIsShooting = true;
	DMAudio.PlayOneShot(p->m_pPed->m_audioEntityId, SOUND_WEAPON_SHOT_FIRED, 0.0f);
}

void
CNetCombat::Update(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	CPlayerInfo &info = CWorld::Players[CWorld::PlayerInFocus];
	bool wasted = info.m_WBState == WBSTATE_WASTED || player->DyingOrDead();
	if(!wasted){
		sReportedDeath = false;
		return;
	}
	if(sReportedDeath)
		return;
	sReportedDeath = true;

	SDied msg;
	msg.killerId = RemoteIdOf(player->m_lastDamEntity);
	msg.weapon = (uint8)Max(0, (int)player->m_lastWepDam);
	CByteWriter w;
	msg.Write(w);
	Send(MP_ROUTE_ALL, 0, true, w);	// the echo shows our own kill-feed line
}

void
CNetCombat::OnDied(uint8 from, const SDied &msg)
{
	char line[96];
	if(msg.killerId != MP_PLAYER_NONE && msg.killerId != from)
		snprintf(line, sizeof(line), "%s killed %s", NameOf(msg.killerId), NameOf(from));
	else
		snprintf(line, sizeof(line), "%s died", NameOf(from));
	KillFeed(line);
}

void
CNetCombat::OnRespawned(uint8 from, const SRespawned &msg)
{
	(void)from;
	(void)msg;
	// The puppet comes back on its own: PSF_DEAD clears and the pose jumps.
}

void
CNetCombat::GetRespawnPoint(CVector &pos, float &headingDeg)
{
	float heading;
	if(sHaveOverride){
		pos = sOverridePos;
		heading = sOverrideHeading;
	}else
		CSpawnPoints::Pick((uint8)(CNetGame::MyId() + sRespawnCount++), pos, heading);
	headingDeg = RADTODEG(heading);
}

void
CNetCombat::OnLocalRespawned(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	if(CNetMatch::IsPlaying())
		CNetMatch::GiveMatchLoadout(player);
	else if(!sHaveOverride)
		CSpawnPoints::GiveLoadout(player);
	player->m_lastDamEntity = nil;	// no registered reference; don't carry it over
	sReportedDeath = false;
	SRespawned msg;
	const CVector &pos = player->GetPosition();
	msg.pos[0] = pos.x;
	msg.pos[1] = pos.y;
	msg.pos[2] = pos.z;
	msg.heading = player->m_fRotationCur;
	CByteWriter w;
	msg.Write(w);
	Send(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
}

void
CNetCombat::SetRespawnOverride(const CVector &pos, float headingRad)
{
	sHaveOverride = true;
	sOverridePos = pos;
	sOverrideHeading = headingRad;
}

void
CNetCombat::ClearRespawnOverride(void)
{
	sHaveOverride = false;
}

void
CNetCombat::Reset(void)
{
	sApplyingNetDamage = false;
	sReportedDeath = false;
	sHaveOverride = false;
}

#endif
