#include "InviteCode.h"

#include <string.h>

static const char sAlphabet[] = MP_INVITE_ALPHABET;
static_assert(sizeof(sAlphabet) - 1 == MP_INVITE_ALPHABET_LEN, "alphabet length");
static_assert(5 + MP_INVITE_TAIL_LEN == MP_INVITE_CODE_LEN, "code length");

static uint32_t
Mix32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

static bool
InAlphabet(char c)
{
	return c != '\0' && strchr(sAlphabet, c) != nullptr;
}

void
MpMakeInviteCode(uint32_t random, char out[MP_INVITE_CODE_LEN + 1])
{
	uint32_t x = Mix32(random);
	memcpy(out, "VICE-", 5);
	for(uint32_t i = 0; i < MP_INVITE_TAIL_LEN; i++){
		out[5 + i] = sAlphabet[x % MP_INVITE_ALPHABET_LEN];
		x /= MP_INVITE_ALPHABET_LEN;
	}
	out[MP_INVITE_CODE_LEN] = '\0';
}

bool
MpNormalizeInviteCode(const char *in, char out[MP_INVITE_CODE_LEN + 1])
{
	out[0] = '\0';
	if(in == nullptr)
		return false;

	// "VICE" + 4 chars is the longest valid input once spaces and '-' are gone.
	char buf[8 + 1];
	uint32_t len = 0;
	for(const char *p = in; *p; p++){
		char c = *p;
		if(c == ' ' || c == '-')
			continue;
		if(c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if(len == 8)
			return false;
		buf[len++] = c;
	}
	buf[len] = '\0';

	const char *tail = buf;
	if(len >= 4 && strncmp(buf, "VICE", 4) == 0)
		tail += 4;
	if(strlen(tail) != MP_INVITE_TAIL_LEN)
		return false;
	for(uint32_t i = 0; i < MP_INVITE_TAIL_LEN; i++)
		if(!InAlphabet(tail[i]))
			return false;

	memcpy(out, "VICE-", 5);
	memcpy(out + 5, tail, MP_INVITE_TAIL_LEN);
	out[MP_INVITE_CODE_LEN] = '\0';
	return true;
}
