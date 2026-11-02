#include "doctest.h"

#include <string.h>

#include "ByteStream.h"

TEST_CASE("bytestream round trip")
{
	CByteWriter w;
	w.WriteU8(0xAB);
	w.WriteU16(0xBEEF);
	w.WriteU32(0xDEADBEEF);
	w.WriteI32(-5);
	w.WriteF32(1.5f);
	w.WriteString("hello", 32);
	CHECK(!w.Overflowed());

	CByteReader r(w.Data(), w.Size());
	uint8_t u8 = 0;
	uint16_t u16 = 0;
	uint32_t u32 = 0;
	int32_t i32 = 0;
	float f32 = 0.0f;
	char str[32];
	CHECK(r.ReadU8(u8));
	CHECK(r.ReadU16(u16));
	CHECK(r.ReadU32(u32));
	CHECK(r.ReadI32(i32));
	CHECK(r.ReadF32(f32));
	CHECK(r.ReadString(str, sizeof(str)));
	CHECK(u8 == 0xAB);
	CHECK(u16 == 0xBEEF);
	CHECK(u32 == 0xDEADBEEF);
	CHECK(i32 == -5);
	CHECK(f32 == 1.5f);
	CHECK(strcmp(str, "hello") == 0);
	CHECK(r.Remaining() == 0);
	CHECK(r.Ok());
}

TEST_CASE("bytestream i16 and bytes")
{
	CByteWriter w;
	const uint8_t raw[3] = { 1, 2, 3 };
	w.WriteI16(-1234);
	w.WriteBytes(raw, 3);
	CByteReader r(w.Data(), w.Size());
	int16_t i16 = 0;
	uint8_t back[3] = { 0, 0, 0 };
	CHECK(r.ReadI16(i16));
	CHECK(r.ReadBytes(back, 3));
	CHECK(i16 == -1234);
	CHECK(memcmp(raw, back, 3) == 0);
	CHECK(r.Remaining() == 0);
}

TEST_CASE("bytestream little endian")
{
	CByteWriter w;
	w.WriteU32(0x01020304);
	REQUIRE(w.Size() == 4);
	CHECK(w.Data()[0] == 0x04);
	CHECK(w.Data()[1] == 0x03);
	CHECK(w.Data()[2] == 0x02);
	CHECK(w.Data()[3] == 0x01);
}

TEST_CASE("reader truncated fails safely")
{
	const uint8_t buf[3] = { 1, 2, 3 };
	CByteReader r(buf, 3);
	uint32_t u32 = 0;
	uint8_t u8 = 0;
	CHECK(!r.ReadU32(u32));
	CHECK(!r.Ok());
	CHECK(!r.ReadU8(u8));
	CHECK(!r.Ok());
}

TEST_CASE("string length prefix beyond buffer")
{
	const uint8_t buf[3] = { 10, 'a', 'b' };
	CByteReader r(buf, 3);
	char out[32];
	strcpy(out, "junk");
	CHECK(!r.ReadString(out, sizeof(out)));
	CHECK(strcmp(out, "") == 0);
	CHECK(!r.Ok());
}

TEST_CASE("string longer than output fails")
{
	CByteWriter w;
	w.WriteString("abcdefgh", 32);
	CByteReader r(w.Data(), w.Size());
	char out[4];
	CHECK(!r.ReadString(out, sizeof(out)));
	CHECK(strcmp(out, "") == 0);
}

TEST_CASE("writer truncates string at maxLen")
{
	CByteWriter w;
	w.WriteString("abcdefgh", 3);
	CByteReader r(w.Data(), w.Size());
	char out[16];
	CHECK(r.ReadString(out, sizeof(out)));
	CHECK(strcmp(out, "abc") == 0);
	CHECK(r.Remaining() == 0);
}

TEST_CASE("writer overflow flagged")
{
	CByteWriter w;
	for(int i = 0; i < 1300; i++)
		w.WriteU8((uint8_t)i);
	CHECK(w.Overflowed());
	CHECK(w.Size() <= MP_MAX_PACKET);
}
