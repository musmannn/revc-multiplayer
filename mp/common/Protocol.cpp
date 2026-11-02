#include "Protocol.h"

#include <string.h>

void
SEnvelope::Write(CByteWriter &w) const
{
	w.WriteU8(route);
	w.WriteU8(player);
}

bool
SEnvelope::Read(CByteReader &r)
{
	return r.ReadU8(route) && r.ReadU8(player);
}

void
SMsgCreateSession::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_CREATE_SESSION);
	w.WriteU16(version);
	w.WriteString(name, MP_MAX_NAME_LEN);
	w.WriteU8(flags);
}

bool
SMsgCreateSession::Read(CByteReader &r)
{
	return r.ReadU16(version) && r.ReadString(name, sizeof(name)) && r.ReadU8(flags);
}

void
SMsgSessionCreated::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_SESSION_CREATED);
	w.WriteString(code, MP_INVITE_CODE_LEN);
	w.WriteU8(playerId);
}

bool
SMsgSessionCreated::Read(CByteReader &r)
{
	return r.ReadString(code, sizeof(code)) && r.ReadU8(playerId);
}

void
SMsgJoinSession::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_JOIN_SESSION);
	w.WriteU16(version);
	w.WriteString(code, MP_INVITE_CODE_LEN);
	w.WriteString(name, MP_MAX_NAME_LEN);
}

bool
SMsgJoinSession::Read(CByteReader &r)
{
	return r.ReadU16(version) && r.ReadString(code, sizeof(code)) && r.ReadString(name, sizeof(name));
}

void
SMsgJoinResult::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_JOIN_RESULT);
	w.WriteU8(result);
	w.WriteU8(playerId);
	w.WriteU8(flags);
}

bool
SMsgJoinResult::Read(CByteReader &r)
{
	return r.ReadU8(result) && r.ReadU8(playerId) && r.ReadU8(flags);
}

void
SMsgPlayerJoined::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_PLAYER_JOINED);
	w.WriteU8(playerId);
	w.WriteString(name, MP_MAX_NAME_LEN);
}

bool
SMsgPlayerJoined::Read(CByteReader &r)
{
	return r.ReadU8(playerId) && r.ReadString(name, sizeof(name));
}

void
SMsgPlayerLeft::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_PLAYER_LEFT);
	w.WriteU8(playerId);
	w.WriteU8(reason);
}

bool
SMsgPlayerLeft::Read(CByteReader &r)
{
	return r.ReadU8(playerId) && r.ReadU8(reason);
}

void
SMsgSessionEnded::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_SESSION_ENDED);
	w.WriteU8(reason);
}

bool
SMsgSessionEnded::Read(CByteReader &r)
{
	return r.ReadU8(reason);
}

void
SMsgChat::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_CHAT);
	w.WriteString(text, MP_MAX_CHAT_LEN);
}

bool
SMsgChat::Read(CByteReader &r)
{
	return r.ReadString(text, sizeof(text));
}

void
SMsgPing::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_PING);
	w.WriteU32(clientSendMs);
}

bool
SMsgPing::Read(CByteReader &r)
{
	return r.ReadU32(clientSendMs);
}

void
SMsgPong::Write(CByteWriter &w) const
{
	w.WriteU8(MSG_PONG);
	w.WriteU32(clientSendMs);
	w.WriteU32(hostMs);
}

bool
SMsgPong::Read(CByteReader &r)
{
	return r.ReadU32(clientSendMs) && r.ReadU32(hostMs);
}

bool
MpSanitizeName(const char *in, char out[MP_MAX_NAME_LEN + 1])
{
	out[0] = '\0';
	if(in == nullptr)
		return false;

	// Keep printable ASCII only, skipping leading spaces.
	uint32_t len = 0;
	for(const char *p = in; *p && len < MP_MAX_NAME_LEN; p++){
		uint8_t c = (uint8_t)*p;
		// '~' starts a CFont token in the game, so it's never part of a name.
		if(c < 0x20 || c > 0x7E || c == '~')
			continue;
		if(c == ' ' && len == 0)
			continue;
		out[len++] = (char)c;
	}
	// Trim trailing spaces (from the input or left by truncation).
	while(len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = '\0';
	return len > 0;
}
