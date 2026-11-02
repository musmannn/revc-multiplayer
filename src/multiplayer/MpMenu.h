#pragma once

#if defined(MULTIPLAYER) && defined(CUSTOM_FRONTEND_OPTIONS)

// Frontend Multiplayer page (rows live in MenuScreensCustom.cpp).
wchar *MpMenuStatusDraw(bool *disabled, bool userHovering);
wchar *MpMenuNameDraw(bool *disabled, bool userHovering);
void MpMenuNamePress(int8 action);
wchar *MpMenuRelayDraw(bool *disabled, bool userHovering);
void MpMenuRelayPress(int8 action);
wchar *MpMenuHostDraw(bool *disabled, bool userHovering);
void MpMenuHostPress(int8 action);
wchar *MpMenuCodeDraw(bool *disabled, bool userHovering);
void MpMenuCodePress(int8 action);
wchar *MpMenuJoinDraw(bool *disabled, bool userHovering);
void MpMenuJoinPress(int8 action);
wchar *MpMenuLeaveDraw(bool *disabled, bool userHovering);
void MpMenuLeavePress(int8 action);
void MpMenuGoBack(void);

// CMenuManager::Process runs this instead of UserInput() while a text field
// is being edited, so typed keys don't navigate the menu.
bool MpMenuIsEditing(void);
void MpMenuUpdateEditing(void);

#endif
