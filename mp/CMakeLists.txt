# reVC multiplayer: engine-free networking layer, relay server, tools and tests.
# Works both as add_subdirectory(mp) from the top-level project and standalone:
#   cmake -S mp -B build-mp -G Ninja && cmake --build build-mp && ctest --test-dir build-mp
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.14)
	project(revc_mp C CXX)
	enable_testing()
	set(MP_STANDALONE ON)
endif()

set(MP_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor")

# --- ENet (vendored, unmodified) ---------------------------------------------
set(ENET_DIR "${MP_VENDOR_DIR}/enet")
add_library(enet STATIC
	${ENET_DIR}/callbacks.c
	${ENET_DIR}/compress.c
	${ENET_DIR}/host.c
	${ENET_DIR}/list.c
	${ENET_DIR}/packet.c
	${ENET_DIR}/peer.c
	${ENET_DIR}/protocol.c
	${ENET_DIR}/unix.c
	${ENET_DIR}/win32.c
)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
set_target_properties(enet PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
	target_link_libraries(enet PUBLIC ws2_32 winmm)
	if(MSVC)
		target_compile_definitions(enet PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
	endif()
else()
	# Same configure checks as ENet's own CMakeLists.txt.
	include(CheckFunctionExists)
	include(CheckStructHasMember)
	include(CheckTypeSize)
	check_function_exists("fcntl" HAS_FCNTL)
	check_function_exists("poll" HAS_POLL)
	check_function_exists("getaddrinfo" HAS_GETADDRINFO)
	check_function_exists("getnameinfo" HAS_GETNAMEINFO)
	check_function_exists("gethostbyname_r" HAS_GETHOSTBYNAME_R)
	check_function_exists("gethostbyaddr_r" HAS_GETHOSTBYADDR_R)
	check_function_exists("inet_pton" HAS_INET_PTON)
	check_function_exists("inet_ntop" HAS_INET_NTOP)
	check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" HAS_MSGHDR_FLAGS)
	set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
	check_type_size("socklen_t" HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
	unset(CMAKE_EXTRA_INCLUDE_FILES)
	foreach(def HAS_FCNTL HAS_POLL HAS_GETADDRINFO HAS_GETNAMEINFO HAS_GETHOSTBYNAME_R
	            HAS_GETHOSTBYADDR_R HAS_INET_PTON HAS_INET_NTOP HAS_MSGHDR_FLAGS HAS_SOCKLEN_T)
		if(${def})
			target_compile_definitions(enet PRIVATE ${def}=1)
		endif()
	endforeach()
endif()

# --- Shared settings for our own targets -------------------------------------
function(mp_target_settings target)
	set_target_properties(${target} PROPERTIES
		CXX_STANDARD 11
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W3)
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
endfunction()

# --- mpcommon: engine-free protocol/session code, no reVC headers ------------
add_library(mpcommon STATIC
	common/MpTypes.cpp
	common/ByteStream.cpp
	common/Protocol.cpp
	common/InviteCode.cpp
	common/SessionClock.cpp
	common/PoseBuffer.cpp
	common/NetInit.cpp
	common/NetClient.cpp
	common/GameProtocol.cpp
	common/RaceRoute.cpp
	common/RaceLogic.cpp
	common/RaceHost.cpp
	common/MatchLogic.cpp
	common/MatchHost.cpp
	common/RouteEditor.cpp
	common/HillLogic.cpp
	common/StuntLogic.cpp
	common/SeatTable.cpp
)
target_include_directories(mpcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(mpcommon PUBLIC enet)
if(WIN32)
	target_link_libraries(mpcommon PUBLIC ws2_32 winmm)
endif()
mp_target_settings(mpcommon)

# --- Relay ------------------------------------------------------------------
add_library(relaycore STATIC
	relay/RelayServer.cpp
)
target_include_directories(relaycore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/relay)
target_link_libraries(relaycore PUBLIC mpcommon)
mp_target_settings(relaycore)

add_executable(revc-relay
	relay/main.cpp
)
target_link_libraries(revc-relay PRIVATE relaycore)
mp_target_settings(revc-relay)

# --- Dedicated server ---------------------------------------------------------
find_package(Threads REQUIRED)
add_library(servercore STATIC
	server/DedicatedHost.cpp
)
target_include_directories(servercore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(servercore PUBLIC mpcommon)
mp_target_settings(servercore)

add_executable(revc-server
	server/main.cpp
)
target_link_libraries(revc-server PRIVATE servercore relaycore Threads::Threads)
mp_target_settings(revc-server)

# --- Tools ------------------------------------------------------------------
add_executable(revc-fakeclient
	tools/fakeclient.cpp
)
target_link_libraries(revc-fakeclient PRIVATE mpcommon Threads::Threads)
mp_target_settings(revc-fakeclient)

# --- Tests --------------------------------------------------------------------
add_executable(mp_tests
	tests/test_main.cpp
	tests/test_bytestream.cpp
	tests/test_protocol.cpp
	tests/test_invitecode.cpp
	tests/test_sessionclock.cpp
	tests/test_posebuffer.cpp
	tests/test_relay_integration.cpp
	tests/test_gameprotocol.cpp
	tests/test_raceroute.cpp
	tests/test_racelogic.cpp
	tests/test_racehost.cpp
	tests/test_matchlogic.cpp
	tests/test_matchhost.cpp
	tests/test_routeeditor.cpp
	tests/test_modes.cpp
	tests/test_server.cpp
)
target_include_directories(mp_tests PRIVATE ${MP_VENDOR_DIR}/doctest)
target_link_libraries(mp_tests PRIVATE servercore relaycore mpcommon Threads::Threads)
mp_target_settings(mp_tests)
add_test(NAME mp_tests COMMAND mp_tests)
