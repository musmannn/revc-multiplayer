#pragma once

// Wire format shared by the game, relay, tools and tests.
//
// Every packet is an SEnvelope followed by one message. A message starts with
// its eMsgType byte. Types below MSG_GAME_FIRST belong to this layer (relay
// control and session basics); types from MSG_GAME_FIRST up are game payloads
// that CNetClient hands to the game unparsed.
//
// Uplink (client -> relay): envelope.player is the target for MP_ROUTE_PLAYER.
// Downlink (relay -> client): envelope.player is the sender id, or
// MP_PLAYER_RELAY for messages from the relay itself.

#include <stdint.h>

#include "MpTypes.h"
#include "ByteStream.h"

enum eMsgType : uint8_t
{
	MSG_NONE = 0,

	// Relay control
	MSG_CREATE_SESSION = 1,
	MSG_SESSION_CREATED = 2,
	MSG_JOIN_SESSION = 3,
	MSG_JOIN_RESULT = 4,
	MSG_PLAYER_JOINED = 5,
	MSG_PLAYER_LEFT = 6,
	MSG_SESSION_ENDED = 7,

	// Session
	MSG_CHAT = 16,
	MSG_PING = 17,
	MSG_PONG = 18,

	// Game payloads (Plans 2-3) start here and are opaque to this layer.
	MSG_GAME_FIRST = 64
};

enum eJoinResult
{
	JOIN_OK,
	JOIN_NOT_FOUND,
	JOIN_FULL,
	JOIN_VERSION_MISMATCH,
	JOIN_BAD_NAME
};

enum eEndReason
{
	END_HOST_LEFT,
	END_RELAY_SHUTDOWN
};

enum eLeaveReason
{
	LEAVE_QUIT,	// the player left on purpose
	LEAVE_LOST	// timed out or dropped
};

// ENet disconnect data a client sends when it leaves on purpose.
static const uint32_t MP_DISCONNECT_QUIT = 1;

// ENet peer timeouts, used by both the relay and the client. A peer that
// stops acknowledging is dropped after MP_PEER_TIMEOUT_MIN_MS at the earliest
// and MP_PEER_TIMEOUT_MAX_MS at the latest. The floor is generous because a
// game can stall for several seconds while it loads the world (ENet's own
// default is 5 s, which a slow first load can exceed).
static const uint32_t MP_PEER_TIMEOUT_MIN_MS = 15000;
static const uint32_t MP_PEER_TIMEOUT_MAX_MS = 30000;

struct SEnvelope
{
	uint8_t route;
	uint8_t player;

	SEnvelope(void) : route(MP_ROUTE_RELAY), player(MP_PLAYER_NONE) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

// Each message's Write() writes the type byte first; Read() reads the fields
// after the type byte and returns false on any truncation or overlong string.

// Session flags, set by the creator and given to every joiner.
enum
{
	MP_SESSION_DEDICATED = 1	// hosted by revc-server: id 0 has no ped
};

struct SMsgCreateSession
{
	uint16_t version;
	char name[MP_MAX_NAME_LEN + 1];
	uint8_t flags;		// MP_SESSION_*

	SMsgCreateSession(void) : version(MP_PROTOCOL_VERSION), flags(0) { name[0] = '\0'; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgSessionCreated
{
	char code[MP_INVITE_CODE_LEN + 1];
	uint8_t playerId;

	SMsgSessionCreated(void) : playerId(0) { code[0] = '\0'; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgJoinSession
{
	uint16_t version;
	char code[MP_INVITE_CODE_LEN + 1];
	char name[MP_MAX_NAME_LEN + 1];

	SMsgJoinSession(void) : version(MP_PROTOCOL_VERSION) { code[0] = '\0'; name[0] = '\0'; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgJoinResult
{
	uint8_t result;		// eJoinResult
	uint8_t playerId;	// MP_PLAYER_NONE unless JOIN_OK
	uint8_t flags;		// the session's MP_SESSION_* (0 unless JOIN_OK)

	SMsgJoinResult(void) : result(JOIN_OK), playerId(MP_PLAYER_NONE), flags(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgPlayerJoined
{
	uint8_t playerId;
	char name[MP_MAX_NAME_LEN + 1];

	SMsgPlayerJoined(void) : playerId(MP_PLAYER_NONE) { name[0] = '\0'; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgPlayerLeft
{
	uint8_t playerId;
	uint8_t reason;		// eLeaveReason

	SMsgPlayerLeft(void) : playerId(MP_PLAYER_NONE), reason(LEAVE_QUIT) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgSessionEnded
{
	uint8_t reason;		// eEndReason

	SMsgSessionEnded(void) : reason(END_HOST_LEFT) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgChat
{
	char text[MP_MAX_CHAT_LEN + 1];

	SMsgChat(void) { text[0] = '\0'; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgPing
{
	uint32_t clientSendMs;

	SMsgPing(void) : clientSendMs(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMsgPong
{
	uint32_t clientSendMs;
	uint32_t hostMs;

	SMsgPong(void) : clientSendMs(0), hostMs(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

// Trims, keeps printable ASCII 0x20-0x7E except '~', truncates to MP_MAX_NAME_LEN.
// Returns false (and an empty out) if nothing is left.
bool MpSanitizeName(const char *in, char out[MP_MAX_NAME_LEN + 1]);
