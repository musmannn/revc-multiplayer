#pragma once

#ifdef MULTIPLAYER

// Keyboard text entry for the Multiplayer menu page and chat. Reads keys from
// CPad::GetPad(0); the caller stops other input handling while it's active.
class CMpTextInput
{
public:
	enum eResult { EDITING, COMMITTED, CANCELLED };
private:
	char *m_buffer;
	int m_maxLen;
	char m_backup[128];
	bool m_active;
	bool m_forceUpper;
public:
	CMpTextInput(void) : m_buffer(nil), m_maxLen(0), m_active(false), m_forceUpper(false) { m_backup[0] = '\0'; }

	// Edits buffer in place (maxLen chars, plus NUL). Esc restores the old value.
	void Begin(char *buffer, int maxLen, bool forceUpper);
	eResult Update(void);
	void Cancel(void);
	bool IsActive(void) const { return m_active; }
	bool IsEditing(const char *buffer) const { return m_active && m_buffer == buffer; }
};

// The buffer text with a blinking cursor while editing, as a static wide string.
wchar *MpTextWithCursor(const char *text, bool editing);

#endif
