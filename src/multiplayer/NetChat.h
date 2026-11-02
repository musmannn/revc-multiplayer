#pragma once

#ifdef MULTIPLAYER

#include "GameProtocol.h"

// Text chat (T to type), join/leave notices, and the host's time and weather.
class CNetChat
{
public:
	// A free CPad::DisablePlayerControls bit (the engine uses up to 0x100).
	enum { PLAYERCONTROL_MP_CHAT = 0x200 };
	enum { NOTICE = 0xFF };	// AddLine colour id for system notices

	// Right after CPad::UpdatePads(), before the frontend reads Esc.
	static void Update(void);
	static void Render(void);
	static void AddLine(const char *text, uint8 colourId);
	static void OnChat(uint8 from, const char *text);
	static bool IsTyping(void);
	static void Reset(void);

	// Time and weather: the host is authoritative.
	static void SendWorld(void);	// host: on each join and every 10 s
	static void OnWorld(uint8 from, const SWorldState &msg);
	static void UpdateWorld(uint32 netNow);
	static void ReleaseWorld(void);	// leaving: weather back to normal
};

#endif
