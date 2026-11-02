#include "common.h"

#ifdef MULTIPLAYER

#include "NetVehicles.h"
#include "NetGame.h"
#include "RemotePlayer.h"
#include "PoseBuffer.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "Automobile.h"
#include "Bike.h"
#include "Boat.h"
#include "CarCtrl.h"
#include "ModelInfo.h"
#include "PlayerPed.h"
#include "Streaming.h"
#include "World.h"
#include "Zones.h"
#include "AudioManager.h"

struct SNetVehicle
{
	bool used;
	bool pending;		// waiting for the model before creating it
	uint16 id;
	uint8 owner;
	CVehicle *vehicle;	// registered reference: the engine nils it on delete
	SVehicleCreate create;
	CPoseBuffer poses;
	SVehicleState last;
	bool haveState;
	uint32 lastStateMs;	// NetNow of the last state received
	uint32 lastSentMs;
	uint32 emptySinceMs;	// ours, nobody inside: when that started (0 = occupied)
	uint8 seats[MP_MAX_SEATS];
};

static SNetVehicle sVehicles[CNetVehicles::MAX_NET_VEHICLES];
static uint16 sCounter;
static uint16 sLocalSeatVehicle = MP_VEHICLE_NONE;	// what we told the host
static uint8 sLocalSeat = MP_SEAT_NONE;

static const uint32 ORPHAN_TIMEOUT_MS = 3000;	// owner gone and silent: go local
static const uint32 RELEASE_EMPTY_MS = 120000;	// our car left empty this long stops syncing

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

static SNetVehicle*
FindEntry(uint16 id)
{
	if(id == MP_VEHICLE_NONE)
		return nil;
	for(int i = 0; i < CNetVehicles::MAX_NET_VEHICLES; i++)
		if(sVehicles[i].used && sVehicles[i].id == id)
			return &sVehicles[i];
	return nil;
}

static SNetVehicle*
FindEntry(CVehicle *vehicle)
{
	if(vehicle == nil)
		return nil;
	for(int i = 0; i < CNetVehicles::MAX_NET_VEHICLES; i++)
		if(sVehicles[i].used && sVehicles[i].vehicle == vehicle)
			return &sVehicles[i];
	return nil;
}

static SNetVehicle*
NewEntry(void)
{
	for(int i = 0; i < CNetVehicles::MAX_NET_VEHICLES; i++)
		if(!sVehicles[i].used){
			SNetVehicle &e = sVehicles[i];
			e.used = true;
			e.pending = false;
			e.id = MP_VEHICLE_NONE;
			e.owner = MP_PLAYER_NONE;
			e.vehicle = nil;
			e.poses.Clear();
			e.haveState = false;
			e.lastStateMs = 0;
			e.lastSentMs = 0;
			e.emptySinceMs = 0;
			for(int s = 0; s < MP_MAX_SEATS; s++)
				e.seats[s] = MP_PLAYER_NONE;
			return &e;
		}
	return nil;
}

static void
SetVehicle(SNetVehicle &e, CVehicle *vehicle)
{
	if(e.vehicle)
		e.vehicle->CleanUpOldReference((CEntity**)&e.vehicle);
	e.vehicle = vehicle;
	if(vehicle)
		vehicle->RegisterReference((CEntity**)&e.vehicle);
}

static void
FreeEntry(SNetVehicle &e)
{
	SetVehicle(e, nil);
	e.used = false;
}

// Rotation matrix columns (right, forward, up) <-> quaternion (x, y, z, w).
static void
MatrixToQuat(const CMatrix &m, float *q)
{
	const CVector &r = m.GetRight();
	const CVector &f = m.GetForward();
	const CVector &u = m.GetUp();
	float m00 = r.x, m01 = f.x, m02 = u.x;
	float m10 = r.y, m11 = f.y, m12 = u.y;
	float m20 = r.z, m21 = f.z, m22 = u.z;
	float trace = m00 + m11 + m22;
	if(trace > 0.0f){
		float s = Sqrt(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (m21 - m12) / s;
		q[1] = (m02 - m20) / s;
		q[2] = (m10 - m01) / s;
	}else if(m00 > m11 && m00 > m22){
		float s = Sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q[3] = (m21 - m12) / s;
		q[0] = 0.25f * s;
		q[1] = (m01 + m10) / s;
		q[2] = (m02 + m20) / s;
	}else if(m11 > m22){
		float s = Sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q[3] = (m02 - m20) / s;
		q[0] = (m01 + m10) / s;
		q[1] = 0.25f * s;
		q[2] = (m12 + m21) / s;
	}else{
		float s = Sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q[3] = (m10 - m01) / s;
		q[0] = (m02 + m20) / s;
		q[1] = (m12 + m21) / s;
		q[2] = 0.25f * s;
	}
}

static void
QuatToMatrix(const float *q, CMatrix &m)
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	m.GetRight() = CVector(1.0f - 2.0f*(y*y + z*z), 2.0f*(x*y + w*z), 2.0f*(x*z - w*y));
	m.GetForward() = CVector(2.0f*(x*y - w*z), 1.0f - 2.0f*(x*x + z*z), 2.0f*(y*z + w*x));
	m.GetUp() = CVector(2.0f*(x*z + w*y), 2.0f*(y*z - w*x), 1.0f - 2.0f*(x*x + y*y));
}

static void
Send(uint8 route, uint8 target, bool reliable, const CByteWriter &w)
{
	CNetGame::Client().SendGame(route, target, reliable, w);
}

static void
SendCreate(const SNetVehicle &e, uint8 route, uint8 target)
{
	if(e.vehicle == nil)
		return;
	SVehicleCreate msg;
	msg.vehicleId = e.id;
	msg.model = (uint16)e.vehicle->GetModelIndex();
	msg.col1 = e.vehicle->m_currentColour1;
	msg.col2 = e.vehicle->m_currentColour2;
	const CVector &pos = e.vehicle->GetPosition();
	msg.pos[0] = pos.x;
	msg.pos[1] = pos.y;
	msg.pos[2] = pos.z;
	MatrixToQuat(e.vehicle->GetMatrix(), msg.quat);
	CByteWriter w;
	msg.Write(w);
	Send(route, target, true, w);
}

uint16
CNetVehicles::Register(CVehicle *vehicle)
{
	if(vehicle == nil)
		return MP_VEHICLE_NONE;
	SNetVehicle *existing = FindEntry(vehicle);
	if(existing)
		return existing->id;
	SNetVehicle *e = NewEntry();
	if(e == nil)
		return MP_VEHICLE_NONE;
	// Ids are unique per owner; skip any still in use after wrapping.
	uint16 id;
	do{
		sCounter = (uint16)(sCounter % 4095 + 1);
		id = MpMakeVehicleId(CNetGame::MyId(), sCounter);
	}while(FindEntry(id));
	e->id = id;
	e->owner = CNetGame::MyId();
	SetVehicle(*e, vehicle);

	// Keep it from being culled, like the script's mission cars.
	switch(vehicle->VehicleCreatedBy){
	case RANDOM_VEHICLE:
		vehicle->VehicleCreatedBy = MISSION_VEHICLE;
		CCarCtrl::NumMissionCars++;
		CCarCtrl::NumRandomCars--;
		break;
	case PARKED_VEHICLE:
		vehicle->VehicleCreatedBy = MISSION_VEHICLE;
		CCarCtrl::NumMissionCars++;
		CCarCtrl::NumParkedCars--;
		break;
	default:
		break;
	}
	SendCreate(*e, MP_ROUTE_ALL_BUT_SELF, 0);
	return id;
}

static void
DeleteVehicle(CVehicle *vehicle)
{
	if(vehicle == nil)
		return;
	CRemotePlayer::UnseatAllFrom(vehicle);
	CPlayerPed *player = FindPlayerPed();
	if(player && player->bInVehicle && player->m_pMyVehicle == vehicle)
		return;	// never delete the car we're sitting in; it just goes local
	CWorld::Remove(vehicle);
	CWorld::RemoveReferencesToDeletedObject(vehicle);
	delete vehicle;
}

void
CNetVehicles::RemoveOwned(CVehicle *vehicle)
{
	SNetVehicle *e = FindEntry(vehicle);
	if(e && e->owner == CNetGame::MyId()){
		SVehicleRemove msg;
		msg.vehicleId = e->id;
		CByteWriter w;
		msg.Write(w);
		Send(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		FreeEntry(*e);
	}
	DeleteVehicle(vehicle);
}

CVehicle*
CNetVehicles::Find(uint16 id)
{
	SNetVehicle *e = FindEntry(id);
	return e ? e->vehicle : nil;
}

uint16
CNetVehicles::IdOf(CVehicle *vehicle)
{
	SNetVehicle *e = FindEntry(vehicle);
	return e ? e->id : MP_VEHICLE_NONE;
}

uint8
CNetVehicles::RemoteOwnerOf(const void *vehicle)
{
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(e.used && e.vehicle && e.vehicle == vehicle)
			return e.owner != CNetGame::MyId() ? e.owner : MP_PLAYER_NONE;
	}
	return MP_PLAYER_NONE;
}

bool
CNetVehicles::IsRemote(CVehicle *vehicle)
{
	SNetVehicle *e = FindEntry(vehicle);
	return e && e->owner != CNetGame::MyId();
}

void
CNetVehicles::OnCreate(uint8 from, const SVehicleCreate &msg)
{
	if(MpVehicleIdOwner(msg.vehicleId) != from || FindEntry(msg.vehicleId))
		return;	// ids are minted by their owner; duplicates are resends
	if(msg.model >= MODELINFOSIZE || CModelInfo::GetModelInfo(msg.model) == nil ||
	   CModelInfo::GetModelInfo(msg.model)->GetModelType() != MITYPE_VEHICLE)
		return;
	SNetVehicle *e = NewEntry();
	if(e == nil)
		return;
	e->id = msg.vehicleId;
	e->owner = from;
	e->create = msg;
	e->pending = true;
	e->lastStateMs = CNetGame::NetNow();
	CStreaming::RequestModel(msg.model, STREAMFLAGS_DONT_REMOVE);
}

static void
CreatePending(SNetVehicle &e)
{
	const SVehicleCreate &c = e.create;
	if(!CStreaming::HasModelLoaded(c.model))
		return;
	CVehicle *vehicle;
	if(CModelInfo::IsBoatModel(c.model))
		vehicle = new CBoat(c.model, MISSION_VEHICLE);
	else if(CModelInfo::IsBikeModel(c.model)){
		vehicle = new CBike(c.model, MISSION_VEHICLE);
		((CBike*)vehicle)->bIsStanding = true;
	}else
		vehicle = new CAutomobile(c.model, MISSION_VEHICLE);
	CVector pos(c.pos[0], c.pos[1], c.pos[2]);
	QuatToMatrix(c.quat, vehicle->GetMatrix());
	vehicle->SetPosition(pos);
	vehicle->m_currentColour1 = c.col1;
	vehicle->m_currentColour2 = c.col2;
	vehicle->SetStatus(STATUS_PLAYER_DISABLED);
	vehicle->bIsLocked = false;
	vehicle->bEngineOn = true;
	vehicle->AutoPilot.m_nCarMission = MISSION_NONE;
	vehicle->AutoPilot.m_nTempAction = TEMPACT_NONE;
	vehicle->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	vehicle->bHasBeenOwnedByPlayer = true;
	CWorld::Add(vehicle);
	CStreaming::SetModelIsDeletable(c.model);
	SetVehicle(e, vehicle);
	e.pending = false;
}

void
CNetVehicles::OnRemove(uint8 from, const SVehicleRemove &msg)
{
	SNetVehicle *e = FindEntry(msg.vehicleId);
	if(e == nil || e->owner != from)
		return;
	CVehicle *vehicle = e->vehicle;
	FreeEntry(*e);
	DeleteVehicle(vehicle);
}

void
CNetVehicles::OnState(uint8 from, const SVehicleState &msg)
{
	SNetVehicle *e = FindEntry(msg.vehicleId);
	if(e == nil || e->owner != from)
		return;
	e->last = msg;
	e->haveState = true;
	e->lastStateMs = CNetGame::NetNow();
	SPose pose;
	pose.time = msg.time;
	memcpy(pose.pos, msg.pos, sizeof(pose.pos));
	memcpy(pose.quat, msg.quat, sizeof(pose.quat));
	memcpy(pose.vel, msg.vel, sizeof(pose.vel));
	e->poses.Push(pose);
}

static void
ClearPlayerSeats(uint8 player, SNetVehicle *except)
{
	for(int i = 0; i < CNetVehicles::MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(!e.used || &e == except)
			continue;
		for(int s = 0; s < MP_MAX_SEATS; s++)
			if(e.seats[s] == player)
				e.seats[s] = MP_PLAYER_NONE;
	}
	if(except)
		for(int s = 0; s < MP_MAX_SEATS; s++)
			if(except->seats[s] == player)
				except->seats[s] = MP_PLAYER_NONE;
}

static void
BroadcastSeat(uint16 id, uint8 player, uint8 seat, bool accepted, uint8 route, uint8 target)
{
	SVehicleSeat msg;
	msg.vehicleId = id;
	msg.playerId = player;
	msg.seat = seat;
	msg.accepted = accepted ? 1 : 0;
	CByteWriter w;
	msg.Write(w);
	Send(route, target, true, w);
}

void
CNetVehicles::OnEnterRequest(uint8 from, const SVehicleEnter &msg)
{
	if(!CNetGame::IsHost())
		return;
	SNetVehicle *e = FindEntry(msg.vehicleId);
	bool ok = e && msg.seat < MP_MAX_SEATS &&
		(e->seats[msg.seat] == MP_PLAYER_NONE || e->seats[msg.seat] == from);
	if(!ok){
		BroadcastSeat(msg.vehicleId, from, msg.seat, false, MP_ROUTE_PLAYER, from);
		return;
	}
	// Arrival order decides: the seat is now taken. Everyone (us included,
	// through the relay) applies it from the broadcast.
	BroadcastSeat(msg.vehicleId, from, msg.seat, true, MP_ROUTE_ALL, 0);
}

void
CNetVehicles::OnExitRequest(uint8 from, const SVehicleExit &msg)
{
	if(!CNetGame::IsHost())
		return;
	BroadcastSeat(msg.vehicleId, from, MP_SEAT_NONE, true, MP_ROUTE_ALL, 0);
}

// Out of the car on the driver's side, like WARP_PLAYER_FROM_CAR_TO_COORD.
void
CNetVehicles::WarpLocalPlayerOut(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || !player->bInVehicle || player->m_pMyVehicle == nil)
		return;
	CVehicle *vehicle = player->m_pMyVehicle;
	CVector pos = vehicle->GetPosition() - vehicle->GetRight() * 2.0f;
	if(vehicle->pDriver == player){
		vehicle->RemoveDriver();
		if(CNetVehicles::IsRemote(vehicle))
			vehicle->SetStatus(STATUS_PLAYER_DISABLED);
	}else
		vehicle->RemovePassenger(player);
	player->RemoveInCarAnims();
	player->bInVehicle = false;
	player->m_pMyVehicle->CleanUpOldReference((CEntity**)&player->m_pMyVehicle);
	player->m_pMyVehicle = nil;
	player->SetPedState(PED_IDLE);
	player->bUsesCollision = true;
	player->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	player->ReplaceWeaponWhenExitingVehicle();
	if(player->m_pVehicleAnim)
		player->m_pVehicleAnim->blendDelta = -1000.0f;
	player->m_pVehicleAnim = nil;
	player->SetMoveState(PEDMOVE_NONE);
	CAnimManager::BlendAnimation(player->GetClump(), player->m_animGroup, ANIM_STD_IDLE, 1000.0f);
	player->RestartNonPartialAnims();
	AudioManager.PlayerJustLeftCar();
	pos.z += player->GetDistanceFromCentreOfMassToBaseOfModel();
	player->Teleport(pos);
}

void
CNetVehicles::OnSeat(const SVehicleSeat &msg)
{
	SNetVehicle *e = FindEntry(msg.vehicleId);
	uint8 me = CNetGame::MyId();
	if(!msg.accepted){
		if(msg.playerId == me){
			// Someone else got the seat first.
			WarpLocalPlayerOut();
			sLocalSeatVehicle = MP_VEHICLE_NONE;
			sLocalSeat = MP_SEAT_NONE;
		}
		return;
	}
	if(e == nil)
		return;
	ClearPlayerSeats(msg.playerId, nil);
	if(msg.seat < MP_MAX_SEATS)
		e->seats[msg.seat] = msg.playerId;

	// The driver owns the car.
	if(msg.seat == MP_SEAT_DRIVER && msg.playerId != e->owner){
		e->owner = msg.playerId;
		e->poses.Clear();
		e->haveState = false;
		e->lastStateMs = CNetGame::NetNow();
		if(e->vehicle && e->vehicle->GetStatus() != STATUS_WRECKED)
			e->vehicle->SetStatus(msg.playerId == me ? STATUS_PLAYER : STATUS_PLAYER_DISABLED);
	}
}

static uint8
LocalSeatIn(CPlayerPed *player, CVehicle *vehicle)
{
	if(vehicle->pDriver == player)
		return MP_SEAT_DRIVER;
	for(int i = 0; i < 3; i++)
		if(vehicle->pPassengers[i] == player)
			return (uint8)(i + 1);
	return MP_SEAT_NONE;
}

uint16
CNetVehicles::LocalVehicleId(uint8 &seat)
{
	seat = MP_SEAT_DRIVER;
	CPlayerPed *player = FindPlayerPed();
	if(player == nil || !player->bInVehicle || player->m_pMyVehicle == nil)
		return MP_VEHICLE_NONE;
	uint16 id = IdOf(player->m_pMyVehicle);
	if(id != MP_VEHICLE_NONE)
		seat = LocalSeatIn(player, player->m_pMyVehicle);
	return id;
}

static void
SendEnter(uint16 id, uint8 seat)
{
	SVehicleEnter msg;
	msg.vehicleId = id;
	msg.seat = seat;
	CByteWriter w;
	msg.Write(w);
	Send(MP_ROUTE_HOST, 0, true, w);
}

static void
SendExit(uint16 id)
{
	SVehicleExit msg;
	msg.vehicleId = id;
	CByteWriter w;
	msg.Write(w);
	Send(MP_ROUTE_HOST, 0, true, w);
}

static void
UpdateLocalPlayerSeat(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	CVehicle *vehicle = player->bInVehicle ? player->m_pMyVehicle : nil;
	uint16 id = MP_VEHICLE_NONE;
	uint8 seat = MP_SEAT_NONE;
	if(vehicle){
		seat = LocalSeatIn(player, vehicle);
		id = CNetVehicles::IdOf(vehicle);
		if(id == MP_VEHICLE_NONE && seat == MP_SEAT_DRIVER && vehicle->GetStatus() != STATUS_WRECKED)
			id = CNetVehicles::Register(vehicle);
	}
	if(id == sLocalSeatVehicle && seat == sLocalSeat)
		return;
	if(sLocalSeatVehicle != MP_VEHICLE_NONE && id != sLocalSeatVehicle)
		SendExit(sLocalSeatVehicle);
	if(id != MP_VEHICLE_NONE && seat != MP_SEAT_NONE)
		SendEnter(id, seat);
	sLocalSeatVehicle = id;
	sLocalSeat = id != MP_VEHICLE_NONE ? seat : MP_SEAT_NONE;
}

static void
PlaceRemote(SNetVehicle &e, uint32 renderTime)
{
	CVehicle *vehicle = e.vehicle;
	SPose pose;
	if(!e.poses.Sample(renderTime, pose))
		return;
	if(vehicle->GetStatus() != STATUS_WRECKED)
		vehicle->SetStatus(STATUS_PLAYER_DISABLED);
	QuatToMatrix(pose.quat, vehicle->GetMatrix());
	vehicle->SetPosition(CVector(pose.pos[0], pose.pos[1], pose.pos[2]));
	vehicle->SetMoveSpeed(MpSpeedFromNet(pose.vel[0]), MpSpeedFromNet(pose.vel[1]), MpSpeedFromNet(pose.vel[2]));
	const SVehicleState &s = e.last;
	vehicle->SetTurnSpeed(MpSpeedFromNet(s.turn[0]), MpSpeedFromNet(s.turn[1]), MpSpeedFromNet(s.turn[2]));
	vehicle->m_fSteerAngle = s.steer;
	vehicle->m_fGasPedal = s.gas;
	vehicle->m_fBrakePedal = s.brake;
	vehicle->bIsHandbrakeOn = !!(s.flags & VSF_HANDBRAKE);
	vehicle->bLightsOn = !!(s.flags & VSF_LIGHTS);
	vehicle->bEngineOn = !!(s.flags & VSF_ENGINE);
	vehicle->m_bSirenOrAlarm = !!(s.flags & VSF_SIREN);
	vehicle->m_fHealth = (float)s.health;
	if(s.health == 0 && vehicle->GetStatus() != STATUS_WRECKED){
		// Puppets die by their own player's state, not by our copy blowing up;
		// the owner as culprit keeps the blast from counting twice for us.
		CRemotePlayer::UnseatAllFrom(vehicle);
		CRemotePlayer *owner = CRemotePlayer::Get(e.owner);
		vehicle->BlowUpCar(owner ? (CEntity*)owner->m_pPed : nil);
	}
	vehicle->GetMatrix().UpdateRW();
	vehicle->UpdateRwFrame();
	vehicle->RemoveAndAdd();

	// Keep whoever sits in it glued to the new pose this frame.
	CPlayerPed *player = FindPlayerPed();
	if(player && player->bInVehicle && player->m_pMyVehicle == vehicle)
		player->SetPedPositionInCar();
}

void
CNetVehicles::Update(uint32 renderTime)
{
	uint8 me = CNetGame::MyId();
	uint32 now = CNetGame::NetNow();
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(!e.used)
			continue;
		if(e.pending){
			CreatePending(e);
			continue;
		}
		if(e.vehicle == nil){
			// The engine deleted it (e.g. a wreck was cleaned up).
			if(e.owner == me){
				SVehicleRemove msg;
				msg.vehicleId = e.id;
				CByteWriter w;
				msg.Write(w);
				Send(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
			}
			FreeEntry(e);
			continue;
		}
		if(e.owner == me){
			// An abandoned car of ours goes back to being ordinary traffic,
			// so it can be culled and its slot reused.
			if(!IsEmpty(e.vehicle)){
				e.emptySinceMs = 0;
				continue;
			}
			if(e.emptySinceMs == 0)
				e.emptySinceMs = now | 1;
			else if(now - e.emptySinceMs >= RELEASE_EMPTY_MS){
				SVehicleRemove msg;
				msg.vehicleId = e.id;
				CByteWriter w;
				msg.Write(w);
				Send(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
				if(e.vehicle->VehicleCreatedBy == MISSION_VEHICLE){
					e.vehicle->VehicleCreatedBy = RANDOM_VEHICLE;
					CCarCtrl::NumMissionCars--;
					CCarCtrl::NumRandomCars++;
				}
				FreeEntry(e);
			}
			continue;
		}
		if(e.owner == MP_PLAYER_NONE || (CNetGame::PlayerName(e.owner) == nil && now - e.lastStateMs > ORPHAN_TIMEOUT_MS)){
			// Nobody owns it any more: it becomes an ordinary local car.
			e.vehicle->SetStatus(STATUS_ABANDONED);
			FreeEntry(e);
			continue;
		}
		// We're getting in as its new driver: don't override our own input
		// while the host's seat confirmation is on its way.
		CPlayerPed *local = FindPlayerPed();
		if(local && local->bInVehicle && e.vehicle->pDriver == local)
			continue;
		if(e.haveState)
			PlaceRemote(e, renderTime);
	}
	UpdateLocalPlayerSeat();
}

void
CNetVehicles::SendOwned(uint32 netNow)
{
	uint8 me = CNetGame::MyId();
	uint32 sessionNow = CNetGame::SessionNow();
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(!e.used || e.pending || e.owner != me || e.vehicle == nil)
			continue;
		CVehicle *v = e.vehicle;
		bool active = v->pDriver != nil || v->GetMoveSpeed().MagnitudeSqr() > sq(0.001f);
		uint32 interval = active ? 1000 / MP_SNAPSHOT_HZ : 500;
		if(netNow - e.lastSentMs < interval)
			continue;
		e.lastSentMs = netNow;

		SVehicleState s;
		s.time = sessionNow;
		s.vehicleId = e.id;
		const CVector &pos = v->GetPosition();
		s.pos[0] = pos.x;
		s.pos[1] = pos.y;
		s.pos[2] = pos.z;
		MatrixToQuat(v->GetMatrix(), s.quat);
		const CVector &vel = v->GetMoveSpeed();
		const CVector &turn = v->GetTurnSpeed();
		s.vel[0] = MpSpeedToNet(vel.x);
		s.vel[1] = MpSpeedToNet(vel.y);
		s.vel[2] = MpSpeedToNet(vel.z);
		s.turn[0] = MpSpeedToNet(turn.x);
		s.turn[1] = MpSpeedToNet(turn.y);
		s.turn[2] = MpSpeedToNet(turn.z);
		s.steer = v->m_fSteerAngle;
		s.gas = v->m_fGasPedal;
		s.brake = v->m_fBrakePedal;
		s.flags = (v->bIsHandbrakeOn ? VSF_HANDBRAKE : 0) | (v->bLightsOn ? VSF_LIGHTS : 0) |
			(v->bEngineOn ? VSF_ENGINE : 0) | (v->m_bSirenOrAlarm ? VSF_SIREN : 0);
		s.health = (uint16)Clamp(v->m_fHealth, 0.0f, 1000.0f);
		if(v->GetStatus() == STATUS_WRECKED)
			s.health = 0;
		CByteWriter w;
		s.Write(w);
		Send(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
	}
}

void
CNetVehicles::OnPlayerJoined(uint8 player)
{
	uint8 me = CNetGame::MyId();
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(e.used && !e.pending && e.owner == me)
			SendCreate(e, MP_ROUTE_PLAYER, player);
	}
}

void
CNetVehicles::RemoveAllOwnedBy(uint8 player)
{
	uint8 me = CNetGame::MyId();
	CPlayerPed *local = FindPlayerPed();
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(!e.used)
			continue;
		uint8 next = MpNextVehicleOwner(e.owner, e.seats, player);
		ClearPlayerSeats(player, &e);
		if(e.owner != player)
			continue;
		bool localInside = e.vehicle && local && local->bInVehicle && local->m_pMyVehicle == e.vehicle;
		if(next != MP_PLAYER_NONE){
			// Everyone computes the same next owner, who takes over whatever
			// seat they're in.
			e.owner = next;
			e.poses.Clear();
			e.haveState = false;
			e.lastStateMs = CNetGame::NetNow();
			if(next == me && e.vehicle && e.vehicle->GetStatus() != STATUS_WRECKED)
				e.vehicle->SetStatus(e.vehicle->pDriver == local ? STATUS_PLAYER : STATUS_ABANDONED);
			continue;
		}
		CVehicle *vehicle = e.vehicle;
		FreeEntry(e);
		if(vehicle == nil)
			continue;
		if(localInside)
			vehicle->SetStatus(vehicle->pDriver == local ? STATUS_PLAYER : STATUS_ABANDONED);	// goes local
		else
			DeleteVehicle(vehicle);
	}
}

void
CNetVehicles::Clear(void)
{
	uint8 me = CNetGame::MyId();
	for(int i = 0; i < MAX_NET_VEHICLES; i++){
		SNetVehicle &e = sVehicles[i];
		if(!e.used)
			continue;
		// Remote copies stay behind as ordinary local cars.
		if(e.vehicle && e.owner != me && e.vehicle->GetStatus() == STATUS_PLAYER_DISABLED)
			e.vehicle->SetStatus(STATUS_ABANDONED);
		FreeEntry(e);
	}
	sLocalSeatVehicle = MP_VEHICLE_NONE;
	sLocalSeat = MP_SEAT_NONE;
}

bool
CNetVehicles::OnLocalPlayerWantsToJack(CPed *player, CVehicle *vehicle)
{
	if(player == nil || vehicle == nil)
		return false;
	bool remoteInside = CRemotePlayer::FindByPed(vehicle->pDriver) != nil;
	for(int i = 0; i < 3 && !remoteInside; i++)
		remoteInside = CRemotePlayer::FindByPed(vehicle->pPassengers[i]) != nil;
	if(!remoteInside)
		return false;	// ordinary car: the engine does its thing

	// Ride along: warp into a free passenger seat (remote enter/exit is a
	// warp in milestone 1). Nothing happens if the car is full.
	bool freeSeat = false;
	for(int i = 0; i < vehicle->m_nNumMaxPassengers && i < 3; i++)
		if(vehicle->pPassengers[i] == nil)
			freeSeat = true;
	player->RestorePreviousState();
	if(!freeSeat || vehicle->IsBike()){
		player->RestorePreviousObjective();
		return true;
	}
	uint8 status = vehicle->GetStatus();
	player->SetObjective(OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);
	player->WarpPedIntoCar(vehicle);
	// WarpPedIntoCar's passenger path doesn't count the passenger, but
	// RemovePassenger uncounts them.
	vehicle->m_nNumPassengers++;
	// WarpPedIntoCar hands the player's car STATUS_PLAYER; the friend drives it.
	vehicle->SetStatus(status);
	return true;
}

#endif
