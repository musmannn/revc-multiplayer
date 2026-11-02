#pragma once

// Constants and enums shared by the game, relay, tools and tests.
// No reVC headers here: mp/common must build on its own.

#include <stdint.h>

static const uint16_t MP_PROTOCOL_VERSION = 2;
static const uint32_t MP_MAX_PLAYERS = 8;		// host included
static const uint16_t MP_DEFAULT_RELAY_PORT = 7777;
static const uint32_t MP_SNAPSHOT_HZ = 20;
static const uint32_t MP_INTERP_DELAY_MS = 100;
static const uint32_t MP_MAX_EXTRAPOLATE_MS = 250;
static const uint32_t MP_MAX_NAME_LEN = 23;		// characters, excluding NUL
static const uint32_t MP_MAX_CHAT_LEN = 127;		// characters, excluding NUL
static const uint32_t MP_MAX_PACKET = 1200;		// bytes, envelope included
static const uint32_t MP_INVITE_CODE_LEN = 9;		// "VICE-XXXX", excluding NUL

enum eMpChannel
{
	MP_CHANNEL_RELIABLE = 0,
	MP_CHANNEL_SNAPSHOT = 1,
	MP_NUM_CHANNELS = 2
};

enum eMpRoute
{
	MP_ROUTE_RELAY = 0,		// for the relay itself (session control)
	MP_ROUTE_HOST = 1,		// to the session host
	MP_ROUTE_ALL = 2,		// to every member, sender included
	MP_ROUTE_ALL_BUT_SELF = 3,	// to every member except the sender
	MP_ROUTE_PLAYER = 4		// to one player (envelope player = target)
};

static const uint8_t MP_PLAYER_NONE = 0xFF;
static const uint8_t MP_PLAYER_RELAY = 0xFE;
