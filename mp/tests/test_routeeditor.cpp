#include "doctest.h"

#include <string.h>

#include <string>
#include <vector>

#include "RouteEditor.h"

namespace {

class CTestHost : public IRouteEditorHost
{
public:
	std::vector<SRaceRoute> saved;
	std::vector<SRaceRoute> tested;
	std::vector<SRaceRoute> library;
	bool failSave = false;

	bool LoadRoute(const char *name, SRaceRoute &out, char *err, uint32_t errSize) override
	{
		for(const SRaceRoute &r : library)
			if(strcmp(r.name, name) == 0){
				out = r;
				return true;
			}
		snprintf(err, errSize, "no such route");
		return false;
	}
	bool SaveRoute(const SRaceRoute &route, char *err, uint32_t errSize) override
	{
		if(failSave){
			snprintf(err, errSize, "disk full");
			return false;
		}
		saved.push_back(route);
		return true;
	}
	void TestRoute(const SRaceRoute &route) override { tested.push_back(route); }
};

struct SEditorFixture
{
	CRouteEditor ed;
	CTestHost host;
	SEditorPlace here;
	char reply[160];

	SEditorFixture(void)
	{
		memset(&here, 0, sizeof(here));
		reply[0] = '\0';
	}
	void At(float x, float y, float z, float heading = 0.0f, uint16_t vehicle = 0)
	{
		here.pos[0] = x;
		here.pos[1] = y;
		here.pos[2] = z;
		here.heading = heading;
		here.vehicleModel = vehicle;
	}
	bool Run(const char *cmd) { return ed.Command(cmd, &here, &host, reply, sizeof(reply)); }
	bool RunNowhere(const char *cmd) { return ed.Command(cmd, nullptr, &host, reply, sizeof(reply)); }
	bool ReplyHas(const char *text) const { return strstr(reply, text) != nullptr; }
};

}

TEST_CASE("route editor: record and save a sprint")
{
	SEditorFixture f;
	CHECK(!f.Run("grid"));
	CHECK(f.ReplyHas("Start with"));
	f.At(10, 20, 5, 90, 159);
	REQUIRE(f.Run("new Ocean Drive  Sprint "));
	CHECK(f.ed.IsActive());
	CHECK(strcmp(f.ed.Route().name, "Ocean Drive  Sprint") == 0);
	CHECK(f.ed.Route().vehicleModel == 159);	// the car we're in
	CHECK(f.Run("grid"));
	f.At(10, 26, 5, 90);
	CHECK(f.Run("grid"));
	CHECK(f.ed.Route().numGrid == 2);
	CHECK(f.ed.Route().grid[1].pos[1] == 26.0f);
	CHECK(f.ed.Route().grid[0].heading == 90.0f);

	CHECK(!f.Run("save"));
	CHECK(f.ReplyHas("too few checkpoints"));
	f.At(100, 20, 5);
	CHECK(f.Run("cp"));
	CHECK(f.ed.Route().cps[0].radius == CRouteEditor::DEFAULT_CP_RADIUS);
	f.At(200, 20, 5);
	CHECK(f.Run("cp 12.5"));
	CHECK(f.ed.Route().cps[1].radius == 12.5f);
	CHECK(!f.Run("cp -3"));
	CHECK(!f.Run("cp 500"));
	CHECK(f.ed.Route().numCheckpoints == 2);

	CHECK(f.ed.IsDirty());
	REQUIRE(f.Run("save"));
	REQUIRE(f.host.saved.size() == 1);
	CHECK(f.host.saved[0].numCheckpoints == 2);
	CHECK(!f.ed.IsDirty());
	CHECK(f.Run("list"));
	CHECK(f.ReplyHas("sprint"));
	CHECK(f.ReplyHas("2 grid slots"));
	CHECK(f.ReplyHas("2 checkpoints"));
	CHECK(!f.ReplyHas("unsaved"));
}

TEST_CASE("route editor: insert, move, delete, radius")
{
	SEditorFixture f;
	f.At(0, 0, 0);
	REQUIRE(f.Run("new R"));
	for(int i = 1; i <= 3; i++){
		f.At((float)i * 100, 0, 0);
		REQUIRE(f.Run("cp"));
	}
	f.At(150, 5, 1);
	REQUIRE(f.Run("ins 2 6"));
	REQUIRE(f.ed.Route().numCheckpoints == 4);
	CHECK(f.ed.Route().cps[0].pos[0] == 100.0f);
	CHECK(f.ed.Route().cps[1].pos[0] == 150.0f);
	CHECK(f.ed.Route().cps[1].radius == 6.0f);
	CHECK(f.ed.Route().cps[2].pos[0] == 200.0f);
	CHECK(!f.Run("ins 0"));
	CHECK(!f.Run("ins 5"));	// append with /rec cp instead

	f.At(333, 3, 3);
	REQUIRE(f.Run("move 4"));
	CHECK(f.ed.Route().cps[3].pos[0] == 333.0f);
	CHECK(!f.Run("move 9"));
	CHECK(!f.Run("move x"));

	REQUIRE(f.Run("radius 1 20"));
	CHECK(f.ed.Route().cps[0].radius == 20.0f);
	CHECK(!f.Run("radius 1"));
	CHECK(!f.Run("radius 1 0"));

	REQUIRE(f.Run("del 2"));
	REQUIRE(f.ed.Route().numCheckpoints == 3);
	CHECK(f.ed.Route().cps[1].pos[0] == 200.0f);
	CHECK(f.ReplyHas("2 left") == false);
	CHECK(f.ReplyHas("3 left"));
}

TEST_CASE("route editor: grid edits and limits")
{
	SEditorFixture f;
	f.At(0, 0, 0);
	REQUIRE(f.Run("new R"));
	for(int i = 0; i < 8; i++){
		f.At((float)i, 0, 0, (float)i * 10);
		REQUIRE(f.Run("grid"));
	}
	CHECK(!f.Run("grid"));
	CHECK(f.ReplyHas("at most 8"));
	f.At(50, 50, 50, 45);
	REQUIRE(f.Run("movegrid 3"));
	CHECK(f.ed.Route().grid[2].pos[0] == 50.0f);
	CHECK(f.ed.Route().grid[2].heading == 45.0f);
	REQUIRE(f.Run("delgrid 1"));
	CHECK(f.ed.Route().numGrid == 7);
	CHECK(f.ed.Route().grid[0].pos[0] == 1.0f);
	CHECK(!f.Run("delgrid 8"));
	CHECK(!f.RunNowhere("grid"));
	CHECK(f.ReplyHas("in the world"));
}

TEST_CASE("route editor: type, laps, vehicle, name")
{
	SEditorFixture f;
	f.At(0, 0, 0);
	REQUIRE(f.Run("new R"));
	CHECK(f.ed.Route().vehicleModel == CRouteEditor::DEFAULT_RACE_VEHICLE);
	CHECK(!f.Run("laps 3"));	// sprints have no laps
	REQUIRE(f.Run("type circuit"));
	CHECK(f.ed.Route().type == RACE_CIRCUIT);
	CHECK(f.ed.Route().laps == 3);
	REQUIRE(f.Run("laps 5"));
	CHECK(f.ed.Route().laps == 5);
	CHECK(!f.Run("laps 10"));
	CHECK(!f.Run("type drift"));
	REQUIRE(f.Run("type sprint"));
	CHECK(f.ed.Route().laps == 1);
	CHECK(!f.Run("vehicle"));	// on foot
	f.At(0, 0, 0, 0, 191);
	REQUIRE(f.Run("vehicle"));
	CHECK(f.ed.Route().vehicleModel == 191);
	REQUIRE(f.Run("vehicle 145"));
	CHECK(f.ed.Route().vehicleModel == 145);
	REQUIRE(f.Run("name Starfish Loop"));
	CHECK(strcmp(f.ed.Route().name, "Starfish Loop") == 0);
	CHECK(!f.Run("name"));
}

TEST_CASE("route editor: undo")
{
	SEditorFixture f;
	f.At(0, 0, 0);
	REQUIRE(f.Run("new R"));
	CHECK(!f.Run("undo"));
	f.At(1, 0, 0);
	REQUIRE(f.Run("cp"));
	f.At(2, 0, 0);
	REQUIRE(f.Run("cp"));
	REQUIRE(f.Run("radius 1 30"));
	REQUIRE(f.Run("undo"));
	CHECK(f.ed.Route().cps[0].radius == CRouteEditor::DEFAULT_CP_RADIUS);
	REQUIRE(f.Run("undo"));
	CHECK(f.ed.Route().numCheckpoints == 1);
	REQUIRE(f.Run("undo"));
	CHECK(f.ed.Route().numCheckpoints == 0);
	CHECK(!f.Run("undo"));

	// Only the last MAX_UNDO steps are kept.
	for(int i = 0; i < CRouteEditor::MAX_UNDO + 5; i++)
		REQUIRE(f.Run("cp"));
	int undone = 0;
	while(f.Run("undo"))
		undone++;
	CHECK(undone == CRouteEditor::MAX_UNDO);
	CHECK(f.ed.Route().numCheckpoints == 5);
}

TEST_CASE("route editor: arenas")
{
	SEditorFixture f;
	f.At(0, 0, 10, 0, 191);
	REQUIRE(f.Run("arena Washington Beach"));
	CHECK(f.ed.Route().type == RACE_ARENA);
	CHECK(f.ed.Route().vehicleModel == 0);	// optional for arenas
	REQUIRE(f.Run("spawn"));
	REQUIRE(f.Run("hill"));
	CHECK(f.ed.Route().cps[0].radius == CRouteEditor::DEFAULT_HILL_RADIUS);
	CHECK(f.ReplyHas("hill 1"));
	REQUIRE(f.Run("save"));
	CHECK(f.host.saved.back().type == RACE_ARENA);
	for(uint32_t i = 1; i < MP_MAX_HILLS; i++)
		REQUIRE(f.Run("hill"));
	CHECK(!f.Run("hill"));
	CHECK(f.ReplyHas("at most 16 hills"));
	CHECK(!f.Run("test"));
	CHECK(f.host.tested.empty());

	// A long race can't become an arena.
	REQUIRE(f.Run("new Long"));
	for(uint32_t i = 0; i < MP_MAX_HILLS + 1; i++)
		REQUIRE(f.Run("cp"));
	CHECK(!f.Run("type arena"));
	CHECK(f.ed.Route().type == RACE_SPRINT);
}

TEST_CASE("route editor: load, test, close")
{
	SEditorFixture f;
	SRaceRoute lib;
	memset(&lib, 0, sizeof(lib));
	strcpy(lib.name, "Bridge Run");
	lib.type = RACE_SPRINT;
	lib.laps = 1;
	lib.vehicleModel = 141;
	lib.numGrid = 1;
	lib.numCheckpoints = 2;
	lib.cps[0].radius = lib.cps[1].radius = 8.0f;
	f.host.library.push_back(lib);

	CHECK(!f.Run("load Nowhere"));
	CHECK(f.ReplyHas("no such route"));
	REQUIRE(f.Run("load Bridge Run"));
	CHECK(!f.ed.IsDirty());
	CHECK(f.ed.Route().numCheckpoints == 2);
	REQUIRE(f.Run("test"));
	REQUIRE(f.host.tested.size() == 1);
	CHECK(strcmp(f.host.tested[0].name, "Bridge Run") == 0);

	f.host.failSave = true;
	CHECK(!f.Run("save"));
	CHECK(f.ReplyHas("disk full"));

	REQUIRE(f.Run("close"));
	CHECK(!f.ed.IsActive());
	CHECK(!f.Run("list"));
	CHECK(!f.Run("frobnicate"));
}

TEST_CASE("route editor: what it saves reads back the same through the text format")
{
	SEditorFixture f;
	f.At(-1234.56f, 789.01f, 11.5f, 271.25f, 141);
	REQUIRE(f.Run("new Round Trip"));
	REQUIRE(f.Run("type circuit"));
	REQUIRE(f.Run("grid"));
	REQUIRE(f.Run("cp 7.25"));
	f.At(-1000.0f, 800.0f, 12.0f);
	REQUIRE(f.Run("cp"));

	char text[4096];
	int len = MpWriteRoute(f.ed.Route(), text, sizeof(text));
	REQUIRE(len > 0);
	SRaceRoute back;
	char err[96];
	REQUIRE_MESSAGE(MpParseRoute(text, back, err, sizeof(err)), err);
	CHECK(strcmp(back.name, "Round Trip") == 0);
	CHECK(back.type == RACE_CIRCUIT);
	CHECK(back.laps == 3);
	CHECK(back.vehicleModel == 141);
	CHECK(back.grid[0].pos[0] == doctest::Approx(-1234.56f));
	CHECK(back.grid[0].heading == doctest::Approx(271.2f).epsilon(0.001));
	CHECK(back.cps[0].radius == doctest::Approx(7.25f));
	CHECK(MpWriteRoute(f.ed.Route(), text, 20) < 0);	// too small
}
