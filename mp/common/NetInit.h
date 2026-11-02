#pragma once

// Reference-counted enet_initialize/enet_deinitialize, so the relay, any
// number of clients and the tests can share one process.

bool MpNetAcquire(void);
void MpNetRelease(void);
