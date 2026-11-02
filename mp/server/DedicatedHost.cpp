#include "DedicatedHost.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

SServerConfig::SServerConfig(void)
	: hour(12), minute(0), weather(0), clockRuns(true), playlistLength(0), playlistGapMs(60000),
	  playlistMinPlayers(2), verbose(true)
{
	strcpy(name, "Server");
	motd[0] = '\0';
	memset(playlist, 0, sizeof(playlist));
}

CDedicatedHost::CDedicatedHost(const SServerConfig &config)
	: m_config(config), m_port(0), m_running(false), m_now(0), m_clockStartMs(0), m_lastWorldMs(0),
	  m_wasBusy(false), m_idleSince(0), m_nextPlaylist(0), m_retryAt(0), m_sessionChanged(false)
{
	m_relay[0] = '\0';
	memset(m_present, 0, sizeof(m_present));
	memset(m_haveState, 0, sizeof(m_haveState));
	memset(m_names, 0, sizeof(m_names));
	memset(m_pos, 0, sizeof(m_pos));
}

void
CDedicatedHost::Log(const char *fmt, ...)
{
	if(!m_config.verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	printf("[server] ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

bool
CDedicatedHost::Start(const char *relayHost, uint16_t port, uint32_t nowMs)
{
	m_now = nowMs;
	strncpy(m_relay, relayHost, sizeof(m_relay) - 1);
	m_relay[sizeof(m_relay) - 1] = '\0';
	m_port = port;
	m_running = true;
	m_retryAt = 0;
	m_clockStartMs = nowMs;
	return m_client.Host(m_relay, m_port, m_config.name, MP_SESSION_DEDICATED);
}

void
CDedicatedHost::Stop(void)
{
	m_running = false;
	m_client.Leave();
}

bool
CDedicatedHost::TakeSessionChanged(void)
{
	bool changed = m_sessionChanged;
	m_sessionChanged = false;
	return changed;
}

int
CDedicatedHost::NumPlayers(void) const
{
	int n = 0;
	for(int i = 1; i < (int)MP_MAX_PLAYERS; i++)
		if(m_present[i])
			n++;
	return n;
}

// ---- World

uint8_t
CDedicatedHost::Hour(void) const
{
	uint32_t minutes = m_config.hour * 60u + m_config.minute;
	if(m_config.clockRuns)
		minutes += (m_now - m_clockStartMs) / 1000;
	return (uint8_t)(minutes / 60 % 24);
}

uint8_t
CDedicatedHost::Minute(void) const
{
	uint32_t minutes = m_config.hour * 60u + m_config.minute;
	if(m_config.clockRuns)
		minutes += (m_now - m_clockStartMs) / 1000;
	return (uint8_t)(minutes % 60);
}

void
CDedicatedHost::Send(uint8_t route, uint8_t target, bool reliable, const CByteWriter &w)
{
	m_client.SendGame(route, target, reliable, w);
}

void
CDedicatedHost::SendWorld(uint8_t target)
{
	SWorldState msg;
	msg.hour = Hour();
	msg.minute = Minute();
	msg.weather = m_config.weather;
	CByteWriter w;
	msg.Write(w);
	if(target == MP_PLAYER_NONE)
		Send(MP_ROUTE_ALL, 0, true, w);
	else
		Send(MP_ROUTE_PLAYER, target, true, w);
}

// A player's world is up (their first PLAYER_STATE): what we sent while they
// were loading was dropped, so send the world and any open invite now.
void
CDedicatedHost::CatchUp(uint8_t id)
{
	SendWorld(id);
	SRaceInvite raceInvite;
	if(m_race.GetInvite(raceInvite)){
		CByteWriter w;
		raceInvite.Write(w);
		Send(MP_ROUTE_PLAYER, id, true, w);
	}
	SMatchInvite matchInvite;
	if(m_match.GetInvite(matchInvite)){
		CByteWriter w;
		matchInvite.Write(w);
		Send(MP_ROUTE_PLAYER, id, true, w);
	}
}

// ---- Session events

void
CDedicatedHost::OnSessionReady(const char *code, uint8_t myId, bool isHost)
{
	(void)isHost;
	Log("session %s is up (we're id %u). Players join with that code.", code, (unsigned)myId);
	m_sessionChanged = true;
	memset(m_present, 0, sizeof(m_present));
	memset(m_haveState, 0, sizeof(m_haveState));
	m_race.Reset();
	m_match.Reset();
	m_seats.Clear();
	m_lastWorldMs = m_now;
	m_idleSince = m_now;
	m_wasBusy = false;
}

void
CDedicatedHost::OnJoinFailed(eJoinResult result)
{
	Log("the relay refused the session (%d); trying again in %u s", (int)result, (unsigned)(RECONNECT_MS / 1000));
	m_retryAt = m_now + RECONNECT_MS;
}

void
CDedicatedHost::OnPlayerJoined(uint8_t id, const char *name)
{
	if(id >= MP_MAX_PLAYERS || id == m_client.MyId())
		return;
	m_present[id] = true;
	m_haveState[id] = false;
	strncpy(m_names[id], name, MP_MAX_NAME_LEN);
	m_names[id][MP_MAX_NAME_LEN] = '\0';
	Log("%s joined as %u (%d players)", m_names[id], (unsigned)id, NumPlayers());
}

void
CDedicatedHost::OnPlayerLeft(uint8_t id)
{
	if(id >= MP_MAX_PLAYERS)
		return;
	if(m_present[id])
		Log("%s left (%d players)", m_names[id], NumPlayers() - 1);
	m_present[id] = false;
	m_haveState[id] = false;
	m_race.OnLeft(id);
	m_match.OnLeft(id);
	m_seats.OnExit(id, MP_VEHICLE_NONE);
}

void
CDedicatedHost::OnConnectionLost(void)
{
	Log("lost the relay; reconnecting in %u s", (unsigned)(RECONNECT_MS / 1000));
	m_retryAt = m_now + RECONNECT_MS;
}

void
CDedicatedHost::OnSessionEnded(eEndReason reason)
{
	Log("the relay ended the session (%s); reconnecting in %u s",
		reason == END_RELAY_SHUTDOWN ? "relay shutting down" : "host left", (unsigned)(RECONNECT_MS / 1000));
	m_retryAt = m_now + RECONNECT_MS;
}

void
CDedicatedHost::OnChat(uint8_t from, const char *text)
{
	if(from < MP_MAX_PLAYERS && from != m_client.MyId())
		Log("<%s> %s", m_names[from], text);
}

// ---- Game messages

void
CDedicatedHost::OnGameMessage(uint8_t from, uint8_t type, CByteReader &body)
{
	if(from >= MP_MAX_PLAYERS || from == m_client.MyId())
		return;
	uint32_t now = SessionNow();
	switch(type){
	case GMSG_PLAYER_STATE: {
		SPlayerState s;
		if(!s.Read(body))
			return;
		memcpy(m_pos[from], s.pos, sizeof(m_pos[from]));
		m_match.SetPosition(from, s.pos);
		if(!m_haveState[from]){
			m_haveState[from] = true;
			CatchUp(from);
			if(m_config.motd[0]){
				char line[MP_MAX_CHAT_LEN + 1];
				snprintf(line, sizeof(line), "Welcome %s! %.90s", m_names[from], m_config.motd);
				m_client.SendChat(line);
			}
		}
		break;
	}
	case GMSG_VEHICLE_CREATE: {
		SVehicleCreate m;
		if(m.Read(body))
			m_seats.OnCreate(m.vehicleId);
		break;
	}
	case GMSG_VEHICLE_REMOVE: {
		SVehicleRemove m;
		if(m.Read(body))
			m_seats.OnRemove(m.vehicleId);
		break;
	}
	case GMSG_VEHICLE_ENTER: {
		SVehicleEnter m;
		if(!m.Read(body))
			return;
		SVehicleSeat seat;
		seat.vehicleId = m.vehicleId;
		seat.playerId = from;
		seat.seat = m.seat;
		seat.accepted = m_seats.OnEnter(from, m.vehicleId, m.seat) ? 1 : 0;
		CByteWriter w;
		seat.Write(w);
		if(seat.accepted)
			Send(MP_ROUTE_ALL, 0, true, w);
		else
			Send(MP_ROUTE_PLAYER, from, true, w);
		break;
	}
	case GMSG_VEHICLE_EXIT: {
		SVehicleExit m;
		if(!m.Read(body))
			return;
		m_seats.OnExit(from, m.vehicleId);
		SVehicleSeat seat;
		seat.vehicleId = m.vehicleId;
		seat.playerId = from;
		seat.seat = MP_SEAT_NONE;
		seat.accepted = 1;
		CByteWriter w;
		seat.Write(w);
		Send(MP_ROUTE_ALL, 0, true, w);
		break;
	}
	case GMSG_DIED: {
		SDied m;
		if(m.Read(body))
			m_match.OnDeath(from, m.killerId, now);
		break;
	}
	case GMSG_RESPAWNED:
		m_match.OnRespawned(from);
		break;
	case GMSG_RACE_REQUEST: {
		SRaceRequest m;
		if(!m.Read(body)){
			SRaceCancel bad;
			bad.raceId = m_race.RaceId();
			bad.reason = RACE_BAD_ROUTE;
			SendCancel(bad, from);
			return;
		}
		if(m_match.IsBusy()){
			SRaceCancel busy;
			busy.raceId = m_race.RaceId();
			busy.reason = RACE_BUSY;
			SendCancel(busy, from);
			return;
		}
		if(m_race.Request(from, m.route, true, now, this))
			Log("%s started a race: %s", m_names[from], m.route.name);
		break;
	}
	case GMSG_RACE_ACCEPT: {
		SRaceAccept m;
		if(m.Read(body))
			m_race.OnAccept(from, m.raceId, now);
		break;
	}
	case GMSG_RACE_CROSS: {
		SRaceCross m;
		if(m.Read(body))
			m_race.OnCross(from, m);
		break;
	}
	case GMSG_MATCH_REQUEST: {
		SMatchRequest m;
		if(!m.Read(body))
			return;
		if(m_race.IsBusy()){
			SMatchCancel busy;
			busy.matchId = m_match.MatchId();
			busy.reason = MATCH_BUSY;
			SendCancel(busy, from);
			return;
		}
		if(m_match.Request(from, m.mode, true, now, this, m.hasArena ? &m.arena : nullptr))
			Log("%s started a match (mode %u%s%s)", m_names[from], (unsigned)m.mode, m.hasArena ? ", arena " : "",
				m.hasArena ? m.arena.name : "");
		break;
	}
	case GMSG_MATCH_ACCEPT: {
		SMatchAccept m;
		if(m.Read(body))
			m_match.OnAccept(from, m.matchId, now);
		break;
	}
	case GMSG_STUNT: {
		SStunt m;
		if(m.Read(body))
			m_match.OnStunt(from, m, now);
		break;
	}
	default:
		break;
	}
}

// ---- Sinks: everything goes to everyone, reliably, except standings/scores.

void CDedicatedHost::SendInvite(const SRaceInvite &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
void CDedicatedHost::SendStart(const SRaceStart &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
void CDedicatedHost::SendStandings(const SRaceStandings &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, false, w); }
void CDedicatedHost::SendEnd(const SRaceStandings &m)
{
	CByteWriter w;
	m.Write(w);
	Send(MP_ROUTE_ALL, 0, true, w);
	if(m.n > 0)
		Log("race over: %s first", m_names[m.entries[0].id < MP_MAX_PLAYERS ? m.entries[0].id : 0]);
}
void
CDedicatedHost::SendCancel(const SRaceCancel &m, uint8_t target)
{
	CByteWriter w;
	m.Write(w);
	if(target == MP_PLAYER_NONE)
		Send(MP_ROUTE_ALL, 0, true, w);
	else if(target != m_client.MyId())
		Send(MP_ROUTE_PLAYER, target, true, w);
}
void CDedicatedHost::SendInvite(const SMatchInvite &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
void CDedicatedHost::SendStart(const SMatchStart &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, true, w); }
void CDedicatedHost::SendScores(const SMatchScores &m) { CByteWriter w; m.Write(w); Send(MP_ROUTE_ALL, 0, false, w); }
void
CDedicatedHost::SendEnd(const SMatchScores &m)
{
	CByteWriter w;
	m.Write(w);
	Send(MP_ROUTE_ALL, 0, true, w);
	if(m.n > 0)
		Log("match over: %s leads with %d", m_names[m.entries[0].id < MP_MAX_PLAYERS ? m.entries[0].id : 0], (int)m.entries[0].score);
}
void
CDedicatedHost::SendCancel(const SMatchCancel &m, uint8_t target)
{
	CByteWriter w;
	m.Write(w);
	if(target == MP_PLAYER_NONE)
		Send(MP_ROUTE_ALL, 0, true, w);
	else if(target != m_client.MyId())
		Send(MP_ROUTE_PLAYER, target, true, w);
}

// ---- Per tick

void
CDedicatedHost::UpdateRaceDistances(void)
{
	if(!m_race.IsRacing())
		return;
	const CRaceState &race = m_race.Race();
	const SRaceRoute &r = race.Route();
	if(r.numCheckpoints == 0)
		return;
	for(int id = 0; id < (int)MP_MAX_PLAYERS; id++){
		const SRacer *racer = race.GetRacer((uint8_t)id);
		if(racer == nullptr || racer->state != RACER_RACING || !m_haveState[id])
			continue;
		const SRaceCheckpoint &cp = r.cps[racer->nextSeq % r.numCheckpoints];
		float dx = m_pos[id][0] - cp.pos[0], dy = m_pos[id][1] - cp.pos[1], dz = m_pos[id][2] - cp.pos[2];
		m_race.SetDistance((uint8_t)id, sqrtf(dx * dx + dy * dy + dz * dz));
	}
}

void
CDedicatedHost::UpdatePlaylist(void)
{
	bool busy = Busy();
	if(m_wasBusy && !busy)
		m_idleSince = m_now;
	m_wasBusy = busy;
	if(busy || m_config.playlistLength == 0 || NumPlayers() < m_config.playlistMinPlayers)
		return;
	if(m_now - m_idleSince < m_config.playlistGapMs)
		return;
	const SPlaylistEntry &e = m_config.playlist[m_nextPlaylist % m_config.playlistLength];
	m_nextPlaylist = (m_nextPlaylist + 1) % m_config.playlistLength;
	uint32_t now = SessionNow();
	uint8_t me = m_client.MyId();
	bool started;
	if(e.race){
		started = m_race.Request(me, e.route, false, now, this);
		Log("playlist: race %s", e.route.name);
	}else{
		started = m_match.Request(me, e.mode, false, now, this, e.hasRoute ? &e.route : nullptr);
		Log("playlist: match mode %u%s%s", (unsigned)e.mode, e.hasRoute ? " at " : "", e.hasRoute ? e.route.name : "");
	}
	m_idleSince = m_now;	// a refused entry waits a full gap before the next
	m_wasBusy = started;
}

void
CDedicatedHost::Service(uint32_t nowMs)
{
	m_now = nowMs;
	if(!m_running)
		return;
	if(m_retryAt && (int32_t)(nowMs - m_retryAt) >= 0){
		m_retryAt = 0;
		m_client.Leave();
		if(!m_client.Host(m_relay, m_port, m_config.name, MP_SESSION_DEDICATED))
			m_retryAt = nowMs + RECONNECT_MS;
	}
	// The host's clock is the session clock: pass our own milliseconds.
	m_client.Service(nowMs, this);
	if(!m_client.IsInSession())
		return;
	uint32_t now = SessionNow();
	if(nowMs - m_lastWorldMs >= WORLD_INTERVAL_MS){
		m_lastWorldMs = nowMs;
		SendWorld(MP_PLAYER_NONE);
	}
	UpdateRaceDistances();
	m_race.Update(now, this);
	m_match.Update(now, this);
	UpdatePlaylist();
}
