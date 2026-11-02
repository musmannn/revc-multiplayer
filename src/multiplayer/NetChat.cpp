#include "common.h"

#ifdef MULTIPLAYER

#include "NetChat.h"
#include "NetGame.h"
#include "MpText.h"
#include "MpTextInput.h"
#include "RemotePlayer.h"
#include "RouteRecorder.h"
#include "Clock.h"
#include "Font.h"
#include "Frontend.h"
#include "Pad.h"
#include "Sprite2d.h"
#include "Weather.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"

enum { MAX_LINES = 8, LINE_LEN = 160 };
static const uint32 LINE_SHOW_MS = 10000;
static const uint32 WORLD_INTERVAL_MS = 10000;

struct SChatLine
{
	char name[MP_MAX_NAME_LEN + 1];	// empty for notices
	char text[LINE_LEN];
	uint8 colourId;
	uint32 time;			// NetNow when added
};

static SChatLine sLines[MAX_LINES];
static int sNumLines;
static CMpTextInput sInput;
static char sBuffer[MP_MAX_CHAT_LEN + 1];
static bool sTyping;
static bool sReleaseNextFrame;	// keep controls off for the frame Enter was pressed
static bool sSwallowEsc;	// an Esc that belonged to chat is still held
static uint32 sLastWorldSentMs;
static int16 sForcedWeather = -1;

static void
PushLine(const char *name, const char *text, uint8 colourId)
{
	if(sNumLines == MAX_LINES){
		memmove(&sLines[0], &sLines[1], sizeof(SChatLine) * (MAX_LINES - 1));
		sNumLines--;
	}
	SChatLine &l = sLines[sNumLines++];
	strncpy(l.name, name ? name : "", MP_MAX_NAME_LEN);
	l.name[MP_MAX_NAME_LEN] = '\0';
	strncpy(l.text, text, LINE_LEN - 1);
	l.text[LINE_LEN - 1] = '\0';
	l.colourId = colourId;
	l.time = CNetGame::NetNow();
}

void
CNetChat::AddLine(const char *text, uint8 colourId)
{
	PushLine(nil, text, colourId);
}

void
CNetChat::OnChat(uint8 from, const char *text)
{
	const char *name = CNetGame::PlayerName(from);
	PushLine(name ? name : "?", text, from);
}

bool
CNetChat::IsTyping(void)
{
	return sTyping;
}

void
CNetChat::Reset(void)
{
	if(sTyping)
		CPad::GetPad(0)->SetEnablePlayerControls(PLAYERCONTROL_MP_CHAT);
	sTyping = false;
	sReleaseNextFrame = false;
	sInput.Cancel();
	sNumLines = 0;
}

// Makes a held Esc look "not just pressed" to everything after us.
static void
SwallowEsc(void)
{
	CPad::OldKeyState.ESC = CPad::NewKeyState.ESC;
	sSwallowEsc = CPad::NewKeyState.ESC != 0;
}

void
CNetChat::Update(void)
{
	CPad *pad = CPad::GetPad(0);
	if(sSwallowEsc && !sTyping)
		SwallowEsc();
	if(sReleaseNextFrame){
		sReleaseNextFrame = false;
		pad->SetEnablePlayerControls(PLAYERCONTROL_MP_CHAT);
	}
	if(!CNetGame::IsActive()){
		if(sTyping)
			Reset();
		return;
	}

	if(!sTyping){
		if(FrontEndMenuManager.m_bMenuActive || pad->ArePlayerControlsDisabled())
			return;
		if(!pad->GetCharJustDown('T'))
			return;
		sBuffer[0] = '\0';
		sInput.Begin(sBuffer, MP_MAX_CHAT_LEN, false);
		sTyping = true;
		pad->SetDisablePlayerControls(PLAYERCONTROL_MP_CHAT);
		return;	// the T itself isn't typed
	}

	CPlayerPed *player = FindPlayerPed();
	if(player == nil || player->DyingOrDead()){
		// Respawning re-enables every control; don't keep typing into it.
		sInput.Cancel();
		sTyping = false;
		pad->SetEnablePlayerControls(PLAYERCONTROL_MP_CHAT);
		return;
	}
	CMpTextInput::eResult result = sInput.Update();
	// Esc closes the chat, never the pause menu behind it.
	SwallowEsc();
	if(result == CMpTextInput::EDITING){
		pad->SetDisablePlayerControls(PLAYERCONTROL_MP_CHAT);
		return;
	}
	// Lines starting with '/' are local commands (route recorder), never sent.
	if(result == CMpTextInput::COMMITTED && sBuffer[0] && !CRouteRecorder::OnCommand(sBuffer))
		CNetGame::Client().SendChat(sBuffer);	// shows up when the echo comes back
	sTyping = false;
	sReleaseNextFrame = true;
}

void
CNetChat::Render(void)
{
	uint32 now = CNetGame::NetNow();
	float x = SCREEN_SCALE_X(24.0f);
	float y = SCREEN_SCALE_Y(120.0f);
	float lineH = SCREEN_SCALE_Y(18.0f);

	CFont::SetPropOn();
	CFont::SetBackgroundOff();
	CFont::SetScale(SCREEN_SCALE_X(0.45f), SCREEN_SCALE_Y(0.8f));
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));

	for(int i = 0; i < sNumLines; i++){
		const SChatLine &l = sLines[i];
		if(!sTyping && now - l.time > LINE_SHOW_MS)
			continue;
		float cx = x;
		if(l.name[0]){
			char name[MP_MAX_NAME_LEN + 3];
			snprintf(name, sizeof(name), "%s: ", l.name);
			wchar *wname = MpWideString(name);
			CFont::SetColor(CRemotePlayer::ColourForId(l.colourId));
			CFont::PrintString(cx, y, wname);
			cx += CFont::GetStringWidth(wname, true);
			CFont::SetColor(CRGBA(255, 255, 255, 255));
		}else
			CFont::SetColor(CRGBA(255, 220, 120, 255));
		CFont::PrintString(cx, y, MpWideString(l.text));
		y += lineH;
	}

	if(sTyping){
		CSprite2d::DrawRect(CRect(x - SCREEN_SCALE_X(6.0f), y - SCREEN_SCALE_Y(2.0f),
			x + SCREEN_SCALE_X(420.0f), y + lineH), CRGBA(0, 0, 0, 150));
		char prompt[MP_MAX_CHAT_LEN + 8];
		snprintf(prompt, sizeof(prompt), "Say: %s", sBuffer);
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		CFont::PrintString(x, y, MpTextWithCursor(prompt, true));
	}
	CFont::SetDropShadowPosition(0);
}

void
CNetChat::SendWorld(void)
{
	if(!CNetGame::IsHost())
		return;
	SWorldState msg;
	msg.hour = CClock::GetHours();
	msg.minute = CClock::GetMinutes();
	msg.weather = (uint8)Max(0, (int)CWeather::NewWeatherType);
	CByteWriter w;
	msg.Write(w);
	CNetGame::Client().SendGame(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
	sLastWorldSentMs = CNetGame::NetNow();
}

void
CNetChat::UpdateWorld(uint32 netNow)
{
	if(CNetGame::IsHost() && netNow - sLastWorldSentMs >= WORLD_INTERVAL_MS)
		SendWorld();
}

void
CNetChat::OnWorld(uint8 from, const SWorldState &msg)
{
	if(from != 0 || CNetGame::IsHost() || msg.hour > 23 || msg.minute > 59)
		return;
	int here = CClock::GetHours() * 60 + CClock::GetMinutes();
	int there = msg.hour * 60 + msg.minute;
	int diff = Abs(here - there);
	diff = Min(diff, 24 * 60 - diff);	// across midnight
	if(diff > 2)
		CClock::SetGameClock(msg.hour, msg.minute);
	if(msg.weather < WEATHER_TOTAL && (int16)msg.weather != sForcedWeather){
		CWeather::ForceWeatherNow(msg.weather);
		sForcedWeather = msg.weather;
	}
}

void
CNetChat::ReleaseWorld(void)
{
	if(sForcedWeather >= 0)
		CWeather::ReleaseWeather();
	sForcedWeather = -1;
	sLastWorldSentMs = 0;
}

#endif
