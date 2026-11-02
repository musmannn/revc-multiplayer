#pragma once

#ifdef MULTIPLAYER

// The in-game route and arena editor: chat commands (lines starting with '/'
// are local and never sent) on top of CRouteEditor (mp/common), plus markers
// for the route being edited. /rec help lists the commands; /pos prints where
// you are.
class CRouteRecorder
{
public:
	// Returns true if text was a command (handled here, not chat).
	static bool OnCommand(const char *text);
	// Per frame while not paused: preview markers for the route being edited.
	static void Process(void);
	static bool IsEditing(void);
};

#endif
