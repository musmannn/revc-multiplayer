# Multiplayer Plan 1: Network Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An engine-free networking layer (protocol, invite codes, session clock, pose interpolation, client), a standalone invite-code relay server, and a headless fake client, all tested without GTA Vice City installed.

**Architecture:** Every player connects only to the relay over ENet (reliable UDP). The relay groups connections into sessions keyed by invite code, assigns player ids (host = 0), and forwards packets by route (to host, to everyone, to one player). The game is the authority; the relay has no game logic. Shared code lives in `mp/common` as a static library with no reVC dependencies, so the game (Plan 2), relay, fake client and tests all link the same code.

**Tech Stack:** C++11 for `mp/common` (the game builds as C++11), C++11 for relay/tools/tests too; ENet 1.3.18 (vendored); doctest 2.4.11 (vendored single header); CMake ≥ 3.14.

**Spec:** `docs/superpowers/specs/2026-10-31-multiplayer-design.md`. Context hub: `docs/multiplayer/README.md`.

**Series:** Plan 1 (this) → Plan 2 free roam (`2026-10-31-mp-2-free-roam.md`) → Plan 3 races (`2026-10-31-mp-3-races.md`).

## Global Constraints

- Sessions hold up to **8 players**, host included.
- Joining uses an **invite code** shown as `VICE-XXXX`; no port forwarding for players; no IP sharing.
- **A player hosts**; the host's game is authoritative. The relay only routes packets.
- **macOS, Linux and Windows** are all first-class: everything in `mp/` builds and its tests pass on all three, and CI proves it (Task 8).
- The relay runs on **a Linux VPS or the owner's own PC** (macOS, Linux or Windows). Default port **UDP 7777**.
- Target: no visible stutter at **≤150 ms ping**.
- Single player must be untouched: nothing in `src/` changes in this plan.
- `mp/common` must not include any reVC header and must compile as C++11 with GCC, Clang and MSVC.
- Style: follow `CODING_STYLE.md` (tabs, `CName` classes, `SName` plain structs, `eName` enums).

## Review Focus

1. **Malformed or truncated packets** (garbage bytes, a header with no body, overlong strings). Expect the reader to report failure, the relay to drop the packet, and nothing to crash. Tested in Task 2 and Task 6.
2. **Host disconnects or crashes mid-session.** Every member gets `SessionEnded`, the code stops working, and the relay frees the slot. Tested in Task 6.
3. **Bad joins:** an unknown code, a full session (a 9th player), a protocol version mismatch, or an empty or overlong name. Each gets a specific `eJoinResult`, not a hang. Tested in Task 6.
4. **Sloppy typed codes:** `vice-ab7k`, ` AB7K `, `ab7k` normalize to the same code. Lookalikes (`0`, `O`, `1`, `I`, `L`) are rejected cleanly, never matched to a different session. Tested in Task 3.
5. **Lost, duplicated or reordered snapshots.** The pose buffer keeps time order, ignores stale samples, and caps extrapolation at 250 ms instead of flying off. Tested in Task 4.

---

## File Structure

```
vendor/enet/                     ENet 1.3.18 source (unmodified) + LICENSE
vendor/doctest/doctest.h         doctest 2.4.11
mp/CMakeLists.txt                builds enet, mpcommon, revc-relay, revc-fakeclient, mp_tests; usable standalone
mp/common/MpTypes.h              constants, enums shared by everything
mp/common/ByteStream.h/.cpp      CByteWriter / CByteReader (little-endian, bounds-checked)
mp/common/Protocol.h/.cpp        envelope + every message struct with Write/Read
mp/common/InviteCode.h/.cpp      code generation + normalization
mp/common/SessionClock.h/.cpp    host-time estimate from ping samples
mp/common/PoseBuffer.h/.cpp      snapshot interpolation/extrapolation
mp/common/NetClient.h/.cpp       ENet client wrapper + session state machine (host/join/roster/chat/ping)
mp/relay/RelayServer.h/.cpp      CRelayServer (library code, used by main and tests)
mp/relay/main.cpp                revc-relay executable
mp/tools/fakeclient.cpp          revc-fakeclient executable
mp/tests/*.cpp                   doctest suites
docs/multiplayer/relay-setup.md  running the relay on a VPS and on a home PC
CMakeLists.txt                   add option REVC_MULTIPLAYER + add_subdirectory(mp)
```

---

### Task 1: Vendor ENet and doctest, scaffold `mp/` build

**Files:**
- Create: `vendor/enet/` (copy of https://github.com/lsalzman/enet tag `v1.3.18`: `*.c`, `include/`, `LICENSE`)
- Create: `vendor/doctest/doctest.h` (https://github.com/doctest/doctest tag `v2.4.11`, `doctest/doctest.h`)
- Create: `mp/CMakeLists.txt`, `mp/common/MpTypes.h`, `mp/tests/test_main.cpp`
- Modify: `CMakeLists.txt` (top level, before `add_subdirectory(src)`)

**Interfaces:**
- Produces CMake targets: `enet` (static, `vendor/enet/include` public), `mpcommon` (static, `mp/common` public, links `enet`; on Windows also `ws2_32 winmm`), `mp_tests` (registered with `add_test(NAME mp_tests ...)`).
- Produces `mp/common/MpTypes.h`:
  - `MP_PROTOCOL_VERSION = 1`, `MP_MAX_PLAYERS = 8`, `MP_DEFAULT_RELAY_PORT = 7777`, `MP_SNAPSHOT_HZ = 20`, `MP_INTERP_DELAY_MS = 100`, `MP_MAX_EXTRAPOLATE_MS = 250`, `MP_MAX_NAME_LEN = 23`, `MP_MAX_CHAT_LEN = 127`, `MP_MAX_PACKET = 1200`, `MP_INVITE_CODE_LEN = 9` (`VICE-XXXX`).
  - `enum eMpChannel { MP_CHANNEL_RELIABLE = 0, MP_CHANNEL_SNAPSHOT = 1, MP_NUM_CHANNELS = 2 };`
  - `enum eMpRoute { MP_ROUTE_RELAY = 0, MP_ROUTE_HOST = 1, MP_ROUTE_ALL = 2, MP_ROUTE_ALL_BUT_SELF = 3, MP_ROUTE_PLAYER = 4 };`
  - `MP_PLAYER_NONE = 0xFF`, `MP_PLAYER_RELAY = 0xFE`.

- [ ] **Step 1: Vendor the libraries.** Download the two tags above into a fresh empty directory under the scratchpad, inspect that they're the expected files, then copy them into `vendor/enet/` and `vendor/doctest/`. Don't modify their sources.
- [ ] **Step 2: Write `mp/CMakeLists.txt`.** It works both as `add_subdirectory(mp)` and standalone (`cmake -S mp -B build-mp`). Guard `cmake_minimum_required`/`project(revc_mp C CXX)` with `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)`. Use `CXX_STANDARD 11`. Add `enable_testing()` when standalone.
- [ ] **Step 3: Write `mp/tests/test_main.cpp`** with `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and one smoke test `TEST_CASE("constants") { CHECK(MP_MAX_PLAYERS == 8); CHECK(MP_DEFAULT_RELAY_PORT == 7777); }`.
- [ ] **Step 4: Top-level CMake.** Add `option(${PROJECT}_MULTIPLAYER "Build multiplayer (relay, tools, game integration)" ON)` and `if(${PROJECT}_MULTIPLAYER) add_subdirectory(mp) endif()` before `add_subdirectory(src)`.
- [ ] **Step 5: Build and run.**
  Run: `cmake -S mp -B build-mp -G Ninja && cmake --build build-mp && ctest --test-dir build-mp --output-on-failure`
  Expected: `100% tests passed`.
- [ ] **Step 6: Commit.** `git add vendor/enet vendor/doctest mp CMakeLists.txt && git commit -m "mp: vendor ENet and doctest, scaffold mp build"`. Add `build-mp/` to `.gitignore` in the same commit.

---

### Task 2: Byte streams and protocol messages

**Files:**
- Create: `mp/common/ByteStream.h/.cpp`, `mp/common/Protocol.h/.cpp`
- Test: `mp/tests/test_bytestream.cpp`, `mp/tests/test_protocol.cpp`

**Interfaces:**
- Produces `class CByteWriter`: `void WriteU8(uint8_t)`, `WriteU16`, `WriteU32`, `WriteI16`, `WriteI32`, `WriteF32(float)`, `void WriteString(const char *s, uint32_t maxLen)` (u8 length prefix, truncates at maxLen), `void WriteBytes(const void*, uint32_t)`, `const uint8_t *Data() const`, `uint32_t Size() const`, `bool Overflowed() const`. Fixed internal buffer of `MP_MAX_PACKET` bytes; writes past it set `Overflowed`.
- Produces `class CByteReader(const uint8_t *data, uint32_t size)`: `bool ReadU8(uint8_t&)` etc. matching the writer, `bool ReadString(char *out, uint32_t outSize)` (always NUL-terminates; fails if the length prefix exceeds the remaining bytes or `outSize-1`), `uint32_t Remaining() const`, `bool Ok() const` (false after any failed read; later reads keep failing).
- Produces `struct SEnvelope { uint8_t route; uint8_t player; }` with `void Write(CByteWriter&) const; bool Read(CByteReader&);`. Uplink: `player` = target (for `MP_ROUTE_PLAYER`). Downlink: `player` = sender id (or `MP_PLAYER_RELAY`).
- Produces `enum eMsgType : uint8_t` and one struct per message, each with `Write(CByteWriter&) const` that writes the type byte first, plus `bool Read(CByteReader&)` reading the fields after the type byte:
  - Relay control: `MSG_CREATE_SESSION {u16 version; char name[24]}`, `MSG_SESSION_CREATED {char code[10]; u8 playerId}`, `MSG_JOIN_SESSION {u16 version; char code[10]; char name[24]}`, `MSG_JOIN_RESULT {u8 result (eJoinResult); u8 playerId}`, `MSG_PLAYER_JOINED {u8 playerId; char name[24]}`, `MSG_PLAYER_LEFT {u8 playerId; u8 reason}`, `MSG_SESSION_ENDED {u8 reason}`.
  - Session: `MSG_CHAT {char text[128]}`, `MSG_PING {u32 clientSendMs}`, `MSG_PONG {u32 clientSendMs; u32 hostMs}`.
  - Game payloads for Plans 2–3 are opaque here: `MSG_GAME_FIRST = 64`. Types ≥ 64 pass through `NetClient` to the game unparsed.
- Produces `enum eJoinResult { JOIN_OK, JOIN_NOT_FOUND, JOIN_FULL, JOIN_VERSION_MISMATCH, JOIN_BAD_NAME }` and `enum eEndReason { END_HOST_LEFT, END_RELAY_SHUTDOWN }`.
- Produces `bool MpSanitizeName(const char *in, char out[MP_MAX_NAME_LEN+1])`: trims, keeps printable ASCII 0x20–0x7E, returns false if the result is empty.

- [ ] **Step 1: Write failing tests.**
  - `TEST_CASE("bytestream round trip")`: write u8 0xAB, u16 0xBEEF, u32 0xDEADBEEF, i32 -5, f32 1.5f, string "hello". Read them back with equal values, then `Remaining()==0`.
  - `TEST_CASE("bytestream little endian")`: `WriteU32(0x01020304)` gives `Data()` bytes `04 03 02 01`.
  - `TEST_CASE("reader truncated fails safely")`: a 3-byte buffer; `ReadU32` returns false, `Ok()==false`, and a following `ReadU8` also returns false.
  - `TEST_CASE("string length prefix beyond buffer")`: bytes `{10,'a','b'}`; `ReadString` returns false and the output is `""`.
  - `TEST_CASE("writer overflow flagged")`: write 1300 bytes; `Overflowed()==true` and `Size()<=MP_MAX_PACKET`.
  - `TEST_CASE("protocol round trip")`: for each message struct, fill fields, write, read back with type byte check, and check fields equal.
  - `TEST_CASE("protocol rejects truncated body")`: write `MSG_JOIN_SESSION`, chop the last byte, and `Read` returns false.
  - `TEST_CASE("sanitize name")`: `"  Tommy "`→`"Tommy"`; `"\x01\x02"`→false; a 40-char name truncates to 23.
- [ ] **Step 2: Run, expect compile failure.** `cmake --build build-mp` fails because `ByteStream.h` doesn't exist.
- [ ] **Step 3: Implement** `ByteStream` and `Protocol` per the interfaces. Encode floats with `memcpy` to a `uint32_t`.
- [ ] **Step 4: Run, expect pass.** `cmake --build build-mp && ctest --test-dir build-mp --output-on-failure` passes 100%.
- [ ] **Step 5: Commit.** `git commit -m "mp: byte streams and protocol messages"`

---

### Task 3: Invite codes

**Files:**
- Create: `mp/common/InviteCode.h/.cpp`
- Test: `mp/tests/test_invitecode.cpp`

**Interfaces:**
- Produces `void MpMakeInviteCode(uint32_t random, char out[MP_INVITE_CODE_LEN+1])` → `"VICE-"` + 4 chars from alphabet `"ABCDEFGHJKMNPQRSTUVWXYZ23456789"` (31 chars, no `0 O 1 I L`).
- Produces `bool MpNormalizeInviteCode(const char *in, char out[MP_INVITE_CODE_LEN+1])`. Rule (exact): uppercase everything; remove spaces and `-`; if the result starts with `VICE`, drop those 4 chars; what's left must be exactly 4 chars, all in the alphabet. Otherwise return false. Lookalikes (`0 O 1 I L`) are never generated, so typing one gives a clear "invalid code" rather than joining the wrong session. On success, `out` = `"VICE-"` + the 4 chars.

- [ ] **Step 1: Write failing tests.**
  - `make code format`: `MpMakeInviteCode(0, c)` gives a string of length 9 that starts with `"VICE-"`, and all 4 tail chars are in the alphabet; 1000 different seeds yield ≥ 990 distinct codes.
  - `normalize accepts sloppy input`: `"vice-ab7k"`, `" AB7K "`, `"ab7k"`, `"VICEAB7K"`, `"vice - ab7k"` all → `"VICE-AB7K"`.
  - `normalize rejects lookalikes and junk`: `"VICE-AB0K"`, `"VICE-ABIK"`, `"VICE-AB7"`, `"VICE-AB7KK"`, `""` → false.
- [ ] **Step 2: Run, expect fail** (undefined symbols).
- [ ] **Step 3: Implement.** Generation maps `random` through a 32-bit mix (`x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; x ^= x>>16`), then takes base-31 digits.
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit.** `git commit -m "mp: invite code generation and normalization"`

---

### Task 4: Session clock and pose buffer

**Files:**
- Create: `mp/common/SessionClock.h/.cpp`, `mp/common/PoseBuffer.h/.cpp`
- Test: `mp/tests/test_sessionclock.cpp`, `mp/tests/test_posebuffer.cpp`

**Interfaces:**
- Produces `class CSessionClock`: `void Reset()`, `void SetIsHost()` (session time = local time), `void AddSample(uint32_t localSendMs, uint32_t hostMs, uint32_t localRecvMs)`, `bool IsSynced() const` (≥ 3 samples or host), `uint32_t ToSession(uint32_t localMs) const`, `uint32_t RttMs() const`. Keep the last 16 samples. Offset = `hostMs + rtt/2 - localRecvMs` from the sample with the lowest RTT.
- Produces `struct SPose { uint32_t time; float pos[3]; float quat[4]; float vel[3]; }` (`quat` is x,y,z,w) and `class CPoseBuffer`: `void Clear()`, `void Push(const SPose&)` (ignores a sample whose `time` ≤ the newest stored; ring of 16), `bool Sample(uint32_t renderTime, SPose &out) const`.
  - Between two samples: lerp `pos`/`vel`, nlerp `quat` (taking the shortest path).
  - After the newest: extrapolate `pos += vel*dt` with `dt` capped at `MP_MAX_EXTRAPOLATE_MS`, then hold.
  - Before the oldest: return the oldest. With no samples: return false.

- [ ] **Step 1: Write failing tests.**
  - `clock host identity`: after `SetIsHost`, `ToSession(1234)==1234` and `IsSynced()`.
  - `clock offset`: samples `(1000, 50100, 1100)`, `(2000, 51080, 2060)`, `(3000, 52200, 3300)`. The best has RTT 60, so offset = 51080+30−2060 = 49050, and `ToSession(4000)==53050`.
  - `clock unsynced`: with 2 samples, `IsSynced()==false`.
  - `pose interpolate`: push t=0 pos(0,0,0) and t=100 pos(10,0,0); `Sample(50)` gives pos.x == 5 within 1e-4.
  - `pose extrapolate capped`: push one sample at t=0, pos 0, vel.x = 0.1 (units/ms). `Sample(100)` gives x≈10; `Sample(1000)` gives x≈25 (capped at 250 ms).
  - `pose ignores stale`: push t=100 then t=50, and the buffer still uses t=100 as the newest.
  - `pose quat shortest path`: q0=(0,0,0,1), q1=(0,0,0,-1) (same rotation). The midpoint is a unit quat equal to ±q0.
- [ ] **Step 2: Run, expect fail.**
- [ ] **Step 3: Implement.** Velocity units are game units per **millisecond**; document this in `PoseBuffer.h`.
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit.** `git commit -m "mp: session clock and pose interpolation buffer"`

---

### Task 5: Relay server

**Files:**
- Create: `mp/relay/RelayServer.h/.cpp`, `mp/relay/main.cpp`
- Modify: `mp/CMakeLists.txt` (`relaycore` static lib from `RelayServer.cpp`; `revc-relay` exe from `main.cpp`)
- Test: covered by Task 6's integration tests (same process)

**Interfaces:**
- Consumes: `Protocol.h` messages, `MpMakeInviteCode`, `MpNormalizeInviteCode`, `MpSanitizeName`.
- Produces `struct SRelayConfig { uint16_t port = 7777; uint32_t maxSessions = 256; uint32_t maxPacketsPerSecond = 200; const char *bindAddress = nullptr; }` and `class CRelayServer`: `bool Start(const SRelayConfig&)`, `void Service(uint32_t timeoutMs)`, `void Stop()` (sends `MSG_SESSION_ENDED{END_RELAY_SHUTDOWN}` to everyone), `uint16_t Port() const` (the bound port; ENet can't reliably bind port 0, so tests try ports 27700–27799 until `Start` succeeds), `uint32_t NumSessions() const`, `uint32_t NumPeers() const`.
- Behaviour (exact):
  - A peer's first message must be `MSG_CREATE_SESSION` or `MSG_JOIN_SESSION`. Anything else gets the peer disconnected.
  - **Create:** check the version, sanitize the name, generate a code (retrying on collision, seeded from `enet_time_get()` ^ a counter ^ the peer address), and reply `MSG_SESSION_CREATED{code, 0}`.
  - **Join:** check the version (`JOIN_VERSION_MISMATCH`), normalize the code (`JOIN_NOT_FOUND`), name (`JOIN_BAD_NAME`), capacity (`JOIN_FULL`). Assign the lowest free id 1..7. If the name is already taken, append ` (2)`, ` (3)`…. Reply `MSG_JOIN_RESULT`. Send the joiner a `MSG_PLAYER_JOINED` for every existing member, then broadcast `MSG_PLAYER_JOINED` for the joiner to the others.
  - **Forwarding:** an envelope with route `HOST`/`ALL`/`ALL_BUT_SELF`/`PLAYER` is re-sent with the downlink envelope `{route, senderId}` and the same payload bytes, on the same channel with the same reliability flag. An unknown target is dropped.
  - **Disconnect of a member:** broadcast `MSG_PLAYER_LEFT`. **Disconnect of the host:** send `MSG_SESSION_ENDED{END_HOST_LEFT}` to all members, disconnect them, and free the code.
  - Drop packets > `MP_MAX_PACKET`, malformed envelopes, and anything over `maxPacketsPerSecond` per peer (counted per 1 s window). Log a one-line warning, rate-limited to 1 per peer per 10 s.
  - Logging: one line per create, join, leave and end, to stdout with a timestamp.
- `revc-relay` CLI: `revc-relay [--port N] [--bind ADDR] [--max-sessions N]`. Runs until SIGINT/SIGTERM (Ctrl+C on Windows), then `Stop()`. `--help` prints usage.

- [ ] **Step 1: Implement `CRelayServer` and `main.cpp`** per the behaviour above (Task 6 holds its tests; they're written first there, so do Task 6 Step 1 before this step if you're following strict TDD).
- [ ] **Step 2: Build.** `cmake --build build-mp` produces `build-mp/revc-relay`. `./build-mp/revc-relay --help` exits 0 and prints usage.
- [ ] **Step 3: Commit.** `git commit -m "mp: invite-code relay server"`

---

### Task 6: Net client + relay integration tests

**Files:**
- Create: `mp/common/NetClient.h/.cpp`
- Test: `mp/tests/test_relay_integration.cpp`

**Interfaces:**
- Consumes: `CRelayServer` (Task 5), all of Task 2–4.
- Produces `class INetClientListener` (all virtual, default empty): `OnSessionReady(const char *code, uint8_t myId, bool isHost)`, `OnJoinFailed(eJoinResult)`, `OnPlayerJoined(uint8_t id, const char *name)`, `OnPlayerLeft(uint8_t id)`, `OnSessionEnded(eEndReason)`, `OnConnectionLost()`, `OnChat(uint8_t from, const char *text)`, `OnGameMessage(uint8_t from, uint8_t type, CByteReader &body)` (type ≥ `MSG_GAME_FIRST`).
- Produces `class CNetClient`:
  - `bool Host(const char *relayAddr, uint16_t port, const char *name)`, `bool Join(const char *relayAddr, uint16_t port, const char *code, const char *name)`. These start an async connect and return false only on an immediate error.
  - `void Service(uint32_t nowMs, INetClientListener *l)`: pumps ENet, dispatches callbacks, sends `MSG_PING` to the host every 1000 ms (non-host), and the host answers `MSG_PONG` automatically using `nowMs`.
  - `void Leave()`.
  - `bool SendGame(uint8_t route, uint8_t target, bool reliable, const CByteWriter &msg)`: reliable → `MP_CHANNEL_RELIABLE`; unreliable → `MP_CHANNEL_SNAPSHOT` with `ENET_PACKET_FLAG_UNSEQUENCED` off (sequenced).
  - `bool SendChat(const char *text)` (route `ALL`, so the sender also receives it, and that echo is the single source of on-screen chat).
  - State queries: `bool IsInSession()`, `bool IsHost()`, `uint8_t MyId()`, `const char *Code()`, `const CSessionClock &Clock()`, `uint32_t SessionTime(uint32_t nowMs)`.
  - Connect timeout is 5000 ms, after which `OnJoinFailed(JOIN_NOT_FOUND)` if never connected, else `OnConnectionLost`.
  - `enet_initialize` is ref-counted in a static helper so the relay, many clients and tests can share a process.

- [ ] **Step 1: Write failing tests.** A test harness spins a `CRelayServer` on a free port in 27700–27799 and N `CNetClient`s with a recording listener, then `Pump(ms)` services everything in a loop with 1 ms sleeps until a predicate holds or a 3 s timeout hits.
  - `host gets code`: `Host("127.0.0.1", port, "Tommy")` → `OnSessionReady` with a code matching `VICE-????`, `myId==0`, `isHost`.
  - `join and roster`: 2 clients join with that code. Each sees `OnPlayerJoined` for the others, the host sees ids 1 and 2, and names come through.
  - `duplicate names`: a second "Lance" joins as `"Lance (2)"`.
  - `full session`: 7 joiners succeed; the 8th joiner (9th player) gets `JOIN_FULL`.
  - `bad code`: `Join(..., "VICE-ZZZZ", ...)` → `JOIN_NOT_FOUND`. `"VICE-AB0K"` → `JOIN_NOT_FOUND`.
  - `version mismatch`: a raw ENet peer sends `MSG_JOIN_SESSION` with version 999 → `JOIN_VERSION_MISMATCH`.
  - `chat to all`: client 1 `SendChat("hi")`. All 3, including the sender, get `OnChat(1, "hi")`.
  - `routing`: client 2 sends a game msg type 64 to `MP_ROUTE_HOST`; only the host gets it, with `from==2`. `MP_ROUTE_PLAYER` target 1 reaches only 1. `ALL_BUT_SELF` reaches everyone except the sender.
  - `host leaves ends session`: the host calls `Leave()`. Others get `OnSessionEnded(END_HOST_LEFT)`, the relay's `NumSessions()` returns 0, and joining the old code gives `JOIN_NOT_FOUND`.
  - `member leaves`: client 1 leaves, and the host + client 2 get `OnPlayerLeft(1)`.
  - `garbage is dropped`: a raw ENet peer in a session sends 50 random-byte packets and a 1500-byte packet. The relay stays up, and a following legit chat still arrives.
  - `clock syncs`: after 4 s of pumping, a joiner's `Clock().IsSynced()` and `|SessionTime(now) − host now| ≤ 5` on loopback.
- [ ] **Step 2: Run, expect fail** (no `NetClient`).
- [ ] **Step 3: Implement `CNetClient`** (and finish `CRelayServer` until the tests pass).
- [ ] **Step 4: Run, expect pass.** Run `ctest --test-dir build-mp --output-on-failure` three times in a row; every run passes, with no flakes.
- [ ] **Step 5: Commit.** `git commit -m "mp: net client with relay integration tests"`

---

### Task 7: Headless fake client and relay setup guide

**Files:**
- Create: `mp/tools/fakeclient.cpp`, `docs/multiplayer/relay-setup.md`
- Modify: `mp/CMakeLists.txt` (`revc-fakeclient`), `docs/multiplayer/README.md` (Status: Plan 1 done; add the build/run commands)

**Interfaces:**
- Consumes: `CNetClient`, `INetClientListener`.
- Produces the CLI `revc-fakeclient --relay HOST[:PORT] (--host | --join CODE) [--name NAME] [--say TEXT] [--seconds N]`. It prints every callback as one line (`[joined] 1 Lance`, `[chat] 0: hi`, `[game] from=0 type=65 bytes=48`), sends `--say` once on joining, and exits after `--seconds` (default: run until Ctrl+C). Plan 2 adds a `--bot` mode that sends synthetic player snapshots, so keep the main loop a function that takes a per-tick callback.
- `relay-setup.md` covers:
  1. **Cloud VPS:** a Linux VPS such as the Oracle Cloud Always Free tier or any ~$5 VPS in a region near the players; building `revc-relay`; a systemd unit (`Restart=always`); opening **UDP 7777** in both the OS firewall and the cloud security list; checking with `revc-fakeclient --relay <ip> --host`.
  2. **Home PC:** running `revc-relay` on Linux or Windows, forwarding **UDP 7777** on the router to that PC, allowing it through the firewall, and finding the public IP.
  3. Pointing the game at a relay (Plan 2's *Relay server* menu setting / default address).
  4. A one-paragraph note that running a public relay for a reverse-engineered game carries legal risk the operator accepts.

- [ ] **Step 1: Implement `fakeclient.cpp`.**
- [ ] **Step 2: Manual end-to-end check.** Run each command in its own terminal:
  `./build-mp/revc-relay --port 7777`
  `./build-mp/revc-fakeclient --relay 127.0.0.1 --host --name Tommy` → prints `[ready] code=VICE-XXXX id=0 host`
  `./build-mp/revc-fakeclient --relay 127.0.0.1 --join VICE-XXXX --name Lance --say hello` → Tommy's terminal prints `[joined] 1 Lance` and `[chat] 1: hello`.
  Ctrl+C on Tommy → Lance prints `[ended] host left` and exits non-zero.
- [ ] **Step 3: Write `relay-setup.md`** and update `docs/multiplayer/README.md`.
- [ ] **Step 4: Commit.** `git commit -m "mp: fake client tool and relay setup guide"`

---

### Task 8: CI on macOS, Linux and Windows

**Files:**
- Create: `.github/workflows/multiplayer.yml`

**Interfaces:**
- Produces a workflow named `multiplayer` that runs on push and pull_request when `mp/**`, `vendor/enet/**`, `vendor/doctest/**` or the workflow itself changes. Matrix `os: [ubuntu-latest, macos-latest, windows-latest]`, `fail-fast: false`. Steps: checkout, `cmake -S mp -B build-mp -DCMAKE_BUILD_TYPE=Release`, `cmake --build build-mp --config Release`, `ctest --test-dir build-mp -C Release --output-on-failure`. Uploads `revc-relay` and `revc-fakeclient` binaries as artifacts named `mp-tools-${{ matrix.os }}`.

- [ ] **Step 1: Write the workflow.**
- [ ] **Step 2: Fix any platform build breaks locally where possible.** MSVC: `ws2_32`/`winmm` linkage, no VLAs, `_CRT_SECURE_NO_WARNINGS`. macOS: Clang warnings. Linux check: `cmake -S mp -B build-mp-clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-mp-clang && ctest --test-dir build-mp-clang`, all pass.
- [ ] **Step 3: Push and check CI.** Push the branch; all three matrix jobs go green. If one fails, fix and push again.
- [ ] **Step 4: Commit.** `git commit -m "ci: build and test multiplayer layer on macOS, Linux and Windows"`
