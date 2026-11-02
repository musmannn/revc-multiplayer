// revc-server: a dedicated session host for reVC multiplayer (Plan 5).

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <thread>
#include <vector>

#include "DedicatedHost.h"
#include "RelayServer.h"

static volatile sig_atomic_t sQuit = 0;

static void
OnSignal(int)
{
	sQuit = 1;
}

static uint32_t
NowMs(void)
{
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void
Usage(FILE *f)
{
	fprintf(f,
		"usage: revc-server [options]\n"
		"\n"
		"Hosts a reVC multiplayer session without a player's game. Players join with\n"
		"the invite code it prints. The server is player 0 and has no ped, so a\n"
		"dedicated session has room for %u players.\n"
		"\n"
		"  --relay HOST[:PORT]   relay to host on (default 127.0.0.1:%u)\n"
		"  --with-relay PORT     also run the relay, in this process, on UDP PORT\n"
		"                        (then --relay defaults to 127.0.0.1:PORT)\n"
		"  --name NAME           the server's name in the session (default Server)\n"
		"  --motd TEXT           said in chat when someone joins\n"
		"  --hour H              time of day to start at, 0-23 (default 12)\n"
		"  --fixed-time          keep that time instead of running the clock\n"
		"  --weather N           0 sunny, 1 cloudy, 2 rainy, 3 foggy, ... (default 0)\n"
		"  --routes DIR          race routes: DIR/index.txt (default data/mp/races)\n"
		"  --arenas DIR          arenas: DIR/index.txt (default data/mp/arenas)\n"
		"  --playlist LIST       events to run by themselves, comma separated, e.g.\n"
		"                        \"race:Ocean Drive Sprint,dm,koth:Washington Beach,stunt\"\n"
		"                        (race:ROUTE, dm, tdm, koth, tkoth, stunt; matches take\n"
		"                        :ARENA; koth, tkoth and stunt use the first arena if none)\n"
		"  --playlist-gap SEC    idle seconds before the next playlist event (default 60)\n"
		"  --min-players N       players needed for a playlist event (default 2)\n"
		"  --code-file PATH      write the invite code there whenever it changes\n"
		"  --quiet               only errors\n"
		"  --help                show this help\n"
		"\n"
		"Runs until Ctrl+C / SIGTERM. If the relay drops the session, it makes a new\n"
		"one (with a new code) after 5 seconds.\n",
		(unsigned)(MP_MAX_PLAYERS - 1), (unsigned)MP_DEFAULT_RELAY_PORT);
}

static bool
ParseUint(const char *s, unsigned long min, unsigned long max, unsigned long &out)
{
	if(s == nullptr || *s == '\0')
		return false;
	char *end;
	unsigned long v = strtoul(s, &end, 10);
	if(*end != '\0' || v < min || v > max)
		return false;
	out = v;
	return true;
}

static bool
SameName(const char *a, const char *b)
{
	for(; *a && *b; a++, b++)
		if(tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

// DIR/index.txt lists .route files in DIR.
static std::vector<SRaceRoute>
LoadDir(const char *dir, bool arenas, bool quiet)
{
	std::vector<SRaceRoute> out;
	char path[1024];
	snprintf(path, sizeof(path), "%s/index.txt", dir);
	FILE *f = fopen(path, "rb");
	if(f == nullptr)
		return out;
	char line[512];
	while(fgets(line, sizeof(line), f)){
		char *p = line;
		while(*p == ' ' || *p == '\t')
			p++;
		size_t n = strlen(p);
		while(n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
			p[--n] = '\0';
		if(*p == '\0' || *p == '#')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, p);
		SRaceRoute r;
		char err[128];
		if(!MpLoadRouteFile(path, r, err, sizeof(err)))
			fprintf(stderr, "revc-server: %s skipped: %s\n", path, err);
		else if((r.type == RACE_ARENA) != arenas)
			fprintf(stderr, "revc-server: %s skipped: %s\n", path, arenas ? "not an arena" : "an arena");
		else{
			out.push_back(r);
			if(!quiet)
				printf("[server] loaded %s '%s'\n", arenas ? "arena" : "route", r.name);
		}
	}
	fclose(f);
	return out;
}

// NAME among the loaded ones, or a .route file path.
static bool
FindRoute(const std::vector<SRaceRoute> &list, const char *name, bool arena, SRaceRoute &out)
{
	for(const SRaceRoute &r : list)
		if(SameName(r.name, name)){
			out = r;
			return true;
		}
	char err[128];
	if(MpLoadRouteFile(name, out, err, sizeof(err)) && (out.type == RACE_ARENA) == arena)
		return true;
	return false;
}

static bool
ParsePlaylist(const char *text, const std::vector<SRaceRoute> &routes, const std::vector<SRaceRoute> &arenas, SServerConfig &config)
{
	static const struct { const char *name; uint8_t mode; } modes[] = {
		{ "dm", MATCH_DM }, { "tdm", MATCH_TDM }, { "koth", MATCH_KOTH }, { "tkoth", MATCH_TKOTH }, { "stunt", MATCH_STUNT },
	};
	char buf[2048];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	config.playlistLength = 0;
	for(char *item = strtok(buf, ","); item; item = strtok(nullptr, ",")){
		while(*item == ' ')
			item++;
		if(*item == '\0')
			continue;
		if(config.playlistLength >= SServerConfig::MAX_PLAYLIST){
			fprintf(stderr, "revc-server: the playlist holds at most %d events\n", (int)SServerConfig::MAX_PLAYLIST);
			return false;
		}
		SPlaylistEntry &e = config.playlist[config.playlistLength];
		memset(&e, 0, sizeof(e));
		char *arg = strchr(item, ':');
		if(arg)
			*arg++ = '\0';
		if(strcmp(item, "race") == 0){
			if(arg == nullptr || !FindRoute(routes, arg, false, e.route)){
				fprintf(stderr, "revc-server: playlist: no race route '%s'\n", arg ? arg : "");
				return false;
			}
			e.race = true;
			e.hasRoute = true;
		}else{
			bool found = false;
			for(const auto &m : modes)
				if(strcmp(item, m.name) == 0){
					e.mode = m.mode;
					found = true;
				}
			if(!found){
				fprintf(stderr, "revc-server: playlist: unknown event '%s'\n", item);
				return false;
			}
			if(arg){
				if(!FindRoute(arenas, arg, true, e.route)){
					fprintf(stderr, "revc-server: playlist: no arena '%s'\n", arg);
					return false;
				}
				e.hasRoute = true;
			}else if(MpModeNeedsArena(e.mode)){
				if(arenas.empty()){
					fprintf(stderr, "revc-server: playlist: '%s' needs an arena and none are loaded\n", item);
					return false;
				}
				e.route = arenas[0];
				e.hasRoute = true;
			}
		}
		config.playlistLength++;
	}
	return true;
}

int
main(int argc, char **argv)
{
	SServerConfig config;
	char relayHost[256] = "127.0.0.1";
	uint16_t relayPort = MP_DEFAULT_RELAY_PORT;
	bool relayGiven = false;
	uint16_t embedPort = 0;
	const char *routesDir = "data/mp/races";
	const char *arenasDir = "data/mp/arenas";
	const char *playlist = nullptr;
	const char *codeFile = nullptr;

	for(int i = 1; i < argc; i++){
		const char *a = argv[i];
		const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
		unsigned long v;
#define NEED(cond, msg) do{ if(!(cond)){ fprintf(stderr, "revc-server: %s\n", msg); return 2; } i++; }while(0)
		if(strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0){
			Usage(stdout);
			return 0;
		}else if(strcmp(a, "--relay") == 0){
			NEED(next && strlen(next) < sizeof(relayHost), "--relay needs HOST[:PORT]");
			strcpy(relayHost, next);
			char *colon = strrchr(relayHost, ':');
			if(colon){
				*colon = '\0';
				if(!ParseUint(colon + 1, 1, 65535, v)){
					fprintf(stderr, "revc-server: bad port in --relay\n");
					return 2;
				}
				relayPort = (uint16_t)v;
			}
			relayGiven = true;
		}else if(strcmp(a, "--with-relay") == 0){
			NEED(ParseUint(next, 1, 65535, v), "--with-relay needs a port 1-65535");
			embedPort = (uint16_t)v;
		}else if(strcmp(a, "--name") == 0){
			NEED(next && MpSanitizeName(next, config.name), "--name needs a name");
		}else if(strcmp(a, "--motd") == 0){
			NEED(next, "--motd needs text");
			strncpy(config.motd, next, MP_MAX_CHAT_LEN - 32);	// room for "Welcome NAME! "
			config.motd[MP_MAX_CHAT_LEN - 32] = '\0';
		}else if(strcmp(a, "--hour") == 0){
			NEED(next && (strcmp(next, "0") == 0 || ParseUint(next, 1, 23, v)), "--hour needs 0-23");
			config.hour = (uint8_t)atoi(next);
		}else if(strcmp(a, "--fixed-time") == 0){
			config.clockRuns = false;
		}else if(strcmp(a, "--weather") == 0){
			NEED(next && (strcmp(next, "0") == 0 || ParseUint(next, 1, 7, v)), "--weather needs 0-7");
			config.weather = (uint8_t)atoi(next);
		}else if(strcmp(a, "--routes") == 0){
			NEED(next, "--routes needs a folder");
			routesDir = next;
		}else if(strcmp(a, "--arenas") == 0){
			NEED(next, "--arenas needs a folder");
			arenasDir = next;
		}else if(strcmp(a, "--playlist") == 0){
			NEED(next, "--playlist needs a list");
			playlist = next;
		}else if(strcmp(a, "--playlist-gap") == 0){
			NEED(ParseUint(next, 1, 3600, v), "--playlist-gap needs 1-3600 seconds");
			config.playlistGapMs = (uint32_t)v * 1000;
		}else if(strcmp(a, "--min-players") == 0){
			NEED(ParseUint(next, 1, MP_MAX_PLAYERS - 1, v), "--min-players needs 1-7");
			config.playlistMinPlayers = (int)v;
		}else if(strcmp(a, "--code-file") == 0){
			NEED(next, "--code-file needs a path");
			codeFile = next;
		}else if(strcmp(a, "--quiet") == 0){
			config.verbose = false;
		}else{
			fprintf(stderr, "revc-server: unknown argument '%s'\n\n", a);
			Usage(stderr);
			return 2;
		}
#undef NEED
	}
	if(embedPort && !relayGiven)
		relayPort = embedPort;

	std::vector<SRaceRoute> routes = LoadDir(routesDir, false, !config.verbose);
	std::vector<SRaceRoute> arenas = LoadDir(arenasDir, true, !config.verbose);
	if(playlist && !ParsePlaylist(playlist, routes, arenas, config))
		return 2;

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);

	CRelayServer relay;
	if(embedPort){
		SRelayConfig rc;
		rc.port = embedPort;
		if(!relay.Start(rc)){
			fprintf(stderr, "revc-server: could not run the relay on UDP %u (port in use?)\n", (unsigned)embedPort);
			return 1;
		}
		if(config.verbose)
			printf("[server] relay listening on UDP %u\n", (unsigned)relay.Port());
	}

	CDedicatedHost host(config);
	if(!host.Start(relayHost, relayPort, NowMs())){
		fprintf(stderr, "revc-server: can't reach relay %s:%u\n", relayHost, (unsigned)relayPort);
		return 1;
	}
	if(config.verbose)
		printf("[server] '%s' connecting to %s:%u, %d playlist events\n", config.name, relayHost, (unsigned)relayPort,
			config.playlistLength);

	while(!sQuit){
		if(embedPort)
			relay.Service(5);
		else
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		host.Service(NowMs());
		if(host.TakeSessionChanged()){
			printf("CODE %s\n", host.Code());
			fflush(stdout);
			if(codeFile){
				FILE *f = fopen(codeFile, "w");
				if(f){
					fprintf(f, "%s\n", host.Code());
					fclose(f);
				}else
					fprintf(stderr, "revc-server: can't write %s\n", codeFile);
			}
		}
	}
	host.Stop();
	if(embedPort){
		// Let the relay tell everyone the session ended.
		uint32_t until = NowMs() + 300;
		while((int32_t)(NowMs() - until) < 0)
			relay.Service(10);
		relay.Stop();
	}
	if(config.verbose)
		printf("[server] stopped\n");
	return 0;
}
