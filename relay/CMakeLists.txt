# relay/CMakeLists.txt — the simulcast relay.
#
# A separate sub-project that DEPENDS ON the core, never the other way round.
# Nothing here is linked into the plugins, and the option defaults to OFF so a
# plugin build is not asked to find sqlite3 or a web server it has no use for.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

if(NOT TARGET multisite_s3)
    message(FATAL_ERROR
        "The relay needs multisite_s3, which needs libcurl. Install libcurl "
        "development headers and configure again.")
endif()

# The pure decision-making, with no I/O in it at all: what may be sent onward,
# and one destination's lifecycle. Split out so the tests link against this
# alone and cannot accidentally reach a bucket or spawn a process.
# The send path (stream_plan, destination) moved to the core so the appliance
# encoder can reuse it; relay_logic is now just the relay's own decision-making.
add_library(relay_logic STATIC
    src/relay_state.cpp)
target_include_directories(relay_logic PUBLIC src)
target_link_libraries(relay_logic PUBLIC multisite_core)

add_executable(multisite-relay
    src/main.cpp
    src/api.cpp
    src/service.cpp
    src/config_store.cpp
    src/auth.cpp
    src/relay_session.cpp
    src/room_feeder.cpp
    src/log.cpp
    # The shared HTTP server, from the core: dependency-free, with a
    # static-file root, which is exactly what this needs.
    ../src/core/http_server.cpp
    ../src/appliance/log.cpp)
target_include_directories(multisite-relay PRIVATE
    src ../src ../src/core ../src/appliance ../src/vendor)
# multisite_core arrives through relay_logic's PUBLIC link; naming it again
# here only makes the linker warn about a duplicate.
find_package(OpenSSL REQUIRED)
target_link_libraries(multisite-relay PRIVATE
    relay_logic multisite_s3 PkgConfig::SQLITE3 OpenSSL::Crypto)

# The version comes from the top-level project(), never from a string of its
# own. A second copy is what left the Pi player reporting 0.1.0 for four
# releases: nothing sets a value nobody remembers exists.
target_compile_definitions(multisite-relay PRIVATE
    MULTISITE_RELAY_VERSION="${PROJECT_VERSION}")

install(TARGETS multisite-relay RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/web/" DESTINATION share/multisite-relay/web)

# ── Tests ────────────────────────────────────────────────────────────────────
# Everything that can be proved without a destination, a bucket or a wait. The
# soak and the kill test are deliberately NOT here: they are the ones that
# actually prove the relay, and they need a real clock.
add_executable(test_stream_plan tests/test_stream_plan.cpp)
target_link_libraries(test_stream_plan PRIVATE relay_logic)
add_test(NAME stream_plan COMMAND test_stream_plan)

add_executable(test_relay_state tests/test_relay_state.cpp)
target_link_libraries(test_relay_state PRIVATE relay_logic)
add_test(NAME relay_state COMMAND test_relay_state)

add_executable(test_config_store tests/test_config_store.cpp src/config_store.cpp)
target_include_directories(test_config_store PRIVATE ../src/core)
target_link_libraries(test_config_store PRIVATE relay_logic multisite_s3 PkgConfig::SQLITE3)
add_test(NAME config_store COMMAND test_config_store)

message(STATUS "Building the simulcast relay")
