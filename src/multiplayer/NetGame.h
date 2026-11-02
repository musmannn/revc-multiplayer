#pragma once

// reVC multiplayer: the game side of a session. Owns the CNetClient from
// mp/common and hooks into the main loop. Everything here is inert outside a
// session (IsActive() is false), so single player behaves as before.

#ifdef MULTIPLAYER

#include "NetClient.h"

// Built-in relay. TODO: the owner's cloud relay address once the VPS exists
// (docs/multiplayer/README.md, open questions).
#define MP_DEFAULT_RELAY "127.0.0.1:7777"

// Engine speeds are game units per 1/50 s step; network speeds are per ms.
inline float MpSpeedToNet(float engineSpeed) { return engineSpeed * (50.0f / 1000.0f); }
inline float MpSpeedFromNet(float netSpeed) { return netSpeed * (1000.0f / 50.0f); }

class CNetGame
{
public:
	// Settings, stored in reVC.ini [Multiplayer] Name / Relay.
	static char ms_playerName[MP_MAX_NAME_LEN + 1];
	static char ms_relayAddress[64];	// "host" or "host:port"

	static void Init(void);
	static void Shutdown(void);
	// Pumps the network. Called every frame, including while paused, in the
	// frontend and from the loading screen.
	static void Service(void);
	static void ServiceDuringLoad(void);
	// Called from long engine loops (CStreaming::LoadAllRequestedModels).
	// While the multiplayer world is loading, services the network at most
	// every 100 ms so a slow first load can't time the session out. Does
	// nothing once the world is running: then the frame loop services it.
	static void KeepAliveWhileLoading(void);
	// Simulation sync. Called only while the game isn't paused.
	static void Process(void);
	static void Render2d(void);
	// Right after CPad::UpdatePads() in CGame::Process: chat input.
	static void AfterPadUpdate(void);

	// In a session with the multiplayer world running.
	static bool IsActive(void);
	// Connecting, or in a session (world loaded or not).
	static bool IsConnected(void);
	static bool IsHost(void);
	// The session is hosted by revc-server: id 0 is the server, not a player.
	static bool IsDedicated(void);
	static uint8 MyId(void);
	static CNetClient &Client(void);
	// Session time now (the host's clock).
	static uint32 SessionNow(void);
	// The network clock: real milliseconds, running while paused.
	static uint32 NetNow(void);

	static bool HostSession(void);
	static bool JoinSession(const char *code);
	static void LeaveSession(void);
	static const char *InviteCode(void);
	static int NumPlayers(void);	// people: a dedicated server isn't counted
	static int MaxPlayers(void);
	static const char *PlayerName(uint8 id);	// nil if no such player
	// MP_* key of the last failure/end reason, or nil. Cleared by Host/Join.
	static const char *LastStatusKey(void);

	// CTheScripts::Init loads freeroam_miami.scm while this is true.
	static bool WantsWorld(void);
	// The running world uses freeroam_miami.scm, so mission scripts must be
	// read from it too (CTheScripts::OpenScript).
	static void SetFreeroamScriptLoaded(bool loaded);
	static bool FreeroamScriptLoaded(void);
	static void ShowNoSave(void);
	// End of CGame::Initialise / ReInitGameObjectVariables.
	static void OnWorldStarted(void);
	// Start of CGame::ShutDownForRestart / ShutDown.
	static void OnWorldShutdown(void);

	static void LoadSettings(void);
	static void SaveSettings(void);
};

#endif
