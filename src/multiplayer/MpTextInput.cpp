#include "common.h"

#ifdef MULTIPLAYER

#include "MpTextInput.h"
#include "MpText.h"
#include "NetGame.h"
#include "Pad.h"

void
CMpTextInput::Begin(char *buffer, int maxLen, bool forceUpper)
{
	m_buffer = buffer;
	m_maxLen = Min(maxLen, (int)sizeof(m_backup) - 1);
	m_forceUpper = forceUpper;
	strncpy(m_backup, buffer, sizeof(m_backup) - 1);
	m_backup[sizeof(m_backup) - 1] = '\0';
	m_active = true;
}

void
CMpTextInput::Cancel(void)
{
	if(m_active && m_buffer)
		strcpy(m_buffer, m_backup);
	m_active = false;
}

static bool
ShiftHeld(void)
{
	return CPad::NewKeyState.LSHIFT || CPad::NewKeyState.RSHIFT || CPad::NewKeyState.SHIFT;
}

CMpTextInput::eResult
CMpTextInput::Update(void)
{
	if(!m_active)
		return CANCELLED;
	CPad *pad = CPad::GetPad(0);

	if(pad->GetEscapeJustDown()){
		Cancel();
		return CANCELLED;
	}
	if(pad->GetReturnJustDown() || pad->GetPadEnterJustDown()){
		m_active = false;
		return COMMITTED;
	}
	int len = (int)strlen(m_buffer);
	if(pad->GetBackspaceJustDown() && len > 0)
		m_buffer[--len] = '\0';

	// Printable keys arrive as their ASCII codes, letters in upper case.
	bool shift = ShiftHeld();
	for(int c = ' '; c < '~' && len < m_maxLen; c++){	// no '~': CFont token marker
		if(!pad->GetCharJustDown(c))
			continue;
		char ch = (char)c;
		if(ch >= 'A' && ch <= 'Z' && !shift && !m_forceUpper)
			ch = (char)(ch - 'A' + 'a');
		else if(shift && ch == '-')
			ch = '_';
		else if(m_forceUpper && ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 'A');
		m_buffer[len++] = ch;
		m_buffer[len] = '\0';
	}
	return EDITING;
}

wchar*
MpTextWithCursor(const char *text, bool editing)
{
	char tmp[MP_TEXT_RING_LEN];
	bool blink = editing && (CNetGame::NetNow() / 400) % 2 == 0;
	snprintf(tmp, sizeof(tmp), "%s%s", text, blink ? "_" : (editing ? " " : ""));
	return MpWideString(tmp);
}

#endif
