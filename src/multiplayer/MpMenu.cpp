#include "common.h"

#if defined(MULTIPLAYER) && defined(CUSTOM_FRONTEND_OPTIONS)

#include "MpMenu.h"
#include "MpText.h"
#include "MpTextInput.h"
#include "NetGame.h"
#include "Frontend.h"
#include "Text.h"

static CMpTextInput sInput;
static char sJoinCode[MP_INVITE_CODE_LEN + 1];

// The last status shows for this long after it changes, except the session line.
static const uint32 STATUS_SHOW_MS = 5000;
static const char *sLastStatusKey;
static uint32 sStatusChangedAt;

bool
MpMenuIsEditing(void)
{
	return sInput.IsActive();
}

void
MpMenuUpdateEditing(void)
{
	if(sInput.Update() == CMpTextInput::COMMITTED)
		CNetGame::LoadSettings();	// sanitizes the name/relay
	if(!sInput.IsActive())
		CNetGame::SaveSettings();
}

void
MpMenuGoBack(void)
{
	sInput.Cancel();
}

static wchar*
Text(const char *key)
{
	return TheText.Get(key);
}

wchar*
MpMenuStatusDraw(bool *disabled, bool userHovering)
{
	(void)disabled;
	(void)userHovering;
	const char *key = CNetGame::LastStatusKey();
	uint32 now = CNetGame::NetNow();
	if(key != sLastStatusKey){
		sLastStatusKey = key;
		sStatusChangedAt = now;
	}
	if(CNetGame::Client().IsInSession()){
		char line[64];
		snprintf(line, sizeof(line), "%s - %d/%d players", CNetGame::InviteCode(), CNetGame::NumPlayers(), CNetGame::MaxPlayers());
		return MpWideString(line);
	}
	if(CNetGame::IsConnected())
		return Text("MP_CONNECTING");
	if(key && now - sStatusChangedAt < STATUS_SHOW_MS)
		return Text(key);
	return MpWideString("-");
}

static void
EditPress(int8 action, char *buffer, int maxLen, bool upper)
{
	if(action != FEOPTION_ACTION_SELECT || CNetGame::IsConnected())
		return;
	sInput.Begin(buffer, maxLen, upper);
}

wchar*
MpMenuNameDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = CNetGame::IsConnected();
	return MpTextWithCursor(CNetGame::ms_playerName, sInput.IsEditing(CNetGame::ms_playerName));
}

void
MpMenuNamePress(int8 action)
{
	EditPress(action, CNetGame::ms_playerName, MP_MAX_NAME_LEN, false);
}

wchar*
MpMenuRelayDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = CNetGame::IsConnected();
	return MpTextWithCursor(CNetGame::ms_relayAddress, sInput.IsEditing(CNetGame::ms_relayAddress));
}

void
MpMenuRelayPress(int8 action)
{
	EditPress(action, CNetGame::ms_relayAddress, sizeof(CNetGame::ms_relayAddress) - 1, false);
}

wchar*
MpMenuHostDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = CNetGame::IsConnected();
	return MpWideString("");
}

void
MpMenuHostPress(int8 action)
{
	if(action == FEOPTION_ACTION_SELECT && !CNetGame::IsConnected())
		CNetGame::HostSession();
}

wchar*
MpMenuCodeDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = CNetGame::IsConnected();
	return MpTextWithCursor(sJoinCode, sInput.IsEditing(sJoinCode));
}

void
MpMenuCodePress(int8 action)
{
	EditPress(action, sJoinCode, MP_INVITE_CODE_LEN, true);
}

wchar*
MpMenuJoinDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = CNetGame::IsConnected() || sJoinCode[0] == '\0';
	return MpWideString("");
}

void
MpMenuJoinPress(int8 action)
{
	if(action == FEOPTION_ACTION_SELECT && !CNetGame::IsConnected() && sJoinCode[0])
		CNetGame::JoinSession(sJoinCode);
}

wchar*
MpMenuLeaveDraw(bool *disabled, bool userHovering)
{
	(void)userHovering;
	*disabled = !CNetGame::IsConnected();
	return MpWideString("");
}

void
MpMenuLeavePress(int8 action)
{
	if(action == FEOPTION_ACTION_SELECT && CNetGame::IsConnected())
		CNetGame::LeaveSession();
}

#endif
