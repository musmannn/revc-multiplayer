#include "MpTypes.h"

// Compile-time checks on the shared constants. This file also gives mpcommon
// a translation unit of its own.

static_assert(MP_MAX_PLAYERS <= 0xFE, "player ids must fit below MP_PLAYER_RELAY");
static_assert(MP_INVITE_CODE_LEN == 9, "invite codes are VICE-XXXX");
static_assert(MP_MAX_NAME_LEN < 255 && MP_MAX_CHAT_LEN < 255, "strings use a u8 length prefix");
