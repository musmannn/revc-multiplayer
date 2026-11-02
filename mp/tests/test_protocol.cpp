#include "doctest.h"

#include <string.h>

#include "Protocol.h"

// Writes msg, reads the type byte and the body back into out.
template<typename T>
static bool
RoundTrip(const T &msg, uint8_t expectedType, T &out)
{
	CByteWriter w;
	msg.Write(w);
	if(w.Overflowed())
		return false;
	CByteReader r(w.Data(), w.Size());
	uint8_t type = 0;
	if(!r.ReadU8(type) || type != expectedType)
		return false;
	return out.Read(r) && r.Remaining() == 0;
}

TEST_CASE("protocol round trip")
{
	SUBCASE("envelope")
	{
		SEnvelope in, out;
		in.route = MP_ROUTE_PLAYER;
		in.player = 5;
		CByteWriter w;
		in.Write(w);
		CHECK(w.Size() == 2);
		CByteReader r(w.Data(), w.Size());
		CHECK(out.Read(r));
		CHECK(out.route == MP_ROUTE_PLAYER);
		CHECK(out.player == 5);
	}
	SUBCASE("create session")
	{
		SMsgCreateSession in, out;
		in.version = MP_PROTOCOL_VERSION;
		strcpy(in.name, "Tommy");
		in.flags = MP_SESSION_DEDICATED;
		CHECK(RoundTrip(in, MSG_CREATE_SESSION, out));
		CHECK(out.version == MP_PROTOCOL_VERSION);
		CHECK(strcmp(out.name, "Tommy") == 0);
		CHECK(out.flags == MP_SESSION_DEDICATED);
	}
	SUBCASE("session created")
	{
		SMsgSessionCreated in, out;
		strcpy(in.code, "VICE-AB7K");
		in.playerId = 0;
		CHECK(RoundTrip(in, MSG_SESSION_CREATED, out));
		CHECK(strcmp(out.code, "VICE-AB7K") == 0);
		CHECK(out.playerId == 0);
	}
	SUBCASE("join session")
	{
		SMsgJoinSession in, out;
		in.version = 3;
		strcpy(in.code, "VICE-AB7K");
		strcpy(in.name, "Lance");
		CHECK(RoundTrip(in, MSG_JOIN_SESSION, out));
		CHECK(out.version == 3);
		CHECK(strcmp(out.code, "VICE-AB7K") == 0);
		CHECK(strcmp(out.name, "Lance") == 0);
	}
	SUBCASE("join result")
	{
		SMsgJoinResult in, out;
		in.result = JOIN_FULL;
		in.playerId = MP_PLAYER_NONE;
		CHECK(RoundTrip(in, MSG_JOIN_RESULT, out));
		CHECK(out.result == JOIN_FULL);
		CHECK(out.playerId == MP_PLAYER_NONE);
		CHECK(out.flags == 0);
		in.result = JOIN_OK;
		in.playerId = 2;
		in.flags = MP_SESSION_DEDICATED;
		CHECK(RoundTrip(in, MSG_JOIN_RESULT, out));
		CHECK(out.flags == MP_SESSION_DEDICATED);
	}
	SUBCASE("player joined")
	{
		SMsgPlayerJoined in, out;
		in.playerId = 3;
		strcpy(in.name, "Lance (2)");
		CHECK(RoundTrip(in, MSG_PLAYER_JOINED, out));
		CHECK(out.playerId == 3);
		CHECK(strcmp(out.name, "Lance (2)") == 0);
	}
	SUBCASE("player left")
	{
		SMsgPlayerLeft in, out;
		in.playerId = 7;
		in.reason = LEAVE_LOST;
		CHECK(RoundTrip(in, MSG_PLAYER_LEFT, out));
		CHECK(out.playerId == 7);
		CHECK(out.reason == LEAVE_LOST);
	}
	SUBCASE("session ended")
	{
		SMsgSessionEnded in, out;
		in.reason = END_RELAY_SHUTDOWN;
		CHECK(RoundTrip(in, MSG_SESSION_ENDED, out));
		CHECK(out.reason == END_RELAY_SHUTDOWN);
	}
	SUBCASE("chat")
	{
		SMsgChat in, out;
		strcpy(in.text, "meet at the malibu");
		CHECK(RoundTrip(in, MSG_CHAT, out));
		CHECK(strcmp(out.text, "meet at the malibu") == 0);
	}
	SUBCASE("chat at max length")
	{
		SMsgChat in, out;
		memset(in.text, 'x', MP_MAX_CHAT_LEN);
		in.text[MP_MAX_CHAT_LEN] = '\0';
		CHECK(RoundTrip(in, MSG_CHAT, out));
		CHECK(strlen(out.text) == MP_MAX_CHAT_LEN);
	}
	SUBCASE("ping")
	{
		SMsgPing in, out;
		in.clientSendMs = 123456789;
		CHECK(RoundTrip(in, MSG_PING, out));
		CHECK(out.clientSendMs == 123456789);
	}
	SUBCASE("pong")
	{
		SMsgPong in, out;
		in.clientSendMs = 42;
		in.hostMs = 0xFFFFFFF0;
		CHECK(RoundTrip(in, MSG_PONG, out));
		CHECK(out.clientSendMs == 42);
		CHECK(out.hostMs == 0xFFFFFFF0);
	}
}

TEST_CASE("protocol rejects truncated body")
{
	SMsgJoinSession in, out;
	in.version = MP_PROTOCOL_VERSION;
	strcpy(in.code, "VICE-AB7K");
	strcpy(in.name, "Lance");
	CByteWriter w;
	in.Write(w);
	REQUIRE(w.Size() > 1);
	CByteReader r(w.Data(), w.Size() - 1);
	uint8_t type = 0;
	CHECK(r.ReadU8(type));
	CHECK(type == MSG_JOIN_SESSION);
	CHECK(!out.Read(r));
}

TEST_CASE("protocol rejects empty body and overlong strings")
{
	SUBCASE("header with no body")
	{
		const uint8_t buf[1] = { MSG_CREATE_SESSION };
		CByteReader r(buf, 1);
		uint8_t type = 0;
		SMsgCreateSession out;
		CHECK(r.ReadU8(type));
		CHECK(!out.Read(r));
	}
	SUBCASE("name length prefix above MP_MAX_NAME_LEN")
	{
		CByteWriter w;
		w.WriteU8(MSG_CREATE_SESSION);
		w.WriteU16(MP_PROTOCOL_VERSION);
		w.WriteString("0123456789012345678901234567890123456789", 64);
		CByteReader r(w.Data(), w.Size());
		uint8_t type = 0;
		SMsgCreateSession out;
		CHECK(r.ReadU8(type));
		CHECK(!out.Read(r));
	}
}

TEST_CASE("sanitize name")
{
	char out[MP_MAX_NAME_LEN + 1];
	CHECK(MpSanitizeName("  Tommy ", out));
	CHECK(strcmp(out, "Tommy") == 0);
	CHECK(!MpSanitizeName("\x01\x02", out));
	CHECK(!MpSanitizeName("", out));
	CHECK(!MpSanitizeName("    ", out));
	CHECK(!MpSanitizeName(nullptr, out));
	CHECK(MpSanitizeName("0123456789012345678901234567890123456789", out));
	CHECK(strlen(out) == MP_MAX_NAME_LEN);
	CHECK(strcmp(out, "01234567890123456789012") == 0);
	CHECK(MpSanitizeName("Ken\tRosenberg\x7F", out));
	CHECK(strcmp(out, "KenRosenberg") == 0);
	CHECK(MpSanitizeName("~r~Lance~", out));
	CHECK(strcmp(out, "rLance") == 0);
}

// Every proper prefix of a message body must fail to Read.
template<typename T>
static void
CheckAllPrefixesFail(const T &msg)
{
	CByteWriter w;
	msg.Write(w);
	REQUIRE(!w.Overflowed());
	for(uint32_t len = 1; len < w.Size(); len++){
		CByteReader r(w.Data(), len);
		uint8_t type = 0;
		REQUIRE(r.ReadU8(type));
		T out;
		CAPTURE(len);
		CHECK(!out.Read(r));
	}
}

TEST_CASE("protocol rejects every truncation")
{
	SMsgCreateSession create;
	strcpy(create.name, "Tommy");
	CheckAllPrefixesFail(create);
	SMsgSessionCreated created;
	strcpy(created.code, "VICE-AB7K");
	CheckAllPrefixesFail(created);
	SMsgJoinSession join;
	strcpy(join.code, "VICE-AB7K");
	strcpy(join.name, "Lance");
	CheckAllPrefixesFail(join);
	SMsgJoinResult result;
	CheckAllPrefixesFail(result);
	SMsgPlayerJoined joined;
	strcpy(joined.name, "Lance");
	CheckAllPrefixesFail(joined);
	SMsgPlayerLeft left;
	CheckAllPrefixesFail(left);
	SMsgSessionEnded ended;
	CheckAllPrefixesFail(ended);
	SMsgChat chat;
	strcpy(chat.text, "hi");
	CheckAllPrefixesFail(chat);
	SMsgPing ping;
	CheckAllPrefixesFail(ping);
	SMsgPong pong;
	CheckAllPrefixesFail(pong);
}
