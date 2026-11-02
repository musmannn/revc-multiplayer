// revc-relay: the invite-code relay server for reVC multiplayer.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "RelayServer.h"

static volatile sig_atomic_t sQuit = 0;

static void
OnSignal(int)
{
	sQuit = 1;
}

static void
Usage(FILE *f)
{
	fprintf(f,
		"usage: revc-relay [--port N] [--bind ADDR] [--max-sessions N]\n"
		"\n"
		"Invite-code relay server for reVC multiplayer.\n"
		"\n"
		"  --port N          UDP port to listen on (default %u)\n"
		"  --bind ADDR       local address to bind (default: all interfaces)\n"
		"  --max-sessions N  maximum concurrent sessions (default 256)\n"
		"  --help            show this help\n"
		"\n"
		"Runs until Ctrl+C / SIGTERM.\n",
		(unsigned)MP_DEFAULT_RELAY_PORT);
}

static bool
ParseUint(const char *s, unsigned long max, unsigned long &out)
{
	if(s == nullptr || *s == '\0')
		return false;
	char *end;
	unsigned long v = strtoul(s, &end, 10);
	if(*end != '\0' || v == 0 || v > max)
		return false;
	out = v;
	return true;
}

int
main(int argc, char **argv)
{
	SRelayConfig config;
	for(int i = 1; i < argc; i++){
		const char *a = argv[i];
		const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
		unsigned long v;
		if(strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0){
			Usage(stdout);
			return 0;
		}else if(strcmp(a, "--port") == 0){
			if(!ParseUint(next, 65535, v)){
				fprintf(stderr, "revc-relay: --port needs a number 1-65535\n");
				return 2;
			}
			config.port = (uint16_t)v;
			i++;
		}else if(strcmp(a, "--bind") == 0){
			if(next == nullptr){
				fprintf(stderr, "revc-relay: --bind needs an address\n");
				return 2;
			}
			config.bindAddress = next;
			i++;
		}else if(strcmp(a, "--max-sessions") == 0){
			if(!ParseUint(next, 500, v)){
				fprintf(stderr, "revc-relay: --max-sessions needs a number 1-500\n");
				return 2;
			}
			config.maxSessions = (uint32_t)v;
			i++;
		}else{
			fprintf(stderr, "revc-relay: unknown argument '%s'\n\n", a);
			Usage(stderr);
			return 2;
		}
	}

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);

	CRelayServer relay;
	if(!relay.Start(config)){
		fprintf(stderr, "revc-relay: could not listen on UDP %s:%u (port in use?)\n",
			config.bindAddress ? config.bindAddress : "*", (unsigned)config.port);
		return 1;
	}
	MpRelayLog("revc-relay listening on UDP %s:%u, protocol %u, up to %u sessions",
		config.bindAddress ? config.bindAddress : "*", (unsigned)relay.Port(),
		(unsigned)MP_PROTOCOL_VERSION, (unsigned)config.maxSessions);

	while(!sQuit)
		relay.Service(50);

	MpRelayLog("shutting down (%u sessions, %u peers)", (unsigned)relay.NumSessions(), (unsigned)relay.NumPeers());
	relay.Stop();
	return 0;
}
