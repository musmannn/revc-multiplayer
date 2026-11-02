#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "MpTypes.h"

TEST_CASE("constants")
{
	CHECK(MP_MAX_PLAYERS == 8);
	CHECK(MP_DEFAULT_RELAY_PORT == 7777);
}
