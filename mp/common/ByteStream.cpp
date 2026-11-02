#include "ByteStream.h"

#include <string.h>

uint8_t*
CByteWriter::Reserve(uint32_t n)
{
	if(m_overflowed || n > MP_MAX_PACKET - m_size){
		m_overflowed = true;
		return nullptr;
	}
	uint8_t *p = m_data + m_size;
	m_size += n;
	return p;
}

void
CByteWriter::WriteU8(uint8_t v)
{
	uint8_t *p = Reserve(1);
	if(p)
		p[0] = v;
}

void
CByteWriter::WriteU16(uint16_t v)
{
	uint8_t *p = Reserve(2);
	if(p){
		p[0] = (uint8_t)(v & 0xFF);
		p[1] = (uint8_t)(v >> 8);
	}
}

void
CByteWriter::WriteU32(uint32_t v)
{
	uint8_t *p = Reserve(4);
	if(p){
		p[0] = (uint8_t)(v & 0xFF);
		p[1] = (uint8_t)((v >> 8) & 0xFF);
		p[2] = (uint8_t)((v >> 16) & 0xFF);
		p[3] = (uint8_t)(v >> 24);
	}
}

void
CByteWriter::WriteI16(int16_t v)
{
	WriteU16((uint16_t)v);
}

void
CByteWriter::WriteI32(int32_t v)
{
	WriteU32((uint32_t)v);
}

void
CByteWriter::WriteF32(float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	WriteU32(u);
}

void
CByteWriter::WriteString(const char *s, uint32_t maxLen)
{
	uint32_t len = s ? (uint32_t)strlen(s) : 0;
	if(len > maxLen)
		len = maxLen;
	if(len > 255)
		len = 255;
	uint8_t *p = Reserve(1 + len);
	if(p){
		p[0] = (uint8_t)len;
		if(len)
			memcpy(p + 1, s, len);
	}
}

void
CByteWriter::WriteBytes(const void *data, uint32_t n)
{
	if(n == 0)
		return;
	uint8_t *p = Reserve(n);
	if(p)
		memcpy(p, data, n);
}

const uint8_t*
CByteReader::Take(uint32_t n)
{
	if(!m_ok || n > m_size - m_pos){
		m_ok = false;
		return nullptr;
	}
	const uint8_t *p = m_data + m_pos;
	m_pos += n;
	return p;
}

bool
CByteReader::ReadU8(uint8_t &v)
{
	const uint8_t *p = Take(1);
	if(!p)
		return false;
	v = p[0];
	return true;
}

bool
CByteReader::ReadU16(uint16_t &v)
{
	const uint8_t *p = Take(2);
	if(!p)
		return false;
	v = (uint16_t)(p[0] | (p[1] << 8));
	return true;
}

bool
CByteReader::ReadU32(uint32_t &v)
{
	const uint8_t *p = Take(4);
	if(!p)
		return false;
	v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	return true;
}

bool
CByteReader::ReadI16(int16_t &v)
{
	uint16_t u;
	if(!ReadU16(u))
		return false;
	v = (int16_t)u;
	return true;
}

bool
CByteReader::ReadI32(int32_t &v)
{
	uint32_t u;
	if(!ReadU32(u))
		return false;
	v = (int32_t)u;
	return true;
}

bool
CByteReader::ReadF32(float &v)
{
	uint32_t u;
	if(!ReadU32(u))
		return false;
	memcpy(&v, &u, 4);
	return true;
}

bool
CByteReader::ReadString(char *out, uint32_t outSize)
{
	if(out == nullptr || outSize == 0){
		m_ok = false;
		return false;
	}
	out[0] = '\0';
	uint8_t len;
	if(!ReadU8(len))
		return false;
	if(len > outSize - 1 || len > Remaining()){
		m_ok = false;
		return false;
	}
	const uint8_t *p = Take(len);
	if(len)
		memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

bool
CByteReader::ReadBytes(void *out, uint32_t n)
{
	if(n == 0)
		return m_ok;
	const uint8_t *p = Take(n);
	if(!p)
		return false;
	if(n)
		memcpy(out, p, n);
	return true;
}
