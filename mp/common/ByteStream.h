#pragma once

// Little-endian, bounds-checked byte streams for network messages.
// Writers never grow past MP_MAX_PACKET; readers never read past their buffer.

#include <stdint.h>

#include "MpTypes.h"

class CByteWriter
{
	uint8_t m_data[MP_MAX_PACKET];
	uint32_t m_size;
	bool m_overflowed;

	uint8_t *Reserve(uint32_t n);
public:
	CByteWriter(void) : m_size(0), m_overflowed(false) {}

	void Clear(void) { m_size = 0; m_overflowed = false; }
	void WriteU8(uint8_t v);
	void WriteU16(uint16_t v);
	void WriteU32(uint32_t v);
	void WriteI16(int16_t v);
	void WriteI32(int32_t v);
	void WriteF32(float v);
	// u8 length prefix followed by the characters (no NUL). Truncates at maxLen (and 255).
	void WriteString(const char *s, uint32_t maxLen);
	void WriteBytes(const void *data, uint32_t n);

	const uint8_t *Data(void) const { return m_data; }
	uint32_t Size(void) const { return m_size; }
	// True once any write didn't fit. The failed write is dropped whole.
	bool Overflowed(void) const { return m_overflowed; }
};

class CByteReader
{
	const uint8_t *m_data;
	uint32_t m_size;
	uint32_t m_pos;
	bool m_ok;

	const uint8_t *Take(uint32_t n);
public:
	CByteReader(const uint8_t *data, uint32_t size) : m_data(data), m_size(data ? size : 0), m_pos(0), m_ok(true) {}

	bool ReadU8(uint8_t &v);
	bool ReadU16(uint16_t &v);
	bool ReadU32(uint32_t &v);
	bool ReadI16(int16_t &v);
	bool ReadI32(int32_t &v);
	bool ReadF32(float &v);
	// Always NUL-terminates (an empty string on failure). Fails if the length
	// prefix exceeds the remaining bytes or outSize-1.
	bool ReadString(char *out, uint32_t outSize);
	bool ReadBytes(void *out, uint32_t n);

	uint32_t Remaining(void) const { return m_size - m_pos; }
	// Pointer to the next unread byte.
	const uint8_t *Current(void) const { return m_data + m_pos; }
	// False after any failed read; every later read fails too.
	bool Ok(void) const { return m_ok; }
};
