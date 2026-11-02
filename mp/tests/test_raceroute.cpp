#include "doctest.h"

#include <string.h>
#include <string>

#include "RaceRoute.h"

static const char *SPRINT =
	"# Ocean Drive, north to south\n"
	"name Ocean Drive Sprint\n"
	"type sprint\n"
	"vehicle 141\n"
	"\n"
	"grid 100.5 -200 10.25 180   # pole\n"
	"grid 104.5 -200 10.25 180\n"
	"cp 100 -300 10 8\n"
	"cp 110 -400 10 8.5\n"
	"cp 120 -500 11 12\n";

static bool
Parse(const char *text, SRaceRoute &route, std::string &err)
{
	char buf[128];
	buf[0] = 'x';
	buf[1] = '\0';
	bool ok = MpParseRoute(text, route, buf, sizeof(buf));
	err = buf;
	return ok;
}

static void
ExpectError(const char *text, const char *prefix)
{
	SRaceRoute route;
	std::string err;
	CAPTURE(text);
	CHECK(!Parse(text, route, err));
	CAPTURE(err);
	CHECK(err.compare(0, strlen(prefix), prefix) == 0);
}

TEST_CASE("route: valid sprint")
{
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse(SPRINT, route, err));
	CHECK(err == "");
	CHECK(strcmp(route.name, "Ocean Drive Sprint") == 0);
	CHECK(route.type == RACE_SPRINT);
	CHECK(route.laps == 1);
	CHECK(route.vehicleModel == 141);
	CHECK(route.numGrid == 2);
	CHECK(route.grid[0].pos[0] == 100.5f);
	CHECK(route.grid[0].pos[1] == -200.0f);
	CHECK(route.grid[0].pos[2] == 10.25f);
	CHECK(route.grid[0].heading == 180.0f);
	CHECK(route.numCheckpoints == 3);
	CHECK(route.cps[1].pos[0] == 110.0f);
	CHECK(route.cps[1].radius == 8.5f);
	CHECK(route.cps[2].pos[2] == 11.0f);
	CHECK(MpRouteTotalCrossings(route) == 3);
}

TEST_CASE("route: valid circuit")
{
	const char *text =
		"name Starfish Loop\n"
		"type circuit\n"
		"laps 3\n"
		"vehicle 145\n"
		"grid 0 0 10 90\n"
		"cp 10 0 10 8\n"
		"cp 20 0 10 8\n"
		"cp 30 0 10 8\n"
		"cp 0 0 10 8\n";
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse(text, route, err));
	CHECK(route.type == RACE_CIRCUIT);
	CHECK(route.laps == 3);
	CHECK(route.numCheckpoints == 4);
	CHECK(MpRouteTotalCrossings(route) == 12);
}

TEST_CASE("route: comments, blank lines, CRLF and tabs")
{
	const char *text =
		"# header comment\r\n"
		"\r\n"
		"   name\tTabbed Name  \r\n"
		"type sprint # trailing comment\r\n"
		"vehicle\t132\r\n"
		"\t\r\n"
		"grid 1 2 3 4\r\n"
		"cp 1 2 3 4\r\n"
		"cp 5 6 7 8";	// no newline at the end
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse(text, route, err));
	CHECK(strcmp(route.name, "Tabbed Name") == 0);
	CHECK(route.vehicleModel == 132);
	CHECK(route.numCheckpoints == 2);
}

TEST_CASE("route: validation errors")
{
	ExpectError("type sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "missing name");
	ExpectError("name X\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad type");
	ExpectError("name X\ntype drag\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad type");
	ExpectError("name X\ntype circuit\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad laps");
	ExpectError("name X\ntype circuit\nlaps 0\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad laps");
	ExpectError("name X\ntype circuit\nlaps 10\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad laps");
	ExpectError("name X\ntype sprint\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad vehicle");
	ExpectError("name X\ntype sprint\nvehicle 0\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad vehicle");
	ExpectError("name X\ntype sprint\nvehicle 70000\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "bad vehicle");
	ExpectError("name X\ntype sprint\nvehicle 1\ncp 0 0 0 1\ncp 1 1 1 1\n", "no grid");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\n", "too few checkpoints");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\n", "too few checkpoints");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 abc 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "line 4: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "line 4: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1 9\ncp 1 1 1 1\n", "line 5: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 12x\ncp 1 1 1 1\n", "line 5: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 nan\ncp 1 1 1 1\n", "line 5: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 0\ncp 1 1 1 1\n", "line 5: bad number");
	ExpectError("name X\ntype sprint\nvehicle twelve\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", "line 3: bad number");
	ExpectError("name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\nbogus 1 2\n", "line 7: unknown directive");
	ExpectError("", "missing name");
}

TEST_CASE("route: too many checkpoints")
{
	std::string text = "name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\n";
	for(uint32_t i = 0; i < MP_MAX_CHECKPOINTS + 1; i++)
		text += "cp 1 2 3 4\n";
	ExpectError(text.c_str(), "too many checkpoints");

	std::string ok = "name X\ntype sprint\nvehicle 1\ngrid 0 0 0 0\n";
	for(uint32_t i = 0; i < MP_MAX_CHECKPOINTS; i++)
		ok += "cp 1 2 3 4\n";
	SRaceRoute route;
	std::string err;
	CHECK(Parse(ok.c_str(), route, err));
	CHECK(route.numCheckpoints == MP_MAX_CHECKPOINTS);
}

TEST_CASE("route: 9 grid lines keep the first 8")
{
	std::string text = "name X\ntype sprint\nvehicle 1\ncp 0 0 0 1\ncp 1 1 1 1\n";
	for(int i = 0; i < 9; i++)
		text += "grid " + std::to_string(i) + " 0 0 0\n";
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse(text.c_str(), route, err));
	CHECK(route.numGrid == 8);
	CHECK(route.grid[7].pos[0] == 7.0f);
}

TEST_CASE("route: sprint forces one lap")
{
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse("name X\ntype sprint\nlaps 5\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", route, err));
	CHECK(route.laps == 1);
	CHECK(MpRouteTotalCrossings(route) == 2);
}

TEST_CASE("route: long names are truncated, null input fails")
{
	SRaceRoute route;
	std::string err;
	REQUIRE(Parse("name 0123456789012345678901234567890123456789\ntype sprint\nvehicle 1\ngrid 0 0 0 0\ncp 0 0 0 1\ncp 1 1 1 1\n", route, err));
	CHECK(strlen(route.name) == sizeof(route.name) - 1);
	char buf[32];
	CHECK(!MpParseRoute(nullptr, route, buf, sizeof(buf)));
	CHECK(!MpParseRoute("name X", route, nullptr, 0));	// no error buffer is fine
}
