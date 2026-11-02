#include "NetInit.h"

#include <enet/enet.h>

// Single-threaded by design: the game, relay and tools all run their network
// code on one thread.
static int sNetRefs = 0;

bool
MpNetAcquire(void)
{
	if(sNetRefs == 0 && enet_initialize() != 0)
		return false;
	sNetRefs++;
	return true;
}

void
MpNetRelease(void)
{
	if(sNetRefs <= 0)
		return;
	if(--sNetRefs == 0)
		enet_deinitialize();
}
