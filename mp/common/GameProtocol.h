#pragma once

// Game messages (types 64..96) carried opaquely by CNetClient::SendGame and
// INetClientListener::OnGameMessage. Engine-free so they're unit tested and
// usable by the fake client.
//
// Units: positions in game units (metres), velocities in game units per
// MILLISECOND, quats (x, y, z, w), headings in radians, times in session ms.

#include <stdint.h>
#include <string.h>

#include "MpTypes.h"
#include "ByteStream.h"
#include "Protocol.h"
#include "RaceRoute.h"
#include "MatchLogic.h"

enum eGameMsg : uint8_t
{
	GMSG_PLAYER_STATE = 64,		// unreliable, all-but-self
	GMSG_VEHICLE_STATE = 65,	// unreliable, all-but-self, from the owner
	GMSG_VEHICLE_CREATE = 66,	// reliable, all-but-self
	GMSG_VEHICLE_REMOVE = 67,	// reliable
	GMSG_VEHICLE_ENTER = 68,	// reliable -> host
	GMSG_VEHICLE_SEAT = 69,		// reliable, host -> all
	GMSG_VEHICLE_EXIT = 70,		// reliable -> host
	GMSG_DAMAGE = 71,		// reliable -> victim
	GMSG_DIED = 72,			// reliable, all
	GMSG_RESPAWNED = 73,		// reliable, all
	GMSG_SHOT = 74,			// unreliable, all-but-self
	GMSG_WORLD = 75,		// reliable, host -> all

	GMSG_RACE_FIRST = 80,		// 80..95 races
	GMSG_RACE_REQUEST = 80,		// -> host, reliable
	GMSG_RACE_INVITE = 81,		// host -> all
	GMSG_RACE_ACCEPT = 82,		// -> host
	GMSG_RACE_START = 83,		// host -> all
	GMSG_RACE_CROSS = 84,		// racer -> host, reliable
	GMSG_RACE_STANDINGS = 85,	// host -> all, 4 Hz, unreliable
	GMSG_RACE_END = 86,		// host -> all, reliable (final standings)
	GMSG_RACE_CANCEL = 87,		// host -> all or one
	GMSG_RACE_LAST = 87,

	GMSG_MATCH_FIRST = 88,		// 88..95 deathmatch / team deathmatch
	GMSG_MATCH_REQUEST = 88,	// -> host
	GMSG_MATCH_INVITE = 89,		// host -> all
	GMSG_MATCH_ACCEPT = 90,		// -> host
	GMSG_MATCH_START = 91,		// host -> all
	GMSG_MATCH_SCORES = 92,		// host -> all, 2 Hz, unreliable
	GMSG_MATCH_END = 93,		// host -> all, reliable (final scores)
	GMSG_MATCH_CANCEL = 94,		// host -> all or one
	GMSG_STUNT = 96,		// stunter -> host, reliable (Stunt Challenge)
	GMSG_MATCH_LAST = 96,
	GMSG_LAST = 96
};

static_assert((uint32_t)GMSG_PLAYER_STATE == (uint32_t)MSG_GAME_FIRST, "game messages start at MSG_GAME_FIRST");

// SPlayerState::flags
enum
{
	PSF_IN_VEHICLE = 1,
	PSF_DEAD = 2,
	PSF_AIMING = 4,
	PSF_FIRING = 8,
	PSF_CROUCH = 16,
	PSF_JUMPING = 32
};

// SVehicleState::flags
enum
{
	VSF_HANDBRAKE = 1,
	VSF_SIREN = 2,
	VSF_LIGHTS = 4,
	VSF_ENGINE = 8
};

static const uint8_t MP_SEAT_DRIVER = 0;	// 1..3 are passengers
static const uint8_t MP_MAX_SEATS = 4;
static const uint8_t MP_SEAT_NONE = 0xFF;	// "left the vehicle"
static const uint16_t MP_VEHICLE_NONE = 0;

// Vehicle ids are (owner << 12) | counter, counter 1..4095, so every player
// can mint ids without asking the host.
inline uint16_t MpMakeVehicleId(uint8_t owner, uint16_t counter) { return (uint16_t)(((owner & 0xF) << 12) | (counter & 0xFFF)); }
inline uint8_t MpVehicleIdOwner(uint16_t id) { return (uint8_t)(id >> 12); }
inline uint16_t MpVehicleIdCounter(uint16_t id) { return (uint16_t)(id & 0xFFF); }

// Who owns a vehicle after leavingPlayer leaves the session. seats[i] is the
// player in seat i (0 = driver) or MP_PLAYER_NONE. If the leaver isn't the
// owner, nothing changes. Otherwise: the remaining driver, else the
// lowest-seat remaining occupant, else MP_PLAYER_NONE (the car goes local).
uint8_t MpNextVehicleOwner(uint8_t currentOwner, const uint8_t seats[MP_MAX_SEATS], uint8_t leavingPlayer);

struct SPlayerState
{
	uint32_t time;
	uint16_t skin;		// model id; 0 = default by player id
	uint8_t flags;		// PSF_*
	float pos[3];
	float heading;		// radians
	float vel[3];		// units per ms
	uint8_t moveState;	// engine eMoveState
	uint8_t weapon;		// engine eWeaponType
	float aimPos[3];
	uint8_t health;
	uint8_t armour;
	uint16_t vehicleId;	// with PSF_IN_VEHICLE
	uint8_t seat;		// with PSF_IN_VEHICLE: 0 driver, 1..3 passengers

	SPlayerState(void);
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleState
{
	uint32_t time;
	uint16_t vehicleId;
	float pos[3];
	float quat[4];
	float vel[3];		// units per ms
	float turn[3];		// engine turn speed, converted like vel
	float steer;
	float gas;
	float brake;
	uint8_t flags;		// VSF_*
	uint16_t health;	// 0..1000

	SVehicleState(void);
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleCreate
{
	uint16_t vehicleId;
	uint16_t model;
	uint8_t col1;
	uint8_t col2;
	float pos[3];
	float quat[4];

	SVehicleCreate(void);
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleRemove
{
	uint16_t vehicleId;

	SVehicleRemove(void) : vehicleId(MP_VEHICLE_NONE) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleEnter
{
	uint16_t vehicleId;
	uint8_t seat;

	SVehicleEnter(void) : vehicleId(MP_VEHICLE_NONE), seat(MP_SEAT_DRIVER) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleSeat
{
	uint16_t vehicleId;
	uint8_t playerId;
	uint8_t seat;		// MP_SEAT_NONE = left the vehicle
	uint8_t accepted;

	SVehicleSeat(void) : vehicleId(MP_VEHICLE_NONE), playerId(MP_PLAYER_NONE), seat(MP_SEAT_NONE), accepted(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SVehicleExit
{
	uint16_t vehicleId;

	SVehicleExit(void) : vehicleId(MP_VEHICLE_NONE) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SDamage
{
	uint8_t victimId;
	uint8_t weapon;
	float amount;
	uint8_t piece;
	uint8_t direction;

	SDamage(void) : victimId(MP_PLAYER_NONE), weapon(0), amount(0.0f), piece(0), direction(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SDied
{
	uint8_t killerId;	// MP_PLAYER_NONE if none
	uint8_t weapon;

	SDied(void) : killerId(MP_PLAYER_NONE), weapon(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRespawned
{
	float pos[3];
	float heading;

	SRespawned(void);
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SShot
{
	uint8_t weapon;
	float from[3];
	float to[3];

	SShot(void);
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SWorldState
{
	uint8_t hour;
	uint8_t minute;
	uint8_t weather;

	SWorldState(void) : hour(12), minute(0), weather(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

// ---- Races (Plan 3). raceId is a u16 the host increments per race; messages
// with a stale raceId are ignored.

enum eRaceCancelReason
{
	RACE_BUSY,		// a race is already pending or running
	RACE_NO_RACERS,		// nobody accepted
	RACE_BAD_ROUTE,		// the route failed MpValidateRoute
	RACE_ABORTED		// the host's world restarted mid-race
};

// Routes travel inside requests and invites (Plan 5), so a route only has to
// exist on the requester's machine. Positions are sent in 1/8 m, headings in
// 1/65536 turn, radii in 1/2 m. Read checks the result with MpValidateRoute.
void MpWriteRouteData(CByteWriter &w, const SRaceRoute &route);
bool MpReadRouteData(CByteReader &r, SRaceRoute &route);

struct SRaceRequest
{
	SRaceRoute route;	// a race type (sprint or circuit)

	SRaceRequest(void) { memset(&route, 0, sizeof(route)); }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceInvite
{
	uint16_t raceId;
	uint8_t startedBy;	// MP_PLAYER_NONE: the dedicated server's playlist
	uint32_t deadline;	// session time; accepts after it are ignored
	SRaceRoute route;	// what's raced

	SRaceInvite(void) : raceId(0), startedBy(MP_PLAYER_NONE), deadline(0) { memset(&route, 0, sizeof(route)); }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceAccept
{
	uint16_t raceId;

	SRaceAccept(void) : raceId(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceStart
{
	uint16_t raceId;
	uint32_t startTime;	// session time of GO
	uint8_t n;
	uint8_t ids[MP_MAX_PLAYERS];	// index = grid slot

	SRaceStart(void) : raceId(0), startTime(0), n(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceCross
{
	uint16_t raceId;
	uint16_t seq;
	uint32_t time;		// session time of the crossing, racer's own clock

	SRaceCross(void) : raceId(0), seq(0), time(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceStandingEntry
{
	uint8_t id;
	uint16_t nextSeq;
	uint8_t state;		// eRacerState
	uint32_t finishTime;	// ms from the start, when finished
};

// GMSG_RACE_STANDINGS, or GMSG_RACE_END with the same body.
struct SRaceStandings
{
	uint8_t msgType;	// which of the two Write() sends
	uint16_t raceId;
	uint8_t n;
	SRaceStandingEntry entries[MP_MAX_PLAYERS];	// standings order

	SRaceStandings(void) : msgType(GMSG_RACE_STANDINGS), raceId(0), n(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SRaceCancel
{
	uint16_t raceId;
	uint8_t reason;		// eRaceCancelReason

	SRaceCancel(void) : raceId(0), reason(RACE_BUSY) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

// ---- Deathmatch / team deathmatch (Plan 4). matchId is a u16 the host
// increments; stale ids are ignored. Kills come from GMSG_DIED.

enum eMatchCancelReason
{
	MATCH_BUSY,		// a match or race is already on
	MATCH_NOT_ENOUGH,	// fewer than 2 accepted (or one team empty)
	MATCH_ABORTED,		// the host's world restarted
	MATCH_NO_ARENA		// the mode needs an arena (hills) and got none
};

struct SMatchRequest
{
	uint8_t mode;		// eMatchMode
	bool hasArena;
	SRaceRoute arena;	// type arena: spawns, hills, stunt vehicle

	SMatchRequest(void) : mode(MATCH_DM), hasArena(false) { memset(&arena, 0, sizeof(arena)); }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMatchInvite
{
	uint16_t matchId;
	uint8_t mode;
	uint8_t startedBy;
	uint16_t scoreLimit;
	uint32_t timeLimitMs;
	uint32_t deadline;	// session time
	bool hasArena;
	SRaceRoute arena;

	SMatchInvite(void) : matchId(0), mode(MATCH_DM), startedBy(MP_PLAYER_NONE), scoreLimit(0), timeLimitMs(0), deadline(0), hasArena(false)
		{ memset(&arena, 0, sizeof(arena)); }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMatchAccept
{
	uint16_t matchId;

	SMatchAccept(void) : matchId(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMatchStart
{
	uint16_t matchId;
	uint8_t mode;
	uint16_t scoreLimit;
	uint32_t timeLimitMs;
	uint32_t startTime;	// session time the fighting starts
	uint8_t n;
	uint8_t ids[MP_MAX_PLAYERS];
	uint8_t teams[MP_MAX_PLAYERS];

	SMatchStart(void) : matchId(0), mode(MATCH_DM), scoreLimit(0), timeLimitMs(0), startTime(0), n(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMatchScoreEntry
{
	uint8_t id;
	uint8_t team;
	int16_t score;
	uint16_t kills;
	uint16_t deaths;
};

// GMSG_MATCH_SCORES, or GMSG_MATCH_END with the same body.
struct SMatchScores
{
	uint8_t msgType;
	uint16_t matchId;
	int16_t teamScore[2];
	uint8_t winnerTeam;	// team DM: MP_TEAM_* or MP_TEAM_NONE for a draw (as things stand)
	uint8_t n;
	SMatchScoreEntry entries[MP_MAX_PLAYERS];	// standings order

	SMatchScores(void) : msgType(GMSG_MATCH_SCORES), matchId(0), winnerTeam(MP_TEAM_NONE), n(0) { teamScore[0] = teamScore[1] = 0; }
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

struct SMatchCancel
{
	uint16_t matchId;
	uint8_t reason;		// eMatchCancelReason

	SMatchCancel(void) : matchId(0), reason(MATCH_BUSY) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};

// GMSG_STUNT: one landed stunt, measured by the stunter's own game (trusted).
struct SStunt
{
	uint16_t matchId;
	uint16_t points;
	uint16_t airMs;
	uint8_t flips;
	uint8_t spins;

	SStunt(void) : matchId(0), points(0), airMs(0), flips(0), spins(0) {}
	void Write(CByteWriter &w) const;
	bool Read(CByteReader &r);
};
