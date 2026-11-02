#include "common.h"

#ifdef MULTIPLAYER

#include "RemotePlayer.h"
#include "NetGame.h"
#include "MpText.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "Camera.h"
#include "CivilianPed.h"
#include "Font.h"
#include "ModelInfo.h"
#include "Pools.h"
#include "Radar.h"
#include "Sprite.h"
#include "Sprite2d.h"
#include "Streaming.h"
#include "WeaponInfo.h"
#include "World.h"
#include "NetVehicles.h"
#include "NetMatch.h"
#include "Vehicle.h"

static CRemotePlayer sPlayers[MP_MAX_PLAYERS];

// Skins for SPlayerState::skin == 0, by player id.
// TODO verify in-game: ped model ids from peds.ide.
static const int32 sDefaultSkins[MP_MAX_PLAYERS] = { 7, 9, 10, 11, 12, 13, 14, 15 };

static const float TELEPORT_DISTANCE = 20.0f;	// snap instead of sliding
static const float NAME_TAG_DISTANCE = 80.0f;

CRGBA
CRemotePlayer::ColourForId(uint8 id)
{
	static const CRGBA colours[MP_MAX_PLAYERS] = {
		CRGBA(255, 200, 40, 255),	// host: gold
		CRGBA(80, 160, 255, 255),
		CRGBA(255, 80, 80, 255),
		CRGBA(90, 220, 90, 255),
		CRGBA(220, 100, 255, 255),
		CRGBA(255, 150, 60, 255),
		CRGBA(60, 230, 230, 255),
		CRGBA(240, 240, 240, 255),
	};
	return colours[id % MP_MAX_PLAYERS];
}

static uint32
BlipColour(uint8 id)
{
	CRGBA c = CRemotePlayer::ColourForId(id);
	return ((uint32)c.r << 24) | ((uint32)c.g << 16) | ((uint32)c.b << 8) | 0xFF;
}

CRemotePlayer*
CRemotePlayer::Get(uint8 id)
{
	if(id >= MP_MAX_PLAYERS || !sPlayers[id].m_bUsed)
		return nil;
	return &sPlayers[id];
}

void
CRemotePlayer::Add(uint8 id, const char *name)
{
	if(id >= MP_MAX_PLAYERS || id == CNetGame::MyId())
		return;
	if(id == 0 && CNetGame::IsDedicated())
		return;	// revc-server: no ped
	CRemotePlayer &p = sPlayers[id];
	if(p.m_bUsed)
		Remove(id);
	p.m_id = id;
	strncpy(p.m_name, name ? name : "", MP_MAX_NAME_LEN);
	p.m_name[MP_MAX_NAME_LEN] = '\0';
	p.m_pPed = nil;
	p.m_poses.Clear();
	p.m_blip = -1;
	p.m_bSpawned = false;
	p.m_bUsed = true;
	p.m_bHaveState = false;
	p.m_modelId = -1;
	p.m_currentAnim = ANIM_STD_IDLE;
	p.m_bShownDead = false;
	p.m_currentWeapon = WEAPONTYPE_UNARMED;
	p.m_pendingWeapon = WEAPONTYPE_UNARMED;
	p.m_pSeatedIn = nil;
	p.m_seat = MP_SEAT_NONE;
}

void
CRemotePlayer::Remove(uint8 id)
{
	if(id >= MP_MAX_PLAYERS || !sPlayers[id].m_bUsed)
		return;
	sPlayers[id].Despawn();
	sPlayers[id].m_bUsed = false;
}

void
CRemotePlayer::RemoveAll(void)
{
	for(int i = 0; i < MP_MAX_PLAYERS; i++)
		Remove((uint8)i);
}

CRemotePlayer*
CRemotePlayer::FindByPed(CPed *ped)
{
	if(ped == nil)
		return nil;
	for(int i = 0; i < MP_MAX_PLAYERS; i++)
		if(sPlayers[i].m_bUsed && sPlayers[i].m_pPed == ped)
			return &sPlayers[i];
	return nil;
}

static void
HeadingToQuat(float heading, float *quat)
{
	quat[0] = 0.0f;
	quat[1] = 0.0f;
	quat[2] = Sin(heading * 0.5f);
	quat[3] = Cos(heading * 0.5f);
}

static float
QuatToHeading(const float *quat)
{
	return 2.0f * Atan2(quat[2], quat[3]);
}

void
CRemotePlayer::OnState(const SPlayerState &state)
{
	m_last = state;
	m_bHaveState = true;
	SPose pose;
	pose.time = state.time;
	memcpy(pose.pos, state.pos, sizeof(pose.pos));
	HeadingToQuat(state.heading, pose.quat);
	memcpy(pose.vel, state.vel, sizeof(pose.vel));
	m_poses.Push(pose);

	int32 model = state.skin ? state.skin : sDefaultSkins[m_id % MP_MAX_PLAYERS];
	if(model <= 0 || model >= MODELINFOSIZE || CModelInfo::GetModelInfo(model) == nil ||
	   CModelInfo::GetModelInfo(model)->GetModelType() != MITYPE_PED)
		model = sDefaultSkins[m_id % MP_MAX_PLAYERS];
	if(model != m_modelId){
		// A new skin: rebuild the puppet once the model streams in.
		if(m_bSpawned)
			Despawn();
		m_modelId = model;
		CStreaming::RequestModel(m_modelId, STREAMFLAGS_DONT_REMOVE);
	}
	if(state.weapon != m_currentWeapon)
		m_pendingWeapon = state.weapon;
}

void
CRemotePlayer::Spawn(void)
{
	SPose pose;
	if(!m_poses.Sample(CNetGame::SessionNow() - MP_INTERP_DELAY_MS, pose))
		return;
	CCivilianPed *ped = new CCivilianPed(PEDTYPE_CIVMALE, m_modelId);
	ped->CharCreatedBy = UNK_CHAR;
	ped->bRespondsToThreats = false;
	ped->SetPosition(CVector(pose.pos[0], pose.pos[1], pose.pos[2]));
	float heading = QuatToHeading(pose.quat);
	ped->m_fRotationCur = heading;
	ped->m_fRotationDest = heading;
	ped->SetHeading(heading);
	CWorld::Add(ped);
	m_pPed = ped;
	// The engine may delete the puppet itself (e.g. killed in an exploding
	// car); the reference turns m_pPed into nil when it does.
	m_pPed->RegisterReference((CEntity**)&m_pPed);
	m_bSpawned = true;
	m_currentAnim = ANIM_STD_IDLE;
	m_bShownDead = false;
	m_currentWeapon = WEAPONTYPE_UNARMED;
	m_pendingWeapon = m_last.weapon;

	m_blip = CRadar::SetEntityBlip(BLIP_CHAR, CPools::GetPedRef(ped), BlipColour(m_id), BLIP_DISPLAY_BLIP_ONLY);
	// SetEntityBlip ignores its colour argument.
	CRadar::ChangeBlipColour(m_blip, BlipColour(m_id));
	CRadar::ChangeBlipBrightness(m_blip, 0);	// 0 = bright (m_bDim is passed as "bright")
	CRadar::ChangeBlipScale(m_blip, 2);
}

void
CRemotePlayer::UnseatAllFrom(CVehicle *vehicle)
{
	for(int i = 0; i < MP_MAX_PLAYERS; i++){
		CRemotePlayer &p = sPlayers[i];
		if(p.m_bUsed && p.m_pPed && p.m_pPed->bInVehicle && p.m_pPed->m_pMyVehicle == vehicle)
			p.Unseat(true);
	}
}

// Like CREATE_CHAR_INSIDE_CAR / CREATE_CHAR_AS_PASSENGER: remote enter/exit
// shows as a warp in milestone 1.
void
CRemotePlayer::SeatIn(CVehicle *vehicle, uint8 seat)
{
	if(seat == MP_SEAT_DRIVER){
		if(vehicle->pDriver && vehicle->pDriver != m_pPed)
			return;
	}else{
		int idx = seat - 1;
		if(idx < 0 || idx >= vehicle->m_nNumMaxPassengers || idx >= 3 || vehicle->pPassengers[idx]){
			// Taken or missing here: any free passenger seat will do visually.
			idx = -1;
			for(int i = 0; i < vehicle->m_nNumMaxPassengers && i < 3; i++)
				if(vehicle->pPassengers[i] == nil){
					idx = i;
					break;
				}
			if(idx < 0)
				return;
		}
		seat = (uint8)(idx + 1);
	}

	CPed *ped = m_pPed;
	ped->SetPedState(PED_DRIVING);
	if(seat == MP_SEAT_DRIVER){
		vehicle->pDriver = ped;
		vehicle->pDriver->RegisterReference((CEntity**)&vehicle->pDriver);
	}else
		vehicle->AddPassenger(ped, seat - 1);
	ped->m_pMyVehicle = vehicle;
	ped->m_pMyVehicle->RegisterReference((CEntity**)&ped->m_pMyVehicle);
	ped->bInVehicle = true;
	ped->bUsesCollision = false;
	ped->AddInCarAnims(vehicle, seat == MP_SEAT_DRIVER);
	if(CNetVehicles::IsRemote(vehicle) && vehicle->GetStatus() != STATUS_WRECKED)
		vehicle->SetStatus(STATUS_PLAYER_DISABLED);
	m_pSeatedIn = vehicle;
	m_pSeatedIn->RegisterReference((CEntity**)&m_pSeatedIn);
	m_seat = seat;
}

void
CRemotePlayer::Unseat(bool placeOutside)
{
	CPed *ped = m_pPed;
	CVehicle *vehicle = ped->m_pMyVehicle;
	if(vehicle){
		if(vehicle->pDriver == ped){
			vehicle->RemoveDriver();
			if(CNetVehicles::IsRemote(vehicle) && vehicle->GetStatus() != STATUS_WRECKED)
				vehicle->SetStatus(STATUS_PLAYER_DISABLED);
		}else
			vehicle->RemovePassenger(ped);
		ped->m_pMyVehicle->CleanUpOldReference((CEntity**)&ped->m_pMyVehicle);
	}
	ped->m_pMyVehicle = nil;
	ped->RemoveInCarAnims();
	ped->bInVehicle = false;
	ped->SetPedState(PED_IDLE);
	ped->bUsesCollision = true;
	ped->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	if(ped->m_pVehicleAnim)
		ped->m_pVehicleAnim->blendDelta = -1000.0f;
	ped->m_pVehicleAnim = nil;
	CAnimManager::BlendAnimation(ped->GetClump(), ped->m_animGroup, ANIM_STD_IDLE, 1000.0f);
	m_currentAnim = ANIM_STD_IDLE;
	if(placeOutside && vehicle){
		CVector pos = vehicle->GetPosition() - vehicle->GetRight() * 2.0f;
		ped->Teleport(pos);
	}
	if(m_pSeatedIn)
		m_pSeatedIn->CleanUpOldReference((CEntity**)&m_pSeatedIn);
	m_pSeatedIn = nil;
	m_seat = MP_SEAT_NONE;
}

void
CRemotePlayer::UpdateSeat(void)
{
	CVehicle *want = nil;
	uint8 wantSeat = MP_SEAT_NONE;
	if((m_last.flags & PSF_IN_VEHICLE) && !(m_last.flags & PSF_DEAD)){
		want = CNetVehicles::Find(m_last.vehicleId);
		wantSeat = m_last.seat;
	}
	bool seated = m_pPed->bInVehicle;
	// Passenger slots may differ visually; driver vs passenger must match.
	bool sameKind = (wantSeat == MP_SEAT_DRIVER) == (m_seat == MP_SEAT_DRIVER);
	if(seated && m_pSeatedIn == want && want != nil && sameKind)
		return;
	if(!seated && want == nil)
		return;
	if(seated)
		Unseat(true);
	if(want)
		SeatIn(want, wantSeat);
}

void
CRemotePlayer::Despawn(void)
{
	if(m_blip >= 0)
		CRadar::ClearBlip(m_blip);
	m_blip = -1;
	if(m_pPed && m_pPed->bInVehicle)
		Unseat(false);
	if(m_pPed){
		CPed *ped = m_pPed;
		ped->CleanUpOldReference((CEntity**)&m_pPed);
		m_pPed = nil;
		CWorld::Remove(ped);
		delete ped;
	}
	if(m_pSeatedIn)
		m_pSeatedIn->CleanUpOldReference((CEntity**)&m_pSeatedIn);
	m_pSeatedIn = nil;
	m_seat = MP_SEAT_NONE;
	m_bSpawned = false;
}

void
CRemotePlayer::UpdateAnimation(void)
{
	if(m_last.flags & PSF_DEAD){
		if(!m_bShownDead){
			m_pPed->SetDie(ANIM_STD_KO_FRONT, 4.0f, 0.0f);
			m_bShownDead = true;
		}
		return;
	}
	if(m_bShownDead || m_pPed->DyingOrDead()){
		// Alive again (respawned), or something killed the puppet locally:
		// start from a fresh puppet.
		Despawn();
		return;
	}

	AnimationId anim = ANIM_STD_IDLE;
	if(m_last.flags & PSF_JUMPING)
		anim = ANIM_STD_JUMP_GLIDE;
	else if(m_last.flags & PSF_CROUCH)
		anim = ANIM_STD_DUCK_LOW;
	else{
		switch(m_last.moveState){
		case PEDMOVE_WALK: anim = ANIM_STD_WALK; break;
		case PEDMOVE_JOG:
		case PEDMOVE_RUN: anim = ANIM_STD_RUN; break;
		case PEDMOVE_SPRINT: anim = ANIM_STD_RUNFAST; break;
		default: anim = ANIM_STD_IDLE; break;
		}
	}
	if(anim != m_currentAnim){
		// Walk-style groups only hold walk/run/sprint/idle; the rest live in STD.
		bool moveAnim = anim == ANIM_STD_WALK || anim == ANIM_STD_RUN || anim == ANIM_STD_RUNFAST || anim == ANIM_STD_IDLE;
		CAnimManager::BlendAnimation(m_pPed->GetClump(), moveAnim ? m_pPed->m_animGroup : ASSOCGRP_STD, anim, 4.0f);
		m_currentAnim = anim;
	}
}

void
CRemotePlayer::UpdateWeapon(void)
{
	if(m_pendingWeapon == m_currentWeapon || m_pendingWeapon >= WEAPONTYPE_TOTALWEAPONS)
		return;
	eWeaponType weapon = (eWeaponType)m_pendingWeapon;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(weapon);
	if(info->m_nModelId > 0 && !CStreaming::HasModelLoaded(info->m_nModelId)){
		CStreaming::RequestModel(info->m_nModelId, STREAMFLAGS_DONT_REMOVE);
		return;	// try again once it streams in
	}
	if(weapon != WEAPONTYPE_UNARMED)
		m_pPed->GiveWeapon(weapon, 1000, true);
	m_pPed->SetCurrentWeapon(weapon);
	m_currentWeapon = m_pendingWeapon;
}

void
CRemotePlayer::Update(uint32 renderTime)
{
	if(!m_bUsed || !m_bHaveState)
		return;
	if(m_bSpawned && m_pPed == nil){
		// The engine deleted the puppet: tidy up and build a new one.
		if(m_blip >= 0)
			CRadar::ClearBlip(m_blip);
		m_blip = -1;
		Despawn();
	}
	if(!m_bSpawned){
		if(m_modelId < 0 || !CStreaming::HasModelLoaded(m_modelId))
			return;
		Spawn();
		if(!m_bSpawned)
			return;
	}

	UpdateSeat();
	m_pPed->m_fHealth = Max(1.0f, (float)m_last.health);
	m_pPed->m_fArmour = (float)m_last.armour;
	if(m_pPed->bInVehicle){
		// The car carries us; CNetVehicles places it first.
		m_pPed->SetPedPositionInCar();
		return;
	}

	SPose pose;
	if(!m_poses.Sample(renderTime, pose))
		return;
	CVector pos(pose.pos[0], pose.pos[1], pose.pos[2]);
	float heading = QuatToHeading(pose.quat);

	if((pos - m_pPed->GetPosition()).Magnitude() > TELEPORT_DISTANCE)
		m_pPed->Teleport(pos);
	else
		m_pPed->SetPosition(pos);
	m_pPed->m_fRotationCur = heading;
	m_pPed->m_fRotationDest = heading;
	m_pPed->SetHeading(heading);
	m_pPed->SetMoveSpeed(MpSpeedFromNet(pose.vel[0]), MpSpeedFromNet(pose.vel[1]), MpSpeedFromNet(pose.vel[2]));
	m_pPed->m_fHealth = Max(1.0f, (float)m_last.health);	// shown on the tag; damage is the owner's call
	m_pPed->m_fArmour = (float)m_last.armour;
	m_pPed->GetMatrix().UpdateRW();
	m_pPed->UpdateRwFrame();
	m_pPed->RemoveAndAdd();

	UpdateAnimation();
	if(m_bSpawned)
		UpdateWeapon();
}

void
CRemotePlayer::UpdateAll(void)
{
	uint32 renderTime = CNetGame::SessionNow() - MP_INTERP_DELAY_MS;
	for(int i = 0; i < MP_MAX_PLAYERS; i++)
		if(sPlayers[i].m_bUsed)
			sPlayers[i].Update(renderTime);
}

void
CRemotePlayer::RenderNameTags(void)
{
	CVector camPos = TheCamera.GetPosition();
	for(int i = 0; i < MP_MAX_PLAYERS; i++){
		CRemotePlayer &p = sPlayers[i];
		if(!p.m_bUsed || !p.m_bSpawned || p.m_pPed == nil)
			continue;
		CVector head = p.m_pPed->GetPosition();
		head.z += 1.5f;
		float dist = (head - camPos).Magnitude();
		if(dist > NAME_TAG_DISTANCE)
			continue;
		RwV3d screen;
		float w, h;
		if(!CSprite::CalcScreenCoors(head, &screen, &w, &h, true))
			continue;

		float scale = Clamp(1.0f - dist / NAME_TAG_DISTANCE, 0.35f, 1.0f);
		CRGBA colour = ColourForId(p.m_id);
		CNetMatch::TeamColour(p.m_id, colour);	// red/blue in team deathmatch
		CFont::SetPropOn();
		CFont::SetBackgroundOff();
		CFont::SetScale(SCREEN_SCALE_X(0.5f * scale), SCREEN_SCALE_Y(0.9f * scale));
		CFont::SetCentreOn();
		CFont::SetCentreSize(SCREEN_WIDTH);
		CFont::SetJustifyOff();
		CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
		CFont::SetDropShadowPosition(1);
		CFont::SetDropColor(CRGBA(0, 0, 0, 255));
		CFont::SetColor(colour);
		CFont::PrintString(screen.x, screen.y, MpWideString(p.m_name));

		// Health bar under the name.
		float barW = SCREEN_SCALE_X(40.0f * scale);
		float barH = SCREEN_SCALE_Y(4.0f * scale);
		float barY = screen.y + SCREEN_SCALE_Y(18.0f * scale);
		float health = Clamp((float)p.m_last.health / 100.0f, 0.0f, 1.0f);
		CSprite2d::DrawRect(CRect(screen.x - barW / 2, barY, screen.x + barW / 2, barY + barH), CRGBA(0, 0, 0, 160));
		CSprite2d::DrawRect(CRect(screen.x - barW / 2, barY, screen.x - barW / 2 + barW * health, barY + barH), CRGBA(200, 40, 40, 220));
	}
	CFont::SetDropShadowPosition(0);
}

#endif
