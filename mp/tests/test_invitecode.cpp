#include "doctest.h"

#include <string.h>
#include <set>
#include <string>

#include "InviteCode.h"

static const char *ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

TEST_CASE("make code format")
{
	char c[MP_INVITE_CODE_LEN + 1];
	MpMakeInviteCode(0, c);
	CHECK(strlen(c) == 9);
	CHECK(strncmp(c, "VICE-", 5) == 0);
	for(int i = 5; i < 9; i++)
		CHECK(strchr(ALPHABET, c[i]) != nullptr);

	std::set<std::string> seen;
	for(uint32_t seed = 0; seed < 1000; seed++){
		MpMakeInviteCode(seed, c);
		REQUIRE(strlen(c) == 9);
		for(int i = 5; i < 9; i++)
			REQUIRE(strchr(ALPHABET, c[i]) != nullptr);
		// A generated code always normalizes to itself.
		char n[MP_INVITE_CODE_LEN + 1];
		REQUIRE(MpNormalizeInviteCode(c, n));
		REQUIRE(strcmp(n, c) == 0);
		seen.insert(c);
	}
	CHECK(seen.size() >= 990);
}

TEST_CASE("normalize accepts sloppy input")
{
	const char *inputs[] = { "vice-ab7k", " AB7K ", "ab7k", "VICEAB7K", "vice - ab7k", "VICE-AB7K" };
	for(const char *in : inputs){
		char out[MP_INVITE_CODE_LEN + 1];
		CAPTURE(in);
		CHECK(MpNormalizeInviteCode(in, out));
		CHECK(strcmp(out, "VICE-AB7K") == 0);
	}
}

TEST_CASE("normalize rejects lookalikes and junk")
{
	const char *inputs[] = { "VICE-AB0K", "VICE-ABIK", "VICE-AB7", "VICE-AB7KK", "", "VICE-ABOK", "VICE-AB1K",
		"VICE-ABLK", "VICE", "VICE-", "AB7K!", "VICE-AB\x01K" };
	for(const char *in : inputs){
		char out[MP_INVITE_CODE_LEN + 1];
		CAPTURE(in);
		CHECK(!MpNormalizeInviteCode(in, out));
	}
	char out[MP_INVITE_CODE_LEN + 1];
	CHECK(!MpNormalizeInviteCode(nullptr, out));
}
