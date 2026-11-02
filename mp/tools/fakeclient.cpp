// revc-fakeclient: a headless multiplayer client for testing the relay and,
// later, the game without running a second copy of it.
//
//   revc-fakeclient --relay HOST[:PORT] (--host | --join CODE) [--name NAME]
//                   [--say TEXT] [--seconds N] [--bot walk|drive|race --at X,Y,Z [--model N]]
//                   [--start-race ROUTE] [--bot dm] [--start-match dm|tdm]
//
// Prints one line per event. Exits 0 when --seconds runs out or on Ctrl+C,
// non-zero when the session ends, a join fails or the connection drops.

#include <chrono>
#include <functional>
#include <thread>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GameProtocol.h"
#include "RaceHost.h"
#include "MatchHost.h"
#include "HillLogic.h"
#include "StuntLogic.h"
#include "NetClient.h"

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

struct SFakeOptions
{
	char relayHost[256];
	uint16_t relayPort;
	bool host;
	const char *code;
	const char *name;
	const char *say;
	uint32_t seconds;	// 0 = until Ctrl+C
	const char *bot;	// nullptr, "walk", "drive" or "race"
	float at[3];
	uint16_t model;
	const char *startRace;	// route file or name to race on (host: runs it; member: asks the host)
	int startMatch;		// -1, or an eMatchMode (host: runs it; member: asks the host)
	const char *arena;	// arena .route file for --start-match
};

static uint32_t sNow;	// NetNow of the current tick, for callbacks
static bool sHaveArena;
static SRaceRoute sArena;	// --arena

static const char*
ModeName(uint8_t mode)
{
	switch(mode){
	case MATCH_TDM: return "team deathmatch";
	case MATCH_KOTH: return "hold the hill";
	case MATCH_TKOTH: return "team hold the hill";
	case MATCH_STUNT: return "stunt challenge";
	default: return "deathmatch";
	}
}
static CNetClient *sClient;
static char sNames[MP_MAX_PLAYERS][MP_MAX_NAME_LEN + 1];

static const char*
FakeName(uint8_t id)
{
	return id < MP_MAX_PLAYERS && sNames[id][0] ? sNames[id] : "?";
}

static void
SendTo(uint8_t route, uint8_t target, bool reliable, const CByteWriter &w)
{
	if(sClient)
		sClient->SendGame(route, target, reliable, w);
}

static void
PrintStandings(const char *tag, const SRaceStandings &s)
{
	printf("[%s] race %u:", tag, (unsigned)s.raceId);
	for(int i = 0; i < s.n; i++){
		const SRaceStandingEntry &e = s.entries[i];
		const char *state = e.state == RACER_FINISHED ? "finished" : e.state == RACER_DNF ? "DNF" : "racing";
		if(e.state == RACER_FINISHED)
			printf(" %d. %s %02u:%02u.%02u", i + 1, FakeName(e.id), (unsigned)(e.finishTime / 60000),
				(unsigned)(e.finishTime / 1000 % 60), (unsigned)(e.finishTime / 10 % 100));
		else
			printf(" %d. %s %s (cp %u)", i + 1, FakeName(e.id), state, (unsigned)e.nextSeq);
	}
	printf("\n");
	fflush(stdout);
}

// --start-race on the fake host: runs the same CRaceHost as the game host.
class CFakeRaceHost : public IRaceHostSink
{
public:
	CRaceHost host;
	SRaceRoute route;
	bool requested;

	CFakeRaceHost(void) : requested(false) {}

	void SendInvite(const SRaceInvite &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		printf("[race] invite %u '%s' open for %u s\n", (unsigned)m.raceId, m.route.name, (unsigned)(CRaceHost::INVITE_MS / 1000));
		fflush(stdout);
	}
	void SendStart(const SRaceStart &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		printf("[race] start %u with %u racers\n", (unsigned)m.raceId, (unsigned)m.n);
		fflush(stdout);
	}
	void SendStandings(const SRaceStandings &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
		static uint32_t lastPrint;
		if(sNow - lastPrint >= 2000){
			lastPrint = sNow;
			PrintStandings("standings", m);
		}
	}
	void SendEnd(const SRaceStandings &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		PrintStandings("end", m);
	}
	void SendCancel(const SRaceCancel &m, uint8_t target) override
	{
		CByteWriter w;
		m.Write(w);
		if(target == MP_PLAYER_NONE)
			SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		else
			SendTo(MP_ROUTE_PLAYER, target, true, w);
		printf("[race] cancelled (reason %u)\n", (unsigned)m.reason);
		fflush(stdout);
	}
};

static CFakeRaceHost sRaceHost;

static void
PrintScores(const char *tag, const SMatchScores &s, uint8_t mode)
{
	printf("[%s] match %u:", tag, (unsigned)s.matchId);
	if(MpIsTeamMode(mode))
		printf(" Red %d - Blue %d |", (int)s.teamScore[MP_TEAM_RED], (int)s.teamScore[MP_TEAM_BLUE]);
	for(int i = 0; i < s.n; i++){
		const SMatchScoreEntry &e = s.entries[i];
		printf(" %d. %s%s %d (%u/%u)", i + 1, FakeName(e.id),
			e.team == MP_TEAM_RED ? "[R]" : e.team == MP_TEAM_BLUE ? "[B]" : "",
			(int)e.score, (unsigned)e.kills, (unsigned)e.deaths);
	}
	printf("\n");
	fflush(stdout);
}

// --start-match on the fake host: the same CMatchHost the game host runs.
class CFakeMatchHost : public IMatchHostSink
{
public:
	CMatchHost host;
	bool requested;
	uint8_t mode;

	CFakeMatchHost(void) : requested(false), mode(MATCH_DM) {}

	void SendInvite(const SMatchInvite &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		printf("[match] invite %u %s to %u%s%s, open for %u s\n", (unsigned)m.matchId, ModeName(m.mode),
			(unsigned)m.scoreLimit, m.hasArena ? " at " : "", m.hasArena ? m.arena.name : "", (unsigned)(CMatchHost::INVITE_MS / 1000));
		fflush(stdout);
	}
	void SendStart(const SMatchStart &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		printf("[match] start %u with %u players\n", (unsigned)m.matchId, (unsigned)m.n);
		fflush(stdout);
	}
	void SendScores(const SMatchScores &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
		static uint32_t lastPrint;
		if(sNow - lastPrint >= 3000){
			lastPrint = sNow;
			PrintScores("scores", m, mode);
		}
	}
	void SendEnd(const SMatchScores &m) override
	{
		CByteWriter w;
		m.Write(w);
		SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		PrintScores("end", m, mode);
	}
	void SendCancel(const SMatchCancel &m, uint8_t target) override
	{
		CByteWriter w;
		m.Write(w);
		if(target == MP_PLAYER_NONE)
			SendTo(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
		else
			SendTo(MP_ROUTE_PLAYER, target, true, w);
		printf("[match] cancelled (reason %u)\n", (unsigned)m.reason);
		fflush(stdout);
	}
};
static CFakeMatchHost sMatchHost;

// --bot dm: accepts every match and "dies" every 3 s, killed by someone else.
// --bot koth: accepts every match and walks to the active hill.
// --bot stunt: accepts every match and lands a stunt every 5 s.
struct SFakeFighter
{
	bool playing;
	uint16_t matchId;
	uint8_t mode;
	uint32_t startTime;
	uint32_t deaths;
	uint8_t others[MP_MAX_PLAYERS];
	int numOthers;
	int slot;
	bool hasArena;
	SRaceRoute arena;		// from the invite
	CHillState hill;
	float pos[3];
	uint32_t lastStateMs;
	uint32_t stunts;
};
static SFakeFighter sFighter;

// --bot race: accepts every invite and crosses a checkpoint every 4 s.
struct SFakeRacer
{
	bool racing;
	uint16_t raceId;
	uint32_t startTime;
	uint16_t seq;
};
static SFakeRacer sRacer;

static bool
LoadRaceRoute(const char *nameOrPath, SRaceRoute &route)
{
	FILE *f = fopen(nameOrPath, "rb");
	if(f){
		static char text[64 * 1024];
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[n] = '\0';
		char err[128];
		if(!MpParseRoute(text, route, err, sizeof(err))){
			fprintf(stderr, "revc-fakeclient: %s: %s\n", nameOrPath, err);
			return false;
		}
		return true;
	}
	// No such file: a small built-in test sprint with that name.
	memset(&route, 0, sizeof(route));
	strncpy(route.name, nameOrPath, MP_MAX_ROUTE_NAME);
	route.type = RACE_SPRINT;
	route.laps = 1;
	route.vehicleModel = 141;
	route.numGrid = MP_MAX_PLAYERS;
	route.numCheckpoints = 3;
	for(int i = 0; i < 3; i++)
		route.cps[i].radius = 8.0f;
	return true;
}

static const char*
JoinResultText(eJoinResult r)
{
	switch(r){
	case JOIN_OK: return "ok";
	case JOIN_NOT_FOUND: return "not found";
	case JOIN_FULL: return "session full";
	case JOIN_VERSION_MISMATCH: return "version mismatch";
	case JOIN_BAD_NAME: return "bad name";
	}
	return "unknown";
}

class CFakeListener : public INetClientListener
{
public:
	const SFakeOptions *opts;
	CNetClient *client;
	int exitCode;
	bool done;

	CFakeListener(const SFakeOptions *o, CNetClient *c) : opts(o), client(c), exitCode(0), done(false) {}

	void OnSessionReady(const char *code, uint8_t myId, bool isHost) override
	{
		printf("[ready] code=%s id=%u %s\n", code, (unsigned)myId, isHost ? "host" : "member");
		fflush(stdout);
		if(myId < MP_MAX_PLAYERS){
			strncpy(sNames[myId], opts->name, MP_MAX_NAME_LEN);
			sNames[myId][MP_MAX_NAME_LEN] = '\0';
		}
		if(opts->say && opts->say[0])
			client->SendChat(opts->say);
		if(!isHost && opts->startRace){
			// A member asks the host (a player's game or revc-server).
			SRaceRequest req;
			req.route = sRaceHost.route;
			CByteWriter w;
			req.Write(w);
			client->SendGame(MP_ROUTE_HOST, 0, true, w);
			printf("[race] asked the host for '%s'\n", req.route.name);
			fflush(stdout);
		}
		if(!isHost && opts->startMatch >= 0){
			SMatchRequest req;
			req.mode = (uint8_t)opts->startMatch;
			req.hasArena = sHaveArena;
			req.arena = sArena;
			CByteWriter w;
			req.Write(w);
			client->SendGame(MP_ROUTE_HOST, 0, true, w);
			printf("[match] asked the host for %s\n", ModeName(req.mode));
			fflush(stdout);
		}
	}
	void OnJoinFailed(eJoinResult result) override
	{
		printf("[failed] %s\n", JoinResultText(result));
		fflush(stdout);
		exitCode = 3;
		done = true;
	}
	void OnPlayerJoined(uint8_t id, const char *name) override
	{
		printf("[joined] %u %s\n", (unsigned)id, name);
		fflush(stdout);
		if(id < MP_MAX_PLAYERS){
			strncpy(sNames[id], name, MP_MAX_NAME_LEN);
			sNames[id][MP_MAX_NAME_LEN] = '\0';
		}
		if(opts->startMatch >= 0 && client->IsHost()){
			if(!sMatchHost.requested){
				sMatchHost.requested = true;
				sMatchHost.mode = (uint8_t)opts->startMatch;
				sMatchHost.host.Request(client->MyId(), sMatchHost.mode, false, client->SessionTime(sNow), &sMatchHost,
					sHaveArena ? &sArena : nullptr);
			}else{
				SMatchInvite invite;
				if(sMatchHost.host.GetInvite(invite)){
					CByteWriter w;
					invite.Write(w);
					client->SendGame(MP_ROUTE_PLAYER, id, true, w);
				}
			}
		}
		if(opts->startRace && client->IsHost()){
			// The race opens when the first player arrives; late joiners
			// still get the open invite.
			if(!sRaceHost.requested){
				sRaceHost.requested = true;
				sRaceHost.host.Request(client->MyId(), sRaceHost.route, false, client->SessionTime(sNow), &sRaceHost);
			}else{
				SRaceInvite invite;
				if(sRaceHost.host.GetInvite(invite)){
					CByteWriter w;
					invite.Write(w);
					client->SendGame(MP_ROUTE_PLAYER, id, true, w);
				}
			}
		}
	}
	void OnPlayerLeft(uint8_t id) override
	{
		printf("[left] %u\n", (unsigned)id);
		fflush(stdout);
		sRaceHost.host.OnLeft(id);
		sMatchHost.host.OnLeft(id);
	}
	void OnSessionEnded(eEndReason reason) override
	{
		printf("[ended] %s\n", reason == END_HOST_LEFT ? "host left" : "relay shutdown");
		fflush(stdout);
		exitCode = 4;
		done = true;
	}
	void OnConnectionLost(void) override
	{
		printf("[lost] connection to relay lost\n");
		fflush(stdout);
		exitCode = 5;
		done = true;
	}
	void OnChat(uint8_t from, const char *text) override
	{
		printf("[chat] %u: %s\n", (unsigned)from, text);
		fflush(stdout);
	}
	void OnGameMessage(uint8_t from, uint8_t type, CByteReader &body) override
	{
		if(type >= GMSG_RACE_FIRST && type <= GMSG_RACE_LAST){
			OnRaceMessage(from, type, body);
			return;
		}
		if(type >= GMSG_MATCH_FIRST && type <= GMSG_MATCH_LAST){
			OnMatchMessage(from, type, body);
			return;
		}
		if(type == GMSG_PLAYER_STATE){
			SPlayerState s;
			if(s.Read(body) && client->IsHost())
				sMatchHost.host.SetPosition(from, s.pos);
			return;
		}
		if(type == GMSG_RESPAWNED){
			if(client->IsHost())
				sMatchHost.host.OnRespawned(from);
			return;
		}
		if(type == GMSG_DIED){
			SDied d;
			if(d.Read(body) && client->IsHost())
				sMatchHost.host.OnDeath(from, d.killerId, client->SessionTime(sNow));
			printf("[died] %s (killer %s)\n", FakeName(from), d.killerId == MP_PLAYER_NONE ? "none" : FakeName(d.killerId));
			fflush(stdout);
			return;
		}
		printf("[game] from=%u type=%u bytes=%u\n", (unsigned)from, (unsigned)type, (unsigned)body.Remaining());
		fflush(stdout);
	}

	void OnMatchMessage(uint8_t from, uint8_t type, CByteReader &body)
	{
		bool dmBot = opts->bot && (strcmp(opts->bot, "dm") == 0 || strcmp(opts->bot, "koth") == 0 || strcmp(opts->bot, "stunt") == 0);
		switch(type){
		case GMSG_MATCH_INVITE: {
			SMatchInvite m;
			if(!m.Read(body))
				return;
			printf("[match] %s invites to %s%s%s\n", FakeName(m.startedBy), ModeName(m.mode), m.hasArena ? " at " : "",
				m.hasArena ? m.arena.name : "");
			fflush(stdout);
			sFighter.hasArena = m.hasArena;
			sFighter.arena = m.arena;
			if(dmBot){
				SMatchAccept a;
				a.matchId = m.matchId;
				CByteWriter w;
				a.Write(w);
				client->SendGame(MP_ROUTE_HOST, 0, true, w);
			}
			break;
		}
		case GMSG_MATCH_START: {
			SMatchStart m;
			if(!m.Read(body))
				return;
			bool in = false;
			sFighter.numOthers = 0;
			for(int i = 0; i < m.n; i++){
				if(m.ids[i] == client->MyId()){
					in = true;
					sFighter.slot = i;
				}else
					sFighter.others[sFighter.numOthers++] = m.ids[i];
			}
			if(in && dmBot){
				sFighter.playing = true;
				sFighter.matchId = m.matchId;
				sFighter.mode = m.mode;
				sFighter.startTime = m.startTime;
				sFighter.deaths = 0;
				sFighter.stunts = 0;
				if(sFighter.hasArena){
					sFighter.hill.Start(sFighter.arena, m.startTime);
					const SRaceGridSlot &g = sFighter.arena.grid[sFighter.slot % sFighter.arena.numGrid];
					memcpy(sFighter.pos, g.pos, sizeof(sFighter.pos));
				}
				printf("[match] fighting in match %u\n", (unsigned)m.matchId);
				fflush(stdout);
			}
			break;
		}
		case GMSG_MATCH_SCORES: {
			SMatchScores m;
			static uint32_t lastPrint;
			if(m.Read(body) && sNow - lastPrint >= 3000){
				lastPrint = sNow;
				PrintScores("scores", m, sFighter.playing ? sFighter.mode : (uint8_t)(m.entries[0].team == MP_TEAM_NONE ? MATCH_DM : MATCH_TDM));
			}
			break;
		}
		case GMSG_MATCH_END: {
			SMatchScores m;
			if(m.Read(body)){
				PrintScores("end", m, m.n && m.entries[0].team != MP_TEAM_NONE ? MATCH_TDM : MATCH_DM);
				if(m.matchId == sFighter.matchId)
					sFighter.playing = false;
			}
			break;
		}
		case GMSG_MATCH_CANCEL: {
			SMatchCancel m;
			if(m.Read(body)){
				printf("[match] cancelled (reason %u)\n", (unsigned)m.reason);
				fflush(stdout);
			}
			break;
		}
		case GMSG_MATCH_ACCEPT: {
			SMatchAccept m;
			if(m.Read(body) && client->IsHost())
				sMatchHost.host.OnAccept(from, m.matchId, client->SessionTime(sNow));
			break;
		}
		case GMSG_STUNT: {
			SStunt m;
			if(m.Read(body) && client->IsHost())
				sMatchHost.host.OnStunt(from, m, client->SessionTime(sNow));
			break;
		}
		default:
			break;
		}
	}

	void OnRaceMessage(uint8_t from, uint8_t type, CByteReader &body)
	{
		bool raceBot = opts->bot && strcmp(opts->bot, "race") == 0;
		switch(type){
		case GMSG_RACE_INVITE: {
			SRaceInvite m;
			if(!m.Read(body))
				return;
			printf("[race] %s invites to '%s'\n", FakeName(m.startedBy), m.route.name);
			fflush(stdout);
			if(raceBot){
				SRaceAccept a;
				a.raceId = m.raceId;
				CByteWriter w;
				a.Write(w);
				client->SendGame(MP_ROUTE_HOST, 0, true, w);
			}
			break;
		}
		case GMSG_RACE_START: {
			SRaceStart m;
			if(!m.Read(body))
				return;
			for(int i = 0; i < m.n; i++)
				if(m.ids[i] == client->MyId()){
					sRacer.racing = raceBot;
					sRacer.raceId = m.raceId;
					sRacer.startTime = m.startTime;
					sRacer.seq = 0;
					printf("[race] racing in race %u from grid slot %d\n", (unsigned)m.raceId, i + 1);
					fflush(stdout);
				}
			break;
		}
		case GMSG_RACE_STANDINGS: {
			SRaceStandings m;
			if(m.Read(body)){
				static uint32_t lastPrint;
				if(sNow - lastPrint >= 2000){
					lastPrint = sNow;
					PrintStandings("standings", m);
				}
			}
			break;
		}
		case GMSG_RACE_END: {
			SRaceStandings m;
			if(m.Read(body)){
				PrintStandings("end", m);
				if(m.raceId == sRacer.raceId)
					sRacer.racing = false;
			}
			break;
		}
		case GMSG_RACE_CANCEL: {
			SRaceCancel m;
			if(m.Read(body)){
				printf("[race] cancelled (reason %u)\n", (unsigned)m.reason);
				fflush(stdout);
			}
			break;
		}
		case GMSG_RACE_ACCEPT: {
			SRaceAccept m;
			if(m.Read(body) && client->IsHost())
				sRaceHost.host.OnAccept(from, m.raceId, client->SessionTime(sNow));
			break;
		}
		case GMSG_RACE_CROSS: {
			SRaceCross m;
			if(m.Read(body) && client->IsHost())
				sRaceHost.host.OnCross(from, m);
			break;
		}
		default:
			break;
		}
	}
};

// Called every tick while in a session.
typedef std::function<void(CNetClient &client, uint32_t nowMs)> FakeTickFn;

// --bot walk|drive: synthetic player (and vehicle) snapshots at MP_SNAPSHOT_HZ.
class CFakeBot
{
	const SFakeOptions &m_opts;
	uint32_t m_lastSendMs;
	uint32_t m_startMs;
	bool m_started;
	bool m_vehicleCreated;
	uint16_t m_vehicleId;
public:
	explicit CFakeBot(const SFakeOptions &opts)
		: m_opts(opts), m_lastSendMs(0), m_startMs(0), m_started(false), m_vehicleCreated(false), m_vehicleId(MP_VEHICLE_NONE) {}

	void Tick(CNetClient &client, uint32_t now)
	{
		if(!m_started){
			m_started = true;
			m_startMs = now;
			m_lastSendMs = now - 1000;
		}
		if(now - m_lastSendMs < 1000 / MP_SNAPSHOT_HZ)
			return;
		m_lastSendMs = now;
		bool drive = strcmp(m_opts.bot, "drive") == 0;
		// walk: 10 m circle at ~2 m/s; drive: 40 m circle at 15 m/s.
		float radius = drive ? 40.0f : 10.0f;
		float speed = drive ? 15.0f : 2.0f;	// m/s
		float omega = speed / radius;		// rad/s
		float a = omega * (float)(now - m_startMs) / 1000.0f;
		float pos[3] = { m_opts.at[0] + radius * cosf(a), m_opts.at[1] + radius * sinf(a), m_opts.at[2] };
		// Moving counter-clockwise: direction (-sin a, cos a). The engine's
		// forward for heading h is (-sin h, cos h), so heading = a.
		float vel[3] = { -sinf(a) * speed / 1000.0f, cosf(a) * speed / 1000.0f, 0.0f };
		float heading = fmodf(a, 6.2831853f);
		uint32_t sessionNow = client.SessionTime(now);

		if(drive){
			if(!m_vehicleCreated){
				m_vehicleId = MpMakeVehicleId(client.MyId(), 1);
				SVehicleCreate create;
				create.vehicleId = m_vehicleId;
				create.model = m_opts.model;
				create.col1 = 1;
				create.col2 = 1;
				memcpy(create.pos, pos, sizeof(pos));
				create.quat[2] = sinf(heading / 2.0f);
				create.quat[3] = cosf(heading / 2.0f);
				CByteWriter w;
				create.Write(w);
				client.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, true, w);
				m_vehicleCreated = true;
			}
			SVehicleState vs;
			vs.time = sessionNow;
			vs.vehicleId = m_vehicleId;
			memcpy(vs.pos, pos, sizeof(pos));
			vs.quat[0] = 0.0f;
			vs.quat[1] = 0.0f;
			vs.quat[2] = sinf(heading / 2.0f);
			vs.quat[3] = cosf(heading / 2.0f);
			memcpy(vs.vel, vel, sizeof(vel));
			vs.turn[2] = omega / 1000.0f;
			vs.steer = 0.2f;
			vs.gas = 0.6f;
			vs.flags = VSF_ENGINE | VSF_LIGHTS;
			vs.health = 1000;
			CByteWriter w;
			vs.Write(w);
			client.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
		}

		SPlayerState ps;
		ps.time = sessionNow;
		ps.skin = 0;
		ps.flags = drive ? PSF_IN_VEHICLE : 0;
		memcpy(ps.pos, pos, sizeof(pos));
		ps.heading = heading;
		memcpy(ps.vel, vel, sizeof(vel));
		ps.moveState = drive ? 1 : 2;	// engine PEDMOVE_STILL / PEDMOVE_WALK
		ps.weapon = 0;
		ps.health = 100;
		ps.armour = 0;
		ps.vehicleId = drive ? m_vehicleId : MP_VEHICLE_NONE;
		ps.seat = MP_SEAT_DRIVER;
		CByteWriter w;
		ps.Write(w);
		client.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
	}
};

static int
RunFakeClient(const SFakeOptions &opts, const FakeTickFn &tick)
{
	CNetClient client;
	CFakeListener listener(&opts, &client);
	sClient = &client;
	if(opts.startRace && !LoadRaceRoute(opts.startRace, sRaceHost.route))
		return 2;
	bool ok = opts.host ? client.Host(opts.relayHost, opts.relayPort, opts.name)
	                    : client.Join(opts.relayHost, opts.relayPort, opts.code, opts.name);
	if(!ok){
		fprintf(stderr, "revc-fakeclient: can't connect to %s:%u\n", opts.relayHost, (unsigned)opts.relayPort);
		return 2;
	}
	printf("[connecting] %s:%u as '%s'\n", opts.relayHost, (unsigned)opts.relayPort, opts.name);
	fflush(stdout);

	uint32_t start = NowMs();
	while(!sQuit && !listener.done){
		uint32_t now = NowMs();
		if(opts.seconds && now - start >= opts.seconds * 1000)
			break;
		sNow = now;
		client.Service(now, &listener);
		if(client.IsInSession() && tick)
			tick(client, now);
		if(client.IsInSession() && client.IsHost() && opts.startRace)
			sRaceHost.host.Update(client.SessionTime(now), &sRaceHost);
		if(client.IsInSession() && client.IsHost() && opts.startMatch >= 0)
			sMatchHost.host.Update(client.SessionTime(now), &sMatchHost);
		if(client.IsInSession() && !client.IsHost() && opts.bot && strcmp(opts.bot, "walk") != 0 && strcmp(opts.bot, "drive") != 0 &&
		   !(sFighter.playing && strcmp(opts.bot, "koth") == 0) && now - sFighter.lastStateMs >= 1000){
			// Event bots stand still, but say so once a second like a real
			// game: the host's catch-up (open invites) waits for a first state.
			sFighter.lastStateMs = now;
			SPlayerState ps;
			ps.time = client.SessionTime(now);
			memcpy(ps.pos, opts.at, sizeof(ps.pos));
			ps.health = 100;
			CByteWriter w;
			ps.Write(w);
			client.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
		}
		if(client.IsInSession() && sFighter.playing && opts.bot && strcmp(opts.bot, "koth") == 0 && sFighter.hasArena &&
		   now - sFighter.lastStateMs >= 1000 / MP_SNAPSHOT_HZ){
			// Walk at 4 m/s to the active hill (from the spawn, during the countdown too).
			float dt = sFighter.lastStateMs ? (float)(now - sFighter.lastStateMs) / 1000.0f : 0.0f;
			sFighter.lastStateMs = now;
			uint32_t sessionNow = client.SessionTime(now);
			const SRaceCheckpoint *h = sFighter.hill.Hill(sFighter.hill.ActiveHill(sessionNow));
			bool started = (int32_t)(sessionNow - sFighter.startTime) >= 0;
			if(h && started){
				float dx = h->pos[0] - sFighter.pos[0], dy = h->pos[1] - sFighter.pos[1];
				float d = sqrtf(dx * dx + dy * dy);
				float step = 4.0f * dt;
				if(d > 0.5f){
					float k = step >= d ? 1.0f : step / d;
					sFighter.pos[0] += dx * k;
					sFighter.pos[1] += dy * k;
				}
				sFighter.pos[2] = h->pos[2];
			}
			SPlayerState ps;
			ps.time = sessionNow;
			memcpy(ps.pos, sFighter.pos, sizeof(ps.pos));
			ps.moveState = 2;
			ps.health = 100;
			CByteWriter w;
			ps.Write(w);
			client.SendGame(MP_ROUTE_ALL_BUT_SELF, 0, false, w);
		}
		if(client.IsInSession() && sFighter.playing && opts.bot && strcmp(opts.bot, "stunt") == 0){
			// A 1.5 s, 30 m jump every 5 s, with a flip every other one.
			uint32_t due = sFighter.startTime + 5000u * (sFighter.stunts + 1u);
			if((int32_t)(client.SessionTime(now) - due) >= 0){
				SStunt st;
				st.matchId = sFighter.matchId;
				st.airMs = 1500;
				st.flips = (uint8_t)(sFighter.stunts % 2);
				st.points = MpStuntPoints(st.airMs, 30.0f, 4.0f, st.flips, 0);
				sFighter.stunts++;
				CByteWriter w;
				st.Write(w);
				client.SendGame(MP_ROUTE_HOST, 0, true, w);
				printf("[stunt] +%u\n", (unsigned)st.points);
				fflush(stdout);
			}
		}
		if(client.IsInSession() && sFighter.playing && sFighter.numOthers > 0 && opts.bot && strcmp(opts.bot, "dm") == 0){
			// Die every 3 s of session time, "killed" by the others in turn.
			uint32_t due = sFighter.startTime + 3000u * (sFighter.deaths + 1u);
			if((int32_t)(client.SessionTime(now) - due) >= 0){
				SDied d;
				d.killerId = sFighter.others[sFighter.deaths % sFighter.numOthers];
				d.weapon = 23;	// pistol
				sFighter.deaths++;
				CByteWriter w;
				d.Write(w);
				client.SendGame(MP_ROUTE_ALL, 0, true, w);
			}
		}
		if(client.IsInSession() && sRacer.racing){
			// One checkpoint every 4 s of session time; the host ignores
			// anything past the finish.
			uint32_t due = sRacer.startTime + 4000u * (sRacer.seq + 1u);
			if((int32_t)(client.SessionTime(now) - due) >= 0 && sRacer.seq < 600){
				SRaceCross c;
				c.raceId = sRacer.raceId;
				c.seq = sRacer.seq++;
				c.time = due;
				CByteWriter w;
				c.Write(w);
				client.SendGame(MP_ROUTE_HOST, 0, true, w);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if(client.IsActive()){
		client.Leave();
		printf("[bye]\n");
		fflush(stdout);
	}
	return listener.exitCode;
}

static void
Usage(FILE *f)
{
	fprintf(f,
		"usage: revc-fakeclient --relay HOST[:PORT] (--host | --join CODE) [--name NAME]\n"
		"                       [--say TEXT] [--seconds N] [--bot walk|drive --at X,Y,Z [--model N]]\n"
		"\n"
		"Headless reVC multiplayer client for testing.\n"
		"\n"
		"  --relay HOST[:PORT]  relay address (default port %u)\n"
		"  --host               create a session and print its invite code\n"
		"  --join CODE          join the session with this invite code\n"
		"  --name NAME          player name (default Player)\n"
		"  --say TEXT           send TEXT as chat once in the session\n"
		"  --seconds N          exit after N seconds (default: until Ctrl+C)\n"
		"  --bot walk|drive     send synthetic snapshots: walk a 10 m circle, or\n"
		"                       drive a 40 m circle at 15 m/s in a spawned vehicle\n"
		"  --at X,Y,Z           centre of the bot's circle (default 0,0,10)\n"
		"  --model N            vehicle model for --bot drive (default 132)\n"
		"  --bot race           accept every race invite and cross a checkpoint every 4 s\n"
		"  --bot dm             accept every match invite and die every 3 s, killed by the others\n"
		"  --bot koth           accept every match invite and walk to the active hill\n"
		"  --bot stunt          accept every match invite and land a stunt every 5 s\n"
		"  --start-match MODE   dm, tdm, koth, tkoth or stunt. Host: open that match when\n"
		"                       someone joins. Member: ask the host (e.g. revc-server) for it\n"
		"  --arena FILE         the arena .route file for --start-match (koth, tkoth and\n"
		"                       stunt need one)\n"
		"  --start-race ROUTE   a .route file, or a name for a built-in 3-checkpoint test\n"
		"                       sprint. Host: open the race when someone joins. Member: ask\n"
		"                       the host for it (the route travels with the request)\n"
		"\n"
		"Exit codes: 0 done, 2 bad arguments/connect error, 3 join failed,\n"
		"4 session ended, 5 connection lost.\n",
		(unsigned)MP_DEFAULT_RELAY_PORT);
}

static bool
ParseRelay(const char *arg, SFakeOptions &opts)
{
	if(arg == nullptr || arg[0] == '\0' || strlen(arg) >= sizeof(opts.relayHost))
		return false;
	strcpy(opts.relayHost, arg);
	opts.relayPort = MP_DEFAULT_RELAY_PORT;
	char *colon = strrchr(opts.relayHost, ':');
	if(colon){
		char *end;
		unsigned long port = strtoul(colon + 1, &end, 10);
		if(*end != '\0' || port == 0 || port > 65535)
			return false;
		opts.relayPort = (uint16_t)port;
		*colon = '\0';
	}
	return opts.relayHost[0] != '\0';
}

int
main(int argc, char **argv)
{
	SFakeOptions opts;
	memset(&opts, 0, sizeof(opts));
	opts.name = "Player";
	opts.startMatch = -1;
	opts.at[2] = 10.0f;
	opts.model = 132;	// TODO verify in-game
	bool haveRelay = false;

	for(int i = 1; i < argc; i++){
		const char *a = argv[i];
		const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
		if(strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0){
			Usage(stdout);
			return 0;
		}else if(strcmp(a, "--relay") == 0){
			if(!ParseRelay(next, opts)){
				fprintf(stderr, "revc-fakeclient: --relay needs HOST or HOST:PORT\n");
				return 2;
			}
			haveRelay = true;
			i++;
		}else if(strcmp(a, "--host") == 0){
			opts.host = true;
		}else if(strcmp(a, "--join") == 0 && next){
			opts.code = next;
			i++;
		}else if(strcmp(a, "--name") == 0 && next){
			opts.name = next;
			i++;
		}else if(strcmp(a, "--say") == 0 && next){
			opts.say = next;
			i++;
		}else if(strcmp(a, "--bot") == 0 && next){
			if(strcmp(next, "walk") != 0 && strcmp(next, "drive") != 0 && strcmp(next, "race") != 0 && strcmp(next, "dm") != 0 &&
			   strcmp(next, "koth") != 0 && strcmp(next, "stunt") != 0){
				fprintf(stderr, "revc-fakeclient: --bot needs walk, drive, race, dm, koth or stunt\n");
				return 2;
			}
			opts.bot = next;
			i++;
		}else if(strcmp(a, "--at") == 0 && next){
			if(sscanf(next, "%f,%f,%f", &opts.at[0], &opts.at[1], &opts.at[2]) != 3){
				fprintf(stderr, "revc-fakeclient: --at needs X,Y,Z\n");
				return 2;
			}
			i++;
		}else if(strcmp(a, "--model") == 0 && next){
			char *end;
			unsigned long m = strtoul(next, &end, 10);
			if(*end != '\0' || m == 0 || m > 65535){
				fprintf(stderr, "revc-fakeclient: --model needs a model id\n");
				return 2;
			}
			opts.model = (uint16_t)m;
			i++;
		}else if(strcmp(a, "--start-match") == 0 && next){
			static const char *names[MATCH_NUM_MODES] = { "dm", "tdm", "koth", "tkoth", "stunt" };
			for(int m = 0; m < MATCH_NUM_MODES; m++)
				if(strcmp(next, names[m]) == 0)
					opts.startMatch = m;
			if(opts.startMatch < 0){
				fprintf(stderr, "revc-fakeclient: --start-match needs dm, tdm, koth, tkoth or stunt\n");
				return 2;
			}
			i++;
		}else if(strcmp(a, "--arena") == 0 && next){
			opts.arena = next;
			i++;
		}else if(strcmp(a, "--start-race") == 0 && next){
			opts.startRace = next;
			i++;
		}else if(strcmp(a, "--seconds") == 0 && next){
			char *end;
			unsigned long s = strtoul(next, &end, 10);
			if(*end != '\0' || s == 0 || s > 86400){
				fprintf(stderr, "revc-fakeclient: --seconds needs a number 1-86400\n");
				return 2;
			}
			opts.seconds = (uint32_t)s;
			i++;
		}else{
			fprintf(stderr, "revc-fakeclient: bad argument '%s'\n\n", a);
			Usage(stderr);
			return 2;
		}
	}
	if(!haveRelay || opts.host == (opts.code != nullptr)){
		Usage(stderr);
		return 2;
	}

	if(opts.arena){
		char err[128];
		if(!MpLoadRouteFile(opts.arena, sArena, err, sizeof(err)) || sArena.type != RACE_ARENA){
			fprintf(stderr, "revc-fakeclient: --arena %s: %s\n", opts.arena, sArena.type != RACE_ARENA && err[0] == '\0' ? "not an arena" : err);
			return 2;
		}
		sHaveArena = true;
	}

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	if(opts.bot && (strcmp(opts.bot, "walk") == 0 || strcmp(opts.bot, "drive") == 0)){
		CFakeBot bot(opts);
		return RunFakeClient(opts, [&bot](CNetClient &client, uint32_t now){ bot.Tick(client, now); });
	}
	return RunFakeClient(opts, FakeTickFn());
}
