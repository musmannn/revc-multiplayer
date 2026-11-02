#include "doctest.h"

#include <math.h>

#include "PoseBuffer.h"

static SPose
MakePose(uint32_t time, float x, float vx = 0.0f)
{
	SPose p;
	p.time = time;
	p.pos[0] = x; p.pos[1] = 0.0f; p.pos[2] = 0.0f;
	p.quat[0] = 0.0f; p.quat[1] = 0.0f; p.quat[2] = 0.0f; p.quat[3] = 1.0f;
	p.vel[0] = vx; p.vel[1] = 0.0f; p.vel[2] = 0.0f;
	return p;
}

TEST_CASE("pose empty")
{
	CPoseBuffer b;
	SPose out;
	CHECK(!b.Sample(0, out));
}

TEST_CASE("pose interpolate")
{
	CPoseBuffer b;
	b.Push(MakePose(0, 0.0f));
	b.Push(MakePose(100, 10.0f));
	SPose out;
	REQUIRE(b.Sample(50, out));
	CHECK(fabsf(out.pos[0] - 5.0f) < 1e-4f);
}

TEST_CASE("pose interpolate picks the right segment")
{
	CPoseBuffer b;
	b.Push(MakePose(0, 0.0f));
	b.Push(MakePose(100, 10.0f));
	b.Push(MakePose(200, 30.0f));
	SPose out;
	REQUIRE(b.Sample(150, out));
	CHECK(fabsf(out.pos[0] - 20.0f) < 1e-4f);
	REQUIRE(b.Sample(100, out));
	CHECK(fabsf(out.pos[0] - 10.0f) < 1e-4f);
}

TEST_CASE("pose extrapolate capped")
{
	CPoseBuffer b;
	b.Push(MakePose(0, 0.0f, 0.1f));
	SPose out;
	REQUIRE(b.Sample(100, out));
	CHECK(fabsf(out.pos[0] - 10.0f) < 1e-4f);
	REQUIRE(b.Sample(1000, out));
	CHECK(fabsf(out.pos[0] - 25.0f) < 1e-4f);
}

TEST_CASE("pose before oldest returns oldest")
{
	CPoseBuffer b;
	b.Push(MakePose(1000, 3.0f, 1.0f));
	b.Push(MakePose(1100, 4.0f, 1.0f));
	SPose out;
	REQUIRE(b.Sample(500, out));
	CHECK(out.time == 1000);
	CHECK(fabsf(out.pos[0] - 3.0f) < 1e-4f);
}

TEST_CASE("pose ignores stale")
{
	CPoseBuffer b;
	b.Push(MakePose(100, 10.0f));
	b.Push(MakePose(50, 99.0f));	// older: ignored
	b.Push(MakePose(100, 77.0f));	// duplicate time: ignored
	SPose out;
	REQUIRE(b.Sample(100, out));
	CHECK(fabsf(out.pos[0] - 10.0f) < 1e-4f);
	REQUIRE(b.Sample(50, out));	// before the oldest (100)
	CHECK(fabsf(out.pos[0] - 10.0f) < 1e-4f);
	b.Push(MakePose(200, 20.0f, 0.1f));
	REQUIRE(b.Sample(250, out));	// extrapolates from the newest (t=200)
	CHECK(fabsf(out.pos[0] - 25.0f) < 1e-4f);
}

TEST_CASE("pose handles time wraparound")
{
	CPoseBuffer b;
	b.Push(MakePose(0xFFFFFFF0u, 0.0f));
	b.Push(MakePose(0x10u, 10.0f));
	b.Push(MakePose(0xFFFFFFF8u, 99.0f));	// older than 0x10 across the wrap: ignored
	SPose out;
	REQUIRE(b.Sample(0, out));
	CHECK(fabsf(out.pos[0] - 5.0f) < 1e-4f);
	REQUIRE(b.Sample(0x10u, out));
	CHECK(fabsf(out.pos[0] - 10.0f) < 1e-4f);
}

TEST_CASE("pose ring keeps the newest 16")
{
	CPoseBuffer b;
	for(uint32_t i = 0; i < 20; i++)
		b.Push(MakePose(i * 10, (float)i));
	SPose out;
	// Samples 0..3 fell out; the oldest kept is t=40.
	REQUIRE(b.Sample(0, out));
	CHECK(out.time == 40);
	REQUIRE(b.Sample(185, out));
	CHECK(fabsf(out.pos[0] - 18.5f) < 1e-4f);
	b.Clear();
	CHECK(!b.Sample(0, out));
}

TEST_CASE("pose quat shortest path")
{
	CPoseBuffer b;
	SPose p0 = MakePose(0, 0.0f);
	SPose p1 = MakePose(100, 0.0f);
	p1.quat[3] = -1.0f;	// same rotation as p0
	b.Push(p0);
	b.Push(p1);
	SPose out;
	REQUIRE(b.Sample(50, out));
	float len = sqrtf(out.quat[0]*out.quat[0] + out.quat[1]*out.quat[1] + out.quat[2]*out.quat[2] + out.quat[3]*out.quat[3]);
	CHECK(fabsf(len - 1.0f) < 1e-4f);
	CHECK(fabsf(out.quat[0]) < 1e-4f);
	CHECK(fabsf(out.quat[1]) < 1e-4f);
	CHECK(fabsf(out.quat[2]) < 1e-4f);
	CHECK(fabsf(fabsf(out.quat[3]) - 1.0f) < 1e-4f);
}

TEST_CASE("pose quat nlerp is normalized")
{
	CPoseBuffer b;
	SPose p0 = MakePose(0, 0.0f);
	SPose p1 = MakePose(100, 0.0f);
	// 90 degrees about z
	p1.quat[2] = sqrtf(0.5f);
	p1.quat[3] = sqrtf(0.5f);
	b.Push(p0);
	b.Push(p1);
	SPose out;
	REQUIRE(b.Sample(50, out));
	float len = sqrtf(out.quat[0]*out.quat[0] + out.quat[1]*out.quat[1] + out.quat[2]*out.quat[2] + out.quat[3]*out.quat[3]);
	CHECK(fabsf(len - 1.0f) < 1e-4f);
	// halfway is 45 degrees about z: (0, 0, sin 22.5, cos 22.5)
	CHECK(fabsf(out.quat[2] - 0.3826834f) < 1e-4f);
	CHECK(fabsf(out.quat[3] - 0.9238795f) < 1e-4f);
}
