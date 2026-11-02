#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"

class CByteReader;
class CPlayerPed;

// Deathmatch and team deathmatch (milestone 2). The host runs CMatchHost
// (mp/common) and scores the GMSG_DIED reports everyone already sends.
class CNetMatch
{
public:
	// A free CPad::DisablePlayerControls bit (0x200 chat, 0x400 menu, 0x800 race).
	enum { PLAYERCONTROL_MP_MATCH = 0x1000 };

	static void Init(void);
	static void Process(void);
	static void Render(void);
	static void RenderScoreboard(void);	// while Tab is held, any session
	static bool IsPlaying(void);		// the local player is in a match
	static bool HostBusy(void);		// host: a match is pending or running
	static void OnMessage(uint8 from, uint8 type, CByteReader &body);
	static void OnDied(uint8 victim, uint8 killer);	// every GMSG_DIED
	static void OnRespawned(uint8 id);		// every GMSG_RESPAWNED
	static void OnPlayerJoined(uint8 id);
	static void OnPlayerLeft(uint8 id);
	static void Reset(void);

	// Team deathmatch: same team as the local player (no friendly fire).
	static bool IsTeammate(uint8 id);
	// Name tag colour override for team deathmatch.
	static bool TeamColour(uint8 id, CRGBA &out);
	// Respawn while playing: match loadout instead of free roam's.
	static void GiveMatchLoadout(CPlayerPed *player);
	static void RequestMatch(int mode);

	enum { MAX_ARENAS = 16 };
	// data/mp/arenas/*.route (type arena), listed in its index.txt.
	static void LoadArenas(void);
	static int NumArenas(void);
	static const SRaceRoute *GetArena(int i);
};

#endif
