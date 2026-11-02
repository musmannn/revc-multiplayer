#include "common.h"

#ifdef MULTIPLAYER

#include "MpText.h"

struct SMpTextEntry
{
	const char *key;
	const char *text;
	wchar wide[64];
	bool converted;
};

static SMpTextEntry sTexts[] = {
	{ "MP_MENU", "MULTIPLAYER" },
	{ "MP_HOST", "HOST SESSION" },
	{ "MP_JOIN", "JOIN WITH CODE" },
	{ "MP_NAME", "NAME" },
	{ "MP_RELAY", "RELAY SERVER" },
	{ "MP_RLAY", "RELAY SERVER" },	// menu keys are at most 7 chars
	{ "MP_LEAV", "LEAVE SESSION" },
	{ "MP_STAT", "SESSION" },
	{ "MP_CODE", "CODE" },
	{ "MP_LEAVE", "LEAVE SESSION" },
	{ "MP_CONNECTING", "Connecting..." },
	{ "MP_ENDED_HOST", "Session ended: host left" },
	{ "MP_ENDED_RELAY", "Session ended: relay shut down" },
	{ "MP_LOST", "Connection lost" },
	{ "MP_NOTFOUND", "No session with that code" },
	{ "MP_FULL", "Session is full (8 players)" },
	{ "MP_VERSION", "Version mismatch: update reVC" },
	{ "MP_BADNAME", "Choose a different name" },
	{ "MP_BADRELAY", "Can't reach the relay server" },
	{ "MP_NOSAVE", "Saving is disabled in multiplayer" },
	{ "MP_WATER", "Must be on water" },
};

static void
Widen(wchar *out, const char *in, int outLen)
{
	int i = 0;
	for(; in[i] && i < outLen - 1; i++)
		out[i] = (uint8)in[i];
	out[i] = 0;
}

wchar*
MpGetText(const char *key)
{
	if(key == nil || key[0] != 'M' || key[1] != 'P' || key[2] != '_')
		return nil;
	for(int i = 0; i < (int)ARRAY_SIZE(sTexts); i++){
		SMpTextEntry &e = sTexts[i];
		if(strcmp(e.key, key) != 0)
			continue;
		if(!e.converted){
			Widen(e.wide, e.text, ARRAY_SIZE(e.wide));
			e.converted = true;
		}
		return e.wide;
	}
	return nil;
}

wchar*
MpWideString(const char *ascii)
{
	static wchar sRing[MP_TEXT_RING][MP_TEXT_RING_LEN];
	static int sNext = 0;
	wchar *out = sRing[sNext];
	sNext = (sNext + 1) % MP_TEXT_RING;
	Widen(out, ascii ? ascii : "", MP_TEXT_RING_LEN);
	return out;
}

#endif
