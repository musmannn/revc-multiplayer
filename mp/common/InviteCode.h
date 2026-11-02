#pragma once

// Invite codes look like "VICE-XXXX". The 4 tail characters come from a
// 31-letter alphabet without the lookalikes 0 O 1 I L, so a mistyped code is
// rejected as invalid instead of matching a different session.

#include <stdint.h>

#include "MpTypes.h"

#define MP_INVITE_ALPHABET "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
static const uint32_t MP_INVITE_ALPHABET_LEN = 31;
static const uint32_t MP_INVITE_TAIL_LEN = 4;

// Mixes random and writes "VICE-" + 4 alphabet chars to out.
void MpMakeInviteCode(uint32_t random, char out[MP_INVITE_CODE_LEN + 1]);

// Uppercases, removes spaces and '-', drops a leading "VICE"; the rest must be
// exactly 4 alphabet chars. On success out = "VICE-" + those 4 chars.
// On failure returns false and out is empty.
bool MpNormalizeInviteCode(const char *in, char out[MP_INVITE_CODE_LEN + 1]);
