#pragma once

#ifdef MULTIPLAYER

// Wide strings for "MP_*" text keys, so multiplayer needs no GXT edits.
// CText::Get asks here first. Returns nil for unknown keys.
wchar *MpGetText(const char *key);

// A static wide copy of an ASCII string, from a small ring of buffers. For
// text that has to outlive the call (CMessages keeps the pointer). The ring
// holds MP_TEXT_RING strings of up to MP_TEXT_RING_LEN-1 characters.
enum { MP_TEXT_RING = 32, MP_TEXT_RING_LEN = 160 };
wchar *MpWideString(const char *ascii);

#endif
