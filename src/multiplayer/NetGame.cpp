#include "common.h"

#ifdef MULTIPLAYER

#include <chrono>

#include "NetGame.h"
#include "MpText.h"
#include "SpawnPoints.h"
#include "RemotePlayer.h"
#include "NetVehicles.h"
#include "NetCombat.h"
#include "NetChat.h"
#include "FreeroamMenu.h"
#include "NetRace.h"
#include "NetMatch.h"
#include "RouteRecorder.h"
#include "GameProtocol.h"
#include "Frontend.h"
#include "Messages.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "Timer.h"
#include "World.h"
#include "Streaming.h"

#ifdef LOAD_INI_SETTINGS
void SaveINISettings();
#endif

char CNetGame::ms_playerName[MP_MAX_NAME_LEN + 1] = "Player";
char CNetGame::ms_relayAddress[64] = MP_DEFAULT_RELAY;

static CNetClient sClient;
static bool sWantWorld;		// session ready; next world start is multiplayer
static bool sInGame;		// multiplayer world running
static bool sLoading;		// servicing from the loading screen
static bool sPendingStart;	// start the world at the next safe point
static bool sPendingEnd;	// end-of-session UI at the next safe point
static const char *sStatusKey;
static uint32 sLastStateSentMs;
static char sCode[MP_INVITE_CODE_LEN + 1];

struct SRosterEntry
{
	bool used;
	char name[MP_MAX_NAME_LEN + 1];
};
static SRosterEntry sRoster[MP_MAX_PLAYERS];

static void
ClearRoster(void)
{
	memset(sRoster, 0, sizeof(sRoster));
}

static void
ShowMessage(const char *key)
{
	wchar *text = MpGetText(key);
	if(text && !FrontEndMenuManager.m_bGameNotLoaded)
		CMessages::AddMessageJumpQ(text, 5000, 0);
}

// Local teardown after the session is gone (ended, lost or left).
static void
EndLocal(const char *statusKey)
{
	bool wasInGame = sInGame;
	CRemotePlayer::RemoveAll();
	CNetVehicles::Clear();
	CNetCombat::Reset();
	CNetChat::Reset();
	CNetChat::ReleaseWorld();
	CFreeroamMenu::Reset();
	CNetRace::Reset();
	CNetMatch::Reset();
	sWantWorld = false;
	sInGame = false;
	sPendingStart = false;
	ClearRoster();
	sStatusKey = statusKey;
	if(wasInGame && statusKey)
		sPendingEnd = true;
}

class CNetGameListener : public INetClientListener
{
public:
	void OnSessionReady(const char *code, uint8 myId, bool isHost) override
	{
		(void)myId;
		(void)isHost;
		strncpy(sCode, code, sizeof(sCode) - 1);
		sCode[sizeof(sCode) - 1] = '\0';
		ClearRoster();
		if(myId < MP_MAX_PLAYERS){
			sRoster[myId].used = true;
			strncpy(sRoster[myId].name, CNetGame::ms_playerName, MP_MAX_NAME_LEN);
		}
		sStatusKey = nil;
		sWantWorld = true;
		sPendingStart = true;
	}
	void OnJoinFailed(eJoinResult result) override
	{
		const char *key = "MP_NOTFOUND";
		switch(result){
		case JOIN_FULL: key = "MP_FULL"; break;
		case JOIN_VERSION_MISMATCH: key = "MP_VERSION"; break;
		case JOIN_BAD_NAME: key = "MP_BADNAME"; break;
		default: break;
		}
		EndLocal(key);
		ShowMessage(key);
	}
	void OnPlayerJoined(uint8 id, const char *name) override
	{
		if(id >= MP_MAX_PLAYERS)
			return;
		sRoster[id].used = true;
		strncpy(sRoster[id].name, name, MP_MAX_NAME_LEN);
		sRoster[id].name[MP_MAX_NAME_LEN] = '\0';
		if(sInGame){
			CRemotePlayer::Add(id, sRoster[id].name);
			CNetVehicles::OnPlayerJoined(id);
			CNetChat::SendWorld();
			CNetRace::OnPlayerJoined(id);
			char line[64];
			snprintf(line, sizeof(line), "%s joined", sRoster[id].name);
			CNetChat::AddLine(line, CNetChat::NOTICE);
		}
	}
	void OnPlayerLeft(uint8 id) override
	{
		if(id >= MP_MAX_PLAYERS)
			return;
		if(sInGame && sRoster[id].used){
			char line[64];
			snprintf(line, sizeof(line), "%s left", sRoster[id].name);
			CNetChat::AddLine(line, CNetChat::NOTICE);
		}
		sRoster[id].used = false;
		if(sInGame){
			CNetRace::OnPlayerLeft(id);
			CNetMatch::OnPlayerLeft(id);
			CNetVehicles::RemoveAllOwnedBy(id);
		}
		CRemotePlayer::Remove(id);
	}
	void OnSessionEnded(eEndReason reason) override
	{
		EndLocal(reason == END_RELAY_SHUTDOWN ? "MP_ENDED_RELAY" : "MP_ENDED_HOST");
	}
	void OnConnectionLost(void) override
	{
		EndLocal("MP_LOST");
	}
	void OnChat(uint8 from, const char *text) override
	{
		if(sInGame)
			CNetChat::OnChat(from, text);
	}
	void OnGameMessage(uint8 from, uint8 type, CByteReader &body) override
	{
		// Game messages only make sense once the multiplayer world runs.
		if(!sInGame)
			return;
		if(type >= GMSG_RACE_FIRST && type <= GMSG_RACE_LAST){
			CNetRace::OnMessage(from, type, body);
			return;
		}
		if(type >= GMSG_MATCH_FIRST && type <= GMSG_MATCH_LAST){
			CNetMatch::OnMessage(from, type, body);
			return;
		}
		switch(type){
		case GMSG_PLAYER_STATE: {
			SPlayerState state;
			CRemotePlayer *player = CRemotePlayer::Get(from);
			if(player && state.Read(body)){
				bool first = !player->m_bHaveState;
				player->OnState(state);
				// Their first state means their world is up: what we sent at
				// join time was dropped while they were loading, so send it now.
				if(first){
					CNetVehicles::OnPlayerJoined(from);
					CNetChat::SendWorld();
					CNetRace::OnPlayerJoined(from);
					CNetMatch::OnPlayerJoined(from);
				}
			}
			break;
		}
		case GMSG_VEHICLE_STATE: {
			SVehicleState msg;
			if(msg.Read(body))
				CNetVehicles::OnState(from, msg);
			break;
		}
		case GMSG_VEHICLE_CREATE: {
			SVehicleCreate msg;
			if(msg.Read(body))
				CNetVehicles::OnCreate(from, msg);
			break;
		}
		case GMSG_VEHICLE_REMOVE: {
			SVehicleRemove msg;
			if(msg.Read(body))
				CNetVehicles::OnRemove(from, msg);
			break;
		}
		case GMSG_VEHICLE_ENTER: {
			SVehicleEnter msg;
			if(msg.Read(body))
				CNetVehicles::OnEnterRequest(from, msg);
			break;
		}
		case GMSG_VEHICLE_SEAT: {
			SVehicleSeat msg;
			// Seat decisions only count from the host.
			if(from == 0 && msg.Read(body))
				CNetVehicles::OnSeat(msg);
			break;
		}
		case GMSG_DAMAGE: {
			SDamage msg;
			if(msg.Read(body))
				CNetCombat::OnDamage(from, msg);
			break;
		}
		case GMSG_DIED: {
			SDied msg;
			if(msg.Read(body)){
				CNetCombat::OnDied(from, msg);
				CNetMatch::OnDied(from, msg.killerId);
			}
			break;
		}
		case GMSG_RESPAWNED: {
			SRespawned msg;
			if(msg.Read(body)){
				CNetCombat::OnRespawned(from, msg);
				CNetMatch::OnRespawned(from);
			}
			break;
		}
		case GMSG_SHOT: {
			SShot msg;
			if(msg.Read(body))
				CNetCombat::OnShot(from, msg);
			break;
		}
		case GMSG_WORLD: {
			SWorldState msg;
			if(msg.Read(body))
				CNetChat::OnWorld(from, msg);
			break;
		}
		case GMSG_VEHICLE_EXIT: {
			SVehicleExit msg;
			if(msg.Read(body))
				CNetVehicles::OnExitRequest(from, msg);
			break;
		}
		default:
			break;
		}
	}
};
static CNetGameListener sListener;

uint32
CNetGame::NetNow(void)
{
	// Real time, independent of the game timer (which stops while paused).
	using namespace std::chrono;
	return (uint32)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint32
CNetGame::SessionNow(void)
{
	return sClient.SessionTime(NetNow());
}

void
CNetGame::Init(void)
{
	sWantWorld = false;
	sInGame = false;
	sPendingStart = false;
	sPendingEnd = false;
	sStatusKey = nil;
	ClearRoster();
}

void
CNetGame::Shutdown(void)
{
	sClient.Leave();
	EndLocal(nil);
}

void
CNetGame::ServiceDuringLoad(void)
{
	// Keeps the connection alive while the world loads. UI reactions wait
	// for the next normal Service().
	sLoading = true;
	sClient.Service(NetNow(), &sListener);
	sLoading = false;
}

void
CNetGame::KeepAliveWhileLoading(void)
{
	static uint32 lastMs;
	if(sInGame || sLoading || !sClient.IsActive())
		return;
	uint32 now = NetNow();
	if(now - lastMs < 100)
		return;
	lastMs = now;
	ServiceDuringLoad();
}

void
CNetGame::Service(void)
{
	if(sLoading)
		return;
	sClient.Service(NetNow(), &sListener);

	if(sPendingStart){
		sPendingStart = false;
		// The New Game path: closes the frontend and restarts the world,
		// which loads freeroam_miami.scm because WantsWorld() is true.
		FrontEndMenuManager.DoSettingsBeforeStartingAGame();
	}
	if(sPendingEnd){
		sPendingEnd = false;
		if(sStatusKey)
			ShowMessage(sStatusKey);
		// Back to the frontend, where the Multiplayer page shows the reason.
		if(!FrontEndMenuManager.m_bMenuActive && !FrontEndMenuManager.m_bGameNotLoaded){
			FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
			FrontEndMenuManager.SwitchMenuOnAndOff();
		}
	}
}

// Our own ped, at MP_SNAPSHOT_HZ, to everyone else.
static void
SendLocalState(void)
{
	CPlayerPed *player = FindPlayerPed();
	if(player == nil)
		return;
	SPlayerState s;
	s.time = CNetGame::SessionNow();
	s.skin = 0;	// default skin by player id
	CVehicle *vehicle = player->bInVehicle ? player->m_pMyVehicle : nil;
	if(vehicle)
		s.flags |= PSF_IN_VEHICLE;
	if(player->DyingOrDead())
		s.flags |= PSF_DEAD;
	if(player->bIsAimingGun)
		s.flags |= PSF_AIMING;
	if(player->bIsDucking)
		s.flags |= PSF_CROUCH;
	if(player->GetPedState() == PED_JUMP)
		s.flags |= PSF_JUMPING;
	CVector pos = vehicle ? vehicle->GetPosition() : player->GetPosition();
	CVector vel = vehicle ? vehicle->GetMoveSpeed() : player->GetMoveSpeed();
	s.pos[0] = pos.x;
	s.pos[1] = pos.y;
	s.pos[2] = pos.z;
	s.heading = player->m_fRotationCur;
	s.vel[0] = MpSpeedToNet(vel.x);
	s.vel[1] = MpSpeedToNet(vel.y);
	s.vel[2] = MpSpeedToNet(vel.z);
	s.moveState = (uint8)player->m_nMoveState;
	s.weapon = (uint8)player->GetWeapon()->m_eWeaponType;
	s.health = (uint8)Clamp(player->m_fHealth, 0.0f, 255.0f);
	s.armour = (uint8)Clamp(player->m_fArmour, 0.0f, 255.0f);
	s.vehicleId = CNetVehicles::LocalVehicleId(s.seat);
	CByteWriter w;
	s.Write(w);
	sClient.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
}

void
CNetGame::Process(void)
{
	if(!IsActive())
		return;
	uint32 now = NetNow();
	uint32 renderTime = SessionNow() - MP_INTERP_DELAY_MS;
	CNetVehicles::Update(renderTime);
	CRemotePlayer::UpdateAll();
	CNetCombat::Update();
	CNetRace::Process();
	CNetMatch::Process();
	CRouteRecorder::Process();
	if(now - sLastStateSentMs >= 1000 / MP_SNAPSHOT_HZ){
		sLastStateSentMs = now;
		SendLocalState();
	}
	CNetVehicles::SendOwned(now);
	CNetChat::UpdateWorld(now);
}

void
CNetGame::Render2d(void)
{
	if(!IsActive())
		return;
	CRemotePlayer::RenderNameTags();
	CNetRace::Render();
	CNetMatch::Render();
	CNetChat::Render();
	CFreeroamMenu::Render();
	CNetMatch::RenderScoreboard();
}

void
CNetGame::AfterPadUpdate(void)
{
	CNetChat::Update();
	CFreeroamMenu::Update();
}

void
CNetGame::OnWorldShutdown(void)
{
	// The engine is about to clear the world: drop our puppets first so
	// nothing points at deleted peds. The session itself stays.
	CNetRace::Reset();
	CNetMatch::Reset();
	CRemotePlayer::RemoveAll();
	CNetVehicles::Clear();
	CFreeroamMenu::Reset();
	sInGame = false;
}

bool
CNetGame::IsActive(void)
{
	return sInGame && sClient.IsInSession();
}

bool
CNetGame::IsConnected(void)
{
	return sClient.IsActive();
}

bool
CNetGame::IsDedicated(void)
{
	return sClient.IsDedicatedSession();
}

bool
CNetGame::IsHost(void)
{
	return sClient.IsHost();
}

uint8
CNetGame::MyId(void)
{
	return sClient.MyId();
}

CNetClient&
CNetGame::Client(void)
{
	return sClient;
}

// "host" or "host:port" from the Relay setting.
static void
ParseRelay(char *host, int hostSize, uint16 &port)
{
	strncpy(host, CNetGame::ms_relayAddress, hostSize - 1);
	host[hostSize - 1] = '\0';
	port = MP_DEFAULT_RELAY_PORT;
	char *colon = strrchr(host, ':');
	if(colon){
		int p = atoi(colon + 1);
		if(p > 0 && p < 65536)
			port = (uint16)p;
		*colon = '\0';
	}
}

bool
CNetGame::HostSession(void)
{
	if(sClient.IsActive())
		return false;
	char host[64];
	uint16 port;
	ParseRelay(host, sizeof(host), port);
	sStatusKey = nil;
	if(!sClient.Host(host, port, ms_playerName)){
		sStatusKey = "MP_BADRELAY";
		return false;
	}
	ShowMessage("MP_CONNECTING");
	return true;
}

bool
CNetGame::JoinSession(const char *code)
{
	if(sClient.IsActive())
		return false;
	char host[64];
	uint16 port;
	ParseRelay(host, sizeof(host), port);
	sStatusKey = nil;
	if(!sClient.Join(host, port, code, ms_playerName)){
		sStatusKey = "MP_BADRELAY";
		return false;
	}
	ShowMessage("MP_CONNECTING");
	return true;
}

void
CNetGame::LeaveSession(void)
{
	sClient.Leave();
	EndLocal(nil);
}

const char*
CNetGame::InviteCode(void)
{
	return sClient.IsInSession() ? sCode : "";
}

int
CNetGame::NumPlayers(void)
{
	int n = 0;
	for(int i = IsDedicated() ? 1 : 0; i < MP_MAX_PLAYERS; i++)
		if(sRoster[i].used)
			n++;
	return n;
}

int
CNetGame::MaxPlayers(void)
{
	return IsDedicated() ? MP_MAX_PLAYERS - 1 : MP_MAX_PLAYERS;
}

const char*
CNetGame::PlayerName(uint8 id)
{
	if(id >= MP_MAX_PLAYERS || !sRoster[id].used)
		return nil;
	return sRoster[id].name;
}

const char*
CNetGame::LastStatusKey(void)
{
	return sStatusKey;
}

bool
CNetGame::WantsWorld(void)
{
	// Loading a save always means the story.
	return sWantWorld && !FrontEndMenuManager.m_bWantToLoad;
}

static bool sFreeroamScriptLoaded;

void
CNetGame::SetFreeroamScriptLoaded(bool loaded)
{
	sFreeroamScriptLoaded = loaded;
}

bool
CNetGame::FreeroamScriptLoaded(void)
{
	return sFreeroamScriptLoaded;
}

void
CNetGame::ShowNoSave(void)
{
	ShowMessage("MP_NOSAVE");
}

void
CNetGame::OnWorldStarted(void)
{
	if(!sWantWorld)
		return;
	if(FrontEndMenuManager.m_bWantToLoad){
		// A save is being loaded: that's single player.
		LeaveSession();
		return;
	}
	if(!sClient.IsInSession()){
		// The session died while the world was loading.
		EndLocal(sStatusKey ? sStatusKey : "MP_LOST");
		sPendingEnd = sStatusKey != nil;
		return;
	}
	sInGame = true;
	for(int i = 0; i < MP_MAX_PLAYERS; i++)
		if(sRoster[i].used && i != sClient.MyId())
			CRemotePlayer::Add((uint8)i, sRoster[i].name);

	CPlayerPed *player = FindPlayerPed();
	if(player){
		CVector pos;
		float heading;
		CSpawnPoints::Pick(sClient.MyId(), pos, heading);
		CStreaming::LoadSceneCollision(pos);
		CStreaming::LoadScene(pos);
		player->Teleport(pos);
		player->m_fRotationCur = heading;
		player->m_fRotationDest = heading;
		player->SetHeading(heading);
		CSpawnPoints::GiveLoadout(player);
	}
	CNetChat::SendWorld();
	CNetRace::Init();
	CNetMatch::Init();
}

void
CNetGame::LoadSettings(void)
{
	// Read by LoadINISettings; this sanitizes what it found.
	char clean[MP_MAX_NAME_LEN + 1];
	if(MpSanitizeName(ms_playerName, clean))
		strcpy(ms_playerName, clean);
	else
		strcpy(ms_playerName, "Player");
	if(ms_relayAddress[0] == '\0')
		strcpy(ms_relayAddress, MP_DEFAULT_RELAY);
}

void
CNetGame::SaveSettings(void)
{
#ifdef LOAD_INI_SETTINGS
	SaveINISettings();
#endif
}

#endif
