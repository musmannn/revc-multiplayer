#include "doctest.h"

#include "SessionClock.h"

TEST_CASE("clock host identity")
{
	CSessionClock c;
	c.SetIsHost();
	CHECK(c.ToSession(1234) == 1234);
	CHECK(c.IsSynced());
	CHECK(c.RttMs() == 0);
}

TEST_CASE("clock offset")
{
	CSessionClock c;
	c.AddSample(1000, 50100, 1100);
	c.AddSample(2000, 51080, 2060);
	c.AddSample(3000, 52200, 3300);
	CHECK(c.IsSynced());
	// The best sample has RTT 60: offset = 51080 + 30 - 2060 = 49050.
	CHECK(c.ToSession(4000) == 53050);
	CHECK(c.RttMs() == 300);	// most recent sample
}

TEST_CASE("clock unsynced")
{
	CSessionClock c;
	CHECK(!c.IsSynced());
	c.AddSample(1000, 50100, 1100);
	c.AddSample(2000, 51080, 2060);
	CHECK(!c.IsSynced());
	c.Reset();
	CHECK(!c.IsSynced());
	CHECK(c.ToSession(77) == 77);
}

TEST_CASE("clock keeps the last 16 samples")
{
	CSessionClock c;
	// A very good sample, then 16 worse ones push it out of the window.
	c.AddSample(0, 10000, 2);
	for(uint32_t i = 1; i <= 16; i++)
		c.AddSample(i * 1000, 20000 + i * 1000, i * 1000 + 100);
	// Every kept sample has RTT 100 and offset 20000 + 50 - 100 = 19950.
	CHECK(c.ToSession(100000) == 119950);
}

TEST_CASE("clock handles local time wraparound")
{
	CSessionClock c;
	const uint32_t base = 0xFFFFFF00u;
	c.AddSample(base, 5000, base + 40);
	c.AddSample(base + 100, 5100, base + 140);
	c.AddSample(base + 200, 5200, base + 240);
	CHECK(c.RttMs() == 40);
	// offset = 5000 + 20 - (base + 40)
	CHECK(c.ToSession(base + 1040) == 6020);
}

TEST_CASE("clock ignores bogus samples")
{
	CSessionClock c;
	c.AddSample(1000, 50000, 900);		// received before sent
	c.AddSample(1000, 50000, 60000);	// 59 s round trip
	CHECK(!c.IsSynced());
	CHECK(c.RttMs() == 0);
	CHECK(c.ToSession(1000) == 1000);
}
